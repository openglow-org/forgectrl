#!/bin/sh
# host_init.sh - the daemon's init script: a stop that waits, a restart that
# never runs two daemons, a fixed environment
# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# SPDX-License-Identifier: MIT
#
# Runs a copy of init/forgectrl.init with its daemon, pid file and fifo in a
# temporary directory, the daemon a stand-in named forgectrl that takes 1 s
# to stop after its SIGTERM (as the daemon's own stop takes seconds), holds a
# lock while it runs, and records what it was started with:
#   - a start passes none of the caller's environment to the daemon;
#   - a stop returns only once the daemon has exited;
#   - a restart's start comes after the old daemon's exit: never two at once;
#   - a daemon whose wrapper is gone gets the stop's own SIGTERM, and ends
#     through its own stop;
#   - a daemon that does not stop within the bound is killed, and the stop
#     returns.
# Runs unprivileged; build.yml runs it:
#
#   sh tests/host_init.sh
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
RELAY=
trap 'rc=$?; set +e; kill $RELAY 2>/dev/null; for p in $(pidof forgectrl 2>/dev/null); do [ "$(readlink -f /proc/$p/exe 2>/dev/null)" = "$T/forgectrl" ] && kill -9 $p; done; rm -rf "$T"; exit $rc' EXIT
fail() { echo "FAIL: $1"; echo "--- the stand-in's log"; cat "$T/log" 2>/dev/null; exit 1; }

cat > "$T/standin.c" <<'EOF'
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t term;
static void on_term(int s) { (void)s; term = 1; }

static void say(const char *dir, const char *what)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/log", dir);
    FILE *f = fopen(p, "a");
    if (f) {
        fprintf(f, "%s %d\n", what, (int)getpid());
        fclose(f);
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    char dir[400], p[512];
    snprintf(dir, sizeof(dir), "%s", argv[0]);
    *strrchr(dir, '/') = '\0';
    signal(SIGTERM, on_term);
    snprintf(p, sizeof(p), "%s/lock", dir);
    int lk = open(p, O_CREAT | O_RDWR, 0600);
    if (flock(lk, LOCK_EX | LOCK_NB) != 0)
        say(dir, "two-at-once");
    say(dir, getenv("HOST_INIT_CALLER") ? "start-with-callers-env" : "start");
    snprintf(p, sizeof(p), "%s/hang", dir);
    while (!term)
        pause();
    if (access(p, F_OK) == 0)
        for (;;)
            pause();
    /* Its own stop takes a second, a second SIGTERM notwithstanding. */
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        struct timespec ts = { 0, 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 < 1000);
    say(dir, "exit");
    return 0;
}
EOF
cc -o "$T/forgectrl" "$T/standin.c" || fail "the stand-in does not build"
sed -e "s|^DAEMON=.*|DAEMON=$T/forgectrl|" -e "s|^PIDFILE=.*|PIDFILE=$T/pid|" \
    -e "s|^FIFO=.*|FIFO=$T/fifo|" -e "s|^STOP_WAIT_S=.*|STOP_WAIT_S=2|" \
    "$ROOT/init/forgectrl.init" > "$T/init"
chmod +x "$T/init"              # a restart runs its own $0 stop and start
grep -q "^DAEMON=$T/forgectrl\$" "$T/init" && grep -q "^STOP_WAIT_S=2$" "$T/init" \
    || fail "the script's daemon and bound lines were not found"
alive() { for p in $(pidof forgectrl 2>/dev/null); do [ "$(readlink -f /proc/$p/exe 2>/dev/null)" = "$T/forgectrl" ] && return 0; done; return 1; }
started() { i=0; until [ "$(grep -c '^start' "$T/log" 2>/dev/null)" -ge "$1" ]; do i=$((i + 1)); [ $i -le 50 ] || return 1; sleep 0.1; done; }

HOST_INIT_CALLER=1 "$T/init" start > /dev/null
started 1 || fail "the daemon did not start"
grep -q '^start ' "$T/log" || fail "the caller's environment reached the daemon"
echo "  ok: a start passes none of the caller's environment to the daemon"

"$T/init" stop > /dev/null
! alive || fail "the stop returned with the daemon still running"
grep -q '^exit ' "$T/log" || fail "the daemon did not finish its own stop"
echo "  ok: a stop returns once the daemon has exited, its own stop finished"

"$T/init" start > /dev/null
started 2 || fail "the daemon did not start again"
"$T/init" restart > /dev/null
started 3 || fail "the restart's daemon did not start"
! grep -q '^two-at-once' "$T/log" || fail "a restart ran two daemons at once"
[ "$(sed -n '4p' "$T/log" | cut -d' ' -f1)" = exit ] && [ "$(sed -n '5p' "$T/log" | cut -d' ' -f1)" = start ] \
    || fail "the restart's start did not follow the old daemon's exit"
echo "  ok: a restart's start follows the old daemon's exit: never two daemons"

# The wrapper killed, the daemon left an orphan: the pid file names a
# process that is gone, and the stop signals the daemon itself.
WRAP=$(cat "$T/pid")
RELAY=$(pgrep -P "$WRAP" -x logger) || fail "the wrapper's logger relay was not found"
exits=$(grep -c '^exit ' "$T/log")
kill -9 "$WRAP"
"$T/init" stop > /dev/null 2>&1
kill $RELAY 2>/dev/null || true # the relay the wrapper's trap would have ended
RELAY=
! alive || fail "the stop returned with the orphaned daemon still running"
[ "$(grep -c '^exit ' "$T/log")" -eq $((exits + 1)) ] \
    || fail "a daemon whose wrapper is gone did not end through its own stop"
echo "  ok: a daemon whose wrapper is gone gets the stop's SIGTERM and finishes its own stop"

"$T/init" start > /dev/null
started 4 || fail "the daemon did not start again"
: > "$T/hang"
"$T/init" stop > /dev/null
! alive || fail "a daemon that does not stop survived the stop"
echo "  ok: a daemon that does not stop within the bound is killed, and the stop returns"
echo "PASS: host_init"
