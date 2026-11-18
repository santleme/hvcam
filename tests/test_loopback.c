/* test_loopback — serves fake frames over TCP on 127.0.0.1 with the
 * real wire protocol, and checks the client end-to-end:
 * REQ -> header -> payload, seq freshness, partial buffers, close.
 *
 * Exercises the same code path a WSL consumer uses; only the socket
 * family differs (AF_INET instead of AF_VSOCK, same server loop).
 */
#define _POSIX_C_SOURCE 199309L /* nanosleep sob -std=c99 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "hvcam_client.h"
#include "test_assert.h"

#define TEST_PORT 18765
#define FAKE_W 4
#define FAKE_H 2
#define FAKE_BYTES (FAKE_W * FAKE_H * 2) /* yuy2 */

static void fill_fake(uint8_t *out, uint64_t seq)
{
    for (int i = 0; i < FAKE_BYTES; i++)
        out[i] = (uint8_t)(seq + i);
}

/* Sends one frame per REQ, seq taken from a shared counter that the
 * test advances between requests to simulate "new frame arrived". */
/* Shared between test thread (writer) and server thread (reader) —
 * accessed only via __atomic builtins to keep the assertion order
 * defined under -O2 and TSAN. */
static uint64_t g_seq;
#define seq_store(v) __atomic_store_n(&g_seq, (v), __ATOMIC_RELEASE)
#define seq_load() __atomic_load_n(&g_seq, __ATOMIC_ACQUIRE)
static int g_serve_n = 64; /* extra REQs: same-seq retry loop spends a few */

/* 0 = full reply; 1 = stalls mid-header;
 * 2 = stalls mid-payload; 3 = replies past the deadline.
 * Atomic like g_seq. */
static int g_stall;
#define stall_load() __atomic_load_n(&g_stall, __ATOMIC_ACQUIRE)

/* send that cannot raise SIGPIPE once the client has closed —
 * the stall server writes AFTER the client has given up. */
static void send_noflag(int fd, const void *p, size_t n)
{
    (void)send(fd, p, n, MSG_NOSIGNAL);
}

static void nap_ms(long ms)
{
    struct timespec d = {ms / 1000, (ms % 1000) * 1000000};
    nanosleep(&d, NULL);
}

static void *server(void *arg)
{
    int lfd = *(int *)arg;
    struct sockaddr_in ca;
    socklen_t clen = sizeof(ca);
    int cfd = accept(lfd, (struct sockaddr *)&ca, &clen);
    if (cfd < 0)
        return NULL;

    uint8_t req;
    for (int i = 0; i < g_serve_n; i++) {
        ssize_t r = recv(cfd, &req, 1, 0);
        if (r != 1)
            break;
        HvFrameHeader hdr = {0};
        hdr.magic = HVCAM_MAGIC;
        hdr.version = HVCAM_VERSION;
        hdr.flags = 1;
        uint64_t s = seq_load();
        hdr.seq_num = s;
        hdr.timestamp_us = 1000000u + s * 33333u;
        hdr.width = FAKE_W;
        hdr.height = FAKE_H;
        hdr.format = HVCAM_FMT_YUY2;
        hdr.payload_bytes = FAKE_BYTES;
        uint8_t payload[FAKE_BYTES];
        fill_fake(payload, s);
        int stall = stall_load();
        if (stall == 1) { /* stalls mid-header */
            send_noflag(cfd, &hdr, 10);
            nap_ms(200);
            send_noflag(cfd, (uint8_t *)&hdr + 10, sizeof(hdr) - 10);
            send_noflag(cfd, payload, sizeof(payload));
        } else if (stall == 2) { /* stalls mid-payload */
            send_noflag(cfd, &hdr, sizeof(hdr));
            send_noflag(cfd, payload, FAKE_BYTES / 2);
            nap_ms(200);
            send_noflag(cfd, payload + FAKE_BYTES / 2,
                        sizeof(payload) - FAKE_BYTES / 2);
        } else if (stall == 3) { /* replies only after the deadline */
            nap_ms(200);
            send_noflag(cfd, &hdr, sizeof(hdr));
            send_noflag(cfd, payload, sizeof(payload));
        } else {
            send_noflag(cfd, &hdr, sizeof(hdr));
            send_noflag(cfd, payload, sizeof(payload));
        }
    }
    close(cfd);
    return NULL;
}

int main(void)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT(lfd >= 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(TEST_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT(bind(lfd, (struct sockaddr *)&a, sizeof(a)) == 0);
    TEST_ASSERT(listen(lfd, 1) == 0);

    pthread_t tid;
    TEST_ASSERT(pthread_create(&tid, NULL, server, &lfd) == 0);

    int fd = hvcam_connect_tcp("127.0.0.1", TEST_PORT);
    TEST_ASSERT(fd >= 0);

    uint8_t buf[64];
    HvFrameHeader hdr;
    uint64_t last = UINT64_MAX;

    /* frame 0 */
    seq_store(0);
    int n = hvcam_grab_next(fd, &hdr, buf, sizeof(buf), &last, 500);
    TEST_ASSERT(n == FAKE_BYTES);
    TEST_ASSERT(hdr.seq_num == 0 && hdr.width == FAKE_W);
    TEST_ASSERT(buf[0] == 0 && buf[FAKE_BYTES - 1] == FAKE_BYTES - 1);

    /* frame 1 — bigger seq */
    seq_store(1);
    n = hvcam_grab_next(fd, &hdr, buf, sizeof(buf), &last, 500);
    TEST_ASSERT(n == FAKE_BYTES && hdr.seq_num == 1 && buf[0] == 1);

    /* frame 2 — same seq must not be delivered as "new": advance so
     * the NEXT request returns the fresher seq instead of hanging. */
    seq_store(2);
    n = hvcam_grab_next(fd, &hdr, buf, sizeof(buf), &last, 500);
    TEST_ASSERT(n == FAKE_BYTES && hdr.seq_num == 2);

    /* undersized buffer: drains full payload, reports cap */
    seq_store(3);
    n = hvcam_grab_next(fd, &hdr, buf, 8, &last, 500);
    TEST_ASSERT(n == 8 && hdr.payload_bytes == FAKE_BYTES);

    /* header-only grab */
    seq_store(4);
    n = hvcam_grab_next(fd, &hdr, NULL, 0, &last, 500);
    TEST_ASSERT(n == 0 && hdr.seq_num == 4);

    /* same-seq request hits the -2 timeout path */
    seq_store(5);
    hvcam_grab_next(fd, &hdr, buf, sizeof(buf), &last, 200);
    n = hvcam_grab_next(fd, &hdr, buf, sizeof(buf), &last, 120);
    TEST_ASSERT(n == -2);

    /* budget below the retry reserve still polls once: -2 would
     * mean the REQ never left. A fast loopback returns the frame;
     * a slow one may legitimately desync to -1 (REQ outstanding at
     * the deadline) — anything but a blind -2 proves the poll. */
    seq_store(6);
    n = hvcam_grab_next(fd, &hdr, buf, sizeof(buf), &last, 10);
    TEST_ASSERT(n != -2);
    if (n > 0)
        TEST_ASSERT(hdr.seq_num == 6);

    hvcam_close(fd);
    close(lfd);
    pthread_join(tid, NULL);

    /* Deadline with a REQ outstanding or mid-response: -1
     * (terminal), not -2 — a reply is still owed or the stream lost
     * its position; retrying requires a reconnect.
     * stall=1 header, stall=2 payload, stall=3 no reply. */
    for (int stall = 1; stall <= 3; stall++) {
        __atomic_store_n(&g_stall, stall, __ATOMIC_RELEASE);
        int l2 = socket(AF_INET, SOCK_STREAM, 0);
        TEST_ASSERT(l2 >= 0);
        setsockopt(l2, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in a2 = {0};
        a2.sin_family = AF_INET;
        a2.sin_port = htons(TEST_PORT + stall);
        a2.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        TEST_ASSERT(bind(l2, (struct sockaddr *)&a2, sizeof(a2)) == 0);
        TEST_ASSERT(listen(l2, 1) == 0);
        pthread_t t2;
        TEST_ASSERT(pthread_create(&t2, NULL, server, &l2) == 0);
        int fd2 = hvcam_connect_tcp("127.0.0.1", TEST_PORT + stall);
        TEST_ASSERT(fd2 >= 0);
        seq_store(10 + stall);
        uint64_t last2 = UINT64_MAX;
        n = hvcam_grab_next(fd2, &hdr, buf, sizeof(buf), &last2, 60);
        TEST_ASSERT(n == -1);
        hvcam_close(fd2);
        close(l2);
        pthread_join(t2, NULL);
        __atomic_store_n(&g_stall, 0, __ATOMIC_RELEASE);
    }

    printf("test_loopback: OK\n");
    return 0;
}
