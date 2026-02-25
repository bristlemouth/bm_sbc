#pragma once

/// @file virtual_port_device.h
/// @brief Per-peer virtual-port network device for local IPC.
///
/// Implements NetworkDeviceTrait with per-peer virtual ports
/// (strict one-link-per-port semantics, max 15 neighbors).
///
/// =========================================================================
/// IPC Transport Design (Milestone 3)
/// =========================================================================
///
/// ## Mechanism — Unix-domain SOCK_DGRAM sockets
///
/// Each bm_sbc process binds exactly ONE Unix-domain SOCK_DGRAM socket for
/// receiving frames.  It opens additional (unbound) SOCK_DGRAM sockets for
/// sending to each known peer.  Using a single receive socket (rather than
/// one per port) keeps the file-descriptor count low and avoids select/epoll
/// complexity — a single recvfrom() thread suffices.
///
/// ## Socket naming convention
///
///   <socket_dir>/bm_sbc_<node_id_hex16>.sock
///
/// Examples (default socket_dir = /tmp):
///   /tmp/bm_sbc_0000000000000001.sock   (node 0x0000000000000001)
///   /tmp/bm_sbc_deadbeefcafe0001.sock   (node 0xdeadbeefcafe0001)
///
/// The 16-digit zero-padded lowercase hex format ensures uniqueness and
/// lexicographic sortability.  The socket_dir is configurable at launch via
/// the --socket-dir CLI flag (default /tmp).
///
/// ## Wire format
///
/// Every datagram carries exactly one raw L2 Ethernet frame, prefixed by a
/// single byte that encodes the egress virtual-port number (1–15) that the
/// frame was sent out on at the sender:
///
///   +-----------+-----------------------------------+
///   | port (1B) | L2 Ethernet frame (14–1514 bytes) |
///   +-----------+-----------------------------------+
///   ^           ^
///   |           Unmodified BM L2 frame as produced by bm_ip / bm_udp.
///   Sender's egress port for this peer (= receiver's ingress port).
///
/// SOCK_DGRAM preserves message boundaries atomically, so no length field,
/// COBS framing, or CRC is required at this layer.  The maximum datagram
/// size is 1 + 1514 = 1515 bytes, well within the default kernel socket
/// buffer (~212 KB on Linux, ~8 KB on macOS — both far exceed 1515 bytes).
///
/// ## Port-number semantics
///
/// The sender writes its **egress port number** as the first byte — i.e.
/// the slot index (1–15) that this peer occupies in the sender's own peer
/// table.  The receiver reads that byte as the **ingress port number** and
/// passes it straight to bm_l2 via callbacks->receive(port_num, data, len).
///
/// This satisfies the Bristlemouth spec requirement that the ingress port is
/// preserved through the L2 layer (bm_l2 uses it for multicast hairpin
/// suppression and the ingress-port encoding in the IPv6 source address).
///
/// Port 0 (flood / all-ports) is used only internally inside send() to
/// iterate all active peers; it is never written on the wire.
///
/// ## Peer discovery (Milestone 3 — static)
///
/// Topology is supplied at launch via repeated --peer <hex_node_id> CLI
/// flags passed through runtime_init().  Peers are assigned deterministic
/// port slots in insertion order (first --peer → port 1, second → port 2,
/// …, up to port 15).  No dynamic rendezvous is performed.
///
/// ## 15-neighbor hard cap
///
/// Attempting to add a 16th peer logs an error (including the rejected
/// node_id) and returns an error code.  Existing mapped peers are never
/// remapped.
/// =========================================================================

#include <inttypes.h> // PRIx64 (also pulls in stdint.h)

/// Maximum number of directly-connected peers per process.
#define VIRTUAL_PORT_MAX_PEERS 15

/// Socket filename template.  Caller must supply a 108-byte buffer (the
/// POSIX limit for a Unix-domain socket path).
/// Format: "<socket_dir>/bm_sbc_<node_id as 16 lowercase hex digits>.sock"
#define VIRTUAL_PORT_SOCK_FMT "%s/bm_sbc_%016" PRIx64 ".sock"

// -------------------------------------------------------------------------
// Wire-format constants
//
// Datagram layout: [port (1 B)] [L2 Ethernet frame (14–1514 B)]
// -------------------------------------------------------------------------

/// Byte offset of the port field within a datagram.
#define VIRTUAL_PORT_DGRAM_PORT_OFF  0

/// Byte offset of the L2 Ethernet frame within a datagram.
#define VIRTUAL_PORT_DGRAM_FRAME_OFF 1

/// Size of the datagram header (the single port byte).
#define VIRTUAL_PORT_DGRAM_HDR_LEN   1

/// Ethernet header length (6-byte dst MAC + 6-byte src MAC + 2-byte ethertype).
/// Matches the sum of ethernet_destination_size_bytes + ethernet_src_size_bytes
/// + ethernet_type_size_bytes defined in bm_core/network/l2.c.
#define VIRTUAL_PORT_ETH_HDR_LEN     14

/// IPv6 MTU (maximum IP payload size per Ethernet frame).
/// Matches ethernet_mtu in bm_core/network/bm_lwip.c.
#define VIRTUAL_PORT_ETH_MTU         1500

/// Maximum L2 frame length passed through the software stack.
/// FCS (4 bytes) is added/stripped by hardware and is never present here.
#define VIRTUAL_PORT_MAX_FRAME_LEN   (VIRTUAL_PORT_ETH_HDR_LEN + VIRTUAL_PORT_ETH_MTU)

/// Minimum valid L2 frame length (Ethernet header with no payload).
#define VIRTUAL_PORT_MIN_FRAME_LEN   VIRTUAL_PORT_ETH_HDR_LEN

/// Maximum total datagram length: port byte + max frame.
/// Both Linux and macOS default socket buffers greatly exceed this value.
#define VIRTUAL_PORT_MAX_DGRAM_LEN   (VIRTUAL_PORT_DGRAM_HDR_LEN + VIRTUAL_PORT_MAX_FRAME_LEN)

/// Minimum total datagram length: port byte + min frame.
#define VIRTUAL_PORT_MIN_DGRAM_LEN   (VIRTUAL_PORT_DGRAM_HDR_LEN + VIRTUAL_PORT_MIN_FRAME_LEN)

// -------------------------------------------------------------------------
// Wire-format accessor macros
// -------------------------------------------------------------------------

/// Extract the ingress port number from a received datagram buffer @p buf.
#define VIRTUAL_PORT_DGRAM_PORT(buf) \
    ((uint8_t)(((const uint8_t *)(buf))[VIRTUAL_PORT_DGRAM_PORT_OFF]))

/// Get a pointer to the start of the L2 frame inside datagram buffer @p buf.
#define VIRTUAL_PORT_DGRAM_FRAME_PTR(buf) \
    ((uint8_t *)(buf) + VIRTUAL_PORT_DGRAM_FRAME_OFF)

/// Compute the L2 frame length from the total received datagram length.
#define VIRTUAL_PORT_FRAME_LEN(dgram_len) \
    ((dgram_len) - VIRTUAL_PORT_DGRAM_HDR_LEN)

/// Compute the total datagram length to allocate given an L2 @p frame_len.
#define VIRTUAL_PORT_DGRAM_LEN(frame_len) \
    ((frame_len) + VIRTUAL_PORT_DGRAM_HDR_LEN)

/// Initialize the virtual-port network device.
/// @return 0 on success, non-zero on failure
int virtual_port_device_init(void);

