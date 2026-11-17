"""hvcam — minimal ctypes client for libhvcam.

    import hvcam
    cam = hvcam.Camera()                    # vsock, port 8765
    info, frame = cam.grab()                # newest frame, blocking
    bgr = cam.to_bgr(info, frame)           # cv2 conversion, or None
    cam.close()

Needs libhvcam.so on LD_LIBRARY_PATH (or hvcam.build it next to this
file) and numpy. cv2 is optional — only to_bgr() imports it; grab()
hands you the raw wire payload either way.
"""

import ctypes
import os

import numpy as np

_FMT_BPP = {1: 3, 2: 2, 3: 1, 4: 1, 5: 4}  # nominal bytes/px; mjpeg = blob


class HvFrameHeader(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("version", ctypes.c_uint16),
        ("flags", ctypes.c_uint16),
        ("seq_num", ctypes.c_uint64),
        ("timestamp_us", ctypes.c_uint64),
        ("width", ctypes.c_uint16),
        ("height", ctypes.c_uint16),
        ("format", ctypes.c_uint32),
        ("payload_bytes", ctypes.c_uint32),
    ]


def _load():
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (
        os.path.join(here, "libhvcam.so"),
        os.path.join(here, "..", "..", "build", "libhvcam.so"),
        "libhvcam.so",
    ):
        try:
            lib = ctypes.CDLL(cand)
            break
        except OSError:
            continue
    else:
        raise OSError("libhvcam.so not found (LD_LIBRARY_PATH)")
    lib.hvcam_connect.argtypes = [ctypes.c_uint32]
    lib.hvcam_connect.restype = ctypes.c_int
    lib.hvcam_connect_tcp.argtypes = [ctypes.c_char_p, ctypes.c_uint16]
    lib.hvcam_connect_tcp.restype = ctypes.c_int
    lib.hvcam_grab_next.argtypes = [
        ctypes.c_int,
        ctypes.POINTER(HvFrameHeader),
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint64),
        ctypes.c_int,
    ]
    lib.hvcam_grab_next.restype = ctypes.c_int
    lib.hvcam_close.argtypes = [ctypes.c_int]
    return lib


_LIB = _load()
_MAX_PAYLOAD = 64 * 1024 * 1024


class Camera:
    """One client connection; pulls the newest frame on demand."""

    def __init__(self, port=8765, tcp=None):
        if tcp is not None:
            host, _, p = tcp.partition(":")
            self.fd = _LIB.hvcam_connect_tcp(
                host.encode() or None, int(p or port)
            )
        else:
            self.fd = _LIB.hvcam_connect(port)
        if self.fd < 0:
            raise OSError(f"hvcam: connect failed ({self.fd})")
        self._buf = (ctypes.c_ubyte * _MAX_PAYLOAD)()
        self._hdr = HvFrameHeader()
        self._last = ctypes.c_uint64(0xFFFFFFFFFFFFFFFF)

    def grab(self, timeout_ms=1000):
        """Returns (header_dict, np.ndarray view over payload).

        Blocks until a frame newer than the previous one arrives or
        `timeout_ms` elapses (then raises TimeoutError)."""
        n = _LIB.hvcam_grab_next(
            self.fd, ctypes.byref(self._hdr), self._buf,
            _MAX_PAYLOAD, ctypes.byref(self._last), timeout_ms,
        )
        if n == -2:
            raise TimeoutError("hvcam: no new frame")
        if n < 0:
            raise OSError("hvcam: stream dropped")
        h = self._hdr
        # Real view over the persistent buffer — overwritten by the
        # next grab(); .copy() if you need to keep the frame.
        arr = np.frombuffer(self._buf, dtype=np.uint8, count=n)
        info = {
            "seq": h.seq_num,
            "ts_us": h.timestamp_us,
            "flags": h.flags,
            "width": h.width,
            "height": h.height,
            "format": h.format,
            "payload": n,
        }
        return info, arr

    @staticmethod
    def _expected_bytes(w, h, fmt):
        """Bytes the wire payload must hold for w x h in `fmt`;
        None for MJPEG (opaque blob). Unknown fmt -> ValueError."""
        if fmt == 1:
            return w * h * 3
        if fmt == 2:
            return w * h * 2
        if fmt == 3:
            return (h * 3 // 2) * w
        if fmt == 4:
            return None
        if fmt == 5:
            return w * h * 4
        raise ValueError(f"hvcam: unknown wire format {fmt}")

    def to_bgr(self, info, frame):
        """Converts a grabbed payload to BGR via cv2.cvtColor.

        Wire formats (protocol.h): 1=BGR24 2=YUY2 3=NV12 4=MJPEG
        5=BGRX32. Returns an (h, w, 3) uint8 array, or None for a
        placeholder reply (flags bit0 clear). flags bit1
        (bottom-up rows) flips the raster formats at the end.
        Never slices channels by hand — MJPEG goes through
        cv2.imdecode, YUV formats through the matching
        COLOR_YUV2BGR_* code."""
        import cv2  # late: optional dependency

        w, hgt, fmt = info["width"], info["height"], info["format"]
        flags = info.get("flags", 1)
        if not flags & 1:
            return None
        expected = self._expected_bytes(w, hgt, fmt)
        # Exact match, not just "big enough": if a row-padded buffer
        # leaked onto the wire, slicing to `expected` would read the
        # interleaved padding as pixels and silently corrupt the
        # image. The wire format has no stride field — reject until
        # the host repacks tightly (or carries the stride).
        if expected is not None and frame.nbytes != expected:
            raise ValueError(
                f"hvcam: payload {frame.nbytes}B != {expected}B for "
                f"{w}x{hgt} fmt={fmt} (unpacked stride not supported)"
            )
        if fmt == 1:
            img = frame.reshape(hgt, w, 3).copy()
        elif fmt == 2:
            img = cv2.cvtColor(
                frame.reshape(hgt, w, 2), cv2.COLOR_YUV2BGR_YUY2
            )
        elif fmt == 3:
            img = cv2.cvtColor(
                frame.reshape(hgt * 3 // 2, w),
                cv2.COLOR_YUV2BGR_NV12,
            )
        elif fmt == 4:
            img = cv2.imdecode(frame, cv2.IMREAD_COLOR)
            if img is None:
                raise ValueError("hvcam: MJPEG payload failed to decode")
            return img
        else:
            img = cv2.cvtColor(
                frame.reshape(hgt, w, 4), cv2.COLOR_BGRA2BGR
            )
        return cv2.flip(img, 0) if flags & 2 else img

    def close(self):
        if self.fd >= 0:
            _LIB.hvcam_close(self.fd)
            self.fd = -1

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def __del__(self):
        # Best-effort: never let a forgotten close() leak the fd
        # (and the 64 MiB scratch buffer) into GC limbo.
        try:
            self.close()
        except Exception:
            pass
