/*
 * wizrun.h - the wizard runner's internals, shared by its two halves
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * wizdark.c owns the worker, the prompt, the log, and the finish;
 * wizlive.c holds the sheet wizards and runs on the same worker with
 * these calls. Nothing here is a route: wiz.h has those.
 */
#ifndef FORGECTRL_WIZRUN_H
#define FORGECTRL_WIZRUN_H

#include <jansson.h>
#include <stddef.h>

typedef struct {
    const char *id;
    void (*run)(void);
    int needs_idle;
} wiz_entry_t;

/* The live wizards' table (wizlive.c), searched after the dark ones. */
extern const wiz_entry_t wizlive_table[];
extern const size_t wizlive_n;

/* Progress and the log line the page shows. */
void wiz_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void wiz_phase(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void wiz_progress(int pct);
int wiz_aborted(void);
/* The abort request as a flag the streamer polls. */
const volatile int *wiz_abort_flag(void);
void wiz_msleep(int ms);
/* Poll cond(ctx) at 100 ms: 0 held, -1 aborted, -2 timed out. */
int wiz_wait_for(int (*cond)(void *), void *ctx, int timeout_s);

/* Prompts: kind is "continue", "confirm", "number", "choice",
 * "multichoice" (the answer is the chosen options joined by commas), or
 * "jog"; up to WIZ_PROMPT_OPTS options. 0 with the answer, -1 aborted,
 * -2 timed out. */
#define WIZ_PROMPT_OPTS 16
int wiz_ask(const char *kind, const char *pid, const char *text, const char *const *opts,
            int nopt, char *answer, size_t alen);
/* The same with its own timeout (a look at the sheet after a burn gets
 * longer than the runner's default; the page counts it down). */
int wiz_ask_t(const char *kind, const char *pid, const char *text, const char *const *opts,
              int nopt, int timeout_s, char *answer, size_t alen);
int wiz_ask_continue(const char *pid, const char *text);
int wiz_ask_confirm(const char *pid, const char *text);     /* 1 yes, 0 no, -1 aborted */
/* A "wait" prompt the machine answers (the press a job waits for): the
 * page shows the text and the counting phase until it is closed. */
void wiz_wait_open(const char *pid, const char *text, int timeout_s);
void wiz_wait_close(void);

/* The outcome. finish_ok borrows result and applied. */
void wiz_finish_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void wiz_finish_ok(int version, json_t *result, json_t *applied);
/* One validated multi-key settings write with the before values. */
int wiz_write_settings(const char *const *keys, const char *const *vals, size_t n,
                       json_t *applied, char *err, size_t elen);
double wiz_setting_num(const char *key, double def);
/* The user's units (ui_units): a length in mm as text in those units
 * ("2.8 mm" or "0.110 in"), and the user's number read back as mm. */
int wiz_imperial(void);
const char *wiz_len(double mm, char *buf, size_t n);
double wiz_len_mm(const char *text);

/* The controller, through the supervisor, with the button LED: while a
 * wizard holds the machine with no controller running the button
 * breathes white ("the machine is working"); the LED is handed back
 * dark before any controller starts, since a controller drives it
 * itself (white for its arm wait). wiz_led_attention lights it amber
 * while the wizard waits for the lid, and puts the working light back. */
int wiz_controller_stop(void);
void wiz_controller_start(void);
void wiz_led_attention(int on);

/* sysfs and the controller. */
long wiz_rd_long(const char *attr, long fallback);
int wiz_wr_attr(const char *attr, const char *val);
int wiz_kernel_wait_idle(double timeout_s);
int wiz_grbl_up(void *ctx);
int wiz_grbl_connect(void);
int wiz_grbl_send(int fd, const char *line);
int wiz_grbl_expect(int fd, const char *until, char *out, size_t olen, int timeout_s);
int wiz_grbl_wait_idle(int fd, int timeout_s);
/* The lens hall reference: `passes` passes that must agree; the lens
 * ends at the top. 0, -1 could not run, -2 never reached the hall, -3
 * the passes disagree. The passes go into r as "z_passes". */
int wiz_z_reference(json_t *r, int passes);

#endif
