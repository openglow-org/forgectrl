#!/bin/sh
# fflog_e2e.sh - host end-to-end check of the ForgeFIRM logging path
# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# SPDX-License-Identifier: MIT
#
# Runs a private rsyslogd on the shipped rsyslog.conf (from the
# forgefirm layer) plus rules rendered by `forgectrl --render-syslog`
# from a test settings file, feeds it through the real emitter
# (fflog_e2e_test) and `logger` relays, and checks that the lines land
# in the right per-logger files, in the ff_line format, filtered by
# level: a tag no logger names in system/, the kern facility in
# kernel/. It runs in a mount namespace of its own, with its temporary
# directory mounted over /data and a tmpfs over /run, so the machine
# paths the renderer and the rules name land in that directory and
# nothing outside it is written. Needs root, unshare, logger, python3,
# and rsyslogd; build.yml runs it:
#
#   sudo tests/fflog_e2e.sh <build-dir> <path/to/rsyslog.conf>
set -e
BUILD=$(cd "${1:?build dir}" && pwd)
RSCONF=$(cd "$(dirname "${2:?shipped rsyslog.conf}")" && pwd)/$(basename "$2")
if [ -z "$FFLOG_E2E_NS" ]; then
    made=
    [ -d /data ] || { mkdir /data; made=1; }
    rc=0
    FFLOG_E2E_NS=1 unshare -m --propagation private sh "$0" "$BUILD" "$RSCONF" || rc=$?
    [ -z "$made" ] || rmdir /data
    exit $rc
fi
T=$(mktemp -d)
# the namespace's mounts go with it; rsyslogd is stopped before its
# directory is removed, and the script's status is kept
trap 'rc=$?; set +e; [ -z "$RSPID" ] || { kill $RSPID; wait $RSPID; } 2>/dev/null; rm -rf "$T"; exit $rc' EXIT
mkdir "$T/data"
mount --bind "$T/data" /data
mount -t tmpfs tmpfs "$(readlink -f /var/run)"
L="$T/data/log/forgefirm"

# settings under test: grblhal at debug, forgectrl at warning
cat > "$T/forgefirm.conf" <<EOF
log_grblhal_disk = debug
log_forgectrl_disk = warning
log_gfcloud_disk = off
EOF
FORGECTRL_CONF="$T/forgefirm.conf" "$BUILD/forgectrl" --render-syslog

# private rsyslog: the shipped conf with the socket and the work
# directory moved, and imklog dropped
sed -e "s|SysSock.Use=\"on\"|SysSock.Use=\"on\" SysSock.Name=\"$T/log\"|" \
    -e "s|workDirectory=\"[^\"]*\"|workDirectory=\"$T\"|" \
    -e '/imklog/d' "$RSCONF" > "$T/rsyslog.conf"
rsyslogd -f "$T/rsyslog.conf" -i "$T/rsyslogd.pid" -n >"$T/rsyslogd.out" 2>&1 &
RSPID=$!
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ -S "$T/log" ] && break
    sleep 0.2
done
[ -S "$T/log" ] || { echo "rsyslogd did not open $T/log"; cat "$T/rsyslogd.out"; exit 1; }

FFLOG_CONF="$T/forgefirm.conf" FFLOG_SOCK="$T/log" "$BUILD/fflog_e2e_test" grblhal
FFLOG_CONF="$T/forgefirm.conf" FFLOG_SOCK="$T/log" "$BUILD/fflog_e2e_test" forgectrl
FFLOG_CONF="$T/forgefirm.conf" FFLOG_SOCK="$T/log" "$BUILD/fflog_e2e_test" gfcloud
echo "relay traceback line" | logger -u "$T/log" -t gfhome -p daemon.warning
echo "stray tag line" | logger -u "$T/log" -t e2e-stray -p daemon.warning
# logger turns the kern facility into user, so the kernel's line goes
# in as the datagram it is: <4>, kern.warning
python3 -c 'import socket, sys; s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); s.connect(sys.argv[1]); s.send(b"<4>e2e-kern: kern facility line")' "$T/log"
sleep 2

fail=0
check() { if eval "$1"; then echo "  ok: $2"; else echo "  FAIL: $2"; fail=1; fi; }
G=$L/grblhal/grblhal.log
F=$L/forgectrl/forgectrl.log
C=$L/gfcloud/gfcloud.log
H=$L/gfhome/gfhome.log
S=$L/system/system.log
K=$L/kernel/kernel.log
echo "--- grblhal.log"; tail -8 "$G"
check "grep -q ' grblhal\[[0-9]*\] DEBUG e2e debug line$' $G" "grblhal debug line, ff_line format"
check "grep -q ' grblhal\[[0-9]*\] ERR e2e error line$' $G" "grblhal error line"
check "grep -q 'INFO e2e info line with newline$' $G" "trailing newline stripped"
check "grep -q 'e2e long xxxx' $G" "long line delivered"
check "grep -qE '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:.]+[+-][0-9:]+ grblhal' $G" "RFC 3339 timestamp"
check "! grep -q 'e2e' $F || ! grep -q 'e2e info line' $F" "forgectrl info line filtered at warning"
check "grep -q 'WARNING e2e warning line' $F" "forgectrl warning line kept"
check "! grep -q 'e2e' $F || ! grep -q 'e2e debug line' $F" "forgectrl debug line filtered"
check "[ ! -s $C ]" "gfcloud off: nothing written"
check "grep -q ' gfhome\[[0-9-]*\] WARNING relay traceback line$' $H" "logger relay routed by tag"
check "grep -q ' e2e-stray\[[0-9-]*\] WARNING stray tag line$' $S && ! grep -rq 'stray tag line' $L --exclude=system.log" \
    "a tag no logger names lands in system/, and only there"
check "grep -q 'WARNING kern facility line$' $K && ! grep -rq 'kern facility line' $L --exclude=kernel.log" \
    "the kern facility lands in kernel/, and only there"
check "[ ! -e /data/log/forgefirm ] || [ /data/log/forgefirm -ef $L ]" "the files are under the temporary directory"
[ $fail -eq 0 ] && echo PASSED || { echo FAILED; exit 1; }
