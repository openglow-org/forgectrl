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
 * accepted, the button was pressed, an account exists, the required
 * steps of the table (preferences, the machine facts, the cloud
 * decision, the coolant flow calibration) are done at their versions,
 * and the intake fans are
 * counted. Every other step is recommended, never required: a step not
 * run, or
 * run at an older version than this image's table, and every flag, fill
 * the recommended list the panel shows.
 */
#ifndef FORGECTRL_SETUP_H
#define FORGECTRL_SETUP_H
#include <jansson.h>
#include <stddef.h>

#define SETUP_SCHEMA 1

/* One step of the setup: the id and the version this image wants. A
 * record that carries an older version, or none, holds the gate when
 * the step is required, and recommends the step again when it is not. */
typedef struct {
    const char *id;
    int version;
    const char *when;   /* NULL, or a machine fact (a boolean key of the
                         * record's machine block, "tec") that must be
                         * true for the step to be recommended at all */
    int required;       /* the gate holds until it is done */
} setup_step_t;

/* A `when` fact that is not in the machine block ("cloud_enabled") is
 * asked of this hook, set by main to read the settings; NULL means no
 * such fact holds. */
extern int (*setup_fact_hook)(const char *fact);
/* Whether this build has a step by that id (the wizard catalog, set by
 * wiz_init): a flag a record kept for a step since retired is left out
 * of the recommended list. NULL takes every id. */
extern int (*setup_known_hook)(const char *id);
/* Whether the machine's intake fan count is known (cool_intake_fans has a
 * value: the Intake fans step, the airflow check, or the operator wrote
 * it), set by main; the gate holds without it. NULL takes it as known. */
extern int (*setup_fans_hook)(void);

/* The record's machine block: a string fact ("model") into buf (empty
 * when absent), or a boolean fact ("tec"). */
void setup_machine_str(const char *key, char *buf, size_t len);
int setup_machine_bool(const char *key);
/* The head serial hash the record carries (empty when none). */
void setup_head_hash(char *buf, size_t len);
/* Replace the record's head facts (machine.head), when the record has
 * machine facts. Takes the reference. */
int setup_set_head(json_t *head);

/* Load the record and evaluate the gate. Idempotent; call once at
 * startup after sheetid_init() and users_init(). */
void setup_init(void);
/* Replace the step table (host tests). The default is the image's
 * table. */
void setup_set_steps(const setup_step_t *s, size_t n);

/* The two questions the daemon asks:
 *  - first run: the guided run has not ended (finished, or set aside
 *    with "Start using the machine" or "Finish later"), so "/" serves
 *    the wizard;
 *  - gate open: controllers may run for senders (consent + account +
 *    the required steps + the intake fan count). */
int setup_first_run(void);
int setup_gate_open(char *why, size_t len);

/* Consent and account facts. Each writes the record atomically and
 * re-evaluates the gate. Return 0 on success. */
int setup_advisory_accept(const char *doc_id, const char *hash,
                                const char *method);
int setup_acceptance_pressed(void);
/* An on-demand document, accepted where its feature is turned on. It is
 * recorded beside the first-run ones and leaves them, and the press that
 * sealed them, as they are. */
int setup_on_demand_accept(const char *doc_id, const char *hash, const char *method);
int setup_set_account(const char *name, long uid);
int setup_clear_account(void);
int setup_set_machine(json_t *machine);   /* takes a reference */
/* A wizard completed at a version, with its result and the settings
 * it applied ({key: {from, to}}); both may be NULL. Takes references.
 * A step of the table is recorded at the table's version when that is
 * higher. */
int setup_wizard_done(const char *id, int version, json_t *result,
                           json_t *applied);
/* Recommend a step again, with the reason; a NULL reason clears the
 * flag. A completed run of the step clears it too. */
int setup_flag(const char *id, const char *reason);
/* 1 when the step carries a flag (a part change or a fault recommends it
 * again). */
int setup_flagged(const char *id);
/* The guided run ended: finished, or set aside to use the machine. */
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
 * machine, reported from the panel, mapped to the steps it recommends
 * again: the ones that measured the old part or prove the new one. */
#define SETUP_CHANGE_WIZ 6
typedef struct {
    const char *id;                 /* "tube" */
    const char *title;              /* "The laser tube was replaced" */
    const char *reason;             /* the flag's reason: "the tube was replaced" */
    const char *wizards[SETUP_CHANGE_WIZ];
} setup_change_t;
const setup_change_t *setup_changes(size_t *n);
/* Raise the flags a change maps to, in one write. -1 for an unknown id. */
int setup_change_apply(const char *what);
/* The menu as JSON for GET /wiz: [{id, title, wizards: [{id}]}]. */
json_t *setup_changes_json(void);
/* The gate and consent summary for GET /wiz and the panel: first_run,
 * gate, why, completed, recommended[{id, reason}], advisories state. */
int setup_status_json(char *buf, size_t len);
/* How many steps the setup recommends now (the recommended list). */
int setup_recommended_count(void);
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
/* Whether `mode` is a homing method the cloud step offers while it turns
 * cloud mode on: the service's camera home, a home by hand, or none. */
int setup_cloud_homing_ok(const char *mode);
#endif
