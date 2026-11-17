// hvcam-host — captures a UVC camera on Windows and serves frames to
// a WSL2 guest over Hyper-V sockets (or TCP fallback).
//
//   hvcam-host                          # camera 0, vsock port 8765
//   hvcam-host --list                   # enumerate UVC devices
//   hvcam-host --device 1 --width 1280 --height 720 --fps 30
//   hvcam-host --lock-exposure --lock-focus
//   hvcam-host --tcp 127.0.0.1:8765     # TCP fallback (no vsock)
// winsock2 must precede every header that pulls windows.h (objbase.h
// included), or the legacy winsock.h is dragged in and collides.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <winreg.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <string>

#include "frame_slots.h"
#include "mf_capture.h"
#include "vsock_server.h"
#include "protocol.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib") // CLSIDFromString (--vm-guid)
#pragma comment(lib, "advapi32.lib") // registry: --register-vsock

namespace {

// The Hyper-V kernel silently drops a guest's SYN when the service
// GUID is not registered under GuestCommunicationServices — this is
// why guest->host vsock "binds fine but never connects". Registering
// the {port}-facb-11e6-bd58-64006a7986d3 key once (as admin) opens
// the channel permanently.
int vsock_registry(uint32_t port, bool remove)
{
    char key[256];
    snprintf(key, sizeof(key),
             "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\"
             "Virtualization\\GuestCommunicationServices\\"
             "{%08x-facb-11e6-bd58-64006a7986d3}",
             (unsigned)port);
    if (remove) {
        LONG rc = RegDeleteTreeA(HKEY_LOCAL_MACHINE, key);
        if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
            fprintf(stderr,
                    "hvcam-host: unregister failed (%ld) — needs admin\n", rc);
            return 1;
        }
        fprintf(stderr, "hvcam: vsock service unregistered (port %u)\n", port);
        return 0;
    }
    HKEY hk = nullptr;
    DWORD disp = 0;
    LONG rc = RegCreateKeyExA(HKEY_LOCAL_MACHINE, key, 0, nullptr,
                              REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                              &hk, &disp);
    if (rc == ERROR_SUCCESS)
        rc = RegSetValueExA(hk, "ElementName", 0, REG_SZ,
                            (const BYTE *)"hvcam", sizeof("hvcam"));
    if (hk) RegCloseKey(hk);
    if (rc != ERROR_SUCCESS) {
        fprintf(stderr,
                "hvcam-host: register failed (%ld) — run this once as "
                "Administrator: hvcam-host --register-vsock --port %u\n",
                rc, (unsigned)port);
        return 1;
    }
    fprintf(stderr,
            "hvcam: vsock service registered (port %u, %s)\n",
            (unsigned)port, disp == REG_CREATED_NEW_KEY ? "created" : "existed");
    return 0;
}

} // namespace

namespace {

// strtol wrapper: rejects garbage atoi would silently accept
// (empty, trailing junk, overflow, out-of-range).
long parse_long(const char *s, const char *name, long lo, long hi)
{
    if (!s || !*s) {
        fprintf(stderr, "hvcam-host: empty value for %s\n", name);
        exit(2);
    }
    char *end = nullptr;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno == ERANGE || end == s || *end != '\0' || v < lo || v > hi) {
        fprintf(stderr, "hvcam-host: invalid %s '%s' (need %ld..%ld)\n",
                name, s, lo, hi);
        exit(2);
    }
    return v;
}

// Vsock service ports are 32-bit (encoded as the first field of the
// hvsocket service GUID), unlike TCP's 16-bit — accept the full
// unsigned range here and bound it at the TCP call site instead.
// strtoull because Windows long is 32-bit and could not hold the cap.
uint32_t parse_port(const char *s, const char *name)
{
    if (!s || !*s || *s == '-') {
        fprintf(stderr, "hvcam-host: invalid %s '%s' (need 1..4294967295)\n",
                name, s ? s : "");
        exit(2);
    }
    char *end = nullptr;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno == ERANGE || end == s || *end != '\0' ||
        v < 1 || v > 0xFFFFFFFFull) {
        fprintf(stderr, "hvcam-host: invalid %s '%s' (need 1..4294967295)\n",
                name, s);
        exit(2);
    }
    return (uint32_t)v;
}

void usage()
{
    fprintf(stderr,
        "usage: hvcam-host [options]\n"
        "  --list                enumerate UVC cameras and exit\n"
        "  --device N            camera index (default 0)\n"
        "  --port N              vsock/tcp port (default 8765)\n"
        "  --width N --height N  requested resolution (default: camera native)\n"
        "  --fps N               requested frame rate\n"
        "  --tcp IP[:PORT]       TCP mode bound to IP (default: vsock)\n"
        "  --vm-guid GUID        accept vsock only from that VM\n"
        "  --register-vsock      one-time admin: register the vsock\n"
        "                        service GUID so guests can connect\n"
        "  --unregister-vsock    remove that registration\n"
        "  --lock-exposure       pin current exposure (manual)\n"
        "  --lock-focus          pin current focus (manual)\n");
}

} // namespace

int main(int argc, char **argv)
{
    MfOpenOptions opts;
    uint32_t port = 8765;
    std::string tcp_ip;
    GUID allowed_vm{};
    bool have_vm_guid = false;
    bool do_list = false;
    bool do_register = false, do_unregister = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                fprintf(stderr, "hvcam-host: %s requires a value\n", name);
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--list") do_list = true;
        else if (a == "--device")
            opts.device_index = (int)parse_long(need("--device"), "--device", 0, 64);
        else if (a == "--port")
            port = parse_port(need("--port"), "--port");
        else if (a == "--width")
            opts.width = (int)parse_long(need("--width"), "--width", 0, 16384);
        else if (a == "--height")
            opts.height = (int)parse_long(need("--height"), "--height", 0, 16384);
        else if (a == "--fps")
            opts.fps = (int)parse_long(need("--fps"), "--fps", 0, 480);
        else if (a == "--tcp") {
            // Accepts "IP" or "IP:port" — the suffix becomes `port`
            // and only the bare address reaches inet_pton (IPv4).
            tcp_ip = need("--tcp");
            size_t colon = tcp_ip.rfind(':');
            if (colon != std::string::npos) {
                port = (uint32_t)parse_long(tcp_ip.c_str() + colon + 1,
                                            "--tcp port", 1, 65535);
                tcp_ip.resize(colon);
            }
            if (tcp_ip.empty()) {
                fprintf(stderr, "hvcam-host: --tcp requires an address\n");
                return 2;
            }
        }
        else if (a == "--vm-guid") {
            std::string g = need("--vm-guid");
            // The accept log prints VmId without braces; CLSIDFromString
            // only takes {...} or ProgID — wrap before parsing.
            if (!g.empty() && g.front() != '{')
                g = "{" + g + "}";
            std::wstring wg(g.begin(), g.end());
            if (FAILED(CLSIDFromString(wg.c_str(), &allowed_vm))) {
                fprintf(stderr, "hvcam-host: invalid --vm-guid\n");
                return 2;
            }
            have_vm_guid = true;
        }
        else if (a == "--lock-exposure") opts.lock_exposure = true;
        else if (a == "--lock-focus") opts.lock_focus = true;
        else if (a == "--register-vsock") do_register = true;
        else if (a == "--unregister-vsock") do_unregister = true;
        else {
            usage();
            return 2;
        }
    }

    if (!tcp_ip.empty() && port > 65535) {
        fprintf(stderr, "hvcam-host: TCP port must be 1..65535\n");
        return 2;
    }

    if (do_register || do_unregister)
        return vsock_registry(port, do_unregister);

    if (do_list) {
        auto devs = MfCapture::list_devices();
        if (devs.empty())
            fprintf(stderr, "no UVC camera found\n");
        for (auto &d : devs)
            fprintf(stderr, "[%d] %ls\n", d.index, d.name.c_str());
        return devs.empty() ? 1 : 0;
    }

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "hvcam-host: WSAStartup failed\n");
        return 1;
    }

    try {
        FrameSlots slots;
        MfCapture cam;
        cam.open(opts);
        cam.start(&slots);
        fprintf(stderr, "hvcam-host: capture active %dx%d fmt=%u\n",
                cam.width(), cam.height(), cam.format());
        int rc = tcp_ip.empty()
                     ? run_server_vsock(&slots, port,
                                        have_vm_guid ? &allowed_vm : nullptr)
                     : run_server_tcp(&slots, tcp_ip.c_str(), (uint16_t)port);
        cam.close();
        WSACleanup();
        return rc;
    } catch (const std::exception &e) {
        fprintf(stderr, "hvcam-host: %s\n", e.what());
        WSACleanup();
        return 1;
    }
}
