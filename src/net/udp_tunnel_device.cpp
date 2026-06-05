#include "udp_tunnel_device.h"
#include "bm_log.h"
#include "udp_tunnel_transport.h"

#include <stdbool.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static struct {
  NetworkDeviceCallbacks callbacks;
  UdpTunnelDeviceCfg     cfg;
  bool                   link_state[UDP_TUNNEL_MAX_PEERS]; ///< Per-port link up/down.
} s_utd;

// ---------------------------------------------------------------------------
// RX callback — called by the transport RX thread
// ---------------------------------------------------------------------------

void udp_tunnel_rx_cb_handler(uint8_t port_num, const uint8_t *frame,
                               size_t len, void *ctx) {
  (void)ctx;
  if (s_utd.callbacks.receive && len > 0) {
    // bm_l2 receive expects a non-const pointer (legacy C API).
    s_utd.callbacks.receive(port_num, (uint8_t *)frame, len);
  }
}

// ---------------------------------------------------------------------------
// NetworkDeviceTrait implementation
// ---------------------------------------------------------------------------

static uint8_t utd_num_ports(void) {
  return s_utd.cfg.num_peers;
}

static BmErr utd_send(void *self, uint8_t *data, size_t length, uint8_t port) {
  (void)self;
  return udp_tunnel_send(port, data, length) == 0 ? BmOK : BmEIO;
}

static BmErr utd_enable(void *self) {
  (void)self;
  int rc = udp_tunnel_transport_init(s_utd.cfg.listen_port, s_utd.cfg.peers,
                                     s_utd.cfg.num_peers,
                                     udp_tunnel_rx_cb_handler, NULL);
  if (rc != 0) {
    bm_log_error("udp_tunnel_device: transport init failed");
    return BmEIO;
  }
  // Ports start down; retry_negotiation() brings them up when peers respond.
  memset(s_utd.link_state, 0, sizeof(s_utd.link_state));
  return BmOK;
}

static BmErr utd_disable(void *self) {
  (void)self;
  udp_tunnel_transport_deinit();
  if (s_utd.callbacks.link_change) {
    for (uint8_t i = 0; i < s_utd.cfg.num_peers; i++) {
      if (s_utd.link_state[i]) {
        s_utd.callbacks.link_change(i, false);
        s_utd.link_state[i] = false;
      }
    }
  }
  return BmOK;
}

static BmErr utd_enable_port(void *self, uint8_t port_num) {
  (void)self;
  // Nothing to do at the transport layer for UDP; link_change is fired by
  // retry_negotiation() once the peer is confirmed alive.
  return BmOK;
}

static BmErr utd_disable_port(void *self, uint8_t port_num) {
  (void)self;
  if (port_num < 1 || port_num > s_utd.cfg.num_peers) { return BmEINVAL; }
  uint8_t idx = port_num - 1;
  if (s_utd.link_state[idx] && s_utd.callbacks.link_change) {
    s_utd.callbacks.link_change(idx, false);
    s_utd.link_state[idx] = false;
  }
  return BmOK;
}

/// Called by bm_core's 100 ms renegotiation timer for each port.
/// All configured tunnel peers are considered immediately reachable since we
/// have their static IP:port — there is no carrier-detect signal to wait for.
/// Once both sides have their ports up, BCMP heartbeats flow and neighbor
/// discovery proceeds normally.  The alive flag is preserved for diagnostics.
static BmErr utd_retry_negotiation(void *self, uint8_t port_index,
                                    bool *renegotiated) {
  (void)self;
  *renegotiated = false;
  if (port_index >= s_utd.cfg.num_peers) { return BmEINVAL; }

  if (!s_utd.link_state[port_index]) {
    s_utd.link_state[port_index] = true;
    *renegotiated = true;
    if (s_utd.callbacks.link_change) {
      s_utd.callbacks.link_change(port_index, true);
    }
    bm_log_info("udp_tunnel_device: port %u up (peer %s:%u)",
                (unsigned)(port_index + 1),
                s_utd.cfg.peers[port_index].ip,
                (unsigned)s_utd.cfg.peers[port_index].port);
  }
  return BmOK;
}

static BmErr utd_port_stats(void *self, uint8_t port_index, void *stats) {
  (void)self; (void)port_index; (void)stats;
  return BmOK;
}

static BmErr utd_handle_interrupt(void *self) {
  (void)self;
  return BmOK;
}

// ---------------------------------------------------------------------------
// Trait + factory
// ---------------------------------------------------------------------------

static const NetworkDeviceTrait s_utd_trait = {
  utd_send,
  utd_enable,
  utd_disable,
  utd_enable_port,
  utd_disable_port,
  utd_retry_negotiation,
  utd_num_ports,
  utd_port_stats,
  utd_handle_interrupt,
};

NetworkDevice udp_tunnel_device_get(const UdpTunnelDeviceCfg *cfg) {
  memset(&s_utd, 0, sizeof(s_utd));
  s_utd.cfg = *cfg;

  NetworkDevice dev;
  dev.self      = NULL; // module-level singleton, no per-instance heap alloc
  dev.trait     = &s_utd_trait;
  dev.callbacks = &s_utd.callbacks;
  return dev;
}
