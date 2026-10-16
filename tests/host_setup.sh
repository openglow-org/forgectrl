#!/bin/sh
# host_setup.sh - the setup gate, its consents, and its checks, on a host
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
#   the checks, driven as the page drives them (start, the status
#     polled, each prompt answered), on scripted readings:
#     sensors: a judge that fails ends the check with no result and no
#       record; at rest it asks nothing, its result carries the readings,
#       the record carries it, and no setting is written
#     the intake fans: both tachs turning count two, the setting written
#       and the record carrying it, no question; intake 2 silent asks for
#       the count, and the answer is written
#     the intake-count prompt with the wall clock an hour behind: the
#       prompt holds (its wait rides CLOCK_MONOTONIC) and takes the answer
#     the cameras: an open lid is waited for; the lid shot, its question,
#       the head shot, its question; each shot served as captured, and
#       the result names the engine's sensor
#     the bed check: an unhomed machine is refused with nothing written
#       and the envelope never opened; homed, every prompt is a jog that
#       offers This is the end, the ends are the home plus the jogs, the
#       keys the ends less 1 mm, the envelope opened and applied; a
#       Grbl client's line closing the envelope ends the next run at its
#       answer with nothing written
#     the flow check: the diagnostic's phases are the check's, its
#       result the check's with pass true, and a thin margin flags the
#       calibration in the record
#     the flow calibration aborted at its first rung: the heater duty
#       as it was (unset stays unset, a seeded one comes back)
#   the laser floor burn: refused before any prompt without Focus on the
#     record; with it, the gantry stops asked, then the controller
#     stopped and started again and the lens referenced before the home
#     question, the lens motor at rest and the lock unwritten before the
#     scrap question, then the
#     thickness: one outside the lens window is not stored and is asked
#     again, with no program line streamed, nothing armed and
#     laser_floor_density unwritten at either question; the program's
#     ;keys=floor-off reaches the controller as M102 with the floor 0 and
#     the dose curve off, the keys are as they were after the burn, and
#     the pick 8 writes the floor 10
#   the first run, in the page's order: each document accepted at its
#     hash, the press step breathing teal, the press accepted, the
#     account and its cookie, the gate's reason after each form step,
#     the intake fan count opening the gate, and the run completed
#
# The switch device is a plain file: a preloaded stand-in answers the
# readers' switch-state request from the switch word, so the lid, the
# button and the motion gate read one word. The cameras' capture is the
# stand-in FORGECTRL_WIZ_SHOTS names, the button LEDs a scratch tree. In
# the burn the controller is a stand-in on the Grbl socket (port 23 on
# loopback, which the preloaded stand-in points at a high port), the
# lens's hall sensor a stand-in that follows the lens steps, and the
# burn's witnesses files the controller's stand-in sets: no laser fires.
# A second preloaded stand-in reads the wall clock an hour behind while
# a flag file exists, for the prompt that must hold across the shift.
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
# The lens's sysfs: none (no head) but in the burn's case
LENS_SYS=$T/no-head
GRBL_PORT=18471
PID=
LPID=
PLANT=
HALL=
cleanup() {
    if [ -n "$PID" ]; then kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; fi
    [ -z "$LPID" ] || kill $LPID 2>/dev/null
    for p in $PLANT $HALL; do kill $p 2>/dev/null; done
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
    env LD_LIBRARY_PATH="$ULIB" FORGECTRL_HOST_TEST=1 TIMESHIM_FLAG="$T/timeshift" \
        FORGECTRL_DATA_DIR="$T/data" FORGECTRL_CONF="$CONF" \
        FFLOG_CONF="$CONF" FFLOG_SOCK="$T/no-log" FFLOG_STDERR=1 \
        GF_RUN_DIR="$T/run" GF_SYSFS_ROOT="$SYS/" GF_SWITCH_DEV="$T/switches" \
        GF_SWITCH_WORD_FILE="$T/switches" GF_PULSE_DEV="$T/pulse" LD_PRELOAD="$T/swshim.so $T/timeshim.so" \
        GF_LED_ROOT="$T/leds" FORGECTRL_WIZ_SHOTS="$T/shots" \
        GF_SYSFS_DIR="$LENS_SYS" GF_LENS_STEP_US=20000 HOST_GRBL_PORT=$GRBL_PORT \
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
# plain file. Then the readings the checks take at rest: both coolant
# sensors at 25 C (ADC 666), the lid IR low, no tube current, the
# supply's power-good sampled good, every fan tach at 1500 rpm (a 20 ms
# period, two pulses a turn), the pump on, the purge drawing.
mkdir -p "$T/data" "$T/run" "$T/bin" "$SYS/cnc" "$SYS/thermal" "$SYS/head" "$SYS/pic" "$T/shots"
echo idle > "$SYS/cnc/state"
for a in cnc/stop cnc/laser_latch cnc/enable cnc/disable cnc/halt \
         head/air_assist_pwm thermal/exhaust_pwm thermal/intake_pwm thermal/heater_pwm; do
    : > "$SYS/$a"
done
at_rest() {
    echo 666 > "$SYS/pic/water_temp_1"
    echo 666 > "$SYS/pic/water_temp_2"
    for i in 1 2 3 4; do echo 10 > "$SYS/pic/lid_ir_$i"; done
    echo 0 > "$SYS/pic/hv_current"
    echo 1 > "$SYS/cnc/laser_pgood_sampled"
    for f in tach_exhaust tach_intake_1 tach_intake_2; do echo 20000000 > "$SYS/thermal/$f"; done
    echo 1 > "$SYS/thermal/water_pump_on"
    echo 1 > "$SYS/head/purge_air"
    echo 630 > "$SYS/head/purge_air_current"
}
at_rest
for ch in 1 2 3; do mkdir -p "$T/leds/button_led_$ch"; done
echo 8 > "$T/switches"
# sw <word>: the switch word, replaced whole (bit 3 the lid closed, bit 2
# the button held)
printf '#!/bin/sh\necho "$1" > "%s/switches.new" && mv "%s/switches.new" "%s/switches"\n' "$T" "$T" "$T" > "$T/bin/sw"
chmod +x "$T/bin/sw"
sw() { "$T/bin/sw" "$1"; }
: > "$T/pulse"
: > "$T/port.log"
echo "cool_intake_fans = 1" > "$CONF"

# The readers' switch-state request (EVIOCGSW) on the switch device,
# answered from the switch word; every other request goes through. For
# the burn's case it also points the Grbl socket (port 23 on loopback)
# at HOST_GRBL_PORT, and the dose keys' saved-keys marker into the data
# directory, as curverec_test's wrapped fopen does.
cat > "$T/swshim.c" <<'EOF'
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

int connect(int fd, const struct sockaddr *sa, socklen_t len)
{
    static int (*real)(int, const struct sockaddr *, socklen_t);
    if (!real)
        real = (int (*)(int, const struct sockaddr *, socklen_t))dlsym(RTLD_NEXT, "connect");
    const char *p = getenv("HOST_GRBL_PORT");
    if (p && sa && sa->sa_family == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
        struct sockaddr_in6 a;
        memcpy(&a, sa, sizeof(a));
        if (ntohs(a.sin6_port) == 23 && IN6_IS_ADDR_LOOPBACK(&a.sin6_addr)) {
            a.sin6_port = htons((unsigned short)atoi(p));
            return real(fd, (struct sockaddr *)&a, sizeof(a));
        }
    } else if (p && sa && sa->sa_family == AF_INET && len >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in a;
        memcpy(&a, sa, sizeof(a));
        if (ntohs(a.sin_port) == 23 && a.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
            a.sin_port = htons((unsigned short)atoi(p));
            return real(fd, (struct sockaddr *)&a, sizeof(a));
        }
    }
    return real(fd, sa, len);
}

static const char *moved(const char *path, char *buf, size_t len)
{
    const char *d = getenv("FORGECTRL_DATA_DIR");
    if (d && path && !strcmp(path, "/data/forgefirm/curverec.saved")) {
        snprintf(buf, len, "%s/curverec.saved", d);
        return buf;
    }
    return path;
}

FILE *fopen(const char *path, const char *mode)
{
    static FILE *(*real)(const char *, const char *);
    char b[512];
    if (!real)
        real = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, "fopen");
    return real(moved(path, b, sizeof(b)), mode);
}

FILE *fopen64(const char *path, const char *mode)
{
    static FILE *(*real)(const char *, const char *);
    char b[512];
    if (!real)
        real = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, "fopen64");
    return real(moved(path, b, sizeof(b)), mode);
}

int unlink(const char *path)
{
    static int (*real)(const char *);
    char b[512];
    if (!real)
        real = (int (*)(const char *))dlsym(RTLD_NEXT, "unlink");
    return real(moved(path, b, sizeof(b)));
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
# The clock stand-in: the daemon's own reading of CLOCK_REALTIME is an
# hour behind while the file TIMESHIM_FLAG names exists. The kernel's
# clock is not touched, so a condvar deadline computed from that
# reading is already past for a condvar on CLOCK_REALTIME and untouched
# for one on CLOCK_MONOTONIC.
cat > "$T/timeshim.c" <<'EOF'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int clock_gettime(clockid_t clk, struct timespec *ts)
{
    static int (*real)(clockid_t, struct timespec *);
    if (!real)
        real = (int (*)(clockid_t, struct timespec *))dlsym(RTLD_NEXT, "clock_gettime");
    int rc = real(clk, ts);
    if (rc == 0 && clk == CLOCK_REALTIME) {
        const char *f = getenv("TIMESHIM_FLAG");
        if (f && access(f, F_OK) == 0)
            ts->tv_sec -= 3600;
    }
    return rc;
}
EOF
cc -shared -fPIC -O2 -o "$T/timeshim.so" "$T/timeshim.c" -ldl || fatal "the clock stand-in did not build"
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

# The controller's port, as host_routes.sh stands it in: every line
# recorded, state answered in the controller's form, a jog moving the
# position it reports, the envelope opened (refused, error:homed, until
# $T/port.homed stands, as the controller refuses it before a home) and
# applied. $T/port.sender is a Grbl client's line: it closes an open
# envelope, as the controller does. The head starts at X10 Y10.
python3 - "$T/run/grbl.ctl" "$T/port.log" "$T/port" <<'EOF' &
import json, os, socket, sys, threading
path, log, ctl = sys.argv[1], sys.argv[2], sys.argv[3]
st = {'x': 10.0, 'y': 10.0, 'open': False}
mu = threading.Lock()
def answer(words):
    if os.path.exists(ctl + '.sender'):
        os.unlink(ctl + '.sender')
        st['open'] = False
    homed = 3 if os.path.exists(ctl + '.homed') else 0
    if words[:1] == ['state']:
        return json.dumps({'state': 'Idle', 'sender': False, 'port_jog': False, 'released': False,
                           'mpos': [round(st['x'], 3), round(st['y'], 3), 0.0], 'homed': homed,
                           'envelope_open': st['open'], 'mcode': None, 'sender_out': False,
                           'tray': 'in'})
    if words[:2] == ['envelope', 'open']:
        if not homed:
            return 'error:homed'
        st['open'] = True
    elif words[:2] == ['envelope', 'apply']:
        st['open'] = False
    elif words[:1] == ['jog']:
        for w in words[1:]:
            if w[:1] in ('X', 'Y'):
                st[w[0].lower()] += float(w[1:])
    return 'ok'
def serve(c):
    for line in c.makefile('rb'):
        with open(log, 'ab') as o:
            o.write(line)
        with mu:
            reply = answer(line.decode().split())
        c.sendall(reply.encode() + b'\n')
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(path)
srv.listen(4)
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

# The checks, as the page drives them: check <id> [what...] starts the
# check, polls GET /wiz/dark, and writes to $T/run.json the start's
# status, every phase and prompt seen, and the status it ended on. Each
# what is answer:<prompt>=<value> (one answer per prompt shown, in
# order), on:<prompt>=<command> (run when the prompt shows, before its
# answer), shot:<prompt>=<cam> (GET /wiz/shot?cam=<cam> when it shows,
# saved as $T/run.json.<cam>), or diag (GET /diag/status's phases beside
# the check's).
cat > "$T/drive.py" <<'EOF'
import json, subprocess, sys, time, urllib.error, urllib.parse, urllib.request
url, token, wid, out = sys.argv[1:5]
answers, actions, shots, diag = {}, {}, {}, False
for a in sys.argv[5:]:
    kind, _, rest = a.partition(':')
    pid, _, v = rest.partition('=')
    if kind == 'answer':
        answers.setdefault(pid, []).append(v)
    elif kind == 'on':
        actions[pid] = v
    elif kind == 'shot':
        shots[pid] = v
    elif kind == 'diag':
        diag = True
def call(method, path, data=None):
    body = urllib.parse.urlencode(data).encode() if data is not None else None
    req = urllib.request.Request(url + path, data=body, method=method, headers={'X-ForgeFIRM-Token': token})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
def seen(log, v):
    if v and (not log or log[-1] != v):
        log.append(v)
rec = {'phases': [], 'prompts': [], 'diag_phases': [], 'shots': {}}
rec['start'], body = call('POST', '/wiz/%s/start' % wid, {})
done, t0 = set(), time.time()
while rec['start'] == 200:
    d = json.loads(call('GET', '/wiz/dark')[1])
    seen(rec['phases'], d.get('phase'))
    if diag:
        seen(rec['diag_phases'], json.loads(call('GET', '/diag/status')[1]).get('phase'))
    p = d.get('prompt')
    if d.get('running') and p and p['seq'] not in done:
        done.add(p['seq'])
        pid = p['id']
        rec['prompts'].append({'id': pid, 'kind': p['kind'], 'options': p['options']})
        if pid in shots:
            st, img = call('GET', '/wiz/shot?cam=' + shots[pid])
            rec['shots'][shots[pid]] = st
            open('%s.%s' % (out, shots[pid]), 'wb').write(img)
        if pid in actions:
            subprocess.run(actions[pid], shell=True)
        if answers.get(pid):
            call('POST', '/wiz/%s/answer' % wid, {'seq': str(p['seq']), 'value': answers[pid].pop(0)})
    if not d.get('running'):
        rec['dark'] = d
        break
    if time.time() - t0 > 240:
        call('POST', '/wiz/%s/abort' % wid, {})
        rec['dark'] = dict(d, timed_out=True)
        break
    time.sleep(0.2)
json.dump(rec, open(out, 'w'))
EOF
# the previous record goes first: a driver that fails leaves no record,
# and every expectation of its check then fails on nothing
check() { c=$1; shift; rm -f "$T/run.json" "$T"/run.json.*; python3 "$T/drive.py" "$URL" "$TOKEN" "$c" "$T/run.json" "$@" 2>> "$T/out" || bad "the driver failed on $c"; }
# run <python expression over d, the driver's record>
run() { jget "$T/run.json" "$1"; }
# the record's version of a step, or nothing
recv() { jget "$REC" "str(d.get('wizards', {}).get('$1', {}).get('version', ''))"; }

echo "== the sensors check"
cp "$CONF" "$T/conf.before"
echo 50 > "$SYS/pic/hv_current"                  # a tube current at rest: the judge fails
check sensors
expect '200|True|True|0' "$(run '"%s|%s|%s|%d" % (d["start"], bool(d["dark"]["error"]), d["dark"]["result"] is None, len(d["prompts"]))')" \
    "a failing judge ends the check with an error, no result, and no question"
expect '' "$(recv sensors)" "and the record carries no sensors"
at_rest
check sensors
expect '200||0' "$(run '"%s|%s|%d" % (d["start"], d["dark"]["error"], len(d["prompts"]))')" "at rest: no error, no question"
expect 'True|1' "$(run '"%s|%s" % (all(k in d["dark"]["result"] for k in ("coolant_down_c", "coolant_up_c", "lid_ir_max", "laser_pgood", "hv_current_max", "exhaust_rpm_idle", "intake_rpm_idle")), d["dark"]["result"]["laser_pgood"])')" \
    "the result carries the readings, power-good 1"
expect 1 "$(recv sensors)" "the record carries sensors at version 1"
cmp -s "$T/conf.before" "$CONF" && ok "the settings file is byte for byte the same" || bad "the sensors check wrote a setting"

echo "== the intake fans step"
check airflow.intakes
expect '200||tachs|2|0' "$(run '"%s|%s|%s|%s|%d" % (d["start"], d["dark"]["error"], d["dark"]["result"]["counted_by"], d["dark"]["result"]["intake_fans"], len(d["prompts"]))')" \
    "both intakes turning: counted 2 by the tachs, no question"
grep -qx 'cool_intake_fans = 2' "$CONF" && ok "the setting reads 2" || bad "the conf file has $(grep cool_intake_fans "$CONF")"
expect 1 "$(recv airflow.intakes)" "the record carries airflow.intakes at version 1"
# the step hands the machine back: the controller up, the cooling engine idle
settled() {
    mode
    [ "$(jget "$T/mode.json" 'd["controller"]')" = running ] && curl -s -o "$T/cool.json" "$URL/cool/status" \
        && [ "$(jget "$T/cool.json" 'd.get("phase", "")')" = idle ]
}
until_ok 100 settled || bad "the machine did not settle after the step: $(cat "$T/mode.json") $(cat "$T/cool.json")"
echo 0 > "$SYS/thermal/tach_intake_2"            # intake 2 silent: the tachs cannot count
check airflow.intakes answer:intake-count=One
expect 'intake-count|choice|One,Two' "$(run '"|".join([d["prompts"][0]["id"], d["prompts"][0]["kind"], ",".join(d["prompts"][0]["options"])])')" \
    "intake 2 silent: the step asks for the count$(run '"" if d["prompts"] else " (%s %s)" % (d["start"], d.get("dark", {}).get("error"))')"
expect '|operator|1' "$(run '"%s|%s|%s" % (d["dark"]["error"], d["dark"]["result"]["counted_by"], d["dark"]["result"]["intake_fans"])')" \
    "the answer One is the count"
grep -qx 'cool_intake_fans = 1' "$CONF" && ok "the setting reads 1" || bad "the conf file has $(grep cool_intake_fans "$CONF")"
at_rest
up || bad "no controller after the intake fans step"

echo "== the intake-count prompt with the wall clock an hour behind"
# The prompt's wait is a timeout, and a timeout rides CLOCK_MONOTONIC:
# with the daemon's reading of CLOCK_REALTIME an hour behind (the clock
# stand-in, while the flag file exists) the prompt stays open and takes
# its answer. A wait on the wall clock ends the moment the prompt
# opens, its deadline already past. Intake 2 silent makes the step ask.
dark() { curl -s -o "$T/dark.json" "$URL/wiz/dark"; }
stopped() { dark && [ "$(jget "$T/dark.json" 'd.get("running")')" = false ]; }
asking() { dark && [ "$(jget "$T/dark.json" '(d.get("prompt") or {}).get("id", "")')" = intake-count ]; }
# the step before has handed the machine back: the controller up, the
# cooling engine out of its cooldown
until_ok 150 settled || bad "the machine did not settle after the step: $(cat "$T/mode.json") $(cat "$T/cool.json")"
echo 0 > "$SYS/thermal/tach_intake_2"
: > "$T/timeshift"
expect 200 "$(post /wiz/airflow.intakes/start "$TOKEN")" "POST /wiz/airflow.intakes/start is 200"
n=200
while ! asking && ! stopped; do n=$((n - 1)); [ $n -gt 0 ] || break; sleep 0.2; done
if asking; then
    ok "the prompt is open under the shift"
    sleep 3
    asking && ok "and still open 3 s later" || bad "the prompt closed within 3 s: $(cat "$T/dark.json")"
    rm -f "$T/timeshift"
    SEQ=$(jget "$T/dark.json" 'd["prompt"]["seq"]')
    expect 200 "$(post /wiz/airflow.intakes/answer "$TOKEN" -d "seq=$SEQ" -d value=One)" "the answer One is 200"
    until_ok 100 stopped && ok "the step ended" || bad "the step did not end: $(cat "$T/dark.json")"
    expect '|operator|1' "$(jget "$T/dark.json" '"%s|%s|%s" % (d["error"], (d["result"] or {}).get("counted_by"), (d["result"] or {}).get("intake_fans"))')" \
        "the answer is the count"
else
    rm -f "$T/timeshift"
    bad "the prompt never stayed open: running $(jget "$T/dark.json" 'd.get("running")'), error '$(jget "$T/dark.json" 'd.get("error")')', result $(jget "$T/dark.json" 'd.get("result")'), log $(jget "$T/dark.json" 'd.get("log")')"
fi
at_rest
up || bad "no controller after the shifted prompt"

echo "== the cameras check"
python3 -c 'import sys
for cam in ("lid", "head"):
    open("%s/%s.jpg" % (sys.argv[1], cam), "wb").write(bytes([255, 216]) + cam.encode() * 8 + bytes([255, 217]))' "$T/shots"
curl -s -o "$T/cam.json" "$URL/cam/status"
SENSOR=$(jget "$T/cam.json" 'd["sensor"]')
sw 0                                             # the lid open
check cameras "on:lid-close=$T/bin/sw 8" shot:lid-view=lid shot:head-view=head answer:lid-view=Yes answer:head-view=Yes
expect 'lid-close|lid-view|head-view' "$(run '"|".join(p["id"] for p in d["prompts"])')" \
    "the lid is waited for, then the lid shot's question, then the head shot's"
expect '200|200' "$(run '"%s|%s" % (d["shots"].get("lid"), d["shots"].get("head"))')" "GET /wiz/shot serves each shot at its question"
cmp -s "$T/run.json.lid" "$T/shots/lid.jpg" && cmp -s "$T/run.json.head" "$T/shots/head.jpg" \
    && ok "each shot is the capture" || bad "a shot served is not the capture"
expect "|True|True|$SENSOR" "$(run '"%s|%s|%s|%s" % (d["dark"]["error"], d["dark"]["result"]["lid_ok"], d["dark"]["result"]["head_ok"], d["dark"]["result"]["sensor"])')" \
    "both views accepted, and the result names the sensor GET /cam/status gives"
expect 1 "$(recv cameras)" "the record carries cameras at version 1"
sw 8

echo "== the bed check"
cp "$CONF" "$T/conf.before"
: > "$T/port.log"
check motion.envelope
expect '200|True|0' "$(run '"%s|%s|%d" % (d["start"], bool(d["dark"]["error"]), len(d["prompts"]))')" \
    "on an unhomed machine the check ends in an error, with no prompt"
grep -q '^envelope' "$T/port.log" && bad "the port got: $(grep '^envelope' "$T/port.log")" || ok "the envelope was never opened"
cmp -s "$T/conf.before" "$CONF" && ok "the settings file is byte for byte the same" || bad "an unhomed check wrote a setting"
touch "$T/port.homed"                            # homed: the head at X10 Y10
X10=answer:x-end=X+10
Y10=answer:y-end=Y+10
check motion.envelope $X10 $X10 $X10 $X10 $X10 answer:x-end=X+1 answer:x-end=X+0.1 "answer:x-end=This is the end" \
    $Y10 $Y10 $Y10 $Y10 $Y10 answer:y-end=Y+1 "answer:y-end=This is the end"
expect '|15|True' "$(run '"%s|%d|%s" % (d["dark"]["error"], len(d["prompts"]), all(p["kind"] == "jog" and "This is the end" in p["options"] for p in d["prompts"]))')" \
    "fifteen prompts, every one a jog that offers This is the end"
expect '61.1|61.0' "$(run '"%.1f|%.1f" % (d["dark"]["result"]["x_end_mm"], d["dark"]["result"]["y_end_mm"])')" \
    "the ends are the home plus the jogs: X 61.1, Y 61.0"
grep -qx 'envelope_x_mm = 60.10' "$CONF" && grep -qx 'envelope_y_mm = 60.00' "$CONF" \
    && ok "the keys are the ends less 1 mm" || bad "the conf file has $(grep envelope_ "$CONF" | tr '\n' ' ')"
expect 'envelope open|envelope apply' "$(grep '^envelope' "$T/port.log" | tr '\n' '|' | sed 's/|$//')" \
    "the envelope was opened for the jogs and applied"
expect 1 "$(recv motion.envelope)" "the record carries motion.envelope at version 1"
cp "$CONF" "$T/conf.mid"
check motion.envelope "on:x-end=touch $T/port.sender" answer:x-end=X+1
expect '200|True|1' "$(run '"%s|%s|%d" % (d["start"], bool(d["dark"]["error"]), len(d["prompts"]))')" \
    "a Grbl client's line closing the envelope ends the check at the answer, in an error"
cmp -s "$T/conf.mid" "$CONF" && ok "and nothing is written" || bad "the closed check wrote a setting"
rm -f "$T/port.homed"
expect 200 "$(post '/settings?envelope_x_mm=&envelope_y_mm=' "$TOKEN")" "clearing the keys is 200"
cmp -s "$T/conf.before" "$CONF" && ok "the settings file is as it was" || bad "the keys were not cleared"

echo "== the flow check"
cp "$CONF" "$T/conf.before"
expect 200 "$(post /settings "$TOKEN" -d cool_flow_check_s=5 -d cool_flow_rise=6.0)" \
    "a 5 s check window and a 6.0 C threshold are 200"
# The loop as the diagnostic reads it: at 20 C at rest; with the heater
# on the downstream sensor 5.0 C up with the pump on and 9.0 C up with it
# off, so the threshold passes with a thin margin on the flowing side.
# The heater's file is emptied as it is read: a shorter value written
# over a longer one would keep the longer one's tail.
python3 - "$SYS" <<'EOF' &
import math, os, sys, time
sysfs = sys.argv[1]
def raw(t):
    r = 10000 * math.exp(-3380 / 298.15) * math.exp(3380 / (t + 273.15))
    return round(1024 * 1.3 / (10000 / r + 1))
def put(attr, v):
    open(os.path.join(sysfs, attr + '.new'), 'w').write('%d\n' % v)
    os.replace(os.path.join(sysfs, attr + '.new'), os.path.join(sysfs, attr))
heater = 0
while True:
    with open(os.path.join(sysfs, 'thermal/heater_pwm'), 'r+') as f:
        v = f.read().strip()
        if v:
            heater = int(v)
            f.truncate(0)
    pump = open(os.path.join(sysfs, 'thermal/water_pump_on')).read().strip() != '0'
    put('pic/water_temp_1', raw(20.0 + ((5.0 if pump else 9.0) if heater else 0.0)))
    put('pic/water_temp_2', raw(20.0))
    time.sleep(0.05)
EOF
PLANT=$!
check cooling.flow-verify diag
kill $PLANT
PLANT=
expect '200||True|True' "$(run '"%s|%s|%s|%s" % (d["start"], d["dark"]["error"], d["dark"]["result"]["pass"], d["dark"]["result"]["thin_margin"])')" \
    "the diagnostic's result is the check's: pass true, a thin margin"
expect True "$(run 'str(len(set(d["phases"]) & set(d["diag_phases"])) >= 4)')" \
    "the diagnostic's phases show as the check's ($(run 'str(len(set(d["phases"]) & set(d["diag_phases"])))') shared)"
expect '1|True' "$(jget "$REC" '"%s|%s" % (d["wizards"]["cooling.flow-verify"]["version"], "cooling.flow" in d.get("flags", {}))')" \
    "the record carries cooling.flow-verify at version 1, and the thin margin flags the calibration"
at_rest
expect 200 "$(post '/settings?cool_flow_check_s=&cool_flow_rise=' "$TOKEN")" "clearing the window and the threshold is 200"
cmp -s "$T/conf.before" "$CONF" && ok "the settings file is as it was" || bad "the flow settings were not cleared"
restart "$T/seed.json"
up || bad "no controller on the seeded record"

echo "== the flow calibration aborted at its first rung"
# The duty is written beside its threshold or not at all: the rung's
# duty is in force while the diagnostic runs under the check, and an
# abort puts back what was there.
calibrating() { curl -s -H "X-ForgeFIRM-Token: $TOKEN" -o "$T/diag.json" "$URL/diag/status" \
    && [ "$(jget "$T/diag.json" '"%s|%s" % (d.get("running"), d.get("tool"))')" = "True|flow-calibrate" ]; }
holder() { curl -s -o "$T/status.json" "$URL/status"; jget "$T/status.json" '(d["lease"]["holder"] or {}).get("owner") or "none"'; }
free() { [ "$(holder)" = none ]; }
start_flow() { [ "$(post /wiz/cooling.flow/start "$TOKEN")" = 200 ]; }
# abort_at_rung1 <label>: started, its diagnostic running, the duty 40 in force, aborted, ended, the machine handed back
abort_at_rung1() {
    until_ok 50 start_flow && ok "$1: POST /wiz/cooling.flow/start is 200" || bad "$1: the start was refused: $(cat "$T/body")"
    until_ok 150 calibrating && ok "$1: the flow-calibrate diagnostic runs under the check" || bad "$1: no diagnostic: $(cat "$T/diag.json")"
    grep -qx 'cool_flow_heater_pct = 40' "$CONF" && ok "$1: the rung's duty 40 is in force" || bad "$1: the conf file has '$(grep cool_flow_heater_pct "$CONF")'"
    expect 200 "$(post /wiz/cooling.flow/abort "$TOKEN")" "$1: POST /wiz/cooling.flow/abort is 200"
    until_ok 300 stopped && ok "$1: the check stopped" || bad "$1: the check still runs: $(cat "$T/dark.json")"
    expect 'True|True' "$(jget "$T/dark.json" '"%s|%s" % (bool(d["error"]), d["result"] is None)')" "$1: it ended in an error with no result"
    until_ok 150 free || bad "$1: the lease is still held: $(holder)"
    up || bad "$1: no controller after the abort"
}
cp "$CONF" "$T/conf.before"
grep -q '^cool_flow_heater_pct' "$CONF" && bad "the conf file starts with a heater duty" || ok "no heater duty in the conf file to start"
abort_at_rung1 unset
grep -q '^cool_flow_heater_pct' "$CONF" && bad "unset: the conf file keeps '$(grep cool_flow_heater_pct "$CONF")' after the abort" \
    || ok "unset: no heater duty in the conf file after the abort"
cmp -s "$T/conf.before" "$CONF" && ok "unset: the settings file is byte for byte as it was" || bad "unset: the settings file changed"
expect 200 "$(post /settings "$TOKEN" -d cool_flow_heater_pct=45)" "a heater duty of 45 is 200"
cp "$CONF" "$T/conf.seeded"
abort_at_rung1 seeded
grep -qx 'cool_flow_heater_pct = 45' "$CONF" && ok "seeded: the duty 45 is back after the abort" || bad "seeded: the conf file has '$(grep cool_flow_heater_pct "$CONF")'"
cmp -s "$T/conf.seeded" "$CONF" && ok "seeded: the settings file is byte for byte as seeded" || bad "seeded: the settings file changed"
expect 200 "$(post '/settings?cool_flow_heater_pct=' "$TOKEN")" "clearing the duty is 200"
cmp -s "$T/conf.before" "$CONF" && ok "the settings file is as it was" || bad "the duty was not cleared"

echo "== the laser floor burn"
cp "$CONF" "$T/conf.before"
: > "$T/burn.log"
check laser.floor
expect '200|True|0' "$(run '"%s|%s|%d" % (d["start"], bool(d["dark"]["error"]), len(d["prompts"]))')" \
    "without Focus on the record the burn is refused before any prompt"
cmp -s "$T/conf.before" "$CONF" && ok "and nothing is written" || bad "the refused burn wrote a setting"
# The burn on a host: Focus on the record; the head's hall sensor
# following the lens steps written to cnc/z_step (a pipe); the GRBL
# controller's stand-in publishing its settings, reporting to the
# cooling engine with its secret, and answering the Grbl socket: ok to
# every line, each recorded; at the program's first M3 or M4 it reports
# its window armed and sets the tube current and the LASER_ON samples
# the burn's witnesses read, at M102 it reads the settings in force, and
# at M2 it puts both witnesses back to zero. Nothing fires: no laser is
# there, and the witnesses are files.
python3 - "$T/seed.json" "$T/focus.json" <<'EOF' || fatal "the Focus record was not written"
import json, sys
r = json.load(open(sys.argv[1]))
r['wizards']['laser.focus'] = {'version': 1, 'completed': r['created'], 'result': {}, 'applied': {}}
json.dump(r, open(sys.argv[2], 'w'), indent=2)
EOF
cat > "$T/burnctl.py" <<'EOF'
import json, os, socket, sys, threading, time, urllib.parse, urllib.request
T, URL, SYS, PORT = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
SECRET, RUN = os.environ.get('GF_REPORT_SECRET', ''), os.environ['GF_RUN_DIR']
def put(path, text):
    open(path + '.new', 'w').write(text)
    os.replace(path + '.new', path)
open(T + '/ctl.pids', 'a').write('%d\n' % os.getpid())
put(T + '/ctl.pid', '%d\n' % os.getpid())
put(T + '/ctl.secret', SECRET + '\n')
put(RUN + '/grbl.settings', '$100=213.333\n$101=213.333\n$102=2.922\n')
armed = [0]
def report():
    data = urllib.parse.urlencode({'mode': 'idle', 'armed': str(armed[0])}).encode()
    try:
        urllib.request.urlopen(urllib.request.Request(URL + '/cool/state', data=data,
                                                      headers={'X-ForgeFIRM-Report': SECRET}), timeout=5).read()
    except OSError:
        pass
def reporter():
    while True:
        put(RUN + '/grbl.state', json.dumps({'connected': False, 'ts_mono': time.monotonic()},
                                          separators=(',', ':')))
        report()
        time.sleep(0.5)
def serve(c):
    c.sendall(b"Grbl 1.1h ['$' for help]\r\n")
    lit = False
    for raw in c.makefile('rb'):
        line = raw.decode().strip()
        open(T + '/burn.log', 'a').write(line + '\n')
        if line == 'M102':
            put(T + '/at_m102.json', urllib.request.urlopen(URL + '/settings', timeout=5).read().decode())
        if not lit and line[:2] in ('M3', 'M4'):
            lit = True
            armed[0] = 1
            report()
            put(SYS + '/pic/hv_current', '60\n')
            put(SYS + '/cnc/laser_on_sampled', '12\n')
        if line == 'M2':
            time.sleep(0.3)
            put(SYS + '/pic/hv_current', '0\n')
            put(SYS + '/cnc/laser_on_sampled', '0\n')
            armed[0] = 0
        c.sendall(b'ok\r\n')
def listen(fam, host):
    s = socket.socket(fam, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((host, PORT))
    s.listen(4)
    while True:
        threading.Thread(target=serve, args=(s.accept()[0],), daemon=True).start()
threading.Thread(target=reporter, daemon=True).start()
threading.Thread(target=listen, args=(socket.AF_INET6, '::1'), daemon=True).start()
listen(socket.AF_INET, '127.0.0.1')
EOF
cat > "$T/hall.py" <<'EOF'
import os, sys
sysfs = sys.argv[1]
fd = os.open(os.path.join(sysfs, 'cnc/z_step'), os.O_RDWR)
k = 0
def hall():
    p = os.path.join(sysfs, 'head/hall_sensor')
    open(p + '.new', 'w').write('0\n' if k >= 0 else '1\n')
    os.replace(p + '.new', p)
hall()
while True:
    for b in os.read(fd, 64):
        k += 1 if b == ord('1') else -1
    hall()
EOF
OLD=$(ctl_pid)
stop_daemon
cp "$T/bin/grbl" "$T/bin/grbl.sh"
printf '#!/bin/sh\nexec python3 "%s/burnctl.py" "%s" "%s" "%s" %s\n' "$T" "$T" "$URL" "$SYS" "$GRBL_PORT" > "$T/bin/grbl"
for a in head/z_current head/z_mode head/z_enable cnc/motor_lock; do : > "$SYS/$a"; done
echo 0 > "$SYS/cnc/laser_on_sampled"
mkfifo "$SYS/cnc/z_step"
python3 "$T/hall.py" "$SYS" &
HALL=$!
until_ok 25 test -s "$SYS/head/hall_sensor" || fatal "the hall sensor's stand-in did not start"
LENS_SYS=$SYS
cp "$T/focus.json" "$REC"
start_daemon
up || bad "no controller on the Focus record"
expect 200 "$(post /settings "$TOKEN" -d laser_floor_density=4)" "a floor of 4 to start from is 200"
curl -s -o "$T/status.json" "$URL/status"
IN=$(jget "$T/status.json" '"%.1f" % ((d["lens"]["reach_min"] + d["lens"]["reach_max"]) / 2)')
OUT=$(jget "$T/status.json" '"%.1f" % (d["lens"]["reach_max"] + 2)')
curl -s -o "$T/floor.gcode" "$URL/wiz/sheet.gcode?card=laser.floor"
grep -qx ';keys=floor-off' "$T/floor.gcode" && ok "the program carries ;keys=floor-off" || bad "the program has no ;keys=floor-off"
# snap <prompt>: the machine as the prompt shows, one JSON line in $T/snaps
# (the controller's pid, the lens's reference marker, the lens motor's
# attributes, the lines on the Grbl socket, the floor setting, the
# wizard's phase, and the cooling engine's armed)
cat > "$T/bin/snap" <<EOF
#!/bin/sh
exec python3 - "\$1" <<'PY'
import json, os, sys, urllib.request
def rd(p):
    try:
        return open(p).read().strip()
    except OSError:
        return None
def get(path):
    return json.loads(urllib.request.urlopen('$URL' + path, timeout=5).read())
conf = rd('$CONF') or ''
floor = [l.split('=', 1)[1].strip() for l in conf.splitlines() if l.startswith('laser_floor_density ')]
print(json.dumps({'at': sys.argv[1], 'pid': rd('$T/ctl.pid'), 'lens_home': os.path.exists('$T/run/lens.home'),
                  'motor_lock': rd('$SYS/cnc/motor_lock'), 'z_mode': rd('$SYS/head/z_mode'),
                  'z_enable': rd('$SYS/head/z_enable'), 'socket_lines': len((rd('$T/burn.log') or '').split()),
                  'floor': floor[0] if floor else None, 'phase': get('/wiz/dark').get('phase'),
                  'armed': get('/cool/status').get('armed')}), file=open('$T/snaps', 'a'))
PY
EOF
chmod +x "$T/bin/snap"
: > "$T/snaps"
P0=$(ctl_pid)
rm -f "$T/run/lens.home"
check laser.floor answer:gantry-stops=Yes "on:home=$T/bin/snap home" "answer:home=The head is home" \
    "on:scrap=$T/bin/snap scrap" answer:scrap=Continue "on:thickness=$T/bin/snap thickness" \
    answer:thickness=$OUT answer:thickness=$IN "on:floor-arm=wc -l < $T/burn.log > $T/at_arm" answer:floor-arm=Continue \
    "on:floor-pick=curl -s -o $T/at_pick.json $URL/settings" answer:floor-pick=8
expect 'gantry-stops|home|scrap|thickness|thickness|floor-arm|floor-pick' "$(run '"|".join(p["id"] for p in d["prompts"])')" \
    "the prompts in order: gantry stops, home, scrap, thickness, thickness again, the arm, the pick"
# snaps <python expression over s, the snapshot at prompt $1 (the n-th: $2)>
snaps() { python3 -c 'import json, sys
s = [json.loads(l) for l in open(sys.argv[1]) if json.loads(l)["at"] == sys.argv[2]][int(sys.argv[3])]
print(eval(sys.argv[4]))' "$T/snaps" "$1" "$2" "$3" 2>&1; }
expect True "$(snaps home 0 's["pid"] not in (None, "'"$P0"'") and s["lens_home"]')" \
    "after the gantry stops, before the home question: the controller stopped and started again, the lens referenced"
expect '|1|0' "$(snaps scrap 0 '"%s|%s|%s" % (s["motor_lock"], s["z_mode"], s["z_enable"])')" \
    "before the scrap question: the lens motor rests after its reference, the lock never written"
for n in 0 1; do
    expect '0|4|False|True' "$(snaps thickness $n '"%s|%s|%s|%s" % (s["socket_lines"], s["floor"], s["armed"], s["phase"] != "the program goes to the controller, the button lights at the arm")')" \
        "at thickness question $((n + 1)): no program line streamed, laser_floor_density still 4, not armed, the burn not entered"
done
expect "$IN" "$(run '"%.1f" % d["dark"]["result"]["thickness_mm"]')" \
    "$OUT mm, outside the lens window, was not stored: the burn ran at $IN mm"
expect 0 "$(cat "$T/at_arm" 2>/dev/null)" "nothing went to the controller before the arm"
expect '1|0' "$(grep -cx M102 "$T/burn.log")|$(grep -c '^;' "$T/burn.log")" \
    "the program reaches the controller with ;keys=floor-off as M102, and no other directive"
expect '0|off' "$(jget "$T/at_m102.json" 'd.get("laser_floor_density", "") + "|" + d.get("laser_dose_curve", "")')" \
    "at M102 the floor is 0 and the dose curve off"
expect '4|' "$(jget "$T/at_pick.json" 'd.get("laser_floor_density", "") + "|" + d.get("laser_dose_curve", "")')" \
    "after the burn the keys are as they were: the floor 4, the curve unset"
expect '|8|10' "$(run '"%s|%g|%g" % (d["dark"]["error"], d["dark"]["result"]["faintest_density"], d["dark"]["result"]["floor_density"])')" \
    "the pick 8 sets the floor 10"
grep -qx 'laser_floor_density = 10' "$CONF" && ok "the setting reads 10" || bad "the conf file has $(grep laser_floor "$CONF")"
expect 1 "$(recv laser.floor)" "the record carries laser.floor at version 1"
[ ! -e "$T/data/curverec.saved" ] && ok "the saved keys' marker is gone" || bad "the saved keys' marker is left"
[ ! -s "$SYS/cnc/motor_lock" ] && ok "the session never wrote the lens lock: it is the controller's" \
    || bad "the session wrote cnc/motor_lock $(cat "$SYS/cnc/motor_lock")"
OLD=$(ctl_pid)
stop_daemon
kill $HALL
wait $HALL 2>/dev/null
HALL=
rm -f "$SYS/cnc/z_step" "$SYS/head/hall_sensor" "$SYS/head/z_current" "$SYS/head/z_mode" "$SYS/head/z_enable" \
      "$SYS/cnc/motor_lock" "$SYS/cnc/laser_on_sampled" "$T/run/grbl.settings" "$T/run/grbl.state" "$T/run/lens.home"
mv "$T/bin/grbl.sh" "$T/bin/grbl"
LENS_SYS=$T/no-head
at_rest
cp "$T/conf.before" "$CONF"
cp "$T/seed.json" "$REC"
start_daemon
up || bad "no controller on the seeded record"

echo "== the first run"
# The record as a first run leaves it behind the checks: no consent, no
# press, no account, no form step; the coolant calibration kept; no
# users file, and the intake fans not counted.
python3 - "$T/seed.json" "$T/first.json" <<'EOF' || fatal "the first-run record was not written"
import json, sys
r = json.load(open(sys.argv[1]))
r['advisories'] = {}
for k in ('acceptance', 'account'):
    r.pop(k, None)
r['wizards'] = {k: v for k, v in r['wizards'].items()
                if k not in ('advisories', 'account', 'preferences', 'machine', 'cloud')}
json.dump(r, open(sys.argv[2], 'w'), indent=2)
EOF
cp "$CONF" "$T/conf.before"
OLD=$(ctl_pid)
stop_daemon
cp "$T/first.json" "$REC"
: > "$CONF"
start_daemon
wiz
expect 'false|false|closed' "$(jget "$T/wiz.json" 'json.dumps(d["advisories_complete"]) + "|" + json.dumps(d["acceptance_done"]) + "|" + d["gate"]')" \
    "GET /wiz: nothing accepted, the gate closed"
for doc in $(jget "$T/wiz.json" '" ".join(x["id"] for x in d["documents"])'); do
    H=$(jget "$T/wiz.json" "[x['hash'] for x in d['documents'] if x['id'] == '$doc'][0]")
    P=$(jget "$T/wiz.json" "[x.get('phrase') or '' for x in d['documents'] if x['id'] == '$doc'][0]")
    expect 200 "$(post /wiz/advisories/accept "$TOKEN" -d "doc=$doc" -d "hash=$H" --data-urlencode "phrase=$P")" \
        "$doc accepted at its hash"
done
wiz
expect true "$(jget "$T/wiz.json" 'json.dumps(d["advisories_complete"])')" "the documents are accepted"
expect 200 "$(post /wiz/advisories/press "$TOKEN")" "POST /wiz/advisories/press is 200"
led() { cat "$T/leds/button_led_$1/$2" 2>/dev/null; }
if [ "$(led 1 target)" = 0 ] && [ "$(led 2 target)" -gt 0 ] && [ "$(led 3 target)" -gt 0 ] \
   && [ "$(led 2 pulse_on)" -gt 0 ] && [ "$(led 3 pulse_on)" -gt 0 ]; then
    ok "the button breathes teal: red off, green and blue lit and pulsing"
else bad "the button LEDs: targets $(led 1 target)/$(led 2 target)/$(led 3 target), pulse_on $(led 2 pulse_on)/$(led 3 pulse_on)"; fi
# a new reason: the supervisor takes it up on its next tick, then holds the gate on it
says() { mode; [ "$(jget "$T/mode.json" 'd.get("why", "")')" = "$1" ]; }
held_next() { until_ok 50 says "$1"; held "$1"; }
press() { curl -s -o "$T/press.json" "$URL/wiz/advisories/press" && [ "$(jget "$T/press.json" 'json.dumps(d["accepted"])')" = true ]; }
sw 12                                            # the button held
until_ok 50 press && ok "the press is taken as the acceptance" || bad "the press was not accepted: $(cat "$T/press.json")"
sw 8
wiz
expect 'true|True' "$(jget "$T/wiz.json" 'json.dumps(d["acceptance_done"])')|$(jget "$REC" 'str(bool(d["acceptance"]["pressed_at"]))')" \
    "acceptance_done, and the record holds the press"
code=$(curl -s -D "$T/head" -o "$T/body" -w '%{http_code}' -X POST -H "X-ForgeFIRM-Token: $TOKEN" \
            -d name=owner -d "password=$(python3 -c 'import secrets; print(secrets.token_hex(8))')" "$URL/wiz/account")
expect 200 "$code" "POST /wiz/account is 200"
tr -d '\r' < "$T/head" | grep -qi '^set-cookie: ffsid=[0-9a-f]' && ok "and sets the session cookie" || bad "no session cookie: $(grep -i cookie "$T/head")"
held_next "setup required: preferences, machine, cloud"
expect 200 "$(post /wiz/preferences "$TOKEN" -d ui_units=metric)" "POST /wiz/preferences is 200"
held_next "setup required: machine, cloud"
expect 200 "$(post /wiz/machine "$TOKEN" -d model=basic -d tec=0)" "POST /wiz/machine is 200"
held_next "setup required: cloud"
expect 200 "$(post /wiz/cloud "$TOKEN" -d enabled=0)" "POST /wiz/cloud is 200"
held_next "the intake fans are not counted"
OLD=$(ctl_pid)
expect 200 "$(post /settings "$TOKEN" -d cool_intake_fans=1)" "cool_intake_fans=1 is 200"
wiz
expect 'open|' "$(jget "$T/wiz.json" 'd["gate"] + "|" + d["why"]')" "the count opens the gate"
up && ok "the controller starts" || bad "no controller with the gate open"
expect 200 "$(post /wiz/complete "$TOKEN")" "POST /wiz/complete is 200"
wiz
expect 'true|open' "$(jget "$T/wiz.json" 'json.dumps(d["completed"]) + "|" + d["gate"]')" "the run is complete, the gate open"
OLD=$(ctl_pid)
stop_daemon
cp "$T/seed.json" "$REC"
cp "$T/conf.before" "$CONF"
rm -f "$T/data/users"
start_daemon
up || bad "no controller on the seeded record"

stop_daemon
if [ $fails -eq 0 ]; then
    echo "PASS: host_setup"
else
    echo "FAIL: host_setup, $fails checks"
    echo "--- the daemon's output"
    tail -60 "$T/out"
    exit 1
fi
