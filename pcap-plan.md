# Plan: Add pcap capture for Bristlemouth traffic

## Context

For debugging, we want to capture all Bristlemouth L2 frames (TX and RX) as a pcap stream. This capability should be generic enough to work on embedded targets too (like `bm_protocol`'s serial-based `pcap.c`), not just Linux. The `bm_sbc` multinode app gets a `--pcap <path>` CLI flag as one concrete sink.

## Architecture: separate concerns across bm_core and bm_sbc

### bm_core — generic pcap formatting + L2 tap (portable, no OS assumptions)

Two pieces go into bm_core:

**1. Pcap stream formatter** (`common/pcap.h` / `common/pcap.c`)

Replaces the bm_protocol version with a sink-agnostic design. Instead of hardcoding `serialWrite`, the caller provides a write callback:

```c
typedef void (*PcapWriteCb)(const uint8_t *data, size_t len, void *ctx);

void pcap_init(PcapWriteCb write_cb, void *ctx);
void pcap_write_packet(const uint8_t *frame, size_t len);
```

- `pcap_init` stores the callback and immediately writes the 24-byte global header (magic `0xA1B2C3D4`, v2.4, `LINKTYPE_ETHERNET=1`, snaplen 65535) through it.
- `pcap_write_packet` builds a `PcapRecordHeader_t` (timestamp from `bm_get_tick_count()` + `bm_ticks_to_ms()`, same approach as bm_protocol's `xTaskGetTickCount`) and writes header + frame through the callback.
- Structs `PcapHeader_t` and `PcapRecordHeader_t` match the existing bm_protocol definitions.
- No file I/O, no serial, no mutexes — the sink owns thread safety.

**2. L2 pcap tap** (addition to `network/l2.c` / `network/l2.h`)

Register a packet observer callback at the L2 layer, similar to the existing `bm_l2_register_link_change_callback` pattern:

```c
typedef void (*L2PcapCb)(const uint8_t *frame, size_t len);

BmErr bm_l2_register_pcap_callback(L2PcapCb cb);
```

Hook points in `l2.c`:
- **RX**: in `bm_l2_rx()` (line 250) — call the callback with the raw frame before queuing
- **TX**: in `send_to_port()` (line 327) and `send_global_multicast_packet()` (line 335) — call the callback with the frame before calling `trait->send`

This keeps the tap inside L2 where all traffic converges regardless of transport (VPD, UART, future transports). Embedded targets can register their own callback (e.g. writing to serial like bm_protocol does today).

### bm_sbc — Linux file sink + CLI wiring

**1. Pcap file sink** (`src/core/pcap_file_sink.h` / `src/core/pcap_file_sink.cpp`)

Linux-specific: opens a file, provides the `PcapWriteCb` that writes to it.

- `pcap_file_sink_open(const char *path)` — opens file for writing, calls `pcap_init()` with its write callback
- The write callback: `pthread_mutex_t`-protected `fwrite` to the open file handle (needed because RX and TX callbacks come from different threads)
- `pcap_file_sink_close()` — flushes and closes

**2. CLI integration** (modify `src/core/runtime.cpp`)

- Add `--pcap <path>` to `getopt_long` options and usage string
- Parse into `char pcap_path[256]`
- After `bm_l2_init` (so L2 context exists), if pcap_path is set:
  - Call `pcap_file_sink_open(pcap_path)`
  - Call `bm_l2_register_pcap_callback(pcap_write_packet)` — the L2 tap feeds frames into the pcap formatter, which writes through the file sink

**3. Build** (modify `CMakeLists.txt`)

Add `src/core/pcap_file_sink.cpp` to `bm_sbc_core` sources. The bm_core files (`pcap.c`, updated `l2.c`) come in via the submodule.

## Data flow

```
L2 RX/TX
  │
  ├─ bm_l2_rx() / send_to_port()
  │    │
  │    └─ L2PcapCb callback (registered via bm_l2_register_pcap_callback)
  │         │
  │         └─ pcap_write_packet()          [bm_core: formats pcap record]
  │              │
  │              └─ PcapWriteCb(data, len)   [sink-specific: file, serial, etc.]
  │
  └─ normal L2 processing continues
```

## Files changed

| Layer | File | Change |
|-------|------|--------|
| bm_core | `common/pcap.h` (new) | `PcapWriteCb` typedef, `pcap_init`, `pcap_write_packet` |
| bm_core | `common/pcap.c` (new) | Pcap header/record formatting, callback-based write |
| bm_core | `network/l2.h` | Add `L2PcapCb` typedef, `bm_l2_register_pcap_callback` |
| bm_core | `network/l2.c` | Add pcap callback field to `BmL2Ctx`, call it in RX + TX paths |
| bm_core | `common/CMakeLists.txt` | Add `pcap.c` |
| bm_sbc | `src/core/pcap_file_sink.h` (new) | `pcap_file_sink_open`, `pcap_file_sink_close` |
| bm_sbc | `src/core/pcap_file_sink.cpp` (new) | File-backed `PcapWriteCb` with mutex |
| bm_sbc | `src/core/runtime.cpp` | `--pcap <path>` CLI option, wiring |
| bm_sbc | `CMakeLists.txt` | Add `pcap_file_sink.cpp` to `bm_sbc_core` |

## Key design decisions

- **Sink-agnostic core** — bm_core's `pcap.c` only formats records and pushes bytes through a callback. The same code works whether the sink is a Linux file, a FreeRTOS serial handle, a network socket, or a ring buffer.
- **L2 tap lives in bm_core** — hooks at the convergence point where all transports (VPD, UART, future) meet. Embedded targets get pcap capture for free.
- **Thread safety is the sink's responsibility** — bm_core's pcap formatter is stateless per-call (just formats and writes). The bm_sbc file sink adds a mutex. An embedded serial sink might use a different mechanism.
- **Timestamps via `bm_get_tick_count()` + `bm_ticks_to_ms()`** — portable across FreeRTOS and POSIX, same approach as bm_protocol's existing pcap.c.
- **No libpcap dependency** — the pcap file format is trivial to write directly.
- **Compatible with proto_bcmp.lua** — the captured frames are Ethernet/IPv6/BCMP, so Wireshark with the existing dissector plugin will decode them.

## Verification

1. Build: `cmake -DAPP=multinode -B build && cmake --build build`
2. Run with pcap: `./build/bm_sbc_multinode --node-id 1 --peer 2 --pcap /tmp/test.pcap`
3. Open `/tmp/test.pcap` in Wireshark with proto_bcmp.lua loaded — should show decoded Bristlemouth/BCMP traffic
4. Run without `--pcap` — no regression, no pcap file created
