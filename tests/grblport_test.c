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
 * next request nothing; a port that never answers is given up on and
 * the request is not repeated; the jog's words and their bounds; what
 * each reply means to an HTTP client.
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
} mock;

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
        for (;;) {
            ssize_t r = read(c, buf + n, sizeof(buf) - 1 - n);
            if (r <= 0)
                break;
            n += (size_t)r;
            char *nl;
            while ((nl = memchr(buf, '\n', n))) {
                *nl = '\0';
                if (mock.lines < 32)
                    snprintf(mock.seen[mock.lines], sizeof(mock.seen[0]), "%.150s", buf);
                mock.lines++;
                const char *ans = !strcmp(buf, "state") ? "{\"state\":\"Idle\",\"released\":false}\n"
                                : strstr(buf, "X99") ? "error:15\n"
                                : "ok\n";
                if (!mock.mute && write(c, ans, strlen(ans)) < 0) { }
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

int main(void)
{
    char reply[128], words[96], err[96];
    pthread_t th;

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
    CHECK(mock.lines == 3 && !strcmp(mock.seen[0], "state") &&
          !strcmp(mock.seen[1], "jog G91 G21 X1.000 F3000") && !strcmp(mock.seen[2], "cancel"),
          "the port saw %d lines: '%s' '%s' '%s'", mock.lines, mock.seen[0], mock.seen[1], mock.seen[2]);

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

    /* The panel set reaches them. */
    rc = grblport_request(GRBLPORT_SET_PANEL, GRBLPORT_RELEASE, NULL, reply, sizeof(reply));
    CHECK(rc == GRBLPORT_OK && !strcmp(reply, "ok"), "release: rc %d, '%s'", rc, reply);
    rc = grblport_request(GRBLPORT_SET_PANEL, GRBLPORT_ENERGIZE, NULL, reply, sizeof(reply));
    CHECK(rc == GRBLPORT_OK, "energize: rc %d", rc);
    rc = grblport_request(GRBLPORT_SET_PANEL, GRBLPORT_HOME, NULL, reply, sizeof(reply));
    CHECK(rc == GRBLPORT_OK, "home: rc %d", rc);
    rc = grblport_request(GRBLPORT_SET_PANEL, GRBLPORT_TRAY, "out", reply, sizeof(reply));
    CHECK(rc == GRBLPORT_OK && !strcmp(reply, "ok"), "tray out: rc %d, '%s'", rc, reply);
    rc = grblport_request(GRBLPORT_SET_PANEL, GRBLPORT_TRAY, "in\nrelease", reply, sizeof(reply));
    CHECK(rc == GRBLPORT_FORBIDDEN, "a tray op that smuggles a line: rc %d", rc);
    settle();
    CHECK(mock.lines == before + 4 && !strcmp(mock.seen[before], "release") &&
          !strcmp(mock.seen[before + 1], "energize") && !strcmp(mock.seen[before + 2], "home") &&
          !strcmp(mock.seen[before + 3], "tray out"),
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
    CHECK(mock.accepts == accepts + 1 && mock.lines == before + 1,
          "after a restart: %d new connections, %d lines", mock.accepts - accepts, mock.lines - before);

    /* A port that takes the request and never answers: given up on, and
     * the request is not sent a second time. */
    mock.mute = 1;
    before = mock.lines;
    rc = grblport_request(GRBLPORT_SET_PANEL, GRBLPORT_RELEASE, NULL, reply, sizeof(reply));
    CHECK(rc == GRBLPORT_UNREACHABLE && strstr(reply, "did not answer"), "a mute port: rc %d, '%s'", rc, reply);
    settle();
    CHECK(mock.lines == before + 1, "a mute port saw the request %d times", mock.lines - before);
    mock.mute = 0;
    rc = grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_STATE, NULL, reply, sizeof(reply));
    CHECK(rc == GRBLPORT_OK && reply[0] == '{', "after a timeout: rc %d, '%s'", rc, reply);

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
    CHECK(grblport_explain("busy:released", why, sizeof(why)) == 409 && strstr(why, "released"), "busy:released");
    CHECK(grblport_explain("busy:sender", why, sizeof(why)) == 409 && strstr(why, "client"), "busy:sender");
    CHECK(grblport_explain("busy:state", why, sizeof(why)) == 409, "busy:state");
    CHECK(grblport_explain("error:mode", why, sizeof(why)) == 409 && strstr(why, "manual"), "error:mode");
    CHECK(grblport_explain("busy:mcode", why, sizeof(why)) == 409 && strstr(why, "M-code"), "busy:mcode");
    CHECK(grblport_explain("error:saved", why, sizeof(why)) == 409 && strstr(why, "tray"), "error:saved");
    CHECK(grblport_explain("error:15", why, sizeof(why)) == 409 && strstr(why, "envelope"), "error:15");
    CHECK(grblport_explain("error:9", why, sizeof(why)) == 409 && strstr(why, "alarm"), "error:9");
    CHECK(grblport_explain("error:33", why, sizeof(why)) == 409 && strstr(why, "error:33"), "error:33");
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
