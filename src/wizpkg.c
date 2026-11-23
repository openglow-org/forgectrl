/*
 * wizpkg.c - a package's own check on the Setup page
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * See wizpkg.h.
 */
#define _GNU_SOURCE
#include "wizpkg.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "btnres.h"
#include "extpkg.h"
#include "fflog.h"
#include "wizrun.h"

double (*wizpkg_on_file_s)(void) = NULL;
int (*wizpkg_job_waits)(char *why, size_t len) = NULL;

/* A check's reservation outlives its prompt: the wait for it to come on
 * file, then the prompt's own timeout. */
_Static_assert((int)BTNRES_TTL_MAX_S > (int)WIZPKG_READY_WAIT_S + WIZ_PROMPT_TIMEOUT_S,
               "a check's reservation of the button must outlive its prompt");

/* The prompt is shown only once its reservation has stood on file for
 * BTNRES_READY_S with no job that would take the press as its own: both
 * controllers have read it by then, and no job waits that a press could
 * reach. Meanwhile the Setup page names the job that waits. A package
 * that keeps the machine waits for nothing else: its reservation withholds
 * nothing from its own job, whenever it comes to keep it. Every check
 * waits while a package's ask stands (one it ended lingers 5 s). Past
 * WIZPKG_READY_WAIT_S, or on an abort, the check ends: 0 to show the
 * prompt, -1 when it has ended. */
static int ready_wait(const char *id, const char *step_phase)
{
    if (!wizpkg_on_file_s)
        return 0;
    struct timespec step = { 0, 100 * 1000 * 1000 };
    char said[160] = "", jw[160];
    for (double waited = 0;; waited += 0.1) {
        /* A package's ask this check's reservation ended lingers its
         * WIZPKG_LINGER_S (btnres.c, btnres_open): the prompt waits it out,
         * so a press made for that package's prompt is nobody's, never
         * this package's own job's. */
        int lingers = btnres_any("ask:");
        if (!lingers && btnres_pkg_holds && btnres_pkg_holds(id)) {
            if (said[0])
                wiz_phase("%s", step_phase);
            return 0;
        }
        int waits = wizpkg_job_waits && wizpkg_job_waits(jw, sizeof(jw));
        if (!lingers && !waits && wizpkg_on_file_s() >= BTNRES_READY_S) {
            if (said[0])
                wiz_phase("%s", step_phase);
            return 0;
        }
        if (waits && strcmp(said, jw)) {
            snprintf(said, sizeof(said), "%s", jw);
            wiz_phase("%s", jw);
        }
        if (wiz_aborted()) {
            wiz_finish_err("aborted");
            return -1;
        }
        if (waited >= WIZPKG_READY_WAIT_S) {
            if (waits)
                wiz_finish_err("%s", jw);
            else
                wiz_finish_err("the machine's button could not be reserved for the check's question");
            return -1;
        }
        nanosleep(&step, NULL);
    }
}

static int fail(char *why, size_t wlen, const char *text)
{
    snprintf(why, wlen, "%s", text);
    return -1;
}

/* Text of at most max bytes, printable (a line break is a space). */
static int text_of(json_t *v, char *out, size_t max)
{
    const char *s = json_string_value(v);
    if (!s || strlen(s) >= max)
        return -1;
    size_t n = 0;
    for (; s[n]; n++)
        out[n] = (unsigned char)s[n] < 0x20 ? ' ' : s[n];
    out[n] = '\0';
    return 0;
}

static int word_ok(const char *s)
{
    size_t n = strlen(s);
    return n >= 1 && n <= 31 && strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789_-") == n;
}

int wizpkg_parse(json_t *body, wizpkg_step_t *s, char *why, size_t wlen)
{
    memset(s, 0, sizeof(*s));
    s->progress = -1;
    if (!json_is_object(body))
        return fail(why, wlen, "a step of the check is a JSON object");
    json_t *log = json_object_get(body, "log"), *v;
    size_t i;
    if (log && (!json_is_array(log) || json_array_size(log) > WIZPKG_LOG_MAX))
        return fail(why, wlen, "a step's log is a list of at most 8 lines");
    json_array_foreach(log, i, v)
        if (text_of(v, s->log[s->nlog++], sizeof(s->log[0])) != 0)
            return fail(why, wlen, "a step's log line is text of at most 159 bytes");
    json_t *phase = json_object_get(body, "phase"), *pct = json_object_get(body, "progress");
    if (phase && text_of(phase, s->phase, sizeof(s->phase)) != 0)
        return fail(why, wlen, "a step's phase is text of at most 95 bytes");
    if (pct) {
        if (!json_is_integer(pct) || json_integer_value(pct) < 0 || json_integer_value(pct) > 100)
            return fail(why, wlen, "a step's progress is a whole number from 0 to 100");
        s->progress = (int)json_integer_value(pct);
    }
    json_t *p = json_object_get(body, "prompt"), *r = json_object_get(body, "result");
    if (!p == !r)
        return fail(why, wlen, "a step names a prompt or a result, and not both");
    if (r) {
        if (!json_is_object(r) || !json_is_boolean(json_object_get(r, "ok")))
            return fail(why, wlen, "a result says ok, true or false");
        s->has_result = 1;
        s->ok = json_is_true(json_object_get(r, "ok"));
        json_t *sum = json_object_get(r, "summary");
        if (sum && text_of(sum, s->summary, sizeof(s->summary)) != 0)
            return fail(why, wlen, "a result's summary is text of at most 199 bytes");
        return 0;
    }
    static const char *const kinds[] = { "continue", "confirm", "number", "choice" };
    const char *kind = json_string_value(json_object_get(p, "kind")), *pid = json_string_value(json_object_get(p, "id"));
    int known = 0;
    for (size_t k = 0; kind && k < sizeof(kinds) / sizeof(kinds[0]); k++)
        known |= !strcmp(kind, kinds[k]);
    if (!known)
        return fail(why, wlen, "a prompt's kind is continue, confirm, number, or choice");
    if (!pid || !word_ok(pid))
        return fail(why, wlen, "a prompt's id is a word of lowercase letters, digits, - and _, at most 31");
    if (text_of(json_object_get(p, "text"), s->text, sizeof(s->text)) != 0)
        return fail(why, wlen, "a prompt's text is text of at most 399 bytes");
    json_t *opts = json_object_get(p, "options");
    if (opts && (!json_is_array(opts) || json_array_size(opts) > WIZPKG_OPTS_MAX))
        return fail(why, wlen, "a prompt's options are a list of at most 8");
    json_array_foreach(opts, i, v)
        if (text_of(v, s->opts[s->nopt++], sizeof(s->opts[0])) != 0 || !s->opts[s->nopt - 1][0])
            return fail(why, wlen, "a prompt's option is text of 1 to 63 bytes");
    if (!strcmp(kind, "choice") && s->nopt < 2)
        return fail(why, wlen, "a choice has at least two options");
    s->has_prompt = 1;
    snprintf(s->kind, sizeof(s->kind), "%s", kind);
    snprintf(s->pid, sizeof(s->pid), "%s", pid);
    /* The runner's buttons for the kinds that name none. */
    if (s->nopt == 0 && !strcmp(kind, "confirm")) {
        snprintf(s->opts[0], sizeof(s->opts[0]), "Yes");
        snprintf(s->opts[1], sizeof(s->opts[1]), "No");
        s->nopt = 2;
    } else if (s->nopt == 0) {
        snprintf(s->opts[0], sizeof(s->opts[0]), "%s", strcmp(kind, "number") ? "Continue" : "OK");
        s->nopt = 1;
    }
    return 0;
}

/* The service's answer to one request, as a step: 0, or -1 with the words. */
static int step_of(const char *doc, wizpkg_step_t *s, char *why, size_t wlen)
{
    json_t *j = json_loads(doc, 0, NULL);
    json_int_t status = json_integer_value(json_object_get(j, "status"));
    json_t *body = json_object_get(j, "body");
    int rc;
    if (status < 200 || status > 299) {
        const char *e = json_string_value(json_object_get(body, "error"));
        snprintf(why, wlen, "the check answered %lld%s%s", (long long)status, e ? ": " : "", e ? e : "");
        rc = -1;
    } else {
        rc = wizpkg_parse(body, s, why, wlen);
    }
    json_decref(j);
    return rc;
}

void wizpkg_run(void)
{
    const char *wid = wiz_current_id(), *id = wid + strlen(WIZPKG_PREFIX);
    char why[300] = "", answer[64], *doc = extpkg_wizard_json(id, "start", NULL, why, sizeof(why));
    int asked = 0;                      /* a prompt of this run has been shown */
    for (int n = 0; n < WIZPKG_STEPS_MAX; n++) {
        wizpkg_step_t s;
        if (!doc) {
            wiz_finish_err("%s", why);
            return;
        }
        int rc = step_of(doc, &s, why, sizeof(why));
        free(doc);
        if (rc != 0) {
            free(extpkg_wizard_json(id, "abort", NULL, answer, sizeof(answer)));
            wiz_finish_err("%s", why);
            return;
        }
        for (int i = 0; i < s.nlog; i++)
            wiz_log("%s", s.log[i]);
        if (s.phase[0])
            wiz_phase("%s", s.phase);
        if (s.progress >= 0)
            wiz_progress(s.progress);
        if (s.has_result) {
            if (!s.ok) {
                wiz_finish_err("%s", s.summary[0] ? s.summary : "the check did not pass");
                return;
            }
            json_t *r = json_pack("{s:b, s:s}", "ok", 1, "summary", s.summary);
            wiz_finish_shown(r);
            json_decref(r);
            return;
        }
        const char *opts[WIZPKG_OPTS_MAX];
        for (int i = 0; i < s.nopt; i++)
            opts[i] = s.opts[i];
        /* A prompt's words are the package's: any of them may ask for the
         * machine's button. It is reserved for the package while the prompt
         * stands, so a press made for it reaches no other party's job
         * (btnres.h), and the prompt waits until the controllers have read
         * that. The package's own jobs still take it. */
        char who[BTNRES_WHO_MAX], rwhy[96];
        snprintf(who, sizeof(who), "check:%s", id);
        if (btnres_open(who, BTNRES_TTL_MAX_S, rwhy, sizeof(rwhy)) != 0) {
            free(extpkg_wizard_json(id, "abort", NULL, why, sizeof(why)));
            wiz_finish_err("%s", rwhy);
            return;
        }
        if (ready_wait(id, s.phase) != 0) {
            /* An earlier prompt's question may still be on a Setup page:
             * its reservation keeps WIZPKG_LINGER_S from here. */
            if (!asked || btnres_open(who, WIZPKG_LINGER_S, rwhy, sizeof(rwhy)) != 0)
                btnres_close(who);
            free(extpkg_wizard_json(id, "abort", NULL, why, sizeof(why)));
            return;
        }
        asked = 1;
        rc = wiz_ask(s.kind, s.pid, s.text, opts, s.nopt, answer, sizeof(answer));
        /* Kept for WIZPKG_LINGER_S, longer than the page shows the
         * question; a next prompt renews it. */
        if (btnres_open(who, WIZPKG_LINGER_S, rwhy, sizeof(rwhy)) != 0)
            btnres_close(who);
        if (rc != 0) {
            /* A prompt nobody answered already ends the step with its
             * reason (the runner names the timeout); an abort is named
             * here. */
            free(extpkg_wizard_json(id, "abort", NULL, why, sizeof(why)));
            if (rc == -1)
                wiz_finish_err("aborted");
            return;
        }
        if (!strcmp(s.kind, "confirm"))
            snprintf(answer, sizeof(answer), "%s", !strcasecmp(answer, "yes") || !strcmp(answer, "1") ? "yes" : "no");
        json_t *a = json_pack("{s:s, s:s}", "id", s.pid, "answer", answer);
        char *body = a ? json_dumps(a, JSON_COMPACT) : NULL;
        json_decref(a);
        doc = body ? extpkg_wizard_json(id, "answer", body, why, sizeof(why)) : NULL;
        if (!body)
            snprintf(why, sizeof(why), "out of memory");
        free(body);
    }
    free(doc);
    free(extpkg_wizard_json(id, "abort", NULL, why, sizeof(why)));
    wiz_finish_err("the check took more than %d steps", WIZPKG_STEPS_MAX);
}

/* ---- the Setup page's list ---- */

static pthread_mutex_t cache_mu = PTHREAD_MUTEX_INITIALIZER;
static json_t *cache;
static time_t cache_at;

static json_t *build(void)
{
    json_t *out = json_array(), *list = extpkg_wizard_list(), *p;
    size_t i;
    json_array_foreach(list, i, p) {
        const char *id = json_string_value(json_object_get(p, "id"));
        char why[200], wid[72];
        char *doc = extpkg_wizard_json(id, "state", NULL, why, sizeof(why));
        json_t *j = doc ? json_loads(doc, 0, NULL) : NULL, *body = json_object_get(j, "body");
        free(doc);
        const char *title = json_string_value(json_object_get(body, "title"));
        snprintf(wid, sizeof(wid), WIZPKG_PREFIX "%s", id);
        json_t *e = json_object();
        json_object_set_new(e, "id", json_string(wid));
        json_object_set_new(e, "title", json_string(title && title[0] && strlen(title) < 80 ? title
                                                    : json_string_value(json_object_get(p, "name"))));
        json_object_set_new(e, "done", json_boolean(json_is_true(json_object_get(body, "done"))));
        if (!body)
            json_object_set_new(e, "unanswered", json_string(why));
        json_array_append_new(out, e);
        json_decref(j);
    }
    json_decref(list);
    return out;
}

json_t *wizpkg_catalog_json(void)
{
    pthread_mutex_lock(&cache_mu);
    if (!cache || time(NULL) - cache_at >= WIZPKG_CACHE_S) {
        json_decref(cache);
        cache = build();
        cache_at = time(NULL);
    }
    json_t *out = json_deep_copy(cache);
    pthread_mutex_unlock(&cache_mu);
    return out ? out : json_array();
}

int wizpkg_known(const char *id)
{
    size_t n = strlen(WIZPKG_PREFIX);
    if (!id || strncmp(id, WIZPKG_PREFIX, n) != 0 || !extpkg_id_ok(id + n))
        return 0;
    json_t *list = extpkg_wizard_list(), *p;
    size_t i;
    int found = 0;
    json_array_foreach(list, i, p)
        found |= !strcmp(json_string_value(json_object_get(p, "id")) ?: "", id + n);
    json_decref(list);
    return found;
}
