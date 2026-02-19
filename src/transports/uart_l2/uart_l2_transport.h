#pragma once

/// @file uart_l2_transport.h
/// @brief Raw L2 Ethernet frame tunnel over UART.
///
/// Transports complete BM L2 Ethernet frames over UART using
/// COBS + length + CRC framing. No app-layer translation;
/// preserves BCMP/middleware/L2 transparency.

/// Initialize the UART L2 transport.
/// @param device_path UART device path (e.g. "/dev/ttyAMA0")
/// @param baud_rate   baud rate for the UART link
/// @return 0 on success, non-zero on failure
int uart_l2_transport_init(const char *device_path, int baud_rate);

