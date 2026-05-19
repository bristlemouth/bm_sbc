#pragma once

/// @file raw_eth_transport.h
/// @brief Raw L2 Ethernet frame transport over a Linux network interface.
///
/// Uses AF_PACKET / SOCK_RAW to send and receive complete L2 Ethernet
/// frames on a named network interface. No framing overhead — the MAC/PHY
/// handles FCS. Linux-only.
///
/// Up to RAW_ETH_MAX_PORTS interfaces may be open simultaneously, each
/// identified by a 0-based port index.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Maximum number of simultaneously open raw Ethernet ports.
#define RAW_ETH_MAX_PORTS 2

/// Callback invoked by the RX thread when a complete L2 Ethernet frame
/// has been received on the interface.
///
/// @param port_idx  0-based index of the port that received the frame.
/// @param frame     Pointer to the raw L2 Ethernet frame (header + payload,
///                  no FCS).
/// @param len       Frame length in bytes.
/// @param ctx       User-supplied context pointer.
typedef void (*raw_eth_rx_cb)(uint8_t port_idx, const uint8_t *frame,
                              size_t len, void *ctx);

/// Open a raw socket on @p iface_name and start an RX thread.
///
/// Puts the socket into all-multicast mode (PACKET_MR_ALLMULTI) so that
/// BM multicast frames (e.g. ff03::1) are received without needing to
/// enumerate every BM multicast group. Falls back to promiscuous mode if
/// the driver does not support all-multicast.
///
/// TX loopback is filtered in the RX thread: frames reflected back by
/// the kernel with PACKET_OUTGOING are silently discarded.
///
/// @param port_idx   0-based port index (must be < RAW_ETH_MAX_PORTS).
/// @param iface_name Linux network interface name (e.g. "eth1").
/// @param rx_cb      Callback for received frames (may be NULL).
/// @param rx_ctx     Context pointer passed to @p rx_cb.
/// @return 0 on success, -1 on failure.
int raw_eth_transport_init(uint8_t port_idx, const char *iface_name,
                           raw_eth_rx_cb rx_cb, void *rx_ctx);

/// Send a complete L2 Ethernet frame on the interface bound to @p port_idx.
///
/// @param port_idx  0-based port index.
/// @param frame     Complete L2 frame (dst MAC + src MAC + EtherType +
///                  payload). Must be a valid Ethernet frame.
/// @param len       Frame length in bytes.
/// @return 0 on success, -1 on failure.
int raw_eth_send(uint8_t port_idx, const uint8_t *frame, size_t len);

/// Check the carrier state of the interface bound to @p port_idx.
///
/// Reads /sys/class/net/<iface>/carrier.
///
/// @return true if carrier is up, false if down or if the state cannot
///         be determined.
bool raw_eth_carrier(uint8_t port_idx);

/// Stop the RX thread and close the raw socket for @p port_idx.
void raw_eth_transport_deinit(uint8_t port_idx);

#ifdef __cplusplus
}
#endif
