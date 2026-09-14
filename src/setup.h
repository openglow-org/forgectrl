/*
 * setup.h - the setup record and the controller gate
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The record (setup.json in the data directory) is the
 * provenance the settings file lacks: which advisories were accepted
 * and when, the account, the machine facts, and for every wizard the
 * version that completed, when, what it found, and what it wrote.
 *
 * The gate: no controller runs for a sender until the advisories are
 * accepted, the button was pressed, an account exists, and every
 * required wizard is complete at the version this image requires.
 * A volatile override file lifts the wizard part of the gate for one
 * boot; nothing lifts the consent or the account.
 */
#ifndef FORGECTRL_SETUP_H
#define FORGECTRL_SETUP_H
#include <jansson.h>
#include <stddef.h>

#define SETUP_SCHEMA 1

/* One required wizard: the id and the version this image requires.
 * A record that carries an older version, or none, makes the wizard
 * required again. */
typedef struct {
    const char *id;
    int version;
    const char *when;   /* NULL, or a machine fact (a boolean key of the
                         * record's machine block, "tec") that must be
                         * true for the wizard to be required at all */
} setup_req_t;

/* A `when` fact that is not in the machine block ("cloud_enabled") is
 * asked of this hook, set by main to read the settings; NULL means no
 * such fact holds. */
extern int (*setup_fact_hook)(const char *fact);

/* The record's machine block: a string fact ("model") into buf (empty
 * when absent), or a boolean fact ("tec"). */
void setup_machine_str(const char *key, char *buf, size_t len);
int setup_machine_bool(const char *key);
/* The head serial hash the record carries (empty when none). */
void setup_head_hash(char *buf, size_t len);

/* Load the record and evaluate the gate. Idempotent; call once at
 * startup after sheetid_init() and users_init(). */
void setup_init(void);
/* Replace the required-wizard table (host tests). The default is the
 * image's table. */
void setup_set_required(const setup_req_t *reqs, size_t n);

/* The three questions the daemon asks:
 *  - first run: the initial setup has not completed, so "/"
 *    serves the wizard;
 *  - gate open: controllers may run for senders (consent + account +
 *    required wizards, or consent + account + the override);
 *  - override: the volatile override file stands. */
int setup_first_run(void);
int setup_gate_open(char *why, size_t len);
int setup_override_active(void);

/* Consent and account facts. Each writes the record atomically and
 * re-evaluates the gate. Return 0 on success. */
int setup_advisory_accept(const char *doc_id, const char *hash,
                                const char *method);
int setup_acceptance_pressed(void);
int setup_set_account(const char *name, long uid);
int setup_clear_account(void);
int setup_set_machine(json_t *machine);   /* takes a reference */
/* A wizard completed at a version, with its result and the settings
 * it applied ({key: {from, to}}); both may be NULL. Takes references. */
int setup_wizard_done(const char *id, int version, json_t *result,
                           json_t *applied);
/* Raise or clear a flag on a wizard: level is "required" or
 * "recommended"; a NULL level clears. */
int setup_flag(const char *id, const char *level, const char *reason);
/* The initial setup is complete. */
int setup_complete(void);
/* The record as JSON text for GET /wiz/record (the account hash is not
 * in it; secrets never are). Returns the length or -1. */
int setup_record_json(char *buf, size_t len);
/* The same as a malloc'd string the caller frees, indented when
 * `indent` is set; NULL when it cannot be made. The record grows with
 * every wizard's result, so nothing sizes a buffer for it. */
char *setup_record_dump(int indent);
/* The record as a new reference the caller owns (the HTML summary). */
json_t *setup_record_copy(void);

/* What changed: a part the owner replaced or a service that opened the
 * machine, reported from the panel, mapped to the wizards that must run
 * again. A wizard whose settings were measured on the old part is
 * required (its numbers are the old part's); one that only proves the
 * part is recommended. */
#define SETUP_CHANGE_WIZ 6
typedef struct {
    const char *id;
    const char *level;              /* "required" | "recommended" */
} setup_change_wiz_t;
typedef struct {
    const char *id;                 /* "tube" */
    const char *title;              /* "The laser tube was replaced" */
    const char *reason;             /* the flag's reason: "the tube was replaced" */
    setup_change_wiz_t wizards[SETUP_CHANGE_WIZ];
} setup_change_t;
const setup_change_t *setup_changes(size_t *n);
/* Raise the flags a change maps to, in one write. -1 for an unknown id. */
int setup_change_apply(const char *what);
/* The menu as JSON for GET /wiz: [{id, title, wizards: [{id, level}]}]. */
json_t *setup_changes_json(void);
/* The gate and consent summary for GET /wiz and the panel: first_run,
 * gate, override, why, required[], recommended[], advisories state. */
int setup_status_json(char *buf, size_t len);
/* Whether every advisory document is accepted at its current hash. */
int setup_advisories_complete(void);
/* Whether the acceptance press is recorded. */
int setup_acceptance_done(void);
/* The recorded completed version of a wizard, 0 when never. */
int setup_wizard_version(const char *id);
/* A completed wizard's recorded result, as a new reference the caller
 * owns; NULL when the wizard never completed. */
json_t *setup_wizard_result(const char *id);
/* The record's completion time ("" when the first run is not complete). */
void setup_completed_at(char *buf, size_t len);
#endif
