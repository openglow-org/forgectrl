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
# and `iw` are stand-ins that record a start. The run directory holds a
# motors.released marker. One unauthenticated read must answer 200 and
# say the motors are released, from both run-dir readers behind it (the
# daemon read the hook's run directory); then the daemon is stopped, and
# nothing may have started a controller or run iw. The daemon's start
# notice names the fourteen overrides set here (the three test hooks
# among them), names only, and none that is not set. Then the daemon is
# started again with FORGECTRL_HOST_TEST unset and the same three test
# hooks set: it must answer the read on its machine paths (the sysfs
# tree and the switch device absent, each read fail-closed; the run
# directory /run/forgefirm, where no marker is, so the read says the
# motors are not released, from both readers), name no hook among the
# overrides in force, and name the three as set and ignored. The read
# judges the run-dir readers on their own: the notice's two lines come
# from the gate the readers take, and a reader past the gate would leave
# both lines as they are. Runs unprivileged; build.yml runs it:
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
# the marker the run-dir readers look for: the hook's directory has it,
# the machine's does not
: > "$T/run/motors.released"
fail() { echo "FAIL: $1"; echo "--- the daemon's output"; cat "$T/out"; exit 1; }
# /status says whether the motors are released twice, from the two
# run-dir readers behind it: status.c's own run_dir (the top-level key)
# and paths.h's ff_run_dir, through the lease's observed block; each is
# judged by its own key
motors() {
    grep -q ",\"motors_released\":$2," "$T/$1" \
        || fail "$3: the status's motors_released is not $2 (status.c's run-dir reader)"
    grep -q "\"sender\":[a-z]*,\"motors_released\":$2}" "$T/$1" \
        || fail "$3: the lease's observed motors_released is not $2 (paths.h's run-dir reader)"
}
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
motors status.json true "the hook's run directory was not read"
echo "  ok: GET /status answered 200, the motors released by the hook's run directory, both readers"

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

line=$(grep -m1 'environment overrides in force: ' "$T/out") || fail "no start notice names the overrides"
case "$line" in *=*) fail "the notice carries a value: $line" ;; esac
named=" ${line##*environment overrides in force: } "
for v in FORGECTRL_HOST_TEST FORGECTRL_DATA_DIR FORGECTRL_CONF FFLOG_CONF FFLOG_SOCK FFLOG_STDERR \
         GF_RUN_DIR GF_SYSFS_ROOT GF_SWITCH_DEV GF_PULSE_DEV FORGECTRL_GRBL_BIN FORGECTRL_CLOUD_BIN \
         FORGECTRL_PORT FORGECTRL_TLS_PORT; do
    case "$named" in *" $v "*) ;; *) fail "the notice does not name $v" ;; esac
done
for v in GF_SWITCH_WORD_FILE FORGECTRL_SSHD_INIT GFCOOL_TEMP_MAX; do
    case "$named" in *" $v "*) fail "the notice names $v, which is not set" ;; esac
done
echo "  ok: the start notice names the fourteen overrides, no others, no values"

[ ! -e "$T/started" ] || fail "started: $(cat "$T/started")"
echo "  ok: no controller started, iw not run"

# The test hooks are honored in a host-test run alone: with the gate
# unset the daemon takes its machine paths and says which hooks it
# ignored. iw's stand-in runs this time (the radio policy applies off a
# host test), and the start record shows it once the daemon has stopped.
mv "$T/out" "$T/out.host-test"
env FORGECTRL_DATA_DIR="$T/data" FORGECTRL_CONF="$T/data/forgefirm.conf" \
    FFLOG_CONF="$T/data/forgefirm.conf" FFLOG_SOCK="$T/no-log" FFLOG_STDERR=1 \
    GF_RUN_DIR="$T/run" GF_SYSFS_ROOT="$T/sys/" GF_SWITCH_DEV="$T/no-switches" \
    GF_PULSE_DEV="$T/no-pulse-device" \
    FORGECTRL_GRBL_BIN="$T/bin/grblHAL_glowforge" FORGECTRL_CLOUD_BIN="$T/bin/gfcloud.py" \
    FORGECTRL_PORT=$HTTP_PORT FORGECTRL_TLS_PORT=$TLS_PORT PATH="$T/bin:$PATH" \
    "$BUILD/forgectrl" > "$T/out" 2>&1 &
PID=$!
code=$(curl -s -o "$T/status2.json" -w '%{http_code}' --retry 30 --retry-connrefused \
            --retry-delay 1 --max-time 10 "http://127.0.0.1:$HTTP_PORT/status") || true
[ "$code" = 200 ] || fail "off a host test, GET /status answered '$code', not 200"
kill $PID
i=0
while alive; do
    i=$((i + 1))
    [ $i -le 75 ] || fail "off a host test, the daemon did not stop in 15 s"
    sleep 0.2
done
wait $PID || fail "off a host test, the daemon exited $? on SIGTERM"
PID=
line=$(grep -m1 'environment overrides in force: ' "$T/out") || fail "off a host test, no start notice names the overrides"
named=" ${line##*environment overrides in force: } "
for v in GF_RUN_DIR GF_SYSFS_ROOT GF_SWITCH_DEV; do
    case "$named" in *" $v "*) fail "off a host test, the notice names the test hook $v as in force" ;; esac
done
case "$named" in *" FORGECTRL_DATA_DIR "*) ;; *) fail "off a host test, the notice does not name FORGECTRL_DATA_DIR" ;; esac
line=$(grep -m1 'test hooks set and ignored' "$T/out") || fail "off a host test, no notice names the ignored hooks"
case "$line" in *=*) fail "the ignored-hooks notice carries a value: $line" ;; esac
named=" ${line##*: } "
for v in GF_RUN_DIR GF_SYSFS_ROOT GF_SWITCH_DEV; do
    case "$named" in *" $v "*) ;; *) fail "off a host test, the ignored-hooks notice does not name $v" ;; esac
done
echo "  ok: off a host test, the three test hooks are named as ignored and none as in force"
motors status2.json false "off a host test, the hook's run directory was read"
echo "  ok: off a host test, the motors read as not released by both readers: the marker in the hook's run directory was not seen"
grep -q '^iw reg reload' "$T/started" || fail "off a host test, iw was not run: $(cat "$T/started" 2>/dev/null)"
echo "  ok: off a host test, iw's stand-in ran"
echo "PASS: host_start"
