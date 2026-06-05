#pragma once
#ifdef __linux__

/// @file eth_tunnel_device.h
/// @brief Composite NetworkDevice: raw Ethernet ports + UDP tunnel peers.
///
/// Presents both ADIN2111 Ethernet ports (via raw_eth_device) and UDP tunnel
/// peers (via udp_tunnel_device) as a single NetworkDevice to bm_core.
///
/// Port numbering (1-based):
///   Ports 1..eth_ports       → raw Ethernet (ADIN2111)
///   Ports eth_ports+1..total → UDP tunnel peers
///
/// Use this when the Pi has an ADIN2111 connected to the BM physical network
/// AND needs to relay BM traffic to a remote peer (e.g. a laptop over
/// Tailscale).

#include "network_device.h"
#include "raw_eth_device.h"
#include "udp_tunnel_device.h"

typedef struct {
  RawEthDeviceCfg    eth;
  UdpTunnelDeviceCfg tunnel;
} EthTunnelDeviceCfg;

#ifdef __cplusplus
extern "C" {
#endif

/// Build and return a composite NetworkDevice backed by raw Ethernet + UDP
/// tunnel transports.  Neither transport is opened here — they are opened
/// lazily by the NetworkDeviceTrait::enable() call inside bm_l2_init().
///
/// @param cfg  Pointer to a populated EthTunnelDeviceCfg.
/// @return     Initialized NetworkDevice ready for bm_l2_init().
NetworkDevice eth_tunnel_device_get(const EthTunnelDeviceCfg *cfg);

#ifdef __cplusplus
}
#endif

#endif // __linux__
