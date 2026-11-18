"""hvcam — minimal ctypes client for libhvcam.

    import hvcam
    cam = hvcam.Camera()                    # vsock, port 8765
    info, frame = cam.grab()                # newest frame, blocking
    bgr = cam.to_bgr(info, frame)           # cv2 conversion, or None
    cam.close()

Needs libhvcam.so on LD_LIBRARY_PATH (or built next to this file)
and numpy. cv2 is optional — only to_bgr() imports it; grab()
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
    # HVCAM_LIB pins an explicit .so (ctest sets it to the configured
    # build dir so `cmake -B out` cannot go stale). The pin is
    # authoritative: a bad path must fail loudly — falling back to a
    # different build would let CI test the wrong library while
    # believing it tested the requested one.
    pinned = os.environ.get("HVCAM_LIB")
    if pinned:
        lib = ctypes.CDLL(pinned)
    else:
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
    lib.hvcam_listen.argtypes = [ctypes.c_uint32]
    lib.hvcam_listen.restype = ctypes.c_int
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

_TJ = None
_TJ_PF_BGR = 0
_TJ_FLAGS = 0
_TJ_TRIED = False


def _turbojpeg():
    """PyTurboJPEG if installed — ~2.5x faster than cv2.imdecode on
    1080p (measured 3.0 ms vs 7.8 ms). HVCAM_TURBOJPEG=<path> points
    at a non-system libturbojpeg; HVCAM_FASTDCT=1 trades a little
    quality for ~5% more speed. Any import/load problem -> cv2."""
    global _TJ, _TJ_PF_BGR, _TJ_FLAGS, _TJ_TRIED
    if _TJ_TRIED:
        return _TJ
    _TJ_TRIED = True
    try:
        from turbojpeg import TurboJPEG, TJPF_BGR, TJFLAG_FASTDCT
        lib = os.environ.get("HVCAM_TURBOJPEG")
        _TJ = TurboJPEG(lib_path=lib) if lib else TurboJPEG()
        _TJ_PF_BGR = TJPF_BGR
        _TJ_FLAGS = TJFLAG_FASTDCT if os.environ.get("HVCAM_FASTDCT") else 0
    except Exception:
        _TJ = None
    return _TJ


class Camera:
    """One client connection; pulls the newest frame on demand.

    `ring` receive slots rotate behind the returned arrays: a frame
    from grab() stays intact for the next `ring - 1` grab() calls —
    another thread can keep processing it while the capture loop
    pulls ahead. ring=1 restores the old single-buffer semantics
    (every grab() clobbers the previous array). Still zero-copy:
    the arrays are views into the slots.

    `listen=True` waits for the Windows host to connect in
    (`hvcam-host --reverse`) instead of dialing out — the path that
    needs no admin-side service registration. Note the listener
    accepts ONE host connection: if the host restarts, create a new
    Camera(listen=True) to accept the next one.
    """

    def __init__(self, port=8765, tcp=None, listen=False, ring=3):
        if ring < 1:
            raise ValueError("hvcam: ring must be >= 1")
        if tcp is not None:
            host, _, p = tcp.partition(":")
            self.fd = _LIB.hvcam_connect_tcp(
                host.encode() or None, int(p or port)
            )
        elif listen:
            self.fd = _LIB.hvcam_listen(port)
        else:
            self.fd = _LIB.hvcam_connect(port)
        if self.fd < 0:
            raise OSError(f"hvcam: connect failed ({self.fd})")
        # np.empty: virtual reservation, pages commit on first write —
        # unlike a ctypes buffer it is not zero-filled up front.
        self._slots = [
            np.empty(_MAX_PAYLOAD, dtype=np.uint8) for _ in range(ring)
        ]
        self._slot = 0
        self._hdr = HvFrameHeader()
        self._last = ctypes.c_uint64(0xFFFFFFFFFFFFFFFF)

    def grab(self, timeout_ms=1000):
        """Returns (header_dict, np.ndarray view over payload).

        Blocks until a frame newer than the previous one arrives or
        `timeout_ms` elapses (then raises TimeoutError)."""
        slot = self._slots[self._slot]
        n = _LIB.hvcam_grab_next(
            self.fd, ctypes.byref(self._hdr), slot.ctypes.data,
            _MAX_PAYLOAD, ctypes.byref(self._last), timeout_ms,
        )
        if n == -2:
            raise TimeoutError("hvcam: no new frame")
        if n < 0:
            raise OSError("hvcam: stream dropped")
        self._slot = (self._slot + 1) % len(self._slots)
        h = self._hdr
        # View into the current ring slot — stays valid for the next
        # len(slots)-1 grabs; .copy() to keep it longer.
        arr = slot[:n]
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
            tj = _turbojpeg()
            if tj is not None:
                try:
                    img = tj.decode(frame, pixel_format=_TJ_PF_BGR,
                                    flags=_TJ_FLAGS)
                except OSError:
                    img = None
            else:
                img = cv2.imdecode(frame, cv2.IMREAD_COLOR)
            if img is None:
                raise ValueError("hvcam: MJPEG payload failed to decode")
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
