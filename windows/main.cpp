// hvcam-host — captures a UVC camera on Windows and serves frames to
// a WSL2 guest over Hyper-V sockets (or TCP fallback).
//
//   hvcam-host                          # camera 0, vsock port 8765
//   hvcam-host --list                   # enumerate UVC devices
//   hvcam-host --device 1 --width 1280 --height 720 --fps 30
//   hvcam-host --lock-exposure --lock-focus
//   hvcam-host --tcp 127.0.0.1:8765     # TCP fallback (no vsock)
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <objbase.h>
#include <stdexcept>
#include <string>
#include <winsock2.h>
#include <windows.h>

#include "frame_slots.h"
#include "mf_capture.h"
#include "vsock_server.h"
#include "protocol.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib") // CLSIDFromString (--vm-guid)

namespace {

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
        else if (a == "--device") opts.device_index = atoi(need("--device"));
        else if (a == "--port") port = (uint32_t)atoi(need("--port"));
        else if (a == "--width") opts.width = atoi(need("--width"));
        else if (a == "--height") opts.height = atoi(need("--height"));
        else if (a == "--fps") opts.fps = atoi(need("--fps"));
        else if (a == "--tcp") {
            // Accepts "IP" or "IP:port" — the suffix becomes `port`
            // and only the bare address reaches inet_pton.
            tcp_ip = need("--tcp");
            size_t colon = tcp_ip.rfind(':');
            if (colon != std::string::npos) {
                port = (uint32_t)atoi(tcp_ip.c_str() + colon + 1);
                tcp_ip.resize(colon);
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
        else {
            usage();
            return 2;
        }
    }

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
