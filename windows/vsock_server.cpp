// vsock_server.cpp — AF_HYPERV listener + shared client loop.
//
// WSL2 vsock interop: a Linux guest connecting to VMADDR_CID_HOST
// (cid 2) on port P reaches the Windows socket bound to
// AF_HYPERV/SOCK_STREAM/HV_PROTOCOL_RAW with ServiceId =
// {P as DWORD}-facb-11e6-bd58-64006a7986d3. The utility VM only
// matches a listener bound to its exact VmId (microsoft/WSL#5751) —
// wildcard never matches it — so the default auto-pin re-detects the
// id via `wsl -- wslinfo --vm-id` and rebinds when it changes.
#include "vsock_server.h"

// winsock2 must come before windows.h, or windows.h drags in the
// legacy winsock.h and every socket prototype below collides.
#include <initguid.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <hvsocket.h>
#include <windows.h>
#include <objbase.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "frame_slots.h"
#include "protocol.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib") // CLSIDFromString (detect_wsl_vmid)

namespace {

GUID service_guid_for_port(uint32_t port)
{
    GUID g{};
    g.Data1 = port;
    g.Data2 = 0xfacb;
    g.Data3 = 0x11e6;
    g.Data4[0] = 0xbd;
    g.Data4[1] = 0x58;
    g.Data4[2] = 0x64;
    g.Data4[3] = 0x00;
    g.Data4[4] = 0x6a;
    g.Data4[5] = 0x79;
    g.Data4[6] = 0x86;
    g.Data4[7] = 0xd3;
    return g;
}

// `wsl -- wslinfo --vm-id` reports the running utility VM's id —
// no admin needed, and every distro shares the same VM. Returns
// false on any failure (WSL absent, not running, unparseable).
bool detect_wsl_vmid(GUID *out)
{
    FILE *p = _popen("wsl.exe -- wslinfo --vm-id", "r");
    if (!p)
        return false;
    char line[128] = {0};
    bool ok = fgets(line, sizeof(line), p) != nullptr;
    // A nonzero wsl.exe exit code means no usable id came out either.
    if (_pclose(p) != 0)
        ok = false;
    if (!ok)
        return false;
    std::string g(line);
    while (!g.empty() && isspace((unsigned char)g.back()))
        g.pop_back();
    if (g.empty())
        return false;
    if (g.front() != '{')
        g = "{" + g + "}";
    std::wstring wg(g.begin(), g.end());
    return SUCCEEDED(CLSIDFromString(wg.c_str(), out));
}

bool send_all(SOCKET s, const void *buf, int len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        int n = send(s, (const char *)p, len, 0);
        if (n <= 0)
            return false;
        p += n;
        len -= n;
    }
    return true;
}

// One client: REQ byte -> newest frame. Pull model means a slow
// consumer never accumulates stale frames; each REQ gets whatever
// the camera produced most recently.
void serve_client(SOCKET c, FrameSlots *slots)
{
    for (;;) {
        uint8_t req = 0;
        int n = recv(c, (char *)&req, 1, 0);
        if (n != 1 || req != HVCAM_REQ)
            return;

        HvSlot *slot = slots->acquire_read();
        HvFrameHeader hdr{};
        hdr.magic = HVCAM_MAGIC;
        hdr.version = HVCAM_VERSION;
        if (slot) {
            hdr.flags = 1;
            hdr.seq_num = slot->seq;
            hdr.timestamp_us = slot->ts_us;
            hdr.width = slot->w;
            hdr.height = slot->h;
            hdr.format = slot->format;
            hdr.payload_bytes = (uint32_t)slot->bytes.size();
        } else {
            hdr.flags = 0; // no frame captured yet
            hdr.payload_bytes = 0;
        }
        bool ok = send_all(c, &hdr, sizeof(hdr));
        if (ok && hdr.payload_bytes)
            ok = send_all(c, slot->bytes.data(), (int)hdr.payload_bytes);
        if (slot)
            slots->release_read(slot);
        if (!ok)
            return;
    }
}

void guid_to_string(const GUID &g, char *out, size_t cap)
{
    snprintf(out, cap,
             "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             (unsigned long)g.Data1, g.Data2, g.Data3,
             g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
             g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

struct VsockBind {
    SOCKET sock = INVALID_SOCKET;
    GUID vm{};             // valid only when pinned
    bool pinned = false;   // false = wildcard bind
};

// Non-blocking listener: the accept loop polls it, which is also
// what lets the WSL re-pin run between clients.
SOCKET listen_vsock(uint32_t port, const GUID *vmid)
{
    SOCKET l = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (l == INVALID_SOCKET) {
        fprintf(stderr, "hvcam: AF_HYPERV socket failed (WSA %d)\n",
                WSAGetLastError());
        return INVALID_SOCKET;
    }
    SOCKADDR_HV a{};
    a.Family = AF_HYPERV;
    a.VmId = vmid ? *vmid : HV_GUID_WILDCARD;
    a.ServiceId = service_guid_for_port(port);
    if (bind(l, (const sockaddr *)&a, sizeof(a)) == SOCKET_ERROR) {
        fprintf(stderr, "hvcam: vsock bind port %u failed (WSA %d)\n",
                port, WSAGetLastError());
        closesocket(l);
        return INVALID_SOCKET;
    }
    if (listen(l, 2) == SOCKET_ERROR) {
        fprintf(stderr, "hvcam: vsock listen failed\n");
        closesocket(l);
        return INVALID_SOCKET;
    }
    u_long one = 1;
    if (ioctlsocket(l, FIONBIO, &one) != 0) {
        fprintf(stderr, "hvcam: vsock nonblock failed\n");
        closesocket(l);
        return INVALID_SOCKET;
    }
    return l;
}

// The utility VM id changes on every WSL boot/idle shutdown — while
// idle, re-check it and swap the listener when it moves. Runs only
// between clients, so a served connection is never cut mid-stream.
void rebind_if_vmid_changed(VsockBind *b, uint32_t port)
{
    GUID cur{};
    if (!detect_wsl_vmid(&cur))
        return; // transient failure or WSL off: keep the current bind
    if (b->pinned && memcmp(&b->vm, &cur, sizeof(GUID)) == 0)
        return;
    SOCKET nl = listen_vsock(port, &cur);
    if (nl == INVALID_SOCKET)
        return; // a stale bind still beats no bind
    closesocket(b->sock);
    b->sock = nl;
    b->vm = cur;
    b->pinned = true;
    char gstr[48];
    guid_to_string(cur, gstr, sizeof(gstr));
    fprintf(stderr, "hvcam: re-pinned vsock bind to WSL vm %s\n", gstr);
}

int vsock_accept_loop(VsockBind *b, uint32_t port, FrameSlots *slots,
                      bool auto_pin)
{
    ULONGLONG last_detect = GetTickCount64();
    for (;;) {
        SOCKADDR_HV peer{};
        int plen = sizeof(peer);
        SOCKET c = accept(b->sock, (sockaddr *)&peer, &plen);
        if (c == INVALID_SOCKET) {
            Sleep(50);
            if (auto_pin && GetTickCount64() - last_detect > 4000) {
                last_detect = GetTickCount64();
                rebind_if_vmid_changed(b, port);
            }
            continue;
        }
        // Accepted sockets inherit the listener's non-blocking
        // mode on Windows — restore blocking before serving.
        u_long zero = 0;
        ioctlsocket(c, FIONBIO, &zero);
        // Access control: a pinned bind already restricts which VM
        // can reach us; the accept check keeps the reject visible.
        char gstr[48];
        guid_to_string(peer.VmId, gstr, sizeof(gstr));
        if (b->pinned && memcmp(&peer.VmId, &b->vm, sizeof(GUID)) != 0) {
            fprintf(stderr, "hvcam: rejected guest %s (not pinned vm)\n",
                    gstr);
            closesocket(c);
            continue;
        }
        fprintf(stderr, "hvcam: vsock client connected (vm=%s)\n", gstr);
        serve_client(c, slots);
        closesocket(c);
        fprintf(stderr, "hvcam: client left\n");
    }
}

int accept_loop(SOCKET listener, FrameSlots *slots, const char *kind,
                bool is_tcp, const GUID *allowed_vm)
{
    for (;;) {
        SOCKADDR_HV peer{};
        int plen = sizeof(peer);
        SOCKET c = accept(listener, (sockaddr *)&peer, &plen);
        if (c == INVALID_SOCKET) {
            Sleep(50);
            continue;
        }
        // Access control: webcam frames do not go to just any
        // partition that knows the port. With --vm-guid only that VM
        // passes; otherwise the CHILDREN bind already restricts to
        // this host's child partitions, and the VmId is logged for
        // audit.
        if (!is_tcp && plen >= (int)sizeof(SOCKADDR_HV)) {
            char gstr[48];
            guid_to_string(peer.VmId, gstr, sizeof(gstr));
            if (allowed_vm &&
                memcmp(&peer.VmId, allowed_vm, sizeof(GUID)) != 0) {
                fprintf(stderr, "hvcam: rejected guest %s (not --vm-guid)\n",
                        gstr);
                closesocket(c);
                continue;
            }
            fprintf(stderr, "hvcam: %s client connected (vm=%s)\n", kind, gstr);
        } else {
            fprintf(stderr, "hvcam: %s client connected\n", kind);
        }
        if (is_tcp) {
            BOOL one = TRUE;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY,
                       (const char *)&one, sizeof(one));
        }
        serve_client(c, slots);
        closesocket(c);
        fprintf(stderr, "hvcam: client left\n");
    }
}

} // namespace

int run_server_vsock(FrameSlots *slots, uint32_t port,
                     const GUID *vm_guid, bool auto_pin)
{
    VsockBind b;
    if (vm_guid) {
        b.sock = listen_vsock(port, vm_guid);
        b.vm = *vm_guid;
        b.pinned = true;
    } else if (auto_pin) {
        GUID wsl{};
        if (detect_wsl_vmid(&wsl)) {
            b.sock = listen_vsock(port, &wsl);
            b.vm = wsl;
            b.pinned = true;
        }
    }
    if (!b.pinned)
        b.sock = listen_vsock(port, nullptr);
    if (b.sock == INVALID_SOCKET)
        return 1;
    if (b.pinned) {
        char gstr[48];
        guid_to_string(b.vm, gstr, sizeof(gstr));
        fprintf(stderr, "hvcam: listening vsock port %u, pinned to vm %s%s\n",
                port, gstr, vm_guid ? " (--vm-guid)" : " (wsl auto)");
    } else {
        fprintf(stderr, "hvcam: listening vsock port %u (WSL: cid=2)\n", port);
        fprintf(stderr,
                "hvcam: warning — wildcard bind; non-WSL guests can pull "
                "frames (peer VmIds are logged at accept)\n");
    }
    // auto_pin re-detects only while no explicit --vm-guid is set.
    return vsock_accept_loop(&b, port, slots, auto_pin && !vm_guid);
}

int run_client_vsock_reverse(FrameSlots *slots, uint32_t port,
                             const GUID *vm_guid, bool auto_detect)
{
    GUID target{};
    bool have_target = false;
    bool waited = false;
    for (;;) {
        // Resolve the destination every cycle: the WSL utility VM
        // gets a fresh VmId on each boot, and in reverse mode a stale
        // id is a connection to nowhere.
        if (vm_guid) {
            target = *vm_guid;
            have_target = true;
        } else if (auto_detect) {
            have_target = detect_wsl_vmid(&target);
        }
        if (!have_target) {
            fprintf(stderr, "hvcam: reverse mode — no WSL VM yet; "
                    "retrying...\r");
            Sleep(1000);
            continue;
        }
        SOCKET c = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
        if (c == INVALID_SOCKET) {
            fprintf(stderr, "hvcam: AF_HYPERV socket failed (WSA %d)\n",
                    WSAGetLastError());
            Sleep(1000);
            continue;
        }
        SOCKADDR_HV a{};
        a.Family = AF_HYPERV;
        a.VmId = target;
        a.ServiceId = service_guid_for_port(port);
        if (connect(c, (const sockaddr *)&a, sizeof(a)) == SOCKET_ERROR) {
            // Guest not listening (or VM still booting): retry.
            // The hint prints once per disconnect streak, not per
            // attempt — a silent 1 Hz retry is indistinguishable
            // from a hang.
            if (!waited) {
                waited = true;
                fprintf(stderr, "hvcam: reverse mode — waiting for a "
                        "guest listener on port %u (hvcam-dump --listen "
                        "/ Camera(listen=True)) (WSA %d)\n",
                        (unsigned)port, WSAGetLastError());
            }
            closesocket(c);
            Sleep(1000);
            continue;
        }
        waited = false;
        char gstr[48];
        guid_to_string(target, gstr, sizeof(gstr));
        fprintf(stderr, "hvcam: connected to guest listener (vm=%s)\n",
                gstr);
        serve_client(c, slots);
        closesocket(c);
        fprintf(stderr, "hvcam: guest disconnected; reconnecting...\n");
    }
}

int run_server_tcp(FrameSlots *slots, const char *bind_ip, uint16_t port)
{
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) {
        fprintf(stderr, "hvcam: TCP socket failed\n");
        return 1;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_ip && *bind_ip ? bind_ip : "127.0.0.1",
                  &a.sin_addr) != 1) {
        fprintf(stderr, "hvcam: invalid bind ip: %s\n", bind_ip);
        closesocket(l);
        return 1;
    }
    if (bind(l, (const sockaddr *)&a, sizeof(a)) == SOCKET_ERROR) {
        fprintf(stderr, "hvcam: tcp bind %s:%u failed (WSA %d)\n",
                bind_ip, port, WSAGetLastError());
        closesocket(l);
        return 1;
    }
    if (listen(l, 2) == SOCKET_ERROR) {
        fprintf(stderr, "hvcam: tcp listen failed\n");
        closesocket(l);
        return 1;
    }
    fprintf(stderr, "hvcam: listening tcp %s:%u\n", bind_ip, port);
    return accept_loop(l, slots, "tcp", true, nullptr);
}
