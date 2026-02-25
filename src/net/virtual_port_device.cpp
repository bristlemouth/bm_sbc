#include "virtual_port_device.h"

#include <pthread.h>   // pthread_mutex_t, pthread_t
#include <stdbool.h>   // bool
#include <string.h>    // memset, strncpy

// IPC transport: Unix SOCK_DGRAM — see full design in virtual_port_device.h.

// -------------------------------------------------------------------------
// Task 2a: Peer table data structure
// -------------------------------------------------------------------------

/// One slot in the peer table.  Slots are indexed 0–14; port numbers are
/// slot_index + 1 (i.e. port 1 == peers[0], port 15 == peers[14]).
typedef struct {
  /// Peer's 64-bit Bristlemouth node ID (0 when slot is inactive).
  uint64_t node_id;

  /// Unbound SOCK_DGRAM fd used to sendto() the peer's receive socket.
  /// -1 when the socket has not been opened yet (or has been closed).
  int send_fd;

  /// True when this slot contains a valid, configured peer.
  bool active;

  /// Absolute path of the peer's receive socket (built from VIRTUAL_PORT_SOCK_FMT).
  char sock_path[VIRTUAL_PORT_SOCK_PATH_LEN];
} PeerEntry;

/// All mutable state for one VirtualPortDevice instance.
/// Stored as a module-level singleton because the NetworkDeviceTrait
/// functions receive only a void *self pointer (or no pointer for num_ports).
typedef struct {
  // ----- peer table -----
  /// Table of up to VIRTUAL_PORT_MAX_PEERS directly-connected peers.
  /// Indexed by (port_num - 1); port_num 1 → peers[0], …, port_num 15 → peers[14].
  PeerEntry peers[VIRTUAL_PORT_MAX_PEERS];

  // ----- own receive socket -----
  /// Bound SOCK_DGRAM fd on which all incoming datagrams arrive.
  /// -1 when the device has not been enabled yet.
  int recv_fd;

  /// Absolute path of this process's own receive socket
  /// (built from own_node_id using VIRTUAL_PORT_SOCK_FMT).
  char own_sock_path[VIRTUAL_PORT_SOCK_PATH_LEN];

  // ----- RX thread (task 2f) -----
  /// Handle of the background recvfrom() thread.  Valid only while enabled.
  pthread_t rx_thread;

  /// Set to false to signal the RX thread to exit; read under lock.
  bool rx_running;

  // ----- peer-table lock -----
  /// Protects peers[], recv_fd, own_sock_path, rx_running, and enabled.
  /// All trait functions that touch shared state must hold this mutex.
  pthread_mutex_t lock;

  // ----- device identity -----
  /// This process's 64-bit Bristlemouth node ID (copied from VirtualPortCfg).
  uint64_t own_node_id;

  /// Directory used for socket files (copied from VirtualPortCfg).
  char socket_dir[VIRTUAL_PORT_SOCK_DIR_MAX + 1];

  // ----- device state -----
  /// True after enable() succeeds; false after disable() or before enable().
  bool enabled;

  // ----- bm_core callbacks -----
  /// Populated by bm_l2_init() after the NetworkDevice is registered.
  /// Must not be invoked before enable() is called.
  NetworkDeviceCallbacks callbacks;
} VirtualPortState;

/// Module-level singleton.  Zero-initialized by the C runtime; recv_fd and
/// all peer send_fds are set to -1 by virtual_port_device_get() (task 2j)
/// before the NetworkDevice is handed to bm_l2_init().
static VirtualPortState g_vport_state;

// -------------------------------------------------------------------------
// TODO (task 2b): num_ports() — return VIRTUAL_PORT_MAX_PEERS (15).
// TODO (task 2c): enable() — bind recv socket at own_sock_path;
//                 open send sockets for each active peer;
//                 call callbacks.link_change(idx, true) for each.
//                 disable() — reverse: close fds, unlink socket,
//                 call link_change(idx, false) for each.
// TODO (task 2d): enable_port() / disable_port() — per-peer
//                 socket open/close and link_change notification.
// TODO (task 2e): send(port=0) — flood all active peers with
//                 [port_byte | frame]; send(port=N) — unicast to peers[N-1].
// TODO (task 2f): RX thread — single pthread recvfrom() loop; strip the
//                 port byte; call callbacks.receive(port_num, frame, len).
// TODO (task 2g): 15-neighbor cap — reject with log + counter if table full.
// TODO (task 2h): retry_negotiation() — reconnect if peer socket path exists.
// TODO (task 2i): port_stats() / handle_interrupt() — safe no-op stubs.
// TODO (task 2j): virtual_port_device_get(cfg) — copy cfg into g_vport_state,
//                 set recv_fd and all send_fds to -1, init the mutex,
//                 wire up trait + callbacks pointer, return NetworkDevice.
// -------------------------------------------------------------------------

NetworkDevice virtual_port_device_get(const VirtualPortCfg *cfg) {
  // Placeholder – will be implemented in tasks 2b–2j.
  (void)cfg;
  NetworkDevice dev = {0};
  return dev;
}

