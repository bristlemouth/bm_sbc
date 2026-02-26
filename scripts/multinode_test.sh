#!/usr/bin/env bash
# scripts/multinode_test.sh — Bristlemouth multiprocess validation
#
# Subtasks validated:
#   6b  2-node launch and log collection
#   6c  BCMP ping reply (🏓 / bcmp_seq=) appears in each node's log
#   6d  Middleware pub/sub — PUBSUB_RX from remote node appears
#   6e  3-node chain topology — each node sees its expected neighbors
#   6f  15-neighbor cap — hub with 16 peers logs truncation warning
#
# Usage: ./scripts/multinode_test.sh [path/to/bm_sbc_multinode]

set -euo pipefail

BINARY="${1:-./build/bm_sbc_multinode}"

if [[ ! -x "$BINARY" ]]; then
  echo "Binary not found: $BINARY"
  echo "Build with: cmake -B build -S . -DBM_SBC_BUILD_ALL_APPS=ON && cmake --build build"
  exit 1
fi

WORK=$(mktemp -d /tmp/bm_sbc_test_XXXXXX)
PASS=0; FAIL=0

# check <description> <log-file> <grep-string>
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

# start_node <log-file> [binary-args...]  — launches in background, prints PID
start_node() {
  local log="$1"; shift
  "$BINARY" "$@" >"$log" 2>&1 &
  echo $!
}

kill_nodes() { kill "$@" 2>/dev/null; wait "$@" 2>/dev/null || true; }

# ---------------------------------------------------------------------------
# Test 1: 2-node mesh (A <-> B)  — covers 6b, 6c, 6d
# ---------------------------------------------------------------------------
echo "=== Test 1: 2-node mesh (A <-> B) ==="
SOCK1="$WORK/sock2"
mkdir -p "$SOCK1"
LA="$WORK/A.log"; LB="$WORK/B.log"

PA=$(start_node "$LA" --node-id 0x0000000000000001 --peer 0x0000000000000002 --socket-dir "$SOCK1")
PB=$(start_node "$LB" --node-id 0x0000000000000002 --peer 0x0000000000000001 --socket-dir "$SOCK1")

echo "  Nodes started (PIDs: $PA $PB). Waiting 15 s for convergence..."
sleep 15

# 6b: both nodes started and produced output
check "A produced output"              "$LA" "multinode app: setup"
check "B produced output"              "$LB" "multinode app: setup"
# 6c: ping replies (bm_core logs "🏓 ... bcmp_seq=..." via bm_debug)
check "A received ping reply from B"   "$LA" "bcmp_seq="
check "B received ping reply from A"   "$LB" "bcmp_seq="
# 6d: pub/sub messages received across the link
check "B received PUBSUB from A"       "$LB" "PUBSUB_RX from=0000000000000001"
check "A received PUBSUB from B"       "$LA" "PUBSUB_RX from=0000000000000002"
# neighbor discovery
check "A discovered B (NEIGHBOR_UP)"   "$LA" "NEIGHBOR_UP node=0000000000000002"
check "B discovered A (NEIGHBOR_UP)"   "$LB" "NEIGHBOR_UP node=0000000000000001"

kill_nodes "$PA" "$PB"
echo ""

# ---------------------------------------------------------------------------
# Test 2: 3-node chain  A -- B -- C  — covers 6e
# ---------------------------------------------------------------------------
echo "=== Test 2: 3-node chain (A -- B -- C) ==="
SOCK2="$WORK/sock3"
mkdir -p "$SOCK2"
LA3="$WORK/A3.log"; LB3="$WORK/B3.log"; LC3="$WORK/C3.log"

PA3=$(start_node "$LA3" --node-id 0x0000000000000001 \
                         --peer 0x0000000000000002 \
                         --socket-dir "$SOCK2")
PB3=$(start_node "$LB3" --node-id 0x0000000000000002 \
                         --peer 0x0000000000000001 \
                         --peer 0x0000000000000003 \
                         --socket-dir "$SOCK2")
PC3=$(start_node "$LC3" --node-id 0x0000000000000003 \
                         --peer 0x0000000000000002 \
                         --socket-dir "$SOCK2")

echo "  Nodes started (PIDs: $PA3 $PB3 $PC3). Waiting 15 s..."
sleep 15

check "A sees B as neighbor"   "$LA3" "NEIGHBOR_UP node=0000000000000002"
check "B sees A as neighbor"   "$LB3" "NEIGHBOR_UP node=0000000000000001"
check "B sees C as neighbor"   "$LB3" "NEIGHBOR_UP node=0000000000000003"
check "C sees B as neighbor"   "$LC3" "NEIGHBOR_UP node=0000000000000002"

kill_nodes "$PA3" "$PB3" "$PC3"
echo ""

# ---------------------------------------------------------------------------
# Test 3: 15-neighbor cap (hub with 16 peers)  — covers 6f
# ---------------------------------------------------------------------------
echo "=== Test 3: 15-neighbor cap (hub + 16 peers) ==="
SOCK3="$WORK/sockN"
mkdir -p "$SOCK3"
LHUB="$WORK/hub.log"

# Build the --peer list: 16 peer IDs (0x1 .. 0x10)
PEER_ARGS=()
for i in $(seq 1 16); do
  PEER_ARGS+=(--peer "$(printf '0x%016x' "$i")")
done

PHUB=$(start_node "$LHUB" --node-id 0x0000000000000000 "${PEER_ARGS[@]}" \
                           --socket-dir "$SOCK3")

echo "  Hub started (PID: $PHUB). Waiting 2 s..."
sleep 2

check "Hub logs peer-count truncation" "$LHUB" "vpd: peer count 16 exceeds cap 15"

kill_nodes "$PHUB"
echo ""

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo "=== Results: $PASS passed, $FAIL failed ==="
rm -rf "$WORK"
[[ $FAIL -eq 0 ]]

