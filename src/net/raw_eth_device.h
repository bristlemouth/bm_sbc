#pragma once

/// @file raw_eth_device.h
/// @brief Bristlemouth NetworkDevice backed by raw Linux Ethernet interfaces.
///
/// Implements NetworkDeviceTrait for up to RAW_ETH_DEVICE_MAX_PORTS
/// interfaces, each mapped to one BM port (1-based). Uses AF_PACKET /
/// SOCK_RAW with all-multicast mode to receive BM frames.
///
/// This is a standalone device (no VirtualPortDevice composite). It is the
/// appropriate choice when the ADIN2111 is directly attached to the SBC and
/// exposes two Linux network interfaces (e.g. eth1, eth2).
///
/// Link state is tracked via /sys/class/net/<iface>/carrier, checked on
/// each retry_negotiation() call from bm_core's 100 ms renegotiation timer.

#include "network_device.h"
#include <stdint.h>

/// Hard cap: must not exceed RAW_ETH_MAX_PORTS in raw_eth_transport.h.
#define RAW_ETH_DEVICE_MAX_PORTS 2

/// Configuration for a single Ethernet port (one ADIN2111 T1L port).
typedef struct {
  char iface_name[16]; ///< Linux network interface name (e.g. "eth1").
} RawEthPortCfg;

/// Configuration for the raw Ethernet NetworkDevice.
typedef struct {
  RawEthPortCfg ports[RAW_ETH_DEVICE_MAX_PORTS];
  uint8_t       num_ports; ///< Number of active ports (1 or 2).
} RawEthDeviceCfg;

#ifdef __cplusplus
extern "C" {
#endif

/// Build and return a NetworkDevice backed by raw Ethernet sockets.
///
/// The configuration is copied; the caller's struct need not remain valid
/// after this call returns. The raw sockets are NOT opened here — they are
/// opened lazily by the NetworkDeviceTrait::enable() call made internally
/// by bm_l2_init().
///
/// @param cfg  Pointer to a populated RawEthDeviceCfg.
/// @return     Initialized NetworkDevice ready for bm_l2_init().
NetworkDevice raw_eth_device_get(const RawEthDeviceCfg *cfg);

#ifdef __cplusplus
}
#endif
