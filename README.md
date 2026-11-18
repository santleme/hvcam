# hvcam

<p align="center">
  <img src="assets/hvcam-pixel-art.png" alt="Webcam HVCam com Linux no WSL2" width="420">
</p>

**Your Windows webcam, inside WSL2 — *no kernel rebuild, no usbipd, no admin.***

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows%20host%20%2B%20WSL2-lightgrey)

A static, dependency-free `hvcam-host.exe` captures the camera on Windows via
**Media Foundation** and streams it into WSL2 over **Hyper-V sockets** (VMBus).
On the Linux side: a small C library (`libhvcam`), a `hvcam-dump` CLI, and a
ctypes/numpy Python binding. The client *pulls* the newest frame when it is
ready — the host never queues stale frames.

## Why not the usual routes?

- **usbipd-win** needs a rebuilt WSL2 kernel for `uvcvideo`, and isochronous
  USB traffic is fragile — raw 1080p over USB 2.0 collapses to ~5 real fps.
- **ffmpeg/Python bridges** need a Windows-side runtime, chase the `vEthernet`
  IP that changes every boot, and buffer stale frames under load.
- **`cv2.VideoCapture` negotiation** can hang UVC drivers at runtime. hvcam
  negotiates the native media type *once*, before streaming starts.

## Quick start

Build both sides (MSVC + Windows SDK ≥ 10.0.17763 on Windows; CMake ≥ 3.20
and a C99 toolchain in WSL2):

```bash
cmake -S . -B build && cmake --build build --config Release   # Windows: hvcam-host.exe
cmake -S . -B build && cmake --build build && ctest --test-dir build   # WSL2: libhvcam + hvcam-dump
```

Run it — the **zero-admin path** first: the host dials *out* to the guest,
so no registry setup is needed at all.

```bash
# Windows
hvcam-host --reverse --device 0

# WSL2
./build/hvcam-dump --listen
```

> The classic guest→host mode also works; it needs a one-time
> `hvcam-host --register-vsock --port 8765` as Administrator. `--reverse`
> skips that entirely. Each listen accepts **one** host connection — if the
> host restarts, start a new listener (the host retries forever, so a small
> restart loop makes the link persistent).

### Python

```python
import hvcam
cam = hvcam.Camera(listen=True)   # or Camera() in guest→host mode
info, raw = cam.grab()            # newest frame, zero-copy numpy view
bgr = cam.to_bgr(info, raw)       # optional OpenCV conversion
```

`grab()` needs `libhvcam.so` on `LD_LIBRARY_PATH` or next to `hvcam.py`;
`HVCAM_LIB=<path>` pins an explicit build. Treat `HVCAM_LIB` /
`HVCAM_TURBOJPEG` like `LD_LIBRARY_PATH` — they load native code, so only
point them at trusted locations.

## Measured on real hardware

*Windows 11 + WSL2 + USB 2.0 UVC camera, 2026-11-17:*

| Result | Number |
| --- | --- |
| Negotiated mode | **1920×1080 MJPEG @ 30 fps** (scored over every native type) |
| Delivered, default exposure | ~19–20 fps |
| Delivered, `--exposure -6` | **30.8 real fps** end-to-end |
| MJPEG decode, 1080p | 3.0 ms (PyTurboJPEG) vs 7.8 ms (`cv2.imdecode`) |

> *Why `--exposure` matters:* UVC firmware stretches the shutter in low
> light (~50 ms), so the sensor physically emits ~20 fps even when it
> advertises 30. Pinning the shutter under the 33 ms budget
> (`--exposure -6`, plus `--gain` to lift the darker frame) is what makes
> 30 fps real — a slightly dark, sharp frame beats a bright, blurred one
> for inference.

## How it works

- **Transport** — `AF_HYPERV`/`AF_VSOCK` over VMBus: no IP discovery, no
  firewall prompts, no NAT. `--tcp` fallback for development. The host
  auto-pins its bind to the running WSL VmId and re-pins when it changes
  (a wildcard bind never matches WSL2's utility VM — microsoft/WSL#5751).
- **Negotiation** — every native media type is scored once
  ([`include/media_score.h`](include/media_score.h)): explicit requests win,
  then reaching the ≥30 fps plateau, then resolution, then format. At ≥720p
  MJPEG wins equal-fps ties — it is ~17× smaller on the wire than raw YUY2.
- **Exchange** — a three-slot *always-newest* buffer; capture and send never
  block each other.
- **Wire** — 1-byte REQ pull → 36-byte header + payload (YUY2, NV12, BGR24,
  BGRX32, MJPEG). Contract in [`include/protocol.h`](include/protocol.h).

Useful flags: `--width/--height/--fps`, `--format mjpeg` (hard filter),
`--lock-focus`, `--lock-exposure` / `--lock-gain` (pin the current values
instead of setting them), `--vm-guid <GUID>` (accept only that guest),
`--no-auto-pin` (serve non-WSL Hyper-V guests).

## Layout

```
include/    wire protocol + media-type scoring policy
windows/    hvcam-host.exe — MF capture, vsock/TCP server
linux/      libhvcam client + hvcam-dump CLI
bindings/   Python (ctypes/numpy)
tests/      loopback, scoring and ring tests (ctest)
```

## Status

Validated end-to-end on hardware: capture → triple buffer → wire →
`libhvcam` → `hvcam-dump`, over both vsock and TCP, including the
zero-admin `--reverse` path. MIT licensed — see [LICENSE](LICENSE).
