#include "virtual_port_device.h"
#include "bm_config.h"       // bm_debug()
#include <errno.h>           // errno
#include <pthread.h>         // pthread_mutex_t, pthread_t, pthread_create, pthread_join
#include <stdio.h>           // snprintf
#include <string.h>          // memset, strncpy, memcpy
#include <sys/socket.h>      // socket, sendto, recvfrom, bind, AF_UNIX, SOCK_DGRAM, setsockopt
#include <sys/time.h>        // struct timeval (SO_RCVTIMEO)
#include <sys/un.h>          // struct sockaddr_un
#include <unistd.h>          // close, unlink, access

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
// Task 2b: num_ports()
// -------------------------------------------------------------------------

/// Returns the fixed maximum number of virtual ports (one per peer slot).
/// Matches VIRTUAL_PORT_MAX_PEERS so bm_l2 and topology_init() know the
/// port count before the device is enabled.
static uint8_t vpd_num_ports(void) {
  return (uint8_t)VIRTUAL_PORT_MAX_PEERS;
}

// -------------------------------------------------------------------------
// Task 2f: Helper + RX thread
// -------------------------------------------------------------------------

/// Fill a sockaddr_un from a socket path string.
static void vpd_fill_peer_addr(struct sockaddr_un *a, const char *path) {
  memset(a, 0, sizeof(*a));
  a->sun_family = AF_UNIX;
  strncpy(a->sun_path, path, sizeof(a->sun_path) - 1);
}

/// Background thread: recvfrom() loop with 1-second SO_RCVTIMEO timeout.
/// Reads datagrams from recv_fd, extracts the ingress-port byte, and
/// dispatches the frame payload to callbacks.receive().
static void *vpd_rx_thread(void *arg) {
  VirtualPortState *s = (VirtualPortState *)arg;
  uint8_t buf[VIRTUAL_PORT_MAX_DGRAM_LEN];
  while (1) {
    pthread_mutex_lock(&s->lock);
    bool running = s->rx_running;
    int  fd      = s->recv_fd;
    pthread_mutex_unlock(&s->lock);
    if (!running || fd < 0) { break; }
    ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
    if (n < 0) {
      // EAGAIN/EWOULDBLOCK = SO_RCVTIMEO fired — check rx_running and loop.
      if (errno == EAGAIN || errno == EWOULDBLOCK) { continue; }
      break; // EBADF or other fatal error — exit thread
    }
    if ((size_t)n < VIRTUAL_PORT_MIN_DGRAM_LEN) { continue; }
    uint8_t port_num = VIRTUAL_PORT_DGRAM_PORT(buf);
    if (port_num < 1 || port_num > VIRTUAL_PORT_MAX_PEERS) { continue; }
    uint8_t *frame     = VIRTUAL_PORT_DGRAM_FRAME_PTR(buf);
    size_t   frame_len = VIRTUAL_PORT_FRAME_LEN((size_t)n);
    // Snapshot callback pointer under lock; invoke outside lock.
    pthread_mutex_lock(&s->lock);
    void (*rcv)(uint8_t, uint8_t *, size_t) = s->callbacks.receive;
    pthread_mutex_unlock(&s->lock);
    if (rcv) { rcv(port_num, frame, frame_len); }
  }
  return NULL;
}

// -------------------------------------------------------------------------
// Task 2c: enable() / disable()
// -------------------------------------------------------------------------

/// Bind the receive socket, open send sockets for each peer, start the RX
/// thread, and fire link_change(idx, true) for every configured peer.
static BmErr vpd_enable(void *self) {
  VirtualPortState *s = (VirtualPortState *)self;
  pthread_mutex_lock(&s->lock);
  if (s->enabled) { pthread_mutex_unlock(&s->lock); return BmOK; }

  // Create the receive socket.
  int rfd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (rfd < 0) {
    pthread_mutex_unlock(&s->lock);
    bm_debug("vpd_enable: socket() failed errno=%d\n", errno);
    return BmEIO;
  }
  // 1-second receive timeout so the RX thread periodically wakes and checks
  // rx_running instead of blocking forever on recvfrom().
  struct timeval tv = {1, 0};
  setsockopt(rfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // Remove any stale socket from a previous run, then bind.
  unlink(s->own_sock_path);
  struct sockaddr_un addr;
  vpd_fill_peer_addr(&addr, s->own_sock_path);
  if (bind(rfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(rfd);
    pthread_mutex_unlock(&s->lock);
    bm_debug("vpd_enable: bind(%s) failed errno=%d\n", s->own_sock_path, errno);
    return BmEIO;
  }
  s->recv_fd    = rfd;
  s->rx_running = true;

  // Open unbound send sockets for configured peers (non-fatal if the peer
  // socket does not exist yet; retry_negotiation() handles reconnection).
  for (int i = 0; i < VIRTUAL_PORT_MAX_PEERS; i++) {
    if (!s->peers[i].active || s->peers[i].send_fd >= 0) { continue; }
    int sfd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sfd >= 0) { s->peers[i].send_fd = sfd; }
  }

  // Start the RX thread.
  if (pthread_create(&s->rx_thread, NULL, vpd_rx_thread, s) != 0) {
    s->rx_running = false;
    close(rfd); s->recv_fd = -1;
    unlink(s->own_sock_path);
    pthread_mutex_unlock(&s->lock);
    bm_debug("vpd_enable: pthread_create() failed\n");
    return BmEIO;
  }
  s->enabled = true;
  pthread_mutex_unlock(&s->lock);

  // Do NOT call link_change here.  The L2 thread starts its renegotiation
  // timers concurrently with this call, so firing link_change now would race
  // with bm_l2_start_renegotiate_check (between ll_item_add and bm_timer_start).
  // vpd_retry_negotiation() detects each peer and calls link_change once the
  // 100 ms renegotiation timer fires — by which point the L2 thread is stable.
  return BmOK;
}

/// Stop the RX thread, close all sockets, unlink the receive socket file, and
/// fire link_change(idx, false) for every previously-active peer.
static BmErr vpd_disable(void *self) {
  VirtualPortState *s = (VirtualPortState *)self;
  pthread_mutex_lock(&s->lock);
  if (!s->enabled) { pthread_mutex_unlock(&s->lock); return BmOK; }
  s->enabled    = false;
  s->rx_running = false;
  int rfd       = s->recv_fd;
  s->recv_fd    = -1;
  void (*lc)(uint8_t, bool) = s->callbacks.link_change;
  pthread_mutex_unlock(&s->lock);

  // Close recv_fd; the 1-second SO_RCVTIMEO guarantees the RX thread exits
  // within ≤1 second even if close() doesn't interrupt recvfrom().
  if (rfd >= 0) { close(rfd); }
  unlink(s->own_sock_path);
  pthread_join(s->rx_thread, NULL);

  // Close all peer send sockets.
  pthread_mutex_lock(&s->lock);
  for (int i = 0; i < VIRTUAL_PORT_MAX_PEERS; i++) {
    if (s->peers[i].send_fd >= 0) {
      close(s->peers[i].send_fd);
      s->peers[i].send_fd = -1;
    }
  }
  pthread_mutex_unlock(&s->lock);

  // Notify L2 that all ports are down.
  if (lc) {
    for (int i = 0; i < VIRTUAL_PORT_MAX_PEERS; i++) {
      if (s->peers[i].active) { lc((uint8_t)i, false); }
    }
  }
  return BmOK;
}

// -------------------------------------------------------------------------
// Task 2d: enable_port() / disable_port()
// -------------------------------------------------------------------------

/// Open the send socket for one peer (port 1–15) and notify L2 it is up.
static BmErr vpd_enable_port(void *self, uint8_t port_num) {
  VirtualPortState *s = (VirtualPortState *)self;
  if (port_num < 1 || port_num > VIRTUAL_PORT_MAX_PEERS) { return BmEINVAL; }
  int idx = port_num - 1;
  pthread_mutex_lock(&s->lock);
  if (!s->peers[idx].active) { pthread_mutex_unlock(&s->lock); return BmEINVAL; }
  if (s->peers[idx].send_fd < 0) {
    int sfd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sfd >= 0) { s->peers[idx].send_fd = sfd; }
  }
  void (*lc)(uint8_t, bool) = s->callbacks.link_change;
  pthread_mutex_unlock(&s->lock);
  if (lc) { lc((uint8_t)idx, true); }
  return BmOK;
}

/// Close the send socket for one peer and notify L2 it is down.
static BmErr vpd_disable_port(void *self, uint8_t port_num) {
  VirtualPortState *s = (VirtualPortState *)self;
  if (port_num < 1 || port_num > VIRTUAL_PORT_MAX_PEERS) { return BmEINVAL; }
  int idx = port_num - 1;
  pthread_mutex_lock(&s->lock);
  if (!s->peers[idx].active) { pthread_mutex_unlock(&s->lock); return BmEINVAL; }
  if (s->peers[idx].send_fd >= 0) {
    close(s->peers[idx].send_fd);
    s->peers[idx].send_fd = -1;
  }
  void (*lc)(uint8_t, bool) = s->callbacks.link_change;
  pthread_mutex_unlock(&s->lock);
  if (lc) { lc((uint8_t)idx, false); }
  return BmOK;
}

// -------------------------------------------------------------------------
// Task 2e: send()
// -------------------------------------------------------------------------

/// Send a raw L2 frame on one port (1–15) or flood all active peers (port 0).
/// Wire format: [1-byte egress-port-num | frame bytes].
static BmErr vpd_send(void *self, uint8_t *data, size_t length, uint8_t port) {
  VirtualPortState *s = (VirtualPortState *)self;
  if (!data || length == 0 || length > VIRTUAL_PORT_MAX_FRAME_LEN) { return BmEINVAL; }
  if (port > VIRTUAL_PORT_MAX_PEERS) { return BmEINVAL; }

  uint8_t dgram[VIRTUAL_PORT_MAX_DGRAM_LEN];
  BmErr   err = BmOK;

  if (port == 0) {
    // Flood: deliver to every active peer, tagging each datagram with the
    // sender's egress port number so the receiver knows the ingress port.
    for (int i = 0; i < VIRTUAL_PORT_MAX_PEERS; i++) {
      pthread_mutex_lock(&s->lock);
      bool active = s->peers[i].active;
      int  sfd    = s->peers[i].send_fd;
      struct sockaddr_un dst;
      if (active && sfd >= 0) { vpd_fill_peer_addr(&dst, s->peers[i].sock_path); }
      pthread_mutex_unlock(&s->lock);
      if (!active || sfd < 0) { continue; }
      dgram[VIRTUAL_PORT_DGRAM_PORT_OFF] = (uint8_t)(i + 1);
      memcpy(VIRTUAL_PORT_DGRAM_FRAME_PTR(dgram), data, length);
      size_t dlen = VIRTUAL_PORT_DGRAM_LEN(length);
      if (sendto(sfd, dgram, dlen, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        bm_debug("vpd_send: flood peer %d failed errno=%d\n", i + 1, errno);
        err = BmEIO;
      }
    }
  } else {
    // Unicast to one peer.
    int idx = port - 1;
    pthread_mutex_lock(&s->lock);
    bool active = s->peers[idx].active;
    int  sfd    = s->peers[idx].send_fd;
    struct sockaddr_un dst;
    if (active && sfd >= 0) { vpd_fill_peer_addr(&dst, s->peers[idx].sock_path); }
    pthread_mutex_unlock(&s->lock);
    if (!active || sfd < 0) { return BmEINVAL; }
    dgram[VIRTUAL_PORT_DGRAM_PORT_OFF] = port;
    memcpy(VIRTUAL_PORT_DGRAM_FRAME_PTR(dgram), data, length);
    size_t dlen = VIRTUAL_PORT_DGRAM_LEN(length);
    if (sendto(sfd, dgram, dlen, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
      bm_debug("vpd_send: unicast port %d failed errno=%d\n", port, errno);
      err = BmEIO;
    }
  }
  return err;
}

// -------------------------------------------------------------------------
// Task 2h: retry_negotiation()
// -------------------------------------------------------------------------

/// Re-open the send socket for a peer that was previously unreachable.
/// @param port_num  1-based port number (same convention as enable_port/disable_port).
/// @param renegotiated  set to true if a send socket is now open.
static BmErr vpd_retry_negotiation(void *self, uint8_t port_num,
                                    bool *renegotiated) {
  VirtualPortState *s = (VirtualPortState *)self;
  if (renegotiated) { *renegotiated = false; }
  if (port_num < 1 || port_num > VIRTUAL_PORT_MAX_PEERS) { return BmEINVAL; }
  int idx = port_num - 1;
  pthread_mutex_lock(&s->lock);
  PeerEntry *p = &s->peers[idx];
  if (!p->active) { pthread_mutex_unlock(&s->lock); return BmOK; } // no peer configured — not an error
  // Check whether the peer's receive socket path now exists on disk.
  if (access(p->sock_path, F_OK) != 0) {
    pthread_mutex_unlock(&s->lock);
    return BmOK; // peer still unreachable
  }
  // (Re-)open the send socket if it is not already open.
  bool newly_connected = false;
  if (p->send_fd < 0) {
    int sfd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sfd >= 0) {
      p->send_fd = sfd;
      newly_connected = true;
      if (renegotiated) { *renegotiated = true; }
    }
  } else {
    // Already open — still report as renegotiated so l2 can stop the timer.
    newly_connected = true;
    if (renegotiated) { *renegotiated = true; }
  }
  // Snapshot link_change under lock before releasing.
  void (*lc)(uint8_t, bool) = s->callbacks.link_change;
  pthread_mutex_unlock(&s->lock);
  // Fire link_change(idx, true) so l2 stops the renegotiation timer and sets
  // the port as enabled in enabled_ports_mask.  This handles the race where
  // the l2 thread starts the renegotiation timer AFTER vpd_enable() already
  // fired link_change — in that case the timer would never stop without this.
  if (newly_connected && lc) { lc((uint8_t)idx, true); }
  return BmOK;
}

// -------------------------------------------------------------------------
// Task 2i: port_stats() / handle_interrupt() stubs
// -------------------------------------------------------------------------

static BmErr vpd_port_stats(void *self, uint8_t port_index, void *stats) {
  (void)self; (void)port_index; (void)stats;
  return BmOK;
}

static BmErr vpd_handle_interrupt(void *self) {
  (void)self;
  return BmOK;
}

// -------------------------------------------------------------------------
// Task 2j: NetworkDeviceTrait + virtual_port_device_get()
// -------------------------------------------------------------------------

static const NetworkDeviceTrait s_vpd_trait = {
  vpd_send,
  vpd_enable,
  vpd_disable,
  vpd_enable_port,
  vpd_disable_port,
  vpd_retry_negotiation,
  vpd_num_ports,
  vpd_port_stats,
  vpd_handle_interrupt,
};

/// Build and return a NetworkDevice backed by the module-level singleton.
/// Task 2g: if cfg->num_peers exceeds VIRTUAL_PORT_MAX_PEERS, excess peers
/// are silently dropped with a log message (cap enforcement).
NetworkDevice virtual_port_device_get(const VirtualPortCfg *cfg) {
  // Task 2g: 15-neighbor cap.
  uint8_t num_peers = cfg->num_peers;
  if (num_peers > VIRTUAL_PORT_MAX_PEERS) {
    bm_debug("vpd: peer count %u exceeds cap %d\n",
             (unsigned)num_peers, VIRTUAL_PORT_MAX_PEERS);
    num_peers = VIRTUAL_PORT_MAX_PEERS;
  }

  memset(&g_vport_state, 0, sizeof(g_vport_state));
  pthread_mutex_init(&g_vport_state.lock, NULL);

  // Set sentinel -1 for all fds (0 is valid for stdin).
  g_vport_state.recv_fd = -1;
  for (int i = 0; i < VIRTUAL_PORT_MAX_PEERS; i++) {
    g_vport_state.peers[i].send_fd = -1;
  }

  // Copy identity fields.
  g_vport_state.own_node_id = cfg->own_node_id;
  strncpy(g_vport_state.socket_dir, cfg->socket_dir,
          sizeof(g_vport_state.socket_dir) - 1);
  snprintf(g_vport_state.own_sock_path, sizeof(g_vport_state.own_sock_path),
           VIRTUAL_PORT_SOCK_FMT, g_vport_state.socket_dir, cfg->own_node_id);

  // Populate peer table (peers[i] ↔ port i+1).
  for (int i = 0; i < (int)num_peers; i++) {
    g_vport_state.peers[i].node_id = cfg->peer_ids[i];
    g_vport_state.peers[i].active  = true;
    g_vport_state.peers[i].send_fd = -1;
    snprintf(g_vport_state.peers[i].sock_path,
             sizeof(g_vport_state.peers[i].sock_path),
             VIRTUAL_PORT_SOCK_FMT, g_vport_state.socket_dir,
             cfg->peer_ids[i]);
  }

  // Point dev.callbacks directly at g_vport_state.callbacks so that when
  // bm_l2_init writes network_device.callbacks->receive / ->link_change, those
  // values are visible to the vpd trait functions that read s->callbacks.*.
  NetworkDevice dev;
  dev.self      = &g_vport_state;
  dev.trait     = &s_vpd_trait;
  dev.callbacks = &g_vport_state.callbacks;
  return dev;
}

