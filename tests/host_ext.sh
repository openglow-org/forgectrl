#!/bin/sh
# host_ext.sh - the package upload sink and its install door, on a host
# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# SPDX-License-Identifier: MIT
#
# Starts the built forgectrl with its machine paths in a temporary
# directory, as host_routes.sh does (the switch word, the sysfs tree, the
# pulse device a plain file), with FORGECTRL_FORGEEXT a stand-in for the
# extension host's command line and FORGECTRL_EXT_STAGE the staging file
# under the same directory. The stand-in records each time it is run; at
# inspect it reads an archive as tier unverified, and refuses one that
# carries the word refuse, in its own words; as key-add it would write
# the key where the host does. One case each:
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
#
# No package is installed and no key is added: each takes the machine's
# button, which a host does not have. Nothing posts confirm=1, a reboot,
# or /system/ssh. The daemon runs on the image's ulfius
# (tests/ulfius_image.sh). Runs unprivileged; build.yml runs it:
#
#   sh tests/host_ext.sh <build-dir> <ulfius-lib-dir>

# as root the daemon would write the paths it has no override for (its
# markers under /run)
[ "$(id -u)" != 0 ] || { echo "host_ext.sh runs unprivileged"; exit 2; }
BUILD=$(cd "${1:?build dir}" && pwd) || exit 2
ULIB=$(cd "${2:?ulfius lib dir}" && pwd) || exit 2
HTTP_PORT=18100
TLS_PORT=18463
URL=http://127.0.0.1:$HTTP_PORT
T=$(mktemp -d)
SYS=$T/sys
STAGE=$T/tmp/ext-upload.ffx
RAN=$T/forgeext.ran
PID=
cleanup() {
    if [ -n "$PID" ]; then kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; fi
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
# upload <token or -> <file>: the archive as the panel's form sends it
upload() { post /ext/upload "$1" -F "file=@$2;filename=package.ffx"; }
runs() { [ -f "$RAN" ] && wc -l < "$RAN" || echo 0; }

# The machine, as host_routes.sh builds it, and the extension host's
# stand-in.
mkdir -p "$T/data" "$T/run" "$T/bin" "$T/keys" "$SYS/cnc" "$SYS/thermal" "$SYS/head"
echo idle > "$SYS/cnc/state"
for a in cnc/stop cnc/laser_latch cnc/enable cnc/disable cnc/halt \
         head/air_assist_pwm thermal/exhaust_pwm thermal/intake_pwm; do
    : > "$SYS/$a"
done
echo 8 > "$T/switches"
: > "$T/pulse"
echo "cool_intake_fans = 1" > "$T/data/forgefirm.conf"
cat > "$T/bin/forgeext" <<EOF
#!/bin/sh
echo "\$*" >> "$RAN"
case \$1 in
inspect)
    if grep -q refuse "\$2"; then
        echo '{"ok": false, "error": "the stand-in host will not take these bytes"}'
        exit 1
    fi
    echo '{"ok": true, "tier": "unverified", "needs_grant": [], "package": {"id": "org.example.hostext", "version": "1.0.0"}}' ;;
key-add) cp "\$3" "$T/keys/\$2.pub" && echo '{"ok": true}' ;;
list)    echo '{"ok": true, "packages": []}' ;;
keys)    echo '{"ok": true, "keys": []}' ;;
*)       echo '{"ok": false, "error": "the stand-in host answers no such command"}'; exit 1 ;;
esac
EOF
chmod +x "$T/bin/forgeext"
printf 'these bytes are refuse to the host\n' > "$T/refused.ffx"
i=0
while [ $i -lt 200 ]; do echo "line $i of an archive the stand-in takes"; i=$((i + 1)); done > "$T/taken.ffx"

env LD_LIBRARY_PATH="$ULIB" FORGECTRL_HOST_TEST=1 \
    FORGECTRL_DATA_DIR="$T/data" FORGECTRL_CONF="$T/data/forgefirm.conf" \
    FFLOG_CONF="$T/data/forgefirm.conf" FFLOG_SOCK="$T/no-log" FFLOG_STDERR=1 \
    GF_RUN_DIR="$T/run" GF_SYSFS_ROOT="$SYS/" GF_SWITCH_DEV="$T/no-switches" \
    GF_SWITCH_WORD_FILE="$T/switches" GF_PULSE_DEV="$T/pulse" \
    FORGECTRL_GRBL_BIN="$T/no-controller" FORGECTRL_CLOUD_BIN="$T/no-controller" \
    FORGECTRL_FORGEEXT="$T/bin/forgeext" FORGECTRL_EXT_STAGE="$STAGE" \
    FORGECTRL_PORT=$HTTP_PORT FORGECTRL_TLS_PORT=$TLS_PORT \
    "$BUILD/forgectrl" >> "$T/out" 2>&1 &
PID=$!
curl -s -o /dev/null --retry 30 --retry-connrefused --retry-delay 1 "$URL/status" \
    || fatal "the daemon did not answer"
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

kill $PID
until_ok 75 eval '! alive $PID' || fatal "the daemon did not stop in 15 s"
wait $PID
PID=
if [ $fails -eq 0 ]; then
    echo "PASS: host_ext"
else
    echo "FAIL: host_ext, $fails checks"
    echo "--- the daemon's output"
    tail -60 "$T/out"
    exit 1
fi
