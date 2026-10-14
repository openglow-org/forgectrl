#!/bin/sh
# host_lan.sh - writes from the LAN go to HTTPS, on a host
# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# SPDX-License-Identifier: MIT
#
# Starts the built forgectrl in a network namespace of its own, joined to
# this host by one veth pair: the daemon's end is 192.0.2.2, the client's
# 192.0.2.1, and nothing else is added (no default route, no other
# interface touched). The daemon runs as the user who invoked sudo, with
# host_guard.sh's scratch paths, the image's ulfius, and
# FORGECTRL_HOST_TEST=1. From 192.0.2.1, a non-loopback peer:
#
#   GET /status is served over HTTPS and over HTTP;
#   POST /settings ui_units=imperial, GET /system/ssh and GET /logs over
#     HTTP, each with the token, are a 302 to the same path on HTTPS and
#     act on nothing (the settings file still says metric);
#   the same settings write from 127.0.0.1 inside the daemon's namespace
#     is served, and the file then says imperial;
#   with no account in the scratch tree, a settings write with the token
#     over HTTPS from 192.0.2.1 is served.
#
# Nothing posts /system/reboot, /system/ssh, or confirm=1. The namespace
# and the veth are removed on the way out, failure included. Needs root
# for the namespace; build.yml runs it:
#
#   sudo sh tests/host_lan.sh <build-dir> <ulfius-lib-dir>

[ "$(id -u)" = 0 ] || { echo "host_lan.sh needs root: it makes a network namespace"; exit 2; }
case ${SUDO_UID:-} in
    ''|0|*[!0-9]*) echo "host_lan.sh runs the daemon as the user who invoked sudo: SUDO_UID is missing or 0"; exit 2 ;;
esac
RUN_UID=$SUDO_UID
RUN_GID=${SUDO_GID:-$SUDO_UID}
BUILD=$(cd "${1:?build dir}" && pwd) || exit 2
ULIB=$(cd "${2:?ulfius lib dir}" && pwd) || exit 2
HTTP_PORT=18096
TLS_PORT=18459
DAEMON=192.0.2.2
CLIENT=192.0.2.1
NS=fclan$$
VA=fcla$$
VB=fclb$$
T=$(mktemp -d)
SYS=$T/sys
CONF=$T/data/forgefirm.conf
PID=
cleanup() {
    if [ -n "$PID" ]; then kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; fi
    ip netns del $NS 2>/dev/null
    ip link del $VA 2>/dev/null
    rm -rf "$T"
}
trap 'rc=$?; cleanup; exit $rc' EXIT

fails=0
ok() { echo "  ok: $1"; }
bad() { echo "  FAIL: $1"; fails=$((fails + 1)); }
expect() { if [ "$2" = "$1" ]; then ok "$3"; else bad "$3 (got '$2', want '$1')"; fi; }
fatal() { echo "FAIL: $1"; echo "--- the daemon's output"; tail -60 "$T/out"; exit 1; }
alive() { s=$(cut -d' ' -f3 /proc/$1/stat 2>/dev/null); [ -n "$s" ] && [ "$s" != Z ]; }
until_ok() { n=$1; shift; while ! "$@"; do n=$((n - 1)); [ $n -gt 0 ] || return 1; sleep 0.2; done; }
header() { tr -d '\r' < "$T/head" | sed -n "s/^$1: //Ip"; }
units() { sed -n 's/^ui_units *= *//p' "$CONF"; }

# lan <method> <url> [curl args]: from 192.0.2.1; the status, the body in
# $T/body, the headers in $T/head; never follows a redirect
lan() {
    m=$1; u=$2; shift 2
    curl -sk --interface $CLIENT -X "$m" -D "$T/head" -o "$T/body" -w '%{http_code}' "$@" "$u"
}
# inside <method> <url> [curl args]: from 127.0.0.1 in the daemon's namespace
inside() {
    m=$1; u=$2; shift 2
    ip netns exec $NS curl -sk -X "$m" -D "$T/head" -o "$T/body" -w '%{http_code}' "$@" "$u"
}

# The network: one namespace, one veth pair, two addresses.
ip netns add $NS || fatal "cannot make the namespace"
ip link add $VA type veth peer name $VB || fatal "cannot make the veth pair"
ip link set $VB netns $NS || fatal "cannot move the veth's end"
ip addr add $CLIENT/24 dev $VA && ip link set $VA up || fatal "cannot raise the client's end"
ip -n $NS addr add $DAEMON/24 dev $VB && ip -n $NS link set $VB up && ip -n $NS link set lo up \
    || fatal "cannot raise the daemon's end"

# The scratch tree host_guard.sh uses, the daemon's user owning it.
mkdir -p "$T/data" "$T/run" "$T/bin" "$T/log" "$SYS/cnc" "$SYS/thermal" "$SYS/head"
echo idle > "$SYS/cnc/state"
for a in cnc/stop cnc/laser_latch cnc/enable cnc/disable cnc/halt \
         head/air_assist_pwm thermal/exhaust_pwm thermal/intake_pwm; do
    : > "$SYS/$a"
done
echo 8 > "$T/switches"
: > "$T/pulse"
echo "ui_units = metric" > "$CONF"
printf '#!/bin/sh\necho "$0" >> "%s/grbl.started"\nexec sleep 600\n' "$T" > "$T/bin/grbl"
printf '#!/bin/sh\necho "$0" >> "%s/cloud.started"\n' "$T" > "$T/bin/cloud"
chmod +x "$T/bin/grbl" "$T/bin/cloud"
chown -R "$RUN_UID:$RUN_GID" "$T"
chmod 755 "$T"

ip netns exec $NS setpriv --reuid="$RUN_UID" --regid="$RUN_GID" --clear-groups \
    env LD_LIBRARY_PATH="$ULIB" FORGECTRL_HOST_TEST=1 \
        FORGECTRL_DATA_DIR="$T/data" FORGECTRL_CONF="$CONF" \
        FFLOG_CONF="$CONF" FFLOG_SOCK="$T/no-log" FFLOG_STDERR=1 \
        GF_RUN_DIR="$T/run" GF_SYSFS_ROOT="$SYS/" GF_SWITCH_DEV="$T/no-switches" \
        GF_SWITCH_WORD_FILE="$T/switches" GF_PULSE_DEV="$T/pulse" \
        FORGECTRL_GRBL_BIN="$T/bin/grbl" FORGECTRL_CLOUD_BIN="$T/bin/cloud" \
        GF_LOGS_ROOT="$T/log" GF_LOGS_RSYSLOG="$T/rsyslog-forgefirm.conf" \
        GF_LOGS_EFFECTIVE="$T/loglevels" GF_LOGS_STAGE="$T/stage" \
        FORGECTRL_PORT=$HTTP_PORT FORGECTRL_TLS_PORT=$TLS_PORT \
        "$BUILD/forgectrl" > "$T/out" 2>&1 &
PID=$!
curl -s -o /dev/null --interface $CLIENT --retry 30 --retry-connrefused --retry-delay 1 \
    "http://$DAEMON:$HTTP_PORT/status" || fatal "the daemon did not answer on $DAEMON"
until_ok 50 test -s "$T/data/panel.token" || fatal "no panel token"
TOKEN=$(cat "$T/data/panel.token")
expect "$RUN_UID" "$(stat -c %u /proc/$PID)" "the daemon runs as the user who invoked sudo"

echo "== from $CLIENT"
expect 200 "$(lan GET "https://$DAEMON:$TLS_PORT/status")" "GET /status over HTTPS is 200"
expect 200 "$(lan GET "http://$DAEMON:$HTTP_PORT/status")" "GET /status over HTTP is 200"
for req in "POST /settings" "GET /system/ssh" "GET /logs"; do
    set -- $req
    if [ "$1" = POST ]; then
        code=$(lan POST "http://$DAEMON:$HTTP_PORT$2" -H "X-ForgeFIRM-Token: $TOKEN" -d ui_units=imperial)
    else
        code=$(lan GET "http://$DAEMON:$HTTP_PORT$2" -H "X-ForgeFIRM-Token: $TOKEN")
    fi
    expect 302 "$code" "$req over HTTP with the token is 302"
    expect "https://$DAEMON:$TLS_PORT$2" "$(header Location)" "to the same path on HTTPS"
done
expect metric "$(units)" "the settings file still says metric"

echo "== from 127.0.0.1 in the daemon's namespace"
expect 200 "$(inside POST "http://127.0.0.1:$HTTP_PORT/settings" -H "X-ForgeFIRM-Token: $TOKEN" -d ui_units=imperial)" \
    "POST /settings ui_units=imperial over HTTP is 200"
expect imperial "$(units)" "the settings file says imperial"

echo "== over HTTPS from $CLIENT, no account"
[ ! -e "$T/data/users" ] && ok "the scratch tree has no account" || bad "the scratch tree has an account"
expect 200 "$(lan POST "https://$DAEMON:$TLS_PORT/settings" -H "X-ForgeFIRM-Token: $TOKEN" -d ui_units=metric)" \
    "POST /settings with the token over HTTPS is 200"
expect metric "$(units)" "the settings file says metric again"

kill $PID
until_ok 75 eval '! alive $PID' || fatal "the daemon did not stop in 15 s"
wait $PID
PID=
if [ $fails -eq 0 ]; then
    echo "PASS: host_lan"
else
    echo "FAIL: host_lan, $fails checks"
    echo "--- the daemon's output"
    tail -60 "$T/out"
    exit 1
fi
