#!/usr/bin/env bash
# scripts/validate.sh — full build + test validation
#
# Runs the complete validation suite:
#   1. CMake configure (all preset, fresh)
#   2. CMake build (all preset, clean-first)
#   3. CTest
#   4. Gateway loopback test (requires socat)
#   5. Multinode test
#   6. Gateway IPC test (python client + CBOR dispatch)
#
# Usage: ./scripts/validate.sh
#   Run from the repository root.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

echo "=== 1/6  CMake configure (preset: all, fresh) ==="
cmake --preset all --fresh

echo ""
echo "=== 2/6  CMake build (preset: all, clean-first) ==="
cmake --build --preset all --clean-first

echo ""
echo "=== 3/6  CTest ==="
ctest --test-dir build/all

echo ""
echo "=== 4/6  Gateway loopback test ==="
./scripts/gateway_loopback_test.sh

echo ""
echo "=== 5/6  Multinode test ==="
./scripts/multinode_test.sh

echo ""
echo "=== 6/6  Gateway IPC test ==="
./scripts/gateway_ipc_test.sh

echo ""
echo "=== All validation steps passed ==="
