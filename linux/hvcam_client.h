/* libhvcam — public C API for receiving frames from a Windows hvcam host.
 *
 * Connect, pull the latest frame, close. The caller provides the
 * payload buffer; hvcam_grab fills the header and up to `cap` bytes.
 */
#ifndef HVCAM_CLIENT_H
#define HVCAM_CLIENT_H

#include <stdint.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Connects to the Windows host over Hyper-V sockets (VMBus).
 * `port` is the service port the host listens on (default 8765).
 * Returns an fd >= 0, or a negative value on failure (-errno-ish). */
int hvcam_connect(uint32_t port);

/* Connects to a host over plain TCP (fallback / development).
 * `host` may be NULL for "127.0.0.1". */
int hvcam_connect_tcp(const char *host, uint16_t port);

/* Pulls the newest frame: sends REQ, reads the header, then reads
 * up to `cap` payload bytes into `payload` (may be NULL to get only
 * the header — payload is then drained and discarded).
 *
 * Returns the payload length actually delivered to `payload`
 * (0..cap), or a negative value on transport/protocol error.
 * `hdr` is always filled on success; hdr->flags bit0 tells whether
 * a real frame was present. */
int hvcam_grab(int fd, HvFrameHeader *hdr, void *payload, uint32_t cap);

/* Waits at most `timeout_ms` for a frame with seq_num different from
 * `*last_seq` (pass UINT64_MAX initially). Same return contract as
 * hvcam_grab; returns -2 on timeout (distinct from -1 transport).
 *
 * Note on timeouts: -2 means the deadline passed before ANY byte of
 * the response was consumed — safe to retry on the same connection.
 * If a deadline expires mid-response (partial header or payload),
 * the stream position is unrecoverable and this returns -1 instead;
 * the caller must close and reconnect. */
int hvcam_grab_next(int fd, HvFrameHeader *hdr, void *payload,
                    uint32_t cap, uint64_t *last_seq, int timeout_ms);

void hvcam_close(int fd);

const char *hvcam_format_name(uint32_t format);

#ifdef __cplusplus
}
#endif

#endif /* HVCAM_CLIENT_H */
