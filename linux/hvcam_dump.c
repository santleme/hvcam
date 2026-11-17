/* hvcam-dump — smallest useful client: connects, pulls frames, prints
 * seq/timestamp/fps. Use it to smoke-test a host before wiring real
 * consumers.
 *
 *   hvcam-dump                 # vsock, port 8765, ~5 s
 *   hvcam-dump --tcp 127.0.0.1:8765
 *   hvcam-dump -n 100          # stop after 100 fresh frames
 */
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

int main(int argc, char **argv)
{
    const char *tcp = NULL;
    uint32_t port = 8765;
    int max_frames = 0;
    double max_seconds = 5.0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tcp") && i + 1 < argc) {
            tcp = argv[++i];
        } else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            port = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            max_frames = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            max_seconds = atof(argv[++i]);
        } else {
            fprintf(stderr, "uso: hvcam-dump [--tcp host[:porta]] [-p porta] [-n frames] [-t segundos]\n");
            return 2;
        }
    }

    int fd;
    if (tcp) {
        const char *colon = strrchr(tcp, ':');
        char host[128] = {0};
        if (colon) {
            snprintf(host, sizeof(host), "%.*s", (int)(colon - tcp), tcp);
            port = (uint32_t)strtoul(colon + 1, NULL, 10);
            tcp = host;
        }
        fd = hvcam_connect_tcp(tcp, (uint16_t)port);
    } else {
        fd = hvcam_connect(port);
    }
    if (fd < 0) {
        fprintf(stderr, "hvcam-dump: conexao falhou (%d)\n", fd);
        return 1;
    }

    uint8_t *buf = malloc(HVCAM_MAX_PAYLOAD);
    if (!buf) {
        fprintf(stderr, "hvcam-dump: sem memoria\n");
        return 1;
    }

    HvFrameHeader hdr;
    uint64_t last_seq = UINT64_MAX;
    uint64_t t0 = now_us();
    uint64_t first_ts = 0;
    int fresh = 0;

    while ((now_us() - t0) < (uint64_t)(max_seconds * 1e6)) {
        int n = hvcam_grab_next(fd, &hdr, buf, HVCAM_MAX_PAYLOAD,
                                &last_seq, 1000);
        if (n == -2)
            continue;
        if (n < 0) {
            fprintf(stderr, "hvcam-dump: stream caiu\n");
            free(buf);
            hvcam_close(fd);
            return 1;
        }
        if (first_ts == 0)
            first_ts = hdr.timestamp_us;
        fresh++;
        printf("seq=%llu %ux%u %s payload=%dB age=%ldus\n",
               (unsigned long long)hdr.seq_num, hdr.width, hdr.height,
               hvcam_format_name(hdr.format), n,
               (long)(now_us() - hdr.timestamp_us));
        if (max_frames && fresh >= max_frames)
            break;
    }

    double secs = (now_us() - t0) / 1e6;
    fprintf(stderr, "hvcam-dump: %d frames novos em %.1fs (%.1f fps)\n",
            fresh, secs, fresh / secs);
    free(buf);
    hvcam_close(fd);
    return fresh ? 0 : 1;
}
