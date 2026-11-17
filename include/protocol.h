/* hvcam wire protocol — shared between the Windows host (C++) and the
 * Linux client (C). Compiles in both languages.
 *
 * Transport: client sends one REQ byte (pull), host replies with a
 * fixed 36-byte HvFrameHeader followed by `payload_bytes` of image
 * data. All integers are little-endian on the wire (both sides are
 * little-endian today; the struct is packed and fields are fixed
 * width — no padding, no endianness surprises on x86/ARM64).
 */
#ifndef HVCAM_PROTOCOL_H
#define HVCAM_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HVCAM_MAGIC 0x4856434Du /* "HVCM" */
#define HVCAM_VERSION 1u
#define HVCAM_REQ 0x51u         /* 'Q': client request for latest frame */
#define HVCAM_MAX_PAYLOAD (64u * 1024u * 1024u) /* sanity bound, 64 MiB */

enum HvPixelFormat {
    HVCAM_FMT_BGR24 = 1, /* rows top-down */
    HVCAM_FMT_YUY2 = 2,  /* packed YUV 4:2:2, 2 bytes/px */
    HVCAM_FMT_NV12 = 3,  /* planar Y + interleaved UV */
    HVCAM_FMT_MJPEG = 4, /* camera-native JPEG, payload is a .jpg blob */
    HVCAM_FMT_BGRX32 = 5, /* 4 bytes/px, BGR + padding byte */
};

#pragma pack(push, 1)
typedef struct HvFrameHeader {
    uint32_t magic;         /* HVCAM_MAGIC */
    uint16_t version;       /* HVCAM_VERSION */
    uint16_t flags;         /* bit0: payload valid; bit1: bottom-up rows */
    uint64_t seq_num;       /* monotonic per session; UINT64_MAX = none yet */
    uint64_t timestamp_us;  /* host monotonic clock (QPC epoch = host start) */
    uint16_t width;
    uint16_t height;
    uint32_t format;        /* enum HvPixelFormat */
    uint32_t payload_bytes; /* bytes that follow this header */
} HvFrameHeader;
#pragma pack(pop)

/* 4+2+2+8+8+2+2+4+4 = 36 bytes, packed. */
typedef char hvcam_header_must_be_36_bytes[(sizeof(HvFrameHeader) == 36) ? 1 : -1];

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HVCAM_PROTOCOL_H */
