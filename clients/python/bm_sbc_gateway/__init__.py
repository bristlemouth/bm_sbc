"""Fire-and-forget client for the bm_sbc_gateway IPC socket.

The gateway binds a Unix-domain `SOCK_DGRAM` listener at
`/run/bm_sbc/gateway_ipc.sock` and accepts CBOR-encoded datagrams matching
the v1 schema documented in the gateway app. This module wraps each supported
message type as a single function call.

Example::

    from bm_sbc_gateway import sensor_data, spotter_tx, spotter_log

    sensor_data("temperature", cbor2.dumps({"t_c": 21.4}))
    spotter_tx(payload_bytes, iridium_fallback=True)
    spotter_log("boot complete", file_name="system.log", print_timestamp=True)
"""

from __future__ import annotations

import os
import socket
from typing import Any, Optional

import cbor2

__all__ = [
    "DEFAULT_SOCKET_PATH",
    "SCHEMA_VERSION",
    "Client",
    "replay_caught_up",
    "sensor_data",
    "spotter_log",
    "spotter_tx",
]

DEFAULT_SOCKET_PATH = "/run/bm_sbc/gateway_ipc.sock"
SCHEMA_VERSION = 1


class Client:
    """Reusable client holding a connected `SOCK_DGRAM` socket.

    Prefer a single `Client` instance over the module-level helpers when
    sending many messages — it avoids re-opening the socket every call.
    """

    def __init__(self, socket_path: str = DEFAULT_SOCKET_PATH) -> None:
        self._path = socket_path
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)

    def close(self) -> None:
        self._sock.close()

    def __enter__(self) -> "Client":
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.close()

    def _send(self, message: dict[str, Any]) -> None:
        message["v"] = SCHEMA_VERSION
        self._sock.sendto(cbor2.dumps(message), self._path)

    def replay_caught_up(self) -> None:
        self._send({"type": "replay_caught_up"})

    def spotter_log(
        self,
        data: str,
        file_name: Optional[str] = None,
        print_timestamp: Optional[bool] = None,
    ) -> None:
        msg: dict[str, Any] = {"type": "spotter_log", "data": data}
        if file_name is not None:
            msg["file_name"] = file_name
        if print_timestamp is not None:
            msg["print_timestamp"] = print_timestamp
        self._send(msg)

    def spotter_tx(
        self,
        data: bytes,
        iridium_fallback: Optional[bool] = None,
    ) -> None:
        msg: dict[str, Any] = {"type": "spotter_tx", "data": data}
        if iridium_fallback is not None:
            msg["iridium_fallback"] = iridium_fallback
        self._send(msg)

    def sensor_data(self, topic_suffix: str, data: bytes) -> None:
        if topic_suffix.startswith("/"):
            raise ValueError(
                "topic_suffix must not begin with '/'; the gateway inserts "
                "the separator between the node ID and the suffix"
            )
        self._send({"type": "sensor_data", "topic_suffix": topic_suffix, "data": data})


_default_client_instance: Optional[Client] = None


def _default_client() -> Client:
    global _default_client_instance
    if _default_client_instance is None:
        path = os.environ.get("BM_SBC_GATEWAY_IPC", DEFAULT_SOCKET_PATH)
        _default_client_instance = Client(path)
    return _default_client_instance


def replay_caught_up() -> None:
    _default_client().replay_caught_up()


def spotter_log(
    data: str,
    file_name: Optional[str] = None,
    print_timestamp: Optional[bool] = None,
) -> None:
    _default_client().spotter_log(data, file_name, print_timestamp)


def spotter_tx(data: bytes, iridium_fallback: Optional[bool] = None) -> None:
    _default_client().spotter_tx(data, iridium_fallback)


def sensor_data(topic_suffix: str, data: bytes) -> None:
    _default_client().sensor_data(topic_suffix, data)
