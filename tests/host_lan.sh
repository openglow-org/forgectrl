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
#   with no account in the scratch tree, the token alone from 192.0.2.1
#     writes on the setup page's routes and on no other: a settings write
#     and a scoped token's creation are 403 "make the account first", and
#     the settings file is unchanged; the full log bundle (sanitize=0) is
#     403, the sanitized one alone before the account;
#   a form body over its bounds (a 20 KiB field, 300 fields, five 15 KiB
#     fields, one key sent twice at 10 KiB each, a repeated key one byte
#     over the field bound once joined with its comma) ends with no answer,
#     and a body inside them (the repeated key at the bound) is answered;
#   the camera key: read from 127.0.0.1 with the token (32 characters);
#     with panel_open_reads=0, GET /cam/status from 192.0.2.1 over HTTP
#     and over HTTPS is 403 with no key or a wrong one, and 200 with the
#     key as ?key= or as X-ForgeFIRM-Camera-Key; the key alone writes no
#     setting (403); after a rotation from 127.0.0.1 the old key is 403 and
#     the new one 200; panel_open_reads is then put back (127.0.0.1 itself
#     reads with the reads closed, so the closed reads are the LAN
#     client's);
#   GET /events with X-ForgeFIRM-Client: extension-host from 127.0.0.1 in
#     the daemon's namespace is 200 text/event-stream greeting with event:
#     hello, and the same header from 192.0.2.1 does not end it: the host's
#     stream carries an event published after that stream was admitted
#     (the motors.released marker made and removed), with no event: bye
#     and no "replaced": the host's slot is a loopback peer's alone;
#   with the reads closed and no account, GET / from 192.0.2.1 is the
#     setup page and GET /wiz with the page's token is 200;
#   a scoped token sent to POST /job over HTTP from 192.0.2.1 is 403 and
#     the log names it, and the upload staged nothing (no run/job.upload);
#   the press, bound to one browser: 192.0.2.1 asks for it; 192.0.2.3 is
#     refused the ask, the take, the cancel and the account step, with no
#     cookie, a forged one, and the holder's with a digit changed, and is
#     the holder with the holder's own cookie; the account step of
#     192.0.2.1 is refused before its press; once its poll says the
#     button's reservation is ready, 127.0.0.1's ask joins that wait and
#     ends nothing, the button pressed, 192.0.2.1 takes the press and
#     makes the account, which ends the request (none stands for
#     192.0.2.3 to be told of), revokes the scoped token made before it
#     and rotates the camera key;
#     the session it gets is no session from 192.0.2.3, and GET /wiz names
#     the account to it alone;
#   GET /setup?step=laser.focus from 192.0.2.1 with no session is then a
#     302 to the login carrying the path and its query;
#   connections held open from four LAN addresses take exactly 48 slots of
#     either listener, and 127.0.0.1 is answered meanwhile on both;
#   restarted with the users file gone and the setup record naming the
#     account, the token alone from 192.0.2.1 writes nothing, the account
#     step included, and the refusal says to hold the button;
#   restarted with the button held (the reset, its hold GF_RESET_HOLD_S=1),
#     on a development image, with GF_PRESS_TIMEOUT_S=3, GF_PRESS_RES_S=1,
#     GF_PRESS_DEBOUNCE_MS=400 and GF_HTTP_IDLE_S=1: a second browser's ask
#     and its account step are refused there too; one request lapses 3 s
#     after its first ask though its holder asks again, and the wait the
#     holder's ask again started ends with it, before its own timeout,
#     leaving no reservation (no press, no linger); a
#     cancel ends it at once; two browsers asking at once while this
#     host's wait stands ready are taken one at a time: one ends that wait
#     and starts its own, not ready 0.6 s later (its teal starts over), the
#     other is refused; a press judged as its request ends is unsaid, the
#     holder's poll is told its request ended and not to press, and the
#     button stays reserved its linger (2.9 s after the press) and no
#     longer 6.6 s after it; a press judged as the page's reservation
#     lapses is unsaid and reserves the button its linger again, which
#     stands 2.9 s after the press and has ended 6.6 s after it; a taken
#     press holds the request
#     for the account step past those 3 s; a polled wait ends at its own
#     timeout; an idle connection to either listener is closed after the
#     1 s.
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
CLIENT2=192.0.2.3
CLIENT3=192.0.2.4
CLIENT4=192.0.2.5
NS=fclan$$
VA=fcla$$
VB=fclb$$
T=$(mktemp -d)
SYS=$T/sys
CONF=$T/data/forgefirm.conf
PID=
EV1=
EV2=
cleanup() {
    for p in $EV1 $EV2; do kill $p 2>/dev/null; wait $p 2>/dev/null; done
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
open_reads() { sed -n 's/^panel_open_reads *= *//p' "$CONF"; }
key() { python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("key") or "")' "$T/body" 2>/dev/null; }

# lan <method> <url> [curl args]: from 192.0.2.1; the status, the body in
# $T/body, the headers in $T/head; never follows a redirect
lan() {
    m=$1; u=$2; shift 2
    curl -sk --interface $CLIENT -X "$m" -D "$T/head" -o "$T/body" -w '%{http_code}' "$@" "$u"
}
# lan2: the same from 192.0.2.3, a second browser
lan2() {
    m=$1; u=$2; shift 2
    curl -sk --interface $CLIENT2 -X "$m" -D "$T/head" -o "$T/body" -w '%{http_code}' "$@" "$u"
}
cookie() { tr -d '\r' < "$T/head" | sed -n "s/^Set-Cookie: $1=\([0-9a-f]*\);.*/\1/Ip" | head -1; }
jfield() { python3 -c 'import json, sys; v = json.load(open(sys.argv[1])).get(sys.argv[2]); print(json.dumps(v) if isinstance(v, (bool, dict, list)) or v is None else v)' "$T/body" "$1" 2>/dev/null; }
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
for a in $CLIENT2 $CLIENT3 $CLIENT4; do
    ip addr add $a/24 dev $VA || fatal "cannot add $a to the client's end"
done
ip -n $NS addr add $DAEMON/24 dev $VB && ip -n $NS link set $VB up && ip -n $NS link set lo up \
    || fatal "cannot raise the daemon's end"

# The scratch tree host_guard.sh uses, the daemon's user owning it.
mkdir -p "$T/data" "$T/run" "$T/bin" "$T/log" "$SYS/cnc" "$SYS/thermal" "$SYS/head"
echo idle > "$SYS/cnc/state"
for a in cnc/stop cnc/laser_latch cnc/laser_latch_cooling cnc/enable cnc/disable cnc/halt cnc/streaming \
         head/air_assist_pwm head/safe thermal/exhaust_pwm thermal/intake_pwm thermal/tec_on thermal/water_pump_on; do
    : > "$SYS/$a"
done
echo 8 > "$T/switches"
: > "$T/pulse"
echo "ui_units = metric" > "$CONF"
# A license bundle of 14 MiB (its manifest and noise that does not
# compress): more than the socket buffers hold, so two readers that do not
# read keep their downloads standing while a third asks.
mkdir -p "$T/lic/common-licenses"
printf 'PACKAGE NAME: forgectrl\nLICENSE: MIT\n\n' > "$T/lic/common-licenses/license.manifest"
head -c 14680064 /dev/urandom > "$T/lic/common-licenses/noise"
tar -C "$T/lic" -czf "$T/licenses.tar.gz" common-licenses
# the press recorded and nothing else: the account route takes a first
# account, and the gate stays closed
printf '{"schema": 1, "acceptance": {"pressed_at": "%s"}}\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$T/data/setup.json"
printf '#!/bin/sh\necho "$0" >> "%s/grbl.started"\nexec sleep 600\n' "$T" > "$T/bin/grbl"
printf '#!/bin/sh\necho "$0" >> "%s/cloud.started"\n' "$T" > "$T/bin/cloud"
chmod +x "$T/bin/grbl" "$T/bin/cloud"
# The machine button: the switch word's file read through a stand-in for
# the switch device's ioctl (bit 2 the button, bit 3 the closed lid).
cat > "$T/swshim.c" <<'EOF'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>

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
chown -R "$RUN_UID:$RUN_GID" "$T"
chmod 755 "$T"

start_daemon() {
    ip netns exec $NS setpriv --reuid="$RUN_UID" --regid="$RUN_GID" --clear-groups \
        env LD_LIBRARY_PATH="$ULIB" LD_PRELOAD="$T/swshim.so" FORGECTRL_HOST_TEST=1 \
            FORGECTRL_DATA_DIR="$T/data" FORGECTRL_CONF="$CONF" \
            FFLOG_CONF="$CONF" FFLOG_SOCK="$T/no-log" FFLOG_STDERR=1 \
            GF_RUN_DIR="$T/run" GF_SYSFS_ROOT="$SYS/" GF_SWITCH_DEV="$T/switches" \
            GF_SWITCH_WORD_FILE="$T/switches" GF_PULSE_DEV="$T/pulse" GF_HTTP_IDLE_S=${IDLE_S:-6} \
            FORGECTRL_GRBL_BIN="$T/bin/grbl" FORGECTRL_CLOUD_BIN="$T/bin/cloud" \
            GF_LOGS_ROOT="$T/log" GF_LOGS_RSYSLOG="$T/rsyslog-forgefirm.conf" \
            GF_LOGS_EFFECTIVE="$T/loglevels" GF_LOGS_STAGE="$T/stage" \
            FORGECTRL_PORT=$HTTP_PORT FORGECTRL_TLS_PORT=$TLS_PORT FORGECTRL_LICENSES="$T/licenses.tar.gz" \
            GF_PRESS_TIMEOUT_S=${PRESS_S:-600} GF_RESET_HOLD_S="${RESET_HOLD_S:-}" GF_PRESS_RES_S="${PRESS_RES_S:-}" \
            GF_PRESS_DEBOUNCE_MS="${DEBOUNCE_MS:-}" \
            GF_DEV_MARKER="${DEV_MARKER:-}" \
            "$BUILD/forgectrl" >> "$T/out" 2>&1 &
    PID=$!
    curl -s -o /dev/null --interface $CLIENT --retry 30 --retry-connrefused --retry-delay 1 \
        "http://$DAEMON:$HTTP_PORT/status" || fatal "the daemon did not answer on $DAEMON"
}
stop_daemon() {
    kill $PID
    until_ok 75 eval '! alive $PID' || fatal "the daemon did not stop in 15 s"
    wait $PID
    PID=
}
start_daemon
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
expect "403|make the account first" "$(lan POST "https://$DAEMON:$TLS_PORT/settings" -H "X-ForgeFIRM-Token: $TOKEN" -d ui_units=metric)|$(jfield error)" \
    "POST /settings with the token over HTTPS is 403, make the account first"
expect imperial "$(units)" "the settings file still says imperial"
expect 403 "$(lan POST "https://$DAEMON:$TLS_PORT/tokens" -H "X-ForgeFIRM-Token: $TOKEN" -d name=lan -d caps=events)" \
    "POST /tokens with the token over HTTPS is 403"
expect 200 "$(inside POST "http://127.0.0.1:$HTTP_PORT/settings" -H "X-ForgeFIRM-Token: $TOKEN" -d ui_units=metric)" \
    "the same settings write from 127.0.0.1 is 200"
expect metric "$(units)" "the settings file says metric again"
expect "403|make the account first: until then the bundle is the sanitized one" \
    "$(lan POST "https://$DAEMON:$TLS_PORT/logs/export?sanitize=0" -H "X-ForgeFIRM-Token: $TOKEN")|$(cat "$T/body")" \
    "the full log bundle with the token before the account is 403: the sanitized one alone"

echo "== form bodies over their bounds, from $CLIENT"
python3 -c 'print("ui_units=" + "x" * 20480, end="")' > "$T/bigfield"
python3 -c 'print("&".join("k%d=v" % i for i in range(300)), end="")' > "$T/manyfields"
python3 -c 'print("&".join("k%d=v" % i for i in range(200)), end="")' > "$T/fewfields"
python3 -c 'print("&".join("k%d=%s" % (i, "x" * 15360) for i in range(5)), end="")' > "$T/total"
python3 -c 'print("ui_units=" + "x" * 10240 + "&ui_units=" + "x" * 10240, end="")' > "$T/repeat"
# joined with its comma: 16383 + 1 + 1 is one over the field bound, 16382 + 1 + 1 is the bound
# (a one-byte second value reaches the post iterator in one piece, with its comma)
python3 -c 'print("ui_units=" + "x" * 16383 + "&ui_units=x", end="")' > "$T/joined.over"
python3 -c 'print("ui_units=" + "x" * 16382 + "&ui_units=x", end="")' > "$T/joined.at"
expect 000 "$(lan POST "https://$DAEMON:$TLS_PORT/settings" -H "X-ForgeFIRM-Token: $TOKEN" --data-binary @"$T/bigfield")" \
    "a 20 KiB field ends with no answer"
expect 000 "$(lan POST "https://$DAEMON:$TLS_PORT/settings" -H "X-ForgeFIRM-Token: $TOKEN" --data-binary @"$T/total")" \
    "five 15 KiB fields (75 KiB in all, each under the field bound) end with no answer"
expect 000 "$(lan POST "https://$DAEMON:$TLS_PORT/settings" -H "X-ForgeFIRM-Token: $TOKEN" --data-binary @"$T/repeat")" \
    "one key sent twice at 10 KiB each (20 KiB joined) ends with no answer"
expect "000|403" "$(lan POST "https://$DAEMON:$TLS_PORT/settings" -H "X-ForgeFIRM-Token: $TOKEN" --data-binary @"$T/joined.over")|$(lan POST "https://$DAEMON:$TLS_PORT/settings" -H "X-ForgeFIRM-Token: $TOKEN" --data-binary @"$T/joined.at")" \
    "a repeated key joined with its comma one byte over the field bound ends unanswered; at the bound it is answered"
expect 000 "$(lan POST "https://$DAEMON:$TLS_PORT/settings" -H "X-ForgeFIRM-Token: $TOKEN" --data-binary @"$T/manyfields")" \
    "300 fields end with no answer"
expect 403 "$(lan POST "https://$DAEMON:$TLS_PORT/settings" -H "X-ForgeFIRM-Token: $TOKEN" --data-binary @"$T/fewfields")" \
    "200 short fields are answered (403, no account)"

echo "== the camera key, with the reads closed"
expect 200 "$(inside GET "http://127.0.0.1:$HTTP_PORT/system/camera-key" -H "X-ForgeFIRM-Token: $TOKEN")" \
    "GET /system/camera-key from 127.0.0.1 with the token is 200"
KEY=$(key)
expect 32 "${#KEY}" "the key is 32 characters"
case $KEY in 0*) WRONG=1${KEY#?} ;; *) WRONG=0${KEY#?} ;; esac
OPEN=$(open_reads)
expect 200 "$(inside POST "http://127.0.0.1:$HTTP_PORT/settings" -H "X-ForgeFIRM-Token: $TOKEN" -d panel_open_reads=0)" \
    "panel_open_reads=0 from 127.0.0.1 is 200"
for base in "http://$DAEMON:$HTTP_PORT" "https://$DAEMON:$TLS_PORT"; do
    expect 403 "$(lan GET "$base/cam/status")" "GET $base/cam/status from $CLIENT with no key is 403"
    expect 403 "$(lan GET "$base/cam/status?key=$WRONG")" "with a wrong key, 403"
    expect 200 "$(lan GET "$base/cam/status?key=$KEY")" "with ?key= of the key, 200"
    expect 200 "$(lan GET "$base/cam/status" -H "X-ForgeFIRM-Camera-Key: $KEY")" "with the key in X-ForgeFIRM-Camera-Key, 200"
done
expect 403 "$(lan POST "https://$DAEMON:$TLS_PORT/settings" -H "X-ForgeFIRM-Camera-Key: $KEY" -d panel_open_reads=0)" \
    "POST /settings from $CLIENT with only the camera key is 403"
# The reads closed and no account: the setup still runs from the LAN.
expect 200 "$(lan GET "https://$DAEMON:$TLS_PORT/")" "GET / from $CLIENT with the reads closed is the setup page"
expect 403 "$(lan GET "https://$DAEMON:$TLS_PORT/wiz")" "GET /wiz with no token is 403"
expect 200 "$(lan GET "https://$DAEMON:$TLS_PORT/wiz" -H "X-ForgeFIRM-Token: $TOKEN")" "GET /wiz with the page's token is 200"
expect 200 "$(inside POST "http://127.0.0.1:$HTTP_PORT/system/camera-key?rotate=1" -H "X-ForgeFIRM-Token: $TOKEN")" \
    "POST /system/camera-key?rotate=1 from 127.0.0.1 with the token is 200"
NEW=$(key)
[ ${#NEW} = 32 ] && [ "$NEW" != "$KEY" ] && ok "the new key is another 32 characters" || bad "the rotation gave '$NEW'"
expect 403 "$(lan GET "http://$DAEMON:$HTTP_PORT/cam/status?key=$KEY")" "the old key is then 403"
expect 200 "$(lan GET "http://$DAEMON:$HTTP_PORT/cam/status?key=$NEW")" "and the new key 200"
# unset is 1, the default; the settings refuse an empty value for it
BACK=${OPEN:-1}
expect 200 "$(inside POST "http://127.0.0.1:$HTTP_PORT/settings" -H "X-ForgeFIRM-Token: $TOKEN" -d "panel_open_reads=$BACK")" \
    "panel_open_reads=$BACK, put back, is 200"
expect "$BACK" "$(open_reads)" "the settings file holds panel_open_reads = $BACK"

echo "== the extension host's event stream"
ip netns exec $NS curl -sN -D "$T/ev1.head" -o "$T/ev1" -H "X-ForgeFIRM-Client: extension-host" \
    "http://127.0.0.1:$HTTP_PORT/events" &
EV1=$!
until_ok 50 grep -qs '^event: hello' "$T/ev1" && ok "the host's stream greets with event: hello" \
    || bad "no greeting on the host's stream"
expect 200 "$(tr -d '\r' < "$T/ev1.head" | sed -n '1s/^HTTP\/[0-9.]* \([0-9]*\).*/\1/p')" \
    "GET /events with the host's header from 127.0.0.1 is 200"
expect text/event-stream "$(tr -d '\r' < "$T/ev1.head" | sed -n 's/^Content-Type: //Ip')" "as text/event-stream"
curl -sN --interface $CLIENT -o "$T/ev2" -H "X-ForgeFIRM-Client: extension-host" "http://$DAEMON:$HTTP_PORT/events" &
EV2=$!
until_ok 50 grep -qs '^event: hello' "$T/ev2" || bad "the stream from $CLIENT was not opened"
# An event published after that stream was admitted reaches the host's
# stream only while its slot is still its own: a replaced stream gets
# event: bye and nothing after it. The motors.released marker is a
# state the sampler reports and this script can make.
touch "$T/run/motors.released"
until_ok 25 grep -qs '^event: motors.released' "$T/ev1" \
    && ok "the host's stream carries an event published after the stream from $CLIENT was admitted" \
    || bad "no event reached the host's stream after the stream from $CLIENT was admitted"
rm -f "$T/run/motors.released"
grep -qs -e '^event: bye' -e replaced "$T/ev1" && bad "the same header from $CLIENT ended the host's stream" \
    || ok "and no event: bye, no replaced: the host's slot is a loopback peer's alone"
kill $EV1 $EV2
wait $EV1 $EV2 2>/dev/null
EV1=
EV2=

echo "== headers, the license bundle, a package's settings"
expect 200 "$(lan GET "https://$DAEMON:$TLS_PORT/")" "GET / from $CLIENT"
expect "nosniff|same-origin" "$(header X-Content-Type-Options)|$(header Referrer-Policy)" \
    "every answer carries nosniff and Referrer-Policy: same-origin"
case $(header Content-Security-Policy) in *"frame-ancestors 'self'"*) ok "a page carries frame-ancestors 'self'" ;;
    *) bad "the page's policy is '$(header Content-Security-Policy)'" ;; esac
expect 200 "$(lan GET "http://$DAEMON:$HTTP_PORT/status")" "GET /status over HTTP from $CLIENT"
expect "nosniff|same-origin" "$(header X-Content-Type-Options)|$(header Referrer-Policy)" \
    "the plain listener's answers carry them too"
expect 200 "$(lan GET "http://$DAEMON:$HTTP_PORT/system/licenses/manifest")" "the license manifest"
grep -q "PACKAGE NAME: forgectrl" "$T/body" && ok "it is the bundle's manifest" || bad "the manifest reads: $(head -c 200 "$T/body")"
mv "$T/licenses.tar.gz" "$T/licenses.away"
expect 200 "$(lan GET "http://$DAEMON:$HTTP_PORT/system/licenses/manifest")" "the manifest again, the bundle file moved away: read once, kept"
mv "$T/licenses.away" "$T/licenses.tar.gz"
# Two downloads that read nothing for 1.5 s, then all of it: each must be
# the whole bundle.
cat > "$T/slowget.py" <<'EOF'
import socket, sys, time
src, host, port, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
s = socket.socket()
s.bind((src, 0))
s.connect((host, port))
s.sendall(b"GET /system/licenses HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n" % host.encode())
time.sleep(1.5)
data = bytearray()
while True:
    b = s.recv(65536)
    if not b:
        break
    data += b
open(out, "wb").write(bytes(data).split(b"\r\n\r\n", 1)[1])
EOF
for n in 1 2; do
    python3 "$T/slowget.py" $CLIENT $DAEMON $HTTP_PORT "$T/lic$n" &
    eval LIC$n=\$!
done
sleep 0.8
expect 503 "$(lan GET "http://$DAEMON:$HTTP_PORT/system/licenses")" "a third download of the bundle while two stream is 503"
wait $LIC1 $LIC2
cmp -s "$T/lic1" "$T/licenses.tar.gz" && cmp -s "$T/lic2" "$T/licenses.tar.gz" && ok "the two streamed downloads are the bundle" \
    || bad "a streamed download differs from the bundle"
expect 405 "$(inside GET "http://127.0.0.1:$HTTP_PORT/ext/settings?id=x&set=%7B%7D" -H "X-ForgeFIRM-Token: $TOKEN")" \
    "GET /ext/settings with a change in it is 405"

echo "== a scoped token over plain HTTP from $CLIENT"
expect 200 "$(inside POST "http://127.0.0.1:$HTTP_PORT/tokens" -H "X-ForgeFIRM-Token: $TOKEN" -d name=lanjob -d caps=motion.job)" \
    "a motion.job token made from 127.0.0.1 (no account)"
SCOPED=$(jfield token)
[ "$(inside POST "http://127.0.0.1:$HTTP_PORT/job/abort" -H "Authorization: Bearer $SCOPED")" != 403 ] \
    && ok "the token reaches POST /job/abort from 127.0.0.1" || bad "the new token is refused: $(cat "$T/body")"
printf 'G0 X1\n' > "$T/prog.gcode"
expect "403|a scoped token is accepted over HTTPS only" \
    "$(lan POST "http://$DAEMON:$HTTP_PORT/job" -H "Authorization: Bearer $SCOPED" -F "program=@$T/prog.gcode")|$(jfield error)" \
    "POST /job over HTTP with it is 403"
until_ok 25 grep -qs "presented over plain HTTP" "$T/out" && ok "and the log names the token" \
    || bad "no log line names the token sent over plain HTTP"
[ ! -e "$T/run/job.upload" ] && ok "the refused upload staged nothing (no run/job.upload)" \
    || bad "the refused upload was staged in run/job.upload"
expect 200 "$(inside GET "http://127.0.0.1:$HTTP_PORT/system/camera-key" -H "X-ForgeFIRM-Token: $TOKEN")" "the camera key before the account"
KEY_BEFORE=$(key)

echo "== the press, bound to one browser"
# The documents, accepted from $CLIENT with the page's token (a setup route).
lan GET "https://$DAEMON:$TLS_PORT/wiz" -H "X-ForgeFIRM-Token: $TOKEN" > /dev/null
python3 -c 'import json, sys
for d in json.load(open(sys.argv[1]))["documents"]:
    print(d["id"], d["hash"], d["phrase"] or "")' "$T/body" > "$T/docs"
while read -r id hash phrase; do
    lan POST "https://$DAEMON:$TLS_PORT/wiz/advisories/accept" -H "X-ForgeFIRM-Token: $TOKEN" \
        --data-urlencode "doc=$id" --data-urlencode "hash=$hash" --data-urlencode "phrase=$phrase" > /dev/null
done < "$T/docs"
expect "409|this browser has not asked for the press" \
    "$(lan2 POST "https://$DAEMON:$TLS_PORT/wiz/advisories/press/take" -H "X-ForgeFIRM-Token: $TOKEN")|$(jfield error)" \
    "a take from $CLIENT2, which asked for nothing, with no request standing, is 409"
expect 200 "$(lan POST "https://$DAEMON:$TLS_PORT/wiz/advisories/press" -H "X-ForgeFIRM-Token: $TOKEN")" \
    "$CLIENT asks for the press"
CLAIM=$(cookie ffpress)
expect 32 "${#CLAIM}" "and holds the request (its cookie, 32 characters)"
case $(header Set-Cookie) in *HttpOnly*Secure*SameSite=Strict*) ok "the cookie is HttpOnly, Secure, SameSite=Strict" ;;
    *) bad "the press cookie reads: $(header Set-Cookie)" ;; esac
expect 409 "$(lan2 POST "https://$DAEMON:$TLS_PORT/wiz/advisories/press" -H "X-ForgeFIRM-Token: $TOKEN")" \
    "$CLIENT2 asking too is 409"
case $(jfield error) in *"another browser asked for the press first"*) ok "the refusal says another browser asked first" ;;
    *) bad "the refusal reads: $(cat "$T/body")" ;; esac
expect 409 "$(lan2 POST "https://$DAEMON:$TLS_PORT/wiz/advisories/press/take" -H "X-ForgeFIRM-Token: $TOKEN")" \
    "$CLIENT2's take is 409"
expect 409 "$(lan2 POST "https://$DAEMON:$TLS_PORT/wiz/advisories/press/cancel" -H "X-ForgeFIRM-Token: $TOKEN")" \
    "$CLIENT2's cancel is 409"
PW=$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')
expect 409 "$(lan2 POST "https://$DAEMON:$TLS_PORT/wiz/account" -H "X-ForgeFIRM-Token: $TOKEN" -d name=intruder -d password=$PW$PW)" \
    "$CLIENT2's account step is 409"
expect 409 "$(lan POST "https://$DAEMON:$TLS_PORT/wiz/account" -H "X-ForgeFIRM-Token: $TOKEN" -H "Cookie: ffpress=$CLAIM" -d name=hostlan -d password=$PW)" \
    "$CLIENT's account step before its press is 409"
# A cookie is the holder only when it is the request's own, every
# character of it: a forged one, and the holder's with its last digit
# changed, are refused from $CLIENT2 at the take, the cancel and the
# account step. The holder's own cookie is the holder from any address
# (the request belongs to the browser that has the cookie).
FORGED=00000000000000000000000000000000
case $CLAIM in *0) NEAR=${CLAIM%?}1 ;; *) NEAR=${CLAIM%?}0 ;; esac
for c in $FORGED $NEAR; do
    for r in press/take press/cancel; do
        expect 409 "$(lan2 POST "https://$DAEMON:$TLS_PORT/wiz/advisories/$r" -H "X-ForgeFIRM-Token: $TOKEN" -H "Cookie: ffpress=$c")" \
            "$CLIENT2's $r with the cookie $c is 409"
    done
    expect 409 "$(lan2 POST "https://$DAEMON:$TLS_PORT/wiz/account" -H "X-ForgeFIRM-Token: $TOKEN" -H "Cookie: ffpress=$c" -d name=intruder -d password=$PW$PW)" \
        "$CLIENT2's account step with the cookie $c is 409"
done
expect "200|false" "$(lan2 POST "https://$DAEMON:$TLS_PORT/wiz/advisories/press/take" -H "X-ForgeFIRM-Token: $TOKEN" -H "Cookie: ffpress=$CLAIM")|$(jfield pressed)" \
    "the holder's own cookie from $CLIENT2 is the holder: its take answers, nothing pressed yet"
ready_now() { [ "$(lan POST "https://$DAEMON:$TLS_PORT/wiz/advisories/press/take" -H "X-ForgeFIRM-Token: $TOKEN" -H "Cookie: ffpress=$CLAIM")" = 200 ] \
              && [ "$(jfield ready)" = true ]; }
until_ok 50 ready_now || bad "the press never came ready: $(cat "$T/body")"
# This host's ask joins the browser's wait (it binds none and ends none).
expect 200 "$(inside POST "http://127.0.0.1:$HTTP_PORT/wiz/advisories/press" -H "X-ForgeFIRM-Token: $TOKEN")" \
    "127.0.0.1 asks for the press too"
ready_now && ok "and the browser's wait still stands, ready" || bad "127.0.0.1's ask ended the browser's wait: $(cat "$T/body")"
sleep 0.2
echo 12 > "$T/switches"; sleep 0.5; echo 8 > "$T/switches"
pressed_now() { [ "$(lan2 GET "https://$DAEMON:$TLS_PORT/wiz/advisories/press")" = 200 ] && [ "$(jfield button)" = pressed ]; }
until_ok 50 pressed_now && ok "GET /wiz/advisories/press from $CLIENT2 reads the press" \
    || bad "the press does not read pressed: $(cat "$T/body")"
expect "200|pressed" "$(lan2 GET "https://$DAEMON:$TLS_PORT/wiz/advisories/press")|$(jfield button)" \
    "and reading it again, it is still there: the read took nothing"
taken() { [ "$(lan POST "https://$DAEMON:$TLS_PORT/wiz/advisories/press/take" -H "X-ForgeFIRM-Token: $TOKEN" -H "Cookie: ffpress=$CLAIM")" = 200 ] \
          && [ "$(jfield pressed)" = true ]; }
until_ok 50 taken && ok "the button pressed, $CLIENT takes its press" || bad "the press was not taken: $(cat "$T/body")"
expect 409 "$(lan2 POST "https://$DAEMON:$TLS_PORT/wiz/account" -H "X-ForgeFIRM-Token: $TOKEN" -d name=intruder -d password=$PW$PW)" \
    "$CLIENT2's account step after that press is still 409"
expect 200 "$(lan POST "https://$DAEMON:$TLS_PORT/wiz/account" -H "X-ForgeFIRM-Token: $TOKEN" -H "Cookie: ffpress=$CLAIM" -d name=hostlan -d password=$PW)" \
    "$CLIENT's account step after its press is 200"
SID=$(cookie ffsid)
B_ACCT=$(cat "$T/body")
expect "200|false" "$(lan2 GET "https://$DAEMON:$TLS_PORT/wiz/advisories/press")|$(jfield asked_by_other)" \
    "the account step ended the request: no request stands for $CLIENT2 to be told of"
echo "$B_ACCT" > "$T/body"
expect "true|true" "$(jfield tokens_revoked)|$(jfield camera_key_rotated)" "the answer says the tokens were revoked and the key rotated"
expect "403|authentication required" "$(inside POST "http://127.0.0.1:$HTTP_PORT/job/abort" -H "Authorization: Bearer $SCOPED")|$(jfield error)" \
    "the token made before the account is refused"
expect 200 "$(inside GET "http://127.0.0.1:$HTTP_PORT/system/camera-key" -H "X-ForgeFIRM-Token: $TOKEN")" "the camera key after the account"
[ -n "$KEY_BEFORE" ] && [ "$(key)" != "$KEY_BEFORE" ] && ok "the camera key changed" || bad "the camera key is the one before the account"
expect "200|true|\"hostlan\"" "$(lan GET "https://$DAEMON:$TLS_PORT/wiz" -H "Cookie: ffsid=$SID")|$(jfield session)|$(python3 -c 'import json,sys; print(json.dumps(json.load(open(sys.argv[1]))["users"]["name"]))' "$T/body")" \
    "GET /wiz from $CLIENT with its session: a session, and the account named"
expect "200|false|\"\"" "$(lan2 GET "https://$DAEMON:$TLS_PORT/wiz" -H "Cookie: ffsid=$SID")|$(jfield session)|$(python3 -c 'import json,sys; print(json.dumps(json.load(open(sys.argv[1]))["users"]["name"]))' "$T/body")" \
    "the same cookie from $CLIENT2: no session, and no name"

echo "== a page from $CLIENT once an account exists"
expect 302 "$(lan GET "https://$DAEMON:$TLS_PORT/setup?step=laser.focus")" \
    "GET /setup?step=laser.focus from $CLIENT with no session is 302"
expect "/login?next=/setup%3Fstep%3Dlaser.focus" "$(header Location)" "to the login, carrying the path and its query"

echo "== connection slots"
# hold <seconds> <port> <host> <address...>: 16 idle connections from each
# address to the listener on <port>; prints how many are still open after
# the wait (what a closed one had queued, a TLS peer's alert, is read
# first). Both listeners at once: each has its own slots. This daemon's
# idle timeout, GF_HTTP_IDLE_S, is 6 s (above the event stream's 5 s
# keep-alive, the slow downloads' pause and this 3 s hold); the idle case
# runs on the last daemon, whose timeout is 1 s.
cat > "$T/hold.py" <<'EOF'
import socket, sys, time
wait, port, host, addrs = float(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4:]
socks = []
for a in addrs:
    for _ in range(16):
        s = socket.socket()
        s.bind((a, 0))
        s.settimeout(2)
        try:
            s.connect((host, port))
            socks.append(s)
        except OSError:
            s.close()
time.sleep(wait)
open_ = 0
for s in socks:
    s.setblocking(False)
    try:
        while True:
            if s.recv(4096) == b"":
                break
    except BlockingIOError:
        open_ += 1
    except OSError:
        pass
print(open_)
EOF
python3 "$T/hold.py" 3 $HTTP_PORT $DAEMON $CLIENT $CLIENT2 $CLIENT3 $CLIENT4 > "$T/held" &
HOLDER=$!
python3 "$T/hold.py" 3 $TLS_PORT $DAEMON $CLIENT $CLIENT2 $CLIENT3 $CLIENT4 > "$T/held.tls" &
HOLDER2=$!
sleep 1
expect 200 "$(inside GET "http://127.0.0.1:$HTTP_PORT/status" --max-time 2)" "127.0.0.1 is answered over HTTP while four LAN addresses hold their connections"
expect 200 "$(inside GET "https://127.0.0.1:$TLS_PORT/status" --max-time 2)" "and over HTTPS"
wait $HOLDER $HOLDER2
for f in held held.tls; do
    HELD=$(cat "$T/$f")
    [ "$HELD" -eq 48 ] && ok "$f: the LAN held its 48 of the 64 slots" || bad "$f: the LAN held $HELD slots, not 48"
done

echo "== the account record in fault"
stop_daemon
rm -f "$T/data/users"
start_daemon
expect 403 "$(lan POST "https://$DAEMON:$TLS_PORT/settings" -H "X-ForgeFIRM-Token: $TOKEN" -d ui_units=imperial)" \
    "users file gone, the record naming the account: POST /settings with the token is 403"
case $(jfield error) in *"hold the machine's button"*) ok "and the refusal says to hold the button" ;;
    *) bad "the refusal reads: $(cat "$T/body")" ;; esac
expect 403 "$(lan POST "https://$DAEMON:$TLS_PORT/wiz/account" -H "X-ForgeFIRM-Token: $TOKEN" -d name=intruder -d password=$PW$PW)" \
    "and the account step with the token is 403"
expect metric "$(units)" "the settings file still says metric"

echo "== sessions end with the old account"
expect "200|true" "$(lan GET "https://$DAEMON:$TLS_PORT/wiz" -H "Cookie: ffsid=$SID")|$(jfield session)" \
    "the session of $CLIENT survived the restart"
expect 200 "$(inside POST "http://127.0.0.1:$HTTP_PORT/wiz/account" -H "X-ForgeFIRM-Token: $TOKEN" -d name=hostlan -d password=$PW$PW)" \
    "an account made from 127.0.0.1 over the record in fault"
expect "200|false" "$(lan GET "https://$DAEMON:$TLS_PORT/wiz" -H "Cookie: ffsid=$SID")|$(jfield session)" \
    "the account made from none ended the session of $CLIENT"
expect 200 "$(inside POST "http://127.0.0.1:$HTTP_PORT/login" -d name=hostlan -d password=$PW$PW)" "a login from 127.0.0.1"
SID2=$(cookie ffsid)
expect "200|true" "$(inside GET "http://127.0.0.1:$HTTP_PORT/wiz" -H "Cookie: ffsid=$SID2")|$(jfield session)" "its session"
stop_daemon
echo 12 > "$T/switches"         # the button held through the start: the reset,
RESET_HOLD_S=1                  # its hold 1 s here (its ten seconds: button_test A),
PRESS_S=3                       # and the press request's life short, for its lifetimes,
PRESS_RES_S=1                   # the renewal a poll gives the press's reservation 1 s,
DEBOUNCE_MS=400                 # a press read again 400 ms after it is first seen (presses held 0.6 s),
IDLE_S=1                        # the idle timeout short, for its case (no stream is held here),
DEV_MARKER=$T/dev-marker        # on a development image, which the press binding holds too
: > "$DEV_MARKER"
chown "$RUN_UID:$RUN_GID" "$DEV_MARKER"
start_daemon
until_ok 100 grep -qs "account reset by the button hold" "$T/out" || bad "the button hold did not reset the account"
echo 8 > "$T/switches"
expect "200|false" "$(inside GET "http://127.0.0.1:$HTTP_PORT/wiz" -H "Cookie: ffsid=$SID2")|$(jfield session)" \
    "the button reset ended the session a restart had kept"

echo "== the press request's lifetimes (GF_PRESS_TIMEOUT_S=$PRESS_S), on a development image"
# One request lives PRESS_S from its first ask, however often its holder
# asks again; a cancel ends it at once; a taken press holds it for the
# account step past that. A development image binds the press to one
# browser as any image does.
press_as() {
    c=$1; shift
    "$c" POST "https://$DAEMON:$TLS_PORT/wiz/advisories/$1" -H "X-ForgeFIRM-Token: $TOKEN" ${2:+-H "Cookie: ffpress=$2"}
}
# until_t <ms>: sleep until <ms> after T0 (ms on the monotonic clock)
mono_ms() { python3 -c 'import time; print(int(time.monotonic() * 1000))'; }
until_t() { python3 -c 'import sys, time; d = (int(sys.argv[1]) + int(sys.argv[2])) / 1000 - time.monotonic(); time.sleep(max(d, 0))' "$T0" "$1"; }
expect 200 "$(press_as lan press)" "$CLIENT asks for the press"
T0=$(mono_ms)                   # the request ends at or before T0 + 3 s
C1=$(cookie ffpress)
expect 409 "$(press_as lan2 press)" "$CLIENT2 asking too is 409, on a development image as on any"
expect "409|press the machine's button for this browser first" \
    "$(lan2 POST "https://$DAEMON:$TLS_PORT/wiz/account" -H "X-ForgeFIRM-Token: $TOKEN" -d name=intruder -d password=$PW$PW)|$(jfield error)" \
    "and its account step with no press of its own is 409"
# The first wait, unpolled, lapsed with its 1 s renewal (GF_PRESS_RES_S=1):
# the holder's ask again starts a second wait, whose own timeout (3 s) runs
# past the request's end. The holder polls it until the request lapses;
# the wait ends with its request, so the next browser's ask finds no wait
# breathing teal for it to join.
until_t 1500
expect "200|waiting" "$(press_as lan press "$C1")|$(jfield button)" "$CLIENT asks again at 1.5 s: a second wait"
while [ $(( $(mono_ms) - T0 )) -lt 3200 ]; do press_as lan press/take "$C1" > /dev/null; sleep 0.2; done
until_t 3400
expect "200|cancelled" "$(lan GET "https://$DAEMON:$TLS_PORT/wiz/advisories/press")|$(jfield button)" \
    "the second wait ended with its request, before its own timeout"
# A request that ends with no press keeps no linger: the reservation
# closes with the wait, and the published verdict says so by 5 s.
vres() { python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("button_reserved"))' "$T/run/cooling.state" 2>&1; }
until_t 5000
expect False "$(vres)" "and the button is not reserved 2 s after that request's end (no press, no linger)"
expect 200 "$(press_as lan2 press)" "$CLIENT2 asks at 5 s: the request lapsed 3 s after its first ask"
C2=$(cookie ffpress)
expect 200 "$(press_as lan2 press/cancel "$C2")" "$CLIENT2 cancels its request"
# Two browsers ask at once while a wait 127.0.0.1 started stands ready.
# The asks are taken one at a time: one ends this host's wait (no browser
# joins a wait another request or this host started) and starts its own,
# whose teal starts over (not ready 0.6 s later, though the reservation
# stood on file throughout); the other is refused, another browser's
# request standing.
T0=$(mono_ms)
expect 200 "$(inside POST "http://127.0.0.1:$HTTP_PORT/wiz/advisories/press" -H "X-ForgeFIRM-Token: $TOKEN")" \
    "127.0.0.1 asks for the press"
# loop_poll <ms>: 127.0.0.1 polls its wait (renewing it) until <ms> after T0
loop_poll() {
    while [ $(( $(mono_ms) - T0 )) -lt "$1" ]; do
        ip netns exec $NS curl -sk -o /dev/null -X POST -H "X-ForgeFIRM-Token: $TOKEN" \
            "http://127.0.0.1:$HTTP_PORT/wiz/advisories/press/take"
        sleep 0.2
    done
}
loop_ready() { [ "$(inside POST "http://127.0.0.1:$HTTP_PORT/wiz/advisories/press/take" -H "X-ForgeFIRM-Token: $TOKEN")" = 200 ] \
                   && [ "$(jfield ready)" = true ]; }
until_ok 15 loop_ready || bad "127.0.0.1's wait never came ready: $(cat "$T/body")"
# ask_bg <address> <n>: an ask with no cookie, its status in $T/c<n>, its
# headers in $T/h<n> and its body in $T/b<n>
ask_bg() {
    curl -sk --interface "$1" -X POST -D "$T/h$2" -o "$T/b$2" -w '%{http_code}' -H "X-ForgeFIRM-Token: $TOKEN" \
        "https://$DAEMON:$TLS_PORT/wiz/advisories/press" > "$T/c$2"
}
ask_bg $CLIENT 1 &
A1=$!
ask_bg $CLIENT2 2 &
A2=$!
wait $A1 $A2
W=
case "$(cat "$T/c1") $(cat "$T/c2")" in
    "200 409") W=lan; N=1 ;;
    "409 200") W=lan2; N=2 ;;
    *) bad "two asks at once answered $(cat "$T/c1") $(cat "$T/b1") | $(cat "$T/c2") $(cat "$T/b2")" ;;
esac
if [ -n "$W" ]; then
    expect False "$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("ready"))' "$T/b$N" 2>&1)" \
        "two asks at once: one makes its request and a wait of its own, not ready, the other is 409"
    CW=$(tr -d '\r' < "$T/h$N" | sed -n "s/^Set-Cookie: ffpress=\([0-9a-f]*\);.*/\1/Ip" | head -1)
    sleep 0.6
    expect "200|waiting|false" "$(press_as $W press/take "$CW")|$(jfield button)|$(jfield ready)" \
        "0.6 s later its own wait is not ready: the teal starts over"
    expect 200 "$(press_as $W press/cancel "$CW")" "and it cancels its request"
fi
# A press caught as its request ends is unsaid, and keeps the button
# reserved its linger, as every press does. (This daemon reads a press again
# 400 ms after it first sees it, GF_PRESS_DEBOUNCE_MS, so a press made
# 0.3 s before the request's end is judged after it.)
expect 200 "$(press_as lan press)" "$CLIENT asks for the press"
T0=$(mono_ms)
CLAIM=$(cookie ffpress)
until_ok 25 ready_now || bad "the press never came ready: $(cat "$T/body")"
# The holder keeps polling (renewing the reservation) past the request's
# end, beside the press.
( while [ $(( $(mono_ms) - T0 )) -lt 3200 ]; do
      curl -sk --interface $CLIENT -o /dev/null -X POST -H "X-ForgeFIRM-Token: $TOKEN" -H "Cookie: ffpress=$CLAIM" \
          "https://$DAEMON:$TLS_PORT/wiz/advisories/press/take"
      sleep 0.2
  done ) &
POLLER=$!
until_t 2700
echo 12 > "$T/switches"; sleep 0.6; echo 8 > "$T/switches"
wait $POLLER
expect "200|cancelled" "$(lan GET "https://$DAEMON:$TLS_PORT/wiz/advisories/press")|$(jfield button)" \
    "a press judged after its request ended is unsaid"
case "$(press_as lan press/take "$CLAIM")|$(jfield error)" in
    "409|"*"request for the press ended: do not press"*) ok "the holder's poll is told its request ended and not to press" ;;
    *) bad "the holder's poll after its request ended: $(cat "$T/body")" ;;
esac
until_t 6000
expect True "$(vres)" "and the button stays reserved 2.9 s after that press (its linger)"
# The linger ends: judged at about 3.1 s, its 5 s run out and the next
# published verdict says so by 9.7 s.
until_t 9700
expect False "$(vres)" "and is no longer reserved 6.6 s after that press (its linger ended)"
# A press caught as the reservation lapses (the page stopped polling: the
# 1 s renewal, GF_PRESS_RES_S, runs out while the press is read again) is
# unsaid too, and reserves the button its linger again.
expect 200 "$(press_as lan press)" "$CLIENT asks for the press again"
CLAIM=$(cookie ffpress)
until_ok 25 ready_now || bad "the press never came ready: $(cat "$T/body")"
T0=$(mono_ms)                   # the last poll: the reservation lapses at or before T0 + 1 s
until_t 650
echo 12 > "$T/switches"; sleep 0.6; echo 8 > "$T/switches"
expect "200|cancelled" "$(lan GET "https://$DAEMON:$TLS_PORT/wiz/advisories/press")|$(jfield button)" \
    "a press judged as the page's reservation lapsed is unsaid"
until_t 4000
expect True "$(vres)" "and the button stays reserved 2.9 s after that press (its linger again)"
until_t 7700
expect False "$(vres)" "and is no longer reserved 6.6 s after that press (that linger ended)"
# A press taken within its request's 3 s. The wait comes ready once its
# reservation has stood 1 s on file, counted from the first verdict that
# carries it, and the cooling engine publishes once a second: an ask made
# just after a publish comes ready at about 2 s, which leaves a press read
# again 400 ms later too little of the 3 s. The ask goes out about 0.25 s
# before a publish (cooling.state's mtime marks each one), so the wait
# comes ready at about 1.25 s.
python3 -c 'import os, sys, time
p = sys.argv[1]
m = os.stat(p).st_mtime_ns
end = time.monotonic() + 3
while os.stat(p).st_mtime_ns == m and time.monotonic() < end:
    time.sleep(0.01)
time.sleep(0.75)' "$T/run/cooling.state"
expect 200 "$(press_as lan press)" "$CLIENT asks then"
T0=$(mono_ms)
CLAIM=$(cookie ffpress)
until_ok 25 ready_now || bad "the press never came ready: $(cat "$T/body")"
sleep 0.2
echo 12 > "$T/switches"; sleep 0.6; echo 8 > "$T/switches"
until_ok 15 taken || bad "the press was not taken: $(cat "$T/body")"
TP=$T0
# The idle timeout, on this daemon's short one (GF_HTTP_IDLE_S=1): 16 idle
# connections to each listener are closed. Meanwhile a wait 127.0.0.1 asks
# for and keeps polling ends at its own timeout (GF_PRESS_TIMEOUT_S=3).
expect 200 "$(inside POST "http://127.0.0.1:$HTTP_PORT/wiz/advisories/press" -H "X-ForgeFIRM-Token: $TOKEN")" \
    "127.0.0.1 asks for the press again"
T0=$(mono_ms)
loop_poll 3200 &
POLLER=$!
python3 "$T/hold.py" 2.5 $HTTP_PORT $DAEMON $CLIENT > "$T/held" &
HOLDER=$!
python3 "$T/hold.py" 2.5 $TLS_PORT $DAEMON $CLIENT > "$T/held.tls"
wait $HOLDER
expect "0|0" "$(cat "$T/held")|$(cat "$T/held.tls")" "16 idle connections to each listener are closed within 2.5 s (the timeout is 1 s)"
wait $POLLER
until_t 3400
expect "200|timeout" "$(inside GET "http://127.0.0.1:$HTTP_PORT/wiz/advisories/press")|$(jfield button)" \
    "the polled wait timed out at its 3 s"
# The taken press, read now that more than 4 s have passed since its ask:
# its request outlives the 3 s, held for the account step.
T0=$TP
[ $(( $(mono_ms) - T0 )) -ge 4000 ] || bad "the taken press's case read before 4 s"
expect 409 "$(press_as lan2 press)" "past 4 s after its ask, the request whose press was taken still stands"
expect 200 "$(lan POST "https://$DAEMON:$TLS_PORT/wiz/account" -H "X-ForgeFIRM-Token: $TOKEN" -H "Cookie: ffpress=$CLAIM" -d name=hostlan -d password=$PW$PW)" \
    "and its holder's account step is 200"

stop_daemon
if [ $fails -eq 0 ]; then
    echo "PASS: host_lan"
else
    echo "FAIL: host_lan, $fails checks"
    echo "--- the daemon's output"
    tail -60 "$T/out"
    exit 1
fi
