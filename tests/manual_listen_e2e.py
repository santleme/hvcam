#!/usr/bin/env python3
"""manual_listen_e2e — hardware-in-the-loop check of the --reverse
path, not part of ctest (needs the real Windows host + camera).

Run the listener first:

    python3 tests/manual_listen_e2e.py

then on Windows:

    hvcam-host --device N --reverse [--exposure -6]

Expected: the host logs `connected to guest listener`, this script
prints a real frame header and a decoded BGR array.
"""
import sys

sys.path.insert(0, __file__.rsplit("/", 2)[0] + "/bindings/python")
import hvcam

cam = hvcam.Camera(listen=True)
print("listening on vsock :8765 — start hvcam-host --reverse")
info, raw = cam.grab(timeout_ms=60000)
print("grab:", info)
bgr = cam.to_bgr(info, raw)
print("to_bgr:", None if bgr is None else (bgr.shape, bgr.dtype))
assert bgr is not None and bgr.shape[:2] == (info["height"], info["width"])
print("manual_listen_e2e: OK")
