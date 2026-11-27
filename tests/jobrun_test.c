/*
 * jobrun_test.c - host test for the job runner
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * A mock controller on a local port answers ok per line and error:9 for
 * a line that says BAD, records a soft reset, raises the tube's current
 * after a chosen line, and holds the ok of a line that says P77 until the
 * test lets go, so a run can be looked at while it plays. A mock sysfs
 * tree carries the witnesses the test flips.
 *
 * The runner: a posted program's check (what is
 * stripped, every offense with its line number), and a package's check
 * (G10, G20, G28.1, G30.1, G43, G51 and G92 in each form the controller's
 * parser reads, G21, G49 and G50 taken, never in a comment; M103 taken
 * only as M103 with one P of 0 or 1 that is the tray's mode in force, and
 * never when that mode cannot be read); a package's program has its M103
 * judged again as the line goes out, and a tray's mode changed since ends
 * the run with the line unsent, while an operator's is not judged so; a
 * program plays as
 * job:<name> with the machine lease held as a sender, refuses a second
 * program and a wizard in the holder's words, gets the runner's M2, and
 * leaves the lease free and its staging file gone; a program that ends
 * in M2 gets no second one; a line is at most 250 characters once
 * trimmed; the runner's $X goes out first when asked for after the
 * runner's own abort or a lost connection's reset while the controller
 * still reads that stop's alarm 3 with the same sender generation, from
 * the same controller, the stop shown late in the read among them, and
 * never otherwise (not with the runner unwired, not after a clean run, the
 * controller's alarm lock, alarm or reset, nor after a run the controller
 * answered no line of (an alarm 3 standing from before it), nor after a
 * stop the controller did not show as alarm 3 with no connection (a stop
 * at rest, another alarm at the stop, a document still connected, a
 * controller restarting inside the read, an alarm shown only once the
 * socket was given back), nor once
 * another alarm, a home, release or energize of this daemon's, another
 * session or another controller came since, nor with no state to read); a
 * program is done when the kernel is idle,
 * not at its last ok nor while its state cannot be read, and at once on an
 * underrun; the abort sends the soft reset and frees the
 * lease; a package's abort ends its own job alone, and never one that
 * starts between its read of the runner and its stop (the link wraps
 * pthread_mutex_unlock to start one there); a connected Grbl sender refuses the start with nothing sent; a
 * start keeps the Grbl socket to this host first, a controller that calls
 * the machine busy refuses it with nothing sent, and a run gives the
 * socket back once at its end; a
 * run on the caller's thread stands inside a wizard's hold and gives the
 * machine back to the wizard, and is not the abort route's to stop; the
 * ready and done hooks of a started run, with the lease given back from
 * inside done; a program that must light and does not, fails; a program
 * that must light is witnessed every tick.
 *
 * The sender underneath (jobstream): a dark program streams to its end;
 * an error stops the run at the line that drew it; a lit program ends
 * after its dark stretch with the witnesses read; the generator's pacing
 * return, and the abort that ends it with the soft reset; the witness
 * rate against the ticks; an ok that answers another session's line is
 * not counted; the sender check reads the state file, and one past its
 * age is no answer. A reset the controller reports (its banner again, as
 * the driver writes it and as a Grbl-compatible build does) fails the run
 * at once with no soft reset of the runner's, in the controller's warning
 * just before it; a connection the controller closes mid-run fails it,
 * with the soft reset on a connection of its own, and so does a write
 * the closed connection refuses (the link wraps write()); one it turns away at
 * the accept fails it with no reset; the window out is half the
 * controller's ring and 32 lines; the controller's alarm stops the run at
 * its line, and one in the end's dark wait fails it; a generator line too
 * long for the buffer fails the run, never split.
 */
#define _GNU_SOURCE
#include "../src/jobrun.h"
#include "../src/lease.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* The link wraps write(): a write that carries fail_write_text fails as a
 * write to a connection the controller closed does, whoever makes it. */
static const char *volatile fail_write_text;
ssize_t __real_write(int fd, const void *buf, size_t n);
ssize_t __wrap_write(int fd, const void *buf, size_t n)
{
    const char *t = fail_write_text;
    if (t && n >= strlen(t) && memmem(buf, n, t, strlen(t))) {
        fail_write_text = NULL;
        errno = EPIPE;
        return -1;
    }
    return __real_write(fd, buf, n);
}

/* The link wraps pthread_mutex_unlock() as well: armed on one thread, its
 * next unlock there is followed at once by race_start(), so a run can be
 * started between a read under the runner's lock and what comes next. */
static void (*volatile race_start)(void);
static pthread_t race_thread;
int __real_pthread_mutex_unlock(pthread_mutex_t *m);
int __wrap_pthread_mutex_unlock(pthread_mutex_t *m)
{
    int rc = __real_pthread_mutex_unlock(m);
    void (*fn)(void) = race_start;
    if (fn && pthread_equal(pthread_self(), race_thread)) {
        race_start = NULL;
        fn();
    }
    return rc;
}

static char root[128], staged[192];

static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void sysfs_write(const char *attr, long v)
{
    char path[256], tmp[272];
    snprintf(path, sizeof(path), "%s/%s", root, attr);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "%ld\n", v);
    fclose(f);
    rename(tmp, path);
}

/* ---- the mock controller ---- */

static struct {
    int listen_fd;
    volatile int lines, resets, holding, release;
    char seen[64][80];
    int lit_after;              /* set hv after this many lines */
    volatile int stray_ok;      /* an ok after the banner that answers nothing of this session's */
    volatile int reset_after;   /* after this many lines' ok, the banner again (a reset), then silence */
    volatile int close_after;   /* close the connection on taking this many lines */
    volatile int ahead_bytes, ahead_lines;  /* the most seen waiting behind a held line */
    volatile int alarm_after;   /* ALARM:1 after this many lines' ok, with nothing in flight */
    volatile int refuse;        /* turn this many connections away at the accept, as a busy controller does */
    volatile int grbl_banner;   /* the banner of a controller built for Grbl compatibility */
    const char *early_msg;      /* a warning after the banner, before any line */
    const char *reset_msg;      /* the controller's warning just before its reset banner */
    volatile int reset_alarm;   /* and ALARM:3 between the warning and the banner */
    volatile int accepts;       /* connections taken */
    volatile int reset_on;      /* the connection (by accepts) the last soft reset came on */
} mock;

/* The controller's banner, as the driver's core writes it (report.c,
 * COMPATIBILITY_LEVEL 0), and as a Grbl-compatible build writes it. */
#define BANNER_GRBLHAL "\r\nGrblHAL 1.1f ['$' or '$HELP' for help]\r\n"
#define BANNER_GRBL    "\r\nGrbl 1.1f ['$' for help]\r\n"
#define BUSY_WORDS     "[MSG:The machine is busy with another sender's job: connect again once it is idle]\r\n"

static void *mock_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int c = accept(mock.listen_fd, NULL, NULL);
        if (c < 0)
            break;
        int conn = ++mock.accepts;
        if (mock.refuse) {
            /* The driver's refusal: its words, then the close, nothing read. */
            mock.refuse--;
            if (write(c, BUSY_WORDS, strlen(BUSY_WORDS)) < 0) { }
            msleep(50);
            char tmp[256];
            ssize_t k = recv(c, tmp, sizeof(tmp), MSG_DONTWAIT);
            if (k > 0 && memchr(tmp, 0x18, (size_t)k))
                mock.resets++;
            close(c);
            continue;
        }
        const char *banner = mock.grbl_banner ? BANNER_GRBL : BANNER_GRBLHAL;
        if (write(c, banner, strlen(banner)) < 0) { }
        if (mock.early_msg && write(c, mock.early_msg, strlen(mock.early_msg)) < 0) { }
        if (mock.stray_ok && write(c, "ok\r\n", 4) < 0) { }
        char buf[1024];
        size_t n = 0;
        int silent = 0, closing = 0;
        for (;;) {
            ssize_t r = read(c, buf + n, sizeof(buf) - 1 - n);
            if (r <= 0)
                break;
            n += (size_t)r;
            size_t i = 0;
            while (i < n) {
                if (buf[i] == 0x18) {
                    mock.resets++;
                    mock.reset_on = conn;
                    i++;
                    continue;
                }
                char *nl = memchr(buf + i, '\n', n - i);
                if (!nl)
                    break;
                *nl = '\0';
                if (mock.lines < 64)
                    snprintf(mock.seen[mock.lines], 80, "%s", buf + i);
                mock.lines++;
                if (mock.close_after && mock.lines >= mock.close_after) {
                    /* The close once the run has gone quiet: it has
                     * written every line the window takes and waits for
                     * their answers, and has read the welcome and the
                     * answers before this line. A close inside its burst
                     * of writes races the read of the welcome. */
                    mock.close_after = 0;
                    closing = 1;
                    struct pollfd pq = { .fd = c, .events = POLLIN };
                    char drop[512];
                    while (poll(&pq, 1, 100) > 0 && read(c, drop, sizeof(drop)) > 0) { }
                    break;
                }
                if (strstr(buf + i, "P77")) {
                    mock.holding = 1;
                    /* Let go by the test, or by an abort: its soft reset
                     * behind whatever lines were in flight, or the close.
                     * What waits behind the held line is the runner's
                     * window. */
                    while (!mock.release) {
                        char ahead[2048];
                        ssize_t k = recv(c, ahead, sizeof(ahead), MSG_DONTWAIT | MSG_PEEK);
                        if (k == 0 || (k > 0 && memchr(ahead, 0x18, (size_t)k)))
                            break;
                        size_t have = n - (size_t)(nl + 1 - buf);
                        int nls = 0;
                        for (size_t j = 0; j < have; j++)
                            nls += nl[1 + j] == '\n';
                        for (ssize_t j = 0; j < k; j++)
                            nls += ahead[j] == '\n';
                        if (k < 0)
                            k = 0;
                        if ((int)(have + (size_t)k) > mock.ahead_bytes)
                            mock.ahead_bytes = (int)(have + (size_t)k);
                        if (nls > mock.ahead_lines)
                            mock.ahead_lines = nls;
                        msleep(10);
                    }
                    mock.holding = 0;
                }
                const char *ans = strstr(buf + i, "BAD") ? "error:9\r\n"
                                : strstr(buf + i, "ALM") ? "ALARM:1\r\n" : "ok\r\n";
                if (mock.lit_after && mock.lines == mock.lit_after)
                    sysfs_write("pic/hv_current", 250);
                if (!silent && write(c, ans, strlen(ans)) < 0) { }
                if (!silent && mock.alarm_after && mock.lines == mock.alarm_after) {
                    msleep(300);        /* the ok read alone first */
                    if (write(c, "ALARM:1\r\n", 9) < 0) { }
                }
                if (!silent && mock.reset_after && mock.lines == mock.reset_after) {
                    /* A reset with no alarm, as the lid's cancel is: its
                     * warning if it has one, the banner again, and no
                     * status for anything in flight. */
                    if (mock.reset_msg && write(c, mock.reset_msg, strlen(mock.reset_msg)) < 0) { }
                    /* An arm the controller ends: the alarm, then the reset. */
                    if (mock.reset_alarm && write(c, "ALARM:3\r\n", 9) < 0) { }
                    if (write(c, banner, strlen(banner)) < 0) { }
                    silent = 1;
                }
                i = (size_t)(nl + 1 - buf);
            }
            if (closing)
                break;
            memmove(buf, buf + i, n - i);
            n -= i;
        }
        close(c);
    }
    return NULL;
}

static void mock_start(void)
{
    pthread_t th;
    mock.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(mock.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(mock.listen_fd, (struct sockaddr *)&sa, sizeof(sa));
    socklen_t sl = sizeof(sa);
    getsockname(mock.listen_fd, (struct sockaddr *)&sa, &sl);
    listen(mock.listen_fd, 4);
    char port[16];
    snprintf(port, sizeof(port), "%d", ntohs(sa.sin_port));
    setenv("GF_GRBL_PORT", port, 1);
    pthread_create(&th, NULL, mock_thread, NULL);
    pthread_detach(th);
}

static void mock_clear(void)
{
    msleep(50);                         /* a closed connection's last bytes land first */
    mock.lines = mock.resets = mock.holding = mock.release = mock.lit_after = mock.stray_ok = 0;
    mock.reset_after = mock.close_after = mock.ahead_bytes = mock.ahead_lines = 0;
    mock.alarm_after = mock.refuse = mock.grbl_banner = mock.reset_alarm = 0;
    mock.early_msg = mock.reset_msg = NULL;
}

static int wait_for(volatile int *flag, int want, int ms)
{
    for (int t = 0; t < ms; t += 10) {
        if (*flag == want)
            return 1;
        msleep(10);
    }
    return *flag == want;
}

/* ---- helpers ---- */

static void stage(const char *text, size_t len)
{
    FILE *f = fopen(staged, "wb");
    fwrite(text, 1, len, f);
    fclose(f);
}
#define STAGE(s) stage(s, sizeof(s) - 1)

static const char *status(void)
{
    static char body[768];
    if (jobrun_status_json(body, sizeof(body)) != 0)
        snprintf(body, sizeof(body), "(no record)");
    return body;
}

static int wait_state(const char *want, int ms)
{
    char key[32];
    snprintf(key, sizeof(key), "\"state\":\"%s\"", want);
    for (int t = 0; t < ms; t += 20) {
        if (strstr(status(), key))
            return 1;
        msleep(20);
    }
    return 0;
}

static int client_on;
static int client_connected(void)
{
    return client_on;
}

/* The Grbl socket's keep, as the controller port would answer it: a
 * machine that is not idle (a held job of a sender that left) refuses. */
static int out_refuse, out_calls, in_calls;
static int give_publishes;              /* the next give publishes a sender's alarm 3 (below) */
static void ctl_state(const char *state, int alarm, long gen);
static int sender_out(char *err, size_t elen)
{
    out_calls++;
    if (out_refuse)
        snprintf(err, elen, "the machine is not idle: a program, a jog, a hold, an armed laser, or an alarm");
    return out_refuse ? -1 : 0;
}
static void sender_in(void)
{
    in_calls++;
    /* A sender that connects the moment the socket is given back, stops
     * its own motion with ^X and leaves: alarm 3, no sender, a newer
     * generation. */
    if (give_publishes) {
        give_publishes = 0;
        ctl_state("Alarm", 3, 9);
    }
}

/* The running controller, as the supervisor names it (its pid). */
static volatile long ctl_pid = 4001;
static long test_controller(void)
{
    return ctl_pid;
}

/* The driver's count of the alarm-3 raises it made itself (its emission
 * witness), as every document below carries it. */
static volatile long ctl_alarm3 = 0;

/* The controller's state file as the driver publishes it: its state, its
 * alarm code, whether a sender is on the socket, the sender generation. */
static void ctl_doc(const char *state, int alarm, long gen, int connected)
{
    char path[256], tmp[272];
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(path, sizeof(path), "%s/run/grbl.state", root);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    char g[48] = "", a3[40] = "";
    if (gen >= 0)                       /* a document with no generation: gen below 0 */
        snprintf(g, sizeof(g), ",\"generation\":%ld", gen);
    if (ctl_alarm3 >= 0)                /* a driver that publishes no count: below 0 */
        snprintf(a3, sizeof(a3), "\"alarm3_raised\":%ld,", (long)ctl_alarm3);
    fprintf(f, "{\"ts_mono\":%.3f,\"state\":\"%s\",\"alarm\":%d,%s"
               "\"sender\":{\"connected\":%s%s,\"for_s\":0,\"peer\":\"\"},"
               "\"laser\":{\"armed\":false,\"arming\":false}}\n",
            (double)ts.tv_sec + ts.tv_nsec / 1e9, state, alarm, a3,
            connected ? "true" : "false", g);
    fclose(f);
    rename(tmp, path);
}

/* The same with no sender on the socket. */
static void ctl_state(const char *state, int alarm, long gen)
{
    ctl_doc(state, alarm, gen, 0);
}

/* The document that shows the stop comes late, while the runner reads the
 * state after its soft reset (a restart: a new pid first). */
static void *stop_shown_late(void *restart)
{
    wait_for(&mock.holding, 1, 3000);   /* the run plays */
    wait_for(&mock.resets, 1, 3000);    /* its soft reset: the state's read comes next */
    msleep(100);
    if (restart)
        ctl_pid++;
    ctl_state("Alarm", 3, 7);
    return NULL;
}

/* The controller port's hook, as main.c wires it. */
static void (*port_touched)(void);

static void ctl_state_gone(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/run/grbl.state", root);
    unlink(path);
}

/* ---- the cases ---- */

static void offense(const char *text, size_t len, const char *line, const char *words)
{
    char err[160] = "";
    int n = -1;
    stage(text, len);
    int rc = jobrun_program_check(staged, &n, err, sizeof(err));
    CHECK(rc == -1 && strstr(err, line) && strstr(err, words), "wanted \"%s ... %s\", got rc %d \"%s\"",
          line, words, rc, err);
}
#define OFFENSE(s, line, words) offense(s, sizeof(s) - 1, line, words)

static void test_check(void)
{
    char err[160] = "";
    int n = -1;
    STAGE("%\r\n(a header! with ~ and ?)\r\nG21 ; metric?\r\n\r\n  G90\t\r\nG1 X10 (in) F600\n%\n");
    CHECK(jobrun_program_check(staged, &n, err, sizeof(err)) == 0 && n == 3, "clean program: n %d \"%s\"", n, err);

    OFFENSE("G21\n$X\nG90\n", "line 2", "$ command");
    OFFENSE("G21\n  $H\n", "line 2", "$ command");
    OFFENSE("G1 X1\nG1 X2!\n", "line 2", "realtime");
    OFFENSE("G1 X1 ?\n", "line 1", "realtime");
    OFFENSE("G21\nG1 X\x9e" "1\n", "line 2", "printable");
    OFFENSE("G21\n\x18\n", "line 2", "printable");
    OFFENSE("G21\nG1\0X1\n", "line 2", "printable");
    OFFENSE("G21\nG1 X1\rG1 X2\n", "line 2", "carriage return");
    OFFENSE("(only a comment)\n\n%\n", "the program", "no lines");

    /* The limit is 250 characters of what goes out: trailing spaces are
     * trimmed first, and a 251st character is refused. */
    char edge[300];
    memset(edge, '1', sizeof(edge));
    memcpy(edge, "G1 X", 4);
    edge[251] = '\n';
    offense(edge, 252, "line 1", "longer");
    memset(edge + 250, ' ', 5);
    edge[255] = '\n';
    stage(edge, 256);
    CHECK(jobrun_program_check(staged, &n, err, sizeof(err)) == 0 && n == 1,
          "250 characters and trailing spaces: n %d \"%s\"", n, err);

    char longline[400];
    memset(longline, 'X', sizeof(longline));
    memcpy(longline, "G1 ", 3);
    longline[sizeof(longline) - 1] = '\n';
    offense(longline, sizeof(longline), "line 1", "longer");
    /* A long comment is no offense: it never goes out. */
    memcpy(longline, "G1(", 3);
    longline[sizeof(longline) - 2] = ')';
    stage(longline, sizeof(longline));
    CHECK(jobrun_program_check(staged, &n, err, sizeof(err)) == 0 && n == 1, "a long comment: n %d \"%s\"", n, err);

    /* A package's program: the words that change what the controller
     * keeps, read as its parser reads them, with the tray's mode in force
     * (0 in, 1 out, -1 not readable). */
    static const struct { const char *line; int tray; const char *word; } kept[] = {
        { "G10 L2 P1 X0", 0, "G10" }, { "g 1 0 l20 p1 x0", 0, "G10" }, { "G010L2P1", 0, "G10" },
        { "G00000000010 L2", 0, "G10" }, { "G10.0 L2", 0, "G10" }, { "N20 G0 X1 G92 X0", 0, "G92" },
        { "G92.1", 0, "G92" }, { "G92.2", 0, "G92" }, { "g 9 2 . 3", 0, "G92" }, { "G+92 X0", 0, "G92" },
        { "G28.1", 0, "G28.1" }, { "G28.10", 0, "G28.1" }, { "G30.1", 0, "G30.1" },
        { "G43 H1", 0, "G43" }, { "g43.1x5", 0, "G43" }, { "G43.10 Z1", 0, "G43" }, { "G43.2 H1", 0, "G43" },
        { "G 5 1 X2", 0, "G51" }, { "G49", 0, NULL }, { "G50", 0, NULL },
        { "G20", 0, "G20" }, { "g 2 0 G0 X1", 0, "G20" }, { "G20.0", 0, "G20" }, { "G21", 0, NULL },
        { "G28", 0, NULL }, { "G30 X0", 0, NULL }, { "G1 X10 Y92 F1092", 0, NULL },
        { "G100", 0, NULL }, { "M3 S103", 0, NULL },
        /* M103: the mode in force passes, any other is refused. */
        { "M103 P0", 0, NULL }, { "M103 P1", 1, NULL }, { "m 1 0 3 p 0 . 0", 0, NULL }, { "M+103 P1", 1, NULL },
        { "m0103p1", 0, "M103" }, { "M+103 P1", 0, "M103" }, { "M103 P0", 1, "M103" }, { "M103", 0, "M103" }, { "M103", 1, "M103" },
        { "M103 P2", 0, "M103" }, { "M103 P0.5", 0, "M103" }, { "M103 P-1", 1, "M103" },
        { "M103 P0", -1, "M103" }, { "M103 P1", -1, "M103" }, { "M103.1 P1", 1, "M103" },
        { "M103 P1 P1", 1, "M103" },
    };
    for (size_t i = 0; i < sizeof(kept) / sizeof(*kept); i++) {
        const char *got = jobrun_line_kept_word(kept[i].line, kept[i].tray);
        CHECK(kept[i].word ? got && !strcmp(got, kept[i].word) : !got, "\"%s\" (tray %d) read as %s, not %s",
              kept[i].line, kept[i].tray, got ? got : "nothing kept", kept[i].word ? kept[i].word : "nothing kept");
    }
    STAGE("G21\n(G92 X0) G1 X1 ; G10 L2\nG91 G0 X1\n");
    CHECK(jobrun_program_check_package(staged, 0, err, sizeof(err)) == 0, "the words in comments refused: \"%s\"", err);
    STAGE("G21\nG1 X1\nG92 X0\n");
    CHECK(jobrun_program_check_package(staged, 0, err, sizeof(err)) == -1 && strstr(err, "line 3") &&
          strstr(err, "G92"), "a package's G92 on line 3: \"%s\"", err);
    STAGE("; a profile's program\nG21 G90 G94 M5\nM103 P1\nM4 S0\n");
    CHECK(jobrun_program_check_package(staged, 1, err, sizeof(err)) == 0, "the tray's mode in force refused: \"%s\"", err);
    CHECK(jobrun_program_check_package(staged, 0, err, sizeof(err)) == -1 && strstr(err, "line 3") &&
          strstr(err, "M103"), "the other tray mode on line 3: \"%s\"", err);

    CHECK(jobrun_name_ok("panel") == 0 && jobrun_name_ok("a.b_c-9") == 0 &&
          jobrun_name_ok("org.example.a-package-id-of-sixty-three-characters-at-the-bound") == 0,
          "good names refused (a package id of 63 characters is one)");
    CHECK(jobrun_name_ok("") != 0 && jobrun_name_ok("a b") != 0 && jobrun_name_ok("a\"b") != 0 &&
          jobrun_name_ok("0123456789012345678901234567890123456789012345678901234567890123") != 0,
          "a bad name passed");
}

static void test_program_plays(void)
{
    char err[160] = "", who[64] = "", lease[512];
    mock_clear();
    STAGE("(header)\nG21\nG4 P77 ; held\nG1 X5 F600\n");
    CHECK(jobrun_program_start(staged, "test", NULL, 0, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(access(staged, F_OK) != 0, "the staged file kept its name under a job that plays");
    CHECK(wait_for(&mock.holding, 1, 3000), "the held line never arrived");

    CHECK(lease_holder(who, sizeof(who)) && !strcmp(who, "job:test"), "the holder is \"%s\"", who);
    lease_json(lease, sizeof(lease));
    CHECK(strstr(lease, "\"kind\":\"sender\"") && strstr(lease, "a job (test)"), "lease: %s", lease);
    const char *st = status();
    CHECK(strstr(st, "\"state\":\"running\"") && strstr(st, "\"owner\":\"job:test\"") &&
          strstr(st, "\"program\":true") && strstr(st, "\"lines\":3"), "record while it plays: %s", st);

    STAGE("G21\n");
    CHECK(jobrun_program_start(staged, "second", NULL, 0, 30, 0, err, sizeof(err)) == -1 &&
          strstr(err, "a job (test) holds the machine"), "a second program: \"%s\"", err);
    CHECK(access(staged, F_OK) != 0, "a refused program was left staged");
    CHECK(lease_take("wizard:motion", LEASE_HARDWARE, NULL, err, sizeof(err)) == -1 &&
          strstr(err, "a job (test) holds the machine"), "a wizard beside the job: \"%s\"", err);

    mock.release = 1;
    CHECK(wait_state("done", 5000), "the job did not end: %s", status());
    CHECK(!lease_holder(who, sizeof(who)), "the lease is still \"%s\"", who);
    msleep(200);
    CHECK(mock.lines == 4, "the controller saw %d lines", mock.lines);
    CHECK(!strcmp(mock.seen[0], "G21") && !strcmp(mock.seen[1], "G4 P77") &&
          !strcmp(mock.seen[2], "G1 X5 F600") && !strcmp(mock.seen[3], "M2"),
          "the lines out: \"%s\" \"%s\" \"%s\" \"%s\"", mock.seen[0], mock.seen[1], mock.seen[2], mock.seen[3]);
    CHECK(mock.resets == 0, "a soft reset on a clean run");
    st = status();
    CHECK(strstr(st, "\"sent\":4") && strstr(st, "\"acked\":4") && strstr(st, "\"lit\":false") &&
          strstr(st, "\"reason\":\"\""), "record at the end: %s", st);
}

static void test_own_m2(void)
{
    char err[160] = "";
    mock_clear();
    STAGE("G21\nM5\nM2\n");
    CHECK(jobrun_program_start(staged, "ends", NULL, 0, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_state("done", 5000), "the job did not end: %s", status());
    msleep(200);
    CHECK(mock.lines == 3 && !strcmp(mock.seen[2], "M2"), "%d lines, the last \"%s\"", mock.lines, mock.seen[2]);
}

/* The $X clears the alarm the runner's own soft reset left: after a
 * clean run it does not go out, nor after a run the controller stopped
 * with its alarm lock (error:9); after an abort it goes out first. */
static void unlock_run(const char *name, int want_x)
{
    char err[160] = "";
    mock_clear();
    STAGE("G21\n");
    CHECK(jobrun_program_start(staged, name, NULL, 0, 30, 1, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_state("done", 5000), "the job did not end: %s", status());
    if (want_x)
        CHECK(mock.lines == 3 && !strcmp(mock.seen[0], "$X") && !strcmp(mock.seen[1], "G21") &&
              !strcmp(mock.seen[2], "M2"), "%s: %d lines: \"%s\" \"%s\" \"%s\"", name, mock.lines, mock.seen[0],
              mock.seen[1], mock.seen[2]);
    else
        CHECK(mock.lines == 2 && !strcmp(mock.seen[0], "G21") && !strcmp(mock.seen[1], "M2"),
              "%s: the $X went out: %d lines, the first \"%s\"", name, mock.lines, mock.seen[0]);
}

/* A run that ends failed, from a mock set up by the caller. */
static void failed_run(const char *name, const char *text)
{
    char err[160] = "";
    stage(text, strlen(text));
    CHECK(jobrun_program_start(staged, name, NULL, 0, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_state("failed", 5000), "%s did not fail: %s", name, status());
}

/* The runner's own stop: a run aborted mid-move (the controller took its
 * first line), or one aborted at its first line, which the controller
 * has not answered. */
static void (*before_abort)(void);     /* what the controller does just before the runner's abort */

static void aborted_run_of(const char *text, int acked)
{
    char err[160] = "", key[24];
    mock_clear();
    stage(text, strlen(text));
    CHECK(jobrun_program_start(staged, "stopped", NULL, 0, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_for(&mock.holding, 1, 3000), "the held line never arrived");
    snprintf(key, sizeof(key), "\"acked\":%d,", acked);
    for (int t = 0; t < 3000 && !strstr(status(), key); t += 10)
        msleep(10);                     /* the runner has read the answers before the held line */
    if (before_abort)
        before_abort();
    CHECK(jobrun_program_abort(err, sizeof(err)) == 0, "abort: \"%s\"", err);
}

/* The driver's emission witness raises its alarm 3 (its count moves on),
 * and the document shows it with no sender on the socket. */
static void witness_raises(void)
{
    ctl_alarm3++;
    ctl_state("Alarm", 3, 7);
}

/* The same inside the end's read, after the runner's soft reset. */
static void *witness_in_the_read(void *unused)
{
    (void)unused;
    wait_for(&mock.holding, 1, 3000);
    wait_for(&mock.resets, 1, 3000);
    msleep(100);
    witness_raises();
    return NULL;
}

static void aborted_run(void)
{
    aborted_run_of("G21\nG4 P77\n", 1);
}

/* The $X goes out only where the runner's own stop stands: the last run
 * ended with the runner's soft reset, and the controller still reads the
 * alarm 3 that stop left, with the sender generation it showed after it,
 * from the same controller. */
static void test_unlock(void)
{
    /* What the controller shows after a soft reset in motion. */
    ctl_state("Alarm", 3, 7);
    /* Unwired, the runner knows no controller: no stop is its own. Wired
     * as main.c wires it: the controller's identity, and the port's hook
     * that forgets the stop. */
    aborted_run();
    unlock_run("not-wired", 0);
    jobrun_wire(test_controller, &port_touched);
    CHECK(port_touched == jobrun_forget_stop, "the port's hook is not the runner's forget");
    unlock_run("after-a-clean-run", 0);
    /* No stop of the runner's, and a document that names no generation:
     * nothing to match. */
    ctl_state("Alarm", 3, -1);
    unlock_run("no-stop-no-generation", 0);
    ctl_state("Alarm", 3, 7);

    /* The stop is not the runner's: the controller's alarm lock, its
     * alarm, its own reset. */
    mock_clear();
    failed_run("locked", "G21\nBAD\n");
    unlock_run("after-the-alarm-lock", 0);
    mock_clear();
    failed_run("alarmed", "G21\nALM\n");
    unlock_run("after-the-controller-alarm", 0);
    mock_clear();
    mock.reset_after = 1;
    failed_run("reset", "G21\nG1 X5 F600\nG1 X10 F600\n");
    unlock_run("after-the-controller-reset", 0);

    /* The runner's own: an abort, and a lost connection's reset. */
    aborted_run();
    unlock_run("after-an-abort", 1);
    mock_clear();
    mock.close_after = 2;
    failed_run("dropped", "G21\nG1 X5 F600\nG1 X10 F600\nG1 X15 F600\n");
    CHECK(wait_for(&mock.resets, 1, 2000), "the lost connection's soft reset never came (%d)", mock.resets);
    unlock_run("after-a-lost-connection", 1);

    /* The stop shown late in the read: the document still shows the
     * connection when the read begins. */
    ctl_doc("Alarm", 3, 7, 1);
    {
        pthread_t t;
        pthread_create(&t, NULL, stop_shown_late, NULL);
        aborted_run();
        pthread_join(t, NULL);
    }
    unlock_run("stop-shown-late", 1);

    /* An alarm 3 that stood before the run (another sender's stop, the
     * driver's own): a run aborted at its first line, which the
     * controller has not answered, and its reset at rest keeps that
     * alarm. Not the runner's. */
    ctl_state("Alarm", 3, 7);
    aborted_run_of("G4 P77\n", 0);
    wait_state("failed", 3000);
    unlock_run("alarm-before-the-run", 0);

    /* The stop leaves no mark the controller did not show: a stop at rest
     * (the state reads Idle at the end) and then the driver's own alarm 3;
     * another alarm at the stop (a soft limit's) and then alarm 3; a
     * document that still shows a connection through the whole read; the
     * controller restarting inside the read. */
    ctl_state("Idle", 0, 7);
    aborted_run();
    wait_state("failed", 3000);
    ctl_state("Alarm", 3, 7);
    unlock_run("stop-at-rest-then-alarm3", 0);
    ctl_state("Alarm", 2, 7);
    aborted_run();
    wait_state("failed", 3000);
    ctl_state("Alarm", 3, 7);
    unlock_run("another-alarm-at-the-stop", 0);
    ctl_state("Idle", 0, 7);            /* a stop never seen leaves no mark, not one of no generation */
    aborted_run();
    wait_state("failed", 3000);
    ctl_state("Alarm", 3, -1);
    unlock_run("unseen-stop-then-no-generation", 0);
    ctl_doc("Alarm", 3, 7, 1);
    aborted_run();
    wait_state("failed", 3000);
    ctl_state("Alarm", 3, 7);
    unlock_run("still-connected-at-the-stop", 0);
    /* The stop is read while the run still keeps the socket: a sender's
     * alarm 3 shown only once the socket is given back is not the
     * runner's. */
    ctl_doc("Alarm", 3, 7, 1);
    give_publishes = 1;
    aborted_run();
    wait_state("failed", 3000);
    unlock_run("a-sender-after-the-give", 0);
    ctl_doc("Alarm", 3, 7, 1);
    {
        pthread_t t;
        static int restart = 1;
        pthread_create(&t, NULL, stop_shown_late, &restart);
        aborted_run();
        pthread_join(t, NULL);
        wait_state("failed", 3000);
    }
    unlock_run("controller-restart-in-the-read", 0);

    /* The runner's stop, and since then something else: an alarm of its
     * own (a soft limit), the state cleared or not Alarm, a home, release
     * or energize from this daemon and then a fresh alarm 3, another
     * sender's session, a controller started since, a state nobody can
     * read. */
    aborted_run();
    ctl_state("Alarm", 2, 7);
    unlock_run("a-limit-alarm-since", 0);
    ctl_state("Alarm", 3, 7);
    aborted_run();
    ctl_state("Idle", 0, 7);
    unlock_run("cleared-since", 0);
    ctl_state("Alarm", 3, 7);
    aborted_run();
    ctl_state("Idle", 3, 7);
    unlock_run("alarm-code-not-in-alarm", 0);
    ctl_state("Alarm", 3, 7);
    aborted_run();
    if (port_touched)
        port_touched();                 /* the port's home: the hook as wired */
    unlock_run("a-home-then-alarm3", 0);
    aborted_run();
    ctl_state("Alarm", 3, 9);
    unlock_run("a-session-since", 0);
    ctl_state("Alarm", 3, 7);
    aborted_run();
    ctl_pid++;
    unlock_run("a-controller-since", 0);
    aborted_run();
    ctl_state_gone();
    unlock_run("no-state", 0);

    /* The driver's own alarm 3 (its emission witness) reads exactly like
     * the runner's: raised while the runner's stands, under the run in the
     * tick the runner aborts it, or inside the end's read after a stop at
     * rest. Its count moved, so none is the runner's to clear. */
    ctl_state("Alarm", 3, 7);
    aborted_run();
    witness_raises();
    unlock_run("a-witness-alarm-while-it-stands", 0);
    ctl_state("Alarm", 3, 7);
    before_abort = witness_raises;
    aborted_run();
    before_abort = NULL;
    wait_state("failed", 3000);
    unlock_run("a-witness-alarm-under-the-run", 0);
    ctl_state("Idle", 0, 7);
    {
        pthread_t t;
        pthread_create(&t, NULL, witness_in_the_read, NULL);
        aborted_run();
        pthread_join(t, NULL);
        wait_state("failed", 3000);
    }
    unlock_run("a-witness-alarm-in-the-read", 0);
    /* And with the count where the run began, the runner's own stop still
     * clears. */
    ctl_state("Alarm", 3, 7);
    aborted_run();
    unlock_run("the-count-unmoved", 1);
    /* A driver that publishes no count marks no stop: no $X. */
    long kept = ctl_alarm3;
    ctl_alarm3 = -1;
    ctl_state("Alarm", 3, 7);
    aborted_run();
    unlock_run("no-count-no-unlock", 0);
    ctl_alarm3 = kept;
    ctl_state("Alarm", 3, 7);
}

/* A reset the controller reports (its banner a second time, as the lid's
 * cancel sends it) ends the run at once, failed, and the runner sends no
 * soft reset of its own: it would cut short the cancel's return move.
 * The banner in either form; the record gives the controller's warning
 * just before it (not its information or debug lines after the warning),
 * and a warning answered by an ok since is not why. An arm the controller
 * ends reports its alarm before the reset: the run fails at the alarm,
 * with the warning in the record. */
static void test_reset_banner(void)
{
    char err[160] = "", who[64];
    for (int form = 0; form < 2; form++) {
        mock_clear();
        mock.reset_after = 1;
        mock.grbl_banner = form;
        mock.reset_msg = form ? NULL : "[MSG:Warning: lid opened - job canceled, returning to the job start]\r\n"
                                       "[MSG:Info: laser disarmed - latch locked]\r\n[MSG:Debug: reset]\r\n";
        mock.early_msg = form ? "[MSG:Warning: laser floor is 0]\r\n" : NULL;
        STAGE("G21\nG1 X5 F600\nG1 X10 F600\nG1 X15 F600\n");
        CHECK(jobrun_program_start(staged, "lid", NULL, 0, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
        CHECK(wait_state("failed", 3000), "the run went on after the controller was reset (%s banner): %s",
              form ? "Grbl" : "GrblHAL", status());
        if (form)
            CHECK(strstr(status(), "reset") && !strstr(status(), "floor"), "record: %s", status());
        else
            CHECK(strstr(status(), "lid opened - job canceled"), "record: %s", status());
        CHECK(!lease_holder(who, sizeof(who)), "the lease is still \"%s\"", who);
        msleep(200);
        CHECK(mock.resets == 0, "%d soft resets after the controller's own", mock.resets);
    }
    mock_clear();
    mock.reset_after = 1;
    mock.reset_alarm = 1;
    mock.reset_msg = "[MSG:Warning: laser arm timed out waiting for the button]\r\n";
    STAGE("G21\nM3 S400\nG1 X5 F600\n");
    CHECK(jobrun_program_start(staged, "arm", NULL, 0, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_state("failed", 3000), "the run went on after the arm ended: %s", status());
    CHECK(strstr(status(), "ALARM:3") && strstr(status(), "laser arm timed out"), "record: %s", status());
}

/* The controller turns the run's connection away at the accept: the run
 * fails in those words, and no soft reset goes out (the run never had the
 * machine), nor is the stop the runner's. */
static void test_refused_connect(void)
{
    mock_clear();
    ctl_state("Alarm", 3, 7);
    mock.refuse = 2;
    failed_run("refused", "G21\nG1 X5 F600\n");
    CHECK(strstr(status(), "refused") && strstr(status(), "busy with another sender"), "record: %s", status());
    msleep(300);
    CHECK(mock.resets == 0, "%d soft resets for a connection the run never had", mock.resets);
    unlock_run("after-a-refused-connection", 0);
    ctl_state_gone();
}

/* The controller closes the run's connection with lines out: the run
 * fails, and its soft reset goes on a connection of its own, so what is
 * planned stops all the same. */
static void test_midrun_close(void)
{
    char err[160] = "";
    mock_clear();
    mock.close_after = 2;
    STAGE("G21\nG1 X5 F600\nG1 X10 F600\nG1 X15 F600\nG1 X20 F600\n");
    CHECK(jobrun_program_start(staged, "dropped", NULL, 0, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_state("failed", 3000), "the run did not fail on the close: %s", status());
    CHECK(wait_for(&mock.resets, 1, 2000), "the soft reset never reached the controller (%d)", mock.resets);

    /* A write the closed connection refuses: the same, the soft reset on
     * a connection of its own, not on the one that refused the write. */
    mock_clear();
    int conn = mock.accepts;
    fail_write_text = "M2";             /* the runner's own end, written once the welcome is read */
    STAGE("G21\nG1 X5 F600\n");
    CHECK(jobrun_program_start(staged, "unwritten", NULL, 0, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_state("failed", 3000), "the run did not fail on the refused write: %s", status());
    CHECK(wait_for(&mock.resets, 1, 2000) && mock.reset_on == conn + 2,
          "after a refused write the soft reset came on connection %d, not on a new one (2), %d resets",
          mock.reset_on - conn, mock.resets);
    fail_write_text = NULL;
}

/* The window the runner keeps: lines out and not acknowledged are held
 * to half the controller's 1024-byte ring and to 32 lines. */
static void inflight_case(const char *line, int nlines)
{
    char err[160] = "", prog[4096];
    size_t o = (size_t)snprintf(prog, sizeof(prog), "G4 P77\n");
    for (int i = 0; i < nlines; i++)
        o += (size_t)snprintf(prog + o, sizeof(prog) - o, "%s\n", line);
    mock_clear();
    stage(prog, o);
    CHECK(jobrun_program_start(staged, "window", NULL, 0, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_for(&mock.holding, 1, 3000), "the held line never arrived");
    msleep(300);
    mock.release = 1;
    CHECK(wait_state("done", 5000), "the job did not end: %s", status());
    printf("    %s: %d bytes in %d lines behind the held one\n", line, mock.ahead_bytes, mock.ahead_lines);
    CHECK(mock.ahead_bytes + 7 <= 512, "%d bytes out past half the ring", mock.ahead_bytes + 7);
    CHECK(mock.ahead_lines + 1 <= 32, "%d lines out past the bound", mock.ahead_lines + 1);
}

static void test_inflight_bound(void)
{
    inflight_case("G1 X1.000 Y1.000 F600 ; long", 60);     /* the bytes bind */
    inflight_case("G0 X1", 60);                              /* the lines bind */
}

/* The controller's alarm stops the run at the line that drew it. */
static void test_alarm_stops(void)
{
    mock_clear();
    jobstream_lines_t lines = { "G21\nALM\nG90\n", 0 };
    jobstream_cfg_t cfg = { .gen = jobstream_lines_gen, .ctx = &lines, .run_timeout_s = 10 };
    jobstream_run_t run;
    char err[160] = "";
    int rc = jobstream_run(&cfg, &run, err, sizeof(err));
    CHECK(rc == -1 && strstr(err, "ALARM:1") && strstr(err, "line 2"), "the alarm: rc %d, %s", rc, err);
}

/* A generator line longer than the buffer fails the run: never split. */
static void test_lines_gen_long(void)
{
    char buf[8];
    jobstream_lines_t l = { "G1 X123456789\nG0\n", 0 };
    CHECK(jobstream_lines_gen(&l, buf, sizeof(buf)) == -1, "an over-long line was split, not refused");
}

static void kernel_state(const char *word)
{
    char path[192], tmp[200];
    snprintf(path, sizeof(path), "%s/cnc/state", root);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    fprintf(f, "%s\n", word);
    fclose(f);
    rename(tmp, path);
}

/* Every line is acknowledged while the kernel still plays the last of
 * the ring: the record must not say done until it is idle. A state that
 * cannot be read is not idle either. A fault or an underrun has nothing
 * left to play: the job is done at once. */
static void test_done_is_idle(void)
{
    char err[160] = "", who[64], path[192];
    mock_clear();
    snprintf(path, sizeof(path), "%s/cnc/state", root);
    unlink(path);
    STAGE("G21\nG1 X5 F600\n");
    CHECK(jobrun_program_start(staged, "tail", NULL, 0, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
    msleep(600);
    CHECK(strstr(status(), "\"acked\":3") && strstr(status(), "\"state\":\"running\""),
          "with the kernel's state unreadable: %s", status());
    kernel_state("running");
    msleep(400);
    CHECK(strstr(status(), "\"state\":\"running\""), "with the kernel still playing: %s", status());
    CHECK(lease_holder(who, sizeof(who)), "the lease went back while the kernel played");
    kernel_state("idle");
    CHECK(wait_state("done", 2000), "the job did not end once the kernel was idle: %s", status());

    static const char *const ended[] = { "underrun", "fault" };
    for (int i = 0; i < 2; i++) {
        mock_clear();
        kernel_state(ended[i]);
        STAGE("G21\n");
        CHECK(jobrun_program_start(staged, ended[i], NULL, 0, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
        CHECK(wait_state("done", 2000), "a kernel in %s left the job waiting: %s", ended[i], status());
    }
    kernel_state("idle");
}

static int raced_rc = -1;
static void start_panel(void)
{
    char e[160] = "";
    raced_rc = jobrun_program_start(staged, "panel", NULL, 0, 30, 0, e, sizeof(e));
    if (raced_rc != 0)
        printf("the panel's start: %s\n", e);
}

static void test_abort(void)
{
    char err[160] = "", who[64];
    mock_clear();
    CHECK(jobrun_program_abort(err, sizeof(err)) == -1 && strstr(err, "no job"), "abort of nothing: \"%s\"", err);
    /* The operator's job, started between a package's abort's read of the
     * runner (nothing running) and what the abort does next: it runs on. */
    STAGE("G21\nG4 P77\nG1 X5 F600\n");
    race_thread = pthread_self();
    race_start = start_panel;
    int rc = jobrun_program_abort_of("stopme", err, sizeof(err));
    race_start = NULL;
    CHECK(raced_rc == 0 && rc == -1 && strstr(status(), "\"state\":\"running\"") &&
          strstr(status(), "\"owner\":\"job:panel\""),
          "a job started after a package's abort read the runner: started %d, the abort %d, %s", raced_rc, rc, status());
    CHECK(wait_for(&mock.holding, 1, 3000), "the operator's held line never arrived");
    CHECK(jobrun_program_abort(err, sizeof(err)) == 0, "the operator's abort: \"%s\"", err);
    mock_clear();
    STAGE("G21\nG4 P77\nG1 X5 F600\n");
    CHECK(jobrun_program_start(staged, "stopme", NULL, 0, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_for(&mock.holding, 1, 3000), "the held line never arrived");
    /* A package ends its own job and no one else's. */
    CHECK(jobrun_program_abort_of("another", err, sizeof(err)) == -1 &&
          lease_holder(who, sizeof(who)) && !strcmp(who, "job:stopme"),
          "another package's abort: \"%s\", the lease \"%s\"", err, who);
    CHECK(jobrun_program_abort_of("stopme", err, sizeof(err)) == 0, "abort: \"%s\"", err);
    /* The abort waits for the run: the record and the lease are final here. */
    const char *st = status();
    CHECK(strstr(st, "\"state\":\"failed\"") && strstr(st, "aborted"), "record after the abort: %s", st);
    CHECK(!lease_holder(who, sizeof(who)), "the lease is still \"%s\"", who);
    msleep(200);
    CHECK(mock.resets == 1, "%d soft resets", mock.resets);
    /* The three lines were in flight behind the held one; the runner's
     * M2 is a barrier and never went out. */
    CHECK(mock.lines == 3 && strcmp(mock.seen[2], "M2"), "%d lines went out, the last \"%s\"",
          mock.lines, mock.seen[2]);
}

/* A package's M103 is judged again as it goes out, against the tray's
 * mode then: the other mode ends the run before the line is sent; the
 * mode it gives goes out. An operator's program is not judged so. */
static int stub_tray = -1;
static int tray_stub(void)
{
    return stub_tray;
}

static int seen_word(const char *word)
{
    for (int i = 0; i < mock.lines && i < 64; i++)
        if (strstr(mock.seen[i], word))
            return 1;
    return 0;
}

static void test_package_tray(void)
{
    char err[160] = "";
    mock_clear();
    stub_tray = 1;
    STAGE("G21\nM103 P0\nG1 X5 F600\n");
    CHECK(jobrun_program_start_as(staged, "org.example.p", NULL, 0, 30, 0, 1, err, sizeof(err)) == 0,
          "start: %s", err);
    CHECK(wait_state("failed", 3000) && strstr(status(), "M103"), "a package's M103 for the other mode: %s",
          status());
    msleep(200);
    CHECK(!seen_word("M103") && !seen_word("X5"), "the line went out after the tray's mode changed: %d lines, "
          "the last \"%s\"", mock.lines, mock.lines ? mock.seen[mock.lines - 1] : "");
    mock_clear();
    stub_tray = 0;
    STAGE("G21\nM103 P0\nG1 X5 F600\n");
    CHECK(jobrun_program_start_as(staged, "org.example.p", NULL, 0, 30, 0, 1, err, sizeof(err)) == 0,
          "start: %s", err);
    CHECK(wait_state("done", 5000) && seen_word("M103 P0"), "a package's M103 for the mode in force: %s",
          status());
    mock_clear();
    stub_tray = 1;
    STAGE("G21\nM103 P0\nG1 X5 F600\n");
    CHECK(jobrun_program_start(staged, "panel", NULL, 0, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_state("done", 5000) && seen_word("M103 P0"), "the operator's M103 judged as a package's: %s",
          status());
    stub_tray = -1;
}

static void test_client_refuses(void)
{
    char err[160] = "", who[64];
    mock_clear();
    client_on = 1;
    STAGE("G21\n");
    CHECK(jobrun_program_start(staged, "test", NULL, 0, 30, 0, err, sizeof(err)) == -1 &&
          strstr(err, "a sender is connected"), "with a client on the socket: \"%s\"", err);
    client_on = 0;
    msleep(200);
    CHECK(mock.lines == 0, "%d lines went out past the refusal", mock.lines);
    CHECK(!lease_holder(who, sizeof(who)), "the lease is \"%s\" after a refusal", who);
    CHECK(access(staged, F_OK) != 0, "a refused program was left staged");
    /* A refused start is nobody's job: the record is still the last one's. */
    CHECK(strstr(status(), "\"owner\":\"job:stopme\"") && strstr(status(), "aborted"),
          "the record after a refusal: %s", status());
}

/* A start keeps the Grbl socket to this host before a line goes out; a
 * machine the controller calls busy refuses it with nothing sent and
 * nothing to give back, and a run gives the socket back once at its end. */
static void test_sender_kept(void)
{
    char err[160] = "", who[64];
    mock_clear();
    out_refuse = 1;
    out_calls = in_calls = 0;
    STAGE("G21\n");
    CHECK(jobrun_program_start(staged, "held", NULL, 0, 30, 0, err, sizeof(err)) == -1 &&
          strstr(err, "not idle"), "a start the controller calls busy: \"%s\"", err);
    out_refuse = 0;
    msleep(200);
    CHECK(mock.lines == 0, "%d lines went out past the refusal", mock.lines);
    CHECK(!lease_holder(who, sizeof(who)), "the lease is \"%s\" after the refusal", who);
    CHECK(out_calls == 1 && in_calls == 0, "the refused keep: %d asked, %d given back", out_calls, in_calls);
    STAGE("G21\n");
    CHECK(jobrun_program_start(staged, "kept", NULL, 0, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_state("done", 5000), "the job did not end: %s", status());
    CHECK(out_calls == 2 && in_calls == 1, "a run: %d asked, %d given back", out_calls, in_calls);
}

static void *sync_thread(void *arg)
{
    int *rc = arg;
    static jobstream_lines_t lines;
    lines = (jobstream_lines_t){ "G21\nG4 P77\nM2\n", 0 };
    jobrun_cfg_t cfg = { .owner = "job:laser.focus", .under = "wizard:laser.focus",
                         .stream = { .gen = jobstream_lines_gen, .ctx = &lines, .run_timeout_s = 30 } };
    jobstream_run_t run;
    char err[160] = "";
    *rc = jobrun_sync(&cfg, &run, err, sizeof(err));
    if (*rc != 0)
        printf("sync: %s\n", err);
    return NULL;
}

static void test_sync_under_a_wizard(void)
{
    char err[160] = "", who[64] = "", lease[512];
    mock_clear();
    CHECK(lease_take("wizard:laser.focus", LEASE_HARDWARE, NULL, err, sizeof(err)) == 0, "the wizard: %s", err);

    /* Not under the wizard: refused in the wizard's words. */
    jobstream_lines_t lines = { "G21\n", 0 };
    jobrun_cfg_t bare = { .owner = "job:x", .stream = { .gen = jobstream_lines_gen, .ctx = &lines } };
    jobstream_run_t run;
    CHECK(jobrun_sync(&bare, &run, err, sizeof(err)) == -1 && strstr(err, "a setup wizard (laser.focus) holds"),
          "a bare job beside a wizard: \"%s\"", err);

    int rc = -2;
    pthread_t t;
    pthread_create(&t, NULL, sync_thread, &rc);
    CHECK(wait_for(&mock.holding, 1, 3000), "the held line never arrived");
    lease_json(lease, sizeof(lease));
    CHECK(strstr(lease, "\"owner\":\"job:laser.focus\"") && strstr(lease, "\"under\":\"wizard:laser.focus\""),
          "lease inside the wizard: %s", lease);
    CHECK(strstr(status(), "\"program\":false"), "record: %s", status());
    CHECK(jobrun_program_abort(err, sizeof(err)) == -1 && strstr(err, "stop it where it was started"),
          "the abort route on a wizard's job: \"%s\"", err);
    /* A package's abort ends its own posted program alone: a wizard's job
     * whose name is the package's is not one. */
    CHECK(jobrun_program_abort_of("laser.focus", err, sizeof(err)) == -1 && strstr(status(), "\"state\":\"running\""),
          "a package's abort on a wizard's job of its name: \"%s\", %s", err, status());
    mock.release = 1;
    pthread_join(t, NULL);
    CHECK(rc == 0, "the wizard's job: rc %d", rc);
    CHECK(lease_holder(who, sizeof(who)) && !strcmp(who, "wizard:laser.focus"), "back to \"%s\"", who);
    lease_release("wizard:laser.focus");
}

static int ready_fail, done_calls, done_rc, held_in_done, held_after_back;
static int on_ready(void *ctx, char *err, size_t elen)
{
    (void)ctx;
    if (ready_fail)
        snprintf(err, elen, "the keys are held");
    return ready_fail ? -1 : 0;
}
static void on_done(void *ctx, int rc, const jobstream_run_t *run, const char *err)
{
    char who[64];
    (void)ctx;
    (void)run;
    (void)err;
    done_calls++;
    done_rc = rc;
    held_in_done = lease_holder(who, sizeof(who)) && !strcmp(who, "recorder");
    jobrun_lease_back();
    held_after_back = lease_holder(who, sizeof(who));
}

static void test_hooks(void)
{
    char err[160] = "", who[64];
    static jobstream_lines_t lines;
    mock_clear();
    lines = (jobstream_lines_t){ "G21\nM2\n", 0 };
    jobrun_cfg_t cfg = { .owner = "recorder", .ready = on_ready, .done = on_done,
                         .stream = { .gen = jobstream_lines_gen, .ctx = &lines, .run_timeout_s = 30 } };
    ready_fail = 1;
    out_calls = in_calls = 0;
    CHECK(jobrun_start(&cfg, err, sizeof(err)) == -1 && strstr(err, "the keys are held"), "ready's refusal: \"%s\"", err);
    CHECK(!lease_holder(who, sizeof(who)), "the lease is \"%s\" after ready refused", who);
    CHECK(out_calls == 1 && in_calls == 1, "the socket after ready refused: %d kept, %d given back", out_calls, in_calls);
    CHECK(!strstr(status(), "\"owner\":\"recorder\""), "ready's refusal rewrote the record: %s", status());
    msleep(200);
    CHECK(mock.lines == 0 && done_calls == 0, "%d lines, %d done calls past the refusal", mock.lines, done_calls);

    ready_fail = 0;
    CHECK(jobrun_start(&cfg, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_state("done", 5000), "the run did not end: %s", status());
    CHECK(done_calls == 1 && done_rc == 0, "done: %d calls, rc %d", done_calls, done_rc);
    CHECK(held_in_done, "done ran without the lease");
    CHECK(!held_after_back, "the lease was still held after jobrun_lease_back()");
    jobrun_stop("recorder");            /* nothing to abort: only the join */

    /* The owner's stop aborts its own run and nobody else's. */
    mock_clear();
    lines = (jobstream_lines_t){ "G4 P77\nM2\n", 0 };
    done_calls = 0;
    CHECK(jobrun_start(&cfg, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_for(&mock.holding, 1, 3000), "the held line never arrived");
    jobrun_stop("job:somebody");
    CHECK(strstr(status(), "\"state\":\"running\""), "somebody else's stop ended it: %s", status());
    jobrun_stop("recorder");
    CHECK(done_calls == 1 && done_rc == -1, "done after the stop: %d calls, rc %d", done_calls, done_rc);
    CHECK(strstr(status(), "aborted"), "record: %s", status());
    msleep(200);
    CHECK(mock.resets == 1, "%d soft resets", mock.resets);
}

static void test_must_light(void)
{
    char err[160] = "";
    mock_clear();
    STAGE("M3 S100\nG1 X1 F600\nM5\n");
    CHECK(jobrun_program_start(staged, "dark", NULL, 5, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_state("failed", 8000), "a dark run passed as lit: %s", status());
    CHECK(strstr(status(), "without a discharge"), "record: %s", status());
    msleep(200);
    CHECK(mock.resets == 1, "%d soft resets", mock.resets);
}

/* A program that must light is witnessed every tick, with no clock: a
 * barrier line (M102) goes out alone once the one before it is
 * acknowledged, one per tick at most, so thirty of them are thirty
 * ticks at least, and the record's samples count them whatever the
 * host's load. */
#define RATE_LINES 30
static void test_lit_full_rate(void)
{
    char err[160] = "", prog[RATE_LINES * 5 + 1] = "";
    for (int i = 0; i < RATE_LINES; i++)
        strcat(prog, "M102\n");
    mock_clear();
    mock.lit_after = 1;                 /* the tube lights as line 1 is taken */
    stage(prog, strlen(prog));
    CHECK(jobrun_program_start(staged, "rate", NULL, 30, 30, 0, err, sizeof(err)) == 0, "start: %s", err);
    CHECK(wait_state("done", 8000), "the lit run did not end: %s", status());
    const char *k = strstr(status(), "\"samples\":");
    int n = k ? atoi(k + 10) : -1;
    printf("    %d samples over %d barrier lines\n", n, RATE_LINES);
    CHECK(n >= RATE_LINES, "a program that must light had %d samples over %d barrier lines", n, RATE_LINES);
    mock.lit_after = 0;
    sysfs_write("pic/hv_current", 0);
    msleep(200);
    CHECK(mock.resets == 0, "%d soft resets", mock.resets);
}

/* ---- the sender underneath ---- */

static int samples_seen;
static void on_sample(void *ctx, const jobstream_sample_t *s)
{
    (void)ctx;
    (void)s;
    samples_seen++;
}

static void test_dark_program(void)
{
    mock_clear();
    jobstream_lines_t lines = { "G21\nG90\n$100=53.4\n", 0 };
    jobstream_cfg_t cfg = { .gen = jobstream_lines_gen, .sample = on_sample, .ctx = &lines,
                            .run_timeout_s = 10 };
    jobstream_run_t run;
    char err[160] = "";
    int rc = jobstream_run(&cfg, &run, err, sizeof(err));
    msleep(200);
    CHECK(rc == 0, "dark program failed: %s", err);
    CHECK(run.sent == 3 && run.acked == 3, "sent %d acked %d", run.sent, run.acked);
    CHECK(mock.lines == 3, "the mock saw %d lines", mock.lines);
    CHECK(!strcmp(mock.seen[2], "$100=53.4"), "line 3 was %s", mock.seen[2]);
    CHECK(mock.resets == 0, "a soft reset on a clean run");
    CHECK(samples_seen >= 1 && run.samples == samples_seen, "samples %d vs %d", samples_seen, run.samples);
    CHECK(!run.lit, "a dark run read as lit");
}

static void test_error_stops(void)
{
    mock_clear();
    jobstream_lines_t lines = { "G21\nBAD LINE\nG90\n", 0 };
    jobstream_cfg_t cfg = { .gen = jobstream_lines_gen, .ctx = &lines, .run_timeout_s = 10 };
    jobstream_run_t run;
    char err[160] = "";
    int rc = jobstream_run(&cfg, &run, err, sizeof(err));
    msleep(200);
    CHECK(rc == -1, "the error did not stop the run");
    CHECK(!strchr(err, '\r'), "the controller's carriage return is in the reason: %s", err);
    CHECK(strstr(err, "error:9") && strstr(err, "line 2") && strstr(err, "BAD LINE"),
          "the error names the wrong line: %s", err);
    CHECK(run.sent >= 2 && run.sent <= 3, "sent %d after the error", run.sent);
    CHECK(mock.resets == 1, "no soft reset after the error (%d)", mock.resets);
}

/* The tube's witnesses from another thread's view: the thermopile
 * and LASER_ON rise 0.4 s after the run starts, the tube goes dark
 * 0.4 s later. */
static void *flip(void *a)
{
    (void)a;
    struct timespec ts = { 0, 400 * 1000000L };
    nanosleep(&ts, NULL);
    sysfs_write("head/beam_detect_analog", 900);
    sysfs_write("cnc/laser_on_sampled", 40);
    nanosleep(&ts, NULL);
    sysfs_write("pic/hv_current", 0);
    return NULL;
}

static void test_lit_program(void)
{
    mock_clear();
    sysfs_write("pic/hv_current", 0);
    sysfs_write("head/beam_detect_analog", 100);
    sysfs_write("cnc/laser_on_sampled", 0);
    mock.lit_after = 2;                 /* the tube lights as line 2 is taken */
    jobstream_lines_t lines = { "M3 S400\nG1 X10 F600\nM5\nM2\n", 0 };
    jobstream_cfg_t cfg = { .gen = jobstream_lines_gen, .ctx = &lines,
                            .wait_timeout_s = 10, .run_timeout_s = 20, .end_dark_s = 0.5 };
    jobstream_run_t run;
    char err[160] = "";
    pthread_t t;
    pthread_create(&t, NULL, flip, NULL);
    int rc = jobstream_run(&cfg, &run, err, sizeof(err));
    pthread_join(t, NULL);
    msleep(200);
    CHECK(rc == 0, "lit program failed: %s", err);
    CHECK(run.lit, "the discharge was not seen");
    CHECK(run.hv_max == 250, "hv_max %ld", run.hv_max);
    CHECK(run.lon_max == 40, "lon_max %ld", run.lon_max);
    CHECK(run.tp_base == 100 && run.tp_max == 900, "thermopile base %ld max %ld", run.tp_base, run.tp_max);
    CHECK(run.t_last_lit >= run.t_first_lit, "lit times");
    CHECK(mock.resets == 0, "a soft reset on a clean lit run");
}

/* The controller alarms after the last line's ok, while the run waits for
 * its dark stretch: the run is not clean. */
static void test_enddark_alarm(void)
{
    mock_clear();
    sysfs_write("pic/hv_current", 0);
    sysfs_write("head/beam_detect_analog", 100);
    sysfs_write("cnc/laser_on_sampled", 0);
    mock.lit_after = 2;
    mock.alarm_after = 4;               /* after the M2's ok: nothing in flight */
    jobstream_lines_t lines = { "M3 S400\nG1 X10 F600\nM5\nM2\n", 0 };
    jobstream_cfg_t cfg = { .gen = jobstream_lines_gen, .ctx = &lines,
                            .wait_timeout_s = 10, .run_timeout_s = 20, .end_dark_s = 0.5 };
    jobstream_run_t run;
    char err[160] = "";
    pthread_t t;
    pthread_create(&t, NULL, flip, NULL);
    int rc = jobstream_run(&cfg, &run, err, sizeof(err));
    pthread_join(t, NULL);
    CHECK(rc == -1 && strstr(err, "ALARM:1"), "an alarm in the dark wait: rc %d, \"%s\"", rc, err);
}

static volatile int abort_flag;
static int slow_gen(void *ctx, char *buf, size_t len)
{
    int *n = ctx;
    if (*n == 0) {
        (*n)++;
        snprintf(buf, len, "G4 P0.1");
        return 1;
    }
    abort_flag = 1;
    return 2;                       /* pacing: nothing yet */
}

static void test_abort_and_pacing(void)
{
    mock_clear();
    int n = 0;
    jobstream_cfg_t cfg = { .gen = slow_gen, .ctx = &n, .abort_flag = &abort_flag, .run_timeout_s = 10 };
    jobstream_run_t run;
    char err[160] = "";
    abort_flag = 0;
    int rc = jobstream_run(&cfg, &run, err, sizeof(err));
    msleep(200);
    CHECK(rc == -1 && !strcmp(err, "aborted"), "abort: rc %d %s", rc, err);
    CHECK(run.sent == 1, "sent %d", run.sent);
    CHECK(mock.resets == 1, "no soft reset on abort (%d)", mock.resets);
}

/* The witness rate against the ticks, with no clock: a generator that
 * paces is asked once per tick, so what the sample hook has seen by
 * its tenth call is the ticks' every div-th. */
#define PACED_TICKS 10
static int paced_calls, paced_samples;
static int paced_gen(void *ctx, char *buf, size_t len)
{
    (void)ctx;
    if (++paced_calls < PACED_TICKS)
        return 2;                       /* pacing: nothing yet */
    if (paced_calls == PACED_TICKS) {
        paced_samples = samples_seen;
        snprintf(buf, len, "G21");
        return 1;
    }
    return 0;
}

static void rate_case(int div, int want)
{
    mock_clear();
    samples_seen = paced_calls = 0;
    paced_samples = -1;
    jobstream_cfg_t cfg = { .gen = paced_gen, .sample = on_sample, .sample_div = div, .run_timeout_s = 10 };
    jobstream_run_t run;
    char err[160] = "";
    int rc = jobstream_run(&cfg, &run, err, sizeof(err));
    msleep(200);
    CHECK(rc == 0, "sample_div %d: %s", div, err);
    CHECK(paced_samples == want, "sample_div %d: %d samples by tick %d, not %d", div, paced_samples, PACED_TICKS, want);
}

static void test_witness_rate(void)
{
    rate_case(0, PACED_TICKS);          /* unset: every tick, the burns' and the recorder's rate */
    rate_case(5, (PACED_TICKS + 4) / 5);
}

/* A sender that left while its line still ran: the controller answers
 * that line's ok on the next session, ahead of anything the run sent.
 * The run counts no ok it has no line out for, so it is still waiting
 * for its program end (held by the mock) when the stray one has come
 * and gone. */
static int stray_calls;
static int stray_gen(void *ctx, char *buf, size_t len)
{
    (void)ctx;
    if (++stray_calls <= 5)
        return 2;                       /* a few ticks: the stray ok lands before the first line */
    if (stray_calls == 6) {
        snprintf(buf, len, "G21");
        return 1;
    }
    if (stray_calls == 7) {
        snprintf(buf, len, "M2 P77");   /* the end, held */
        return 1;
    }
    return 0;
}
static jobstream_run_t stray_run;
static volatile int stray_rc = -2, stray_back;
static void *stray_thread(void *arg)
{
    (void)arg;
    char err[160] = "";
    jobstream_cfg_t cfg = { .gen = stray_gen, .run_timeout_s = 10 };
    stray_rc = jobstream_run(&cfg, &stray_run, err, sizeof(err));
    stray_back = 1;
    return NULL;
}

static void test_stray_ok(void)
{
    mock_clear();
    mock.stray_ok = 1;
    stray_calls = 0;
    stray_back = 0;
    pthread_t t;
    pthread_create(&t, NULL, stray_thread, NULL);
    CHECK(wait_for(&mock.holding, 1, 3000), "the held program end never arrived");
    msleep(300);
    CHECK(!stray_back, "the run ended before its program end was answered (sent %d, acked %d)",
          stray_run.sent, stray_run.acked);
    mock.release = 1;
    pthread_join(t, NULL);
    CHECK(stray_rc == 0 && stray_run.sent == 2 && stray_run.acked == 2,
          "after the release: rc %d, sent %d, acked %d", stray_rc, stray_run.sent, stray_run.acked);
}

/* The controller's state file, stamped age seconds ago on this clock. */
static void grbl_state(const char *path, double age, const char *sender)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    FILE *f = fopen(path, "w");
    fprintf(f, "{\"ts_mono\":%.3f,\"state\":\"Idle\",\"sender\":%s}\n",
            (double)ts.tv_sec + ts.tv_nsec / 1e9 - age, sender);
    fclose(f);
}

static void test_sender_check(void)
{
    char dir[160], path[256];
    snprintf(dir, sizeof(dir), "%s/run", root);
    mkdir(dir, 0700);
    setenv("GF_RUN_DIR", dir, 1);
    snprintf(path, sizeof(path), "%s/grbl.state", dir);
    unlink(path);
    CHECK(jobstream_sender_connected() == -1, "no file reads as known");
    CHECK(jobstream_sender_blocks(), "an unreadable state blocks a run (it is no license to displace a sender)");
    grbl_state(path, 0.0, "{\"connected\":true,\"generation\":3}");
    CHECK(jobstream_sender_connected() == 1, "connected not read");
    CHECK(jobstream_sender_blocks(), "a connected sender blocks a run");
    grbl_state(path, -0.5, "{\"connected\":false}");
    CHECK(jobstream_sender_connected() == 0, "disconnected not read (stamped half a second ahead: the same moment)");
    CHECK(!jobstream_sender_blocks(), "no sender: a run may go");
    /* A controller that stopped writing its file: what it last said is
     * not known, and a sender it called gone may be at work. */
    grbl_state(path, JOBSTREAM_STATE_STALE_S + 1.0, "{\"connected\":false}");
    CHECK(jobstream_sender_connected() == -1, "a state file past its age read as current");
    CHECK(jobstream_sender_blocks(), "an old state file let a run go");
}

int main(void)
{
    snprintf(root, sizeof(root), "/tmp/jobrun_test.%d", (int)getpid());
    setenv("FORGECTRL_HOST_TEST", "1", 1);     /* the test hooks' gate (testhook.h) */
    mkdir(root, 0755);
    char sub[192];
    const char *dirs[] = { "pic", "head", "cnc", "run" };
    for (int i = 0; i < 4; i++) {
        snprintf(sub, sizeof(sub), "%s/%s", root, dirs[i]);
        mkdir(sub, 0755);
    }
    setenv("GF_SYSFS_ROOT", root, 1);
    snprintf(sub, sizeof(sub), "%s/run", root);
    setenv("GF_RUN_DIR", sub, 1);       /* the controller's state file: none until a case writes it */
    sysfs_write("pic/hv_current", 0);
    sysfs_write("head/beam_detect_analog", 100);
    sysfs_write("cnc/laser_on_sampled", 0);
    for (int i = 1; i <= 4; i++) {
        char a[32];
        snprintf(a, sizeof(a), "pic/lid_ir_%d", i);
        sysfs_write(a, 100);
    }
    kernel_state("idle");               /* an unreadable state is not idle */
    snprintf(staged, sizeof(staged), "%s/job.upload", root);
    lease_sender_connected = client_connected;
    jobrun_sender_out = sender_out;
    jobrun_sender_in = sender_in;
    jobrun_tray_mode = tray_stub;
    mock_start();

    static const struct { const char *name; void (*run)(void); } cases[] = {
        { "check", test_check }, { "program-plays", test_program_plays }, { "own-m2", test_own_m2 }, { "unlock", test_unlock }, { "done-is-idle", test_done_is_idle },
        { "abort", test_abort }, { "client-refuses", test_client_refuses }, { "sender-kept", test_sender_kept },
        { "reset-banner", test_reset_banner }, { "midrun-close", test_midrun_close },
        { "inflight-bound", test_inflight_bound }, { "alarm-stops", test_alarm_stops },
        { "lines-gen-long", test_lines_gen_long },
        { "sync-under-a-wizard", test_sync_under_a_wizard }, { "hooks", test_hooks },
        { "must-light", test_must_light }, { "lit-full-rate", test_lit_full_rate },
        { "dark-program", test_dark_program }, { "error-stops", test_error_stops },
        { "lit-program", test_lit_program }, { "enddark-alarm", test_enddark_alarm },
        { "refused-connect", test_refused_connect }, { "abort-and-pacing", test_abort_and_pacing },
        { "witness-rate", test_witness_rate }, { "stray-ok", test_stray_ok },
        { "sender-check", test_sender_check }, { "package-tray", test_package_tray },
    };
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);           /* the mock answers a connection an abort has closed */
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        printf("case %s\n", cases[i].name);
        cases[i].run();
    }

    char cmd[192];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd) != 0) { }
    printf(fails ? "jobrun_test: %d FAILED\n" : "jobrun_test: all passed\n", fails);
    return fails ? 1 : 0;
}
