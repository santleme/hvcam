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
| Exchange | Three-slot "always newest" buffer — capture and send never block each other |
| Wire | 1-byte REQ pull → 36-byte `HvFrameHeader` + payload (YUY2, NV12, BGR24, BGRX32, MJPEG). See [`include/protocol.h`](include/protocol.h) |

The host binds `HV_GUID_WILDCARD` (required for WSL interop) and logs every accepted
vsock peer VmId. On multi-VM or shared hosts, pin the listener with `--vm-guid <GUID>`
so only that guest can pull frames.

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
> `--unregister-vsock` removes it. The `--tcp` path needs no registration.
>
> On shared hosts, combine `--register-vsock` with `--vm-guid <GUID>` so only the
> intended guest can connect.

```bash
# Windows
hvcam-host --list                              # enumerate UVC cameras
hvcam-host --device 0 --width 1280 --height 720 --lock-focus
hvcam-host --tcp 127.0.0.1:8765                # TCP fallback (IP[:port]); unencrypted
hvcam-host --vm-guid <guid>                    # accept only that guest VM

# WSL2 — use the same port the host is listening on (default 8765)
./build/hvcam-dump                             # print seq/ts/fps for ~5 s
./build/hvcam-dump --tcp 127.0.0.1:8765        # TCP fallback
```

### Python

```python
import hvcam                 # bindings/python/hvcam.py
cam = hvcam.Camera()         # vsock :8765
info, raw = cam.grab()       # newest frame — zero-copy np view
bgr = cam.to_bgr(info, raw)  # optional cv2 conversion
```

`grab()` needs `libhvcam.so` on `LD_LIBRARY_PATH` or next to `hvcam.py`.

## Repository layout

```
include/protocol.h    wire format: HvFrameHeader + pixel formats
windows/              hvcam-host.exe — MF capture, vsock/TCP server (MSVC)
linux/                libhvcam client + hvcam-dump CLI
bindings/python/      ctypes/numpy wrapper
tests/                loopback test (ctest)
```

## Status

Validated on hardware (2026-11-17, Windows 11 + WSL2 + USB UVC camera): MSVC build
compiles clean, `--list` enumerates devices, and the full path — Media Foundation
capture → triple-buffer → wire protocol → `libhvcam` → `hvcam-dump` — delivers real
frames over TCP (1920×1080 YUY2 native mode, ~5 fps camera-limited). Guest→host
vsock requires the one-time `--register-vsock` step above; TCP needs no
registration and is the verified path today.

## License

[MIT](LICENSE)
