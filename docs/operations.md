# Operations

## CLI reference

```
bm_sbc_<app> --node-id <hex64> [--peer <hex64>]... [--socket-dir <path>]
             [--uart <device>] [--baud <rate>]
```

| Flag           | Required | Default    | Description                              |
|----------------|----------|------------|------------------------------------------|
| `--node-id`    | yes      |            | 64-bit node ID in hex (e.g. `0x0001`)    |
| `--peer`       | no       |            | Peer node ID. Repeat for each peer.      |
| `--socket-dir` | no       | `/tmp`     | Directory for Unix domain sockets.       |
| `--uart`       | no       |            | Serial device path. Enables gateway mode.|
| `--baud`       | no       | `115200`   | UART baud rate.                          |

## Modes

**Normal mode** (no `--uart`): process communicates with peers over Unix
domain sockets in `--socket-dir`. Each `--peer` gets a virtual BM port
(1-indexed, in CLI order).

**Gateway mode** (`--uart` given): same as normal, plus a UART port is
appended as the highest-numbered port. Frames are bridged between local
peers and the UART link transparently.

## Topology

Topology is static and fully specified at launch via `--peer` flags.
There is no dynamic peer discovery between processes.

Each process binds a socket at:
```
<socket-dir>/bm_sbc_<node_id_hex16>.sock
```

Two processes are neighbors if and only if each lists the other as a `--peer`.

Port assignment is deterministic: the Nth `--peer` flag maps to virtual
port N.

## Limits

| Limit                  | Value | Notes                                    |
|------------------------|-------|------------------------------------------|
| Max peers per process  | 15    | BM protocol 4-bit port nibble constraint |
| Max `--peer` flags     | 16    | 16th triggers truncation warning; 15 used|
| Socket path length     | 108   | `sun_path` limit; socket-dir must be short|
| Max L2 frame           | 1514  | 14-byte Ethernet header + 1500-byte MTU  |

## Diagnostics

All output goes to stdout via `bm_debug()` (which is `printf`). Key
strings to grep for:

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

## Stopping

Send SIGTERM or SIGINT. There is no graceful shutdown sequence; the
process exits and the OS cleans up sockets and file descriptors.

<!-- TODO: Add graceful shutdown if needed in the future. -->

