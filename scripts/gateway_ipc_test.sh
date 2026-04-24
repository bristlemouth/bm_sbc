#!/usr/bin/env bash
# scripts/gateway_ipc_test.sh — gateway IPC validation
#
# Drives the bm_sbc_ipc_test binary (which binds the real gateway IPC
# listener) with the Python client and verifies that each v1 CBOR message
# type is parsed and routed correctly — checked by grepping the server log
# for the "IPC RX <type> ..." lines emitted by each handler.
#
# Requirements:
#   - python3 with cbor2 (pip install cbor2)
#   - A built bm_sbc_ipc_test binary
#
# Usage: ./scripts/gateway_ipc_test.sh [path/to/bm_sbc_ipc_test]

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="${1:-$REPO_ROOT/build/all/bm_sbc_ipc_test}"

if [[ ! -x "$BINARY" ]]; then
  echo "Binary not found: $BINARY"
  echo "Build with: cmake --preset all && cmake --build --preset all"
  exit 1
fi

# --- Python venv ------------------------------------------------------------
# Reuse a project-local venv so the test is self-contained and repeatable.
# Re-run is cheap: python -m venv is a no-op on an existing venv, and pip
# install -r requirements.txt short-circuits when everything is satisfied.
VENV_DIR="$REPO_ROOT/.venv"
REQ_FILE="$REPO_ROOT/clients/python/requirements.txt"

if [[ ! -d "$VENV_DIR" ]]; then
  echo "=== Creating Python venv at $VENV_DIR ==="
  python3 -m venv "$VENV_DIR"
fi
# shellcheck disable=SC1091
source "$VENV_DIR/bin/activate"
python -m pip install --quiet --disable-pip-version-check -r "$REQ_FILE"

WORK=$(mktemp -d /tmp/bm_sbc_ipc_test_XXXXXX)
SOCK_PATH="$WORK/gateway_ipc.sock"
LOG="$WORK/server.log"
PASS=0; FAIL=0

check() {
  local desc="$1" pattern="$2"
  if grep -qF "$pattern" "$LOG" 2>/dev/null; then
    echo "  PASS: $desc"
    PASS=$((PASS + 1))
  else
    echo "  FAIL: $desc"
    echo "        (pattern not found: '$pattern')"
    FAIL=$((FAIL + 1))
  fi
}

cleanup() {
  [[ -n "${SERVER_PID:-}" ]] && kill "$SERVER_PID" 2>/dev/null || true
  wait "$SERVER_PID" 2>/dev/null || true
  if [[ $FAIL -ne 0 ]]; then
    echo ""
    echo "--- server log ($LOG) ---"
    cat "$LOG" 2>/dev/null || true
  else
    rm -rf "$WORK"
  fi
}
trap cleanup EXIT

echo "=== Starting bm_sbc_ipc_test ==="
BM_SBC_LOG_STDOUT=1 BM_SBC_LOG_LEVEL=info \
  BM_SBC_GATEWAY_IPC="$SOCK_PATH" \
  "$BINARY" --node-id 0x0000000000000001 \
            --socket-dir "$WORK" \
            --log-dir "$WORK/logs" \
            >"$LOG" 2>&1 &
SERVER_PID=$!

# Wait until the server has bound the socket (up to ~3 s).
for _ in $(seq 1 30); do
  if [[ -S "$SOCK_PATH" ]]; then break; fi
  sleep 0.1
done

if [[ ! -S "$SOCK_PATH" ]]; then
  echo "FAIL: server did not bind $SOCK_PATH"
  exit 1
fi

echo "  Server up (PID $SERVER_PID), socket at $SOCK_PATH"
echo ""

echo "=== Sending messages via Python client ==="
PYTHONPATH="$REPO_ROOT/clients/python" \
BM_SBC_GATEWAY_IPC="$SOCK_PATH" \
python - <<'PY'
from bm_sbc_gateway import (
    replay_caught_up,
    sensor_data,
    spotter_log,
    spotter_tx,
)

replay_caught_up()
spotter_log("hello", file_name="boot.log", print_timestamp=True)
spotter_tx(b"\x01\x02\x03\x04", iridium_fallback=True)
sensor_data("temperature", b"\xaa\xbb\xcc")
# no-opt variants: make sure omitted-field path works too
spotter_log("no file, no ts")
spotter_tx(b"\x10\x20")
PY

# Give the server a moment to process all datagrams.
sleep 0.5
echo ""

echo "=== Checking decoded output ==="
check "replay_caught_up handled"            "IPC RX replay_caught_up"
check "spotter_log with file + timestamp"   "IPC RX spotter_log data_len=5 file_name='boot.log' print_timestamp=1"
check "spotter_log minimal (no file/ts)"    "IPC RX spotter_log data_len=14 file_name='' print_timestamp=0"
check "spotter_tx iridium_fallback=1"       "IPC RX spotter_tx data_len=4 iridium_fallback=1"
check "spotter_tx cellular-only default"    "IPC RX spotter_tx data_len=2 iridium_fallback=0"
check "sensor_data topic construction"      "IPC RX sensor_data topic='sensor/0000000000000001/temperature' data_len=3"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[[ $FAIL -eq 0 ]]
