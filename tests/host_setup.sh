#!/bin/sh
# host_setup.sh - the setup gate and its consents, on a host
# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# SPDX-License-Identifier: MIT
#
# Starts the built forgectrl on the machine host_routes.sh builds (the
# same environment, sysfs tree, stand-ins, port listener, and seeded
# setup record) and walks what the setup gate and its consents decide,
# one case each:
#
#   the gate: the record without consent, without the account, and
#     without the required steps each hold the controller with its own
#     reason; the record whole opens it; clearing the intake fan count
#     closes it live and stops the controller, and the count brings it
#     back
#   the advisories: the first-run documents, their hashes, and what an
#     acceptance takes and refuses; a new acceptance takes the press back
#   the extensions: the on-demand advisory, refused on the first-run
#     route, and turning extensions on over it through the settings
#   cloud mode off: the built-in's roles, and every way of pointing at
#     the cloud refused while it is off
#
# The record is read at start, so it is replaced only while the daemon
# is stopped. Each case puts back what it changed. The daemon runs on
# the image's ulfius (tests/ulfius_image.sh). Runs unprivileged;
# build.yml runs it:
#
#   sh tests/host_setup.sh <build-dir> <ulfius-lib-dir>

# as root the daemon would write the paths it has no override for (its
# markers under /run)
[ "$(id -u)" != 0 ] || { echo "host_setup.sh runs unprivileged"; exit 2; }
BUILD=$(cd "${1:?build dir}" && pwd) || exit 2
ULIB=$(cd "${2:?ulfius lib dir}" && pwd) || exit 2
HTTP_PORT=18092
TLS_PORT=18455
URL=http://127.0.0.1:$HTTP_PORT
T=$(mktemp -d)
SYS=$T/sys
REC=$T/data/setup.json
CONF=$T/data/forgefirm.conf
PID=
LPID=
cleanup() {
    if [ -n "$PID" ]; then kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; fi
    [ -z "$LPID" ] || kill $LPID 2>/dev/null
    [ ! -f "$T/ctl.pids" ] || kill $(cat "$T/ctl.pids") 2>/dev/null
    chmod 755 "$T/data" 2>/dev/null
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
wiz() { curl -s -o "$T/wiz.json" "$URL/wiz"; }
mode() { curl -s -o "$T/mode.json" "$URL/mode"; }
setting() { curl -s -o "$T/settings.json" "$URL/settings"; jget "$T/settings.json" "d.get('$1', '')"; }
said() { grep -q "$1" "$T/body" && ok "and says $1" || bad "the body is $(cat "$T/body")"; }

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

# The controller's stand-in, by the pid it last recorded.
ctl_pid() { cat "$T/ctl.pid" 2>/dev/null; }
ctl_up() { p=$(ctl_pid); [ -n "$p" ] && [ "$p" != "$OLD" ] && alive "$p"; }
ctl_down() { p=$(ctl_pid); [ -z "$p" ] || ! alive "$p"; }
gated() { mode; [ "$(jget "$T/mode.json" 'd["gated"]')" = true ]; }
# up: wait for a stand-in newer than the one before
up() { until_ok 100 ctl_up; }
# restart <record file>: the daemon stopped, the record replaced, started
restart() {
    OLD=$(ctl_pid)
    stop_daemon
    cp "$1" "$REC"
    start_daemon
}
# held <reason>: the gate is closed with that reason in /mode and /wiz,
# no stand-in runs, and the cloud client's never started
held() {
    if until_ok 50 gated; then
        expect gated "$(jget "$T/mode.json" 'd["controller"]')" "GET /mode: controller gated, gated true"
    else bad "GET /mode never reported gated: $(cat "$T/mode.json")"; fi
    expect "$1" "$(jget "$T/mode.json" 'd["why"]')" "GET /mode gives the reason"
    wiz
    expect "closed|$1" "$(jget "$T/wiz.json" 'd["gate"] + "|" + d["why"]')" "GET /wiz: gate closed, the same reason"
    ctl_down && ok "no controller runs" || bad "a controller runs (pid $(ctl_pid))"
    [ ! -e "$T/cloud.started" ] && ok "the cloud client never started" || bad "the cloud client started"
}

# The machine, as host_routes.sh builds it: the kernel idle, the files
# the supervisor's safing and the probe write, the three fan duties, no
# head accelerometer; the switch word closed (8); the pulse device a
# plain file.
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
OLD=
stop_daemon
python3 - "$T/wiz0.json" "$T/seed.json" <<'EOF' || fatal "the setup record was not written"
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
# The record's variants for the gate.
python3 - "$T/seed.json" "$T" <<'EOF' || fatal "the record's variants were not written"
import copy, json, sys
seed, out = json.load(open(sys.argv[1])), sys.argv[2]
def put(name, rec):
    json.dump(rec, open('%s/%s.json' % (out, name), 'w'), indent=2)
r = copy.deepcopy(seed); r['advisories'] = {}; r.pop('acceptance'); put('no-consent', r)
r = copy.deepcopy(seed); r.pop('account'); put('no-account', r)
r = copy.deepcopy(seed); r['wizards'] = {}; r['flags'] = {}; put('no-steps', r)
EOF

# The controller's port, as host_routes.sh stands it in.
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

cp "$T/seed.json" "$REC"
start_daemon
up || fatal "no controller started on the seeded record"
TOKEN=$(cat "$T/data/panel.token")
wiz
expect open "$(jget "$T/wiz.json" 'd["gate"]')" "the seeded record opens the gate"
cp "$T/wiz.json" "$T/wiz.before"

echo "== the gate"
restart "$T/no-consent.json"
held "the advisories are not accepted"
restart "$T/no-account.json"
held "no account exists"
restart "$T/no-steps.json"
held "setup required: preferences, machine, cloud, cooling.flow"
restart "$T/seed.json"
up && ok "the record whole: the controller is back" || bad "no controller with the record whole"
wiz
expect 'open||true' "$(jget "$T/wiz.json" 'd["gate"] + "|" + d["why"] + "|" + json.dumps(d["fans_counted"])')" \
    "GET /wiz: gate open, no reason, fans counted"
OLD=$(ctl_pid)
# cleared by an empty query parameter: an empty form field is no field
expect 200 "$(post '/settings?cool_intake_fans=' "$TOKEN")" "clearing cool_intake_fans is 200"
wiz
expect 'false|the intake fans are not counted' "$(jget "$T/wiz.json" 'json.dumps(d["fans_counted"]) + "|" + d["why"]')" \
    "the fans are not counted, and that is the reason"
until_ok 100 ctl_down && ok "the controller stops" || bad "the controller still runs (pid $(ctl_pid))"
expect 200 "$(post /settings "$TOKEN" -d cool_intake_fans=1)" "cool_intake_fans=1 is 200"
up && ok "the controller is back" || bad "no controller after the count"
restart "$T/seed.json"
up || bad "no controller on the seeded record"
wiz
expect open "$(jget "$T/wiz.json" 'd["gate"]')" "the seeded record restored: the gate is open"
expect "$(jget "$T/wiz.before" 'd["recommended"]')" "$(jget "$T/wiz.json" 'd["recommended"]')" \
    "the recommended list is the one before the walk"

echo "== the advisories"
wiz
expect '["safety-and-risk", "licenses", "privacy", "cloud-service"]' "$(jget "$T/wiz.json" '[x["id"] for x in d["documents"]]')" \
    "GET /wiz lists the four first-run documents"
expect 'typed|I UNDERSTAND' "$(jget "$T/wiz.json" '[x["consent"] + "|" + x["phrase"] for x in d["documents"] if x["id"] == "safety-and-risk"][0]')" \
    "safety-and-risk takes the typed phrase I UNDERSTAND"
LISTED=$(jget "$T/wiz.json" '[x["hash"] for x in d["documents"] if x["id"] == "safety-and-risk"][0]')
code=$(curl -s -D "$T/adv.h" -o "$T/adv.md" -w '%{http_code}' "$URL/advisories/safety-and-risk")
ETAG=$(tr -d '\r' < "$T/adv.h" | sed -n 's/^[Ee][Tt][Aa][Gg]: //p')
SUM=$(sha256sum "$T/adv.md" | cut -d' ' -f1)
expect 200 "$code" "GET /advisories/safety-and-risk is 200"
tr -d '\r' < "$T/adv.h" | grep -qi '^content-type: text/markdown' && ok "text/markdown" || bad "its content type is not text/markdown"
expect "$SUM|$SUM" "$ETAG|$LISTED" "its ETag is the SHA-256 of the body, and the hash GET /wiz lists"
expect 404 "$(curl -s -o /dev/null -w '%{http_code}' "$URL/advisories/no-such-document")" "GET /advisories/no-such-document is 404"
DONE0=$(jget "$T/wiz.json" 'json.dumps(d["acceptance_done"]) + "|" + d["gate"] + "|" + json.dumps(d["advisories_complete"])')
ZERO=0000000000000000000000000000000000000000000000000000000000000000
expect 400 "$(post /wiz/advisories/accept "$TOKEN" -d doc=no-such-document -d "hash=$ETAG")" "accepting no-such-document is 400"
expect 409 "$(post /wiz/advisories/accept "$TOKEN" -d doc=safety-and-risk -d "hash=$ZERO" --data-urlencode "phrase=I UNDERSTAND")" \
    "accepting safety-and-risk at a stale hash is 409"
expect 400 "$(post /wiz/advisories/accept "$TOKEN" -d doc=safety-and-risk -d "hash=$ETAG")" "accepting it without the phrase is 400"
wiz
expect true "$(jget "$T/wiz.json" 'json.dumps(d["acceptance_done"])')" "the three leave acceptance_done as it was"
OLD=$(ctl_pid)
expect 200 "$(post /wiz/advisories/accept "$TOKEN" -d doc=safety-and-risk -d "hash=$ETAG" --data-urlencode "phrase=I UNDERSTAND")" \
    "accepting it at the current hash with I UNDERSTAND is 200"
wiz
expect 'true|false|closed' "$(jget "$T/wiz.json" 'json.dumps([x["accepted"] for x in d["documents"] if x["id"] == "safety-and-risk"][0]) + "|" + json.dumps(d["acceptance_done"]) + "|" + d["gate"]')" \
    "the document reads accepted, acceptance_done is false, the gate is closed"
case $(jget "$T/wiz.json" 'd["why"]') in *"not accepted"*) ok "the reason says not accepted" ;; *) bad "the reason is '$(jget "$T/wiz.json" 'd["why"]')'" ;; esac
expect "False|$ETAG" "$(jget "$REC" 'str("acceptance" in d) + "|" + d["advisories"]["safety-and-risk"]["hash"]')" \
    "the record has no acceptance key, and the document's hash is the ETag"
restart "$T/seed.json"
up || bad "no controller on the seeded record"
wiz
expect "$DONE0" "$(jget "$T/wiz.json" 'json.dumps(d["acceptance_done"]) + "|" + d["gate"] + "|" + json.dumps(d["advisories_complete"])')" \
    "restored: acceptance_done, the gate and advisories_complete as before"

echo "== the extensions"
DMODE=$(stat -c %a "$T/data")
cp "$CONF" "$T/conf.before"
chmod 0700 "$T/data"
wiz
expect False "$(jget "$T/wiz.json" 'str("extensions" in [x["id"] for x in d["documents"]])')" "GET /wiz does not list extensions"
code=$(curl -s -D "$T/ext.h" -o "$T/ext.md" -w '%{http_code}' "$URL/advisories/extensions")
XTAG=$(tr -d '\r' < "$T/ext.h" | sed -n 's/^[Ee][Tt][Aa][Gg]: //p')
expect 200 "$code" "GET /advisories/extensions is 200"
tr -d '\r' < "$T/ext.h" | grep -qi '^content-type: text/markdown' && ok "text/markdown" || bad "its content type is not text/markdown"
expect "$(sha256sum "$T/ext.md" | cut -d' ' -f1)" "$XTAG" "its ETag is the SHA-256 of the body"
expect 400 "$(post /wiz/advisories/accept "$TOKEN" -d doc=extensions -d "hash=$XTAG" --data-urlencode "phrase=I UNDERSTAND")" \
    "accepting extensions on the first-run route is 400"
wiz
expect true "$(jget "$T/wiz.json" 'json.dumps(d["acceptance_done"])')" "and acceptance_done is as it was"
cp "$REC" "$T/rec.before"
FIRST=$(jget "$REC" '[d["advisories"], d["acceptance"]]')
GATE=$(jget "$T/wiz.json" 'json.dumps(d["acceptance_done"]) + "|" + d["gate"]')
expect 409 "$(post /settings "$TOKEN" -d ext_enabled=1)" "ext_enabled=1 with no advisory field is 409"
said "read the Extensions advisory first"
expect 409 "$(post /settings "$TOKEN" -d ext_enabled=1 -d "advisory=$ZERO" --data-urlencode "phrase=I UNDERSTAND")" \
    "ext_enabled=1 at a stale hash is 409"
said "read the Extensions advisory first"
expect 400 "$(post /settings "$TOKEN" -d ext_enabled=1 -d "advisory=$XTAG")" "ext_enabled=1 without the phrase is 400"
said "type I UNDERSTAND"
expect 400 "$(post /settings "$TOKEN" -d ext_enabled=1 -d "advisory=$XTAG" --data-urlencode "phrase=i understand")" \
    "ext_enabled=1 with the phrase in lowercase is 400"
said "type I UNDERSTAND"
expect 409 "$(post /settings "$TOKEN" -d ext_enabled=1 -d "advisory=$XTAG" --data-urlencode "phrase=I UNDERSTAND" \
                   -d cloud_enabled=0 -d controller_mode=cloud)" \
    "ext_enabled=1 beside cloud_enabled=0 and controller_mode=cloud is 409"
said "cloud mode is not enabled on this machine"
[ "$(setting ext_enabled)" != 1 ] && ok "ext_enabled stays off" || bad "ext_enabled reads 1"
cmp -s "$T/rec.before" "$REC" && ok "setup.json is byte for byte the same" || bad "a refused post changed setup.json"
expect 200 "$(post /settings "$TOKEN" -d ext_enabled=1 -d "advisory=$XTAG" --data-urlencode "phrase=I UNDERSTAND")" \
    "ext_enabled=1 at the current hash with I UNDERSTAND is 200"
expect 1 "$(setting ext_enabled)" "the setting reads 1"
expect "$XTAG|typed|True|False" \
    "$(jget "$REC" '(lambda e: "%s|%s|%s|%s" % (e["hash"], e["method"], bool(e["accepted"]), "extensions" in d["advisories"]))(d["on_demand"]["extensions"])')" \
    "the record's on_demand.extensions holds the hash, typed, and a time; advisories does not"
expect 711 "$(stat -c %a "$T/data")" "the data directory is 0711"
expect "$FIRST" "$(jget "$REC" '[d["advisories"], d["acceptance"]]')" "the first-run advisories and the press are unchanged"
wiz
expect "$GATE" "$(jget "$T/wiz.json" 'json.dumps(d["acceptance_done"]) + "|" + d["gate"]')" "acceptance_done and the gate are unchanged"
expect 200 "$(post /settings "$TOKEN" -d ext_enabled=1)" "ext_enabled=1 again, with no hash, is 200"
expect 200 "$(post /settings "$TOKEN" -d ext_enabled=0)" "ext_enabled=0 is 200"
[ "$(setting ext_enabled)" != 1 ] && ok "and it reads off" || bad "ext_enabled still reads 1"
OLD=$(ctl_pid)
stop_daemon
cp "$T/conf.before" "$CONF"
chmod "$DMODE" "$T/data"
cp "$T/seed.json" "$REC"
start_daemon
up || bad "no controller on the seeded record"

echo "== cloud mode off"
curl -s -o "$T/ext.json" "$URL/extensions"
expect 'false|homing:gfcloud:none:false,controller:cloud:grbl:false' \
    "$(jget "$T/ext.json" '(lambda e: json.dumps(e["enabled"]) + "|" + ",".join("%s:%s:%s:%s" % (r["role"], r["provider"], r["fallback"], json.dumps(r["active"])) for r in e["roles"]))([x for x in d["extensions"] if x["id"] == "cloud"][0])')" \
    "the cloud built-in: off, homing gfcloud (fallback none), controller cloud (fallback grbl), neither active"
MODES=$(setting controller_mode)/$(setting homing_mode)
expect 409 "$(post /settings "$TOKEN" -d controller_mode=cloud)" "controller_mode=cloud is 409"
said "cloud mode is not enabled on this machine"
expect 409 "$(post /settings "$TOKEN" -d homing_mode=gfcloud)" "homing_mode=gfcloud is 409"
said "cloud homing needs cloud mode enabled"
expect 409 "$(post /settings "$TOKEN" -d cloud_enabled=0 -d controller_mode=cloud)" "cloud_enabled=0 with controller_mode=cloud is 409"
expect "$MODES" "$(setting controller_mode)/$(setting homing_mode)" "controller_mode and homing_mode are unchanged"
expect 400 "$(post /settings "$TOKEN" -d cloud_enabled=1)" "cloud_enabled=1 without the phrase is 400"
[ "$(setting cloud_enabled)" != 1 ] && ok "cloud_enabled stays off" || bad "cloud_enabled reads 1"
expect 409 "$(post /mode "$TOKEN" -d controller=cloud)" "POST /mode controller=cloud is 409"
said "cloud mode"
mode
expect 'grbl|running' "$(jget "$T/mode.json" 'd["mode"] + "|" + d["controller"]')" "GET /mode stays grbl, the controller running"
[ ! -e "$T/cloud.started" ] && ok "the cloud client never started" || bad "the cloud client started"
grep -qx 'cool_intake_fans = 1' "$CONF" && ok "the conf file keeps cool_intake_fans = 1" || bad "the conf file lost the fan count"

stop_daemon
if [ $fails -eq 0 ]; then
    echo "PASS: host_setup"
else
    echo "FAIL: host_setup, $fails checks"
    echo "--- the daemon's output"
    tail -60 "$T/out"
    exit 1
fi
