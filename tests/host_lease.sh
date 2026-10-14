#!/bin/sh
# host_lease.sh - the machine lease: a holder refuses everything else, by
# name, on a host
# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# SPDX-License-Identifier: MIT
#
# Starts the built forgectrl the way host_setup.sh does for the whole
# record (the same environment, sysfs tree with cnc/state idle, stand-ins,
# port listener, and seeded setup record), so the gate is open and the
# controller's stand-in runs. With an event stream open, the switches
# check is started and left at its first wait: with no switch device the
# lid reads open, so it waits on "Close the lid to begin." and moves
# nothing. While it waits, one case each:
#
#   /status names the holder wizard:switches, of kind hardware, and
#     observes sender and motors_released beside it
#   POST /diag/flow-verify, /curve/record, /logs/export, /mode with the
#     mode in force, /controller/start, and /settings with the current
#     ui_units are each 409 in the holder's name (settings are locked),
#     none of them for an idle machine or a closed gate
#   the holder and the settings file are as they were
#
# The check is then aborted (and a diagnostic and the recorder stopped,
# in case a refusal failed): the lease is free, the check is not running,
# the same settings write is 200, and the event stream said
# lease.changed for the hold and then for its end. Nothing posts
# confirm=1, a reboot, /system/ssh, or an update. The daemon runs on the
# image's ulfius (tests/ulfius_image.sh). Runs unprivileged; build.yml
# runs it:
#
#   sh tests/host_lease.sh <build-dir> <ulfius-lib-dir>

# as root the daemon would write the paths it has no override for (its
# markers under /run)
[ "$(id -u)" != 0 ] || { echo "host_lease.sh runs unprivileged"; exit 2; }
BUILD=$(cd "${1:?build dir}" && pwd) || exit 2
ULIB=$(cd "${2:?ulfius lib dir}" && pwd) || exit 2
HTTP_PORT=18102
TLS_PORT=18465
URL=http://127.0.0.1:$HTTP_PORT
T=$(mktemp -d)
SYS=$T/sys
REC=$T/data/setup.json
CONF=$T/data/forgefirm.conf
PID=
LPID=
EVS=
cleanup() {
    [ -z "$EVS" ] || kill $EVS 2>/dev/null
    if [ -n "$PID" ]; then kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; fi
    [ -z "$LPID" ] || kill $LPID 2>/dev/null
    [ ! -f "$T/ctl.pids" ] || kill $(cat "$T/ctl.pids") 2>/dev/null
    rm -rf "$T"
}
trap 'rc=$?; cleanup; exit $rc' EXIT

fails=0
ok() { echo "  ok: $1"; }
bad() { echo "  FAIL: $1"; fails=$((fails + 1)); }
expect() { if [ "$2" = "$1" ]; then ok "$3"; else bad "$3 (got '$2', want '$1')"; fi; }
fatal() { echo "FAIL: $1"; echo "--- the daemon's output"; tail -60 "$T/out"; exit 1; }
# running, and not a zombie waiting to be reaped
alive() { s=$(cut -d' ' -f3 /proc/$1/stat 2>/dev/null); [ -n "$s" ] && [ "$s" != Z ]; }
# until <tries of 0.2 s> <command...>
until_ok() { n=$1; shift; while ! "$@"; do n=$((n - 1)); [ $n -gt 0 ] || return 1; sleep 0.2; done; }
# jget <json file> <python expression over d>: the value (JSON unless a string)
jget() {
    python3 -c 'import json, sys
d = json.load(open(sys.argv[1]))
v = eval(sys.argv[2])
print(v if isinstance(v, str) else json.dumps(v, sort_keys=True))' "$1" "$2"
}
# post <path> <token or -> [curl args]: the status; the body is in $T/body
post() {
    p=$1; tok=$2; shift 2
    if [ "$tok" = - ]; then
        curl -s -o "$T/body" -w '%{http_code}' -X POST "$@" "$URL$p"
    else
        curl -s -o "$T/body" -w '%{http_code}' -X POST -H "X-ForgeFIRM-Token: $tok" "$@" "$URL$p"
    fi
}
# the lease as /status reports it, in $T/status.json
lease() { curl -s -o "$T/status.json" "$URL/status"; }
holder() { lease && jget "$T/status.json" '(d["lease"]["holder"] or {}).get("owner") or "none"'; }
dark() { curl -s -o "$T/dark.json" "$URL/wiz/dark"; }
waiting() { dark && [ "$(jget "$T/dark.json" '"%s|%s" % (d.get("running"), d.get("phase"))')" = "True|Close the lid to begin." ]; }
stopped() { dark && [ "$(jget "$T/dark.json" 'd.get("running")')" = false ]; }
free() { [ "$(holder)" = none ]; }
held() { [ "$(holder)" = wizard:switches ]; }

start_daemon() {
    env LD_LIBRARY_PATH="$ULIB" FORGECTRL_HOST_TEST=1 \
        FORGECTRL_DATA_DIR="$T/data" FORGECTRL_CONF="$CONF" \
        FFLOG_CONF="$CONF" FFLOG_SOCK="$T/no-log" FFLOG_STDERR=1 \
        GF_RUN_DIR="$T/run" GF_SYSFS_ROOT="$SYS/" GF_SWITCH_DEV="$T/no-switches" \
        GF_SWITCH_WORD_FILE="$T/switches" GF_PULSE_DEV="$T/pulse" \
        FORGECTRL_GRBL_BIN="$T/bin/grbl" FORGECTRL_CLOUD_BIN="$T/bin/cloud" \
        FORGECTRL_PORT=$HTTP_PORT FORGECTRL_TLS_PORT=$TLS_PORT \
        "$BUILD/forgectrl" >> "$T/out" 2>&1 &
    PID=$!
    curl -s -o /dev/null --retry 30 --retry-connrefused --retry-delay 1 "$URL/status" \
        || fatal "the daemon did not answer"
}
stop_daemon() {
    kill $PID
    until_ok 75 eval '! alive $PID' || fatal "the daemon did not stop in 15 s"
    wait $PID
    PID=
}
ctl_up() { p=$(cat "$T/ctl.pid" 2>/dev/null); [ -n "$p" ] && alive "$p"; }

# The machine, as host_setup.sh builds it.
mkdir -p "$T/data" "$T/run" "$T/bin" "$SYS/cnc" "$SYS/thermal" "$SYS/head"
echo idle > "$SYS/cnc/state"
for a in cnc/stop cnc/laser_latch cnc/enable cnc/disable cnc/halt \
         head/air_assist_pwm thermal/exhaust_pwm thermal/intake_pwm; do
    : > "$SYS/$a"
done
echo 8 > "$T/switches"
: > "$T/pulse"
: > "$T/port.log"
echo "cool_intake_fans = 1" > "$CONF"
cat > "$T/bin/grbl" <<EOF
#!/bin/sh
echo \$\$ >> "$T/ctl.pids"
echo \$\$ > "$T/ctl.pid"
printf '%s\n' "\$GF_REPORT_SECRET" > "$T/ctl.secret.new"
mv "$T/ctl.secret.new" "$T/ctl.secret"
exec sleep 600
EOF
printf '#!/bin/sh\necho "$0" >> "%s/cloud.started"\n' "$T" > "$T/bin/cloud"
chmod +x "$T/bin/grbl" "$T/bin/cloud"

# A first start reads the documents' hashes and the required steps; the
# seeded record is what a finished first run leaves.
start_daemon
curl -s -o "$T/wiz0.json" "$URL/wiz" || fatal "GET /wiz failed"
stop_daemon
python3 - "$T/wiz0.json" "$REC" <<'EOF' || fatal "the setup record was not written"
import json, sys, time
st = json.load(open(sys.argv[1]))
ts = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
version = {w['id']: w['version'] for w in st['wizards']}
rec = {
    'schema': 1, 'created': ts,
    'advisories': {d['id']: {'hash': d['hash'], 'accepted': ts, 'method': d['consent']}
                   for d in st['documents']},
    'acceptance': {'pressed_at': ts},
    'account': {'name': 'owner', 'uid': 1000, 'created': ts},
    'wizards': {i: {'version': version[i], 'completed': ts, 'result': {}, 'applied': {}}
                for i in st['missing']},
    'flags': {},
}
json.dump(rec, open(sys.argv[2], 'w'), indent=2)
EOF

# The controller's port, as host_setup.sh stands it in.
python3 - "$T/run/grbl.ctl" "$T/port.log" <<'EOF' &
import socket, sys, threading
path, log = sys.argv[1], sys.argv[2]
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(path)
srv.listen(4)
def serve(c):
    for line in c.makefile('rb'):
        with open(log, 'ab') as o:
            o.write(line)
        c.sendall(b'ok\n')
while True:
    c, _ = srv.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
EOF
LPID=$!
until_ok 50 test -S "$T/run/grbl.ctl" || fatal "the port's stand-in did not listen"

start_daemon
until_ok 100 ctl_up || fatal "no controller started on the seeded record"
TOKEN=$(cat "$T/data/panel.token")
curl -s -o "$T/wiz.json" "$URL/wiz"
[ "$(jget "$T/wiz.json" 'd["gate"]')" = open ] || fatal "the gate is not open: $(jget "$T/wiz.json" 'd.get("why")')"
[ "$(cat "$SYS/cnc/state")" = idle ] || fatal "cnc/state is not idle"
ok "the gate is open and cnc/state is idle"
free || fatal "the lease is held before the check: $(holder)"
MODE=$(curl -s "$URL/mode" | python3 -c 'import json, sys; print(json.load(sys.stdin)["mode"])')
UNITS=$(curl -s "$URL/settings" | python3 -c 'import json, sys; print(json.load(sys.stdin).get("ui_units") or "metric")')

echo "== the switches check holds the machine"
curl -s -N -o "$T/events" "$URL/events" &
EVS=$!
until_ok 50 grep -qs '^event: hello' "$T/events" || fatal "the event stream did not open"
sleep 1
OFF=$(wc -c < "$T/events")
expect 200 "$(post /wiz/switches/start "$TOKEN")" "POST /wiz/switches/start is 200"
until_ok 100 waiting && ok "it waits: Close the lid to begin." || bad "the check is not at its first wait: $(cat "$T/dark.json")"
until_ok 50 held || bad "the lease is not the check's: $(holder)"
lease
expect 'wizard:switches|hardware|motors_released,sender' \
    "$(jget "$T/status.json" '"%s|%s|%s" % (d["lease"]["holder"].get("owner"), d["lease"]["holder"].get("kind"), ",".join(sorted(d["lease"].get("observed") or {})))')" \
    "/status: the holder wizard:switches, of kind hardware, sender and motors_released observed"
cp "$CONF" "$T/conf.before"
# by_holder <what>: the body refuses in the holder's name, and for nothing else
by_holder() {
    words=$(cat "$T/body")
    case $words in
        *"machine is not idle"*|*"controllers are gated"*) bad "$1 was refused for something else: $words" ;;
        *"holds the machine"*) case $words in *switches*) ok "$1: $words" ;; *) bad "$1: $words" ;; esac ;;
        *) bad "$1 is not refused in the holder's name: $words" ;;
    esac
}
for req in "/diag/flow-verify" "/curve/record" "/logs/export" "/mode controller=$MODE" "/controller/start"; do
    set -- $req
    if [ $# -gt 1 ]; then code=$(post $1 "$TOKEN" -d "$2"); else code=$(post $1 "$TOKEN"); fi
    expect 409 "$code" "POST $req under the check's hold is 409"
    by_holder "POST $req"
done
expect 409 "$(post /settings "$TOKEN" -d "ui_units=$UNITS")" "POST /settings ui_units=$UNITS under the check's hold is 409"
by_holder "POST /settings"
grep -qF 'settings are locked' "$T/body" && ok "and says the settings are locked" \
    || bad "the settings refusal does not say they are locked"
expect wizard:switches "$(holder)" "the holder is unchanged"
cmp -s "$T/conf.before" "$CONF" && ok "the settings file is unchanged" || bad "a refused write changed the settings file"

echo "== the abort frees it"
expect 200 "$(post /wiz/switches/abort "$TOKEN")" "POST /wiz/switches/abort is 200"
post /diag/abort "$TOKEN" > /dev/null
post /curve/stop "$TOKEN" > /dev/null
until_ok 150 free && ok "the lease has no holder" || bad "the lease is still held: $(holder)"
until_ok 150 stopped && ok "the check is not running" || bad "the check still runs: $(cat "$T/dark.json")"
expect 200 "$(post /settings "$TOKEN" -d "ui_units=$UNITS")" "the same settings write is then 200"
sleep 1
kill $EVS 2>/dev/null
wait $EVS 2>/dev/null
EVS=
tail -c +$((OFF + 1)) "$T/events" > "$T/events.after"
grep -q '^event: lease.changed' "$T/events.after" && ok "the stream said event: lease.changed" \
    || bad "no lease.changed on the stream"
python3 - "$T/events.after" <<'EOF' && ok 'the stream named "owner":"wizard:switches" and then "owner":null' \
    || bad "the stream did not name the hold and then its end"
import sys
t = open(sys.argv[1], encoding='utf-8', errors='replace').read()
take, free = t.find('"owner":"wizard:switches"'), t.find('"owner":null')
sys.exit(0 if 0 <= take < free else 1)
EOF

stop_daemon
if [ $fails -eq 0 ]; then
    echo "PASS: host_lease"
else
    echo "FAIL: host_lease, $fails checks"
    echo "--- the daemon's output"
    tail -60 "$T/out"
    exit 1
fi
