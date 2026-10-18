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
#     report with the secret ends the session; the secret dies with its
#     controller
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
#   the page (its frame policy, the token, nothing external, the theme
#     and the save bar), /status's switches and its grbl block only
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
        FORGECTRL_SSHD_INIT="${SSHD_INIT:-$T/bin/sshd-init}" FORGECTRL_EXT_HOST_TOKEN="$T/run/ext-host.token" \
        FORGECTRL_LICENSES="$T/licenses.tar.gz" \
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
