#!/bin/sh
# DFU rollback guard — run as ExecStartPre by systemd before each gateway start.
#
# This script runs even if the installed binary is broken (crash-before-user-code).
# It tracks how many times systemd has tried to start the gateway while a DFU swap
# is pending and rolls back to the previous binary if the threshold is exceeded.
#
# All paths are derived from the script's own location so this works regardless
# of CMAKE_INSTALL_PREFIX (e.g. /usr/local/bin, /opt/bm_sbc/bin, etc.).
#
# Files managed (all co-located with the binary):
#   dfu_pending.bin    — marker written by set_pending_and_reset(); deleted by set_confirmed()
#   bm_sbc_gateway.bak — hard link to the previous binary; deleted by set_confirmed()
#   dfu_attempts.txt   — plain-text boot-attempt counter; managed entirely by this script

echo "dfu_prestart.sh running"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALL="$SCRIPT_DIR/bm_sbc_gateway"
BAK="$SCRIPT_DIR/bm_sbc_gateway.bak"
MARKER="$SCRIPT_DIR/dfu_pending.bin"
ATTEMPTS_FILE="$SCRIPT_DIR/dfu_attempts.txt"
MAX_ATTEMPTS=3

# No DFU in progress — clean up any stale counter and exit immediately.
if [ ! -f "$MARKER" ]; then
    rm -f "$ATTEMPTS_FILE"
    exit 0
fi

# DFU is pending.  Increment the boot-attempt counter.
attempts=$(cat "$ATTEMPTS_FILE" 2>/dev/null || echo 0)
attempts=$((attempts + 1))
echo "$attempts" > "$ATTEMPTS_FILE"

# If we have exceeded the limit, roll back to the previous binary.
if [ "$attempts" -gt "$MAX_ATTEMPTS" ]; then
    echo "dfu_prestart: $attempts failed attempts, rolling back"
    if [ -f "$BAK" ]; then
        mv "$BAK" "$INSTALL"
        chmod 755 "$INSTALL"
    else
        echo "dfu_prestart: no .bak found, staying with current binary"
    fi
    rm -f "$MARKER" "$ATTEMPTS_FILE"
fi

exit 0
