"""hvcam — minimal ctypes client for libhvcam.

    import hvcam
    cam = hvcam.Camera()                    # vsock, port 8765
    hdr, frame = cam.grab()                 # newest frame, blocking
    # frame is a numpy view over the raw payload; convert with cv2:
    #   yuy2 -> cv2.cvtColor(frame.reshape(h, w, 2), cv2.COLOR_YUV2BGR_YUY2)
    #   nv12 -> cv2.cvtColor(frame.reshape(h * 3 // 2, w), cv2.COLOR_YUV2BGR_NV12)
    cam.close()

Needs libhvcam.so on LD_LIBRARY_PATH (or hvcam.build it next to this
file) and numpy. No cv2 hard dependency — the raw arrays are yours.
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
            raise TimeoutError("hvcam: nenhum frame novo")
        if n < 0:
            raise OSError("hvcam: stream dropped")
        h = self._hdr
        # Real view over the persistent buffer — overwritten by the
        # next grab(); .copy() if you need to keep the frame.
        arr = np.frombuffer(self._buf, dtype=np.uint8, count=n)
        info = {
            "seq": h.seq_num,
            "ts_us": h.timestamp_us,
            "width": h.width,
            "height": h.height,
            "format": h.format,
            "payload": n,
        }
        return info, arr

    def close(self):
        if self.fd >= 0:
            _LIB.hvcam_close(self.fd)
            self.fd = -1

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
