#pragma once

/// @file uart_l2_transport.h
/// @brief Raw L2 Ethernet frame tunnel over UART.
///
/// Transports complete BM L2 Ethernet frames over UART using
/// COBS + length + CRC framing. No app-layer translation;
/// preserves BCMP/middleware/L2 transparency.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Callback invoked by the RX thread when a complete, valid L2 frame
/// has been received and decoded from the UART link.
/// @param frame  Pointer to the decoded L2 Ethernet frame.
/// @param len    Length of the frame in bytes.
/// @param ctx    User-provided context pointer.
typedef void (*uart_l2_rx_cb)(const uint8_t *frame, size_t len, void *ctx);

/// Initialize the UART L2 transport.
///
/// Opens the serial device, configures it for raw mode (8N1, no flow
/// control), starts a background RX thread, and prepares TX.
///
/// @param device_path  UART device path (e.g. "/dev/ttyUSB0")
/// @param baud_rate    Baud rate (e.g. 115200)
/// @param rx_cb        Callback for received L2 frames (may be NULL)
/// @param rx_ctx       Context pointer passed to @p rx_cb
/// @return 0 on success, -1 on failure
int uart_l2_transport_init(const char *device_path, int baud_rate,
                           uart_l2_rx_cb rx_cb, void *rx_ctx);

/// Send an L2 frame over the UART link.
///
/// Encodes the frame using the wire protocol (COBS + length + CRC-32C)
/// and writes it to the serial port.
///
/// @param l2_frame  The raw L2 Ethernet frame to send.
/// @param l2_len    Length of the frame in bytes.
/// @return 0 on success, -1 on failure.
int uart_l2_send(const uint8_t *l2_frame, size_t l2_len);

/// Stop the UART transport and close the serial port.
void uart_l2_transport_deinit(void);

#ifdef __cplusplus
}
#endif
