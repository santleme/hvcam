/* hvcam media-type scoring — pure function shared by the Windows
 * host (mf_capture.cpp) and the Linux ctest suite.
 *
 * Picks one native camera media type. Deliberately platform-free:
 * the scorer sees enum HvPixelFormat + geometry + a rational frame
 * rate, never a GUID or an IMFMediaType. Compiles as C99 and C++17.
 */
#ifndef HVCAM_MEDIA_SCORE_H
#define HVCAM_MEDIA_SCORE_H

#include <stdint.h>

#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HvScoreOpts {
    int32_t width;          /* requested; 0 = any */
    int32_t height;         /* requested; 0 = any */
    int32_t fps;            /* requested; 0 = any */
    uint32_t prefer_format; /* enum HvPixelFormat; 0 = auto — when set,
                             * a HARD filter: other formats score
                             * INT32_MIN and cannot be selected */
} HvScoreOpts;

/* Higher score = better type. Returns INT32_MIN for a wire format
 * the protocol cannot carry (fmt == 0). fps_num/fps_den is the
 * camera-advertised frame interval as a ratio; fps_den == 0 means
 * "rate unknown". */
static inline int32_t hvcam_media_score(uint32_t fmt, uint32_t w, uint32_t h,
                                        uint32_t fps_num, uint32_t fps_den,
                                        const HvScoreOpts *o)
{
    if (!fmt)
        return INT32_MIN;
    /* --format is a hard filter: an explicitly chosen subtype must
     * never silently degrade into another format that scores higher.
     * No matching type = "no usable native type" error, which is the
     * honest answer to an impossible --format request. */
    if (o->prefer_format && fmt != o->prefer_format)
        return INT32_MIN;
    int32_t score = 0;
    if (o->width && w == (uint32_t)o->width) score += 10000;
    if (o->height && h == (uint32_t)o->height) score += 10000;
    if (o->width && w != (uint32_t)o->width)
        score -= (w > (uint32_t)o->width ? 200 : 50);
    if (o->height && h != (uint32_t)o->height) score -= 200;
    /* Delivered frame rate. On USB 2.0 the isochronous ceiling
     * (~24 MB/s) pins 1080p YUY2 (4.15 MB/frame) at ~5 fps while the
     * camera's on-chip MJPEG sustains 30 — preferring raw formats
     * unconditionally picks the slower stream. Reaching the >=30fps
     * plateau outranks the resolution and format terms COMBINED
     * (a 30 fps VGA mode beats a 15 fps 1080p mode); below it the
     * score is linear. NTSC 29.97 rounds into the plateau. */
    uint32_t fps = fps_den ? (fps_num + fps_den / 2) / fps_den : 0;
    if (o->fps && fps == (uint32_t)o->fps)
        score += 1000;
    score += fps >= 30 ? 300 : (int32_t)fps * 2;
    /* Resolution, saturating at 1080p: with no explicit request the
     * largest smooth mode should beat a tiny faster one. */
    uint32_t px = w * h;
    score += (int32_t)((px > 2073600u ? 2073600u : px) / 51840u);
    /* Format preference breaks ties at equal advertised fps. The
     * winner flips at 720p: a big raw frame (1080p YUY2 = 4.15 MB,
     * NV12 = 3.1 MB) cannot actually reach its advertised rate over
     * USB 2.0's ~24 MB/s isochronous ceiling — MJPEG (~0.2 MB) can —
     * so compression wins large-frame ties; below 720p the raw
     * pixels win (they fit the bus and skip a decode). */
    int32_t large = px >= 921600u; /* >= 1280x720 */
    if (fmt == HVCAM_FMT_MJPEG) score += large ? 32 : 20;
    else if (fmt == HVCAM_FMT_YUY2) score += 30;
    else if (fmt == HVCAM_FMT_NV12) score += 25;
    else if (fmt == HVCAM_FMT_BGR24) score += 10;
    return score;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HVCAM_MEDIA_SCORE_H */
