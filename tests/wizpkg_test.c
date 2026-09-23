/*
 * wizpkg_test.c - host test: a package's check on the Setup page
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The test binary is its own stand-in for the extension host: run as
 * "list" or "wizard ...", it records its arguments and answers from the
 * files the test wrote, and as the host's pid in the status file it is a
 * host that is alive. The runner's calls are stand-ins here too, recording
 * what the check did. Cases: every step held to its form (a log, a phase,
 * a progress, a prompt of each kind, a result, and each way out of form);
 * a check run end to end (start, the prompt asked, the answer carried back
 * as the host's answer command, a confirm as yes, the result shown and
 * recorded by nobody); a result that did not pass; the service's refusal;
 * a step out of form, and an operator's abort, each sending abort; the
 * host's own refusal; and the Setup page's list, only for a package whose
 * service runs, with its title and whether it is done.
 */
#include "../src/extpkg.h"
#include "../src/wizpkg.h"
#include "../src/wizrun.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static char dir[] = "/tmp/wizpkg-test-XXXXXX";

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static const char *read_file(const char *path)
{
    static char text[4096];
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
    if (f)
        fclose(f);
    text[n] = '\0';
    return text;
}

/* ---- the host, as this binary run again ---- */

static int stand_in(int argc, char **argv)
{
    const char *d = getenv("WIZPKG_TEST_DIR");
    char path[400];
    snprintf(path, sizeof(path), "%s/args", d);
    FILE *f = fopen(path, "a");
    for (int i = 1; f && i < argc; i++)
        fprintf(f, "%s%s", argv[i], i + 1 < argc ? " " : "\n");
    if (f)
        fclose(f);
    if (!strcmp(argv[1], "list"))
        snprintf(path, sizeof(path), "%s/list.json", d);
    else
        snprintf(path, sizeof(path), "%s/%s.json", d, argc > 3 ? argv[3] : "none");
    const char *text = read_file(path);
    fputs(text[0] ? text : "{\"ok\": false, \"error\": \"no answer written\"}", stdout);
    return strstr(text, "\"ok\": true") ? 0 : 1;
}

/* ---- the runner, as stand-ins that record ---- */

static char logged[1024], phase[128], finished[256], asked[512], answer_given[64];
static int progress = -1, shown, ask_rc;
static const char *current = "pkg:org.example.check";

void wiz_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t n = strlen(logged);
    vsnprintf(logged + n, sizeof(logged) - n, fmt, ap);
    va_end(ap);
    strncat(logged, "|", sizeof(logged) - strlen(logged) - 1);
}
void wiz_phase(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(phase, sizeof(phase), fmt, ap);
    va_end(ap);
}
void wiz_progress(int pct) { progress = pct; }
void wiz_finish_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(finished, sizeof(finished), fmt, ap);
    va_end(ap);
}
void wiz_finish_shown(json_t *result)
{
    char *t = json_dumps(result, JSON_COMPACT | JSON_SORT_KEYS);
    snprintf(finished, sizeof(finished), "shown %s", t ? t : "");
    free(t);
    shown = 1;
}
const char *wiz_current_id(void) { return current; }
int wiz_ask(const char *kind, const char *pid, const char *text, const char *const *opts, int nopt, char *answer,
            size_t alen)
{
    snprintf(asked, sizeof(asked), "%s %s %s [", kind, pid, text);
    for (int i = 0; i < nopt; i++) {
        strncat(asked, opts[i], sizeof(asked) - strlen(asked) - 1);
        strncat(asked, i + 1 < nopt ? "," : "", sizeof(asked) - strlen(asked) - 1);
    }
    strncat(asked, "]", sizeof(asked) - strlen(asked) - 1);
    snprintf(answer, alen, "%s", answer_given);
    return ask_rc;
}

static void reset(void)
{
    char path[400];
    logged[0] = phase[0] = finished[0] = asked[0] = '\0';
    progress = -1;
    shown = 0;
    ask_rc = 0;
    snprintf(path, sizeof(path), "%s/args", dir);
    unlink(path);
}

static void answers(const char *verb, const char *text)
{
    char path[400];
    snprintf(path, sizeof(path), "%s/%s.json", dir, verb);
    write_file(path, text);
}

static const char *args(void)
{
    char path[400];
    snprintf(path, sizeof(path), "%s/args", dir);
    return read_file(path);
}

static int parse(const char *text, wizpkg_step_t *s, char *why, size_t wlen)
{
    json_t *j = json_loads(text, 0, NULL);
    int rc = wizpkg_parse(j, s, why, wlen);
    json_decref(j);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "list") || !strcmp(argv[1], "wizard")))
        return stand_in(argc, argv);
    if (!mkdtemp(dir))
        return 1;
    char self[PATH_MAX], path[400], text[2048], why[300];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    self[n > 0 ? n : 0] = '\0';
    setenv("FORGECTRL_FORGEEXT", self, 1);
    setenv("WIZPKG_TEST_DIR", dir, 1);
    snprintf(path, sizeof(path), "%s/status.json", dir);
    setenv("FORGECTRL_EXT_STATUS", path, 1);
    wizpkg_step_t s;

    /* Each step held to its form. */
    CHECK(parse("{\"log\": [\"a\", \"b\"], \"phase\": \"looking\", \"progress\": 40, \"prompt\": {\"kind\": \"choice\", "
                "\"id\": \"fan-1\", \"text\": \"Which fan?\", \"options\": [\"left\", \"right\"]}}", &s, why, sizeof(why)) == 0
          && s.nlog == 2 && s.progress == 40 && !strcmp(s.phase, "looking") && s.has_prompt && s.nopt == 2
          && !strcmp(s.opts[1], "right"), "a full step: %s", why);
    CHECK(parse("{\"prompt\": {\"kind\": \"confirm\", \"id\": \"q\", \"text\": \"Sure?\"}}", &s, why, sizeof(why)) == 0
          && s.nopt == 2 && !strcmp(s.opts[0], "Yes") && !strcmp(s.opts[1], "No"), "a confirm gets Yes and No: %s", why);
    CHECK(parse("{\"prompt\": {\"kind\": \"number\", \"id\": \"q\", \"text\": \"How many?\"}}", &s, why, sizeof(why)) == 0
          && s.nopt == 1 && !strcmp(s.opts[0], "OK"), "a number gets OK: %s", why);
    CHECK(parse("{\"result\": {\"ok\": true, \"summary\": \"fine\"}}", &s, why, sizeof(why)) == 0 && s.has_result && s.ok
          && !strcmp(s.summary, "fine"), "a result: %s", why);
    static const char *const bad[] = {
        "[1]", "{}", "{\"prompt\": {\"kind\": \"continue\", \"id\": \"a\", \"text\": \"t\"}, \"result\": {\"ok\": true}}",
        "{\"prompt\": {\"kind\": \"jog\", \"id\": \"a\", \"text\": \"t\"}}",
        "{\"prompt\": {\"kind\": \"multichoice\", \"id\": \"a\", \"text\": \"t\", \"options\": [\"x\", \"y\"]}}",
        "{\"prompt\": {\"kind\": \"continue\", \"id\": \"A b\", \"text\": \"t\"}}",
        "{\"prompt\": {\"kind\": \"continue\", \"id\": \"a\"}}",
        "{\"prompt\": {\"kind\": \"choice\", \"id\": \"a\", \"text\": \"t\", \"options\": [\"only\"]}}",
        "{\"prompt\": {\"kind\": \"choice\", \"id\": \"a\", \"text\": \"t\", \"options\": [\"\", \"y\"]}}",
        "{\"result\": {\"summary\": \"no ok\"}}", "{\"result\": {\"ok\": \"yes\"}}",
        "{\"progress\": 101, \"result\": {\"ok\": true}}", "{\"progress\": 1.5, \"result\": {\"ok\": true}}",
        "{\"log\": \"one line\", \"result\": {\"ok\": true}}", "{\"log\": [1], \"result\": {\"ok\": true}}",
        "{\"log\": [\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\"], \"result\": {\"ok\": true}}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        why[0] = '\0';
        CHECK(parse(bad[i], &s, why, sizeof(why)) != 0 && why[0], "out of form taken: %s", bad[i]);
    }
    snprintf(text, sizeof(text), "{\"prompt\": {\"kind\": \"continue\", \"id\": \"a\", \"text\": \"%0400d\"}}", 0);
    CHECK(parse(text, &s, why, sizeof(why)) != 0, "a prompt's text past 399 bytes taken");

    /* A check run end to end. */
    reset();
    answers("start", "{\"ok\": true, \"id\": \"org.example.check\", \"status\": 200, \"body\": {\"log\": [\"looking\"], "
                     "\"progress\": 50, \"prompt\": {\"kind\": \"confirm\", \"id\": \"fan\", \"text\": \"Is it on?\"}}}");
    answers("answer", "{\"ok\": true, \"id\": \"org.example.check\", \"status\": 200, \"body\": {\"log\": [\"thanks\"], "
                      "\"result\": {\"ok\": true, \"summary\": \"the exhaust runs\"}}}");
    snprintf(answer_given, sizeof(answer_given), "Yes");
    wizpkg_run();
    CHECK(shown && !strcmp(finished, "shown {\"ok\":true,\"summary\":\"the exhaust runs\"}"), "the result: %s", finished);
    CHECK(!strcmp(asked, "confirm fan Is it on? [Yes,No]"), "the prompt asked: %s", asked);
    CHECK(!strcmp(logged, "looking|thanks|") && progress == 50, "the log and the progress: %s %d", logged, progress);
    CHECK(!strcmp(args(), "wizard org.example.check start\nwizard org.example.check answer {\"id\":\"fan\",\"answer\":\"yes\"}\n"),
          "the host was asked: [%s]", args());

    /* A result that did not pass. */
    reset();
    answers("answer", "{\"ok\": true, \"status\": 200, \"body\": {\"result\": {\"ok\": false, \"summary\": \"no exhaust\"}}}");
    wizpkg_run();
    CHECK(!shown && !strcmp(finished, "no exhaust"), "a check that did not pass: %s", finished);

    /* The service's refusal, a step out of form, and an abort: each sends abort. */
    answers("abort", "{\"ok\": true, \"status\": 200, \"body\": {}}");
    reset();
    answers("start", "{\"ok\": true, \"status\": 409, \"body\": {\"error\": \"the fan is not mine\"}}");
    wizpkg_run();
    CHECK(!strcmp(finished, "the check answered 409: the fan is not mine") && strstr(args(), "wizard org.example.check abort"),
          "the service's refusal: %s [%s]", finished, args());
    reset();
    answers("start", "{\"ok\": true, \"status\": 200, \"body\": {\"prompt\": {\"kind\": \"jog\", \"id\": \"x\", \"text\": \"t\"}}}");
    wizpkg_run();
    CHECK(strstr(finished, "kind is continue, confirm, number, or choice") && strstr(args(), "abort"),
          "a step out of form: %s [%s]", finished, args());
    reset();
    answers("start", "{\"ok\": true, \"status\": 200, \"body\": {\"prompt\": {\"kind\": \"continue\", \"id\": \"x\", \"text\": \"t\"}}}");
    ask_rc = -1;
    wizpkg_run();
    CHECK(!strcmp(finished, "aborted") && strstr(args(), "abort"), "an operator's abort: %s [%s]", finished, args());
    reset();
    answers("start", "{\"ok\": false, \"error\": \"its service is frozen while a job is armed\"}");
    wizpkg_run();
    CHECK(!strcmp(finished, "its service is frozen while a job is armed"), "the host's refusal: %s", finished);

    /* The Setup page's list: a host that is alive, a service that runs. */
    snprintf(path, sizeof(path), "%s/list.json", dir);
    write_file(path, "{\"ok\": true, \"packages\": ["
               "{\"id\": \"org.example.check\", \"enabled\": true, \"effective\": [\"wizard\"], \"package\": {\"name\": \"Check\"}},"
               "{\"id\": \"org.example.off\", \"enabled\": false, \"effective\": [\"wizard\"], \"package\": {\"name\": \"Off\"}},"
               "{\"id\": \"org.example.plain\", \"enabled\": true, \"effective\": [\"events\"], \"package\": {\"name\": \"Plain\"}},"
               "{\"id\": \"org.example.stopped\", \"enabled\": true, \"effective\": [\"wizard\"], \"package\": {\"name\": \"S\"}}]}");
    snprintf(text, sizeof(text), "{\"pid\": %d, \"services\": [{\"id\": \"org.example.check\", \"state\": \"running\"}, "
             "{\"id\": \"org.example.off\", \"state\": \"running\"}, {\"id\": \"org.example.plain\", \"state\": \"running\"}, "
             "{\"id\": \"org.example.stopped\", \"state\": \"backoff\"}]}", (int)getpid());
    snprintf(path, sizeof(path), "%s/status.json", dir);
    write_file(path, text);
    answers("state", "{\"ok\": true, \"status\": 200, \"body\": {\"title\": \"The exhaust check\", \"done\": true}}");
    json_t *cat = wizpkg_catalog_json();
    char *ct = json_dumps(cat, JSON_COMPACT | JSON_SORT_KEYS);
    CHECK(ct && !strcmp(ct, "[{\"done\":true,\"id\":\"pkg:org.example.check\",\"title\":\"The exhaust check\"}]"),
          "the list: %s", ct ? ct : "none");
    free(ct);
    json_decref(cat);
    CHECK(wizpkg_known("pkg:org.example.check") && !wizpkg_known("pkg:org.example.plain") && !wizpkg_known("pkg:org.example.off")
          && !wizpkg_known("org.example.check") && !wizpkg_known("pkg:../etc"), "which ids are a package's check");
    write_file(path, "{\"pid\": 1, \"services\": [{\"id\": \"org.example.check\", \"state\": \"running\"}]}");
    CHECK(!wizpkg_known("pkg:org.example.check"), "a host that is not alive lists no check");

    char cmd[340];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0)
        fails++;
    printf(fails ? "wizpkg_test: %d FAILED\n" : "wizpkg_test: all passed\n", fails);
    return fails ? 1 : 0;
}
