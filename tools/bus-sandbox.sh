#!/usr/bin/env bash
# Run a command against a working zenoh bus, on a machine whose loopback cannot
# carry one.
#
# WHY THIS EXISTS. zenoh peers find each other by UDP multicast scouting, and
# zenoh picks the interface in zenoh-link-udp's multicast.rs by taking the first
# address matching is_up() && is_running() && is_multicast() && !is_loopback().
# Linux gives lo flags IFF_UP|IFF_LOOPBACK -- no IFF_MULTICAST, no IFF_RUNNING --
# so lo fails three of those four and is never chosen. On a workstation with a
# real NIC the scouts go out on the NIC and same-host peers still find each
# other; a router told to listen on 127.0.0.1 does not, and the symptom is
# `inspect --mode client --connect` reporting "No node is publishing health"
# while the same query as a peer works perfectly.
#
# The board fixes this properly: meta-redline's 10-loopback.network sets
# Multicast=yes on lo, and its comment documents the same failure in detail.
# This reproduces that condition in a throwaway network namespace so bus tests
# behave here the way they do on a board.
#
#     tools/bus-sandbox.sh ./build/nodes/inspect/inspect health
#     tools/bus-sandbox.sh --router ./build/nodes/inspect/inspect \
#         --mode client --connect tcp/127.0.0.1:7447 health
#
# --router also starts zenohd on tcp/127.0.0.1:7447 and ws/127.0.0.1:7446,
# which is what the web console's browser client connects to.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WITH_ROUTER=0
if [ "${1:-}" = "--router" ]; then
    WITH_ROUTER=1
    shift
fi

if [ $# -eq 0 ]; then
    echo "usage: $0 [--router] <command> [args...]" >&2
    exit 2
fi

ZENOHD="${REDLINE_ZENOHD:-${ROOT}/build/zenohd-target/release/zenohd}"
if [ "$WITH_ROUTER" = "1" ] && [ ! -x "$ZENOHD" ]; then
    echo "bus-sandbox: no zenohd at $ZENOHD -- build with -DREDLINE_BUILD_ZENOHD=ON" >&2
    exit 1
fi

export REDLINE_BUS_SANDBOX_INNER=1
export REDLINE_BUS_SANDBOX_ROUTER="$WITH_ROUTER"
export REDLINE_BUS_SANDBOX_ZENOHD="$ZENOHD"

# -r maps the caller to root inside the namespace, which is what allows
# `ip link set lo multicast on` without any privilege on the host.
#
# -p --fork adds a PID NAMESPACE, and that is not a nicety. Without it the
# router and any node started inside outlive this script: -n gives a network
# namespace but children keep running in the host's PID space, and a trap does
# not fire when the supervisor kills the script. Six orphaned web_console
# processes and five routers accumulated that way, and because a stale router
# held 7446/7447, later runs silently talked to IT rather than to the one they
# started -- a false green that is worse than a failure. As PID 1 of its own
# namespace, this script takes everything with it when it exits.
exec unshare -rpn --fork "$ROOT/tools/bus-sandbox-inner.sh" "$@"
