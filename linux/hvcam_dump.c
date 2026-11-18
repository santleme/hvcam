/* hvcam-dump — smallest useful client: connects, pulls frames, prints
 * seq/timestamp/fps. Use it to smoke-test a host before wiring real
 * consumers.
 *
 *   hvcam-dump                 # vsock, port 8765, ~5 s
 *   hvcam-dump --tcp 127.0.0.1:8765
 *   hvcam-dump -n 100          # stop after 100 fresh frames
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "hvcam_client.h"

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static long parse_long(const char *s, const char *name, long lo, long hi)
{
    char *end = NULL;
    long v;
    if (!s || !*s) {
        fprintf(stderr, "hvcam-dump: empty value for %s\n", name);
        exit(2);
    }
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno == ERANGE || end == s || *end != '\0' || v < lo || v > hi) {
        fprintf(stderr, "hvcam-dump: invalid %s '%s' (need %ld..%ld)\n",
                name, s, lo, hi);
        exit(2);
    }
    return v;
}

static double parse_seconds(const char *s)
{
    char *end = NULL;
    double v;
    errno = 0;
    v = strtod(s, &end);
    /* !(v > 0) also rejects NaN and negatives — a negative double
     * cast to uint64_t below would be undefined behavior. */
    if (errno == ERANGE || end == s || *end != '\0' || !(v > 0.0) ||
        v > 86400.0) {
        fprintf(stderr, "hvcam-dump: invalid seconds '%s' (need > 0)\n", s);
        exit(2);
    }
    return v;
}

int main(int argc, char **argv)
{
    const char *tcp = NULL;
    uint32_t port = 8765;
    int max_frames = 0;
    int use_listen = 0;
    double max_seconds = 5.0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tcp") && i + 1 < argc) {
            tcp = argv[++i];
        } else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            /* vsock service ports are 32-bit (GUID field), unlike
             * TCP's 16-bit — the TCP bound is enforced below only
             * when --tcp mode is actually selected. */
            port = (uint32_t)parse_long(argv[++i], "-p", 1, 4294967295L);
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            max_frames = (int)parse_long(argv[++i], "-n", 0, INT_MAX);
        } else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            max_seconds = parse_seconds(argv[++i]);
        } else if (!strcmp(argv[i], "--listen")) {
            /* Reverse mode: wait for `hvcam-host --reverse` to
             * connect in. No host-side registration needed. */
            use_listen = 1;
        } else {
            fprintf(stderr, "usage: hvcam-dump [--tcp host[:port]] [--listen] [-p port] [-n frames] [-t seconds]\n");
            return 2;
        }
    }

    int fd;
    if (tcp) {
        /* host[:port] — the colon splits a TCP-style port suffix.
         * IPv6 literals without brackets are not supported here. */
        const char *colon = strrchr(tcp, ':');
        char host[128] = {0};
        if (colon) {
            long hlen = colon - tcp;
            if (hlen <= 0 || hlen >= (long)sizeof(host)) {
                fprintf(stderr, "hvcam-dump: invalid --tcp host\n");
                return 2;
            }
            snprintf(host, sizeof(host), "%.*s", (int)hlen, tcp);
            port = (uint32_t)parse_long(colon + 1, "--tcp port", 1, 65535);
            tcp = host;
        } else if (!*tcp) {
            fprintf(stderr, "hvcam-dump: empty --tcp host\n");
            return 2;
        }
        if (port > 65535) {
            fprintf(stderr, "hvcam-dump: TCP port must be 1..65535\n");
            return 2;
        }
        fd = hvcam_connect_tcp(tcp, (uint16_t)port);
    } else if (use_listen) {
        fd = hvcam_listen(port);
    } else {
        fd = hvcam_connect(port);
    }
    if (fd < 0) {
        fprintf(stderr, "hvcam-dump: connect failed (%d)\n", fd);
        return 1;
    }

    uint8_t *buf = malloc(HVCAM_MAX_PAYLOAD);
    if (!buf) {
        fprintf(stderr, "hvcam-dump: out of memory\n");
        hvcam_close(fd);
        return 1;
    }

    HvFrameHeader hdr;
    uint64_t last_seq = UINT64_MAX;
    uint64_t t0 = now_us();
    uint64_t last_arrival = 0;
    int fresh = 0;

    while ((now_us() - t0) < (uint64_t)(max_seconds * 1e6)) {
        int n = hvcam_grab_next(fd, &hdr, buf, HVCAM_MAX_PAYLOAD,
                                &last_seq, 1000);
        if (n == -2)
            continue;
        if (n < 0) {
            fprintf(stderr, "hvcam-dump: stream dropped\n");
            free(buf);
            hvcam_close(fd);
            return 1;
        }
        uint64_t arrival = now_us();
        /* host timestamp_us and the local clock share no origin;
         * the honest metric is the local arrival-to-arrival gap. */
        long gap = last_arrival ? (long)(arrival - last_arrival) : 0;
        last_arrival = arrival;
        fresh++;
        printf("seq=%llu %ux%u %s payload=%dB gap=%ldus\n",
               (unsigned long long)hdr.seq_num, hdr.width, hdr.height,
               hvcam_format_name(hdr.format), n, gap);
        if (max_frames && fresh >= max_frames)
            break;
    }

    double secs = (now_us() - t0) / 1e6;
    fprintf(stderr, "hvcam-dump: %d fresh frames in %.1fs (%.1f fps)\n",
            fresh, secs, secs > 0 ? fresh / secs : 0.0);
    free(buf);
    hvcam_close(fd);
    return fresh ? 0 : 1;
}
