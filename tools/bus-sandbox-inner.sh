#!/usr/bin/env bash
# The inside of tools/bus-sandbox.sh. Not meant to be run directly.
set -euo pipefail

if [ "${REDLINE_BUS_SANDBOX_INNER:-}" != "1" ]; then
    echo "bus-sandbox-inner: run tools/bus-sandbox.sh instead" >&2
    exit 2
fi

ip link set lo up
# The one line this whole script exists for.
ip link set lo multicast on

ROUTER_PID=""
# Belt and braces alongside the PID namespace: TERM and INT as well as EXIT, and
# the whole process group rather than just the router, so anything the command
# started goes too.
cleanup() {
    [ -n "$ROUTER_PID" ] && kill "$ROUTER_PID" 2>/dev/null || true
    kill -- -$$ 2>/dev/null || true
}
trap cleanup EXIT INT TERM

if [ "${REDLINE_BUS_SANDBOX_ROUTER:-0}" = "1" ]; then
    "$REDLINE_BUS_SANDBOX_ZENOHD" \
        -l tcp/127.0.0.1:7447 \
        -l ws/127.0.0.1:7446 \
        > "${REDLINE_BUS_SANDBOX_LOG:-/tmp/bus-sandbox-zenohd.log}" 2>&1 &
    ROUTER_PID=$!
    # Long enough for the listeners to bind; the router is useless before that
    # and a client that races it just fails confusingly.
    sleep 2
fi

exec "$@"
