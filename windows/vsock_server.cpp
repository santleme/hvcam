// vsock_server.cpp — AF_HYPERV listener + shared client loop.
//
// WSL2 vsock interop: a Linux guest connecting to VMADDR_CID_HOST
// (cid 2) on port P reaches the Windows socket bound to
// AF_HYPERV/SOCK_STREAM/HV_PROTOCOL_RAW with ServiceId =
// {P as DWORD}-facb-11e6-bd58-64006a7986d3 and VmId = HV_GUID_WILDCARD
// (accepts any child partition). This is the documented WSL2
// hvsocket service template.
#include "vsock_server.h"

#include <initguid.h>
#include <windows.h>
#include <winsock2.h>
#include <hvsocket.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>

#include "frame_slots.h"
#include "protocol.h"

#pragma comment(lib, "ws2_32.lib")

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

int accept_loop(SOCKET listener, FrameSlots *slots, const char *kind,
                bool is_tcp)
{
    for (;;) {
        SOCKET c = accept(listener, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            Sleep(50);
            continue;
        }
        fprintf(stderr, "hvcam: cliente %s conectou\n", kind);
        if (is_tcp) {
            BOOL one = TRUE;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY,
                       (const char *)&one, sizeof(one));
        }
        serve_client(c, slots);
        closesocket(c);
        fprintf(stderr, "hvcam: cliente saiu\n");
    }
    return 0;
}

} // namespace

int run_server_vsock(FrameSlots *slots, uint32_t port)
{
    SOCKET l = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (l == INVALID_SOCKET) {
        fprintf(stderr, "hvcam: socket AF_HYPERV falhou (WSA %d)\n",
                WSAGetLastError());
        return 1;
    }
    SOCKADDR_HV a{};
    a.Family = AF_HYPERV;
    a.VmId = HV_GUID_WILDCARD; // qualquer particao filha (o WSL2)
    a.ServiceId = service_guid_for_port(port);
    if (bind(l, (const sockaddr *)&a, sizeof(a)) == SOCKET_ERROR) {
        fprintf(stderr, "hvcam: bind vsock porta %u falhou (WSA %d)\n",
                port, WSAGetLastError());
        closesocket(l);
        return 1;
    }
    if (listen(l, 2) == SOCKET_ERROR) {
        fprintf(stderr, "hvcam: listen vsock falhou\n");
        closesocket(l);
        return 1;
    }
    fprintf(stderr, "hvcam: escutando vsock porta %u (WSL: cid=2)\n", port);
    return accept_loop(l, slots, "vsock", false);
}

int run_server_tcp(FrameSlots *slots, const char *bind_ip, uint16_t port)
{
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) {
        fprintf(stderr, "hvcam: socket TCP falhou\n");
        return 1;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_ip && *bind_ip ? bind_ip : "127.0.0.1",
                  &a.sin_addr) != 1) {
        fprintf(stderr, "hvcam: ip de bind invalido: %s\n", bind_ip);
        closesocket(l);
        return 1;
    }
    if (bind(l, (const sockaddr *)&a, sizeof(a)) == SOCKET_ERROR) {
        fprintf(stderr, "hvcam: bind tcp %s:%u falhou (WSA %d)\n",
                bind_ip, port, WSAGetLastError());
        closesocket(l);
        return 1;
    }
    if (listen(l, 2) == SOCKET_ERROR) {
        fprintf(stderr, "hvcam: listen tcp falhou\n");
        closesocket(l);
        return 1;
    }
    fprintf(stderr, "hvcam: escutando tcp %s:%u\n", bind_ip, port);
    return accept_loop(l, slots, "tcp", true);
}
