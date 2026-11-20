/*
 * grblport_test.c - host test for the daemon's client of the controller port
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * A mock port on a Unix socket in a scratch run directory records every
 * line it receives and answers from a script. Cases: no controller on
 * the port; a request and its reply; one connection kept across
 * requests; the package set cannot reach a panel operation (the crumb
 * tray's mode among them), and nothing is written when it tries; an argument that carries a line break is
 * refused with nothing written; a controller that restarted costs the
 * next request nothing; a home, release or energize tells the alarm's
 * watcher before it goes out, answered or not; a port that never answers
 * is given up on and the request is not repeated; an answer that comes after its request
 * was given up on is taken by the next connection's fence, and every
 * later request has its own answer; an answer longer than the buffer is
 * no answer, and the next is whole; a state that is not one object
 * (cut short, or not opened) is no state; a jog's cancel cuts a request
 * the port holds instead of waiting (the port sees the hang-up at once),
 * sends no line, and the cut request is not sent again; the jog's words and their bounds; what each reply
 * means to an HTTP client.
 */
#include "../src/grblport.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static char root[64];

/* ---- the mock port ---- */

static struct {
    int listen_fd;
    volatile int accepts, lines;
    char seen[32][160];
    volatile int hang_up_after;     /* close the client after this many lines in all */
    volatile int mute;              /* take lines, answer nothing */
    volatile int late;              /* hold the next answer for the next connection */
    char owed[64];                  /* an answer held, as the controller holds a line's status */
    volatile int long_state;        /* answer the state with a line longer than any buffer */
    volatile long hung_ms;          /* when the client last hung up, on mono_ms() */
} mock;

static int touched;
static void alarm_touched(void)
{
    touched++;
}

static long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *mock_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int c = accept(mock.listen_fd, NULL, NULL);
        if (c < 0)
            break;
        mock.accepts++;
        char buf[256];
        size_t n = 0;
        int first = 1;
        for (;;) {
            ssize_t r = read(c, buf + n, sizeof(buf) - 1 - n);
            if (r <= 0) {
                mock.hung_ms = mono_ms();
                break;
            }
            n += (size_t)r;
            char *nl;
            while ((nl = memchr(buf, '\n', n))) {
                *nl = '\0';
                if (mock.lines < 32)
                    snprintf(mock.seen[mock.lines], sizeof(mock.seen[0]), "%.150s", buf);
                mock.lines++;
                static char big[700];
                memset(big, ' ', sizeof(big));
                big[0] = '{';
                big[sizeof(big) - 3] = '}';
                big[sizeof(big) - 2] = '\n';
                big[sizeof(big) - 1] = '\0';
                static char longword[400];
        memset(longword, 'x', sizeof(longword));
        memcpy(longword, "error:", 6);
        longword[sizeof(longword) - 2] = '\n';
        longword[sizeof(longword) - 1] = '\0';
        const char *ans = mock.long_state == 3 && !strncmp(buf, "jog", 3) ? longword
                        : mock.long_state == 4 && !strcmp(buf, "state") ? "\"state\":\"Idle\"}\n"
                        : mock.long_state == 2 && !strcmp(buf, "state") ? "{\"state\":\"Idle\",\"sender\n"
                        : mock.long_state && !strcmp(buf, "state") ? big
                                : !strcmp(buf, "state") ? "{\"state\":\"Idle\",\"released\":false}\n"
                                : strstr(buf, "X99") ? "error:15\n"
                                : "ok\n";
                /* The answer a client gave up on goes to whoever holds the
                 * port when it comes: the next connection, ahead of the
                 * answer to its first line. */
                if (first && mock.owed[0]) {
                    if (write(c, mock.owed, strlen(mock.owed)) < 0) { }
                    mock.owed[0] = '\0';
                }
                first = 0;
                if (mock.late) {
                    mock.late = 0;
                    snprintf(mock.owed, sizeof(mock.owed), "error:aborted\n");
                } else if (!mock.mute && write(c, ans, strlen(ans)) < 0) { }
                size_t rest = n - (size_t)(nl + 1 - buf);
                memmove(buf, nl + 1, rest);
                n = rest;
            }
            if (mock.hang_up_after && mock.lines >= mock.hang_up_after) {
                mock.hang_up_after = 0;
                break;
            }
        }
        close(c);
    }
    return NULL;
}

static int mock_start(pthread_t *th)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%.64s/grbl.ctl", root);
    mock.listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (mock.listen_fd < 0 || bind(mock.listen_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        listen(mock.listen_fd, 4) != 0)
        return -1;
    return pthread_create(th, NULL, mock_thread, NULL);
}

static void settle(void)
{
    usleep(50 * 1000);
}

static int released_rc;
static char released_reply[128];
static void *release_thread(void *arg)
{
    (void)arg;
    released_rc = grblport_request(GRBLPORT_SET_PANEL, GRBLPORT_RELEASE, NULL, released_reply,
                                   sizeof(released_reply));
    return NULL;
}

int main(void)
{
    char reply[128], words[96], err[96];
    pthread_t th;
    setenv("FORGECTRL_HOST_TEST", "1", 1);     /* the test hooks' gate (testhook.h) */

    signal(SIGPIPE, SIG_IGN);
    snprintf(root, sizeof(root), "/tmp/grblport_test.%d", (int)getpid());
    mkdir(root, 0700);
    setenv("GF_RUN_DIR", root, 1);

    /* No controller on the port. */
    int rc = grblport_request(GRBLPORT_SET_PANEL, GRBLPORT_STATE, NULL, reply, sizeof(reply));
    CHECK(rc == GRBLPORT_UNREACHABLE && strstr(reply, "not on its port"),
          "no port: rc %d, '%s'", rc, reply);

    if (mock_start(&th) != 0) {
        printf("FAIL: cannot start the mock port\n");
        return 1;
    }

    /* A request and its reply; the connection is kept. */
    rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_STATE, NULL, reply, sizeof(reply));
    CHECK(rc == GRBLPORT_OK && !strcmp(reply, "{\"state\":\"Idle\",\"released\":false}"),
          "state: rc %d, '%s'", rc, reply);
    rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_JOG, "G91 G21 X1.000 F3000", reply, sizeof(reply));
    CHECK(rc == GRBLPORT_OK && !strcmp(reply, "ok"), "jog: rc %d, '%s'", rc, reply);
    rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_CANCEL, NULL, reply, sizeof(reply));
    CHECK(rc == GRBLPORT_OK && !strcmp(reply, "ok"), "cancel: rc %d, '%s'", rc, reply);
    settle();
    CHECK(mock.accepts == 1, "three requests used %d connections, not one", mock.accepts);
    /* A new connection begins with its fence, a state request. */
    CHECK(mock.lines == 4 && !strcmp(mock.seen[0], "state") && !strcmp(mock.seen[1], "state") &&
          !strcmp(mock.seen[2], "jog G91 G21 X1.000 F3000") && !strcmp(mock.seen[3], "cancel"),
          "the port saw %d lines: '%s' '%s' '%s' '%s'", mock.lines, mock.seen[0], mock.seen[1], mock.seen[2],
          mock.seen[3]);

    /* The package set cannot reach a panel operation: refused before the
     * socket, so the port sees nothing. */
    static const grblport_op_t panel_ops[] = { GRBLPORT_RELEASE, GRBLPORT_ENERGIZE, GRBLPORT_HOME };
    int before = mock.lines;
    for (size_t i = 0; i < sizeof(panel_ops) / sizeof(panel_ops[0]); i++) {
        rc = grblport_request(GRBLPORT_SET_PACKAGE, panel_ops[i], NULL, reply, sizeof(reply));
        CHECK(rc == GRBLPORT_FORBIDDEN, "panel op %d from the package set: rc %d", (int)panel_ops[i], rc);
    }
    /* The crumb tray's mode is the panel's too, argument and all. */
    rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_TRAY, "out", reply, sizeof(reply));
    CHECK(rc == GRBLPORT_FORBIDDEN, "tray out from the package set: rc %d", rc);
    rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_TRAY, "in", reply, sizeof(reply));
    CHECK(rc == GRBLPORT_FORBIDDEN, "tray in from the package set: rc %d", rc);
    /* Keeping the sender out is the daemon's alone. */
    rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_SENDER, "out", reply, sizeof(reply));
    CHECK(rc == GRBLPORT_FORBIDDEN, "sender out from the package set: rc %d", rc);
    rc = grblport_request(GRBLPORT_SET_PANEL, GRBLPORT_SENDER, "in", reply, sizeof(reply));
    CHECK(rc == GRBLPORT_FORBIDDEN, "sender in from the panel set: rc %d", rc);
    rc = grblport_request(GRBLPORT_SET_PACKAGE, (grblport_op_t)99, NULL, reply, sizeof(reply));
    CHECK(rc == GRBLPORT_FORBIDDEN, "an unknown op: rc %d", rc);
    settle();
    CHECK(mock.lines == before, "a refused operation reached the port (%d lines)", mock.lines - before);

    /* An argument with a line break would be a second request that passed
     * no set check: refused, nothing written. */
    rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_JOG, "G91 X1 F100\nrelease", reply, sizeof(reply));
    CHECK(rc == GRBLPORT_FORBIDDEN, "a jog that smuggles a line: rc %d", rc);
    rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_JOG, "G91 X1 F100\rrelease", reply, sizeof(reply));
    CHECK(rc == GRBLPORT_FORBIDDEN, "a jog that smuggles a carriage return: rc %d", rc);
    rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_JOG, NULL, reply, sizeof(reply));
    CHECK(rc == GRBLPORT_FORBIDDEN, "a jog without words: rc %d", rc);
    settle();
    CHECK(mock.lines == before, "a smuggled line reached the port (%d lines)", mock.lines - before);

    /* The panel set reaches them; each of release, energize and home tells
     * whoever watches the alarm (the job runner's stop), and nothing else
     * does. */
    grblport_alarm_touched = alarm_touched;
    touched = 0;
    rc = grblport_request(GRBLPORT_SET_PANEL, GRBLPORT_RELEASE, NULL, reply, sizeof(reply));
    CHECK(rc == GRBLPORT_OK && !strcmp(reply, "ok"), "release: rc %d, '%s'", rc, reply);
    CHECK(touched == 1, "a release told %d times", touched);
    rc = grblport_request(GRBLPORT_SET_PANEL, GRBLPORT_ENERGIZE, NULL, reply, sizeof(reply));
    CHECK(rc == GRBLPORT_OK, "energize: rc %d", rc);
    CHECK(touched == 2, "an energize told %d times in all", touched);
    rc = grblport_request(GRBLPORT_SET_PANEL, GRBLPORT_HOME, NULL, reply, sizeof(reply));
    CHECK(rc == GRBLPORT_OK, "home: rc %d", rc);
    CHECK(touched == 3, "a home told %d times in all", touched);
    rc = grblport_request(GRBLPORT_SET_PANEL, GRBLPORT_TRAY, "out", reply, sizeof(reply));
    CHECK(rc == GRBLPORT_OK && !strcmp(reply, "ok"), "tray out: rc %d, '%s'", rc, reply);
    rc = grblport_request(GRBLPORT_SET_PANEL, GRBLPORT_STATE, NULL, reply, sizeof(reply));
    CHECK(touched == 3, "a tray or state request told the alarm's watcher (%d)", touched);
    grblport_alarm_touched = NULL;
    rc = grblport_request(GRBLPORT_SET_PANEL, GRBLPORT_TRAY, "in\nrelease", reply, sizeof(reply));
    CHECK(rc == GRBLPORT_FORBIDDEN, "a tray op that smuggles a line: rc %d", rc);
    settle();
    CHECK(mock.lines == before + 5 && !strcmp(mock.seen[before], "release") &&
          !strcmp(mock.seen[before + 1], "energize") && !strcmp(mock.seen[before + 2], "home") &&
          !strcmp(mock.seen[before + 3], "tray out") && !strcmp(mock.seen[before + 4], "state"),
          "the panel operations: %d lines", mock.lines - before);

    /* A refusal is a reply like any other. */
    rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_JOG, "G91 G21 X99.000 F3000", reply, sizeof(reply));
    CHECK(rc == GRBLPORT_OK && !strcmp(reply, "error:15"), "a refused jog: rc %d, '%s'", rc, reply);

    /* The controller restarts: the kept connection is dead, and the next
     * request finds that out and goes to the new one, once. */
    mock.hang_up_after = mock.lines + 1;
    rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_CANCEL, NULL, reply, sizeof(reply));
    CHECK(rc == GRBLPORT_OK, "the request before the hang-up: rc %d", rc);
    settle();
    before = mock.lines;
    int accepts = mock.accepts;
    rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_STATE, NULL, reply, sizeof(reply));
    CHECK(rc == GRBLPORT_OK && reply[0] == '{', "after a restart: rc %d, '%s'", rc, reply);
    settle();
    CHECK(mock.accepts == accepts + 1 && mock.lines == before + 2,
          "after a restart: %d new connections, %d lines (the fence and the request)", mock.accepts - accepts,
          mock.lines - before);

    /* A port that takes the request and never answers: given up on, and
     * the request is not sent a second time. The controller may still act
     * on it, so the alarm's watcher was told all the same. */
    mock.mute = 1;
    before = mock.lines;
    grblport_alarm_touched = alarm_touched;
    touched = 0;
    rc = grblport_request(GRBLPORT_SET_PANEL, GRBLPORT_RELEASE, NULL, reply, sizeof(reply));
    CHECK(rc == GRBLPORT_UNREACHABLE && strstr(reply, "did not answer"), "a mute port: rc %d, '%s'", rc, reply);
    CHECK(touched == 1, "a release the port never answered told %d times", touched);
    grblport_alarm_touched = NULL;
    settle();
    CHECK(mock.lines == before + 1, "a mute port saw the request %d times", mock.lines - before);
    mock.mute = 0;
    rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_STATE, NULL, reply, sizeof(reply));
    CHECK(rc == GRBLPORT_OK && reply[0] == '{', "after a timeout: rc %d, '%s'", rc, reply);

    /* An answer that comes after the request was given up on reaches the
     * next connection ahead of that connection's own: the fence takes it,
     * and every request after has its own answer. */
    mock.late = 1;
    rc = grblport_request(GRBLPORT_SET_PANEL, GRBLPORT_RELEASE, NULL, reply, sizeof(reply));
    CHECK(rc == GRBLPORT_UNREACHABLE, "a late answer: the request is given up on: rc %d", rc);
    rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_STATE, NULL, reply, sizeof(reply));
    CHECK(rc == GRBLPORT_OK && reply[0] == '{', "after a late answer, the state is the state: rc %d, '%s'", rc, reply);
    rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_JOG, "G91 G21 X99.000 F3000", reply, sizeof(reply));
    CHECK(rc == GRBLPORT_OK && !strcmp(reply, "error:15"), "and a jog has its own answer: rc %d, '%s'", rc, reply);

    /* An answer longer than the caller's buffer is no answer, and the next
     * one is still a whole line of its own. */
    {
        char big[512];
        mock.long_state = 1;
        rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_STATE, NULL, big, sizeof(big));
        mock.long_state = 0;
        CHECK(rc != GRBLPORT_OK, "an answer that does not fit passed: rc %d", rc);
        mock.long_state = 2;
        rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_STATE, NULL, big, sizeof(big));
        mock.long_state = 0;
        CHECK(rc != GRBLPORT_OK, "a state that is not one whole object passed: rc %d, '%s'", rc, big);
        mock.long_state = 4;
        rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_STATE, NULL, big, sizeof(big));
        mock.long_state = 0;
        CHECK(rc != GRBLPORT_OK, "a state that does not open an object passed: rc %d, '%s'", rc, big);
        mock.long_state = 3;
        rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_JOG, "G91 G21 X1.000 F3000", reply, sizeof(reply));
        mock.long_state = 0;
        CHECK(rc != GRBLPORT_OK, "a jog's answer that does not fit passed as '%s'", reply);
        rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_JOG, "G91 G21 X1.000 F3000", reply, sizeof(reply));
        CHECK(rc == GRBLPORT_OK && !strcmp(reply, "ok"), "the answer after it: rc %d, '%s'", rc, reply);
    }

    /* A jog's cancel does not wait behind a request the port holds: it
     * cuts the connection (the jog's dead-man), sends no line, and the
     * request it cut is not sent again. */
    {
        pthread_t rt;
        mock.mute = 1;
        before = mock.lines;
        pthread_create(&rt, NULL, release_thread, NULL);
        for (int t = 0; t < 200 && mock.lines == before; t++)
            usleep(1000);
        mock.hung_ms = 0;
        rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_CANCEL, NULL, reply, sizeof(reply));
        long cancel_ms = mono_ms();
        /* The cut is what stops the jog: the port sees the hang-up at once,
         * not when the held request gives up (GRBLPORT_REPLY_TIMEOUT_MS). */
        for (int t = 0; t < 200 && !mock.hung_ms; t++)
            usleep(1000);
        CHECK(mock.hung_ms && mock.hung_ms - cancel_ms < 200,
              "the port did not see the cancel's cut within 200 ms (%ld ms)",
              mock.hung_ms ? mock.hung_ms - cancel_ms : -1);
        pthread_join(rt, NULL);
        mock.mute = 0;
        settle();
        CHECK(rc == GRBLPORT_OK && mock.lines == before + 1 && !strcmp(mock.seen[before], "release"),
              "the cancel waited for the port, or a line went out: rc %d, %d lines", rc, mock.lines - before);
        CHECK(released_rc == GRBLPORT_UNREACHABLE && strstr(released_reply, "cancel"),
              "the request the cancel cut: rc %d, '%s'", released_rc, released_reply);
    }

    /* The jog's words. */
    rc = grblport_jog_words(1, 0, 0, 3000, words, sizeof(words), err, sizeof(err));
    CHECK(rc == 0 && !strcmp(words, "G91 G21 X1.000 F3000"), "x jog: %d '%s'", rc, words);
    rc = grblport_jog_words(-0.1, 25.5, 0, 600, words, sizeof(words), err, sizeof(err));
    CHECK(rc == 0 && !strcmp(words, "G91 G21 X-0.100 Y25.500 F600"), "xy jog: %d '%s'", rc, words);
    rc = grblport_jog_words(0, 0, -0.5, 100, words, sizeof(words), err, sizeof(err));
    CHECK(rc == 0 && !strcmp(words, "G91 G21 Z-0.500 F100"), "z jog: %d '%s'", rc, words);
    for (const char *p = words; *p; p++)
        CHECK((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr(".-+ ", *p),
              "a character the port refuses: '%c'", *p);
    CHECK(grblport_jog_words(0, 0, 0, 3000, words, sizeof(words), err, sizeof(err)) != 0, "a jog of nothing");
    CHECK(grblport_jog_words(0.0001, 0, 0, 3000, words, sizeof(words), err, sizeof(err)) != 0,
          "a jog below the resolution");
    CHECK(grblport_jog_words(100.5, 0, 0, 3000, words, sizeof(words), err, sizeof(err)) != 0, "x past its bound");
    CHECK(grblport_jog_words(0, -100.5, 0, 3000, words, sizeof(words), err, sizeof(err)) != 0, "y past its bound");
    CHECK(grblport_jog_words(0, 0, 5.5, 100, words, sizeof(words), err, sizeof(err)) != 0, "z past its bound");
    CHECK(grblport_jog_words(100, -100, 5, 12000, words, sizeof(words), err, sizeof(err)) == 0, "the bounds themselves");
    CHECK(grblport_jog_words(1, 0, 0, 5, words, sizeof(words), err, sizeof(err)) != 0, "a feed below the range");
    CHECK(grblport_jog_words(1, 0, 0, 12001, words, sizeof(words), err, sizeof(err)) != 0, "a feed above the range");
    CHECK(grblport_jog_words(strtod("nan", NULL), 0, 0, 3000, words, sizeof(words), err, sizeof(err)) != 0, "NaN");
    CHECK(grblport_jog_words(1, 0, 0, strtod("inf", NULL), words, sizeof(words), err, sizeof(err)) != 0, "inf");
    CHECK(grblport_jog_words(1, 1, 1, 3000, words, 12, err, sizeof(err)) != 0, "a buffer too short");

    /* What a reply means. */
    char why[128];
    CHECK(grblport_explain("ok", why, sizeof(why)) == 0, "ok");
    CHECK(grblport_explain("busy:released", why, sizeof(why)) == 409 && why[0], "a busy word is a conflict, explained");
    CHECK(grblport_explain("error:15", why, sizeof(why)) == 409 && why[0], "an error word is a conflict, explained");
    CHECK(grblport_explain("error:33", why, sizeof(why)) == 409 && strstr(why, "error:33"),
          "an error with no explanation carries its word");
    CHECK(grblport_explain("what", why, sizeof(why)) == 502, "an unknown reply");

    grblport_close();
    shutdown(mock.listen_fd, SHUT_RDWR);
    close(mock.listen_fd);
    pthread_join(th, NULL);
    char path[160];
    snprintf(path, sizeof(path), "%s/grbl.ctl", root);
    unlink(path);
    rmdir(root);

    printf(fails ? "grblport_test: %d FAILED\n" : "grblport_test: all passed\n", fails);
    return fails ? 1 : 0;
}
