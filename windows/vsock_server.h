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

#include <guiddef.h>
#include <stdint.h>

class FrameSlots;

// Runs the accept loop forever (until process signal).
// vsock port is the hvsocket service port; tcp_ip empty = vsock mode.
// vm_guid != nullptr pins bind+accept to that one partition.
// auto_pin (only meaningful when vm_guid is null) makes the loop
// re-detect the WSL utility VM's id every few seconds and rebind —
// the id changes on every WSL boot.
int run_server_vsock(FrameSlots *slots, uint32_t port,
                     const GUID *vm_guid, bool auto_pin);

// Reverse direction: the host connects OUT to a vsock listener in
// the guest (libhvcam's hvcam_listen / `hvcam-dump --listen`).
// Host->guest hvsock needs no GuestCommunicationServices key — the
// zero-admin path. Loops forever: re-resolves the WSL VmId between
// attempts, so it self-heals across guest restarts and WSL reboots.
// vm_guid pins the target explicitly; auto_detect re-discovers WSL.
int run_client_vsock_reverse(FrameSlots *slots, uint32_t port,
                             const GUID *vm_guid, bool auto_detect);

int run_server_tcp(FrameSlots *slots, const char *bind_ip, uint16_t port);
