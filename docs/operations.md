# Operations

## CLI reference

```
bm_sbc_<app> --init <toml> [overrides...]
bm_sbc_<app> --node-id <hex64> [options...]
```

Full option list:

```
bm_sbc_<app> --node-id <hex64> [--init <toml>] [--cfg-dir <path>]
             [--peer <hex64>]... [--socket-dir <path>]
             [--uart <device>] [--baud <rate>] [--pcap <path>]
             [--log-dir <path>] [--log-level <level>] [--log-stdout]
```

| Flag            | Required | Default              | Description                                           |
|-----------------|----------|----------------------|-------------------------------------------------------|
| `--init`        | no       |                      | TOML init file; provides all settings below.          |
| `--node-id`     | yes*     |                      | 64-bit node ID in hex (e.g. `0x0001`). *Required if not set by `--init`. |
| `--cfg-dir`     | no       |                      | Directory for config partition files.                 |
| `--peer`        | no       |                      | Peer node ID. Repeat for each peer.                   |
| `--socket-dir`  | no       | `/tmp`               | Directory for Unix domain sockets.                    |
| `--uart`        | no       |                      | Serial device path. Enables gateway mode.             |
| `--baud`        | no       | `115200`             | UART baud rate.                                       |
| `--pcap`        | no       |                      | Write captured L2 frames to a pcap file.              |
| `--log-dir`     | no       | `/var/log/bm_sbc`    | Directory for log files.                              |
| `--log-level`   | no       | `info`               | Minimum log level: `trace`/`debug`/`info`/`warn`/`error`/`fatal`. |
| `--log-stdout`  | no       | false (true if TTY)  | Also write logs to stdout.                            |

CLI flags override values from the init file. The init file is loaded first; any flag supplied on the command line takes precedence.

## Init file (TOML)

All settings can be provided via a TOML file passed with `--init`. Example files are in `examples/`. TOML keys mirror the CLI flag names (hyphens preserved):

```toml
node-id    = "0x0000000000000001"
cfg-dir    = "/tmp/bm_node1"
socket-dir = "/tmp"
peers      = ["0x0000000000000002"]

# UART gateway (optional)
# uart-device = "/dev/ttyUSB0"
# uart-baud   = 115200

# Logging (all optional)
# log-dir    = "/var/log/bm_sbc"
# log-level  = "info"
# log-stdout = false
```

See `examples/node1.toml` and `examples/node2.toml` for working examples.

## Environment variables

Log settings can also be seeded from environment variables. CLI flags and TOML values override them.

| Variable              | Description                                              |
|-----------------------|----------------------------------------------------------|
| `BM_SBC_LOG_DIR`      | Log file directory (same as `--log-dir`).                |
| `BM_SBC_LOG_LEVEL`    | Minimum log level name (same as `--log-level`).          |
| `BM_SBC_LOG_STDOUT`   | Set to `1` to tee logs to stdout (same as `--log-stdout`).|

## Modes

**Normal mode** (no `--uart`): process communicates with peers over Unix
domain sockets in `--socket-dir`. Each `--peer` gets a virtual BM port
(1-indexed, in CLI order).

**Gateway mode** (`--uart` given): same as normal, plus a UART port is
appended as the highest-numbered port. Frames are bridged between local
peers and the UART link transparently.

## Topology

Topology is static and fully specified at launch via `--peer` flags (or
`peers` in the init file). There is no dynamic peer discovery between
processes.

Each process binds a socket at:
```
<socket-dir>/bm_sbc_<node_id_hex16>.sock
```

Two processes are neighbors if and only if each lists the other as a peer.

Port assignment is deterministic: the Nth peer maps to virtual port N.

## Limits

| Limit                  | Value | Notes                                    |
|------------------------|-------|------------------------------------------|
| Max peers per process  | 15    | BM protocol 4-bit port nibble constraint |
| Max `--peer` flags     | 16    | 16th triggers truncation warning; 15 used|
| Socket path length     | 108   | `sun_path` limit; socket-dir must be short|
| Max L2 frame           | 1514  | 14-byte Ethernet header + 1500-byte MTU  |

## Logging

Log output is written to a per-process file:
```
<log-dir>/<app_name>_<node_id_hex16>.log
```

Default directory: `/var/log/bm_sbc`. If the directory cannot be created or
the file cannot be opened, the process falls back to stdout-only and prints a
warning to stderr.

Log line format:
```
2024-01-15T12:34:56.789012Z INFO  [multinode node=0x0000000000000001] stack initialized
```

**Log rotation**: send `SIGHUP` to reopen the log file. Useful with
`logrotate`.

When stdout is a TTY (interactive shell), logs are also written to stdout
automatically. Use `--log-stdout` to force this in non-TTY contexts.

## Diagnostics

Key patterns to search for in log output:

| Pattern                              | Meaning                              |
|--------------------------------------|--------------------------------------|
| `stack initialized`                  | BM startup sequence completed OK     |
| `startup sequence failed err=N`      | Fatal: stack init failed             |
| `NEIGHBOR_UP node=<id>`              | Peer discovered (multinode app)      |
| `NEIGHBOR_DOWN node=<id>`            | Peer lost (multinode app)            |
| `PUBSUB_RX from=<id>`               | Pub/sub message received             |
| `bcmp_seq=`                          | Ping reply received                  |
| `vpd: peer count N exceeds cap 15`  | Peer list was truncated              |
| `UART transport init failed`         | Serial port open/config failed       |
| `err: N at <file>:<line>`            | bm_core internal error               |
| `pcap capture ->`                    | pcap capture is active               |

## Stopping

Send SIGTERM or SIGINT. There is no graceful shutdown sequence; the
process exits and the OS cleans up sockets and file descriptors.

<!-- TODO: Add graceful shutdown if needed in the future. -->
