#include "gateway_device.h"
#include "uart_l2_transport.h"
#include "virtual_port_device.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

/// The gateway wraps an existing VPD device and adds a UART port.
/// VPD owns ports 1..vpd_num_ports, UART is vpd_num_ports + 1.
static struct {
  NetworkDevice *vpd;           ///< Underlying VirtualPortDevice.
  uint8_t vpd_ports;            ///< Number of VPD ports (cached).
  uint8_t uart_port;            ///< Port number for the UART link.
  NetworkDeviceCallbacks cbs;   ///< Callbacks (receive, link_change, etc.)
} s_gw;

// ---------------------------------------------------------------------------
// Trait implementation
// ---------------------------------------------------------------------------

static uint8_t gw_num_ports(void) {
  return s_gw.vpd_ports + 1; // VPD ports + 1 UART port
}

/// Send on the gateway: delegate to VPD for ports 1..N, UART for port N+1,
/// flood all for port 0.
static BmErr gw_send(void *self, uint8_t *data, size_t length, uint8_t port) {
  (void)self;

  if (port == 0) {
    // Flood: send on all VPD ports + UART.
    BmErr vpd_err = s_gw.vpd->trait->send(s_gw.vpd->self, data, length, 0);
    int uart_err = uart_l2_send(data, length);
    // Return error only if both failed.
    if (vpd_err != BmOK && uart_err != 0) {
      return vpd_err;
    }
    return BmOK;
  }

  if (port >= 1 && port <= s_gw.vpd_ports) {
    // Delegate to VPD.
    return s_gw.vpd->trait->send(s_gw.vpd->self, data, length, port);
  }

  if (port == s_gw.uart_port) {
    // Send on UART.
    return uart_l2_send(data, length) == 0 ? BmOK : BmEIO;
  }

  return BmEINVAL;
}

static BmErr gw_enable(void *self) {
  (void)self;
  // Enable the VPD; UART is already running (started in transport_init).
  BmErr err = s_gw.vpd->trait->enable(s_gw.vpd->self);
  if (err == BmOK) {
    // Signal link-up for the UART port.
    if (s_gw.cbs.link_change) {
      s_gw.cbs.link_change(s_gw.uart_port, true);
    }
  }
  return err;
}

static BmErr gw_disable(void *self) {
  (void)self;
  if (s_gw.cbs.link_change) {
    s_gw.cbs.link_change(s_gw.uart_port, false);
  }
  uart_l2_transport_deinit();
  return s_gw.vpd->trait->disable(s_gw.vpd->self);
}

static BmErr gw_enable_port(void *self, uint8_t port_num) {
  (void)self;
  if (port_num >= 1 && port_num <= s_gw.vpd_ports) {
    return s_gw.vpd->trait->enable_port(s_gw.vpd->self, port_num);
  }
  if (port_num == s_gw.uart_port) {
    return BmOK; // UART is always enabled once transport_init succeeds.
  }
  return BmEINVAL;
}

static BmErr gw_disable_port(void *self, uint8_t port_num) {
  (void)self;
  if (port_num >= 1 && port_num <= s_gw.vpd_ports) {
    return s_gw.vpd->trait->disable_port(s_gw.vpd->self, port_num);
  }
  if (port_num == s_gw.uart_port) {
    return BmOK; // No-op for UART port.
  }
  return BmEINVAL;
}

static BmErr gw_retry_negotiation(void *self, uint8_t port_index,
                                   bool *renegotiated) {
  (void)self;
  if (port_index < s_gw.vpd_ports) {
    return s_gw.vpd->trait->retry_negotiation(s_gw.vpd->self, port_index,
                                               renegotiated);
  }
  // UART port: no negotiation needed.
  if (renegotiated) { *renegotiated = false; }
  return BmOK;
}

static BmErr gw_port_stats(void *self, uint8_t port_index, void *stats) {
  (void)self;
  if (port_index < s_gw.vpd_ports) {
    return s_gw.vpd->trait->port_stats(s_gw.vpd->self, port_index, stats);
  }
  return BmOK;
}

static BmErr gw_handle_interrupt(void *self) {
  (void)self;
  return s_gw.vpd->trait->handle_interrupt(s_gw.vpd->self);
}

// ---------------------------------------------------------------------------
// Trait table
// ---------------------------------------------------------------------------

static const NetworkDeviceTrait s_gw_trait = {
  gw_send,
  gw_enable,
  gw_disable,
  gw_enable_port,
  gw_disable_port,
  gw_retry_negotiation,
  gw_num_ports,
  gw_port_stats,
  gw_handle_interrupt,
};



// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

NetworkDevice gateway_device_get(NetworkDevice *vpd_dev) {
  s_gw.vpd = vpd_dev;
  s_gw.vpd_ports = vpd_dev->trait->num_ports();
  s_gw.uart_port = s_gw.vpd_ports + 1;
  memset(&s_gw.cbs, 0, sizeof(s_gw.cbs));

  // Wire VPD callbacks through to ours so link_change/receive from VPD
  // peers still reaches the stack.
  vpd_dev->callbacks = &s_gw.cbs;

  NetworkDevice dev;
  dev.self = nullptr;
  dev.trait = &s_gw_trait;
  dev.callbacks = &s_gw.cbs;
  return dev;
}

void gateway_uart_rx_cb(const uint8_t *frame, size_t len, void *ctx) {
  (void)ctx;
  if (s_gw.cbs.receive && len > 0) {
    // Deliver the UART frame to the stack as arriving on the UART port.
    // The receive callback expects a non-const pointer (legacy API).
    s_gw.cbs.receive(s_gw.uart_port, (uint8_t *)frame, len);
  }
}