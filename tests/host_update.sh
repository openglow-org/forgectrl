#!/bin/sh
# host_update.sh - the release check and an update job's lease, on a host
# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# SPDX-License-Identifier: MIT
#
# Starts the built forgectrl the way host_guard.sh does (the same
# environment, sysfs tree, stand-ins, port listener, and seeded setup
# record), with FORGECTRL_RELCHECK_URL and FORGECTRL_RELCHECK_DOWNLOAD_FMT
# pointed at a release server of its own on 127.0.0.1:18099. That server
# answers release v0.0.9 with notes and a forgefirm.fw asset of a size
# above 0, and serves a few bytes that are no archive as acceptance.json,
# after a delay that keeps the job, and so its lease, running. fwup on the
# daemon's PATH is a stand-in that reads an archive as text: -m prints its
# meta- lines, -V passes only for the key a signed-by line names, -l passes
# on an archive with a product and lists its task lines, and every call is
# recorded. One case each:
#
#   the release check: GET /update/release's keys; POST /update/check;
#     the version, the size, the notes, and new; a dismissal refused for
#     a version that is not a tag, taken for the tag, and cleared by an
#     empty version
#   an update job's lease: a probe download (202) holds the machine, so a
#     settings write and a G21 job beside it are refused (409) in the
#     job's name, and /status names update:download, of kind system; the
#     probe ends refused by the signature check, leaving neither
#     download-probe nor download.fw, and the same settings write is then
#     taken
#   the product gate on POST /update/upload: an extension package
#     unsigned, one signed with a throwaway key, and one that carries a
#     task are each 400, an extension package, not firmware; an archive of
#     another product is 400, not firmware; each leaves nothing staged.
#     ForgeFIRM firmware is taken as the unsigned upload it is (200,
#     signature unsigned, the bytes staged), and the staged file is then
#     removed; fwup is never asked to apply
#
# No firmware file is requested, nothing is applied, no slot is written,
# and nothing posts confirm=1, a reboot, or /system/ssh. The daemon runs
# on the image's ulfius (tests/ulfius_image.sh). Runs unprivileged;
# build.yml runs it:
#
#   sh tests/host_update.sh <build-dir> <ulfius-lib-dir>

# as root the daemon would write the paths it has no override for (its
# markers under /run)
[ "$(id -u)" != 0 ] || { echo "host_update.sh runs unprivileged"; exit 2; }
BUILD=$(cd "${1:?build dir}" && pwd) || exit 2
ULIB=$(cd "${2:?ulfius lib dir}" && pwd) || exit 2
HTTP_PORT=18098
TLS_PORT=18461
REL_PORT=18099
URL=http://127.0.0.1:$HTTP_PORT
T=$(mktemp -d)
SYS=$T/sys
REC=$T/data/setup.json
CONF=$T/data/forgefirm.conf
PID=
LPID=
RPID=
cleanup() {
    if [ -n "$PID" ]; then kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; fi
    [ -z "$LPID" ] || kill $LPID 2>/dev/null
    [ -z "$RPID" ] || kill $RPID 2>/dev/null
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
get() { curl -s -o "$T/body" -w '%{http_code}' "$URL$1"; }

start_daemon() {
    env LD_LIBRARY_PATH="$ULIB" FORGECTRL_HOST_TEST=1 \
        FORGECTRL_DATA_DIR="$T/data" FORGECTRL_CONF="$CONF" \
        FFLOG_CONF="$CONF" FFLOG_SOCK="$T/no-log" FFLOG_STDERR=1 \
        GF_RUN_DIR="$T/run" GF_SYSFS_ROOT="$SYS/" GF_SWITCH_DEV="$T/no-switches" \
        GF_SWITCH_WORD_FILE="$T/switches" GF_PULSE_DEV="$T/pulse" \
        FORGECTRL_GRBL_BIN="$T/bin/grbl" FORGECTRL_CLOUD_BIN="$T/bin/cloud" \
        GF_LOGS_ROOT="$T/log" GF_LOGS_RSYSLOG="$T/rsyslog-forgefirm.conf" \
        GF_LOGS_EFFECTIVE="$T/loglevels" GF_LOGS_STAGE="$T/stage" \
        FORGECTRL_RELCHECK_URL="http://127.0.0.1:$REL_PORT/repos/openglow-org/forgefirm/releases/latest" \
        FORGECTRL_RELCHECK_DOWNLOAD_FMT="http://127.0.0.1:$REL_PORT/releases/download/%s/%s" \
        FORGECTRL_PORT=$HTTP_PORT FORGECTRL_TLS_PORT=$TLS_PORT PATH="$T/path:$PATH" \
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

# The machine, as host_guard.sh builds it.
mkdir -p "$T/data" "$T/run" "$T/bin" "$T/log" "$SYS/cnc" "$SYS/thermal" "$SYS/head"
echo idle > "$SYS/cnc/state"
for a in cnc/stop cnc/laser_latch cnc/enable cnc/disable cnc/halt \
         head/air_assist_pwm thermal/exhaust_pwm thermal/intake_pwm; do
    : > "$SYS/$a"
done
echo 8 > "$T/switches"
: > "$T/pulse"
: > "$T/port.log"
printf 'cool_intake_fans = 1\nui_units = metric\n' > "$CONF"
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
# fwup's stand-in: the archive is text, and the gate decides from what
# this prints, as it does from what fwup prints.
mkdir -p "$T/path"
cat > "$T/path/fwup" <<'EOF'
#!/bin/sh
echo "$*" >> "${0%/*}/fwup.log"
op=$1
shift
f=
k=
while [ $# -gt 0 ]; do
    case $1 in -i) f=$2; shift ;; -p) k=$2; shift ;; esac
    shift
done
[ -f "$f" ] || exit 1
case $op in
-m) grep '^meta-' "$f" ;;
-V) [ -n "$k" ] && grep -qxF "signed-by=$k" "$f" ;;
-l) grep -q '^meta-product=' "$f" && sed -n 's/^task //p' "$f" ;;
*) exit 1 ;;
esac
EOF
chmod +x "$T/path/fwup"

# The release server: the latest release, and the acceptance record late.
python3 - "$REL_PORT" "$T/rel.log" <<'EOF' &
import http.server, json, sys, time
port, log = int(sys.argv[1]), sys.argv[2]
LATEST = json.dumps({
    'tag_name': 'v0.0.9', 'published_at': '2026-10-14T00:00:00Z',
    'body': 'Notes for the host test.\n\n- one line',
    'assets': [{'name': 'forgefirm.fw', 'size': 4096}, {'name': 'acceptance.json', 'size': 32}],
}).encode()
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        with open(log, 'a') as o:
            o.write(self.path + '\n')
        if self.path == '/repos/openglow-org/forgefirm/releases/latest':
            body, code = LATEST, 200
        elif self.path == '/releases/download/v0.0.9/acceptance.json':
            time.sleep(6)
            body, code = b'not an acceptance record\n', 200
        else:
            body, code = b'{"message":"Not Found"}', 404
        self.send_response(code)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *a):
        pass
http.server.ThreadingHTTPServer(('127.0.0.1', port), H).serve_forever()
EOF
RPID=$!

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

# The controller's port, as host_guard.sh stands it in.
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

echo "== the release check"
expect 200 "$(get /update/release)" "GET /update/release is 200"
expect True "$(jget "$T/body" 'str(all(k in d for k in ("available", "version", "current", "new", "checked", "dismissed", "detail", "bytes")))')" \
    "it carries available, version, current, new, checked, dismissed, detail, and bytes"
expect 200 "$(post /update/check "$TOKEN")" "POST /update/check is 200"
cp "$T/body" "$T/release.json"
TAG=$(jget "$T/release.json" 'd.get("version") or ""')
expect True "$(jget "$T/release.json" 'str(d["available"] is True and __import__("re").fullmatch(r"v\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?", d["version"]) is not None)')" \
    "a release is available at a v<semver> tag ($TAG)"
expect True "$(jget "$T/release.json" 'str(isinstance(d["bytes"], int) and d["bytes"] > 0)')" "its firmware file's size is above 0"
expect True "$(jget "$T/release.json" 'str(isinstance(d["notes"], str) and isinstance(d["new"], bool))')" \
    "its notes are a string and new is a bool"
expect 400 "$(post /update/dismiss "$TOKEN" --data-urlencode 'version=v0.0.1; rm -rf /')" \
    "dismissing 'v0.0.1; rm -rf /' is 400"
expect 200 "$(post /update/dismiss "$TOKEN" -d "version=$TAG")" "dismissing $TAG is 200"
expect true "$(jget "$T/body" 'json.dumps(d.get("dismissed"))')" "and the release reads dismissed"
expect 200 "$(post '/update/dismiss?version=' "$TOKEN")" "an empty version is 200"
expect false "$(jget "$T/body" 'json.dumps(d.get("dismissed"))')" "and clears the dismissal"

echo "== an update job's lease"
printf 'G21\n' > "$T/g21.gcode"
expect 202 "$(post '/update/download?probe=1' "$TOKEN")" "POST /update/download?probe=1 is 202"
expect 409 "$(post /settings "$TOKEN" -d ui_units=metric)" "a settings write beside it is 409"
grep -qF 'an update job (download) holds the machine' "$T/body" && grep -qF 'settings are locked' "$T/body" \
    && ok "in the job's name, the settings locked" || bad "the refusal says '$(cat "$T/body")'"
expect 409 "$(post /job "$TOKEN" -F name=beside-an-update -F "program=@$T/g21.gcode;filename=job.gcode;type=text/plain")" \
    "a G21 job beside it is 409"
get /status > /dev/null
expect 'update:download|system' "$(jget "$T/body" '(lambda h: "%s|%s" % (h.get("owner"), h.get("kind")))((d.get("lease") or {}).get("holder") or {})')" \
    "/status names update:download, of kind system, as the holder"
until_ok 300 eval 'get /update/status > /dev/null; [ "$(jget "$T/body" "json.dumps(d[\"running\"])")" = false ]' \
    || bad "the job did not end within a minute"
case $(jget "$T/body" 'str((d.get("result") or {}).get("error"))') in
    *"signature verification failed"*) ok "the job ended refused by the signature check" ;;
    *) bad "the job ended with $(cat "$T/body")" ;;
esac
[ ! -e "$T/data/download-probe" ] && ok "no download-probe is left" || bad "download-probe is left"
[ ! -e "$T/data/download.fw" ] && ok "no download.fw was made" || bad "download.fw was made"
grep -qs 'forgefirm.fw' "$T/rel.log" && bad "the firmware file was requested" || ok "the firmware file was never requested"
expect 200 "$(post /settings "$TOKEN" -d ui_units=metric)" "the same settings write is then 200"

echo "== the product gate on the upload"
STAGED=$T/data/upload.fw
upload() { post /update/upload "$TOKEN" -F "file=@$1;filename=${1##*/}"; }
ext='meta-product="ForgeFIRM extension"\nmeta-version=1.0.0\n'
printf "$ext" > "$T/ext-unsigned.fw"
printf "${ext}signed-by=$T/throwaway.pub\n" > "$T/ext-signed.fw"
printf "${ext}task upgrade.a\n" > "$T/ext-task.fw"
printf 'meta-product="forgetest"\nmeta-version=0.0.0-test\n' > "$T/other.fw"
printf 'meta-product="ForgeFIRM firmware"\nmeta-version=0.0.0-test\n' > "$T/firmware.fw"
for a in ext-unsigned ext-signed ext-task; do
    expect 400 "$(upload "$T/$a.fw")" "$a is 400"
    grep -qF 'an extension package, not firmware' "$T/body" && ok "and says an extension package, not firmware" \
        || bad "the refusal says '$(cat "$T/body")'"
    [ ! -e "$STAGED" ] && ok "and nothing is staged" || bad "$a is staged"
done
expect 400 "$(upload "$T/other.fw")" "an archive of another product is 400"
grep -qF 'not firmware' "$T/body" && ok "and says not firmware" || bad "the refusal says '$(cat "$T/body")'"
[ ! -e "$STAGED" ] && ok "and nothing is staged" || bad "the other product is staged"
expect 200 "$(upload "$T/firmware.fw")" "ForgeFIRM firmware is 200"
expect unsigned "$(jget "$T/body" 'd.get("signature")')" "its signature reads unsigned"
cmp -s "$T/firmware.fw" "$STAGED" && ok "and its bytes are staged" || bad "the staged file is not the upload"
rm -f "$STAGED"
grep -q '^-a' "$T/path/fwup.log" && bad "fwup was asked to apply" || ok "fwup was never asked to apply"

stop_daemon
if [ $fails -eq 0 ]; then
    echo "PASS: host_update"
else
    echo "FAIL: host_update, $fails checks"
    echo "--- the daemon's output"
    tail -60 "$T/out"
    exit 1
fi
