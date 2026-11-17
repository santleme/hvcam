/* test_loopback — serves fake frames over TCP on 127.0.0.1 with the
 * real wire protocol, and checks the client end-to-end:
 * REQ -> header -> payload, seq freshness, partial buffers, close.
 *
 * Exercises the same code path a WSL consumer uses; only the socket
 * family differs (AF_INET instead of AF_VSOCK, same server loop).
 */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "hvcam_client.h"

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
static uint64_t g_seq;
static int g_serve_n = 64; /* extra REQs: same-seq retry loop spends a few */

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
        hdr.seq_num = g_seq;
        hdr.timestamp_us = 1000000u + g_seq * 33333u;
        hdr.width = FAKE_W;
        hdr.height = FAKE_H;
        hdr.format = HVCAM_FMT_YUY2;
        hdr.payload_bytes = FAKE_BYTES;
        uint8_t payload[FAKE_BYTES];
        fill_fake(payload, g_seq);
        send(cfd, &hdr, sizeof(hdr), 0);
        send(cfd, payload, sizeof(payload), 0);
    }
    close(cfd);
    return NULL;
}

int main(void)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(lfd >= 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(TEST_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(lfd, (struct sockaddr *)&a, sizeof(a)) == 0);
    assert(listen(lfd, 1) == 0);

    pthread_t tid;
    assert(pthread_create(&tid, NULL, server, &lfd) == 0);

    int fd = hvcam_connect_tcp("127.0.0.1", TEST_PORT);
    assert(fd >= 0);

    uint8_t buf[64];
    HvFrameHeader hdr;
    uint64_t last = UINT64_MAX;

    /* frame 0 */
    g_seq = 0;
    int n = hvcam_grab_next(fd, &hdr, buf, sizeof(buf), &last, 500);
    assert(n == FAKE_BYTES);
    assert(hdr.seq_num == 0 && hdr.width == FAKE_W);
    assert(buf[0] == 0 && buf[FAKE_BYTES - 1] == FAKE_BYTES - 1);

    /* frame 1 — bigger seq */
    g_seq = 1;
    n = hvcam_grab_next(fd, &hdr, buf, sizeof(buf), &last, 500);
    assert(n == FAKE_BYTES && hdr.seq_num == 1 && buf[0] == 1);

    /* frame 2 — same seq must not be delivered as "new": advance so
     * the NEXT request returns the fresher seq instead of hanging. */
    g_seq = 2;
    n = hvcam_grab_next(fd, &hdr, buf, sizeof(buf), &last, 500);
    assert(n == FAKE_BYTES && hdr.seq_num == 2);

    /* undersized buffer: drains full payload, reports cap */
    g_seq = 3;
    n = hvcam_grab_next(fd, &hdr, buf, 8, &last, 500);
    assert(n == 8 && hdr.payload_bytes == FAKE_BYTES);

    /* header-only grab */
    g_seq = 4;
    n = hvcam_grab_next(fd, &hdr, NULL, 0, &last, 500);
    assert(n == 0 && hdr.seq_num == 4);

    /* same-seq request hits the -2 timeout path */
    g_seq = 5;
    hvcam_grab_next(fd, &hdr, buf, sizeof(buf), &last, 200);
    n = hvcam_grab_next(fd, &hdr, buf, sizeof(buf), &last, 120);
    assert(n == -2);

    hvcam_close(fd);
    close(lfd);
    pthread_join(tid, NULL);
    printf("test_loopback: OK\n");
    return 0;
}
