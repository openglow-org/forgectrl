/*
 * mcode_test.c - host test: the M-code relay's two ends
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * What the GRBL controller is told: the table from the host's status file
 * (only a host that is alive, only numbers in range, in order, "-" for
 * none); the host's mcode command run as an argument vector with the
 * code and the words, its refusal passed on in its words; and the answer
 * line, ok for a 2xx with the body's message and fail for everything else,
 * held to the port's form (printable, no brackets, bounded); the relay's
 * memory of what it answered, kept per controller pid; the relay's thread
 * against a controller that never takes the answer tries again
 * MCODE_RETRY_S apart. The daemon set: neither operation is open to a
 * package or a panel caller, and nothing is written when one tries.
 */
#include "../src/extpkg.h"
#include "../src/grblport.h"
#include "../src/mcode.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static char dir[] = "/tmp/mcode-test-XXXXXX";

/* The relay's thread runs against a scripted world (the link wraps the
 * port, the host's table and its answer; the supervisor is stubbed): a
 * controller that waits at M160 and never takes the answer. Off, every
 * wrapped call is the real one. */
static volatile int relay_world;
static volatile int relay_tries;
long super_grbl_pid(void)
{
    return relay_world ? 4242 : 0;
}
int __real_grblport_request(grblport_set_t set, grblport_op_t op, const char *arg, char *reply, size_t len);
int __wrap_grblport_request(grblport_set_t set, grblport_op_t op, const char *arg, char *reply, size_t len)
{
    if (!relay_world)
        return __real_grblport_request(set, op, arg, reply, len);
    if (op == GRBLPORT_MCODE_RESULT) {
        relay_tries++;
        snprintf(reply, len, "the GRBL controller did not answer");
        return GRBLPORT_UNREACHABLE;
    }
    snprintf(reply, len, "%s", op == GRBLPORT_STATE
             ? "{\"sender\":false,\"mcode\":{\"seq\":5,\"code\":160,\"words\":{}}}" : "ok");
    return GRBLPORT_OK;
}
void __real_extpkg_mcode_table(char *buf, size_t len);
void __wrap_extpkg_mcode_table(char *buf, size_t len)
{
    if (!relay_world)
        __real_extpkg_mcode_table(buf, len);
    else
        snprintf(buf, len, "160");
}
char *__real_extpkg_mcode_json(int code, const char *words, char *why, size_t wlen);
char *__wrap_extpkg_mcode_json(int code, const char *words, char *why, size_t wlen)
{
    if (!relay_world)
        return __real_extpkg_mcode_json(code, words, why, wlen);
    return strdup("{\"status\":200,\"body\":{\"message\":\"done\"}}");
}

static void write_file(const char *path, const char *text, mode_t mode)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
        chmod(path, mode);
    }
}

static const char *line(unsigned seq, const char *doc, const char *why)
{
    static char out[200];
    mcode_answer_line(seq, doc, why, out, sizeof(out));
    return out;
}

int main(void)
{
    char status_file[300], self[PATH_MAX], text[1024], host[300], args[300], why[300];
    if (!mkdtemp(dir))
        return 1;
    snprintf(status_file, sizeof(status_file), "%s/status.json", dir);
    setenv("FORGECTRL_EXT_STATUS", status_file, 1);

    /* The answer line. */
    CHECK(!strcmp(line(7, "{\"id\":\"a.b\",\"status\":200,\"body\":{\"message\":\"exhaust on\"}}", NULL), "7 ok exhaust on"),
          "a 2xx with a message: [%s]", line(7, "{\"status\":200,\"body\":{\"message\":\"exhaust on\"}}", NULL));
    CHECK(!strcmp(line(8, "{\"status\":204,\"body\":null}", NULL), "8 ok"), "a 2xx with no body: [%s]",
          line(8, "{\"status\":204,\"body\":null}", NULL));
    CHECK(!strcmp(line(9, "{\"status\":409,\"body\":{\"error\":\"the plug did not answer\"}}", NULL),
                  "9 fail the plug did not answer"), "a 409 with its error: [%s]",
          line(9, "{\"status\":409,\"body\":{\"error\":\"the plug did not answer\"}}", NULL));
    CHECK(!strncmp(line(10, "{\"status\":500,\"body\":{}}", NULL), "10 fail ", 8), "a 500 with nothing: [%s]",
          line(10, "{\"status\":500,\"body\":{}}", NULL));
    CHECK(!strcmp(line(11, NULL, "its service is frozen while a job is armed"), "11 fail its service is frozen while a job is armed"),
          "the host's refusal: [%s]", line(11, NULL, "its service is frozen while a job is armed"));
    CHECK(!strncmp(line(12, NULL, NULL), "12 fail ", 8), "nothing at all: [%s]", line(12, NULL, NULL));
    CHECK(!strncmp(line(13, "not json", NULL), "13 fail ", 8), "not JSON: [%s]", line(13, "not json", NULL));
    const char *l = line(14, "{\"status\":200,\"body\":{\"message\":\"[MSG:x]\\n\\u00e9ok\"}}", NULL);
    CHECK(!strcmp(l, "14 ok (MSG:x)   ok"), "brackets, a line break, and a byte past ASCII: [%s]", l);
    char big[400];
    snprintf(big, sizeof(big), "{\"status\":200,\"body\":{\"message\":\"%0300d\"}}", 0);
    l = line(15, big, NULL);
    CHECK(strlen(l) == strlen("15 ok ") + MCODE_TEXT_MAX, "a long message is cut to %d bytes: %zu", MCODE_TEXT_MAX, strlen(l));

    /* The relay's memory is one controller's: each numbers its M-codes from
     * 1, so a respawned controller (a new pid, even between two polls)
     * starts it empty, and its first M-code is answered. */
    mcode_memory_t mem = { 0 };
    CHECK(mcode_memory_for(&mem, 4001) == 1 && mem.done == 0, "a first controller starts the memory empty");
    mem.done = 1;
    CHECK(mcode_memory_for(&mem, 4001) == 0 && mem.done == 1, "the same controller keeps what was answered");
    CHECK(mcode_memory_for(&mem, 4002) == 1 && mem.done == 0, "a new controller's sequence 1 is not taken for answered");
    /* A sequence is answered once the controller says so, or refuses the
     * answer in its own words; an answer that did not arrive is asked
     * again. */
    unsigned t1 = mcode_memory_answered(&mem, 3, GRBLPORT_UNREACHABLE, "the GRBL controller did not answer");
    CHECK(mem.done == 0, "an answer the controller never took marked its sequence answered");
    /* The tries that did not take are counted, so only the first is
     * logged; the one that takes ends the count. */
    unsigned t2 = mcode_memory_answered(&mem, 3, GRBLPORT_OK, "busy:sender");
    CHECK(t1 == 1 && t2 == 2, "the tries that did not take counted %u, %u", t1, t2);
    CHECK(mcode_memory_answered(&mem, 3, GRBLPORT_OK, "ok") == 0, "a taken answer still counted as a try");
    CHECK(mem.done == 3, "an answer the controller took left its sequence unanswered");
    mcode_memory_answered(&mem, 4, GRBLPORT_OK, "error:stale");
    CHECK(mem.done == 4, "a stale refusal left its sequence to be asked again");
    /* The extension hears an M-code once: an answer that did not reach the
     * controller goes again, and the extension is not asked again. */
    CHECK(mcode_memory_ask(&mem, 5) == 1, "a new sequence did not go to its extension");
    CHECK(mcode_memory_ask(&mem, 5) == 0, "a sequence went to its extension twice");
    CHECK(mcode_memory_answered(&mem, 5, GRBLPORT_UNREACHABLE, "") == 1,
          "a new sequence's first try that did not take is not its first");
    CHECK(mcode_memory_for(&mem, 4003) == 1 && mcode_memory_ask(&mem, 5) == 1,
          "a new controller's sequence was taken for one its extension heard");

    /* The table, from the host's own status, only while that host is alive. */
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    self[n > 0 ? n : 0] = '\0';
    setenv("FORGECTRL_FORGEEXT", self, 1);
    char table[128];
    snprintf(text, sizeof(text), "{\"pid\": %d, \"mcodes\": [{\"code\": 171, \"id\": \"b.c\"}, {\"code\": 160, \"id\": \"a.b\"}, "
                                 "{\"code\": 159, \"id\": \"x.y\"}, {\"code\": 180, \"id\": \"x.y\"}, {\"code\": \"165\"}]}", (int)getpid());
    write_file(status_file, text, 0644);
    extpkg_mcode_table(table, sizeof(table));
    CHECK(!strcmp(table, "160,171"), "the numbers in range, in order: [%s]", table);
    snprintf(text, sizeof(text), "{\"pid\": %d, \"mcodes\": []}", (int)getpid());
    write_file(status_file, text, 0644);
    extpkg_mcode_table(table, sizeof(table));
    CHECK(!strcmp(table, "-"), "none: [%s]", table);
    write_file(status_file, "{\"pid\": 1, \"mcodes\": [{\"code\": 160, \"id\": \"a.b\"}]}", 0644);
    extpkg_mcode_table(table, sizeof(table));
    CHECK(!strcmp(table, "-"), "a host that is not alive answers none: [%s]", table);
    unlink(status_file);
    extpkg_mcode_table(table, sizeof(table));
    CHECK(!strcmp(table, "-"), "no status file: [%s]", table);

    /* The host's mcode command. */
    snprintf(host, sizeof(host), "%s/forgeext", dir);
    snprintf(args, sizeof(args), "%s/args", dir);
    snprintf(text, sizeof(text), "#!/bin/sh\nfor a in \"$@\"; do echo \"$a\"; done > %s\n"
             "if [ \"$2\" = 161 ]; then echo '{\"ok\": false, \"error\": \"no package answers this M-code\"}'; exit 1; fi\n"
             "echo '{\"ok\": true, \"id\": \"a.b\", \"status\": 200, \"body\": {\"message\": \"done\"}}'\n", args);
    write_file(host, text, 0755);
    setenv("FORGECTRL_FORGEEXT", host, 1);
    char *doc = extpkg_mcode_json(160, "{\"P\":2}", why, sizeof(why));
    FILE *f = fopen(args, "r");
    size_t got = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
    if (f)
        fclose(f);
    text[got] = '\0';
    CHECK(doc && !strcmp(text, "mcode\n160\n{\"P\":2}\n") && !strcmp(line(3, doc, NULL), "3 ok done"),
          "the command and its answer: [%s] %s", text, doc ? doc : why);
    free(doc);
    doc = extpkg_mcode_json(161, "{}", why, sizeof(why));
    CHECK(!doc && !strcmp(why, "no package answers this M-code"), "the host's refusal in its words: %s", why);
    free(doc);
    unlink(args);
    static const struct { int code; const char *words; } bad[] = { { 159, "{}" }, { 180, "{}" }, { 160, NULL },
                                                                  { 160, "--root" }, { 160, "[1]" } };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        doc = extpkg_mcode_json(bad[i].code, bad[i].words, why, sizeof(why));
        CHECK(!doc && access(args, F_OK) != 0, "M%d %s ran the host", bad[i].code, bad[i].words ? bad[i].words : "(none)");
        free(doc);
    }

    /* The daemon set: no request's caller reaches either operation. */
    char reply[128];
    CHECK(grblport_request(GRBLPORT_SET_PANEL, GRBLPORT_MCODES, "160", reply, sizeof(reply)) == GRBLPORT_FORBIDDEN,
          "the panel set told the controller its M-codes");
    CHECK(grblport_request(GRBLPORT_SET_PACKAGE, GRBLPORT_MCODE_RESULT, "1 ok", reply, sizeof(reply)) == GRBLPORT_FORBIDDEN,
          "the package set answered an M-code");
    CHECK(grblport_request(GRBLPORT_SET_DAEMON, GRBLPORT_MCODE_RESULT, "1 ok\nrelease", reply, sizeof(reply)) == GRBLPORT_FORBIDDEN,
          "an answer that carries a second line went out");

    /* The relay's thread: an answer the controller does not take goes
     * again MCODE_RETRY_S apart, not at the poll's pace. Over 1.5 s that
     * is at most 1.5 / MCODE_RETRY_S + 1 tries (the extension asked once). */
    relay_world = 1;
    mcode_init();
    struct timespec run = { 1, 500 * 1000000L };
    nanosleep(&run, NULL);
    int tries = relay_tries;
    relay_world = 0;                    /* the relay idles: no controller */
    printf("the relay tried %d times in 1.5 s\n", tries);
    CHECK(tries >= 2 && tries <= (int)(1.5 / MCODE_RETRY_S) + 1,
          "an answer the controller did not take went %d times in 1.5 s", tries);

    char cmd[340];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0)
        fails++;
    printf(fails ? "mcode_test: %d FAILED\n" : "mcode_test: all passed\n", fails);
    return fails ? 1 : 0;
}
