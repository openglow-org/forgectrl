#!/bin/sh
# host_ext.sh - the package routes, the upload sink, and a package's
# check, on a host
# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# SPDX-License-Identifier: MIT
#
# Starts the built forgectrl with its machine paths in a temporary
# directory, as host_routes.sh does (the switch word, the sysfs tree, the
# pulse device a plain file), with FORGECTRL_FORGEEXT a stand-in for the
# extension host, FORGECTRL_EXT_STATUS the host's status file, and
# FORGECTRL_EXT_STAGE the staging file under the same directory. The
# stand-in is tests/host_ext_forgeext.c, compiled here: started as hold
# before the daemon, it is a host that is alive (its status file names its
# own pid, and /proc/<pid>/exe is FORGECTRL_FORGEEXT); run as any other
# command, it records its arguments and answers for one package,
# org.example.hostext, with a wizard check. The record seeded accepts the
# advisories and leaves the gate closed, so no controller starts. One
# case each:
#
#   an upload without the token: 403, and no staging file
#   an upload while cnc/state reads running: 409, not idle, no staging
#     file, and the stand-in never run
#   bytes the stand-in refuses: 400 in its words, and the staging file
#     gone
#   bytes it takes: 200, tier unverified, consent button, and the staging
#     file holds those bytes; the discard: 200, and the file gone
#   an install with nothing staged: 409, and the words name it
#   a key with the token: 409, the button named, and no key written
#   GET /ext/status and POST /ext/package without the token: 403, and the
#     host not run
#   GET /ext/status: the package as the host lists it, and the host
#     running with the stand-in's pid
#   enable through POST /ext/package run as the host's command; an action
#     outside the list and an id that is none: 400, the host not run; a
#     package that is not installed: 409 in the host's words
#   the package's check: listed on GET /wiz, run to its prompt with the
#     machine lease free, answered yes, ended with its summary, listed
#     done; the setup record's wizards and the gate as they were; a second
#     run aborted; a check of a package that is not installed: 404
#
# No package is installed and no key is added: each takes the machine's
# button, which a host does not have. Nothing posts confirm=1, a reboot,
# or /system/ssh. The daemon runs on the image's ulfius
# (tests/ulfius_image.sh). Needs a C compiler (cc). Runs unprivileged;
# build.yml runs it:
#
#   sh tests/host_ext.sh <build-dir> <ulfius-lib-dir>

# as root the daemon would write the paths it has no override for (its
# markers under /run)
[ "$(id -u)" != 0 ] || { echo "host_ext.sh runs unprivileged"; exit 2; }
BUILD=$(cd "${1:?build dir}" && pwd) || exit 2
ULIB=$(cd "${2:?ulfius lib dir}" && pwd) || exit 2
HERE=$(cd "$(dirname "$0")" && pwd) || exit 2
HTTP_PORT=18100
TLS_PORT=18463
URL=http://127.0.0.1:$HTTP_PORT
T=$(mktemp -d)
SYS=$T/sys
STAGE=$T/tmp/ext-upload.ffx
RAN=$T/forgeext.ran
HOST_STATUS=$T/run/ext-host.status
REC=$T/data/setup.json
PKG=org.example.hostext
PID=
HPID=
cleanup() {
    if [ -n "$PID" ]; then kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; fi
    if [ -n "$HPID" ]; then kill $HPID 2>/dev/null; wait $HPID 2>/dev/null; fi
    rm -rf "$T"
}
trap 'rc=$?; cleanup; exit $rc' EXIT

fails=0
ok() { echo "  ok: $1"; }
bad() { echo "  FAIL: $1"; fails=$((fails + 1)); }
expect() { if [ "$2" = "$1" ]; then ok "$3"; else bad "$3 (got '$2', want '$1')"; fi; }
fatal() { echo "FAIL: $1"; echo "--- the daemon's output"; tail -60 "$T/out" 2>/dev/null; exit 1; }
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
# get <path> <token or -> [curl args]: the status; the body is in $T/body
get() {
    p=$1; tok=$2; shift 2
    if [ "$tok" = - ]; then
        curl -s -o "$T/body" -w '%{http_code}' "$@" "$URL$p"
    else
        curl -s -o "$T/body" -w '%{http_code}' -H "X-ForgeFIRM-Token: $tok" "$@" "$URL$p"
    fi
}
# upload <token or -> <file>: the archive as the panel's form sends it
upload() { post /ext/upload "$1" -F "file=@$2;filename=package.ffx"; }
runs() { [ -f "$RAN" ] && wc -l < "$RAN" || echo 0; }
# the runner's state, in $T/dark.json
dark() { curl -s -o "$T/dark.json" "$URL/wiz/dark"; }
at_prompt() { dark && [ "$(jget "$T/dark.json" '(d.get("prompt") or {}).get("id") or ""')" = ready ]; }
ended() { dark && [ "$(jget "$T/dark.json" 'd.get("running")')" = false ]; }
# the check's entry in GET /wiz, as JSON
listed() { curl -s -o "$T/wiz.json" "$URL/wiz" && jget "$T/wiz.json" \
    'next((e for e in d.get("extensions") or [] if e.get("id") == "pkg:'$PKG'"), None)'; }
# the setup record's wizards, sorted, one line
record_wizards() { jget "$REC" '" ".join(sorted((d.get("wizards") or {}).keys()))'; }

start_daemon() {
    env LD_LIBRARY_PATH="$ULIB" FORGECTRL_HOST_TEST=1 \
        FORGECTRL_DATA_DIR="$T/data" FORGECTRL_CONF="$T/data/forgefirm.conf" \
        FFLOG_CONF="$T/data/forgefirm.conf" FFLOG_SOCK="$T/no-log" FFLOG_STDERR=1 \
        GF_RUN_DIR="$T/run" GF_SYSFS_ROOT="$SYS/" GF_SWITCH_DEV="$T/no-switches" \
        GF_SWITCH_WORD_FILE="$T/switches" GF_PULSE_DEV="$T/pulse" \
        FORGECTRL_GRBL_BIN="$T/no-controller" FORGECTRL_CLOUD_BIN="$T/no-controller" \
        FORGECTRL_FORGEEXT="$T/bin/forgeext" FORGECTRL_EXT_STATUS="$HOST_STATUS" \
        FORGECTRL_EXT_STAGE="$STAGE" \
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

# The machine, as host_routes.sh builds it, and the extension host's
# stand-in, compiled.
mkdir -p "$T/data" "$T/run" "$T/bin" "$T/keys" "$SYS/cnc" "$SYS/thermal" "$SYS/head"
echo idle > "$SYS/cnc/state"
for a in cnc/stop cnc/laser_latch cnc/enable cnc/disable cnc/halt \
         head/air_assist_pwm thermal/exhaust_pwm thermal/intake_pwm; do
    : > "$SYS/$a"
done
echo 8 > "$T/switches"
: > "$T/pulse"
echo "cool_intake_fans = 1" > "$T/data/forgefirm.conf"
command -v cc > /dev/null || fatal "no C compiler (cc): the host's stand-in is compiled here"
cc -O2 -DRAN_FILE="\"$RAN\"" -DKEY_DIR="\"$T/keys\"" -o "$T/bin/forgeext" "$HERE/host_ext_forgeext.c" \
    || fatal "the host's stand-in did not compile"
printf 'these bytes are refuse to the host\n' > "$T/refused.ffx"
i=0
while [ $i -lt 200 ]; do echo "line $i of an archive the stand-in takes"; i=$((i + 1)); done > "$T/taken.ffx"

# The host that is alive, before the daemon.
FORGECTRL_EXT_STATUS="$HOST_STATUS" "$T/bin/forgeext" hold &
HPID=$!
until_ok 50 test -s "$HOST_STATUS" || fatal "the stand-in host wrote no status file"

# A first start reads the documents' hashes and the required steps; the
# record seeded accepts the advisories and records every step but the
# last, so the gate stays closed.
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
    'wizards': {i: {'version': version[i], 'completed': ts, 'result': {}, 'applied': {}}
                for i in st['missing'][:-1]},
    'flags': {},
}
json.dump(rec, open(sys.argv[2], 'w'), indent=2)
EOF
start_daemon
until_ok 50 test -s "$T/data/panel.token" || fatal "no panel token was written"
TOKEN=$(cat "$T/data/panel.token")

echo "== the upload sink"
expect 403 "$(upload - "$T/taken.ffx")" "an upload without the token is 403"
[ ! -e "$STAGE" ] && ok "and nothing is staged" || bad "an upload without the token staged a file"

echo running > "$SYS/cnc/state"
n=$(runs)
expect 409 "$(upload "$TOKEN" "$T/taken.ffx")" "an upload while the machine runs is 409"
grep -q 'not idle' "$T/body" && ok "and says not idle" || bad "the body is $(cat "$T/body")"
[ ! -e "$STAGE" ] && ok "nothing is staged" || bad "an upload while the machine runs staged a file"
expect "$n" "$(runs)" "the stand-in host was not run"
echo idle > "$SYS/cnc/state"

expect 400 "$(upload "$TOKEN" "$T/refused.ffx")" "bytes the host refuses are 400"
grep -qF 'the stand-in host will not take these bytes' "$T/body" && ok "in the host's words" \
    || bad "the body is $(cat "$T/body")"
[ ! -e "$STAGE" ] && ok "and the staging file is gone" || bad "the refused bytes are still staged"

expect 200 "$(upload "$TOKEN" "$T/taken.ffx")" "bytes the host takes are 200"
expect 'unverified|button' "$(jget "$T/body" '"%s|%s" % (d.get("tier"), d.get("consent"))')" \
    "tier unverified, consent button"
cmp -s "$T/taken.ffx" "$STAGE" && ok "the staging file holds those bytes" \
    || bad "the staging file does not hold the bytes sent"
expect 200 "$(post /ext/upload/discard "$TOKEN")" "the discard is 200"
[ ! -e "$STAGE" ] && ok "the discard leaves no staging file" || bad "the discarded upload is still staged"

echo "== the install and a key"
expect 409 "$(post /ext/install "$TOKEN")" "an install with nothing staged is 409"
grep -q 'staged' "$T/body" && ok "and says nothing is staged" || bad "the body is $(cat "$T/body")"
expect 409 "$(post /ext/key "$TOKEN" -d name=maker --data-urlencode 'key=a public key the host never sees')" \
    "a key with the token is 409"
grep -q 'button' "$T/body" && ok "and names the button" || bad "the body is $(cat "$T/body")"
grep -q '^key-add' "$RAN" 2>/dev/null && bad "the stand-in host was run as key-add" || ok "the host was not asked to add it"
[ -z "$(ls -A "$T/keys")" ] && ok "and no key was written" || bad "a key was written: $(ls "$T/keys")"

echo "== the package routes"
n=$(runs)
expect 403 "$(get /ext/status -)" "GET /ext/status without the token is 403"
expect 403 "$(post /ext/package - -d id=$PKG -d action=disable)" "POST /ext/package without the token is 403"
expect "$n" "$(runs)" "the host was not run"
expect 200 "$(get /ext/status "$TOKEN")" "GET /ext/status with the token is 200"
expect "$PKG|community|true|advisory|[\"hold\"]|[\"wizard\"]|forgetest check|[\"wizard\"]" \
    "$(jget "$T/body" '(lambda p: "|".join(str(x) if isinstance(x, str) else json.dumps(x) for x in (p.get("id"), p.get("tier"), p.get("enabled"), p.get("hold"), p.get("grants"), p.get("effective"), (p.get("package") or {}).get("name"), (p.get("package") or {}).get("capabilities"))))((d.get("packages") or [{}])[0])')" \
    "the package as the host lists it"
expect "true|$HPID" "$(jget "$T/body" '"%s|%s" % (json.dumps(d["host"].get("running")), d["host"].get("pid"))')" \
    "the host running, with the stand-in's pid"
expect 200 "$(post /ext/package "$TOKEN" -d id=$PKG -d action=enable)" "enable of $PKG is 200"
grep -qx "enable $PKG" "$RAN" && ok "run as the host's enable $PKG" || bad "the host ran: $(tail -3 "$RAN")"
n=$(runs)
expect 400 "$(post /ext/package "$TOKEN" -d id=$PKG -d action=install)" "an action outside the list is 400"
expect 400 "$(post /ext/package "$TOKEN" --data-urlencode 'id=org.example;reboot' -d action=disable)" \
    "an id that is no package id is 400"
expect "$n" "$(runs)" "the host was not run"
expect 409 "$(post /ext/package "$TOKEN" -d id=org.example.nothere -d action=disable)" \
    "disable of a package that is not installed is 409"
grep -qF 'that package is not installed' "$T/body" && ok "in the host's words" || bad "the body is $(cat "$T/body")"

echo "== a package's check"
WID=pkg:$PKG
listed > /dev/null || fatal "GET /wiz failed"
GATE=$(jget "$T/wiz.json" 'd.get("gate")')
WIZARDS=$(record_wizards)
expect '{"done": false, "id": "'$WID'", "title": "forgetest check"}' "$(listed)" "GET /wiz lists the check, not done"
expect 200 "$(post /wiz/$WID/start "$TOKEN")" "POST /wiz/$WID/start is 200"
if until_ok 100 at_prompt; then
    ok "the run reaches the check's prompt"
else
    bad "no prompt: $(cat "$T/dark.json")"
fi
expect 'True|ready|50|confirm' "$(jget "$T/dark.json" '"%s|%s|%s|%s" % (any(l.endswith("the check begins") for l in d.get("log") or []), d.get("phase"), d.get("progress"), (d.get("prompt") or {}).get("kind"))')" \
    "GET /wiz/dark: the check's log line, phase, progress, and a confirm prompt"
get /status - > /dev/null
expect null "$(jget "$T/body" 'json.dumps(d["lease"]["holder"])')" "the machine lease has no holder at the prompt"
SEQ=$(jget "$T/dark.json" 'd["prompt"]["seq"]')
expect 200 "$(post /wiz/$WID/answer "$TOKEN" -d seq=$SEQ -d value=Yes)" "the answer Yes is 200"
until_ok 100 ended || bad "the run did not end: $(cat "$T/dark.json")"
expect '|ready' "$(jget "$T/dark.json" '"%s|%s" % (d.get("error") or "", (d.get("result") or {}).get("summary"))')" \
    "the run ends with no error and the check's summary"
grep "^wizard $PKG answer " "$RAN" | grep -qF '"answer":"yes"' && ok "the host was answered yes" \
    || bad "the host's answers: $(grep answer "$RAN")"
sleep 11                        # the Setup page's list is built again at most every 10 s
expect '{"done": true, "id": "'$WID'", "title": "forgetest check"}' "$(listed)" "GET /wiz then lists the check done"
expect "$WIZARDS" "$(record_wizards)" "the setup record's wizards are as they were ($(echo $WIZARDS | wc -w) of them)"
case " $(record_wizards)" in *" pkg:"*) bad "the setup record holds a package's check" ;; *) ok "and none is a package's" ;; esac
expect "$GATE" "$(jget "$T/wiz.json" 'd.get("gate")')" "the gate is as it was ($GATE)"

expect 200 "$(post /wiz/$WID/start "$TOKEN")" "a second run is 200"
until_ok 100 at_prompt && ok "and reaches the prompt" || bad "no prompt: $(cat "$T/dark.json")"
expect 200 "$(post /wiz/$WID/abort "$TOKEN")" "POST /wiz/$WID/abort is 200"
until_ok 100 ended || bad "the run did not end: $(cat "$T/dark.json")"
expect aborted "$(jget "$T/dark.json" 'd.get("error")')" "the run ends aborted"
grep -qx "wizard $PKG abort" "$RAN" && ok "the host was told of the abort" || bad "the host ran: $(tail -3 "$RAN")"
expect 404 "$(post /wiz/pkg:org.forgetest.nothere/start "$TOKEN")" "a check of a package that is not installed is 404"

stop_daemon
if [ $fails -eq 0 ]; then
    echo "PASS: host_ext"
else
    echo "FAIL: host_ext, $fails checks"
    echo "--- the daemon's output"
    tail -60 "$T/out"
    exit 1
fi
