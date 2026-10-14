#!/bin/sh
# host_routes.sh - the routes that need a running daemon, on a host
# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# SPDX-License-Identifier: MIT
#
# Starts the built forgectrl as host_start.sh does, with the GRBL
# controller up: a setup record the gate accepts, the switch word closed
# (GF_SWITCH_WORD_FILE), a pulse device that is a plain file the broker
# opens and locks, and a sysfs tree with no head accelerometer, so the
# motion probe is skipped and the supervisor spawns the controller's
# stand-in, which stays running and records the report secret it was
# handed. A listener on $GF_RUN_DIR/grbl.ctl stands in for the
# controller's port and answers ok. One case per route, against that
# daemon:
#
#   the report channel: a run report with the secret moves the exhaust
#     duty; an idle report with no secret, a wrong one, or one of the
#     wrong length is refused and leaves the duty file as it was; an idle
#     report with the secret ends the session; the secret dies with its
#     controller
#   /events: the stream, the cap, the reads beside it, a freed place
#   the motion routes: the token, the numbers, a line on the port each
#   /ext/call with extensions off, a package check that is no wizard,
#   /wiz/changed, and the menu GET /wiz carries
#   the write routes without the token, an upload, and a body over the
#     cap; the Host, Origin and Sec-Fetch-Site checks on a read
#   the settings' bounds, in the file FORGECTRL_CONF names; the page's
#     frame policy
#
# The daemon runs on the image's ulfius (tests/ulfius_image.sh): the
# distribution's library cuts the peer address that the loopback checks
# read. Runs unprivileged; build.yml runs it:
#
#   sh tests/host_routes.sh <build-dir> <ulfius-lib-dir>

# as root the daemon would write the paths it has no override for (its
# markers under /run)
[ "$(id -u)" != 0 ] || { echo "host_routes.sh runs unprivileged"; exit 2; }
BUILD=$(cd "${1:?build dir}" && pwd) || exit 2
ULIB=$(cd "${2:?ulfius lib dir}" && pwd) || exit 2
HTTP_PORT=18090
TLS_PORT=18453
URL=http://127.0.0.1:$HTTP_PORT
T=$(mktemp -d)
SYS=$T/sys
PID=
LPID=
EVS=
cleanup() {
    for p in $EVS; do kill $p 2>/dev/null; done
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

# post <path> <token or -> [curl args]: the status; the body is in $T/body
post() {
    p=$1; tok=$2; shift 2
    if [ "$tok" = - ]; then
        curl -s -o "$T/body" -w '%{http_code}' -X POST "$@" "$URL$p"
    else
        curl -s -o "$T/body" -w '%{http_code}' -X POST -H "X-ForgeFIRM-Token: $tok" "$@" "$URL$p"
    fi
}
# report <mode> [<secret>]: the report channel, as the controller sends it
report() {
    if [ $# -gt 1 ]; then
        curl -s -o "$T/body" -w '%{http_code}' -X POST -H "X-ForgeFIRM-Report: $2" -d "mode=$1" "$URL/cool/state"
    else
        curl -s -o "$T/body" -w '%{http_code}' -X POST -d "mode=$1" "$URL/cool/state"
    fi
}

start_daemon() {
    env LD_LIBRARY_PATH="$ULIB" FORGECTRL_HOST_TEST=1 \
        FORGECTRL_DATA_DIR="$T/data" FORGECTRL_CONF="$T/data/forgefirm.conf" \
        FFLOG_CONF="$T/data/forgefirm.conf" FFLOG_SOCK="$T/no-log" FFLOG_STDERR=1 \
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

# The machine: the kernel idle, the files the supervisor's safing and
# the probe write, the three fan duties, no head accelerometer; the
# switch word with the lid chain closed (bit 3) and the interlock loop
# closed (bit 5 clear); the pulse device a plain file.
mkdir -p "$T/data" "$T/run" "$T/bin" "$SYS/cnc" "$SYS/thermal" "$SYS/head"
echo idle > "$SYS/cnc/state"
for a in cnc/stop cnc/laser_latch cnc/enable cnc/disable cnc/halt \
         head/air_assist_pwm thermal/exhaust_pwm thermal/intake_pwm; do
    : > "$SYS/$a"
done
echo 8 > "$T/switches"
: > "$T/pulse"
: > "$T/port.log"
echo "cool_intake_fans = 1" > "$T/data/forgefirm.conf"
# The GRBL controller's stand-in: it records its pid and the secret it
# was handed, and stays up. The cloud client's must never start.
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
# record is then written as a finished first run would leave it.
start_daemon
curl -s -o "$T/wiz0.json" "$URL/wiz" || fatal "GET /wiz failed"
stop_daemon
python3 - "$T/wiz0.json" "$T/data/setup.json" <<'EOF' || fatal "the setup record was not written"
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

# The controller's port: every line is recorded and answered ok.
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
curl -s -o "$T/wiz1.json" "$URL/wiz"
grep -q '"gate":"open"' "$T/wiz1.json" || fatal "the gate is closed: the record is short ($(grep -o '"why":"[^"]*"' "$T/wiz1.json"))"
until_ok 150 test -s "$T/ctl.secret" || fatal "no controller started: there is no secret to accept"
SEC=$(cat "$T/ctl.secret")
TOKEN=$(cat "$T/data/panel.token")
case $SEC in a*) WRONG=b${SEC#?} ;; *) WRONG=a${SEC#?} ;; esac
SHORT=${SEC%?}
echo "  the controller is up; its secret is ${#SEC} characters"

echo "== the report channel"
EXH=$SYS/thermal/exhaust_pwm
cp "$EXH" "$T/exh.start"
expect 200 "$(report run "$SEC")" "a run report with the secret is taken"
if until_ok 25 eval '[ "$(cat "$EXH")" = 65535 ]' && ! cmp -s "$T/exh.start" "$EXH"; then
    ok "the run duty is written: exhaust $(cat "$T/exh.start") -> 65535"
else
    bad "the exhaust stayed at '$(cat "$EXH")' after the run report"
fi
cp "$EXH" "$T/exh.run"
for forged in none wrong short; do
    report run "$SEC" > /dev/null       # the session stays fresh (5 s)
    case $forged in
    none)  code=$(report idle) ;;
    wrong) code=$(report idle "$WRONG") ;;
    short) code=$(report idle "$SHORT") ;;
    esac
    expect 403 "$code" "an idle report with $forged secret is refused"
    sleep 2.5                           # two engine ticks
    if cmp -s "$T/exh.run" "$EXH"; then ok "the exhaust file is byte for byte the same"
    else bad "the forged idle moved the exhaust to '$(cat "$EXH")'"; fi
done
# The ring still plays as the session ends, so the exit takes the
# cooldown duty: a plain file keeps the tail of a longer value, and the
# cooldown duty has the run duty's width where the idle duty does not.
report run "$SEC" > /dev/null
echo running > "$SYS/cnc/state"
expect 200 "$(report idle "$SEC")" "an idle report with the secret is taken"
if until_ok 25 eval '[ "$(cat "$EXH")" = 32768 ]'; then ok "the session ends: exhaust 65535 -> 32768"
else bad "the exhaust stayed at '$(cat "$EXH")' after the idle report"; fi
echo idle > "$SYS/cnc/state"

echo "== the event streams"
for i in 2 3 4; do
    curl -s -N --interface 127.0.0.$i -D "$T/ev$i.h" -o /dev/null "$URL/events" &
    EVS="$EVS $!"
done
until_ok 25 eval 'grep -qs "^HTTP" "$T/ev2.h" && grep -qs "^HTTP" "$T/ev3.h" && grep -qs "^HTTP" "$T/ev4.h"'
if grep -q '^HTTP/1.1 200' "$T/ev2.h" && grep -qi '^content-type: text/event-stream' "$T/ev2.h"; then
    ok "GET /events is 200, text/event-stream"
else bad "GET /events answered $(head -1 "$T/ev2.h")"; fi
grep -q '^HTTP/1.1 200' "$T/ev3.h" && grep -q '^HTTP/1.1 200' "$T/ev4.h" \
    && ok "two more streams from two more peers are 200" || bad "the second and third streams were refused"
expect 503 "$(curl -s --interface 127.0.0.5 --max-time 5 -o /dev/null -w '%{http_code}' "$URL/events")" \
    "a fourth stream beside three held ones is 503"
expect 200 "$(curl -s -o /dev/null -w '%{http_code}' "$URL/settings")" "GET /settings is 200 while they are held"
set -- $EVS
kill $1
EVS="$2 $3"
fourth() {
    curl -s -N --interface 127.0.0.5 --max-time 2 -D "$T/ev5.h" -o /dev/null "$URL/events"
    grep -q '^HTTP/1.1 200' "$T/ev5.h"
}
until_ok 10 eval 'fourth || { sleep 1; false; }' && ok "closing one admits the next" \
    || bad "the fourth stream is still refused after one closed"
for p in $EVS; do kill $p 2>/dev/null; done
EVS=

echo "== the motion routes"
lines() { wc -l < "$T/port.log"; }
last() { tail -1 "$T/port.log"; }
n=$(lines)
expect 403 "$(post /motion/jog - -d x=1)" "an unauthenticated jog is refused"
expect "$n" "$(lines)" "the port got nothing"
expect 400 "$(post /motion/jog "$TOKEN" -d x=one)" "a jog that is not a number is 400"
expect "$n" "$(lines)" "the port got nothing"
expect 200 "$(post /motion/jog "$TOKEN" -d x=1)" "a numeric jog is taken"
case $(last) in "jog G91 G21 X1"*) ok "the port got: $(last)" ;; *) bad "the port's last line is '$(last)'" ;; esac
for op in cancel release energize; do
    expect 200 "$(post /motion/$op "$TOKEN")" "$op is taken"
    expect "$op" "$(last)" "the port got $op"
done
expect 200 "$(post /motion/tray "$TOKEN" -d tray=in)" "tray=in is taken"
expect "tray in" "$(last)" "the port got tray in"
n=$(lines)
expect 400 "$(post /motion/tray "$TOKEN" -d tray=up)" "tray=up is 400"
expect "$n" "$(lines)" "the port got nothing"

echo "== the extension and setup routes"
expect 403 "$(post /ext/call - -d id=x)" "an unauthenticated /ext/call is refused"
expect 409 "$(post /ext/call "$TOKEN" -d id=x)" "/ext/call with extensions off is 409"
grep -q 'extensions are off' "$T/body" && ok "and says extensions are off" || bad "the body is $(cat "$T/body")"
expect 404 "$(post /wiz/pkg:not-a-package/start "$TOKEN")" "a package check that is no wizard is 404"
grep -q 'no such wizard' "$T/body" && ok "and says no such wizard" || bad "the body is $(cat "$T/body")"
expect 403 "$(post /wiz/changed - -d what=tray)" "an unauthenticated /wiz/changed is refused"
expect 400 "$(post /wiz/changed "$TOKEN" -d what=nothing-like-this)" "an unknown change is 400"
curl -s -o "$T/wiz2.json" "$URL/wiz"
if python3 - "$T/wiz2.json" <<'EOF'
import json, sys
st = json.load(open(sys.argv[1]))
have = {w['id'] for w in st.get('wizards', [])}
menu = st.get('changes') or []
named = {w['id'] for c in menu for w in c.get('wizards', [])}
print('  the menu: %d changes naming %d steps; not in the wizard list: %s'
      % (len(menu), len(named), sorted(named - have) or 'none'))
sys.exit(0 if menu and named and named <= have else 1)
EOF
then ok "GET /wiz carries the menu, and every step it names is one of the wizards"
else bad "the menu GET /wiz carries names a step the wizard list does not have"; fi

echo "== the write routes without the token"
n=$(lines)
for p in /controller/stop /controller/start /mode /settings /diag/flow-verify /diag/abort \
         /update/apply /boot /update/check /update/dismiss /system/reboot /restore/factory \
         /restore/factory-return /system/ssh /wiz/advisories/accept /wiz/account /wiz/complete; do
    code=$(post $p -)
    if [ "$code" = 403 ] && grep -q 'authentication required' "$T/body"; then
        ok "POST $p: 403, authentication required"
    else bad "POST $p without the token -> $code $(cat "$T/body")"; fi
done
expect "$n" "$(lines)" "the port got nothing from them"
echo "not firmware" > "$T/small"
code=$(curl -s -o "$T/body" -w '%{http_code}' -X POST -F "file=@$T/small;filename=update.fw" "$URL/update/upload")
case $code in 400|403) ok "an upload without the token is $code" ;; *) bad "an upload without the token -> $code" ;; esac
head -c 100000 /dev/zero | tr '\0' a | sed 's/^/x=/' > "$T/big"
expect 403 "$(post /settings - --data-binary @"$T/big")" "a settings body over the cap without the token is 403"
expect 200 "$(curl -s -o /dev/null -w '%{http_code}' "$URL/status")" "GET /status is 200 after it"

echo "== the Host, Origin and fetch checks on a read"
status_with() { curl -s -o /dev/null -w '%{http_code}' "$@" "$URL/status"; }
NAME=$(uname -n)
expect 403 "$(status_with -H 'Host: evil.example.net')" "Host: evil.example.net is refused"
expect 200 "$(status_with -H "Host: $NAME")" "Host: $NAME (this machine's name) is 200"
expect 403 "$(status_with -H "Host: $NAME.example.net")" "Host: $NAME.example.net is refused"
expect 403 "$(status_with -H 'Origin: http://evil.example.net')" "Origin: http://evil.example.net is refused"
expect 403 "$(status_with -H 'Sec-Fetch-Site: cross-site')" "Sec-Fetch-Site: cross-site is refused"
expect 200 "$(status_with -H 'Sec-Fetch-Site: same-origin' -H 'Origin: http://127.0.0.1')" \
    "Sec-Fetch-Site: same-origin from http://127.0.0.1 is 200"

echo "== the settings' bounds"
CONF=$T/data/forgefirm.conf
cp "$CONF" "$T/conf.before"
expect 400 "$(post /settings "$TOKEN" -d gf_serial=1234567890123)" "a 13-digit gf_serial is 400"
expect 400 "$(post /settings "$TOKEN" -d laser_disarm_s=99999)" "laser_disarm_s=99999 is 400"
cmp -s "$T/conf.before" "$CONF" && ok "the settings file is byte for byte the same" \
    || bad "a refused value changed the settings file"
expect 200 "$(post /settings "$TOKEN" -d laser_disarm_s=120)" "laser_disarm_s=120 is 200"
grep -qx 'laser_disarm_s = 120' "$CONF" && ok "the settings file holds it" \
    || bad "the settings file does not hold laser_disarm_s = 120"

echo "== the page"
expect 200 "$(curl -s -D "$T/page.h" -o /dev/null -w '%{http_code}' "$URL/")" "GET / is 200"
tr -d '\r' < "$T/page.h" | grep -qix "content-security-policy: frame-src 'none'" \
    && ok "its Content-Security-Policy is frame-src 'none'" \
    || bad "its Content-Security-Policy is '$(tr -d '\r' < "$T/page.h" | grep -i '^content-security-policy')'"

echo "== the secret ends with its controller"
CP=$(cat "$T/ctl.pid")
kill $CP
until_ok 50 eval '! alive $CP' || bad "the controller's stand-in did not exit"
expect 403 "$(report run "$SEC")" "the exited controller's secret is refused"

stop_daemon
[ ! -e "$T/cloud.started" ] || bad "the cloud client's stand-in started"
if [ $fails -eq 0 ]; then
    echo "PASS: host_routes"
else
    echo "FAIL: host_routes, $fails checks"
    echo "--- the daemon's output"
    tail -60 "$T/out"
    exit 1
fi
