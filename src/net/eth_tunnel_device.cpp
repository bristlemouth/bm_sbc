#ifdef __linux__

#include "eth_tunnel_device.h"
#include "bm_log.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static struct {
  NetworkDevice eth_dev;
  NetworkDevice tunnel_dev;
  uint8_t       eth_ports;    ///< num_ports of the raw_eth sub-device.
  uint8_t       tunnel_ports; ///< num_peers of the udp_tunnel sub-device.
  NetworkDeviceCallbacks callbacks; ///< bm_l2_init() writes receive/link_change here.
} s_etd;

// ---------------------------------------------------------------------------
// Tunnel link_change wrapper — adds eth port offset before forwarding.
//
// raw_eth fires link_change(i, up) where i is 0-based (0..eth_ports-1).
// udp_tunnel fires link_change(idx, up) where idx is 0-based (0..tunnel_ports-1).
// bm_l2's link_change() expects a 0-based port_idx across all ports.
// So tunnel's idx needs to be shifted by eth_ports before forwarding.
// ---------------------------------------------------------------------------

static void etd_tunnel_link_change(uint8_t port_idx, bool is_up) {
  if (s_etd.callbacks.link_change) {
    s_etd.callbacks.link_change((uint8_t)(port_idx + s_etd.eth_ports), is_up);
  }
}

/// Tunnel receive wrapper — adjusts 1-based tunnel-local port_num to the
/// composite device's 1-based port number by adding the eth port offset.
/// Without this, bm_l2 would think tunnel frames arrived on eth port 1.
static void etd_tunnel_receive(uint8_t port_num, uint8_t *data, size_t len) {
  if (s_etd.callbacks.receive) {
    s_etd.callbacks.receive((uint8_t)(s_etd.eth_ports + port_num), data, len);
  }
}

// ---------------------------------------------------------------------------
// NetworkDeviceTrait implementation
// ---------------------------------------------------------------------------

static uint8_t etd_num_ports(void) {
  return (uint8_t)(s_etd.eth_ports + s_etd.tunnel_ports);
}

static BmErr etd_send(void *self, uint8_t *data, size_t length, uint8_t port) {
  (void)self;
  if (port == 0) {
    // Flood: send to all eth ports and all tunnel peers.
    BmErr e1 = s_etd.eth_dev.trait->send(s_etd.eth_dev.self, data, length, 0);
    BmErr e2 = s_etd.tunnel_dev.trait->send(s_etd.tunnel_dev.self, data, length, 0);
    return (e1 != BmOK) ? e1 : e2;
  }
  if (port >= 1 && port <= s_etd.eth_ports) {
    return s_etd.eth_dev.trait->send(s_etd.eth_dev.self, data, length, port);
  }
  // Tunnel port: adjust to tunnel-local 1-based port number.
  uint8_t tport = (uint8_t)(port - s_etd.eth_ports);
  return s_etd.tunnel_dev.trait->send(s_etd.tunnel_dev.self, data, length, tport);
}

static BmErr etd_enable(void *self) {
  (void)self;
  // bm_l2_init() has already written receive/link_change into s_etd.callbacks.
  // Propagate them to both sub-devices before enabling.
  //
  // raw_eth: copy all callbacks directly — its port indices are already
  // in the 0-based range [0..eth_ports-1] which is correct for bm_l2.
  *s_etd.eth_dev.callbacks = s_etd.callbacks;
  //
  // udp_tunnel: offset-adjusted wrappers for both receive and link_change.
  s_etd.tunnel_dev.callbacks->receive     = etd_tunnel_receive;
  s_etd.tunnel_dev.callbacks->link_change = etd_tunnel_link_change;

  BmErr err = s_etd.eth_dev.trait->enable(s_etd.eth_dev.self);
  if (err != BmOK) {
    bm_log_error("eth_tunnel_device: eth enable failed (%d)", (int)err);
    return err;
  }
  err = s_etd.tunnel_dev.trait->enable(s_etd.tunnel_dev.self);
  if (err != BmOK) {
    bm_log_error("eth_tunnel_device: tunnel enable failed (%d)", (int)err);
  }
  return err;
}

static BmErr etd_disable(void *self) {
  (void)self;
  s_etd.eth_dev.trait->disable(s_etd.eth_dev.self);
  return s_etd.tunnel_dev.trait->disable(s_etd.tunnel_dev.self);
}

static BmErr etd_enable_port(void *self, uint8_t port_num) {
  (void)self;
  if (port_num >= 1 && port_num <= s_etd.eth_ports) {
    return s_etd.eth_dev.trait->enable_port(s_etd.eth_dev.self, port_num);
  }
  return s_etd.tunnel_dev.trait->enable_port(s_etd.tunnel_dev.self,
                                              (uint8_t)(port_num - s_etd.eth_ports));
}

static BmErr etd_disable_port(void *self, uint8_t port_num) {
  (void)self;
  if (port_num >= 1 && port_num <= s_etd.eth_ports) {
    return s_etd.eth_dev.trait->disable_port(s_etd.eth_dev.self, port_num);
  }
  return s_etd.tunnel_dev.trait->disable_port(s_etd.tunnel_dev.self,
                                               (uint8_t)(port_num - s_etd.eth_ports));
}

/// bm_l2 passes 1-based port_num (= timer ID) to retry_negotiation for all
/// ports 1..total_ports.
/// - Eth ports 1..eth_ports   → forwarded as-is to raw_eth (which treats the
///   value as 0-based due to its own convention; this is pre-existing behavior).
/// - Tunnel ports eth_ports+1..total → adjusted to 1-based tunnel-local port
///   (port_num - eth_ports), which udp_tunnel_device handles correctly.
static BmErr etd_retry_negotiation(void *self, uint8_t port_num,
                                    bool *renegotiated) {
  (void)self;
  if (renegotiated) { *renegotiated = false; }
  if (port_num >= 1 && port_num <= s_etd.eth_ports) {
    return s_etd.eth_dev.trait->retry_negotiation(s_etd.eth_dev.self,
                                                   port_num, renegotiated);
  }
  uint8_t tport = (uint8_t)(port_num - s_etd.eth_ports);
  return s_etd.tunnel_dev.trait->retry_negotiation(s_etd.tunnel_dev.self,
                                                    tport, renegotiated);
}

static BmErr etd_port_stats(void *self, uint8_t port_index, void *stats) {
  (void)self;
  if (port_index < s_etd.eth_ports) {
    return s_etd.eth_dev.trait->port_stats(s_etd.eth_dev.self, port_index, stats);
  }
  return s_etd.tunnel_dev.trait->port_stats(s_etd.tunnel_dev.self,
                                             (uint8_t)(port_index - s_etd.eth_ports),
                                             stats);
}

static BmErr etd_handle_interrupt(void *self) {
  (void)self;
  return s_etd.eth_dev.trait->handle_interrupt(s_etd.eth_dev.self);
}

// ---------------------------------------------------------------------------
// Trait table + factory
// ---------------------------------------------------------------------------

static const NetworkDeviceTrait s_etd_trait = {
    etd_send,        etd_enable,            etd_disable,
    etd_enable_port, etd_disable_port,      etd_retry_negotiation,
    etd_num_ports,   etd_port_stats,        etd_handle_interrupt,
};

NetworkDevice eth_tunnel_device_get(const EthTunnelDeviceCfg *cfg) {
  memset(&s_etd, 0, sizeof(s_etd));
  s_etd.eth_dev    = raw_eth_device_get(&cfg->eth);
  s_etd.tunnel_dev = udp_tunnel_device_get(&cfg->tunnel);
  s_etd.eth_ports    = s_etd.eth_dev.trait->num_ports();
  s_etd.tunnel_ports = s_etd.tunnel_dev.trait->num_ports();

  bm_log_info("eth_tunnel_device: %u eth port(s) + %u tunnel peer(s)",
              (unsigned)s_etd.eth_ports, (unsigned)s_etd.tunnel_ports);

  NetworkDevice dev;
  dev.self      = NULL;
  dev.trait     = &s_etd_trait;
  dev.callbacks = &s_etd.callbacks;
  return dev;
}

#endif // __linux__
