#!/bin/sh
# host_guard.sh - the factory return's guard and the record's routes, on
# a host
# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# SPDX-License-Identifier: MIT
#
# Starts the built forgectrl the way host_setup.sh does (the same
# environment, sysfs tree, stand-ins, port listener, and seeded setup
# record), with the logging tree's paths in the temporary directory too,
# and proves, one case each:
#
#   the factory return refused without confirm=1, no update job started,
#     the slot inventory's archive list, and the setup page's confirmed
#     call; confirm=1 is never sent
#   the record: as JSON with the token (schema 1, the sheet id GET /wiz
#     reports, no password hash, not an attachment), as a download (an
#     attachment of the record named with that sheet id), as a printable
#     page (text/html, no script, the sheet id, every completed wizard),
#     both refused without a token or a login; and the sanitized log
#     export carrying it as system/setup.json with the same sheet id and
#     wizards and no password hash, and no panel token anywhere in the
#     archive though a log line in the tree carries it
#
# GF_FUSE_SERIAL gives the daemon a fuse serial, so it derives the sheet
# id from the salt it makes in the data directory, as a machine does; the
# seeded record carries none, and the script reads the id from GET /wiz.
#
# The daemon runs on the image's ulfius (tests/ulfius_image.sh). Runs
# unprivileged; build.yml runs it:
#
#   sh tests/host_guard.sh <build-dir> <ulfius-lib-dir>

# as root the daemon would write the paths it has no override for (its
# markers under /run)
[ "$(id -u)" != 0 ] || { echo "host_guard.sh runs unprivileged"; exit 2; }
BUILD=$(cd "${1:?build dir}" && pwd) || exit 2
ULIB=$(cd "${2:?ulfius lib dir}" && pwd) || exit 2
HTTP_PORT=18094
TLS_PORT=18457
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
# get <path> <token or -> [curl args]: the status; the body is in $T/body,
# the headers in $T/head
get() {
    p=$1; tok=$2; shift 2
    if [ "$tok" = - ]; then
        curl -s -D "$T/head" -o "$T/body" -w '%{http_code}' "$@" "$URL$p"
    else
        curl -s -D "$T/head" -o "$T/body" -w '%{http_code}' -H "X-ForgeFIRM-Token: $tok" "$@" "$URL$p"
    fi
}
header() { tr -d '\r' < "$T/head" | sed -n "s/^$1: //Ip"; }

start_daemon() {
    env LD_LIBRARY_PATH="$ULIB" FORGECTRL_HOST_TEST=1 GF_FUSE_SERIAL=314159265 \
        FORGECTRL_DATA_DIR="$T/data" FORGECTRL_CONF="$CONF" \
        FFLOG_CONF="$CONF" FFLOG_SOCK="$T/no-log" FFLOG_STDERR=1 \
        GF_RUN_DIR="$T/run" GF_SYSFS_ROOT="$SYS/" GF_SWITCH_DEV="$T/no-switches" \
        GF_SWITCH_WORD_FILE="$T/switches" GF_PULSE_DEV="$T/pulse" \
        FORGECTRL_GRBL_BIN="$T/bin/grbl" FORGECTRL_CLOUD_BIN="$T/bin/cloud" \
        GF_LOGS_ROOT="$T/log" GF_LOGS_RSYSLOG="$T/rsyslog-forgefirm.conf" \
        GF_LOGS_EFFECTIVE="$T/loglevels" GF_LOGS_STAGE="$T/stage" \
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

# The machine, as host_setup.sh builds it, and the logging tree's paths
# in the temporary directory.
mkdir -p "$T/data" "$T/run" "$T/bin" "$T/log" "$SYS/cnc" "$SYS/thermal" "$SYS/head"
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

cp "$T/seed.json" "$REC"
start_daemon
until_ok 100 ctl_up || fatal "no controller started on the seeded record"
TOKEN=$(cat "$T/data/panel.token")

echo "== the factory return"
expect 400 "$(post /restore/factory-return "$TOKEN")" "POST /restore/factory-return without confirm is 400"
grep -q confirm "$T/body" && ok "and the body names confirm" || bad "the body is $(cat "$T/body")"
expect 400 "$(post '/restore/factory-return?confirm=0' "$TOKEN")" "confirm=0 is 400"
expect 200 "$(get /update/status -)" "GET /update/status is 200"
expect false "$(jget "$T/body" 'json.dumps(d["running"])')" "no update job runs"
if [ -x /usr/sbin/ffboot ]; then
    echo "  skipped: GET /slots (/usr/sbin/ffboot is on this host; the inventory would run it)"
else
    expect 200 "$(get /slots -)" "GET /slots is 200"
    expect True "$(jget "$T/body" 'str(isinstance(d["archives"], list))')" \
        "its archives are a list ($(jget "$T/body" 'len(d["archives"])') here)"
fi
expect 200 "$(get /setup -)" "GET /setup is 200"
grep -qF '/restore/factory-return?confirm=1' "$T/body" && ok "the page carries the confirmed call" \
    || bad "the setup page does not carry /restore/factory-return?confirm=1"

echo "== the record"
curl -s -o "$T/wiz.json" "$URL/wiz"
SID=$(jget "$T/wiz.json" 'd.get("sheet_id") or ""')
[ -n "$SID" ] && ok "GET /wiz reports the sheet id $SID" || bad "GET /wiz reports no sheet id"
expect 200 "$(get /wiz/record "$TOKEN")" "GET /wiz/record with the token is 200"
cp "$T/body" "$T/record.json"
expect "1|$SID" "$(jget "$T/record.json" '"%s|%s" % (d.get("schema"), d.get("sheet_id") or "")')" "the record: schema 1, the sheet id"
grep -qF '$6$' "$T/record.json" && bad "the record carries a password hash" || ok "no password hash"
case $(header Content-Disposition) in
    *attachment*) bad "a plain read came as an attachment" ;;
    *) ok "a plain read is not an attachment" ;;
esac
expect 200 "$(get '/wiz/record?download=1' "$TOKEN")" "download=1 is 200"
case $SID:$(header Content-Disposition) in
    ?*:*attachment*"$SID"*) ok "an attachment named with the sheet id: $(header Content-Disposition)" ;;
    *) bad "the download's disposition is '$(header Content-Disposition)'" ;;
esac
expect True "$(jget "$T/body" "str(d == json.load(open('$T/record.json')))")" "the download is the record"
expect 200 "$(get /wiz/record.html "$TOKEN")" "GET /wiz/record.html is 200"
cp "$T/body" "$T/record.html"
case $(header Content-Type) in
    *text/html*) ok "text/html" ;;
    *) bad "the page's type is '$(header Content-Type)'" ;;
esac
grep -qF 'setup record' "$T/record.html" && ok "the page says setup record" || bad "the page does not say setup record"
grep -qi '<script' "$T/record.html" && bad "the page carries a script" || ok "no script"
[ -n "$SID" ] && grep -qF "$SID" "$T/record.html" && ok "the page carries the sheet id" \
    || bad "the page lacks the sheet id"
missing=$(python3 - "$T/record.json" "$T/wiz.json" "$T/record.html" <<'EOF'
import json, sys
rec, w = json.load(open(sys.argv[1])), json.load(open(sys.argv[2]))
page = open(sys.argv[3], encoding='utf-8').read()
titles = {z['id']: z['title'] for z in w['wizards']}
print(' '.join(i for i in sorted(rec.get('wizards') or {}) if titles.get(i) and '<h3>' + titles[i] not in page))
EOF
)
done_n=$(jget "$T/record.json" 'len(d.get("wizards") or {})')
[ -z "$missing" ] && [ "$done_n" -gt 0 ] && ok "the page names the $done_n completed wizards" \
    || bad "completed wizards missing from the page: '$missing' (of $done_n)"
expect 403 "$(get /wiz/record -)" "GET /wiz/record without a token or a login is 403"
expect 403 "$(get /wiz/record.html -)" "GET /wiz/record.html without a token or a login is 403"

echo "== the log export"
# a log line that carries the token, so the export has one to remove
mkdir -p "$T/log/forgectrl"
echo "2026-10-14T09:00:00 forgectrl[1] INFO probe panel token $TOKEN" >> "$T/log/forgectrl/forgectrl.log"
code=$(curl -s -o "$T/export.tgz" -w '%{http_code}' --max-time 120 -X POST -H "X-ForgeFIRM-Token: $TOKEN" "$URL/logs/export")
expect 200 "$code" "a sanitized POST /logs/export is 200"
python3 - "$T/export.tgz" "$SID" "$TOKEN" "$T/record.json" <<'EOF'
import gzip, io, json, sys, tarfile
path, sid, token = sys.argv[1], sys.argv[2], sys.argv[3].encode()
record = json.load(open(sys.argv[4]))
raw = gzip.decompress(open(path, 'rb').read())
tf = tarfile.open(fileobj=io.BytesIO(raw))
names = {m.name: m for m in tf.getmembers()}
member = names.get('forgefirm-logs/system/setup.json')
text = tf.extractfile(member).read() if member else b''
rec = json.loads(text.decode('utf-8')) if member else None
def say(ok, what):
    print('  %s: %s' % ('ok' if ok else 'FAIL', what))
    return 0 if ok else 1
fails = say(member is not None, 'the archive carries forgefirm-logs/system/setup.json')
fails += say(isinstance(rec, dict) and sid and rec.get('sheet_id') == sid, 'it parses, with the sheet id %s' % sid)
fails += say(isinstance(rec, dict) and sorted(rec.get('wizards') or {}) == sorted(record.get('wizards') or {}),
             "its wizards are the record's")
fails += say(member is not None and b'$6$' not in text, 'it carries no password hash')
fails += say(token not in raw, 'the panel token is nowhere in the archive')
sys.exit(1 if fails else 0)
EOF
[ $? -eq 0 ] || fails=$((fails + 1))
[ -z "$(ls -A "$T/stage" 2>/dev/null)" ] && ok "the export's staging is gone from the temporary tree" \
    || bad "the staging is left: $(ls "$T/stage")"

stop_daemon
if [ $fails -eq 0 ]; then
    echo "PASS: host_guard"
else
    echo "FAIL: host_guard, $fails checks"
    echo "--- the daemon's output"
    tail -60 "$T/out"
    exit 1
fi
