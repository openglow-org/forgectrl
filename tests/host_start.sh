#!/bin/sh
# host_start.sh - the built daemon starts on a host and answers a read
# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# SPDX-License-Identifier: MIT
#
# Starts the built forgectrl with its machine paths in a temporary
# directory (the data directory, the settings file, the run directory,
# the sysfs root, the switch device) and a pulse device that is not
# there, on high ports, with FORGECTRL_HOST_TEST=1. The two controllers
# and `iw` are stand-ins that record a start. One unauthenticated read
# must answer 200; then the daemon is stopped, and nothing may have
# started a controller or run iw. Runs unprivileged; build.yml runs it:
#
#   sh tests/host_start.sh <build-dir>
set -e
# as root the daemon would write the paths it has no override for (its
# markers under /run)
[ "$(id -u)" != 0 ] || { echo "host_start.sh runs unprivileged"; exit 2; }
BUILD=$(cd "${1:?build dir}" && pwd)
HTTP_PORT=18080
TLS_PORT=18443
T=$(mktemp -d)
PID=
trap 'rc=$?; set +e; [ -z "$PID" ] || { kill -9 $PID; wait $PID; } 2>/dev/null; rm -rf "$T"; exit $rc' EXIT
mkdir "$T/data" "$T/run" "$T/sys" "$T/bin"
for b in iw grblHAL_glowforge gfcloud.py; do
    printf '#!/bin/sh\necho "%s $*" >> "%s/started"\n' "$b" "$T" > "$T/bin/$b"
    chmod +x "$T/bin/$b"
done
fail() { echo "FAIL: $1"; echo "--- the daemon's output"; cat "$T/out"; exit 1; }
# running, and not a zombie waiting to be reaped
alive() { s=$(cut -d' ' -f3 /proc/$PID/stat 2>/dev/null); [ -n "$s" ] && [ "$s" != Z ]; }

env FORGECTRL_HOST_TEST=1 \
    FORGECTRL_DATA_DIR="$T/data" FORGECTRL_CONF="$T/data/forgefirm.conf" \
    FFLOG_CONF="$T/data/forgefirm.conf" FFLOG_SOCK="$T/no-log" FFLOG_STDERR=1 \
    GF_RUN_DIR="$T/run" GF_SYSFS_ROOT="$T/sys/" GF_SWITCH_DEV="$T/no-switches" \
    GF_PULSE_DEV="$T/no-pulse-device" \
    FORGECTRL_GRBL_BIN="$T/bin/grblHAL_glowforge" FORGECTRL_CLOUD_BIN="$T/bin/gfcloud.py" \
    FORGECTRL_PORT=$HTTP_PORT FORGECTRL_TLS_PORT=$TLS_PORT PATH="$T/bin:$PATH" \
    "$BUILD/forgectrl" > "$T/out" 2>&1 &
PID=$!

# one unauthenticated read, sent once the port takes connections
code=$(curl -s -o "$T/status.json" -w '%{http_code}' --retry 30 --retry-connrefused \
            --retry-delay 1 --max-time 10 "http://127.0.0.1:$HTTP_PORT/status") || true
[ "$code" = 200 ] || fail "GET /status answered '$code', not 200"
echo "  ok: GET /status answered 200"

kill $PID
i=0
while alive; do
    i=$((i + 1))
    [ $i -le 75 ] || fail "the daemon did not stop in 15 s"
    sleep 0.2
done
wait $PID || fail "the daemon exited $? on SIGTERM"
PID=
echo "  ok: the daemon stopped"

[ ! -e "$T/started" ] || fail "started: $(cat "$T/started")"
echo "  ok: no controller started, iw not run"
echo "PASS: host_start"
