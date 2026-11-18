# hvcam

**Low-latency webcam streaming from a Windows host into WSL2 over Hyper-V sockets.**

A static, dependency-free `hvcam-host.exe` captures the camera on Windows via Media
Foundation. On the Linux side: a small C client library (`libhvcam`), a `hvcam-dump`
CLI, and a minimal ctypes/numpy Python binding.

## Contents

- [How it works](#how-it-works)
- [Why hvcam](#why-hvcam)
- [Architecture](#architecture)
- [Getting started](#getting-started)
- [Repository layout](#repository-layout)
- [Status](#status)
- [License](#license)

## How it works

The WSL2 guest connects to the host on `VMADDR_CID_HOST` (cid 2) and a fixed port —
no IP discovery, no firewall prompts, no NAT. The client **pulls** the newest frame
when it is ready; the host never queues stale frames. Transport runs over the
hypervisor's shared-memory channel (VMBus): sub-millisecond latency and enough
bandwidth for uncompressed video. A `--tcp` fallback covers development and
non-WSL guests.

## Why hvcam

- **usbipd-win** requires rebuilding the WSL2 kernel for `uvcvideo` and is fragile
  with isochronous USB traffic.
- **Python/ffmpeg bridges** need a Windows-side runtime, depend on the dynamic
  `vEthernet` IP (it changes every boot), and buffer stale frames under load.
- **OpenCV `cap.set()` negotiation** can hang UVC drivers at runtime — hvcam
  negotiates the native media type once, before streaming starts.

## Architecture

| Layer | Design |
| --- | --- |
| Transport | `AF_HYPERV`/`AF_VSOCK` over VMBus; service GUID `{<port as 8-digit hex>-facb-11e6-bd58-64006a7986d3}` (port 8765 → `0000223d-facb-11e6-bd58-64006a7986d3`) |
| Capture | `IMFSourceReader` async callback on an MF worker thread; native media type set before the pump starts |
| Negotiation | Scores every native type once (see [`include/media_score.h`](include/media_score.h)): explicit requests (`--width`/`--height`/`--fps`/`--format`) dominate, then **reaching the ≥30 fps plateau** (a 30 fps VGA mode beats a 15 fps 1080p one), then resolution, then format. At ≥720p MJPEG wins equal-fps ties — a 1080p raw frame (YUY2 4.15 MB, NV12 3.1 MB) cannot actually reach its advertised rate through USB 2.0's ~24 MB/s isochronous ceiling (~5 real fps), while on-chip MJPEG (~0.2 MB/frame) sustains 30 fps and shrinks the wire payload ~17× |
| Exchange | Three-slot "always newest" buffer — capture and send never block each other |
| Wire | 1-byte REQ pull → 36-byte `HvFrameHeader` + payload (YUY2, NV12, BGR24, BGRX32, MJPEG). See [`include/protocol.h`](include/protocol.h) |

WSL2's utility VM only connects to listeners bound to its exact VmId — a wildcard
bind never matches (microsoft/WSL#5751). The host therefore auto-pins the vsock bind
to the running WSL VM via `wslinfo --vm-id` (no admin needed) and **re-pins
automatically when the id changes** — the utility VM gets a fresh VmId on every
`wsl --shutdown`, idle stop, or Windows reboot. Wildcard is the fallback when no
WSL VM is detected. Every accepted peer VmId is logged; `--vm-guid <GUID>` pins
explicitly, and `--no-auto-pin` keeps the wildcard bind for serving non-WSL
Hyper-V guests on a host that also has WSL installed.

## Getting started

### Requirements

| Side | Needs |
| --- | --- |
| Windows host | MSVC + Windows SDK ≥ 10.0.17763, CMake ≥ 3.20 |
| Linux guest | WSL2, CMake ≥ 3.20, C99 toolchain |
| Python binding | numpy; opencv only for `to_bgr()` |

### Build

```bash
# Windows host — produces a static /MT hvcam-host.exe
cmake -S . -B build
cmake --build build --config Release

# Linux guest — produces libhvcam.so + hvcam-dump
cmake -S . -B build && cmake --build build
ctest --test-dir build
```

### Run

> **One-time vsock setup.** Windows drops guest→host `AF_VSOCK` connections until
> the service GUID is registered for that port. Run once, as Administrator:
>
> ```
> hvcam-host --register-vsock --port 8765
> ```
>
> `--unregister-vsock` removes it. The `--tcp` path needs no registration —
> and neither does `--reverse`, where the host connects out to the guest's
> listener instead (see below).

> **Why 30 fps needs `--exposure`.** UVC firmware runs low-light
> compensation: indoors it stretches the shutter from 33 ms (1/30 s)
> toward ~50 ms (1/20 s), so the camera physically emits ~20 fps even
> when its descriptor advertises 30 — with motion blur on fast moves.
> `--exposure -6` (1/64 s) pins the shutter under the 1/30 s budget and
> the sensor delivers a real 30.8 fps (measured); add `--gain` to lift
> the darker frame — a slightly dark, sharp frame beats a bright,
> blurred one for inference.
>
> On shared hosts, combine `--register-vsock` with `--vm-guid <GUID>` so only the
> intended guest can connect.

```bash
# Windows
hvcam-host --list                              # enumerate UVC cameras
hvcam-host --device 0 --width 1280 --height 720 --lock-focus
hvcam-host --format mjpeg                      # hard filter to one wire format (yuy2|nv12|mjpeg|
                                               #   bgr24|bgrx32); fails if the camera lacks it
hvcam-host --exposure -6 --gain 40             # manual exposure (log2 s) + gain — real 30 fps
hvcam-host --tcp 127.0.0.1:8765                # TCP fallback (IP[:port]); unencrypted
hvcam-host --reverse                           # connect OUT to the guest listener — zero admin
hvcam-host --vm-guid <guid>                    # accept only that guest (skips auto-pin)
hvcam-host --no-auto-pin                       # wildcard bind, never re-pin (non-WSL guests)

# WSL2 — use the same port the host is listening on (default 8765)
./build/hvcam-dump                             # print seq/ts/fps for ~5 s
./build/hvcam-dump --tcp 127.0.0.1:8765        # TCP fallback
./build/hvcam-dump --listen                    # reverse mode: host dials in (pair with --reverse)
```

> **Zero-admin path.** `--reverse` inverts who dials: the WSL side
> listens, the host connects out by VmId. Host→guest hvsock needs no
> `GuestCommunicationServices` key, so the pair `--reverse` +
> `hvcam-dump --listen` (or `Camera(listen=True)`) works with no
> `--register-vsock` at all. In normal guest→host mode the host now
> warns at startup when the port's service GUID is unregistered —
> that is the failure that used to surface as a silent client
> timeout.

### Python

```python
import hvcam                 # bindings/python/hvcam.py
cam = hvcam.Camera()         # vsock :8765
cam = hvcam.Camera(listen=True)   # or: wait for `hvcam-host --reverse`
info, raw = cam.grab()       # newest frame — zero-copy np view
bgr = cam.to_bgr(info, raw)  # optional cv2 conversion
# raw stays valid for the next `ring-1` grabs (Camera(ring=3) by
# default — a second thread can keep reading while you pull).
# .copy() to keep it longer; ring=1 restores single-buffer mode.
```

`grab()` needs `libhvcam.so` on `LD_LIBRARY_PATH` or next to `hvcam.py`;
`HVCAM_LIB=<path>` pins an explicit build (ctest uses it so `cmake -B out`
never loads a stale default-build library).
`to_bgr()` decodes MJPEG with PyTurboJPEG when importable (~2.5× faster
than `cv2.imdecode`: 3.0 ms vs 7.8 ms at 1080p) and falls back to cv2
otherwise; `HVCAM_TURBOJPEG=<path to libturbojpeg.so>` points at a
non-system library, `HVCAM_FASTDCT=1` trades a little quality for speed.

## Repository layout

```
include/protocol.h      wire format: HvFrameHeader + pixel formats
include/media_score.h   media-type scoring policy (pure, unit-tested)
windows/                hvcam-host.exe — MF capture, vsock/TCP server (MSVC)
linux/                  libhvcam client + hvcam-dump CLI
bindings/python/        ctypes/numpy wrapper
tests/                  loopback test, scoring test, ring test (ctest)
```

## Status

Validated on hardware (2026-11-17, Windows 11 + WSL2 + USB UVC camera): MSVC build
compiles clean, `--list` enumerates devices, and the full path — Media Foundation
capture → triple-buffer → wire protocol → `libhvcam` → `hvcam-dump` — delivers real
frames over TCP and vsock. With fps-aware negotiation the host now picks
`1920×1080 MJPEG @30` on a USB 2.0 camera (previously `YUY2 @~5 fps`); measured
~19–20 fps delivered over both transports vs 13.6 fps forced-NV12 — the delta to
30 was sensor exposure (low-light comp), not bus or protocol: with
`--exposure -6` the same camera delivers ~30.8 real fps end-to-end. `--reverse` +
`hvcam-dump --listen` and `Camera(listen=True)` were verified end-to-end with no
registry entry. Guest→host vsock still accepts
the one-time `--register-vsock` step; it is no longer required when `--reverse`
is used, and its absence is now a loud startup warning instead of a silent
client timeout.

## License

[MIT](LICENSE)
