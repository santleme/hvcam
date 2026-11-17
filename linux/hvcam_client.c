/* libhvcam — Linux client for the hvcam Windows host.
 *
 * Primary transport is AF_VSOCK to the Hyper-V host (VMADDR_CID_HOST);
 * a TCP fallback exists for development and non-WSL environments.
 */
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

static int read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r == 0)
            return -1; /* peer closed */
        if (r < 0) {
            if (errno == EINTR)
                continue;
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

/* Reads one frame after sending REQ. Shared by grab/grab_next. */
static int pull_frame(int fd, HvFrameHeader *hdr, void *payload, uint32_t cap)
{
    uint8_t req = HVCAM_REQ;
    if (write_full(fd, &req, 1) < 0)
        return -1;
    if (read_full(fd, hdr, sizeof(*hdr)) < 0)
        return -1;
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
        if (read_full(fd, dst, chunk) < 0)
            return -1;
        if (dst != scratch)
            stored += chunk;
        remaining -= chunk;
    }
    return (int)stored;
}

int hvcam_grab(int fd, HvFrameHeader *hdr, void *payload, uint32_t cap)
{
    return pull_frame(fd, hdr, payload, cap);
}

int hvcam_grab_next(int fd, HvFrameHeader *hdr, void *payload,
                    uint32_t cap, uint64_t *last_seq, int timeout_ms)
{
    /* Bounded wait: polls the socket; the host answers each REQ with
     * the newest frame, so a repeated seq means "nothing newer yet".
     * We REQ, and if seq repeats we poll-wait then REQ again — keeps
     * traffic at one in-flight request per poll turn. */
    int elapsed = 0;
    const int step = 40;
    for (;;) {
        int n = pull_frame(fd, hdr, payload, cap);
        if (n < 0)
            return n;
        if (!(hdr->flags & 1)) /* no real frame yet */
            return n;
        if (hdr->seq_num != *last_seq) {
            *last_seq = hdr->seq_num;
            return n;
        }
        if (elapsed >= timeout_ms)
            return -2;
        struct pollfd pfd = {fd, POLLIN, 0};
        /* Cheap retry cadence without busy-spinning. */
        int r = poll(&pfd, 0, step); /* fd has no buffered data; just sleep */
        (void)r;
        elapsed += step;
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
    default: return "unknown";
    }
}
