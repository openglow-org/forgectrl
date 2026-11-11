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
# controller's port: it answers state in the controller's form and every
# other line ok. One case per route, against that daemon:
#
#   the report channel: a run report with the secret moves the exhaust
#     duty; an idle report with no secret, a wrong one, or one of the
#     wrong length is refused and leaves the duty file as it was; an idle
#     report with the secret ends the session, and a quiet hold may not
#     take the pump while the engine cools down after it; the secret dies
#     with its controller
#   /events: the stream, the cap, the reads beside it, a freed place
#   the motion routes: the token, the numbers, a line on the port each;
#     GET /motion/state passing the port's state on, GET /motion/sender,
#     and the panel page's tray switch
#   /ext/call with extensions off, a package check that is no wizard,
#   /wiz/changed, and the menu GET /wiz carries
#   the write routes without the token, an upload (nothing staged), a
#     4 MiB body, and the
#     fuse view without the token; the Host, Origin and Sec-Fetch-Site
#     checks on a read
#   the license bundle (FORGECTRL_LICENSES, a fixture in the image's
#     form): the manifest, the bundle and the Licenses page, each read
#     with no token
#   the settings' bounds, in the file FORGECTRL_CONF names: each refusal
#     leaves the file byte for byte, a write changes its own key alone,
#     and an empty value clears it
#   the XY microstep mode: a write that changes the mode in force, an
#     unset key running at x32, restarts the controller; one that does
#     not leaves it running
#   the page (its frame policy, the token, nothing external, the theme
#     and the save bar, no dev bar, and none on the certificate page),
#     /status's switches and its grbl block only
#     while the controller runs, /cam/status, the dose-curve reads, and
#     the controller's settings store in the data directory
#   scoped tokens: each refused a route it was not given, and neither
#     in the list nor in the store (mode 0600)
#   the extension host's credential (FORGECTRL_EXT_HOST_TOKEN): a 0600
#     file; a package's jog through it is taken as ext:<id>, and the
#     same credential is refused a camera
#   SSH until the reboot, sshd's init a stand-in (FORGECTRL_SSHD_INIT),
#     and one outside its form runs nothing
#   the login over HTTPS: the lock, the cookie, the logout
#   the cooling engine's attention on a fresh start (no coolant channel
#     in the sysfs tree: SENSOR stands): GET /cool/flow with the watch
#     off; the Acknowledge refused while SENSOR stands; the channels
#     readable, SENSOR cleared at idle is a notice and the Acknowledge is
#     taken; a dismissal up to an earlier seq leaves the notice, one with
#     no upto takes it
#   the button held as proof of an operator (the switch device a file,
#     its ioctl a preloaded stand-in; GF_SLOT_LIST the slot inventory,
#     GF_REBOOT_FILE the reboot): a login that cannot make a session (the
#     stand-in refuses /dev/urandom while a flag file stands) is a JSON
#     500; the reset of every setting, an unsigned
#     install and the fuse view each refuse 403 with the button up; under
#     an armed job the reset, the unsigned install, /ext/key, /ext/install
#     and the fuse view each answer 409 before the button is asked for, and
#     so do the fuse view beside a paused or a running GRBL job, the fuse
#     view and the reset while the GRBL controller's state file says it is
#     arming, and the reset with the next boot on the other slot; with the
#     button held the reset is still refused under the lease, on a machine
#     that is not idle, and with a request that cannot be written or a
#     reboot that cannot be scheduled, leaving no request; then it is
#     taken, its request written and the reboot scheduled; GET /slots reads
#     the inventory, and POST /system/reboot schedules its reboot 1 s on;
#     with the controller stopped, a paused job's state file refuses nothing
#   a dev image (GF_DEV_MARKER): a setup seed with no record becomes the
#     record at start, with the account the users file holds; the login
#     page, the panel and the setup
#     each carry the dev bar above the header, with where the boot's
#     settings are (GF_DATA_STATE, the boot step's record); a card boot
#     whose data partition is not in use refuses a mode switch; the
#     certificate page, the licenses page and the printable record carry
#     the bar inline after <body>; the bar's data line for each word the
#     record can hold
#
# The daemon runs on the image's ulfius (tests/ulfius_image.sh): the
# distribution's library cuts the peer address that the loopback checks
# read. Needs a C compiler (cc). Runs unprivileged; build.yml runs it:
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
        GF_RUN_DIR="$T/run" GF_SYSFS_ROOT="$SYS/" GF_SWITCH_DEV="${SWITCH_DEV:-$T/no-switches}" \
        GF_SWITCH_WORD_FILE="$T/switches" GF_PULSE_DEV="$T/pulse" \
        FORGECTRL_GRBL_BIN="$T/bin/grbl" FORGECTRL_CLOUD_BIN="$T/bin/cloud" \
        FORGECTRL_SSHD_INIT="${SSHD_INIT:-$T/bin/sshd-init}" FORGECTRL_EXT_HOST_TOKEN="$T/run/ext-host.token" \
        FORGECTRL_LICENSES="$T/licenses.tar.gz" GF_DEV_MARKER="${DEV_MARKER:-}" \
        GF_DATA_STATE="${DATA_STATE:-}" GF_SLOT_LIST="${SLOT_LIST:-}" GF_REBOOT_FILE="${REBOOT_FILE:-}" \
        LD_PRELOAD="${PRELOAD:-}" SWSHIM_NO_URANDOM="$T/no-urandom" \
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
# The GRBL controller's stand-in: it records its pid, its arguments and
# the secret it was handed, and stays up. The cloud client's must never
# start. sshd's init script is a stand-in that records each action.
cat > "$T/bin/grbl" <<EOF
#!/bin/sh
echo \$\$ >> "$T/ctl.pids"
echo \$\$ > "$T/ctl.pid"
echo "\$*" > "$T/ctl.args"
printf '%s\n' "\$GF_REPORT_SECRET" > "$T/ctl.secret.new"
mv "$T/ctl.secret.new" "$T/ctl.secret"
exec sleep 600
EOF
printf '#!/bin/sh\necho "$0" >> "%s/cloud.started"\n' "$T" > "$T/bin/cloud"
printf '#!/bin/sh\necho "$1" >> "%s/sshd.log"\n' "$T" > "$T/bin/sshd-init"
chmod +x "$T/bin/grbl" "$T/bin/cloud" "$T/bin/sshd-init"
# The license bundle in the image's form: a gzip tar holding
# common-licenses/license.manifest. The daemon hands its path to tar in
# single quotes, and mktemp's name has none.
mkdir -p "$T/lic/common-licenses"
printf 'PACKAGE NAME: forgectrl\nPACKAGE VERSION: 1.0\nRECIPE NAME: forgectrl\nLICENSE: MIT\n\n' \
    > "$T/lic/common-licenses/license.manifest"
tar -C "$T/lic" -czf "$T/licenses.tar.gz" common-licenses

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
    'completed': ts,
}
json.dump(rec, open(sys.argv[2], 'w'), indent=2)
EOF

# The controller's port: every line is recorded.
python3 - "$T/run/grbl.ctl" "$T/port.log" <<'EOF' &
import socket, sys, threading
path, log = sys.argv[1], sys.argv[2]
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(path)
srv.listen(4)
# state is answered in the controller's form (ctlport.c), with the sender
# kept out as the last sender line said (the sender keeper pushes it again
# when the state disagrees); every other line ok
STATE = (b'{"state":"Idle","sender":false,"port_jog":false,"released":false,"mpos":[0.000,0.000,0.000],'
         b'"homed":3,"envelope_open":false,"mcode":null,"sender_out":%s,"tray":"in"}\n')
out = False
def serve(c):
    global out
    for line in c.makefile('rb'):
        with open(log, 'ab') as o:
            o.write(line)
        word = line.strip()
        if word == b'sender out':
            out = True
        elif word == b'sender in':
            out = False
        c.sendall(STATE % (b'true' if out else b'false') if word == b'state' else b'ok\n')
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
# The machine idle and the engine cooling down after the session: a quiet
# hold may not take the pump.
code=$(post /cool/quiet "$TOKEN" -d on=1 -d pump=1)
if [ "$code" = 409 ] && grep -q 'the pump stays on' "$T/body"; then
    ok "POST /cool/quiet pump=1 while the engine cools down: 409, the pump stays on"
else bad "POST /cool/quiet pump=1 while the engine cools down -> $code $(cat "$T/body")"; fi

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
# The daemon's own keepers speak on the port on their clock, whatever the
# routes do: the M-code relay pushes its table (mcodes) and the sender
# keeper reads the controller's state (state), each every 2 s. What the
# routes sent is the port's log without those two lines.
routed() { grep -v -e '^state$' -e '^mcodes ' "$T/port.log"; }
lines() { routed | wc -l; }
last() { routed | tail -1; }
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
code=$(curl -s -o "$T/body" -w '%{http_code}' "$URL/motion/state")
expect 200 "$code" "GET /motion/state is 200"
expect True "$(python3 -c 'import json, sys; d = json.load(open(sys.argv[1])); print("state" in d and "sender" in d)' "$T/body" 2>&1)" \
    "its body has state and sender"
expect 200 "$(curl -s -o "$T/body" -w '%{http_code}' "$URL/motion/sender")" "GET /motion/sender is 200"
expect 200 "$(curl -s -o "$T/page.html" -w '%{http_code}' "$URL/")" "the panel page is 200"
for want in 'id="traybtn"' trayToggle /motion/tray; do
    grep -qF "$want" "$T/page.html" && ok "and carries $want" || bad "the panel page lacks $want"
done

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
for p in /controller/stop /controller/start /mode /settings /diag/aa-offset-calibrate /diag/abort \
         /update/apply /boot /update/check /update/dismiss /system/reboot /system/reset /restore/factory \
         /restore/factory-return /system/ssh /wiz/advisories/accept /wiz/account /wiz/complete \
         /cool/attention/ack /cool/notices/dismiss; do
    code=$(post $p -)
    if [ "$code" = 403 ] && grep -q 'authentication required' "$T/body"; then
        ok "POST $p: 403, authentication required"
    else bad "POST $p without the token -> $code $(cat "$T/body")"; fi
done
expect "$n" "$(lines)" "the port got nothing from them"
echo "not firmware" > "$T/small"
code=$(curl -s -o "$T/body" -w '%{http_code}' -X POST -F "file=@$T/small;filename=update.fw" "$URL/update/upload")
if [ "$code" = 403 ] && grep -q 'authentication required' "$T/body"; then
    ok "an upload without the token: 403, authentication required"
else bad "an upload without the token -> $code $(cat "$T/body")"; fi
[ ! -e "$T/data/upload.fw" ] && ok "and nothing is staged" || bad "an upload without the token staged data/upload.fw"
head -c 4194304 /dev/zero | tr '\0' a | sed 's/^/x=/' > "$T/big"
expect 403 "$(post /settings - --data-binary @"$T/big")" "a 4 MiB settings body without the token is 403"
expect 200 "$(curl -s -o /dev/null -w '%{http_code}' "$URL/status")" "GET /status is 200 after it"
code=$(curl -s -o "$T/body" -w '%{http_code}' "$URL/fuse-identity")
if [ "$code" = 403 ] && grep -q 'authentication required' "$T/body"; then
    ok "GET /fuse-identity without the token: 403, authentication required"
else bad "GET /fuse-identity without the token -> $code $(cat "$T/body")"; fi

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

echo "== the license bundle, read with no token and no Origin"
LIC=$T/licenses.tar.gz
expect 200 "$(curl -s -o "$T/body" -w '%{http_code}' "$URL/system/licenses/manifest")" \
    "GET /system/licenses/manifest is 200"
cmp -s "$T/lic/common-licenses/license.manifest" "$T/body" && ok "its body is the manifest" \
    || bad "its body is not the manifest: $(cat "$T/body")"
expect 200 "$(curl -s -o "$T/lic.got" -w '%{http_code}' "$URL/system/licenses")" "GET /system/licenses is 200"
expect "$(wc -c < "$LIC")" "$(wc -c < "$T/lic.got")" "the bundle is as long as the file"
tar -tzf "$T/lic.got" 2>/dev/null | grep -qx common-licenses/license.manifest \
    && ok "the served tar holds common-licenses/license.manifest" || bad "the served tar holds no manifest"
expect 200 "$(curl -s -o "$T/lic.html" -w '%{http_code}' "$URL/licenses")" "GET /licenses is 200"
grep -qF /system/licenses "$T/lic.html" && grep -qF 'PACKAGE NAME:' "$T/lic.html" \
    && ok "the page carries /system/licenses and the manifest" || bad "the page lacks the link or the manifest"

echo "== the settings' bounds"
CONF=$T/data/forgefirm.conf
cp "$CONF" "$T/conf.before"
# refused <what> <curl args>: 400 (404 too for a key that is no setting),
# and the settings file byte for byte the same
refused() {
    what=$1; shift
    code=$(post /settings "$TOKEN" "$@")
    case $code in 400|404) ;; *) bad "$what -> $code"; return ;; esac
    cmp -s "$T/conf.before" "$CONF" && ok "$what is $code, and the file is byte for byte the same" \
        || bad "$what is $code, and it changed the settings file"
}
refused "a 13-digit gf_serial" -d gf_serial=1234567890123
refused "a 300-character gf_serial" -d gf_serial=$(head -c 300 /dev/zero | tr '\0' X)
refused "laser_disarm_s=99999" -d laser_disarm_s=99999
refused "pulse_reject_threshold_bytes=2000000000" -d pulse_reject_threshold_bytes=2000000000
refused "a key that is no setting" -d no_such_key_hostroutes=1
for v in 256 -1 bright; do refused "lid_lamp_idle=$v" -d lid_lamp_idle=$v; done
expect 200 "$(post /settings "$TOKEN" -d laser_disarm_s=120)" "laser_disarm_s=120 is 200"
grep -qx 'laser_disarm_s = 120' "$CONF" && ok "the settings file holds it" \
    || bad "the settings file does not hold laser_disarm_s = 120"
grep -v '^laser_disarm_s ' "$CONF" > "$T/conf.others"
expect 200 "$(post /settings "$TOKEN" -d laser_disarm_s=90)" "laser_disarm_s=90, a key the file holds, is 200"
grep -qx 'laser_disarm_s = 90' "$CONF" && grep -v '^laser_disarm_s ' "$CONF" | cmp -s - "$T/conf.others" \
    && ok "the file holds 90, and every other line is as it was" || bad "the write changed another key: $(cat "$CONF")"
# cleared by an empty query parameter: an empty form field is no field
expect 200 "$(post '/settings?laser_disarm_s=' "$TOKEN")" "an empty laser_disarm_s is 200"
cmp -s "$T/conf.before" "$CONF" && ok "the key is gone: the file is as it was before the writes" \
    || bad "the cleared key left the file as: $(cat "$CONF")"
# The flow watch's settings and the sensors logger's levels: a value
# outside each one's range refused, each alone; the six at the ends of
# their ranges taken in one write, then cleared in one.
refused "cool_flow_watch=2" -d cool_flow_watch=2
refused "cool_flow_hold_c=0.4" -d cool_flow_hold_c=0.4
refused "cool_flow_duty_min=4.5" -d cool_flow_duty_min=4.5
refused "cool_flow_duty_max=26" -d cool_flow_duty_max=26
refused "log_sensors_disk=loud" -d log_sensors_disk=loud
refused "log_sensors_remote=loud" -d log_sensors_remote=loud
FLOWSET='cool_flow_watch=0 cool_flow_hold_c=3 cool_flow_duty_min=4 cool_flow_duty_max=25 log_sensors_disk=debug log_sensors_remote=off'
expect 200 "$(post /settings "$TOKEN" $(for kv in $FLOWSET; do echo "-d $kv"; done))" "the six at their ranges' ends in one write: 200"
held=0
for kv in $FLOWSET; do grep -qx "${kv%%=*} = ${kv#*=}" "$CONF" && held=$((held + 1)); done
expect 6 "$held" "and the settings file holds each of them"
CLEAR=$(for kv in $FLOWSET; do printf '%s=&' "${kv%%=*}"; done)
expect 200 "$(post "/settings?${CLEAR%&}" "$TOKEN")" "clearing the six is 200"
cmp -s "$T/conf.before" "$CONF" && ok "and the file is as it was before the writes" \
    || bad "the cleared keys left the file as: $(cat "$CONF")"
# The lid lamp under the sysfs root follows lid_lamp_idle at once: a level
# is written to it, a refused value writes nothing, and a cleared key
# writes the default.
mkdir -p "$SYS/pic"
lamp() { cat "$SYS/pic/lid_led" 2>/dev/null; }
expect 200 "$(post /settings "$TOKEN" -d lid_lamp_idle=100)" "lid_lamp_idle=100 is 200"
expect 100 "$(lamp)" "and pic/lid_led reads 100"
expect 400 "$(post /settings "$TOKEN" -d lid_lamp_idle=bright)" "lid_lamp_idle=bright is 400"
expect 100 "$(lamp)" "and pic/lid_led is as it was"
expect 200 "$(post '/settings?lid_lamp_idle=' "$TOKEN")" "an empty lid_lamp_idle is 200"
expect 236 "$(lamp)" "and pic/lid_led reads the default, 236"

echo "== an XY microstep change restarts the controller"
# An unset key runs at x32, the controller's own default. A write that
# changes the mode in force restarts the idle controller before the reply
# (the stop is waited for); one that does not leaves it running. Each
# case reads the running controller's pid just before its write.
up() { until_ok 150 eval 'alive "$(cat "$T/ctl.pid")"' || fatal "no controller running"; cat "$T/ctl.pid"; }
# restarted <pid before> <what>: the controller that ran is gone, a new one runs
restarted() {
    if ! alive "$1" && until_ok 150 eval '[ "$(cat "$T/ctl.pid")" != "$1" ] && alive "$(cat "$T/ctl.pid")"'; then
        ok "and the controller was restarted for $2 (pid $1 -> $(cat "$T/ctl.pid"))"
    else
        bad "the controller was not restarted for $2 (pid $1, now $(cat "$T/ctl.pid"))"
    fi
}
P=$(up)
expect 200 "$(post /settings "$TOKEN" -d xy_microsteps=32)" "xy_microsteps=32 over the unset key is 200"
alive "$P" && expect "$P" "$(cat "$T/ctl.pid")" "and the controller runs on: x32 is the unset key's mode" \
    || bad "x32 over the unset key stopped the controller"
expect 200 "$(post '/settings?xy_microsteps=' "$TOKEN")" "clearing it again is 200"
P=$(up)
expect 200 "$(post /settings "$TOKEN" -d xy_microsteps=8)" "xy_microsteps=8 over the unset key is 200"
restarted "$P" x8
P=$(up)
expect 200 "$(post '/settings?xy_microsteps=' "$TOKEN")" "clearing x8 is 200"
restarted "$P" x32
cmp -s "$T/conf.before" "$CONF" && ok "the settings file is as it was before the writes" \
    || bad "the microstep writes left the file as: $(cat "$CONF")"
# The restarted controller was handed a new secret.
until_ok 50 eval '[ "$(cat "$T/ctl.secret")" != "$SEC" ]' || bad "the restarted controller has its predecessor's secret"
SEC=$(cat "$T/ctl.secret")

echo "== the page and the reads beside it"
expect 200 "$(curl -s -D "$T/page.h" -o "$T/page.html" -w '%{http_code}' "$URL/")" "GET / is 200"
tr -d '\r' < "$T/page.h" | grep -qix "content-security-policy: frame-src 'none'" \
    && ok "its Content-Security-Policy is frame-src 'none'" \
    || bad "its Content-Security-Policy is '$(tr -d '\r' < "$T/page.h" | grep -i '^content-security-policy')'"
grep -qF "$TOKEN" "$T/page.html" && ok "the page carries the panel token" || bad "the page lacks the panel token"
grep -q -e '<link ' -e '<script src=' "$T/page.html" && bad "the page references an external asset" \
    || ok "no external link or script"
for want in data-bs-theme 'id="savebar"'; do
    grep -qF "$want" "$T/page.html" && ok "the page carries $want" || bad "the page lacks $want"
done
grep -qF -e 'class="ff-devbar"' -e '<!--FFDEV-->' "$T/page.html" \
    && bad "a release image's page carries the dev bar or its mark" || ok "no dev bar and no mark on a release image"
expect 200 "$(curl -s -o "$T/cert.html" -w '%{http_code}' "$URL/cert")" "GET /cert is 200"
grep -qF 'ff-devbar' "$T/cert.html" && bad "a release image's certificate page carries the dev bar" \
    || ok "and carries no dev bar on a release image"
curl -s -o "$T/status.json" "$URL/status"
expect True "$(python3 -c 'import json, sys
d = json.load(open(sys.argv[1]))
print("state" in d and all(k in (d.get("switches") or {}) for k in ("lid", "button", "interlock_ok", "head", "hv_enable")))' \
    "$T/status.json" 2>&1)" "/status has state, and the switches lid, button, interlock_ok, head, hv_enable"
# The controller's state file, in the GRBL controller's shape: /status
# echoes it as the grbl block while the controller runs.
python3 - "$T/run/grbl.state" <<'EOF'
import json, sys, time
json.dump({'ts_mono': round(time.monotonic(), 3), 'state': 'Idle', 'sender': False,
           'laser': {'model': 'density'}, 'modals': {}}, open(sys.argv[1], 'w'))
EOF
grbl_block() {
    curl -s -o "$T/status.json" "$URL/status"
    python3 -c 'import json, sys; g = json.load(open(sys.argv[1])).get("grbl"); print(g["report"]["state"] if g else "none")' \
        "$T/status.json" 2>&1
}
expect Idle "$(grbl_block)" "with the controller running, /status carries the grbl block"
code=$(curl -s -o "$T/body" -w '%{http_code}' "$URL/cam/status")
expect '200|True' "$code|$(python3 -c 'import json, sys; print("running" in json.load(open(sys.argv[1])))' "$T/body" 2>&1)" \
    "GET /cam/status is 200 with running"
code=$(curl -s -o "$T/body" -w '%{http_code}' "$URL/curve/status")
expect '200|True' "$code|$(python3 -c 'import json, sys
print(json.load(open(sys.argv[1])).get("state") in ("idle", "waiting", "recording", "done", "failed"))' "$T/body" 2>&1)" \
    "GET /curve/status is 200, in one of its states"
expect 200 "$(curl -s -o "$T/ladder" -w '%{http_code}' "$URL/curve/ladder.gcode")" "GET /curve/ladder.gcode is 200"
grep -q S1000 "$T/ladder" && grep -q M5 "$T/ladder" && ok "and holds S1000 and M5" || bad "the ladder lacks S1000 or M5"
case " $(cat "$T/ctl.args") " in
    *" -e $T/data/EEPROM-glowforge.DAT "*) ok "the controller's settings store is in the data directory" ;;
    *) bad "the controller was started with '$(cat "$T/ctl.args")'" ;;
esac
[ ! -e "$T/EEPROM-glowforge.DAT" ] && ok "and nothing of it is above the data directory" \
    || bad "a settings store is above the data directory"

# jv <python expression over d>: from the JSON body in $T/body
jv() { python3 -c 'import json, sys; d = json.load(open(sys.argv[1])); print(eval(sys.argv[2]))' "$T/body" "$1" 2>&1; }
# bearer <token> <method> <path> [curl args]: the status, the body in $T/body
bearer() {
    b=$1; m=$2; p=$3; shift 3
    curl -s -o "$T/body" -w '%{http_code}' -X "$m" -H "Authorization: Bearer $b" "$@" "$URL$p"
}

echo "== scoped tokens"
expect 200 "$(post /tokens "$TOKEN" -d name=reader -d caps=machine.read)" "a machine.read token is made"
RTOK=$(jv 'd["token"]'); RID=$(jv 'd["id"]')
expect 200 "$(post /tokens "$TOKEN" -d name=jogger -d caps=motion.jog)" "a motion.jog token is made"
JTOK=$(jv 'd["token"]'); JID=$(jv 'd["id"]')
n=$(lines)
expect 403 "$(bearer "$RTOK" POST /motion/jog -d x=1)" "the machine.read token's jog is 403"
expect "$n" "$(lines)" "the port got nothing"
expect 403 "$(bearer "$JTOK" GET /status)" "the motion.jog token's GET /status is 403"
expect 200 "$(bearer "$RTOK" GET /status)" "the machine.read token's GET /status is 200"
expect 200 "$(bearer "$JTOK" POST /motion/jog -d x=1)" "the motion.jog token's jog is 200"
expect $((n + 1)) "$(lines)" "and the port got one line"
expect 200 "$(curl -s -o "$T/body" -w '%{http_code}' -H "X-ForgeFIRM-Token: $TOKEN" "$URL/tokens")" "GET /tokens is 200"
expect 'jogger,reader' "$(jv '",".join(sorted(t["name"] for t in d["tokens"]))')" "it lists both"
LISTED=$(cat "$T/body")
case $LISTED in *"${RTOK#fft_}"*|*"${JTOK#fft_}"*|*sha*|*hash*) bad "the list carries a token or a hash: $LISTED" ;;
    *) ok "the list carries neither token and no hash" ;; esac
expect 600 "$(stat -c %a "$T/data/tokens")" "the store is mode 0600"
grep -q -e "${RTOK#fft_}" -e "${JTOK#fft_}" "$T/data/tokens" && bad "the store holds a token" \
    || ok "the store holds neither token"
for id in $RID $JID; do post /tokens/revoke "$TOKEN" -d id=$id > /dev/null; done

echo "== the extension host's credential"
CRED_FILE=$T/run/ext-host.token
expect 600 "$(stat -c %a "$CRED_FILE")" "the daemon minted it into a 0600 file"
CRED=$(cat "$CRED_FILE")
PKG=org.example.hostroutes
# host <path> [curl args]: a POST with the credential, as the host sends it
host() { p=$1; shift; curl -s -o "$T/body" -w '%{http_code}' -X POST -H "X-ForgeFIRM-Token: $CRED" "$@" "$URL$p"; }
expect 200 "$(host /motion/sender -d id=$PKG -d out=1)" "with it, $PKG keeps the sender out"
curl -s -o "$T/body" "$URL/status"
expect "ext:$PKG" "$(jv '(d.get("lease") or {}).get("holder") and d["lease"]["holder"]["owner"]')" \
    "the machine's lease holder is ext:$PKG"
n=$(lines)
expect 409 "$(post /motion/jog "$TOKEN" -d x=1)" "the panel's jog beside it is 409"
expect 409 "$(host /motion/jog -d id=org.example.other -d x=1)" "the credential's jog for another package is 409"
expect "$n" "$(lines)" "the port got nothing"
expect 200 "$(host /motion/jog -d id=$PKG -d x=1)" "the credential's jog for $PKG is taken as ext:$PKG"
case $(last) in "jog G91 G21 X1"*) ok "the port got: $(last)" ;; *) bad "the port's last line is '$(last)'" ;; esac
expect 403 "$(curl -s -o /dev/null -w '%{http_code}' -H "X-ForgeFIRM-Token: $CRED" "$URL/cam/snapshot?cam=lid")" \
    "the same credential on GET /cam/snapshot is 403"
# let through to the camera's own refusal: with no switch device the lid
# reads open, and the snapshot is 409 for it, not 403 from the guard
expect 409 "$(curl -s -o "$T/body" -w '%{http_code}' -H "X-ForgeFIRM-Token: $TOKEN" "$URL/cam/snapshot?cam=lid")" \
    "where the panel token is let through: 409, the camera's lid refusal"
expect 200 "$(host /motion/sender -d id=$PKG -d out=0)" "$PKG lets the sender back in"

echo "== SSH until the next reboot"
FLAG=$T/run/ssh-enabled
expect 200 "$(curl -s -o "$T/body" -w '%{http_code}' -H "X-ForgeFIRM-Token: $TOKEN" "$URL/system/ssh")" "GET /system/ssh is 200"
expect 'True|True|True' "$(jv '"|".join(str(isinstance(d.get(k), bool)) for k in ("enabled", "running", "dev_image"))')" \
    "enabled, running and dev_image are booleans"
expect 200 "$(post /system/ssh "$TOKEN" -d enable=1)" "enable=1 is 200"
[ -e "$FLAG" ] && ok "the flag is written" || bad "no flag at $FLAG"
expect start "$(tail -1 "$T/sshd.log" 2>/dev/null)" "and sshd's init is told start"
expect 400 "$(post /system/ssh "$TOKEN" -d enable=2)" "enable=2 is 400"
[ -e "$FLAG" ] && expect 1 "$(wc -l < "$T/sshd.log")" "the flag and the init are as they were" \
    || bad "enable=2 removed the flag"
expect 200 "$(post /system/ssh "$TOKEN" -d enable=0)" "enable=0 is 200"
[ ! -e "$FLAG" ] && ok "the flag is removed" || bad "the flag is still there"
expect stop "$(tail -1 "$T/sshd.log")" "and sshd's init is told stop"

echo "== the login"
TLS=https://127.0.0.1:$TLS_PORT
NAME=hostroutes
PW=$(python3 -c 'import secrets; print(secrets.token_hex(8))')
expect 200 "$(post /wiz/account "$TOKEN" -d name=$NAME -d password=$PW)" "a temporary account is made"
login() { curl -sk -D "$T/head" -o "$T/body" -w '%{http_code}' -X POST -d "name=$1" -d "password=$2" "$TLS/login"; }
session() { curl -sk -o "$T/body" -H "Cookie: ffsid=$1" "$TLS/wiz"; jv 'd.get("session")'; }
expect 401 "$(login $NAME not-$PW)" "a wrong password is 401"
expect True "$(jv 'bool(d.get("error"))')" "with a JSON error, the one the login page shows"
codes=
i=0
while [ $i -lt 6 ]; do
    c=$(login $NAME not-$PW-$i)
    codes="$codes $c"
    [ "$c" = 429 ] && break
    i=$((i + 1))
done
case "$codes " in *" 429 ") ok "wrong passwords lock the address:$codes" ;; *) bad "no lock after wrong passwords:$codes" ;; esac
WAIT=$(jv 'd.get("wait")')
expect 429 "$(login $NAME $PW)" "the right password inside the lock is 429"
sleep $((WAIT + 1))
expect 401 "$(login not-$NAME $PW)" "after the lock, a wrong name is 401"
expect 200 "$(login $NAME $PW)" "the right pair is 200"
SID=$(tr -d '\r' < "$T/head" | sed -n 's/^[Ss]et-[Cc]ookie: ffsid=\([0-9a-f]*\).*/\1/p' | head -1)
[ -n "$SID" ] && ok "and sets the session cookie" || bad "no session cookie: $(grep -i set-cookie "$T/head")"
expect True "$(session "$SID")" "GET /wiz with the cookie sees the session"
expect 200 "$(curl -sk -D "$T/head" -o /dev/null -w '%{http_code}' -X POST -H "Cookie: ffsid=$SID" "$TLS/logout")" \
    "POST /logout is 200"
tr -d '\r' < "$T/head" | grep -qi '^set-cookie: ffsid=.*max-age=0' && ok "and clears the cookie" \
    || bad "the logout did not clear the cookie"
expect False "$(session "$SID")" "GET /wiz with the old cookie sees no session"

echo "== the secret ends with its controller"
CP=$(cat "$T/ctl.pid")
kill $CP
until_ok 50 eval '! alive $CP' || bad "the controller's stand-in did not exit"
expect 403 "$(report run "$SEC")" "the exited controller's secret is refused"
until_ok 50 eval '[ "$(grbl_block)" = none ]' && ok "/status no longer carries the grbl block" \
    || bad "the grbl block outlived its controller: $(grbl_block)"

echo "== an sshd init override outside its form runs nothing"
stop_daemon
cp "$T/bin/sshd-init" "$T/bin/sshd init"     # a runnable file, at a path with a space
n=$(wc -l < "$T/sshd.log")
SSHD_INIT="$T/bin/sshd init" start_daemon
expect 200 "$(post /system/ssh "$TOKEN" -d enable=1)" "enable=1 is 200 with the init refused"
[ -e "$FLAG" ] && ok "and the flag is written as before" || bad "no flag at $FLAG"
expect "$n" "$(wc -l < "$T/sshd.log")" "and nothing was run"
grep -q "FORGECTRL_SSHD_INIT is not an absolute path" "$T/out" && ok "the refusal names the variable, not the value" || bad "no refusal in the log"
! grep -q "sshd init" "$T/out" && ok "the value is not in the log" || bad "the log carries the value"
expect 200 "$(post /system/ssh "$TOKEN" -d enable=0)" "enable=0 is 200"
expect "$n" "$(wc -l < "$T/sshd.log")" "and the stop ran nothing either"

echo "== the cooling engine's Acknowledge, its notices and the flow watch"
# This start has run no session, and the sysfs tree has no coolant
# channel: SENSOR stands 2 s after the start, live, and is no fail tier.
expect 200 "$(curl -s -o "$T/body" -w '%{http_code}' "$URL/cool/flow")" "GET /cool/flow is 200"
expect off "$(jv 'd["watch"]')" "and the watch is off, the setting unset"
coolv() { curl -s -o "$T/cool.json" "$URL/cool/status"; python3 -c 'import json, sys
d = json.load(open(sys.argv[1])); print(eval(sys.argv[2]))' "$T/cool.json" "$1" 2>&1; }
live() { [ "$(coolv 'int(d["attention"]["live"])')" = 1 ]; }
until_ok 25 live || bad "SENSOR is not live with no coolant channel: $(cat "$T/cool.json")"
code=$(post /cool/attention/ack "$TOKEN")
expect "409|the failure still stands" "$code|$(cat "$T/body")" "POST /cool/attention/ack while SENSOR stands is 409"
# The channels readable (694 counts, 22.8 C): SENSOR clears at idle, so it
# leaves a notice and nothing waits for the Acknowledge.
echo 694 > "$SYS/pic/water_temp_1"
echo 694 > "$SYS/pic/water_temp_2"
noticed() { [ "$(coolv 'int(not d["attention"]["live"] and len(d["notices"]) == 1)')" = 1 ]; }
until_ok 50 noticed || bad "SENSOR did not clear into one notice: $(cat "$T/cool.json")"
SEQ=$(coolv 'd["notices"][0]["seq"]')
code=$(post /cool/attention/ack "$TOKEN")
expect "200|True" "$code|$(jv 'd.get("acknowledged")')" "SENSOR cleared: POST /cool/attention/ack is 200, acknowledged"
expect "200|1" "$(post /cool/notices/dismiss "$TOKEN" -d upto=$((SEQ - 1)))|$(coolv 'len(d["notices"])')" \
    "a dismissal up to the seq before the notice's leaves it"
expect "200|0" "$(post /cool/notices/dismiss "$TOKEN")|$(coolv 'len(d["notices"])')" \
    "a dismissal with no upto takes every notice"
rm -f "$SYS/pic/water_temp_1" "$SYS/pic/water_temp_2"

echo "== the button held as proof of an operator, and the jobs that would take the hold"
# The daemon again, with the machine button to hold: the switch device is
# the switch word's file, read through a stand-in for the device's ioctl
# (bit 2 is the button, bit 3 the closed lid); the slot inventory is a
# file that says the next boot comes back to the booted slot; a reboot is
# written to a file. The same stand-in refuses to open /dev/urandom while
# the file SWSHIM_NO_URANDOM names stands, so a login cannot make its
# session.
stop_daemon
cat > "$T/swshim.c" <<'EOF'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int urandom_refused(const char *path)
{
    const char *flag = getenv("SWSHIM_NO_URANDOM");
    return path && !strcmp(path, "/dev/urandom") && flag && access(flag, F_OK) == 0;
}

static int open_next(const char *sym, const char *path, int flags, va_list ap)
{
    mode_t mode = (flags & (O_CREAT | O_TMPFILE)) ? (mode_t)va_arg(ap, int) : 0;
    if (urandom_refused(path)) {
        errno = EACCES;
        return -1;
    }
    int (*real)(const char *, int, ...) = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, sym);
    return real(path, flags, mode);
}

int open(const char *path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    int fd = open_next("open", path, flags, ap);
    va_end(ap);
    return fd;
}

int open64(const char *path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    int fd = open_next("open64", path, flags, ap);
    va_end(ap);
    return fd;
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (_IOC_TYPE(req) == 'E' && _IOC_NR(req) == _IOC_NR(EVIOCGSW(0)) && _IOC_DIR(req) == _IOC_READ) {
        const char *wf = getenv("GF_SWITCH_WORD_FILE");
        FILE *f = wf ? fopen(wf, "r") : NULL;
        unsigned long word;
        int got = f && fscanf(f, "%lu", &word) == 1;
        if (f)
            fclose(f);
        if (!got)
            return -1;
        unsigned char *b = arg;
        for (unsigned i = 0; i < _IOC_SIZE(req); i++)
            b[i] = (unsigned char)(word >> (8 * i));
        return (int)_IOC_SIZE(req);
    }
    static int (*real)(int, unsigned long, ...);
    if (!real)
        real = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    return real(fd, req, arg);
}
EOF
cc -shared -fPIC -O2 -o "$T/swshim.so" "$T/swshim.c" -ldl || fatal "the switch stand-in did not build"
printf 'booted.root=/dev/mmcblk2p1\nslot.a.device=/dev/mmcblk2p1\nslot.a.present=yes\nslot.a.booted=yes\nslot.a.next=yes\n' \
    > "$T/slots.list"
OLD=$(cat "$T/ctl.secret")
SWITCH_DEV="$T/switches" PRELOAD="$T/swshim.so" SLOT_LIST="$T/slots.list" REBOOT_FILE="$T/reboot" start_daemon
until_ok 150 eval '[ "$(cat "$T/ctl.secret")" != "$OLD" ]' || fatal "no controller started beside the button's stand-in"
SEC=$(cat "$T/ctl.secret")
# armed <1|0>: the controller's report, its armed window open or closed
armed() {
    if [ "$1" = 1 ]; then set -- run 1; else set -- idle 0; fi
    [ "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "X-ForgeFIRM-Report: $SEC" -d "mode=$1" -d "armed=$2" \
              "$URL/cool/state")" = 200 ] || bad "the controller's report (mode=$1 armed=$2) was refused"
}
# gstate <state> [arming]: the GRBL controller's own state file, the laser
# arming (waiting for the press) when the second argument is given
gstate() {
    python3 - "$T/run/grbl.state" "$1" "${2:-}" <<'EOF'
import json, os, sys, time
tmp = sys.argv[1] + '.tmp'
json.dump({'ts_mono': round(time.monotonic(), 3), 'state': sys.argv[2], 'alarm': 0,
           'laser': {'armed': False, 'arming': bool(sys.argv[3]), 'model': 'density'}}, open(tmp, 'w'))
os.replace(tmp, sys.argv[1])
EOF
}
getfuse() { curl -s -o "$T/body" -w '%{http_code}' -H "X-ForgeFIRM-Token: $TOKEN" "$URL/fuse-identity"; }
WAITS='a job waits for the button press: cancel it first'
gstate Idle
echo unsigned > "$T/data/upload.fw"

# A login whose session cannot be made answers JSON too, as every login
# answer does: the page shows its error.
: > "$T/no-urandom"
code=$(login $NAME $PW)
rm -f "$T/no-urandom"
expect "500|True" "$code|$(jv 'bool(d.get("error"))')" "a login that cannot make a session: 500, with a JSON error"

# The button up, no job: each route's own refusal for the button.
expect 400 "$(post /system/reset "$TOKEN")" "POST /system/reset without confirm=1 is 400"
code=$(post /system/reset "$TOKEN" -d confirm=1)
if [ "$code" = 403 ] && [ "$(jv 'd.get("button")')" = "not held" ]; then
    ok "confirm=1 without the machine button held: 403, the button named (the panel's dialog stays open on it)"
else bad "confirm=1 without the button -> $code $(cat "$T/body")"; fi
code=$(post /update/apply "$TOKEN" -d slot=a -d file=upload -d confirm_unsigned=1)
if [ "$code" = 403 ] && [ "$(jv 'd.get("button")')" = "not held" ]; then
    ok "an unsigned install without the button held: 403, the button named"
else bad "confirm_unsigned=1 without the button -> $code $(cat "$T/body")"; fi
expect 403 "$(getfuse)" "GET /fuse-identity without the button held is 403"

# A job armed, the button still up: 409 from each, before the button is
# asked for, in the words the routes share.
armed 1
code=$(post /system/reset "$TOKEN" -d confirm=1)
expect "409|$WAITS" "$code|$(jv 'd.get("error")')" "the reset under an armed job: 409, not the button's 403"
code=$(post /update/apply "$TOKEN" -d slot=a -d file=upload -d confirm_unsigned=1)
expect "409|$WAITS" "$code|$(jv 'd.get("error")')" "an unsigned install under an armed job: 409, a job waits"
expect "409|$WAITS" "$(post /ext/key "$TOKEN" -d name=hostroutes -d key=none)|$(cat "$T/body")" \
    "POST /ext/key under an armed job: 409, a job waits"
expect "409|$WAITS" "$(post /ext/install "$TOKEN")|$(cat "$T/body")" "POST /ext/install under an armed job: 409, a job waits"
expect "409|$WAITS" "$(getfuse)|$(cat "$T/body")" "GET /fuse-identity under an armed job: 409, a job waits"
armed 0
# A GRBL job paused with its window closed: the controller's state file says so.
gstate Hold
expect 409 "$(getfuse)" "GET /fuse-identity beside a paused GRBL job is 409"
gstate Run
expect "409|a job is running or paused: finish or cancel it first" "$(getfuse)|$(cat "$T/body")" \
    "GET /fuse-identity beside a running GRBL job: 409, a job runs"
# The GRBL controller waiting for the press before its report says armed:
# its state file is the one source.
gstate Idle arming
expect "409|$WAITS" "$(getfuse)|$(cat "$T/body")" "GET /fuse-identity while the GRBL state file says arming: 409, a job waits"
code=$(post /system/reset "$TOKEN" -d confirm=1)
expect "409|$WAITS" "$code|$(jv 'd.get("error")')" "and the reset: 409, a job waits"
gstate Idle
expect 403 "$(getfuse)" "and 403 again, the button's, with the controller idle"
[ ! -e "$T/data/reset-requested" ] && [ ! -e "$T/reboot" ] && ok "no reset is requested, and nothing rebooted" \
    || bad "a refused reset left data/reset-requested or a reboot"

# The next boot set to the other slot: refused before the button too.
printf 'slot.a.booted=yes\nslot.b.next=yes\n' > "$T/slots.other"
cp "$T/slots.list" "$T/slots.same"
cp "$T/slots.other" "$T/slots.list"
expect 409 "$(post /system/reset "$TOKEN" -d confirm=1)" "the reset with the next boot on the other slot is 409"
cp "$T/slots.same" "$T/slots.list"

# The button held, and each refusal still comes first: the machine's
# lease (an extension keeping the sender out holds it), a machine that is
# not idle, a request that cannot be written (its name a link to
# /dev/full), a reboot that cannot be scheduled (the stand-in's file a
# directory). None leaves a request or a reboot.
echo 12 > "$T/switches"
none_left() { [ ! -e "$T/data/reset-requested" ] && [ ! -L "$T/data/reset-requested" ] && [ ! -s "$T/reboot" ]; }
CRED=$(cat "$CRED_FILE")
expect 200 "$(host /motion/sender -d id=$PKG -d out=1)" "$PKG keeps the sender out: it holds the machine"
code=$(post /system/reset "$TOKEN" -d confirm=1)
expect "409|True" "$code|$(jv '"'$PKG'" in d.get("error", "")')" "the reset under the lease, the button held: 409, the holder named"
expect 200 "$(host /motion/sender -d id=$PKG -d out=0)" "$PKG lets the sender back in"
echo running > "$SYS/cnc/state"
code=$(post /system/reset "$TOKEN" -d confirm=1)
expect "409|machine is not idle" "$code|$(jv 'd.get("error")')" "the reset on a machine that is not idle, the button held: 409"
echo idle > "$SYS/cnc/state"
none_left && ok "neither left a request or a reboot" || bad "a refused reset left data/reset-requested or a reboot"
ln -s /dev/full "$T/data/reset-requested"
code=$(post /system/reset "$TOKEN" -d confirm=1)
expect "500|cannot write the reset request" "$code|$(jv 'd.get("error")')" "a reset request that cannot be written: 500"
none_left && ok "and no request is left, nor a reboot" || bad "the unwritten request left data/reset-requested or a reboot"
mkdir "$T/reboot"
code=$(post /system/reset "$TOKEN" -d confirm=1)
expect "500|cannot schedule reboot" "$code|$(jv 'd.get("error")')" "a reset whose reboot cannot be scheduled: 500"
[ ! -e "$T/data/reset-requested" ] && ok "and the request is withdrawn" || bad "a reset with no reboot left data/reset-requested"
rmdir "$T/reboot"

# The button held: the reset is taken, its request written, the reboot scheduled.
code=$(post /system/reset "$TOKEN" -d confirm=1)
expect "200|True|True" "$code|$(jv 'd.get("rebooting")')|$(jv 'd.get("reset")')" "with the button held the reset is taken"
[ -s "$T/data/reset-requested" ] && ok "the reset request is in the data directory" || bad "no data/reset-requested"
[ -s "$T/reboot" ] && ok "and the reboot is scheduled (written to the stand-in's file)" || bad "no reboot was scheduled"
echo 8 > "$T/switches"
rm -f "$T/data/reset-requested" "$T/data/upload.fw" "$T/reboot"

# The inventory and the reboot route, through the same hooks.
expect "200|True" "$(curl -s -o "$T/body" -w '%{http_code}' "$URL/slots")|$(jv 'd["slots"]["a"]["booted"] is True')" \
    "GET /slots reads the inventory: slot a booted"
code=$(post /system/reboot "$TOKEN" -d confirm=1)
expect "200|True|reboot in 1 s" "$code|$(jv 'd.get("rebooting")')|$(cat "$T/reboot" 2>/dev/null)" \
    "POST /system/reboot: 200, the reboot scheduled 1 s on"

# A paused job's state file left by a GRBL controller that no longer runs
# refuses nothing: the fuse view asks for the button again.
gstate Hold
expect 200 "$(post /controller/stop "$TOKEN")" "the controller is stopped"
expect 403 "$(getfuse)" "beside a paused job's state file with no GRBL controller: 403, the button's"

echo "== a dev image: the seed, and the dev bar on its pages"
stop_daemon
: > "$T/forgefirm-dev"
echo card-failed > "$T/data-state"
# The dev card's seed, with no record beside it: the start makes it the
# record, with the account the users file holds (the login's, made above).
mv "$T/data/setup.json" "$T/setup.finished"
: > "$T/data/setup.seed"
DEV_MARKER="$T/forgefirm-dev" DATA_STATE="$T/data-state" start_daemon
curl -s -o "$T/body" "$URL/wiz"
if [ ! -e "$T/data/setup.seed" ] && [ "$(jv 'd["first_run"], d["gate"]')" = "(False, 'open')" ] && \
        [ "$(python3 -c 'import json, sys; d = json.load(open(sys.argv[1])); print(bool(d.get("seeded")), d["account"]["name"])' \
             "$T/data/setup.json" 2>&1)" = "True $NAME" ]; then
    ok "the seed is the record at start, the account the users file's ($NAME), the gate open"
else bad "the seed was not taken with the users file's account: seed $(ls "$T/data/setup.seed" 2>&1), GET /wiz $(cat "$T/body")"; fi
# devpage <path> <the page's own id> <what it is>: the bar between <body>
# and the header, in the words the bar says, with the boot's data line:
# a card boot whose data partition is not mounted says so
devpage() {
    expect 200 "$(curl -sk -o "$T/dev.html" -w '%{http_code}' "$TLS$1")" "GET $1 is 200"
    if grep -qF "$2" "$T/dev.html" && python3 - "$T/dev.html" <<'EOF'
import sys
p = open(sys.argv[1], encoding='utf-8').read()
head = ('<div class="ff-devbar" role="status"><strong>DEVELOPMENT IMAGE</strong>: '
        'SSH open with passwordless root logon<br>')
b = p.find('<body>')
h = p.find('<header>', b)
bar = p[b:h]
sys.exit(0 if 0 <= b < h and head in bar and
         '<strong>DATA Mount</strong>: SD card failed - nothing is saved this boot' in bar and
         'controllers held' in bar and
         '<!--FFDEV-->' not in p else 1)
EOF
    then ok "$3 carries the dev bar above its header, with the card's data not mounted"
    else bad "$3 lacks the dev bar above its header, or its data line"; fi
}
devpage /login 'id="lg"' "the login page"
devpage / 'id="savebar"' "the panel"
devpage /setup 'id="rail"' "the setup"
# that boot holds the controllers, and says why
code=$(curl -s -o "$T/mode.json" -w '%{http_code}' "$URL/mode")
if [ "$code" = 200 ] && grep -q '"gated":true' "$T/mode.json" && \
        grep -q "the SD card's data partition is not in use" "$T/mode.json"; then
    ok "a card boot whose data partition is not in use holds the controllers, and /mode says why"
else bad "GET /mode on a card-failed boot -> $code $(cat "$T/mode.json")"; fi
# and refuses a mode switch for it, the setup's own gate open
code=$(post /mode "$TOKEN" -d controller=grbl)
if [ "$code" = 409 ] && grep -q "controllers are gated: the SD card's data partition" "$T/body"; then
    ok "a mode switch on that boot: 409, the controllers gated by the card"
else bad "POST /mode on a card-failed boot -> $code $(cat "$T/body")"; fi
# The pages the daemon writes itself carry the bar inline, right after
# <body>: the certificate page, the licenses page and the printable record.
DEVHEAD='<strong>DEVELOPMENT IMAGE</strong>: SSH open with passwordless root logon<br>'
FAILED='<strong>DATA Mount</strong>: SD card failed - nothing is saved this boot (see logs); controllers held'
inline() {
    expect 200 "$(curl -sk -o "$T/dev.html" -w '%{http_code}' -H "X-ForgeFIRM-Token: $TOKEN" "$TLS$1")" "GET $1 is 200"
    if python3 - "$T/dev.html" "$DEVHEAD$FAILED" <<'EOF'
import sys
p = open(sys.argv[1], encoding='utf-8').read()
b = p.find('<body>') + len('<body>')
sys.exit(0 if p.startswith('<div class="ff-devbar"', b) and sys.argv[2] in p[b:p.find('</div>', b)] else 1)
EOF
    then ok "$1 carries the dev bar right after <body>, with the card's data not mounted"
    else bad "$1 lacks the dev bar right after <body>, or its data line"; fi
}
inline /cert
inline /licenses
inline /wiz/record.html
# The data line for each word the boot's record can hold, read again at
# each page (the certificate page's)
for word in card emmc card-none unknown-word; do
    echo "$word" > "$T/data-state"
    curl -s -o "$T/dev.html" "$URL/cert"
    case $word in
        card) want='<strong>DATA Mount</strong>: SD Card</div>' ;;
        emmc|card-none) want='<strong>DATA Mount</strong>: eMMC</div>' ;;
        *) want='<strong>DATA Mount</strong>: UNKNOWN (error - see logs); controllers held</div>' ;;
    esac
    grep -qF "$want" "$T/dev.html" && ok "the bar's data line for $word" || bad "the bar's data line for $word is not '$want'"
done
echo card-failed > "$T/data-state"

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
