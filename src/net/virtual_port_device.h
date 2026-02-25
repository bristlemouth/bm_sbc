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

#include <inttypes.h>     // PRIx64 (also pulls in stdint.h)
#include "network_device.h" // NetworkDevice, NetworkDeviceTrait, NetworkDeviceCallbacks

/// Maximum number of directly-connected peers per process.
#define VIRTUAL_PORT_MAX_PEERS 15

/// Size of a complete Unix-domain socket path buffer (Linux UNIX_PATH_MAX).
/// sun_path is 104 bytes on macOS and 108 bytes on Linux; using 108 here.
/// Callers that need strict portability should compare against the platform's
/// actual sun_path size from <sys/un.h>.
#define VIRTUAL_PORT_SOCK_PATH_LEN 108

/// Maximum number of characters in the socket directory path (excluding NUL).
/// Derived from VIRTUAL_PORT_SOCK_PATH_LEN minus the fixed-length filename
/// suffix written by VIRTUAL_PORT_SOCK_FMT:
///   '/' (1) + "bm_sbc_" (7) + 16 hex digits (16) + ".sock" (5) + NUL (1) = 30
#define VIRTUAL_PORT_SOCK_DIR_MAX  (VIRTUAL_PORT_SOCK_PATH_LEN - 30)

/// Socket filename template.  Caller must supply a VIRTUAL_PORT_SOCK_PATH_LEN
/// byte buffer.
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

// -------------------------------------------------------------------------
// Peer discovery — static CLI topology (Milestone 3)
//
// Topology is fully specified at launch; no dynamic rendezvous occurs.
//
// CLI flags parsed by bm_sbc_runtime_init():
//
//   --node-id    <hex64>    This process's 64-bit Bristlemouth node ID.
//                           Required; fatal error if absent or malformed.
//
//   --peer       <hex64>    A peer's node ID (bare hex or 0x-prefixed).
//                           Repeatable 0–VIRTUAL_PORT_MAX_PEERS times.
//                           Insertion order determines port-slot assignment:
//                             1st --peer  →  virtual port 1
//                             2nd --peer  →  virtual port 2
//                             …
//                             15th --peer →  virtual port 15
//                           A 16th --peer is a fatal CLI error.
//
//   --socket-dir <path>    Directory used for socket files.
//                           Optional; defaults to VIRTUAL_PORT_DEFAULT_SOCKET_DIR.
//                           Must be ≤ VIRTUAL_PORT_SOCK_DIR_MAX characters.
// -------------------------------------------------------------------------

/// Default directory for Unix-domain socket files.
#define VIRTUAL_PORT_DEFAULT_SOCKET_DIR "/tmp"

/// Static peer-topology configuration passed to virtual_port_device_get().
///
/// Populated by bm_sbc_runtime_init() from CLI arguments (see above).
/// All fields are copied into the device's internal state by
/// virtual_port_device_get(), so the struct need not remain valid after
/// that call returns.
typedef struct {
  /// This process's 64-bit Bristlemouth node ID (from --node-id).
  uint64_t own_node_id;

  /// Directory used for socket files (from --socket-dir, or the default).
  /// Must be NUL-terminated and ≤ VIRTUAL_PORT_SOCK_DIR_MAX characters.
  char socket_dir[VIRTUAL_PORT_SOCK_DIR_MAX + 1];

  /// Peer node IDs in port-slot order (from --peer flags, in order given).
  /// peer_ids[0] → virtual port 1, peer_ids[1] → virtual port 2, …
  uint64_t peer_ids[VIRTUAL_PORT_MAX_PEERS];

  /// Number of valid entries in peer_ids[].  0–VIRTUAL_PORT_MAX_PEERS.
  uint8_t num_peers;
} VirtualPortCfg;

/// Build and return a NetworkDevice backed by Unix-domain SOCK_DGRAM IPC.
///
/// @param cfg  Caller-owned static topology configuration.  All data is
///             copied internally; the pointer need not remain valid after
///             this call returns.
///
/// @return     Fully initialized NetworkDevice ready to pass to bm_l2_init().
///             The device is not yet enabled; bm_l2_init() will invoke
///             trait->enable() as part of its bring-up sequence.
///
/// Implemented in tasks 2a–2j.
NetworkDevice virtual_port_device_get(const VirtualPortCfg *cfg);

