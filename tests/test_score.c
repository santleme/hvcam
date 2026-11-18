/* test_score — pins the media-type negotiation policy in
 * include/media_score.h. The load-bearing case: on USB 2.0 the
 * isochronous ceiling (~24 MB/s) caps 1080p YUY2 (4.15 MB/frame)
 * near 5 fps, while the camera's on-chip MJPEG sustains 30 — so at
 * a requested resolution the type delivering more REAL frames must
 * win, and raw formats only break ties at equal frame rate.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "media_score.h"

#define S(fmt, w, h, fn, fd, o) hvcam_media_score((fmt), (w), (h), (fn), (fd), (o))

int main(void)
{
    const HvScoreOpts none = {0, 0, 0, 0};
    const HvScoreOpts fhd = {1920, 1080, 0, 0};
    const HvScoreOpts fhd_30 = {1920, 1080, 30, 0};
    const HvScoreOpts fhd_5 = {1920, 1080, 5, 0};
    const HvScoreOpts hd = {1280, 720, 0, 0};
    const HvScoreOpts fhd_yuy2 = {1920, 1080, 0, HVCAM_FMT_YUY2};

    /* The USB 2.0 trap: requested 1080p, camera offers YUY2@5 and
     * MJPEG@30 — MJPEG must win or the stream pins at ~5 fps. */
    assert(S(HVCAM_FMT_MJPEG, 1920, 1080, 30, 1, &fhd) >
           S(HVCAM_FMT_YUY2, 1920, 1080, 5, 1, &fhd));

    /* Same at 720p: MJPEG@30 beats YUY2@10 — the rule is delivered
     * frame rate, not a resolution threshold. */
    assert(S(HVCAM_FMT_MJPEG, 1280, 720, 30, 1, &hd) >
           S(HVCAM_FMT_YUY2, 1280, 720, 10, 1, &hd));

    /* Equal frame rate, large frame: the compressed type wins the
     * tie at >=720p — an advertised "30 fps" raw 1080p mode cannot
     * actually flow through USB 2.0 (measured: NV12 delivers 13.6
     * real fps where MJPEG reaches ~20+), and even on USB 3 the
     * compressed payload is ~17x smaller on the wire. */
    assert(S(HVCAM_FMT_MJPEG, 1920, 1080, 30, 1, &fhd) >
           S(HVCAM_FMT_NV12, 1920, 1080, 30, 1, &fhd));
    assert(S(HVCAM_FMT_MJPEG, 1280, 720, 30, 1, &hd) >
           S(HVCAM_FMT_YUY2, 1280, 720, 30, 1, &hd));

    /* Equal frame rate at small size: raw pixels win — 640x480
     * uncompressed fits USB 2.0 trivially and skips a decode. */
    assert(S(HVCAM_FMT_YUY2, 640, 480, 30, 1, &none) >
           S(HVCAM_FMT_MJPEG, 640, 480, 30, 1, &none));
    assert(S(HVCAM_FMT_NV12, 640, 480, 30, 1, &none) >
           S(HVCAM_FMT_MJPEG, 640, 480, 30, 1, &none));

    /* A raw type still wins when it is genuinely FASTER (below the
     * >=30fps plateau: 30 real fps outranks 15 compressed). */
    assert(S(HVCAM_FMT_YUY2, 1280, 720, 30, 1, &hd) >
           S(HVCAM_FMT_MJPEG, 1280, 720, 15, 1, &hd));

    /* Default (no request): prefer the largest >=30fps mode —
     * 1080p MJPEG@30 over a VGA YUY2@30. */
    assert(S(HVCAM_FMT_MJPEG, 1920, 1080, 30, 1, &none) >
           S(HVCAM_FMT_YUY2, 640, 480, 30, 1, &none));

    /* Explicit --fps is user intent: --fps 5 picks the 5 fps type. */
    assert(S(HVCAM_FMT_YUY2, 1920, 1080, 5, 1, &fhd_5) >
           S(HVCAM_FMT_MJPEG, 1920, 1080, 30, 1, &fhd_5));
    assert(S(HVCAM_FMT_MJPEG, 1920, 1080, 30, 1, &fhd_30) >
           S(HVCAM_FMT_YUY2, 1920, 1080, 5, 1, &fhd_30));

    /* prefer_format is a hard filter, not a preference: other
     * formats are eliminated even when they score far higher —
     * --format yuy2 must not silently hand back MJPEG just because
     * YUY2 exists only at VGA. */
    assert(S(HVCAM_FMT_MJPEG, 1920, 1080, 30, 1, &fhd_yuy2) == INT32_MIN);
    assert(S(HVCAM_FMT_NV12, 1920, 1080, 30, 1, &fhd_yuy2) == INT32_MIN);
    /* Survives the filter with a positive score: -250 in size
     * penalties is still outweighed by the 30 fps plateau. */
    assert(S(HVCAM_FMT_YUY2, 640, 480, 30, 1, &fhd_yuy2) > 0);
    assert(S(HVCAM_FMT_YUY2, 640, 480, 30, 1, &fhd_yuy2) >
           S(HVCAM_FMT_MJPEG, 1920, 1080, 30, 1, &fhd_yuy2));

    /* Regression: the >=30fps plateau must outrank resolution and
     * format combined — a 30 fps VGA YUY2 beats a 15 fps 1080p
     * MJPEG in default negotiation (previously 95 < 102 picked the
     * slower large mode). */
    assert(S(HVCAM_FMT_YUY2, 640, 480, 30, 1, &none) >
           S(HVCAM_FMT_MJPEG, 1920, 1080, 15, 1, &none));

    /* NTSC 29.97 rounds into the >=30 plateau, not under it. */
    assert(S(HVCAM_FMT_MJPEG, 1920, 1080, 30000, 1001, &none) >
           S(HVCAM_FMT_MJPEG, 1920, 1080, 15, 1, &none));

    /* Bigger beats smaller at equal fps when nothing is requested. */
    assert(S(HVCAM_FMT_YUY2, 1920, 1080, 15, 1, &none) >
           S(HVCAM_FMT_YUY2, 1280, 720, 15, 1, &none));

    /* Unknown wire formats and a missing frame-rate attribute are
     * handled, not crashed on. */
    assert(S(0, 1920, 1080, 30, 1, &none) == INT32_MIN);
    (void)S(HVCAM_FMT_YUY2, 1920, 1080, 0, 0, &none);

    /* Fractional rates (NTSC 29.97) reach the >=30 plateau like 30. */
    assert(S(HVCAM_FMT_MJPEG, 1920, 1080, 30000, 1001, &fhd) >
           S(HVCAM_FMT_YUY2, 1920, 1080, 5, 1, &fhd));

    /* Negative request values are "unset", not a huge unsigned
     * request — a negative width must score exactly like none. */
    const HvScoreOpts neg = {-1, -1, -1, 0};
    assert(S(HVCAM_FMT_MJPEG, 1920, 1080, 30, 1, &neg) ==
           S(HVCAM_FMT_MJPEG, 1920, 1080, 30, 1, &none));

    printf("test_score: OK\n");
    return 0;
}
