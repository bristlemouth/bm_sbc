#pragma once

/// @file gateway_device.h
/// @brief Composite network device that bridges VirtualPortDevice peers
///        with a UART L2 transport link.
///
/// The gateway device presents a single NetworkDevice to the Bristlemouth
/// stack. Ports 1–N are delegated to the underlying VirtualPortDevice,
/// and port N+1 is the UART link. Flooding (port 0) sends to all ports.
///
/// UART RX frames are delivered via callbacks->receive(uart_port, data, len).

#include "network_device.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Build and return a NetworkDevice that wraps @p vpd_dev plus the UART
/// transport (which must already be initialized via uart_l2_transport_init).
///
/// @param vpd_dev  Pointer to an initialized VirtualPortDevice NetworkDevice.
///                 The struct is copied by value; the pointer need not remain
///                 valid after this call returns.
/// @return         A fully initialized composite NetworkDevice.
NetworkDevice gateway_device_get(NetworkDevice *vpd_dev);

/// UART RX callback — pass this to uart_l2_transport_init() as the rx_cb.
/// It delivers received L2 frames to the Bristlemouth stack via the gateway
/// device's callbacks->receive() with the UART port number.
void gateway_uart_rx_cb(const uint8_t *frame, size_t len, void *ctx);

#ifdef __cplusplus
}
#endif
