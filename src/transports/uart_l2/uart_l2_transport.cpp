#include "uart_l2_transport.h"

// TODO: Raw framed UART TX/RX (COBS + length + CRC)
// TODO: Integrate with gateway network-device path

int uart_l2_transport_init(const char *device_path, int baud_rate) {
  (void)device_path;
  (void)baud_rate;
  // Placeholder – will be implemented in milestone 4
  return 0;
}

