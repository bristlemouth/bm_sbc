#pragma once

/// @file virtual_port_device.h
/// @brief Per-peer virtual-port network device for local IPC.
///
/// Implements NetworkDeviceTrait with per-peer virtual ports
/// (strict one-link-per-port semantics, max 15 neighbors).

/// Initialize the virtual-port network device.
/// @return 0 on success, non-zero on failure
int virtual_port_device_init(void);

