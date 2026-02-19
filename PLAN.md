# `bm_sbc` Project Plan

## 1. Scope

`bm_sbc` is a Linux SBC Bristlemouth runtime (first target: Raspberry Pi Zero 2W, 64-bit) that:

- Integrates `bm_core` with CMake in a `bm_protocol`-style integration flow.
- Runs multiple concurrent processes, each with a unique Bristlemouth node ID.
- Supports local BCMP + middleware communication between processes.
- Supports off-SBC Bristlemouth connectivity via raw L2 frame tunnel over UART.
- Uses no `lwip` and no `bm_serial`.

## 2. Decisions

1. IP stack integration name in `bm_core`:
- Use `LINUX` (not `LINUX_NATIVE`).

2. Neighbor model:
- Use per-peer virtual ports (strict one-link-per-port semantics).
- Enforce max 15 neighbors per process (hard cap).
- No v1 overflow tree/multi-hop overlay design.

3. Project naming:
- Project/repo family: `bm_sbc`.
- Linux platform backend naming: `LINUX`.

## 3. Naming and Structure

### 3.1 Top-Level Naming

- CMake project: `bm_sbc`
- Core library target: `bm_sbc_core`
- Optional app runtime helper target: `bm_sbc_app`
- Default node executable name: `bm_sbc_node`

### 3.2 Directory Layout

- `lib/bm_core/` (submodule)
- `src/core/` (stack runtime glue, boot sequence, shared services)
- `src/platform/linux/` (OS + IP integration wrappers and Linux-specific hooks)
- `src/net/` (local virtual-port fabric + network device implementation)
- `src/transports/uart_l2/` (raw L2 UART tunnel transport)
- `apps/<app_name>/` (small app entrypoints and user code)
- `docs/` (integration notes, operational guide, limits)

## 4. `bm_core` Integration Plan

Because upstream currently provides `LWIP` and `FREERTOS` platform files:

- Add `setup_bm_os(POSIX ...)` implementation in `bm_core/common` for `bm_os.h` APIs.
- Add `setup_bm_ip_stack(LINUX ...)` implementation in `bm_core/network` for `bm_ip.h` APIs.

Root integration flow (in `bm_sbc` CMake):

- include `lib/bm_core/cmake/bm_core.cmake`
- call `setup_bm_ip_stack(LINUX "...includes...")`
- call `setup_bm_os(POSIX "...includes...")`
- add `lib/bm_core` as subdirectory and link `bmcore`

## 5. App Model (separate executables, preferred)

Each app remains small (Arduino-like), while stack logic lives in shared core:

- App contract:
  - `void setup(void);`
  - `void loop(void);`
- Shared app runner in `bm_sbc_core`:
  - Calls `setup()` once.
  - Calls `loop()` repeatedly on a scheduler-friendly cadence.

Build selection:

- `-DBM_SBC_APP=<app_name>` builds one app executable.
- `-DBM_SBC_BUILD_ALL_APPS=ON` builds all in-tree apps.
- Optional phase 2: `-DBM_SBC_APP_PATH=/abs/path/to/external/app`.

## 6. Local Networking Model (Per-Peer Virtual Ports)

### 6.1 Port Assignment

- Each process maintains a peer table mapping neighbor node ID -> virtual BM port (1-15).
- Port 0 remains flood/all-ports behavior per trait contract.
- Deterministic assignment strategy required (stable under restarts/churn as much as feasible).

### 6.2 Cap Behavior

When >15 direct neighbors are discovered:

- New neighbor is rejected locally for direct-link modeling.
- Emit clear diagnostics/metrics for operator visibility.
- Keep existing mapped peers unchanged (no cascading remaps unless explicitly designed).

This keeps behavior explicit and spec-aligned with current port model assumptions.

## 7. UART Uplink (Raw L2 Tunnel)

### 7.1 Wire Behavior

- Transport complete BM L2 Ethernet frames over UART.
- Framing: COBS + length + CRC.
- No app-layer translation; preserve BCMP/middleware/L2 transparency.

### 7.2 Gateway Role

- Gateway process exposes:
  - Local virtual-port fabric side.
  - UART tunnel side (another BM port).
- Hardware nodes discover topology through gateway propagation using native BCMP/topology flows.

## 8. Core Components

1. Runtime bootstrap (`src/core/`):
- CLI parsing (`--node-id`, storage paths, gateway flags, UART params).
- `DeviceCfg` initialization.
- Bristlemouth startup sequence:
  - `bm_l2_init(custom_network_device)`
  - `bm_ip_init()`
  - `bcmp_init(custom_network_device)`
  - `topology_init(...)`
  - `bm_service_init()`
  - `bm_middleware_init(...)`

2. Platform wrappers (`src/platform/linux/`):
- Config partition read/write/reset wrapper.
- RTC wrapper.
- DFU wrapper (safe stub initially, expand later if needed).

3. Network device (`src/net/`):
- Implements `NetworkDeviceTrait`.
- Supports per-peer virtual ports over local IPC.

4. Transport (`src/transports/uart_l2/`):
- Raw framed UART TX/RX.
- Integrates with gateway network-device path.

## 9. Validation Plan

### 9.1 Local Multiprocess

- Start N processes with distinct node IDs.
- Validate:
  - neighbor discovery,
  - BCMP ping/info/config,
  - middleware pub/sub,
  - topology correctness.
- Validate strict 15-neighbor cap behavior and logs.

### 9.2 Gateway + Hardware

- Start gateway + local peers.
- Connect UART tunnel to dev kit endpoint.
- Validate hardware-side topology includes SBC-hosted nodes as expected.

## 10. Milestones

1. Bootstrap `bm_sbc` repo skeleton, CMake, naming, and app-selection flow.
2. Add `POSIX` + `LINUX` integrations in `bm_core` and build native on Pi.
3. Implement local per-peer virtual-port network device and multiprocess validation.
4. Implement raw L2 UART tunnel and gateway validation.
5. Harden docs, diagnostics, and operator guidance.
