# Gateway IPC

Local clients (Hydrotwin, tools) send commands to a running `bm_sbc_gateway`
over a Unix-domain `SOCK_DGRAM` socket.
Each datagram is one self-contained CBOR map.
There are no replies — the socket is one-way client → gateway.

## Python client

A reference client lives in `clients/python/bm_sbc_gateway/`,
with one helper per message type
(`config_set`, `replay_caught_up`, `sensor_data`, `spotter_log`, `spotter_tx`)
and a `Client` class for callers that want to keep one socket open.
The helpers handle CBOR encoding and the `v=1` envelope.

## Transport

- **Socket path:** `/run/bm_sbc/gateway_ipc.sock`
  (override with `BM_SBC_GATEWAY_IPC` for tests).
- **Type:** `AF_UNIX` / `SOCK_DGRAM`, non-blocking, world-writable (`0666`).
  Access control is expected at the directory level.
- **Encoding:** CBOR (definite-length maps).
- **Max datagram:** 4096 bytes.

## Envelope

Every message is a CBOR map with two required keys plus message-specific
fields:

| key    | type    | value                          |
| ------ | ------- | ------------------------------ |
| `v`    | integer | Schema version. Currently `1`. |
| `type` | text    | Message type (see below).      |

Unknown `type` values are logged and dropped.
Malformed datagrams (bad CBOR, missing `v`, wrong schema version) are dropped.

## Messages

### `replay_caught_up`

Signal that processing of replayed audio has caught up to realtime.
The gateway will signal `cobs_to_shm` to stop.
Then Hydrotwin or other clients should exit cleanly upon ingesting EOF.
The gateway will request a poweroff service ack from the mote,
then run `systemctl poweroff` once acknowledged.

No additional fields.

### `spotter_log`

Append a line to the Spotter on-board log.

| key               | type    | required | notes                          |
| ----------------- | ------- | -------- | ------------------------------ |
| `data`            | text    | yes      | Log line. Max 1024 bytes.      |
| `file_name`       | text    | no       | Target log file. Max 63 bytes. |
| `print_timestamp` | boolean | no       | Default `false`.               |

### `spotter_tx`

Transmit a payload over the Spotter satellite link.

| key                | type    | required | notes                                    |
| ------------------ | ------- | -------- | ---------------------------------------- |
| `data`             | bytes   | yes      | Payload to transmit.                     |
| `iridium_fallback` | boolean | no       | `true` ⇒ cellular with Iridium fallback. |

### `sensor_data`

Publish sensor data on the Bristlemouth pub/sub network.
Published topic is `sensor/<node_id_hex16>/<topic_suffix>`.

| key            | type  | required | notes                                            |
| -------------- | ----- | -------- | ------------------------------------------------ |
| `topic_suffix` | text  | yes      | Must not begin with `/`. Bounded by total ≤ 255. |
| `data`         | bytes | yes      | Payload bytes.                                   |

### `config_set`

Write a key/value into the local system config partition
(`BM_CFG_PARTITION_SYSTEM`) and persist to disk.

| key            | type                      | required | notes                    |
| -------------- | ------------------------- | -------- | ------------------------ |
| `config_key`   | text                      | yes      | Max 32 bytes, non-empty. |
| `config_value` | text / uint / int / float | yes      | Type inferred from CBOR. |

Type mapping is taken directly from the CBOR wire type:

| CBOR type        | Stored as |
| ---------------- | --------- |
| text string      | `STR`     |
| unsigned integer | `UINT32`  |
| negative integer | `INT32`   |
| float / double   | `FLOAT`   |

Limits:

- Strings ≤ 48 bytes
  (the backing CBOR value buffer is 50 bytes and length-prefix takes 1–2).
- `UINT32` values must fit in `uint32_t`; `INT32` in `int32_t`.
- Floats are stored as `float` regardless of source precision.

Successful writes are persisted via `save_config(BM_CFG_PARTITION_SYSTEM)`
before the handler returns.

## Testing

`apps/ipc_test` runs only the IPC listener (no UART, no mote)
and is what `scripts/gateway_ipc_test.sh` exercises against a Python client.
