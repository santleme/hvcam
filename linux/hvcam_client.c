/* libhvcam — Linux client for the hvcam Windows host.
 *
 * Primary transport is AF_VSOCK to the Hyper-V host (VMADDR_CID_HOST);
 * a TCP fallback exists for development and non-WSL environments.
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime + poll sob -std=c99 */

#include "hvcam_client.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/vm_sockets.h>
#include <netdb.h>
#include <stdio.h>
#include <sys/socket.h>
#include <time.h>

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000ll + ts.tv_nsec / 1000;
}

/* Waits for the fd to be readable or the deadline to pass.
 * Returns 0 ready, -2 timeout, -1 error/peer-close. */
static int wait_readable(int fd, int64_t deadline_us)
{
    struct pollfd pfd = {fd, POLLIN, 0};
    for (;;) {
        /* Recomputa o restante a cada tentativa — EINTR nao
         * reinicia o intervalo cheio, senao sinais periodicos
         * venceriam o deadline indefinidamente. */
        int64_t left = deadline_us - now_us();
        if (left <= 0)
            return -2;
        int ms = (int)(left / 1000) + (left % 1000 ? 1 : 0);
        int r = poll(&pfd, 1, ms);
        if (r < 0 && errno == EINTR)
            continue;
        if (r == 0)
            return -2;
        if (r < 0)
            return -1;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return -1;
        return 0;
    }
}

/* deadline_us == INT64_MAX -> bloqueante (modo sem timeout).
 * Retorna -2 so quando NENHUM byte foi consumido nesta chamada:
 * estourar o prazo no meio de uma resposta deixa o stream
 * dessincronizado (o resto do frame seria lido como header na
 * proxima grab) — ai e -1, falha terminal que exige reconectar. */
static int read_full(int fd, void *buf, size_t n, int64_t deadline_us)
{
    uint8_t *p = (uint8_t *)buf;
    size_t total = n;
    while (n > 0) {
        if (deadline_us != INT64_MAX) {
            int w = wait_readable(fd, deadline_us);
            if (w == -2)
                return n < total ? -1 : -2;
            if (w)
                return w;
        }
        ssize_t r = recv(fd, p, n, 0);
        if (r == 0)
            return -1; /* peer closed */
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue; /* deadline ainda vale no proximo poll */
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int hvcam_connect(uint32_t port)
{
    int fd = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (fd < 0)
        return -errno;

    struct sockaddr_vm addr;
    memset(&addr, 0, sizeof(addr));
    addr.svm_family = AF_VSOCK;
    addr.svm_cid = VMADDR_CID_HOST; /* the Windows host, always cid 2 */
    addr.svm_port = port;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    return fd;
}

int hvcam_connect_tcp(const char *host, uint16_t port)
{
    char service[8];
    struct addrinfo hints, *res = NULL, *it;
    int fd = -1;

    snprintf(service, sizeof(service), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host ? host : "127.0.0.1", service, &hints, &res) != 0)
        return -1;

    for (it = res; it; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Reads one frame after sending REQ. Shared by grab/grab_next.
 * deadline_us cobre TUDO: o REQ, o header e o payload inteiro —
 * um peer que trava no meio da resposta nao passa do prazo. */
static int pull_frame(int fd, HvFrameHeader *hdr, void *payload,
                      uint32_t cap, int64_t deadline_us)
{
    uint8_t req = HVCAM_REQ;
    if (write_full(fd, &req, 1) < 0)
        return -1;
    int rc = read_full(fd, hdr, sizeof(*hdr), deadline_us);
    if (rc)
        return rc;
    if (hdr->magic != HVCAM_MAGIC || hdr->version != HVCAM_VERSION ||
        hdr->payload_bytes > HVCAM_MAX_PAYLOAD)
        return -1;

    /* Always drain the declared payload, even past `cap`. */
    uint32_t remaining = hdr->payload_bytes;
    uint8_t *out = (uint8_t *)payload;
    uint32_t stored = 0;
    uint8_t scratch[4096];
    while (remaining > 0) {
        uint32_t chunk = remaining > sizeof(scratch) ? (uint32_t)sizeof(scratch) : remaining;
        void *dst = scratch;
        if (out && stored < cap) {
            uint32_t room = cap - stored;
            chunk = remaining > room ? room : remaining;
            dst = out + stored;
        }
        if (chunk == 0)
            chunk = remaining > sizeof(scratch) ? (uint32_t)sizeof(scratch) : remaining;
        rc = read_full(fd, dst, chunk, deadline_us);
        if (rc == -2)
            return -1; /* header ja saiu do socket: resposta parcial = desync */
        if (rc)
            return rc;
        if (dst != scratch)
            stored += chunk;
        remaining -= chunk;
    }
    return (int)stored;
}

int hvcam_grab(int fd, HvFrameHeader *hdr, void *payload, uint32_t cap)
{
    return pull_frame(fd, hdr, payload, cap, INT64_MAX);
}

int hvcam_grab_next(int fd, HvFrameHeader *hdr, void *payload,
                    uint32_t cap, uint64_t *last_seq, int timeout_ms)
{
    /* Um unico prazo cobre todo o wait: REQ, headers e payloads.
     * Resposta placeholder (flags bit0=0) ou seq repetida nao conta
     * como frame novo — espera ate o prazo.
     *
     * -2 so sai com a conexao consistente: deadline verificado ANTES
     * de enviar cada REQ. Estourar o prazo com um REQ ja no fio
     * deixa uma resposta a dever — a proxima grab leria a resposta
     * atrasada como se fosse a nova — entao vira -1 (reconectar). */
    int64_t deadline_us = now_us() + (int64_t)timeout_ms * 1000;
    for (;;) {
        if (now_us() >= deadline_us)
            return -2;
        int n = pull_frame(fd, hdr, payload, cap, deadline_us);
        if (n == -2)
            return -1; /* REQ sem resposta: uma resposta ficou devendo */
        if (n < 0)
            return n;
        if ((hdr->flags & 1) && hdr->seq_num != *last_seq) {
            *last_seq = hdr->seq_num;
            return n;
        }
        /* Respiro entre REQs: dorme ~15ms clampado ao deadline e
         * detecta queda do peer de graca (POLLHUP sai cedo). */
        int64_t nap_end = now_us() + 15000;
        if (nap_end > deadline_us)
            nap_end = deadline_us;
        int w = wait_readable(fd, nap_end);
        if (w == -1)
            return -1;
    }
}

void hvcam_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

const char *hvcam_format_name(uint32_t format)
{
    switch (format) {
    case HVCAM_FMT_BGR24: return "bgr24";
    case HVCAM_FMT_YUY2: return "yuy2";
    case HVCAM_FMT_NV12: return "nv12";
    case HVCAM_FMT_MJPEG: return "mjpeg";
    case HVCAM_FMT_BGRX32: return "bgrx32";
    default: return "unknown";
    }
}
