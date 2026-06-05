#include "udp_tunnel_transport.h"
#include "bm_log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

// Maximum BM L2 frame size (Ethernet MTU, no FCS).
#define BM_L2_FRAME_MAX 1514

// ---------------------------------------------------------------------------
// Per-peer TX state
// ---------------------------------------------------------------------------

typedef struct {
  UdpTunnelPeerCfg  cfg;
  struct sockaddr_storage addr;   ///< Resolved peer address for sendto().
  socklen_t               addrlen;
  bool                    resolved; ///< True if addr has been populated.
  bool                    alive;    ///< True once we've received from this peer.
} TunnelPeer;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

typedef struct {
  int           recv_fd;
  pthread_t     rx_thread;
  bool          rx_running;
  pthread_mutex_t lock;
  udp_tunnel_rx_cb rx_cb;
  void            *rx_ctx;
  TunnelPeer    peers[UDP_TUNNEL_MAX_PEERS];
  uint8_t       num_peers;
  bool          initialized;
} UdpTunnelState;

static UdpTunnelState s_state;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Resolve a peer's IP+port string into a sockaddr_storage.
/// Returns true on success.
static bool resolve_peer(TunnelPeer *peer) {
  struct addrinfo hints, *res;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_INET;    // Force IPv4 — Tailscale peers are 100.x.x.x
  hints.ai_socktype = SOCK_DGRAM;
  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", (unsigned)peer->cfg.port);

  int rc = getaddrinfo(peer->cfg.ip, port_str, &hints, &res);
  if (rc != 0) {
    bm_log_error("udp_tunnel: getaddrinfo(%s:%u) failed: %s",
                 peer->cfg.ip, (unsigned)peer->cfg.port, gai_strerror(rc));
    return false;
  }
  memcpy(&peer->addr, res->ai_addr, res->ai_addrlen);
  peer->addrlen = (socklen_t)res->ai_addrlen;
  peer->resolved = true;
  freeaddrinfo(res);
  return true;
}

/// Find the 0-based peer index whose resolved address matches @p src.
/// Returns -1 if not found.
static int find_peer_by_addr(const struct sockaddr_storage *src,
                              socklen_t src_len) {
  for (int i = 0; i < (int)s_state.num_peers; i++) {
    TunnelPeer *p = &s_state.peers[i];
    if (!p->resolved) {
      continue;
    }
    if (p->addrlen != src_len) {
      continue;
    }
    if (memcmp(&p->addr, src, src_len) == 0) {
      return i;
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// RX thread
// ---------------------------------------------------------------------------

static void *rx_thread_func(void *arg) {
  (void)arg;
  uint8_t buf[BM_L2_FRAME_MAX];

  while (1) {
    pthread_mutex_lock(&s_state.lock);
    bool running = s_state.rx_running;
    int  fd      = s_state.recv_fd;
    pthread_mutex_unlock(&s_state.lock);

    if (!running || fd < 0) {
      break;
    }

    struct sockaddr_storage src_addr;
    socklen_t src_len = sizeof(src_addr);
    ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                         (struct sockaddr *)&src_addr, &src_len);
    if (n <= 0) {
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        continue;
      }
      if (n < 0) {
        pthread_mutex_lock(&s_state.lock);
        bool still_running = s_state.rx_running;
        pthread_mutex_unlock(&s_state.lock);
        if (still_running) {
          bm_log_error("udp_tunnel: recvfrom error: %s", strerror(errno));
        }
      }
      break;
    }

    // Identify which peer sent this frame.
    pthread_mutex_lock(&s_state.lock);
    int peer_idx = find_peer_by_addr(&src_addr, src_len);
    if (peer_idx < 0) {
      // Unknown sender — log and discard.
      pthread_mutex_unlock(&s_state.lock);
      char host[64] = "<unknown>";
      char svc[16]  = "";
      getnameinfo((struct sockaddr *)&src_addr, src_len,
                  host, sizeof(host), svc, sizeof(svc),
                  NI_NUMERICHOST | NI_NUMERICSERV);
      bm_log_warn("udp_tunnel: frame from unknown peer %s:%s — discarding",
                  host, svc);
      continue;
    }
    s_state.peers[peer_idx].alive = true;
    udp_tunnel_rx_cb cb  = s_state.rx_cb;
    void            *ctx = s_state.rx_ctx;
    pthread_mutex_unlock(&s_state.lock);

    if (cb) {
      // BM port numbers are 1-based; peer_idx is 0-based.
      cb((uint8_t)(peer_idx + 1), buf, (size_t)n, ctx);
    }
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int udp_tunnel_transport_init(uint16_t listen_port,
                              const UdpTunnelPeerCfg *peers, uint8_t num_peers,
                              udp_tunnel_rx_cb rx_cb, void *rx_ctx) {
  if (num_peers > UDP_TUNNEL_MAX_PEERS) {
    bm_log_error("udp_tunnel: num_peers %u exceeds max %d", (unsigned)num_peers,
                 UDP_TUNNEL_MAX_PEERS);
    return -1;
  }

  memset(&s_state, 0, sizeof(s_state));
  pthread_mutex_init(&s_state.lock, NULL);
  s_state.recv_fd   = -1;
  s_state.rx_cb     = rx_cb;
  s_state.rx_ctx    = rx_ctx;
  s_state.num_peers = num_peers;

  // Copy and resolve peer configs.
  for (uint8_t i = 0; i < num_peers; i++) {
    s_state.peers[i].cfg = peers[i];
    if (!resolve_peer(&s_state.peers[i])) {
      bm_log_error("udp_tunnel: failed to resolve peer %u (%s:%u)", (unsigned)i,
                   peers[i].ip, (unsigned)peers[i].port);
      // Non-fatal: retry_negotiation() will re-attempt resolution.
    }
  }

  // Open an IPv4 UDP socket bound to listen_port on all interfaces.
  // Tailscale uses 100.64.0.0/10 (IPv4) so AF_INET is always correct.
  // A dual-stack AF_INET6 socket causes sendto(EINVAL) when the resolved
  // peer address is AF_INET, and receives frames as ::ffff:x.x.x.x which
  // breaks peer lookup.
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    bm_log_error("udp_tunnel: socket() failed: %s", strerror(errno));
    return -1;
  }
  struct sockaddr_in sa4;
  memset(&sa4, 0, sizeof(sa4));
  sa4.sin_family      = AF_INET;
  sa4.sin_port        = htons(listen_port);
  sa4.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (struct sockaddr *)&sa4, sizeof(sa4)) < 0) {
    bm_log_error("udp_tunnel: bind(:%u) failed: %s",
                 (unsigned)listen_port, strerror(errno));
    close(fd);
    return -1;
  }

  // 1-second RX timeout so the RX thread wakes periodically to check rx_running.
  struct timeval tv = {1, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  s_state.recv_fd    = fd;
  s_state.rx_running = true;
  s_state.initialized = true;

  if (pthread_create(&s_state.rx_thread, NULL, rx_thread_func, NULL) != 0) {
    bm_log_error("udp_tunnel: pthread_create failed: %s", strerror(errno));
    close(fd);
    s_state.recv_fd    = -1;
    s_state.rx_running = false;
    s_state.initialized = false;
    return -1;
  }

  bm_log_info("udp_tunnel: listening on :%u with %u peer(s)",
              (unsigned)listen_port, (unsigned)num_peers);
  return 0;
}

int udp_tunnel_send(uint8_t port, const uint8_t *frame, size_t len) {
  if (!frame || len == 0 || len > BM_L2_FRAME_MAX) {
    return -1;
  }

  pthread_mutex_lock(&s_state.lock);
  if (!s_state.initialized || s_state.recv_fd < 0) {
    pthread_mutex_unlock(&s_state.lock);
    return -1;
  }
  int fd = s_state.recv_fd;
  pthread_mutex_unlock(&s_state.lock);

  int result = 0;

  if (port == 0) {
    // Flood all peers.
    for (uint8_t i = 0; i < s_state.num_peers; i++) {
      TunnelPeer *p = &s_state.peers[i];
      if (!p->resolved) {
        // Attempt lazy resolution.
        resolve_peer(p);
        if (!p->resolved) { continue; }
      }
      ssize_t sent = sendto(fd, frame, len, 0,
                            (const struct sockaddr *)&p->addr, p->addrlen);
      if (sent < 0) {
        bm_log_error("udp_tunnel: sendto peer %u (%s:%u) failed: %s",
                     (unsigned)i, p->cfg.ip, (unsigned)p->cfg.port,
                     strerror(errno));
        result = -1;
      }
    }
  } else {
    // Single peer (1-based port → 0-based index).
    if (port < 1 || port > s_state.num_peers) {
      return -1;
    }
    TunnelPeer *p = &s_state.peers[port - 1];
    if (!p->resolved) {
      resolve_peer(p);
      if (!p->resolved) { return -1; }
    }
    ssize_t sent = sendto(fd, frame, len, 0,
                          (const struct sockaddr *)&p->addr, p->addrlen);
    if (sent < 0) {
      bm_log_error("udp_tunnel: sendto peer %u (%s:%u) failed: %s",
                   (unsigned)(port - 1), p->cfg.ip, (unsigned)p->cfg.port,
                   strerror(errno));
      result = -1;
    }
  }
  return result;
}

void udp_tunnel_transport_deinit(void) {
  pthread_mutex_lock(&s_state.lock);
  s_state.rx_running  = false;
  s_state.initialized = false;
  int fd = s_state.recv_fd;
  s_state.recv_fd = -1;
  pthread_mutex_unlock(&s_state.lock);

  if (fd >= 0) { close(fd); }
  pthread_join(s_state.rx_thread, NULL);
  pthread_mutex_destroy(&s_state.lock);
}

bool udp_tunnel_peer_alive(uint8_t port_num) {
  if (port_num < 1 || port_num > s_state.num_peers) { return false; }
  pthread_mutex_lock(&s_state.lock);
  bool alive = s_state.peers[port_num - 1].alive;
  pthread_mutex_unlock(&s_state.lock);
  return alive;
}
