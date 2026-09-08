/*
 * commission_test.c - host test: the commissioning record and the gate
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Against a scratch data and run directory: a fresh record closes the
 * gate and serves the wizard; consent needs every document at its
 * current hash and then the press; a changed document re-opens the
 * question; the account; the required table and its versions; an
 * engine flag; the override file lifts the wizard part only; a damaged
 * record reads as nothing done; the status document parses and says
 * what the gate says.
 */
#include "../src/advisories.h"
#include "../src/commission.h"
#include "../src/sheetid.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

unsigned long fuse_serial(void) { return 123456789; }
static int fact_yes(const char *fact) { return !strcmp(fact, "cloud_enabled"); }

static int failures;
static char data_dir[] = "/tmp/commission-data-XXXXXX";
static char run_dir[] = "/tmp/commission-run-XXXXXX";

#define CHECK(cond, name) do { \
    if (cond) printf("ok   %s\n", name); \
    else { printf("FAIL %s\n", name); failures++; } } while (0)

static json_t *status(void)
{
    static char buf[8192];
    if (commission_status_json(buf, sizeof(buf)) < 0)
        return NULL;
    return json_loads(buf, 0, NULL);
}

static const char *sget(json_t *o, const char *k)
{
    const char *v = json_string_value(json_object_get(o, k));
    return v ? v : "";
}

static void accept_all(void)
{
    size_t n;
    const advisory_t *docs = advisories_list(&n);
    for (size_t i = 0; i < n; i++)
        commission_advisory_accept(docs[i].id, docs[i].hash, docs[i].consent);
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

    static const commission_req_t reqs[] = {
        { "advisories", 1, NULL }, { "account", 1, NULL }, { "preferences", 1, NULL },
    };

    /* 1. A fresh record: first run, gate closed, why = advisories. */
    commission_init();
    commission_set_required(reqs, 3);
    char why[256];
    CHECK(commission_first_run(), "fresh record is a first run");
    CHECK(!commission_gate_open(why, sizeof(why)), "fresh record: gate closed");
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
    CHECK(commission_advisory_accept(docs[0].id, "0000", "typed") == -1,
          "a stale hash is refused");
    CHECK(commission_acceptance_pressed() == -1, "a press before the documents is refused");
    accept_all();
    CHECK(commission_advisories_complete(), "every document accepted");
    CHECK(!commission_acceptance_done(), "not pressed yet");
    CHECK(!commission_gate_open(why, sizeof(why)), "documents alone do not open the gate");
    CHECK(commission_acceptance_pressed() == 0, "the press is recorded");
    CHECK(commission_acceptance_done(), "pressed");
    commission_gate_open(why, sizeof(why));
    CHECK(strstr(why, "account") != NULL, "why now names the account");

    /* 3. A new acceptance of one document takes the press back. */
    commission_advisory_accept(docs[1].id, docs[1].hash, "check");
    CHECK(!commission_acceptance_done(), "a re-acceptance needs the press again");
    commission_acceptance_pressed();

    /* 4. The account, then the required wizards. */
    CHECK(commission_set_account("owner", 1000) == 0, "account recorded");
    CHECK(!commission_gate_open(why, sizeof(why)), "required wizards still close the gate");
    CHECK(strstr(why, "preferences") != NULL, "why names the missing wizard");
    commission_wizard_done("advisories", 1, NULL, NULL);
    commission_wizard_done("account", 1, NULL, NULL);
    json_t *result = json_pack("{s:s}", "units", "metric");
    json_t *applied = json_pack("{s:{s:s,s:s}}", "ui_units", "from", "", "to", "metric");
    commission_wizard_done("preferences", 1, result, applied);
    CHECK(commission_gate_open(why, sizeof(why)), "every required wizard done: gate open");
    CHECK(commission_first_run(), "still the first run until complete");
    commission_complete();
    CHECK(!commission_first_run(), "complete ends the first run");

    /* 5. A version bump on the image requires the wizard again. */
    static const commission_req_t reqs2[] = {
        { "advisories", 1, NULL }, { "account", 1, NULL }, { "preferences", 2, NULL },
    };
    commission_set_required(reqs2, 3);
    CHECK(!commission_gate_open(why, sizeof(why)), "a bumped version closes the gate");
    CHECK(strstr(why, "preferences") != NULL, "why names the outdated wizard");

    /* 5b. A conditional requirement stands only on a machine with the
     * fact: the TEC wizard on a machine with a TEC. */
    static const commission_req_t reqs3[] = {
        { "advisories", 1, NULL }, { "account", 1, NULL }, { "preferences", 1, NULL },
        { "cooling.tec", 1, "tec" },
    };
    commission_set_required(reqs3, 4);
    commission_set_machine(json_pack("{s:b,s:s,s:{s:s}}", "tec", 0, "model", "basic",
                                     "head", "serial_hash", "ABCDE-FGHIJ"));
    CHECK(commission_gate_open(why, sizeof(why)), "no TEC: the TEC wizard is not required");
    {
        json_t *st = status();
        char *reqtext = json_dumps(json_object_get(st, "required"), JSON_COMPACT);
        CHECK(reqtext && !strstr(reqtext, "cooling.tec"), "the status lists no TEC requirement without a TEC");
        free(reqtext);
        json_decref(st);
    }
    CHECK(!commission_machine_bool("tec"), "the machine fact reads false");
    char mstr[24], hh[32];
    commission_machine_str("model", mstr, sizeof(mstr));
    CHECK(!strcmp(mstr, "basic"), "the machine model reads back");
    commission_head_hash(hh, sizeof(hh));
    CHECK(!strcmp(hh, "ABCDE-FGHIJ"), "the head hash reads back");
    commission_set_machine(json_pack("{s:b,s:s}", "tec", 1, "model", "pro"));
    CHECK(!commission_gate_open(why, sizeof(why)) && strstr(why, "cooling.tec"),
          "with a TEC the TEC wizard is required");
    /* 5c. A fact outside the machine block comes from the hook. */
    static const commission_req_t reqs4[] = {
        { "advisories", 1, NULL }, { "account", 1, NULL }, { "preferences", 1, NULL },
        { "cloud.header", 1, "cloud_enabled" },
    };
    commission_set_required(reqs4, 4);
    commission_fact_hook = NULL;
    CHECK(commission_gate_open(why, sizeof(why)), "no hook: the cloud header is not required");
    commission_fact_hook = fact_yes;
    commission_set_required(reqs4, 4);
    CHECK(!commission_gate_open(why, sizeof(why)) && strstr(why, "cloud.header"),
          "the hook says cloud is on: the cloud header is required");
    commission_fact_hook = NULL;
    commission_set_required(reqs2, 3);
    commission_wizard_done("preferences", 2, NULL, NULL);
    CHECK(commission_gate_open(why, sizeof(why)), "re-run at the new version reopens");
    CHECK(commission_wizard_version("preferences") == 2, "version recorded");

    /* 6. Engine flags: recommended does not gate, required does, a
     * completed run clears it. */
    commission_flag("preferences", "recommended", "thin margin");
    CHECK(commission_gate_open(why, sizeof(why)), "a recommendation keeps the gate open");
    commission_flag("preferences", "required", "fault twice");
    CHECK(!commission_gate_open(why, sizeof(why)), "a required flag closes the gate");
    st = status();
    CHECK(st && json_array_size(json_object_get(st, "required")) == 1,
          "status lists the required flag");
    json_decref(st);
    commission_wizard_done("preferences", 2, NULL, NULL);
    CHECK(commission_gate_open(why, sizeof(why)), "a completed run clears the flag");
    CHECK(commission_flag("x", "bogus", "") == -1, "a bogus level is refused");

    /* 7. The override lifts the wizard part only. */
    commission_flag("preferences", "required", "again");
    char ov[300];
    snprintf(ov, sizeof(ov), "%s/commissioning-override", run_dir);
    FILE *f = fopen(ov, "w");
    if (f)
        fclose(f);
    CHECK(commission_override_active(), "override file seen");
    CHECK(commission_gate_open(why, sizeof(why)), "override opens a wizard-gated machine");
    CHECK(strstr(why, "override") != NULL, "why says override");
    commission_clear_account();
    CHECK(!commission_gate_open(why, sizeof(why)), "override never lifts the account");
    unlink(ov);
    commission_set_account("owner", 1000);
    CHECK(!commission_gate_open(why, sizeof(why)), "without the override the flag gates again");
    commission_flag("preferences", NULL, NULL);
    CHECK(commission_gate_open(why, sizeof(why)), "flag cleared");

    /* 7b. What changed: a replaced part flags the wizards that measured
     * it as required and the ones that prove it as recommended; a
     * required flag never drops to recommended; a run clears it; an
     * unknown change is refused; the menu is in the status. */
    CHECK(commission_change_apply("nothing-like-this") == -1, "an unknown change is refused");
    size_t nch = 0;
    const commission_change_t *ch = commission_changes(&nch);
    CHECK(nch >= 7 && ch && !strcmp(ch[0].id, "tube"), "the menu starts with the tube");
    static const commission_req_t reqs_tube[] = {
        { "advisories", 1, NULL }, { "account", 1, NULL }, { "preferences", 2, NULL },
        { "laser.floor", 1, NULL }, { "laser.corner", 1, NULL },
    };
    commission_set_required(reqs_tube, 5);
    commission_wizard_done("laser.floor", 1, NULL, NULL);
    commission_wizard_done("laser.corner", 1, NULL, NULL);
    CHECK(commission_gate_open(why, sizeof(why)), "the floor and the corner done: open");
    CHECK(commission_change_apply("tube") == 0, "the tube change applies");
    CHECK(!commission_gate_open(why, sizeof(why)) && strstr(why, "laser.floor"),
          "a new tube requires the floor again");
    st = status();
    {
        json_t *rq = json_object_get(st, "required"), *rm = json_object_get(st, "recommended");
        int floor_req = 0, corner_rec = 0;
        size_t i;
        json_t *e;
        json_array_foreach(rq, i, e)
            if (json_is_object(e) && !strcmp(sget(e, "id"), "laser.floor") &&
                strstr(sget(e, "reason"), "tube"))
                floor_req = 1;
        json_array_foreach(rm, i, e)
            if (json_is_object(e) && !strcmp(sget(e, "id"), "laser.corner"))
                corner_rec = 1;
        CHECK(floor_req, "the floor is required with the tube as the reason");
        CHECK(corner_rec, "the corner card is recommended");
        CHECK(json_is_array(json_object_get(st, "changes")) == 0 ||
              json_array_size(json_object_get(st, "changes")) == 0,
              "the status document itself carries no menu (the route adds it)");
    }
    json_decref(st);
    json_t *menu = commission_changes_json();
    CHECK(json_is_array(menu) && json_array_size(menu) == nch &&
          json_array_size(json_object_get(json_array_get(menu, 0), "wizards")) == 4,
          "the menu JSON lists every change with its wizards");
    json_decref(menu);
    commission_flag("laser.corner", "required", "by hand");
    commission_change_apply("tube");
    st = status();
    {
        int corner_req = 0;
        size_t i;
        json_t *e;
        json_array_foreach(json_object_get(st, "required"), i, e)
            if (json_is_object(e) && !strcmp(sget(e, "id"), "laser.corner"))
                corner_req = 1;
        CHECK(corner_req, "a required flag never drops to recommended");
    }
    json_decref(st);
    commission_wizard_done("laser.floor", 1, NULL, NULL);
    commission_wizard_done("laser.corner", 1, NULL, NULL);
    CHECK(commission_gate_open(why, sizeof(why)), "the cards run again: the gate reopens");
    commission_set_required(reqs, 3);

    /* 8. Persistence: a reload sees the same state. */
    commission_init();
    CHECK(!commission_first_run() && commission_gate_open(why, sizeof(why)),
          "the record survives a reload");
    char rec[16384];
    CHECK(commission_record_json(rec, sizeof(rec)) > 0 && strstr(rec, "\"owner\""),
          "the record JSON carries the account name");
    CHECK(strstr(rec, "\"hash\"") == NULL || strstr(rec, "$6$") == NULL,
          "no password hash in the record");
    char *dump = commission_record_dump(1);
    CHECK(dump && strstr(dump, "\"owner\"") && strchr(dump, '\n'),
          "the malloc'd dump is the record, indented");
    free(dump);
    dump = commission_record_dump(0);
    CHECK(dump && !strchr(dump, '\n'), "the compact dump is one line");
    free(dump);
    json_t *copy = commission_record_copy();
    CHECK(copy && json_is_object(json_object_get(copy, "wizards")), "the record copy is the record");
    json_decref(copy);

    /* 9. A damaged record reads as nothing done. */
    char path[300];
    snprintf(path, sizeof(path), "%s/commissioning.json", data_dir);
    f = fopen(path, "w");
    if (f) {
        fputs("{ this is not json", f);
        fclose(f);
    }
    commission_init();
    CHECK(commission_first_run() && !commission_gate_open(why, sizeof(why)),
          "a damaged record closes the gate and serves the wizard");

    /* Clean up. */
    unlink(path);
    snprintf(path, sizeof(path), "%s/sheet.salt", data_dir);
    unlink(path);
    rmdir(data_dir);
    rmdir(run_dir);
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
