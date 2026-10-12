/*
 * setup_test.c - host test: the setup record and the gate
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Against a scratch data and run directory: a fresh record closes the
 * gate and serves the wizard; consent needs every document at its
 * current hash and then the press; a changed document re-opens the
 * question; the account and the intake fan count open the gate and
 * nothing else closes it;
 * the step table and its versions, the flags, and a part change fill
 * the recommended list with their reasons; a retired step's flag is
 * left out; a damaged record reads as nothing done; the status document
 * parses and says what the gate says; a record under the file's earlier
 * name is adopted once and never over a current one; the cloud step's
 * homing choices.
 */
#include "../src/advisories.h"
#include "../src/setup.h"
#include "../src/sheetid.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

unsigned long fuse_serial(void) { return 123456789; }
static int fact_yes(const char *fact) { return !strcmp(fact, "cloud_enabled"); }

static int failures;
static char data_dir[] = "/tmp/setup-data-XXXXXX";
static char run_dir[] = "/tmp/setup-run-XXXXXX";

#define CHECK(cond, name) do { \
    if (cond) printf("ok   %s\n", name); \
    else { printf("FAIL %s\n", name); failures++; } } while (0)

static json_t *status(void)
{
    static char buf[8192];
    if (setup_status_json(buf, sizeof(buf)) < 0)
        return NULL;
    return json_loads(buf, 0, NULL);
}

static const char *sget(json_t *o, const char *k)
{
    const char *v = json_string_value(json_object_get(o, k));
    return v ? v : "";
}

/* The reason the status gives for recommending a step, or NULL when it
 * is not recommended. */
static const char *rec_reason(const char *id)
{
    static char out[160];
    json_t *st = status();
    const char *found = NULL;
    size_t i;
    json_t *e;
    json_array_foreach(json_object_get(st, "recommended"), i, e)
        if (!strcmp(sget(e, "id"), id)) {
            snprintf(out, sizeof(out), "%s", sget(e, "reason"));
            found = out;
        }
    json_decref(st);
    return found;
}

/* A build that has no sheet steps any more. */
static int known_not_sheet(const char *id) { return strncmp(id, "sheet.", 6) != 0; }
/* The intake fan count, unknown and known. */
static int fans_unknown(void) { return 0; }
static int fans_known(void) { return 1; }

static void accept_all(void)
{
    size_t n;
    const advisory_t *docs = advisories_list(&n);
    for (size_t i = 0; i < n; i++)
        setup_advisory_accept(docs[i].id, docs[i].hash, docs[i].consent);
}

int main(void)
{
    if (!mkdtemp(data_dir) || !mkdtemp(run_dir)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    setenv("FORGECTRL_DATA_DIR", data_dir, 1);
    setenv("GF_RUN_DIR", run_dir, 1);
    advisories_init();
    sheetid_init();

    static const setup_step_t steps[] = {
        { "advisories", 1, NULL, 0 }, { "account", 1, NULL, 0 }, { "preferences", 1, NULL, 0 },
    };

    /* 1. A fresh record: first run, gate closed, why = advisories. */
    setup_init();
    setup_set_steps(steps, 3);
    char why[256];
    CHECK(setup_first_run(), "fresh record is a first run");
    CHECK(!setup_gate_open(why, sizeof(why)), "fresh record: gate closed");
    CHECK(strstr(why, "advisories") != NULL, "why names the advisories");
    json_t *st = status();
    CHECK(st && !strcmp(sget(st, "gate"), "closed"), "status says closed");
    CHECK(st && json_array_size(json_object_get(st, "documents")) == 4,
          "status lists four documents");
    CHECK(st && strlen(sget(st, "sheet_id")) == SHEETID_LEN, "status carries the sheet id");
    json_decref(st);

    /* 2. Consent: a wrong hash is refused; all four accepted is not yet
     * consent (the press is missing); then the press. */
    size_t n;
    const advisory_t *docs = advisories_list(&n);
    CHECK(setup_advisory_accept(docs[0].id, "0000", "typed") == -1,
          "a stale hash is refused");
    CHECK(setup_acceptance_pressed() == -1, "a press before the documents is refused");
    accept_all();
    CHECK(setup_advisories_complete(), "every document accepted");
    CHECK(!setup_acceptance_done(), "not pressed yet");
    CHECK(!setup_gate_open(why, sizeof(why)), "documents alone do not open the gate");
    CHECK(setup_acceptance_pressed() == 0, "the press is recorded");
    CHECK(setup_acceptance_done(), "pressed");
    setup_gate_open(why, sizeof(why));
    CHECK(strstr(why, "account") != NULL, "why now names the account");

    /* 3. A new acceptance of one document takes the press back. */
    setup_advisory_accept(docs[1].id, docs[1].hash, "check");
    CHECK(!setup_acceptance_done(), "a re-acceptance needs the press again");
    setup_acceptance_pressed();

    /* 3b. An on-demand document is no part of the first run: it is not
     * in the list, the first-run acceptance refuses it, and its own
     * acceptance is recorded beside the others and leaves the press. */
    {
        const advisory_t *x = advisories_find("extensions");
        int listed = 0;
        for (size_t i = 0; i < n; i++)
            if (docs[i].on_demand || !strcmp(docs[i].id, "extensions"))
                listed = 1;
        CHECK(x && x->on_demand && x->hash[0] && x->len > 0, "the Extensions advisory is on demand");
        CHECK(n == 4 && !listed, "the first-run list carries no on-demand document");
        CHECK(x && setup_advisory_accept("extensions", x->hash, "typed") == -1,
              "the first-run acceptance refuses an on-demand document");
        CHECK(setup_acceptance_done(), "the refusal left the press");
        CHECK(setup_on_demand_accept("extensions", "0000", "typed") == -1,
              "an on-demand acceptance with a stale hash is refused");
        CHECK(setup_on_demand_accept(docs[0].id, docs[0].hash, "typed") == -1,
              "an on-demand acceptance of a first-run document is refused");
        CHECK(x && setup_on_demand_accept("extensions", x->hash, "typed") == 0,
              "the on-demand acceptance is recorded");
        CHECK(setup_acceptance_done() && setup_advisories_complete(),
              "it leaves the first-run documents and their press as they are");
        json_t *r = setup_record_copy();
        json_t *e = json_object_get(json_object_get(r, "on_demand"), "extensions");
        CHECK(x && e && !strcmp(sget(e, "hash"), x->hash) && !strcmp(sget(e, "method"), "typed") &&
              sget(e, "accepted")[0], "the record holds the hash, the method, and the time");
        CHECK(!json_object_get(json_object_get(r, "advisories"), "extensions"),
              "and not among the first-run ones");
        json_decref(r);
    }

    /* 4. The account opens the gate: every step after it is recommended,
     * never required. */
    CHECK(setup_set_account("owner", 1000) == 0, "account recorded");
    CHECK(setup_gate_open(why, sizeof(why)), "the advisories and the account open the gate");
    CHECK(why[0] == '\0', "an open gate has no why");
    CHECK(rec_reason("preferences") && !strcmp(rec_reason("preferences"), "not run yet"),
          "a step not run is recommended: not run yet");
    CHECK(!rec_reason("advisories") && !rec_reason("account"),
          "the advisories and the account are the gate, never recommended");
    CHECK(setup_recommended_count() == 1, "one step recommended");
    setup_wizard_done("advisories", 1, NULL, NULL);
    setup_wizard_done("account", 1, NULL, NULL);
    json_t *result = json_pack("{s:s}", "units", "metric");
    json_t *applied = json_pack("{s:{s:s,s:s}}", "ui_units", "from", "", "to", "metric");
    setup_wizard_done("preferences", 1, result, applied);
    CHECK(setup_recommended_count() == 0, "every step done: nothing recommended");
    CHECK(setup_first_run(), "still the first run until the guided run ends");
    setup_complete();
    CHECK(!setup_first_run(), "complete ends the first run");

    /* 4b. A required step holds the gate until it is done at its version,
     * says so in the why, and is never listed as recommended; a bumped
     * version holds it again; a flag on it recommends it and leaves the
     * gate as the version says. */
    static const setup_step_t steps_req[] = {
        { "advisories", 1, NULL, 0 }, { "account", 1, NULL, 0 }, { "preferences", 1, NULL, 0 },
        { "machine", 1, NULL, 1 },
    };
    static const setup_step_t steps_req2[] = {
        { "advisories", 1, NULL, 0 }, { "account", 1, NULL, 0 }, { "preferences", 1, NULL, 0 },
        { "machine", 2, NULL, 1 },
    };
    setup_set_steps(steps_req, 4);
    CHECK(!setup_gate_open(why, sizeof(why)) && strstr(why, "setup required: machine"),
          "a required step not done holds the gate and names it");
    CHECK(!rec_reason("machine"), "a required step is the gate, never a recommendation");
    setup_wizard_done("machine", 1, NULL, NULL);
    CHECK(setup_gate_open(why, sizeof(why)), "the required step done: the gate opens");
    setup_flag("machine", "the head changed");
    CHECK(setup_gate_open(why, sizeof(why)) && rec_reason("machine"),
          "a flag on a done required step recommends it and leaves the gate open");
    setup_flag("machine", NULL);
    setup_set_steps(steps_req2, 4);
    CHECK(!setup_gate_open(why, sizeof(why)) && strstr(why, "machine"),
          "a required step bumped by the image holds the gate again");
    setup_wizard_done("machine", 2, NULL, NULL);
    CHECK(setup_gate_open(why, sizeof(why)), "run at the new version: open");
    setup_set_steps(steps, 3);

    /* 5. A version bump on the image recommends the step again and
     * leaves the gate open. */
    static const setup_step_t steps2[] = {
        { "advisories", 1, NULL, 0 }, { "account", 1, NULL, 0 }, { "preferences", 2, NULL, 0 },
    };
    setup_set_steps(steps2, 3);
    CHECK(setup_gate_open(why, sizeof(why)), "a bumped version leaves the gate open");
    CHECK(rec_reason("preferences") && strstr(rec_reason("preferences"), "release"),
          "a bumped version is recommended: this release asks for it again");

    /* 5b. A conditional step stands only on a machine with the fact: a
     * check that needs the machine block's tec fact. */
    static const setup_step_t steps3[] = {
        { "advisories", 1, NULL, 0 }, { "account", 1, NULL, 0 }, { "preferences", 1, NULL, 0 },
        { "example.tec", 1, "tec", 0 },
    };
    setup_set_steps(steps3, 4);
    setup_set_machine(json_pack("{s:b,s:s,s:{s:s}}", "tec", 0, "model", "basic",
                                     "head", "serial_hash", "ABCDE-FGHIJ"));
    CHECK(!rec_reason("example.tec"), "tec false: the check is not recommended");
    CHECK(!setup_machine_bool("tec"), "the machine fact reads false");
    char mstr[24], hh[32];
    setup_machine_str("model", mstr, sizeof(mstr));
    CHECK(!strcmp(mstr, "basic"), "the machine model reads back");
    setup_head_hash(hh, sizeof(hh));
    CHECK(!strcmp(hh, "ABCDE-FGHIJ"), "the head hash reads back");
    setup_set_machine(json_pack("{s:b,s:s}", "tec", 1, "model", "pro"));
    CHECK(rec_reason("example.tec") && setup_gate_open(why, sizeof(why)),
          "tec true: the check is recommended, and the gate stays open");
    /* 5c. A fact outside the machine block comes from the hook. */
    static const setup_step_t steps4[] = {
        { "advisories", 1, NULL, 0 }, { "account", 1, NULL, 0 }, { "preferences", 1, NULL, 0 },
        { "cloud.header", 1, "cloud_enabled", 0 },
    };
    setup_set_steps(steps4, 4);
    setup_fact_hook = NULL;
    CHECK(!rec_reason("cloud.header"), "no hook: the cloud header is not recommended");
    setup_fact_hook = fact_yes;
    CHECK(rec_reason("cloud.header") != NULL, "the hook says cloud is on: the cloud header is recommended");
    setup_fact_hook = NULL;
    setup_set_steps(steps2, 3);
    setup_wizard_done("preferences", 2, NULL, NULL);
    CHECK(!rec_reason("preferences"), "a re-run at the new version is no longer recommended");
    CHECK(setup_wizard_version("preferences") == 2, "version recorded");

    /* 6. Flags recommend a step with their reason, never close the gate,
     * and a completed run clears them; a NULL reason clears one. */
    setup_flag("preferences", "fault twice");
    CHECK(setup_gate_open(why, sizeof(why)), "a flag keeps the gate open");
    CHECK(rec_reason("preferences") && !strcmp(rec_reason("preferences"), "fault twice"),
          "the flag is recommended with its reason");
    setup_wizard_done("preferences", 2, NULL, NULL);
    CHECK(!rec_reason("preferences"), "a completed run clears the flag");
    setup_flag("preferences", "again");
    setup_flag("preferences", NULL);
    CHECK(!rec_reason("preferences"), "a NULL reason clears the flag");

    /* 6b. A flag on a step this build no longer has is left out, and only
     * when the build says which steps it has. */
    setup_flag("sheet.place", "kept from an older build");
    setup_known_hook = NULL;
    CHECK(rec_reason("sheet.place") != NULL, "no catalog hook: every flag is listed");
    setup_known_hook = known_not_sheet;
    CHECK(!rec_reason("sheet.place") && setup_recommended_count() == 0,
          "a retired step's flag is left out of the list and the count");
    setup_known_hook = NULL;
    setup_flag("sheet.place", NULL);

    /* 6c. Nothing but the advisories and the account closes the gate:
     * without the account it closes, with it back it opens. */
    setup_flag("preferences", "fault twice");
    setup_clear_account();
    CHECK(!setup_gate_open(why, sizeof(why)) && strstr(why, "account"),
          "without the account the gate closes and says why");
    setup_set_account("owner", 1000);
    CHECK(setup_gate_open(why, sizeof(why)), "the account back: open, the flag still listed");
    setup_flag("preferences", NULL);

    /* 6d. The intake fan count is part of the gate: with the advisories
     * and the account, an uncounted machine still holds, and says why;
     * the count opens it. */
    setup_fans_hook = fans_unknown;
    CHECK(!setup_gate_open(why, sizeof(why)) && strstr(why, "intake fans"),
          "an uncounted machine holds the gate and says why");
    st = status();
    CHECK(st && json_is_false(json_object_get(st, "fans_counted")) && !strcmp(sget(st, "gate"), "closed"),
          "the status says the fans are not counted and the gate is closed");
    json_decref(st);
    setup_fans_hook = fans_known;
    CHECK(setup_gate_open(why, sizeof(why)), "the count opens the gate");
    st = status();
    CHECK(st && json_is_true(json_object_get(st, "fans_counted")), "the status says the fans are counted");
    json_decref(st);
    setup_fans_hook = NULL;

    /* 7. What changed: a replaced part recommends the steps that
     * measured it or prove it, with the change as the reason; a run
     * clears it; an unknown change is refused; the menu is in the
     * status route, not the status document. */
    CHECK(setup_change_apply("nothing-like-this") == -1, "an unknown change is refused");
    size_t nch = 0;
    const setup_change_t *ch = setup_changes(&nch);
    CHECK(nch >= 7 && ch && !strcmp(ch[0].id, "tube"), "the menu starts with the tube");
    static const setup_step_t steps_tube[] = {
        { "advisories", 1, NULL, 0 }, { "account", 1, NULL, 0 }, { "preferences", 2, NULL, 0 },
    };
    setup_set_steps(steps_tube, 3);
    setup_wizard_done("laser.floor", 1, NULL, NULL);
    CHECK(setup_change_apply("tube") == 0, "the tube change applies");
    CHECK(setup_gate_open(why, sizeof(why)), "a new tube leaves the gate open");
    CHECK(rec_reason("laser.floor") && strstr(rec_reason("laser.floor"), "tube"),
          "a new tube recommends the floor with the tube as the reason");
    st = status();
    CHECK(st && !json_object_get(st, "required") && !json_object_get(st, "override"),
          "the status carries no required list and no override");
    CHECK(st && (json_is_array(json_object_get(st, "changes")) == 0 ||
                 json_array_size(json_object_get(st, "changes")) == 0),
          "the status document itself carries no menu (the route adds it)");
    json_decref(st);
    json_t *menu = setup_changes_json();
    json_t *tube = json_array_get(menu, 0);
    json_t *tw = json_object_get(tube, "wizards");
    CHECK(json_is_array(menu) && json_array_size(menu) == nch && json_array_size(tw) == 1 &&
          !strcmp(sget(json_array_get(tw, 0), "id"), "laser.floor") &&
          !json_object_get(json_array_get(tw, 0), "level"),
          "the menu JSON lists every change with its steps, with no level");
    json_decref(menu);
    CHECK(setup_change_apply("head") == 0 && rec_reason("lens.travel") &&
          rec_reason("laser.focus") && rec_reason("machine"),
          "a new head recommends the machine facts, the lens travel, and the focus");
    setup_wizard_done("laser.floor", 1, NULL, NULL);
    CHECK(!rec_reason("laser.floor"), "the floor run again: no longer recommended");
    const char *head[] = { "machine", "lens.travel", "laser.focus", "motion", "cameras" };
    for (size_t i = 0; i < sizeof(head) / sizeof(*head); i++)
        setup_flag(head[i], NULL);
    setup_set_steps(steps, 3);
    FILE *f;

    /* 8. Persistence: a reload sees the same state. */
    setup_init();
    CHECK(!setup_first_run() && setup_gate_open(why, sizeof(why)),
          "the record survives a reload");
    char rec[16384];
    CHECK(setup_record_json(rec, sizeof(rec)) > 0 && strstr(rec, "\"owner\""),
          "the record JSON carries the account name");
    CHECK(strstr(rec, "\"hash\"") == NULL || strstr(rec, "$6$") == NULL,
          "no password hash in the record");
    char *dump = setup_record_dump(1);
    CHECK(dump && strstr(dump, "\"owner\"") && strchr(dump, '\n'),
          "the malloc'd dump is the record, indented");
    free(dump);
    dump = setup_record_dump(0);
    CHECK(dump && !strchr(dump, '\n'), "the compact dump is one line");
    free(dump);
    json_t *copy = setup_record_copy();
    CHECK(copy && json_is_object(json_object_get(copy, "wizards")), "the record copy is the record");
    json_decref(copy);

    /* 9. A damaged record reads as nothing done. */
    char path[300];
    snprintf(path, sizeof(path), "%s/setup.json", data_dir);
    f = fopen(path, "w");
    if (f) {
        fputs("{ this is not json", f);
        fclose(f);
    }
    setup_init();
    CHECK(setup_first_run() && !setup_gate_open(why, sizeof(why)),
          "a damaged record closes the gate and serves the wizard");
    unlink(path);

    /* 10. A record under the earlier name is adopted once: with no
     * current record, the old file becomes the record (its consent and
     * account stand); with a current record, the old file is left as
     * it is. */
    char old[300];
    snprintf(old, sizeof(old), "%s/commissioning.json", data_dir);
    setup_init();
    accept_all();
    setup_acceptance_pressed();
    setup_set_account("owner", 1000);
    if (rename(path, old) != 0)
        printf("FAIL rename to the earlier name\n"), failures++;
    setup_init();
    CHECK(access(old, F_OK) != 0 && access(path, F_OK) == 0,
          "the earlier record moved to the current name");
    CHECK(setup_acceptance_done(), "the adopted record keeps the consent");
    st = status();
    CHECK(st && json_is_true(json_object_get(st, "account")),
          "the adopted record keeps the account");
    json_decref(st);
    f = fopen(old, "w");
    if (f) {
        fputs("{\"schema\":1,\"stale\":true}", f);
        fclose(f);
    }
    setup_init();
    CHECK(access(old, F_OK) == 0 && setup_acceptance_done(),
          "an earlier record never replaces a current one");
    unlink(old);

    /* 11. The cloud step's homing choices: the camera home, a home by
     * hand, none; nothing else. */
    CHECK(setup_cloud_homing_ok("gfcloud") && setup_cloud_homing_ok("none"),
          "the cloud step takes the camera home and none");
    CHECK(setup_cloud_homing_ok("manual"), "the cloud step keeps a home by hand");
    CHECK(!setup_cloud_homing_ok("switches") && !setup_cloud_homing_ok("") &&
          !setup_cloud_homing_ok("Manual") && !setup_cloud_homing_ok(NULL),
          "the cloud step refuses anything else");

    /* Clean up. */
    unlink(path);
    snprintf(path, sizeof(path), "%s/sheet.salt", data_dir);
    unlink(path);
    rmdir(data_dir);
    rmdir(run_dir);
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
