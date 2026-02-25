#include "virtual_port_device.h"

// IPC transport: Unix SOCK_DGRAM — see full design in virtual_port_device.h.
//
// TODO (task 2a): PeerEntry table — static array[15] of {node_id, send_fd,
//                 active, sock_path[108]} plus own recv_fd and a pthread_mutex.
// TODO (task 2b): num_ports() — return VIRTUAL_PORT_MAX_PEERS (15).
// TODO (task 2c): enable() — bind recv socket at VIRTUAL_PORT_SOCK_FMT path;
//                 open send sockets for each peer; call link_change(idx, true).
//                 disable() — reverse: close fds, unlink socket, link_change false.
// TODO (task 2d): enable_port() / disable_port() — per-peer connect/disconnect.
// TODO (task 2e): send(port=0) — flood all active peers with [port_byte|frame];
//                 send(port=N) — unicast to peer at slot N-1.
// TODO (task 2f): RX thread — single pthread recvfrom() loop; strip port byte;
//                 call callbacks->receive(port_num, frame, len).
// TODO (task 2g): 15-neighbor cap — reject with log + counter if table full.
// TODO (task 2h): retry_negotiation() — reconnect if peer socket path exists.
// TODO (task 2i): port_stats() / handle_interrupt() — safe no-op stubs.
// TODO (task 2j): virtual_port_device_get(node_id, peer_ids, num_peers)
//                 → NetworkDevice with trait + callbacks wired up.

int virtual_port_device_init(void) {
  // Placeholder – will be implemented in task 2a–2j.
  return 0;
}

