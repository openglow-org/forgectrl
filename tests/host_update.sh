#!/bin/sh
# host_update.sh - the update routes on a host: the release check, an update
# job's lease, the download, the apply, the boot selection, the factory
# return and the reboot, against a scratch slot table
# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# SPDX-License-Identifier: MIT
#
# Starts the built forgectrl the way host_guard.sh does (the same
# environment, sysfs tree, stand-ins, port listener, and seeded setup
# record), with FORGECTRL_RELCHECK_URL and FORGECTRL_RELCHECK_DOWNLOAD_FMT
# pointed at a release server of its own on 127.0.0.1:18099 (plain HTTP,
# which the test hook GF_RELEASE_HTTP lets curl take). That server answers
# the release whose tag $T/rel.tag holds, with notes and a forgefirm.fw
# asset of a size above 0; it serves $T/served.fw (a text stand-in, never a
# release asset) as that release's forgefirm.fw, and a few bytes that are
# no archive as acceptance.json, after a delay that keeps the job, and so
# its lease, running. The installed version is $T/installed
# (GF_VERSION_FILE), v0.0.10.
#
# The slot seam (GF_SLOT_DIR, testhook.h) is $T/slots: the slots' devices
# are files there (mmcblk2p1 the booted root, which `root` links to;
# mmcblk2p2 the other), their sizes under sys/, and ffboot is a stand-in
# that records its arguments and moves the next boot in the inventory file
# (GF_SLOT_LIST). On the daemon's PATH, fwup is a stand-in that reads an
# archive as text: -m prints its meta- lines, -V passes only for the key a
# signed-by line names, -l passes on an archive with a product and lists
# its task lines, -a writes its file lines into the slot's file; mount and
# umount stand in for the verification's read-only mount, making a slot's
# file lines into files. A reboot is written to $T/reboot (GF_REBOOT_FILE),
# whose removal is the restart. A route waits at a named point while
# $T/pause/<point> stands (GF_PAUSE_DIR).
# The machine's button is the switch word's bit 2, read through a
# preloaded stand-in for the switch device's ioctl, as host_routes.sh does,
# and the GRBL controller's state file is kept current by a writer of the
# test's own from the job state $T/gwant names.
#
# One case each:
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
#   the product gate on POST /update/upload, and an upload's minute of
#     silence (as before); a refused upload leaves the staged archive
#   the download: a release not newer than the installed one is not
#     fetched, unless downgrade=1; an archive signed with another key, one
#     that is not firmware, and one whose version is not its tag are each
#     discarded; the file stands under its own name while it arrives and
#     takes download.fw only once verified
#   the apply on the slot seam: the booted slot and a slot that cannot be
#     read refused; the product gate in the job; the archive the request
#     names (its SHA-256); an archive not newer than the installed one
#     needs downgrade=1 and the button held inside the reservation; a
#     download not signed with the release key refused even with the
#     button; the incomplete mark left by a failed write and a failed
#     verification (no version file, no kernel), refusing the boot
#     selection, and cleared by a write that verifies; the guards asked
#     again under the update lock; a release applied with the release key;
#     an archive signed with another key refused at the job's signature
#     gate without confirm_unsigned=1, and written with no key with it and
#     the button
#   the boot selection: force is no parameter; refused under the update
#     lock; the inventory answered from its last probe under the lock
#   the factory return: the button held inside the reservation; a job
#     that comes up at the point-back keeps the machine before any write;
#     the guards asked again under the update lock; a restore whose write
#     fails, or whose image does not verify, keeps the slot's mark; the
#     next boot pointed back at the running slot before a restore; a job
#     that comes up during the restore, or during the selection, keeps the
#     machine; a marked slot that reads factory restored, not selected; on
#     a card boot the factory slot, or the slot named; the reboot 5 s out,
#     which refuses a boot selection while it is pending
#   one predicate: the reboot, the boot selection, the factory return, an
#     update job's start, the reset of every setting and a downgrade's and
#     an unsigned install (before the button is read) each refused under
#     each job state the cases name (the kernel running, an armed window, a
#     print paused past its grace under either controller, fresh and its
#     report past 5 s under the cloud one, the cloud client's wait for the
#     press reported with the mode idle, fresh and past 5 s, a GRBL job
#     waiting for its press, running, paused, a state file gone stale, a
#     Grbl sender connected), and a hold dialog's reservation not ready
#     under a paused print and a reported wait; the cloud client's cooldown
#     and a stopped controller's last report are no job, and a report that
#     passed its check as its controller stopped stores nothing; the reboot
#     route and the reset hold the machine's lease from their check to
#     their schedule (an apply, and a downgrade on the reset's hold, landing
#     in between are refused), a reboot pending refuses an apply, and the
#     reset's a downgrade on the same hold, and a downgrade whose check
#     passed before the reset was scheduled is refused under its own hold;
#     the reset refused while an update job writes
#   the daemon's stop during a slot write: the write ended, the slot left
#     marked incomplete
#   HTTPS alone: with no GF_RELEASE_HTTP the release check refuses the
#     plain-HTTP server; that start removes a download.fw.part left behind
#
# Nothing posts /system/ssh. The daemon runs on the image's ulfius
# (tests/ulfius_image.sh). Needs a C compiler (cc). Runs unprivileged;
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
SLOTS=$T/slots
PID=
LPID=
# plain HTTP to the release server allowed (the test hook); empty: HTTPS alone
RELHTTP=1
RPID=
KPID=
cleanup() {
    if [ -n "$PID" ]; then kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; fi
    [ -z "$LPID" ] || kill $LPID 2>/dev/null
    [ -z "$RPID" ] || kill $RPID 2>/dev/null
    [ -z "$KPID" ] || kill $KPID 2>/dev/null
    [ ! -f "$T/ctl.pids" ] || kill $(cat "$T/ctl.pids") 2>/dev/null
    [ ! -f "$T/fwup.pid" ] || kill $(cat "$T/fwup.pid") 2>/dev/null
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
jv() { jget "$T/body" "$1" 2>/dev/null; }

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
        LD_PRELOAD="$T/wallshim.so" WALLSHIFT_FILE="$T/wallshift" \
        FORGECTRL_DATA_DIR="$T/data" FORGECTRL_CONF="$CONF" \
        FFLOG_CONF="$CONF" FFLOG_SOCK="$T/no-log" FFLOG_STDERR=1 \
        GF_RUN_DIR="$T/run" GF_SYSFS_ROOT="$SYS/" GF_SWITCH_DEV="$T/switches" \
        GF_SWITCH_WORD_FILE="$T/switches" GF_PULSE_DEV="$T/pulse" \
        GF_SLOT_DIR="$SLOTS" GF_SLOT_LIST="$T/slots.list" GF_REBOOT_FILE="$T/reboot" GF_PAUSE_DIR="$T/pause" \
        GF_VERSION_FILE="$T/installed" ${RELHTTP:+GF_RELEASE_HTTP=1} \
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
mkdir -p "$T/data/archive" "$T/run" "$T/bin" "$T/log" "$T/pause" "$SYS/cnc" "$SYS/thermal" "$SYS/head"
echo idle > "$SYS/cnc/state"
for a in cnc/stop cnc/laser_latch cnc/laser_latch_cooling cnc/enable cnc/disable cnc/halt cnc/streaming \
         head/air_assist_pwm head/safe thermal/exhaust_pwm thermal/intake_pwm thermal/tec_on thermal/water_pump_on; do
    : > "$SYS/$a"
done
echo 8 > "$T/switches"
: > "$T/pulse"
: > "$T/port.log"
echo v0.0.10 > "$T/installed"
echo v0.0.10 > "$T/rel.tag"
printf 'cool_intake_fans = 1\nui_units = metric\n' > "$CONF"
cat > "$T/bin/grbl" <<EOF
#!/bin/sh
echo \$\$ >> "$T/ctl.pids"
echo \$\$ > "$T/ctl.pid"
printf '%s\n' "\$GF_REPORT_SECRET" > "$T/ctl.secret.new"
mv "$T/ctl.secret.new" "$T/ctl.secret"
exec sleep 600
EOF
cat > "$T/bin/cloud" <<EOF
#!/bin/sh
echo \$\$ >> "$T/ctl.pids"
printf '%s\n' "\$GF_REPORT_SECRET" > "$T/cloud.secret.new"
mv "$T/cloud.secret.new" "$T/cloud.secret"
exec sleep 600
EOF
chmod +x "$T/bin/grbl" "$T/bin/cloud"

# The GRBL controller's state file, as the live controller keeps it
# current: gstate.py <run dir> <state> writes it once; with --keep <file>
# it writes the state that file names twice a second. A state is Idle,
# Run, Hold, arming (Idle, the laser waiting for its press), connected
# (Idle, a Grbl sender on the port) or stale (Idle, 30 s old).
cat > "$T/gstate.py" <<'EOF'
import json, os, sys, time
def write(run, st):
    d = {'ts_mono': round(time.monotonic() - (30 if st == 'stale' else 0), 3),
         'state': st if st in ('Idle', 'Run', 'Hold') else 'Idle', 'alarm': 0,
         'laser': {'armed': False, 'arming': st == 'arming', 'model': 'density'},
         'sender': {'connected': st == 'connected'}}
    tmp = run + '/grbl.state.tmp'
    with open(tmp, 'w') as f:
        json.dump(d, f, separators=(',', ':'))
    os.replace(tmp, run + '/grbl.state')
if sys.argv[2] == '--keep':
    while True:
        try:
            want = open(sys.argv[3]).read().strip() or 'Idle'
        except OSError:
            want = 'Idle'
        write(sys.argv[1], want)
        time.sleep(0.5)
write(sys.argv[1], sys.argv[2])
EOF
echo Idle > "$T/gwant"
# gwant <state>: the job state from now on, written at once
gwant() { echo "$1" > "$T/gwant"; python3 "$T/gstate.py" "$T/run" "$1"; }

# The slot seam: two eMMC slots of the factory size, the first booted.
mkdir -p "$SLOTS/sys/mmcblk2p1" "$SLOTS/sys/mmcblk2p2"
echo 409600 > "$SLOTS/sys/mmcblk2p1/size"
echo 409600 > "$SLOTS/sys/mmcblk2p2/size"
printf 'etc/forgefirm-version=v0.0.10\nboot/zImage=kernel\n' > "$SLOTS/mmcblk2p1"
printf 'etc/forgefirm-version=v0.0.9\nboot/zImage=kernel\n' > "$SLOTS/mmcblk2p2"
ln -s mmcblk2p1 "$SLOTS/root"
# inventory <a type> <b type> <next>: the inventory ffboot -l prints, slot a booted
inventory() {
    printf 'booted.root=/dev/mmcblk2p1\nslot.a.device=/dev/mmcblk2p1\nslot.a.present=yes\nslot.a.type=%s\nslot.a.booted=yes\nslot.b.device=/dev/mmcblk2p2\nslot.b.present=yes\nslot.b.type=%s\nslot.%s.next=yes\n' \
        "$1" "$2" "$3" > "$T/slots.list"
}
inventory forgefirm forgefirm a
# ffboot's stand-in: its arguments recorded, the next boot moved in the
# inventory; with $T/ffboot.run-on-<slot> standing, a GRBL job starts as
# that slot is selected.
cat > "$SLOTS/ffboot" <<EOF
#!/bin/sh
echo "\$*" >> "$T/ffboot.log"
case \$1 in -e1) s=a ;; -e2) s=b ;; -s) s=sd ;; *) exit 2 ;; esac
[ ! -e "$T/ffboot.fail" ] || { echo "ffboot: refused"; exit 1; }
if [ -e "$T/ffboot.run-on-\$s" ]; then echo Run > "$T/gwant"; python3 "$T/gstate.py" "$T/run" Run; fi
grep -v '\.next=yes\$' "$T/slots.list" > "$T/slots.list.new"
echo "slot.\$s.next=yes" >> "$T/slots.list.new"
mv "$T/slots.list.new" "$T/slots.list"
EOF
chmod +x "$SLOTS/ffboot"

# fwup's stand-in: the archive is text, and the gate decides from what
# this prints, as it does from what fwup prints. -a writes the archive's
# file lines into the slot's file; it fails while $T/fwup.fail stands, and
# hangs (its pid in $T/fwup.pid) while $T/fwup.slow does.
mkdir -p "$T/path"
cat > "$T/path/fwup" <<'EOF'
#!/bin/sh
echo "$*" >> "${0%/*}/fwup.log"
op=$1
shift
f=
k=
d=
t=
while [ $# -gt 0 ]; do
    case $1 in -i) f=$2; shift ;; -p) k=$2; shift ;; -d) d=$2; shift ;; -t) t=$2; shift ;; esac
    shift
done
[ -f "$f" ] || exit 1
T=${0%/path/fwup}
case $op in
-m) grep '^meta-' "$f" ;;
-V) [ -n "$k" ] && grep -qxF "signed-by=$k" "$f" ;;
-l) grep -q '^meta-product=' "$f" && sed -n 's/^task //p' "$f" ;;
-a) [ ! -e "$T/fwup.fail" ] || { echo "fwup: the write failed"; exit 1; }
    if [ -e "$T/fwup.slow" ]; then echo $$ > "$T/fwup.pid"; sleep 30; fi
    grep -qxF "task $t" "$f" || { echo "fwup: no task $t"; exit 1; }
    sed -n 's/^file //p' "$f" > "$d" ;;
*) exit 1 ;;
esac
EOF
# The verification's read-only mount: the slot's file lines become files;
# with $T/mount.run standing, a GRBL job starts as the slot is read.
cat > "$T/path/mount" <<'EOF'
#!/bin/sh
T=${0%/path/mount}
eval "dev=\${$(($# - 1))}"
eval "mp=\${$#}"
[ -s "$dev" ] || { echo "mount: $dev: no filesystem"; exit 32; }
if [ -e "$T/mount.run" ]; then echo Run > "$T/gwant"; python3 "$T/gstate.py" "$T/run" Run; fi
while IFS= read -r line; do
    p=${line%%=*}
    mkdir -p "$mp/$(dirname "$p")"
    printf '%s\n' "${line#*=}" > "$mp/$p"
done < "$dev"
EOF
cat > "$T/path/umount" <<'EOF'
#!/bin/sh
for a in "$@"; do mp=$a; done
[ -d "$mp" ] && find "$mp" -mindepth 1 -delete
exit 0
EOF
chmod +x "$T/path/fwup" "$T/path/mount" "$T/path/umount"
KEY=/etc/forgefirm/keys/forgefirm-release.pub
# archive <file> <product> <version> <signed-by key or -> [more lines]: a
# stand-in archive with both upgrade tasks and a slot image of that version
archive() {
    f=$1; p=$2; v=$3; k=$4; shift 4
    { printf 'meta-product="%s"\nmeta-version="%s"\nmeta-platform="glowforge"\n' "$p" "$v"
      [ "$k" = - ] || echo "signed-by=$k"
      printf 'task upgrade.a\ntask upgrade.b\nfile etc/forgefirm-version=%s\nfile boot/zImage=kernel\n' "$v"
      for l in "$@"; do echo "$l"; done; } > "$f"
}
sha() { sha256sum "$1" | cut -d' ' -f1; }

# The stand-ins preloaded into the daemon: its reading of the wall clock
# (time() and CLOCK_REALTIME) runs ahead by the seconds the file
# WALLSHIFT_FILE names holds, while it exists (the kernel's clocks are not
# touched); and the switch device's ioctl reads the switch word's file
# (bit 2 the button, bit 3 the closed lid).
command -v cc > /dev/null || fatal "no C compiler (cc): the stand-ins are compiled here"
cat > "$T/wallshim.c" <<'EOF'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static long shift(void)
{
    const char *f = getenv("WALLSHIFT_FILE");
    int fd = f ? open(f, O_RDONLY | O_CLOEXEC) : -1;
    if (fd < 0)
        return 0;
    char b[32];
    ssize_t n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    b[n] = 0;
    return strtol(b, NULL, 10);
}

int clock_gettime(clockid_t clk, struct timespec *ts)
{
    static int (*real)(clockid_t, struct timespec *);
    if (!real)
        real = (int (*)(clockid_t, struct timespec *))dlsym(RTLD_NEXT, "clock_gettime");
    int rc = real(clk, ts);
    if (rc == 0 && clk == CLOCK_REALTIME)
        ts->tv_sec += shift();
    return rc;
}

time_t time(time_t *t)
{
    static time_t (*real)(time_t *);
    if (!real)
        real = (time_t (*)(time_t *))dlsym(RTLD_NEXT, "time");
    time_t now = real(NULL) + shift();
    if (t)
        *t = now;
    return now;
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
cc -shared -fPIC -O2 -o "$T/wallshim.so" "$T/wallshim.c" -ldl || fatal "the stand-ins did not build"

# The release server: the latest release ($T/rel.tag), its firmware file
# ($T/served.fw, sent in two halves 2 s apart while $T/served.slow
# stands), and the acceptance record late.
python3 - "$REL_PORT" "$T" <<'EOF' &
import http.server, json, os, sys, time
port, t = int(sys.argv[1]), sys.argv[2]
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        with open(t + '/rel.log', 'a') as o:
            o.write(self.path + '\n')
        tag = open(t + '/rel.tag').read().strip()
        slow = False
        if self.path == '/repos/openglow-org/forgefirm/releases/latest':
            body, code = json.dumps({
                'tag_name': tag, 'published_at': '2026-10-14T00:00:00Z',
                'body': 'Notes for the host test.\n\n- one line',
                'assets': [{'name': 'forgefirm.fw', 'size': 4096}, {'name': 'acceptance.json', 'size': 32}],
            }).encode(), 200
        elif self.path == '/releases/download/%s/acceptance.json' % tag:
            time.sleep(6)
            body, code = b'not an acceptance record\n', 200
        elif self.path == '/releases/download/%s/forgefirm.fw' % tag:
            body, code = open(t + '/served.fw', 'rb').read(), 200
            slow = os.path.exists(t + '/served.slow')
        else:
            body, code = b'{"message":"Not Found"}', 404
        self.send_response(code)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        if slow:
            self.wfile.write(body[:len(body) // 2])
            self.wfile.flush()
            time.sleep(2)
            self.wfile.write(body[len(body) // 2:])
        else:
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
python3 "$T/gstate.py" "$T/run" --keep "$T/gwant" &
KPID=$!
until_ok 25 test -s "$T/run/grbl.state" || fatal "the state file's writer did not start"
TOKEN=$(cat "$T/data/panel.token")
until_ok 50 test -s "$T/ctl.secret" || fatal "the controller left no report secret"
SEC=$(cat "$T/ctl.secret")
# report <secret> <mode> <armed>: a controller's job-state report
report() {
    [ "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "X-ForgeFIRM-Report: $1" -d "mode=$2" -d "armed=$3" \
              "$URL/cool/state")" = 200 ] || bad "the controller's report (mode=$2 armed=$3) was refused"
}
# armed <1|0>: the controller's report, its armed window open or closed
armed() { if [ "$1" = 1 ]; then report "$SEC" run 1; else report "$SEC" idle 0; fi; }
# The panel's reservation of the button, renewed until it is ready (on file
# long enough for both controllers to have read it), then ended.
reserve_ready() {
    until_ok 50 eval '[ "$(post "/button/reserve?on=1&id=d1" "$TOKEN")|$(jv "d.get(\"ready\")")" = "200|true" ]'
}
unreserve() { post '/button/reserve?on=0&id=d1' "$TOKEN" > /dev/null; }
# job_end: the update job's end, its result in $T/job.json
job_end() {
    until_ok 150 eval 'get /update/status > /dev/null; [ "$(jv "json.dumps(d[\"running\"])")" = false ]' \
        || bad "the job did not end within 30 s"
    cp "$T/body" "$T/job.json"
}
jres() { jget "$T/job.json" "$1" 2>/dev/null; }
NR='the button was not reserved for this: start it again from the control panel'

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
job_end
case $(jres 'str((d.get("result") or {}).get("error"))') in
    *"signature verification failed"*) ok "the job ended refused by the signature check" ;;
    *) bad "the job ended with $(cat "$T/job.json")" ;;
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
expect "unsigned|False|$(sha "$T/firmware.fw")" "$(jv 'd.get("signature")')|$(jv 'str(d.get("newer"))')|$(jv 'd.get("sha256")')" \
    "its signature reads unsigned, not newer than the installed v0.0.10, and its SHA-256 is named"
cmp -s "$T/firmware.fw" "$STAGED" && ok "and its bytes are staged" || bad "the staged file is not the upload"
# An upload refused at its first chunk (the machine running) opened
# nothing, and the archive staged before it is not its to discard.
echo running > "$SYS/cnc/state"
expect 400 "$(upload "$T/other.fw")" "an upload while the machine runs is 400"
cmp -s "$T/firmware.fw" "$STAGED" && ok "and the archive staged before it is left as it was" \
    || bad "the refused upload removed the staged archive"
echo idle > "$SYS/cnc/state"
rm -f "$STAGED"
grep -q '^-a' "$T/path/fwup.log" && bad "fwup was asked to apply" || ok "fwup was never asked to apply"

echo "== an upload's minute of silence"
# The minute of silence that holds a paused upload's staging file is on
# CLOCK_MONOTONIC: a step of the daemon's wall clock ends it for neither
# a second upload nor a job. The paused upload sends its first quarter,
# then the rest once $T/go exists.
printf 'meta-product="ForgeFIRM firmware"\nmeta-version=0.0.0-paused\n' > "$T/paused.fw"
python3 -c 'import sys; sys.stdout.write("# pad\n" * 20000)' >> "$T/paused.fw"
python3 - "$HTTP_PORT" "$TOKEN" "$T/paused.fw" "$T/go" > "$T/paused.answer" 2>&1 <<'EOF' &
import os, socket, sys, time
port, token, path, go = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
data = open(path, 'rb').read()
bnd = b'----host-update-paused'
pre = (b'--' + bnd + b'\r\nContent-Disposition: form-data; name="file"; filename="paused.fw"\r\n'
       b'Content-Type: application/octet-stream\r\n\r\n')
body = pre + data + b'\r\n--' + bnd + b'--\r\n'
head = (b'POST /update/upload HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nX-ForgeFIRM-Token: %s\r\n'
        b'Content-Type: multipart/form-data; boundary=%s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n'
        % (port, token.encode(), bnd, len(body)))
s = socket.create_connection(('127.0.0.1', port))
s.settimeout(60)
split = len(pre) + len(data) // 4
s.sendall(head + body[:split])
t0 = time.time()
while not os.path.exists(go) and time.time() - t0 < 120:
    time.sleep(0.1)
s.sendall(body[split:])
resp = b''
try:
    while True:
        c = s.recv(65536)
        if not c:
            break
        resp += c
except socket.timeout:
    pass
sys.stdout.write(resp.decode('latin-1'))
EOF
UPID=$!
until_ok 50 test -s "$STAGED" && ok "a paused upload's first part is staged" || bad "nothing of the paused upload was staged"
expect 409 "$(upload "$T/firmware.fw")" "a second upload beside it is 409"
echo 3600 > "$T/wallshift"
expect 409 "$(upload "$T/firmware.fw")" "with the wall clock an hour ahead and no new chunk, a second upload is still 409"
expect 409 "$(post '/update/download?probe=1' "$TOKEN")" "and a probe download beside it is 409"
rm -f "$T/wallshift"
touch "$T/go"
wait $UPID
expect 200 "$(awk 'NR == 1 { print $2 }' "$T/paused.answer")" "the paused upload then ends 200"
cmp -s "$T/paused.fw" "$STAGED" && ok "with its own archive staged" || bad "the staged file is not the paused upload's"
rm -f "$STAGED"

echo "== the download: newer than the installed version, and the archive its tag's"
DL=$T/data/download.fw
# The release is v0.0.10, the installed version too: not fetched.
expect 202 "$(post /update/download "$TOKEN")" "POST /update/download of a release that is not newer is 202"
job_end
case $(jres 'str((d.get("result") or {}).get("error"))') in
    *"not newer than the installed version"*) ok "the job ends: not newer than the installed version" ;;
    *) bad "the job ended with $(cat "$T/job.json")" ;;
esac
grep -qs 'forgefirm.fw' "$T/rel.log" && bad "the firmware file was requested" || ok "and the firmware file was not requested"
# downgrade=1 fetches it; an archive whose version is not the tag is discarded.
archive "$T/served.fw" "ForgeFIRM firmware" v0.0.9 "$KEY"
expect 202 "$(post '/update/download?downgrade=1' "$TOKEN")" "downgrade=1 fetches it (202)"
job_end
case $(jres 'str((d.get("result") or {}).get("error"))') in
    *"version v0.0.9, not the release's v0.0.10"*) ok "a release-signed v0.0.9 served under v0.0.10 is discarded" ;;
    *) bad "the job ended with $(cat "$T/job.json")" ;;
esac
[ ! -e "$DL" ] && [ ! -e "$DL.part" ] && ok "and nothing is staged, under either name" || bad "the discarded archive is staged"
# A newer release: v0.0.11.
echo v0.0.11 > "$T/rel.tag"
expect 200 "$(post /update/check "$TOKEN")" "the check finds v0.0.11"
archive "$T/served.fw" "ForgeFIRM firmware" v0.0.11 "$T/throwaway.pub"
expect 202 "$(post /update/download "$TOKEN")" "an archive signed with another key: 202"
job_end
case $(jres 'str((d.get("result") or {}).get("error"))') in
    *"signature verification failed"*) ok "it is discarded at the signature" ;;
    *) bad "the job ended with $(cat "$T/job.json")" ;;
esac
[ ! -e "$DL" ] && ok "and not staged" || bad "an archive signed with another key is staged"
archive "$T/served.fw" "ForgeFIRM extension" v0.0.11 "$KEY"
expect 202 "$(post /update/download "$TOKEN")" "a release-signed archive that is not firmware: 202"
job_end
case $(jres 'str((d.get("result") or {}).get("error"))') in
    *"not firmware"*) ok "it is discarded at the product gate" ;;
    *) bad "the job ended with $(cat "$T/job.json")" ;;
esac
[ ! -e "$DL" ] && ok "and not staged" || bad "an extension package is staged as the download"
archive "$T/served.fw" "ForgeFIRM firmware" v0.0.11 "$KEY"
: > "$T/served.slow"
expect 202 "$(post /update/download "$TOKEN")" "the release itself: 202"
until_ok 25 test -e "$DL.part" && [ ! -e "$DL" ] && ok "while it arrives it stands as download.fw.part, and no download.fw" \
    || bad "the arriving download stands as download.fw"
rm -f "$T/served.slow"
job_end
expect "True|v0.0.11" "$(jres 'str((d.get("result") or {}).get("ok"))')|$(jres '(d.get("result") or {}).get("version")')" \
    "the job ends ok with v0.0.11"
cmp -s "$T/served.fw" "$DL" && [ ! -e "$DL.part" ] && ok "and it is staged as download.fw" || bad "the verified download is not staged"
: > "$DL.part"
get /slots > /dev/null
expect "True" "$(jv 'str(d["staged"]["download"]["present"] and d["staged"]["download"]["version"] == "v0.0.11")')" \
    "GET /slots lists the verified download, not a part beside it"
rm -f "$DL.part"

echo "== the apply on the slot seam"
B=$SLOTS/mmcblk2p2
MARK=$T/data/slot-b.incomplete
expect 409 "$(post /update/apply "$TOKEN" -d slot=a -d file=download)" "an apply to the booted slot is 409"
mv "$B" "$B.away"
expect 409 "$(post /update/apply "$TOKEN" -d slot=b -d file=download)" "an apply to a slot whose device cannot be read is 409"
grep -qF 'booted root' "$T/body" && ok "refused as the booted root, failing closed" || bad "the refusal says '$(cat "$T/body")'"
mv "$B.away" "$B"
expect 202 "$(post /update/apply "$TOKEN" -d slot=b -d file=download -d "sha256=$(sha "$DL")")" \
    "the download applied to slot b, named by its SHA-256: 202"
job_end
expect "True|v0.0.11" "$(jres 'str((d.get("result") or {}).get("ok"))')|$(jres '(d.get("result") or {}).get("version")')" \
    "the job ends ok, the slot verified at v0.0.11"
grep -qxF 'etc/forgefirm-version=v0.0.11' "$B" && ok "slot b holds v0.0.11" || bad "slot b holds $(cat "$B")"
[ ! -e "$MARK" ] && ok "and carries no incomplete mark" || bad "a verified write left its slot marked"
grep '^-a' "$T/path/fwup.log" | tail -1 | grep -qF -- "-p $KEY" && ok "fwup applied it with the release key" \
    || bad "fwup -a ran without the release key: $(grep '^-a' "$T/path/fwup.log" | tail -1)"
# The job's own product gate: a release-signed extension package staged as
# the upload is refused before fwup writes.
archive "$STAGED" "ForgeFIRM extension" v0.0.12 "$KEY"
: > "$T/path/fwup.log"
expect 202 "$(post /update/apply "$TOKEN" -d slot=b -d file=upload)" "a release-signed extension package staged: 202"
job_end
case $(jres 'str((d.get("result") or {}).get("error"))') in
    *"not firmware"*) ok "the job refuses it at the product gate" ;;
    *) bad "the job ended with $(cat "$T/job.json")" ;;
esac
grep -q '^-a' "$T/path/fwup.log" && bad "fwup was asked to write it" || ok "and fwup never wrote"
# The archive the request names.
archive "$STAGED" "ForgeFIRM firmware" v0.0.12 "$KEY"
expect 202 "$(post /update/apply "$TOKEN" -d slot=b -d file=upload -d "sha256=$(sha "$T/firmware.fw")")" \
    "an apply naming another archive's SHA-256: 202"
job_end
case $(jres 'str((d.get("result") or {}).get("error"))') in
    *"not the one the install was asked for"*) ok "the job refuses the staged archive" ;;
    *) bad "the job ended with $(cat "$T/job.json")" ;;
esac
grep -q '^-a' "$T/path/fwup.log" && bad "fwup wrote another archive than the one named" || ok "and fwup never wrote"
# A write that fails, and one whose slot does not verify: the slot keeps
# its incomplete mark, which the boot selection refuses.
: > "$T/fwup.fail"
expect 202 "$(post /update/apply "$TOKEN" -d slot=b -d file=upload)" "an apply whose write fails: 202"
job_end
rm -f "$T/fwup.fail"
expect False "$(jres 'str((d.get("result") or {}).get("ok"))')" "the job fails"
[ -e "$MARK" ] && ok "slot b is marked incomplete" || bad "a failed write left no mark"
get /slots > /dev/null
expect True "$(jv 'str(d["slots"]["b"]["incomplete"])')" "GET /slots says slot b is incomplete"
: > "$T/ffboot.log"
expect 409 "$(post '/boot?target=b' "$TOKEN")" "the boot selection of the marked slot is 409"
[ -s "$T/ffboot.log" ] && bad "ffboot ran for the marked slot: $(cat "$T/ffboot.log")" || ok "and ffboot was not run"
rm -f "$MARK"
archive "$STAGED" "ForgeFIRM firmware" v0.0.12 "$KEY"
sed -i '/^file etc\/forgefirm-version/d' "$STAGED"
expect 202 "$(post /update/apply "$TOKEN" -d slot=b -d file=upload)" "an image with no version file: 202"
job_end
expect "False|written slot failed verification" "$(jres 'str((d.get("result") or {}).get("ok"))')|$(jres '(d.get("result") or {}).get("error")')" \
    "its slot fails the verification"
[ -e "$MARK" ] && ok "and keeps the incomplete mark" || bad "a slot that did not verify left no mark"
rm -f "$MARK"
archive "$STAGED" "ForgeFIRM firmware" v0.0.12 "$KEY"
sed -i '/^file boot\/zImage/d' "$STAGED"
expect 202 "$(post /update/apply "$TOKEN" -d slot=b -d file=upload)" "an image with no kernel: 202"
job_end
expect "False|written slot failed verification" "$(jres 'str((d.get("result") or {}).get("ok"))')|$(jres '(d.get("result") or {}).get("error")')" \
    "its slot fails the verification"
[ -e "$MARK" ] && ok "and keeps the incomplete mark" || bad "a slot with no kernel left no mark"
archive "$STAGED" "ForgeFIRM firmware" v0.0.12 "$KEY"
expect 202 "$(post /update/apply "$TOKEN" -d slot=b -d file=upload)" "v0.0.12 written again: 202"
job_end
expect "True" "$(jres 'str((d.get("result") or {}).get("ok"))')" "the job ends ok"
[ ! -e "$MARK" ] && ok "and the verified write clears the mark" || bad "the mark stands after a verified write"
# Not newer than the installed version: the operator's word and the button.
archive "$STAGED" "ForgeFIRM firmware" v0.0.9 "$KEY"
S9=$(sha "$STAGED")
get /slots > /dev/null
expect "False" "$(jv 'str(d["staged"]["upload"]["newer"])')" "GET /slots says the staged v0.0.9 is not newer"
: > "$T/path/fwup.log"
expect 202 "$(post /update/apply "$TOKEN" -d slot=b -d file=upload)" "an older archive without downgrade=1: 202"
job_end
case $(jres 'str((d.get("result") or {}).get("error"))') in
    *"not newer than the installed version"*"downgrade=1"*) ok "the job refuses it, naming downgrade=1 and the button" ;;
    *) bad "the job ended with $(cat "$T/job.json")" ;;
esac
grep -q '^-a' "$T/path/fwup.log" && bad "fwup wrote an older archive" || ok "and fwup never wrote"
code=$(post /update/apply "$TOKEN" -d slot=b -d file=upload -d downgrade=1 -d "sha256=$S9")
expect "403|not held" "$code|$(jv 'd.get("button")')" "downgrade=1 without the button: 403, the button named"
echo 12 > "$T/switches"
code=$(post /update/apply "$TOKEN" -d slot=b -d file=upload -d downgrade=1 -d "sha256=$S9")
expect "409|$NR" "$code|$(jv 'd.get("error")')" "downgrade=1 with the button held outside the reservation: 409"
reserve_ready || bad "the reservation did not come ready"
expect 400 "$(post /update/apply "$TOKEN" -d slot=b -d file=upload -d downgrade=1)" \
    "downgrade=1 with the button held that names no archive: 400"
expect 202 "$(post /update/apply "$TOKEN" -d slot=b -d file=upload -d downgrade=1 -d "sha256=$S9")" \
    "downgrade=1, the button held inside the reservation, the archive named: 202"
job_end
expect "True|v0.0.9" "$(jres 'str((d.get("result") or {}).get("ok"))')|$(jres '(d.get("result") or {}).get("version")')" \
    "the older version is written"
# An archive signed with another key: refused at the job's signature gate
# without confirm_unsigned=1, before fwup writes; with it, the button held
# inside the reservation and the archive named, written with no key.
archive "$STAGED" "ForgeFIRM firmware" v0.0.12 "$T/throwaway.pub"
SU=$(sha "$STAGED")
: > "$T/path/fwup.log"
expect 202 "$(post /update/apply "$TOKEN" -d slot=b -d file=upload)" "an archive signed with another key, no confirm_unsigned: 202"
job_end
case $(jres 'str((d.get("result") or {}).get("error"))') in
    "archive is not signed with the ForgeFIRM release key"*) ok "the job refuses it at its signature gate" ;;
    *) bad "the job ended with $(cat "$T/job.json")" ;;
esac
grep -q '^-a' "$T/path/fwup.log" && bad "fwup wrote an unsigned archive" || ok "and fwup never wrote"
reserve_ready || bad "the reservation did not come ready"
expect 202 "$(post /update/apply "$TOKEN" -d slot=b -d file=upload -d confirm_unsigned=1 -d "sha256=$SU")" \
    "confirm_unsigned=1, the button held inside the reservation, the archive named: 202"
job_end
expect "True|False" "$(jres 'str((d.get("result") or {}).get("ok"))')|$(jres 'str((d.get("result") or {}).get("signed"))')" \
    "it is written, unsigned"
grep '^-a' "$T/path/fwup.log" | tail -1 | grep -qF -- " -p " && bad "fwup applied an unsigned archive with a key" \
    || ok "fwup applied it with no key"
# A staged download is still the release it was downloaded as: one of
# another version staged over it is refused.
archive "$DL" "ForgeFIRM firmware" v0.0.12 "$KEY"
: > "$T/path/fwup.log"
expect 202 "$(post /update/apply "$TOKEN" -d slot=b -d file=download)" "a download staged over with another version: 202"
job_end
case $(jres 'str((d.get("result") or {}).get("error"))') in
    *"version v0.0.12, not the release v0.0.11"*) ok "the job refuses it: not the release it was downloaded as" ;;
    *) bad "the job ended with $(cat "$T/job.json")" ;;
esac
grep -q '^-a' "$T/path/fwup.log" && bad "fwup wrote a download of another version" || ok "and fwup never wrote"
# A download installs only with the release key, the button or not.
archive "$DL" "ForgeFIRM firmware" v0.0.11 -
: > "$T/path/fwup.log"
reserve_ready || bad "the reservation did not come ready"
expect 202 "$(post /update/apply "$TOKEN" -d slot=b -d file=download -d confirm_unsigned=1 -d "sha256=$(sha "$DL")")" \
    "an unsigned download, confirm_unsigned=1 and the button held: 202"
job_end
case $(jres 'str((d.get("result") or {}).get("error"))') in
    *"a download installs only with it"*) ok "the job refuses it" ;;
    *) bad "the job ended with $(cat "$T/job.json")" ;;
esac
grep -q '^-a' "$T/path/fwup.log" && bad "fwup wrote an unsigned download" || ok "and fwup never wrote"
unreserve
echo 8 > "$T/switches"
# The guards again under the update lock: the next boot moved onto the
# target while the job waited for the lock refuses the write.
archive "$STAGED" "ForgeFIRM firmware" v0.0.12 "$KEY"
python3 -c 'import fcntl, os, sys, time
fd = os.open(sys.argv[1], os.O_RDWR | os.O_CREAT, 0o644)
fcntl.flock(fd, fcntl.LOCK_EX)
open(sys.argv[2], "w").close()
while not os.path.exists(sys.argv[3]):
    time.sleep(0.05)' "$T/data/update.lock" "$T/locked" "$T/unlock" &
LKPID=$!
until_ok 25 test -e "$T/locked" || bad "the test could not take the update lock"
expect 409 "$(post '/boot?target=b' "$TOKEN")" "the boot selection under the update lock is 409"
expect 202 "$(post /update/apply "$TOKEN" -d slot=b -d file=upload)" "an apply under the update lock: 202, waiting for it"
inventory forgefirm forgefirm b
touch "$T/unlock"
wait $LKPID
job_end
case $(jres 'str((d.get("result") or {}).get("error"))') in
    *"slot selected for the next boot"*) ok "once the lock is free, the job refuses the slot that became the next boot" ;;
    *) bad "the job ended with $(cat "$T/job.json")" ;;
esac
rm -f "$T/locked" "$T/unlock"
inventory forgefirm forgefirm a
# The inventory under the update lock: the last probe's answer.
get /slots > /dev/null
python3 -c 'import fcntl, os, sys, time
fd = os.open(sys.argv[1], os.O_RDWR | os.O_CREAT, 0o644)
fcntl.flock(fd, fcntl.LOCK_EX)
open(sys.argv[2], "w").close()
while not os.path.exists(sys.argv[3]):
    time.sleep(0.05)' "$T/data/update.lock" "$T/locked" "$T/unlock" &
LKPID=$!
until_ok 25 test -e "$T/locked" || bad "the test could not take the update lock"
inventory forgefirm factory a
expect "200|forgefirm" "$(get /slots)|$(jv 'd["slots"]["b"]["type"]')" "GET /slots under the update lock answers the last probe"
touch "$T/unlock"
wait $LKPID
rm -f "$T/locked" "$T/unlock"
expect "200|factory" "$(get /slots)|$(jv 'd["slots"]["b"]["type"]')" "and probes again once it is free"
inventory forgefirm forgefirm a
# force is no parameter of the boot selection.
: > "$T/ffboot.log"
expect 200 "$(post '/boot?target=b&force=1' "$TOKEN")" "POST /boot target=b force=1 is 200"
expect "-e2 -n" "$(cat "$T/ffboot.log")" "and ffboot is run with no -f"

echo "== the factory return"
# Slot b holds ForgeFIRM and is the next boot (the selection above): the
# return restores the archived factory image into it.
printf 'etc/version=20260101000000\nboot/zImage=factory-kernel\n' | gzip -c > "$T/data/archive/factory-rootfs-20260101000000.img.gz"
echo "2026-01-01 00:00:00 slot2 factory 20260101000000 ver=2.6.0 factory-rootfs-20260101000000.img.gz md5=$(md5sum "$T/data/archive/factory-rootfs-20260101000000.img.gz" | cut -d' ' -f1)" \
    > "$T/data/archive/manifest"
code=$(post '/restore/factory-return?confirm=1' "$TOKEN")
expect "403|not held" "$code|$(jv 'd.get("button")')" "the return without the button: 403, the button named"
echo 12 > "$T/switches"
code=$(post '/restore/factory-return?confirm=1' "$TOKEN")
expect "409|$NR" "$code|$(jv 'd.get("error")')" "the button held outside the reservation: 409"
BMARK=$T/data/slot-b.incomplete
BBEFORE=$(cat "$B")
# A job that comes up as the next boot is pointed back, before the restore
# writes: nothing is written.
: > "$T/ffboot.run-on-a"
: > "$T/ffboot.log"
rm -f "$T/reboot"
reserve_ready || bad "the reservation did not come ready"
expect 202 "$(post '/restore/factory-return?confirm=1' "$TOKEN")" "the return, a job coming up at the point-back: 202"
job_end
rm -f "$T/ffboot.run-on-a"
gwant Idle
case $(jres 'str((d.get("result") or {}).get("error"))') in
    *"nothing was written"*) ok "the job that came up before the write keeps the machine: nothing was written" ;;
    *) bad "the job ended with $(cat "$T/job.json")" ;;
esac
[ "$(cat "$B")" = "$BBEFORE" ] && [ ! -e "$BMARK" ] && ok "slot b is as it was, with no mark" || bad "slot b holds $(cat "$B")"
# The guards again under the update lock: the next boot moved onto the
# target while the restore waited for the lock refuses the write.
python3 -c 'import fcntl, os, sys, time
fd = os.open(sys.argv[1], os.O_RDWR | os.O_CREAT, 0o644)
fcntl.flock(fd, fcntl.LOCK_EX)
open(sys.argv[2], "w").close()
while not os.path.exists(sys.argv[3]):
    time.sleep(0.05)' "$T/data/update.lock" "$T/locked" "$T/unlock" &
LKPID=$!
until_ok 25 test -e "$T/locked" || bad "the test could not take the update lock"
reserve_ready || bad "the reservation did not come ready"
expect 202 "$(post '/restore/factory-return?confirm=1' "$TOKEN")" "a return under the update lock: 202, waiting for it"
until_ok 50 eval 'get /update/status > /dev/null; [ "$(jv "d[\"phase\"]")" = "taking update lock" ]' \
    || bad "the restore did not reach the lock"
inventory forgefirm forgefirm b
touch "$T/unlock"
wait $LKPID
rm -f "$T/locked" "$T/unlock"
job_end
case $(jres 'str((d.get("result") or {}).get("error"))') in
    *"slot selected for the next boot"*) ok "once the lock is free, the restore refuses the slot that became the next boot" ;;
    *) bad "the job ended with $(cat "$T/job.json")" ;;
esac
[ "$(cat "$B")" = "$BBEFORE" ] && [ ! -e "$BMARK" ] && ok "and slot b is as it was" || bad "slot b holds $(cat "$B")"
inventory forgefirm forgefirm a
# A restore whose write fails, and one whose slot does not verify (no
# kernel): the slot keeps its mark. Each is the newest archive, its md5 in
# the manifest.
ARCH=$T/data/archive
cp "$ARCH/manifest" "$ARCH/manifest.keep"
add_archive() {
    echo "2026-02-02 00:00:00 slot2 factory $1 ver=2.7.0 factory-rootfs-$1.img.gz md5=$(md5sum "$ARCH/factory-rootfs-$1.img.gz" | cut -d' ' -f1)" \
        >> "$ARCH/manifest"
}
printf 'not a gzip stream\n' > "$ARCH/factory-rootfs-20260202000000.img.gz"
add_archive 20260202000000
reserve_ready || bad "the reservation did not come ready"
expect 202 "$(post '/restore/factory-return?confirm=1' "$TOKEN")" "a return whose restore write fails: 202"
job_end
case $(jres 'str((d.get("result") or {}).get("error"))') in
    *"restore write failed"*) ok "the restore's write fails" ;;
    *) bad "the job ended with $(cat "$T/job.json")" ;;
esac
[ -e "$BMARK" ] && ok "and slot b keeps its incomplete mark" || bad "a failed restore write left no mark"
rm -f "$BMARK" "$ARCH/factory-rootfs-20260202000000.img.gz"
printf 'etc/version=20260303000000\n' | gzip -c > "$ARCH/factory-rootfs-20260303000000.img.gz"
add_archive 20260303000000
reserve_ready || bad "the reservation did not come ready"
expect 202 "$(post '/restore/factory-return?confirm=1' "$TOKEN")" "a return whose restored image has no kernel: 202"
job_end
case $(jres 'str((d.get("result") or {}).get("error"))') in
    *"restored slot failed verification"*) ok "the restored slot fails its verification (no kernel)" ;;
    *) bad "the job ended with $(cat "$T/job.json")" ;;
esac
[ -e "$BMARK" ] && ok "and slot b keeps its incomplete mark" || bad "a restore that did not verify left no mark"
rm -f "$BMARK" "$ARCH/factory-rootfs-20260303000000.img.gz"
mv "$ARCH/manifest.keep" "$ARCH/manifest"
[ ! -e "$T/reboot" ] && ok "none of these rebooted" || bad "a failed return rebooted: $(cat "$T/reboot")"
inventory forgefirm forgefirm b
# A job that comes up during the restore keeps the machine.
: > "$T/mount.run"
: > "$T/ffboot.log"
rm -f "$T/reboot"
reserve_ready || bad "the reservation did not come ready"
expect 202 "$(post '/restore/factory-return?confirm=1' "$TOKEN")" "the return, the button held inside the reservation: 202"
job_end
rm -f "$T/mount.run"
gwant Idle
case $(jres 'str((d.get("result") or {}).get("error"))') in
    *"the next boot was not moved"*) ok "a job that came up during the restore ends it before the selection" ;;
    *) bad "the job ended with $(cat "$T/job.json")" ;;
esac
expect "-e1 -n" "$(cat "$T/ffboot.log")" "the next boot was pointed back at the running slot before the restore, and nothing after"
grep -qxF 'etc/version=20260101000000' "$B" && ok "slot b holds the factory image" || bad "slot b holds $(cat "$B")"
[ ! -e "$T/reboot" ] && ok "and nothing rebooted" || bad "the return rebooted under a job: $(cat "$T/reboot")"
# The factory image in place: only the selection moves; a job that comes
# up during it puts the next boot back and nothing reboots.
inventory forgefirm factory a
: > "$T/ffboot.run-on-b"
: > "$T/ffboot.log"
reserve_ready || bad "the reservation did not come ready"
expect 202 "$(post '/restore/factory-return?confirm=1' "$TOKEN")" "the return to the factory image in place: 202"
job_end
rm -f "$T/ffboot.run-on-b"
gwant Idle
case $(jres 'str((d.get("result") or {}).get("error"))') in
    *"the next boot is the running slot again"*) ok "a job that came up during the selection puts the next boot back" ;;
    *) bad "the job ended with $(cat "$T/job.json")" ;;
esac
expect "-e2 -n|-e1 -n" "$(paste -sd'|' "$T/ffboot.log")" "slot b selected, then slot a again"
[ ! -e "$T/reboot" ] && ok "and nothing rebooted" || bad "the return rebooted under a job: $(cat "$T/reboot")"
: > "$T/ffboot.log"
reserve_ready || bad "the reservation did not come ready"
expect 202 "$(post '/restore/factory-return?confirm=1' "$TOKEN")" "the return again, no job: 202"
job_end
expect "True|-e2 -n|reboot in 5 s" "$(jres 'str((d.get("result") or {}).get("ok"))')|$(cat "$T/ffboot.log")|$(cat "$T/reboot" 2>/dev/null)" \
    "slot b selected and the reboot 5 s out, for the button to be let go"
expect 409 "$(post '/boot?target=a' "$TOKEN")" "the return's reboot pending refuses a boot selection: 409"
rm -f "$T/reboot"
# A slot that probes as factory but is marked incomplete (a cut restore) is
# restored, never just selected.
inventory forgefirm factory a
echo cut > "$T/data/slot-b.incomplete"
: > "$T/ffboot.log"
reserve_ready || bad "the reservation did not come ready"
expect 202 "$(post '/restore/factory-return?confirm=1' "$TOKEN")" "the return to a marked slot that reads factory: 202"
job_end
expect "True|True" "$(jres 'str((d.get("result") or {}).get("ok"))')|$(jres 'str((d.get("result") or {}).get("restored"))')" \
    "the marked slot is restored, not just selected"
[ ! -e "$T/data/slot-b.incomplete" ] && ok "and its verified restore clears the mark" || bad "the mark stands after the restore"
rm -f "$T/reboot"
# Booted from the card: the slot that holds the factory image, or the one named.
printf 'card\n' > "$SLOTS/mmcblk1p1"
ln -sf mmcblk1p1 "$SLOTS/root"
inventory forgefirm forgefirm sd
expect 409 "$(post '/restore/factory-return?confirm=1' "$TOKEN")" "booted from the card with ForgeFIRM in both slots: 409"
grep -qF 'name the slot' "$T/body" && ok "and the operator is asked to name the slot" || bad "the refusal says '$(cat "$T/body")'"
inventory factory forgefirm sd
: > "$T/ffboot.log"
reserve_ready || bad "the reservation did not come ready"
expect 202 "$(post '/restore/factory-return?confirm=1' "$TOKEN")" "booted from the card with the factory image in slot a: 202"
job_end
expect "True|a|-e1 -n" "$(jres 'str((d.get("result") or {}).get("ok"))')|$(jres '(d.get("result") or {}).get("slot")')|$(cat "$T/ffboot.log")" \
    "slot a is the one selected"
unreserve
echo 8 > "$T/switches"
ln -sf mmcblk2p1 "$SLOTS/root"
rm -f "$SLOTS/mmcblk1p1" "$T/reboot"
inventory forgefirm forgefirm a

echo "== one predicate: nothing reboots, moves the boot or starts an update job under a job"
: > "$T/ffboot.log"
# The reboot, the boot selection, the factory return, an update job's start,
# the reset of every setting (its reboot), and a downgrade's and an unsigned
# install asked before the button is read (the button is up here: 409,
# never 403's "hold the button").
refused_all() {
    for r in '/system/reboot?confirm=1' '/boot?target=b' '/restore/factory-return?confirm=1' '/update/download?probe=1' \
             '/system/reset?confirm=1' '/update/apply?slot=b&file=upload&downgrade=1' \
             '/update/apply?slot=b&file=upload&confirm_unsigned=1'; do
        code=$(post "$r" "$TOKEN")
        [ "$code" = 409 ] || { bad "$1: POST $r -> $code $(cat "$T/body")"; return; }
    done
    ok "$1: the reboot, the boot selection, the factory return, an update job's start, the reset, a downgrade's and an unsigned install are each 409"
}
# not_ready <state>: a hold dialog's reservation is not ready for its press
not_ready() {
    code=$(post '/button/reserve?on=1&id=d2' "$TOKEN")
    expect "200|False" "$code|$(jv 'str(d.get("ready"))')" "$1: a hold dialog's reservation is not ready for its press"
    post '/button/reserve?on=0&id=d2' "$TOKEN" > /dev/null
}
# taken <state>: no job, so the boot selection (of the slot already next) is 200
taken() {
    expect 200 "$(post '/boot?target=a' "$TOKEN")" "$1: the boot selection is taken (200)"
    : > "$T/ffboot.log"
}
# at: the monotonic clock; stale <at>: wait until a report made at <at> is
# past the cooling engine's 5 s
at() { python3 -c 'import time; print(time.monotonic())'; }
stale() { python3 -c 'import sys, time; time.sleep(max(0.0, float(sys.argv[1]) + 5.5 - time.monotonic()))' "$1"; }
[ -s "$STAGED" ] || bad "no upload is staged for the downgrade's case"
echo running > "$SYS/cnc/state"
refused_all "the kernel running"
echo idle > "$SYS/cnc/state"
armed 1
refused_all "a job inside its armed window"
armed 0
# A cloud print paused past its grace: its window closed (armed 0), the
# print still running (mode run); a press would re-arm it.
report "$SEC" run 0
refused_all "a print paused past its grace (mode run, the window closed)"
not_ready "a print paused past its grace"
armed 0
for s in arming Run Hold stale connected; do
    gwant $s
    refused_all "the GRBL state $s"
done
gwant Idle
# The cloud controller in place of GRBL, so no GRBL state file is read: its
# wait for the press (armed with the mode still idle: the cloud client arms
# before its warm-up) and a print paused past its grace are each refused,
# fresh and with the report past 5 s (a controller gone silent); its
# cooldown is no job; and the controller's stop forgets its last report.
# switch <controller> <its secret file>: POST /mode, which waits for the new
# controller's first report; switch_end: the report made, the answer read.
switch() {
    rm -f "$2"
    post /mode "$TOKEN" -d controller=$1 > "$T/mode.code" &
    MPID=$!
    until_ok 100 test -s "$2" || bad "the $1 controller left no report secret"
}
switch_end() {
    report "$(cat "$1")" idle 0
    wait $MPID
    expect 200 "$(cat "$T/mode.code")" "$2"
}
expect 200 "$(post /settings "$TOKEN" -d cloud_enabled=1 --data-urlencode 'phrase=I UNDERSTAND')" "cloud mode enabled"
switch cloud "$T/cloud.secret"
switch_end "$T/cloud.secret" "the cloud controller started in place of GRBL"
CSEC=$(cat "$T/cloud.secret")
report "$CSEC" idle 1
R0=$(at)
refused_all "the cloud client waiting for its press (armed, mode idle)"
not_ready "the cloud client waiting for its press"
stale "$R0"
refused_all "the same report gone stale"
not_ready "the same report gone stale"
report "$CSEC" cooldown 0
taken "the cloud client's cooldown reported"
report "$CSEC" run 0
R0=$(at)
refused_all "the cloud controller's print paused past its grace"
not_ready "the cloud controller's print paused past its grace"
stale "$R0"
refused_all "the same report gone stale"
# The print armed again, then its controller stopped, GRBL started and not
# yet reported: the stopped controller's mode and armed flag are forgotten.
report "$CSEC" run 1
switch grbl "$T/ctl.secret"
taken "the paused print's controller stopped"
switch_end "$T/ctl.secret" "GRBL again, the cloud controller stopped"
SEC=$(cat "$T/ctl.secret")
[ ! -e "$T/reboot" ] && [ ! -s "$T/ffboot.log" ] && ok "nothing was rebooted or selected" \
    || bad "a refused request rebooted or selected: $(cat "$T/reboot" "$T/ffboot.log" 2>/dev/null)"
get /update/status > /dev/null
expect false "$(jv 'json.dumps(d["running"])')" "and no update job started"
# pause <point>: the next request to reach <point> waits there (the test
# hook GF_PAUSE_DIR) until go_on <point>; arrived <point>: one waits there.
pause() { : > "$T/pause/$1"; }
arrived() { until_ok 50 test -e "$T/pause/$1.at"; }
go_on() { rm -f "$T/pause/$1"; }
# bgpost <status file> <curl args...>: a POST in the background, its status
# into the file; BG its pid
bgpost() { o=$1; shift; curl -s -o /dev/null -w '%{http_code}' -X POST "$@" > "$o" & BG=$!; }
# A report that passed its secret check as its controller is stopped
# stores nothing: the stop's forgetting stands (the armed flag it carried
# would refuse every route with no controller left to cancel it).
pause report
bgpost "$T/late.code" -H "X-ForgeFIRM-Report: $SEC" -d mode=idle -d armed=1 "$URL/cool/state"
arrived report || bad "the report did not reach its store"
expect 200 "$(post /controller/stop "$TOKEN")" "the controller stopped while its report waits to be stored"
go_on report
wait $BG
expect 409 "$(cat "$T/late.code")" "that report, its controller stopped since its check: 409"
taken "the stopped controller's late report stored nothing"
rm -f "$T/ctl.secret"
expect 200 "$(post /controller/start "$TOKEN")" "the controller started again"
until_ok 100 test -s "$T/ctl.secret" || bad "the controller left no report secret"
SEC=$(cat "$T/ctl.secret")
armed 0
# A reboot route holds the machine's lease from its check to its schedule,
# so an update job asked for in between is refused; once scheduled, the
# reboot refuses what it would cut, and a second consent on the hold that
# asked for it. An update job whose check passed before a reboot was
# scheduled asks again under its own hold.
pause reboot
bgpost "$T/bg.code" -H "X-ForgeFIRM-Token: $TOKEN" "$URL/system/reboot?confirm=1"
arrived reboot || bad "the reboot did not reach its schedule"
expect 409 "$(post /update/apply "$TOKEN" -d slot=b -d file=upload)" \
    "an apply while the reboot route is between its check and its schedule: 409"
go_on reboot
wait $BG
expect 200 "$(cat "$T/bg.code")" "that reboot: 200"
expect 409 "$(post /update/apply "$TOKEN" -d slot=b -d file=upload)" "an apply while the reboot is pending: 409"
rm -f "$T/reboot"
echo 12 > "$T/switches"
reserve_ready || bad "the reservation did not come ready"
DG="-d slot=b -d file=upload -d downgrade=1 -d sha256=$(sha "$STAGED")"
pause reboot
bgpost "$T/bg.code" -H "X-ForgeFIRM-Token: $TOKEN" "$URL/system/reset?confirm=1"
arrived reboot || bad "the reset did not reach its schedule"
expect 409 "$(post /update/apply "$TOKEN" $DG)" \
    "a downgrade on the same hold while the reset is between its check and its schedule: 409"
go_on reboot
wait $BG
expect 200 "$(cat "$T/bg.code")" "the reset, the button held inside the reservation: 200"
expect 409 "$(post /update/apply "$TOKEN" $DG)" "a downgrade on the same hold, the reset's reboot pending: 409"
rm -f "$T/reboot" "$T/data/reset-requested"
pause job_start
bgpost "$T/bg.code" -H "X-ForgeFIRM-Token: $TOKEN" $DG "$URL/update/apply"
arrived job_start || bad "the downgrade did not reach its hold"
expect 200 "$(post '/system/reset?confirm=1' "$TOKEN")" "the reset while a downgrade is between its check and its hold: 200"
go_on job_start
wait $BG
expect 409 "$(cat "$T/bg.code")" "that downgrade, asked again under its hold: 409"
unreserve
echo 8 > "$T/switches"
rm -f "$T/reboot" "$T/data/reset-requested"
get /update/status > /dev/null
expect false "$(jv 'json.dumps(d["running"])')" "and no update job started"

echo "== the daemon's stop during a slot write"
archive "$STAGED" "ForgeFIRM firmware" v0.0.12 "$KEY"
: > "$T/fwup.slow"
expect 202 "$(post /update/apply "$TOKEN" -d slot=b -d file=upload)" "an apply whose write hangs: 202"
until_ok 50 test -s "$T/fwup.pid" || bad "the write did not start"
expect 409 "$(post '/system/reset?confirm=1' "$TOKEN")" "the reset while the update job writes: 409"
WPID=$(cat "$T/fwup.pid")
stop_daemon
rm -f "$T/fwup.slow"
alive "$WPID" && bad "the write runs on after the daemon stopped" || ok "the daemon's stop ended the write"
[ -e "$MARK" ] && ok "and slot b stays marked incomplete" || bad "the ended write left no mark"
rm -f "$T/fwup.pid"

echo "== the release server is HTTPS alone"
# Without the test hook curl takes no plain-HTTP server, the release
# check's and the download's alike.
# A download a power loss cut stands under its own name; the start
# removes it.
: > "$DL.part"
RELHTTP= start_daemon
[ ! -e "$DL.part" ] && ok "the start removed the download.fw.part a cut download left" \
    || bad "download.fw.part stands after the start"
expect 502 "$(post /update/check "$TOKEN")" "with no GF_RELEASE_HTTP the plain-HTTP release server is not taken: 502"
stop_daemon

if [ $fails -eq 0 ]; then
    echo "PASS: host_update"
else
    echo "FAIL: host_update, $fails checks"
    echo "--- the daemon's output"
    tail -60 "$T/out"
    exit 1
fi
