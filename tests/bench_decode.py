#!/usr/bin/env python3
"""Grab one real MJPEG frame over TCP and benchmark decode paths:
cv2.imdecode (fresh 6 MB alloc/frame) vs PyTurboJPEG dst-ring if
available. Usage: python3 tests/bench_decode.py <host:port>
"""
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 2)[0] + "/bindings/python")
import numpy as np
import hvcam

cam = hvcam.Camera(tcp=sys.argv[1])
info, raw = cam.grab(timeout_ms=8000)
print(f"frame fmt={info['format']} {info['width']}x{info['height']} "
      f"{len(raw)}B")
assert info["format"] == 4, "need an MJPEG frame — run host --format mjpeg"

import cv2

N = 60
cv2.setNumThreads(4)

t0 = time.perf_counter()
for _ in range(N):
    img = cv2.imdecode(raw, cv2.IMREAD_COLOR)
d_cv = (time.perf_counter() - t0) / N
print(f"cv2.imdecode:        {d_cv*1e3:6.2f} ms/frame  shape={img.shape}")

t0 = time.perf_counter()
for _ in range(N):
    buf = np.empty(1920 * 1080 * 3, dtype=np.uint8)
d_alloc = (time.perf_counter() - t0) / N
print(f"np.empty(6.2MB):     {d_alloc*1e3:6.2f} ms/frame  (alloc-only floor)")

try:
    from turbojpeg import TurboJPEG, TJPF_BGR, TJFLAG_FASTDCT
except ImportError:
    print("PyTurboJPEG: not installed")
    sys.exit(0)

import os
lib = os.environ.get("HVCAM_TURBOJPEG")  # path to libturbojpeg.so if not system-installed
tj = TurboJPEG(lib_path=lib) if lib else TurboJPEG()
t0 = time.perf_counter()
for _ in range(N):
    img2 = tj.decode(bytes(raw), pixel_format=TJPF_BGR)
d_tj = (time.perf_counter() - t0) / N
print(f"turbojpeg.decode:    {d_tj*1e3:6.2f} ms/frame  shape={img2.shape}")

t0 = time.perf_counter()
for _ in range(N):
    img3 = tj.decode(bytes(raw), pixel_format=TJPF_BGR, flags=TJFLAG_FASTDCT)
d_fast = (time.perf_counter() - t0) / N
print(f"turbojpeg FASTDCT:   {d_fast*1e3:6.2f} ms/frame")

import inspect
sig = inspect.signature(tj.decode)
print("decode() params:", list(sig.parameters))
print("quality equal:", np.array_equal(img2, img))
