# UART Gateway

## Overview

Gateway mode bridges local Unix-socket peers with an external Bristlemouth
network over a serial link. The UART port appears as one additional BM port
on the gateway process's network device. Frames pass through transparently
-- no protocol translation.

## Usage

```
./build/bm_sbc_multinode \
  --node-id 0x0000000000000001 \
  --peer 0x0000000000000002 \
  --uart /dev/ttyUSB0 \
  --baud 115200
```

This creates a gateway with:
- Virtual port 1: local peer `0x0002`
- Virtual port 2: UART link

The UART port number is always `(number of VPD peers) + 1`.

## Wire format

```
[COBS-encoded payload] [0x00 delimiter]
```

Payload (before COBS encoding):
```
[len_hi] [len_lo] [L2 frame ...] [CRC-32C, 4 bytes, big-endian]
```

- Length: 2-byte big-endian, equals the L2 frame size.
- CRC-32C (Castagnoli): computed over length + L2 frame bytes.
- COBS ensures no `0x00` in the encoded payload, so `0x00` is an
  unambiguous frame delimiter.
- Serial config: 8N1, no flow control, raw mode.

Supported baud rates: 9600, 19200, 38400, 57600, 115200, 230400.

## Loopback test (no hardware)

Uses `socat` to create a virtual null-modem:

```
./scripts/gateway_loopback_test.sh
```

Requires `socat` (`brew install socat` on macOS).

Two gateway processes connect over linked PTYs and validate neighbor
discovery and ping across the UART link.

## Hardware connection

<!-- TODO: Fill in after validating with dev kit hardware.
     Topics to cover:
     - Which UART pins / connector on the dev kit
     - Required USB-serial adapter (if any)
     - Wiring diagram or pinout
     - Baud rate confirmed to work reliably
     - Any observed latency or throughput limits
     - Steps to verify end-to-end topology (SBC nodes visible from
       hardware-side CLI)
-->

Not yet validated on physical hardware. The loopback test confirms the
software path works end-to-end. Hardware validation is tracked separately.

