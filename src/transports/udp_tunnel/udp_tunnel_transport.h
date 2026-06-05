#pragma once

/// @file udp_tunnel_transport.h
/// @brief BM L2 frame tunnel over unicast UDP.
///
/// Wraps complete BM L2 Ethernet frames in ordinary IPv4/IPv6 UDP datagrams
/// sent to a statically-configured peer IP:port.  Works over any routed
/// network — WiFi, cellular, Tailscale, etc. — without needing AF_PACKET or
/// multicast delivery.
///
/// Wire format (one UDP payload per datagram):
///
///   +---------------------+
///   | BM L2 frame (raw)   |
///   +---------------------+
///
/// No extra framing: SOCK_DGRAM preserves message boundaries atomically, so
/// no length prefix or CRC is needed at this layer.
///
/// Up to UDP_TUNNEL_MAX_PEERS peers may be registered, each assigned a
/// 1-based BM port index.  Port 1 = peers[0], port 2 = peers[1], etc.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Maximum number of simultaneous tunnel peers.
#define UDP_TUNNEL_MAX_PEERS 4

/// UDP port used by default if not specified per-peer.
#define UDP_TUNNEL_DEFAULT_PORT 4844

/// Callback invoked by the RX thread when a complete BM L2 frame has been
/// received from any peer.
///
/// @param port_num  1-based BM port the frame arrived on.
/// @param frame     Pointer to the raw BM L2 Ethernet frame.
/// @param len       Frame length in bytes.
/// @param ctx       User-supplied context pointer.
typedef void (*udp_tunnel_rx_cb)(uint8_t port_num, const uint8_t *frame,
                                 size_t len, void *ctx);

/// Configuration for a single tunnel peer.
typedef struct {
  char     ip[64];   ///< Peer IPv4 or IPv6 address string (e.g. "100.94.12.77").
  uint16_t port;     ///< Peer UDP port.
  uint64_t node_id;  ///< Peer BM node ID (informational, used for logging).
} UdpTunnelPeerCfg;

/// Initialize the UDP tunnel transport.
///
/// Opens a single bound UDP socket on @p listen_port for receiving from all
/// peers, and stores peer addresses for TX.  Starts a background RX thread.
///
/// Must be called before udp_tunnel_send().
///
/// @param listen_port  Local UDP port to listen on.
/// @param peers        Array of peer configurations.
/// @param num_peers    Number of entries in @p peers (max UDP_TUNNEL_MAX_PEERS).
/// @param rx_cb        Callback for received frames (may be NULL).
/// @param rx_ctx       Context pointer passed to @p rx_cb.
/// @return 0 on success, -1 on failure.
int udp_tunnel_transport_init(uint16_t listen_port,
                              const UdpTunnelPeerCfg *peers, uint8_t num_peers,
                              udp_tunnel_rx_cb rx_cb, void *rx_ctx);

/// Send a BM L2 frame to a specific peer port (1-based) or flood all peers
/// (port 0).
///
/// @param port   1-based BM port index, or 0 to flood all configured peers.
/// @param frame  Complete BM L2 Ethernet frame.
/// @param len    Frame length in bytes.
/// @return 0 on success, -1 on any send failure.
int udp_tunnel_send(uint8_t port, const uint8_t *frame, size_t len);

/// Stop the RX thread and close all sockets.
void udp_tunnel_transport_deinit(void);

/// Return true if the peer at 1-based @p port_num is reachable (i.e. we have
/// received at least one frame from it since init, confirming two-way path).
bool udp_tunnel_peer_alive(uint8_t port_num);

#ifdef __cplusplus
}
#endif
