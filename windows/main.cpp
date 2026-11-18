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

// Read-only probe, no admin needed: is the service GUID for this
// port registered? Without it the Hyper-V kernel drops guest->host
// SYNs silently and the client just times out — catch that at
// startup instead of after a confusing wait.
bool vsock_service_registered(uint32_t port)
{
    char key[256];
    snprintf(key, sizeof(key),
             "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\"
             "Virtualization\\GuestCommunicationServices\\"
             "{%08x-facb-11e6-bd58-64006a7986d3}",
             (unsigned)port);
    HKEY hk = nullptr;
    LONG rc = RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &hk);
    if (rc == ERROR_SUCCESS)
        RegCloseKey(hk);
    return rc == ERROR_SUCCESS;
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
        "  --format FMT          force pixel format: yuy2|nv12|mjpeg|\n"
        "                        bgr24|bgrx32 (default: highest delivered\n"
        "                        fps at the requested size)\n"
        "  --tcp IP[:PORT]       TCP mode bound to IP (default: vsock)\n"
        "  --reverse             connect out to the guest's vsock listener\n"
        "                        instead of accepting guest->host (no\n"
        "                        --register-vsock / admin needed)\n"
        "  --vm-guid GUID        accept vsock only from that VM\n"
        "  --no-auto-pin         keep the wildcard vsock bind; do not\n"
        "                        auto-pin to the running WSL VM\n"
        "  --register-vsock      one-time admin: register the vsock\n"
        "                        service GUID so guests can connect\n"
        "  --unregister-vsock    remove that registration\n"
        "  --exposure N          manual exposure, log2 seconds (e.g.\n"
        "                        -6 = 1/64s — under 1/30s keeps the real\n"
        "                        30 fps; auto-exposure caps ~20 indoors)\n"
        "  --gain N              manual gain, device units\n"
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
    bool no_auto_pin = false;
    bool do_reverse = false;
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
        else if (a == "--format") {
            std::string f = need("--format");
            if      (f == "yuy2")   opts.prefer_format = HVCAM_FMT_YUY2;
            else if (f == "nv12")   opts.prefer_format = HVCAM_FMT_NV12;
            else if (f == "mjpeg")  opts.prefer_format = HVCAM_FMT_MJPEG;
            else if (f == "bgr24")  opts.prefer_format = HVCAM_FMT_BGR24;
            else if (f == "bgrx32") opts.prefer_format = HVCAM_FMT_BGRX32;
            else {
                fprintf(stderr, "hvcam-host: invalid --format '%s' "
                        "(yuy2|nv12|mjpeg|bgr24|bgrx32)\n", f.c_str());
                return 2;
            }
        }
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
        else if (a == "--exposure")
            opts.exposure = (int)parse_long(need("--exposure"),
                                          "--exposure", -32, 31);
        else if (a == "--gain")
            opts.gain = (int)parse_long(need("--gain"), "--gain", 0, 100000);
        else if (a == "--lock-exposure") opts.lock_exposure = true;
        else if (a == "--lock-focus") opts.lock_focus = true;
        else if (a == "--no-auto-pin") no_auto_pin = true;
        else if (a == "--reverse") do_reverse = true;
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
    if (do_reverse && !tcp_ip.empty()) {
        fprintf(stderr, "hvcam-host: --reverse is vsock-only\n");
        return 2;
    }
    if (do_reverse && no_auto_pin && !have_vm_guid) {
        fprintf(stderr, "hvcam-host: --reverse needs a target VM — "
                "--no-auto-pin requires --vm-guid\n");
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

    // vsock: with no explicit --vm-guid and no --no-auto-pin, the
    // server auto-pins the bind to the running WSL VM and re-pins
    // when its id changes (WSL boots get a fresh VmId each time).
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
        int rc;
        if (!tcp_ip.empty()) {
            rc = run_server_tcp(&slots, tcp_ip.c_str(), (uint16_t)port);
        } else if (do_reverse) {
            rc = run_client_vsock_reverse(
                     &slots, port,
                     have_vm_guid ? &allowed_vm : nullptr,
                     !no_auto_pin);
        } else {
            if (!vsock_service_registered(port))
                fprintf(stderr,
                        "hvcam: warning — vsock service GUID not registered "
                        "for port %u; guest->host connects will time out.\n"
                        "  Run once as admin: hvcam-host --register-vsock "
                        "--port %u — or use --tcp / --reverse (no admin).\n",
                        (unsigned)port, (unsigned)port);
            rc = run_server_vsock(&slots, port,
                                  have_vm_guid ? &allowed_vm : nullptr,
                                  !have_vm_guid && !no_auto_pin);
        }
        cam.close();
        WSACleanup();
        return rc;
    } catch (const std::exception &e) {
        fprintf(stderr, "hvcam-host: %s\n", e.what());
        WSACleanup();
        return 1;
    }
}
