#!/usr/bin/env bash
# scripts/gateway_loopback_test.sh — UART gateway loopback validation
#
# Uses socat to create a virtual null-modem (two linked PTYs) and verifies
# that two bm_sbc gateway processes can discover each other over the UART
# link.
#
# Requirements:
#   - socat (optional; only needed for this test, not for normal operation)
#   - A built bm_sbc_multinode binary
#
# Usage: ./scripts/gateway_loopback_test.sh [path/to/bm_sbc_multinode]

# Ignore SIGHUP and SIGINT so background children and sleeps survive
# terminal close / tool-runner signal propagation.
trap '' HUP INT
set -euo pipefail

BINARY="${1:-./build/bm_sbc_multinode}"

if [[ ! -x "$BINARY" ]]; then
  echo "Binary not found: $BINARY"
  echo "Build with: cmake -B build -S . -DBM_SBC_BUILD_ALL_APPS=ON && cmake --build build"
  exit 1
fi

if ! command -v socat &>/dev/null; then
  echo "socat is not installed. It is required only for this loopback test."
  echo "Install with: brew install socat"
  exit 1
fi

WORK=$(mktemp -d /tmp/bm_sbc_gw_test_XXXXXX)
PASS=0; FAIL=0

check() {
  local desc="$1" file="$2" pattern="$3"
  if grep -qF "$pattern" "$file" 2>/dev/null; then
    echo "  PASS: $desc"
    PASS=$((PASS + 1))
  else
    echo "  FAIL: $desc"
    echo "        (pattern not found: '$pattern' in $file)"
    FAIL=$((FAIL + 1))
  fi
}

start_node() {
  local log="$1"; shift
  # Launch in its own process group to avoid stray SIGINT.
  perl -MPOSIX -e 'setpgrp(0,0); exec @ARGV' -- \
    "$BINARY" "$@" >"$log" 2>&1 &
  echo $!
}

kill_nodes() { kill "$@" 2>/dev/null; wait "$@" 2>/dev/null || true; }

cleanup() {
  # Kill any remaining processes.
  [[ -n "${SOCAT_PID:-}" ]] && kill "$SOCAT_PID" 2>/dev/null || true
  wait "$SOCAT_PID" 2>/dev/null || true
  echo "  Logs preserved in: $WORK"
  # rm -rf "$WORK"  # uncomment to auto-clean
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Create virtual null-modem via socat
# ---------------------------------------------------------------------------
echo "=== Setting up virtual serial ports via socat ==="
SOCAT_LOG="$WORK/socat.log"
# Launch socat in its own process group so stray SIGINT from the terminal
# doesn't kill it.  macOS lacks setsid(1), so use perl's setpgrp().
perl -MPOSIX -e 'setpgrp(0,0); exec @ARGV' -- \
  socat -d -d pty,raw,echo=0 pty,raw,echo=0 >"$SOCAT_LOG" 2>&1 &
SOCAT_PID=$!

# Wait for socat to create both PTYs and log the device paths.
sleep 1
if ! kill -0 "$SOCAT_PID" 2>/dev/null; then
  echo "socat failed to start. Log:"
  cat "$SOCAT_LOG"
  exit 1
fi

# Parse PTY paths from socat's stderr (format: "... PTY is /dev/ttys00X")
PTY1=$(grep -o '/dev/ttys[0-9]*\|/dev/pts/[0-9]*' "$SOCAT_LOG" | head -1)
PTY2=$(grep -o '/dev/ttys[0-9]*\|/dev/pts/[0-9]*' "$SOCAT_LOG" | tail -1)

if [[ -z "$PTY1" || -z "$PTY2" || "$PTY1" == "$PTY2" ]]; then
  echo "Failed to parse two distinct PTY paths from socat log:"
  cat "$SOCAT_LOG"
  exit 1
fi

echo "  PTY1: $PTY1"
echo "  PTY2: $PTY2"
echo ""

# ---------------------------------------------------------------------------
# Test 1: Two nodes connected ONLY via UART (no VPD peers)
# ---------------------------------------------------------------------------
echo "=== Test 1: UART-only link (A <--UART--> B) ==="
SOCK1="$WORK/sock1"
mkdir -p "$SOCK1"
LA="$WORK/A.log"; LB="$WORK/B.log"

PA=$(start_node "$LA" --node-id 0x0000000000000A01 \
                       --socket-dir "$SOCK1" \
                       --uart "$PTY1")
PB=$(start_node "$LB" --node-id 0x0000000000000B02 \
                       --socket-dir "$SOCK1" \
                       --uart "$PTY2")

echo "  Nodes started (PIDs: $PA $PB). Waiting 15 s for convergence..."
sleep 15

# Both processes should have started and initialized the stack.
check "A initialized stack"             "$LA" "stack initialized"
check "B initialized stack"             "$LB" "stack initialized"

# Neighbor discovery across the UART link.
check "A discovered B (NEIGHBOR_UP)"    "$LA" "NEIGHBOR_UP node=0000000000000b02"
check "B discovered A (NEIGHBOR_UP)"    "$LB" "NEIGHBOR_UP node=0000000000000a01"

# BCMP ping replies across UART.
check "A received ping reply from B"    "$LA" "bcmp_seq="
check "B received ping reply from A"    "$LB" "bcmp_seq="

kill_nodes "$PA" "$PB"
echo ""

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo "=== Results: $PASS passed, $FAIL failed ==="
[[ $FAIL -eq 0 ]]

