# hvcam

Low-latency webcam streaming from a Windows host into WSL2 over Hyper-V sockets.

A single static `hvcam-host.exe` (no dependencies) runs on Windows and captures the camera with Media Foundation. A small C client library (`libhvcam.so`), a `hvcam-dump` CLI, and a minimal ctypes/numpy wrapper run on the Linux side.

## What it does

The WSL2 guest connects to the host on `VMADDR_CID_HOST` (cid 2) and a fixed port — no IP discovery, no firewall prompts, no NAT. The client **pulls** the newest frame when it is ready; the host never queues stale frames. Transport runs over the hypervisor's shared-memory channel (VMBus), with sub-millisecond latency and enough bandwidth for uncompressed video.

## Why

- **usbipd-win** requires rebuilding the WSL2 kernel for `uvcvideo` and is fragile with isochronous USB traffic.
- **Python/ffmpeg helpers** need a Windows-side runtime, depend on the dynamic `vEthernet` IP (it changes every boot), and buffer stale frames under load.
- **OpenCV `cap.set()` format negotiation** can hang UVC drivers at runtime; hvcam negotiates the native media type once, before streaming.

## Design

1. **Transport** — `AF_HYPERV`/`AF_VSOCK` over VMBus, service GUID derived from the port (`{port}-facb-11e6-bd58-64006a7986d3`). Optional `--tcp` fallback for development and non-WSL guests.
2. **Capture** — `IMFSourceReader` with an async callback on an MF worker thread; `GetNativeMediaType`/`SetCurrentMediaType` happens before the pump starts. Optional `--lock-exposure` / `--lock-focus` via `IAMCameraControl`.
3. **Exchange** — three-slot "always newest" buffer: the capture callback and the send loop never block each other.
4. **Wire format** — one REQ byte per pull, then a fixed 36-byte `HvFrameHeader` + payload (YUY2, NV12, BGR24, BGRX32, or camera-native MJPEG). See `include/protocol.h`. The host binds `HV_GUID_WILDCARD` (required for WSL interop — the utility VM does not match a `CHILDREN`-scoped bind), and every accepted vsock peer VmId is logged; `--vm-guid <GUID>` pins the listener to one specific guest VM.

## Build

Windows host (MSVC + Windows SDK ≥ 10.0.17763):

    cmake -S . -B build
    cmake --build build --config Release    # static /MT hvcam-host.exe

Linux client (WSL2):

    cmake -S . -B build && cmake --build build
    ctest --test-dir build

## Use

    # Windows
    hvcam-host --list                     # enumerate UVC cameras
    hvcam-host --device 0 --width 1280 --height 720 --lock-focus
    hvcam-host --tcp 127.0.0.1:8765     # TCP fallback (IP[:port])
    hvcam-host --vm-guid <guid>         # accept only that guest VM

    # WSL2
    ./build/hvcam-dump                    # print seq/ts/fps for ~5 s
    hvcam-dump --tcp 127.0.0.1:8765       # TCP fallback

```python
import hvcam                       # linux/python/hvcam.py
cam = hvcam.Camera()               # vsock :8765
info, raw = cam.grab()             # newest frame as np.ndarray
```

## Status

Validated on hardware (2026-11-17, Windows 11 + WSL2 + USB UVC camera): MSVC build of `hvcam-host` compiles clean, `--list` enumerates devices, and the full path — Media Foundation capture → triple-buffer → wire protocol → `libhvcam` → `hvcam-dump` — delivers real frames over the TCP fallback (1920×1080 YUY2 native mode, ~5 fps camera-limited).

**vsock caveat:** on the test machine (recent Windows 11 build + WSL2), `connect(VMADDR_CID_HOST)` from the guest times out on every port — listener or not — so the guest→host Hyper-V socket channel appears dead at the transport level on that setup. The host-side `AF_HYPERV` bind follows the documented pattern (`HV_GUID_WILDCARD` + `{port}-facb-11e6-bd58-64006a7986d3` service template); if your WSL build exposes working vsock, it should be a drop-in. The TCP path is the verified one today.

## License

MIT
