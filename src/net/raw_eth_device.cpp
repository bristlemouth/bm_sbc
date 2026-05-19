#include "raw_eth_device.h"
#include "raw_eth_transport.h"
#include "bm_log.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static struct {
  NetworkDeviceCallbacks callbacks; ///< Written by bm_l2_init().
  RawEthDeviceCfg        cfg;       ///< Copy of caller's configuration.
  bool link_state[RAW_ETH_DEVICE_MAX_PORTS]; ///< Cached carrier state.
} s_red;

// ---------------------------------------------------------------------------
// RX callback — shared by all ports; port_idx distinguishes them
// ---------------------------------------------------------------------------

static void red_rx_cb(uint8_t port_idx, const uint8_t *frame, size_t len,
                      void *ctx) {
  (void)ctx;
  // Transport port_idx is 0-based; BM port numbers are 1-based.
  uint8_t bm_port = port_idx + 1;
  if (s_red.callbacks.receive && len > 0) {
    // bm_l2 receive expects a non-const pointer (legacy C API).
    s_red.callbacks.receive(bm_port, (uint8_t *)frame, len);
  }
}

// ---------------------------------------------------------------------------
// NetworkDeviceTrait implementation
// ---------------------------------------------------------------------------

static uint8_t red_num_ports(void) {
  return s_red.cfg.num_ports;
}

static BmErr red_send(void *self, uint8_t *data, size_t length, uint8_t port) {
  (void)self;

  if (port == 0) {
    // Flood: send on all active ports.
    BmErr result = BmOK;
    for (uint8_t i = 0; i < s_red.cfg.num_ports; i++) {
      if (raw_eth_send(i, data, length) != 0) {
        result = BmEIO;
      }
    }
    return result;
  }

  if (port >= 1 && port <= s_red.cfg.num_ports) {
    // 1-based BM port → 0-based transport index.
    return raw_eth_send((uint8_t)(port - 1), data, length) == 0 ? BmOK : BmEIO;
  }

  return BmEINVAL;
}

static BmErr red_enable(void *self) {
  (void)self;
  for (uint8_t i = 0; i < s_red.cfg.num_ports; i++) {
    if (raw_eth_transport_init(i, s_red.cfg.ports[i].iface_name, red_rx_cb,
                               NULL) != 0) {
      bm_log_error("raw_eth_device: failed to open port %u (%s)", i,
                   s_red.cfg.ports[i].iface_name);
      return BmEIO;
    }
    // Fire link_change for any port that already has carrier at enable time.
    bool up = raw_eth_carrier(i);
    s_red.link_state[i] = up;
    if (up && s_red.callbacks.link_change) {
      s_red.callbacks.link_change(i, true);
    }
  }
  return BmOK;
}

static BmErr red_disable(void *self) {
  (void)self;
  for (uint8_t i = 0; i < s_red.cfg.num_ports; i++) {
    if (s_red.callbacks.link_change) {
      s_red.callbacks.link_change(i, false);
    }
    raw_eth_transport_deinit(i);
    s_red.link_state[i] = false;
  }
  return BmOK;
}

static BmErr red_enable_port(void *self, uint8_t port_num) {
  (void)self;
  if (port_num < 1 || port_num > s_red.cfg.num_ports) {
    return BmEINVAL;
  }
  return BmOK; // Ports are always enabled once red_enable() runs.
}

static BmErr red_disable_port(void *self, uint8_t port_num) {
  (void)self;
  if (port_num < 1 || port_num > s_red.cfg.num_ports) {
    return BmEINVAL;
  }
  return BmOK; // Individual port teardown not supported; use red_disable().
}

/// Called by bm_core's 100 ms renegotiation timer for each port.
/// Polls /sys/class/net/<iface>/carrier and fires link_change when the
/// state differs from the cached value.
static BmErr red_retry_negotiation(void *self, uint8_t port_index,
                                   bool *renegotiated) {
  (void)self;
  if (port_index >= s_red.cfg.num_ports) {
    if (renegotiated) {
      *renegotiated = false;
    }
    return BmOK;
  }

  bool up = raw_eth_carrier(port_index);
  if (up != s_red.link_state[port_index]) {
    s_red.link_state[port_index] = up;
    bm_log_info("raw_eth_device: port %u (%s) link %s", port_index + 1,
                s_red.cfg.ports[port_index].iface_name, up ? "up" : "down");
    if (s_red.callbacks.link_change) {
      s_red.callbacks.link_change(port_index, up);
    }
    if (renegotiated) {
      *renegotiated = true;
    }
  } else {
    if (renegotiated) {
      *renegotiated = false;
    }
  }
  return BmOK;
}

static BmErr red_port_stats(void *self, uint8_t port_index, void *stats) {
  (void)self;
  (void)port_index;
  (void)stats;
  return BmOK;
}

static BmErr red_handle_interrupt(void *self) {
  (void)self;
  return BmOK;
}

// ---------------------------------------------------------------------------
// Trait table
// ---------------------------------------------------------------------------

static const NetworkDeviceTrait s_red_trait = {
    red_send,        red_enable,           red_disable,
    red_enable_port, red_disable_port,     red_retry_negotiation,
    red_num_ports,   red_port_stats,       red_handle_interrupt,
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

NetworkDevice raw_eth_device_get(const RawEthDeviceCfg *cfg) {
  memset(&s_red, 0, sizeof(s_red));
  s_red.cfg = *cfg; // Copy config; caller's struct can go out of scope.

  NetworkDevice dev;
  dev.self      = NULL;
  dev.trait     = &s_red_trait;
  dev.callbacks = &s_red.callbacks;
  return dev;
}
