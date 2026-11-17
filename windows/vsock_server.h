// vsock_server — serves FrameSlots to one client at a time.
//
// Primary transport: AF_HYPERV (Hyper-V sockets / VMBus). The host
// listens on a service GUID derived from the TCP-style port number;
// the WSL2 guest connects to VMADDR_CID_HOST:port and gets routed
// here with zero IP config and zero firewall involvement.
//
// Fallback: --tcp <bind-ip> listens on plain TCP (development,
// mirrored-networking WSL, non-WSL guests).
#pragma once

#include <stdint.h>

class FrameSlots;

// Runs the accept loop forever (until process signal).
// vsock port is the hvsocket service port; tcp_ip empty = vsock mode.
int run_server_vsock(FrameSlots *slots, uint32_t port);
int run_server_tcp(FrameSlots *slots, const char *bind_ip, uint16_t port);
