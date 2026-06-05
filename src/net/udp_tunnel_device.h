#pragma once

/// @file udp_tunnel_device.h
/// @brief Bristlemouth NetworkDevice backed by the UDP tunnel transport.
///
/// Implements NetworkDeviceTrait over udp_tunnel_transport, presenting
/// each configured tunnel peer as one BM port (1-based).
///
/// Link state is inferred from udp_tunnel_peer_alive(): a port is considered
/// "up" once the first frame is received from that peer (confirming two-way
/// reachability), and stays up until the device is disabled.
///
/// This device is the appropriate choice when BM needs to run over a routed
/// IP network — WiFi, Tailscale, internet — rather than a direct L2 link.

#include "network_device.h"
#include "udp_tunnel_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Configuration for the UDP tunnel NetworkDevice.
typedef struct {
  uint16_t          listen_port;                    ///< Local UDP port to bind (receive).
  UdpTunnelPeerCfg  peers[UDP_TUNNEL_MAX_PEERS];    ///< Peer configurations.
  uint8_t           num_peers;                      ///< Number of active peers.
} UdpTunnelDeviceCfg;

/// Build and return a NetworkDevice backed by the UDP tunnel transport.
///
/// The configuration is copied; the caller's struct need not remain valid
/// after this call.  The UDP socket is NOT opened here — it is opened by
/// the NetworkDeviceTrait::enable() call made internally by bm_l2_init().
///
/// @param cfg  Pointer to a populated UdpTunnelDeviceCfg.
/// @return     Initialized NetworkDevice ready for bm_l2_init().
NetworkDevice udp_tunnel_device_get(const UdpTunnelDeviceCfg *cfg);

/// RX callback — pass this to udp_tunnel_transport_init() as rx_cb.
/// Delivers received frames to the BM stack via callbacks->receive().
void udp_tunnel_rx_cb_handler(uint8_t port_num, const uint8_t *frame,
                               size_t len, void *ctx);

#ifdef __cplusplus
}
#endif
