/*
 * wizdark.h - the dark wizards: the machine checked with the laser locked
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A dark wizard takes the machine (the controller stopped, or grblHAL
 * in loopback posture for the motion jogs), runs a bounded procedure on
 * a worker thread, asks the operator questions through a prompt the
 * page polls and answers, and ends with a result for the record and,
 * for the wizards that measure, one validated settings write. The
 * laser latch stays locked throughout. One dark wizard runs at a time,
 * and none runs beside a diagnostic or the dose recorder.
 *
 * Ids: switches, sensors, airflow, cooling.aa-offset, cooling.flow,
 * cooling.flow-verify, cooling.tec, motion, cameras.
 */
#ifndef FORGECTRL_WIZDARK_H
#define FORGECTRL_WIZDARK_H

#include <stddef.h>
#include <stdint.h>

void wizdark_init(void);
void wizdark_shutdown(void);

/* Is `id` a dark wizard this build knows? */
int wizdark_known(const char *id);

/* Start `id`. 0, or -1 with the reason in err (unknown, one is running,
 * the machine is not idle, a diagnostic holds the hardware). `owner` is
 * the login session of the browser that starts it ("" for a tool with
 * no session): that browser drives the run, another one mirrors it. */
int wizdark_start(const char *id, const char *owner, char *err, size_t elen);

/* The operator's answer to the current prompt: `seq` must be the
 * prompt's sequence number, value is the text. -1 when no such prompt
 * is open, -2 when `requester` is not the run's owner. */
int wizdark_answer(const char *id, int seq, const char *value, const char *requester);

/* Stop the running wizard; the worker stands the hardware down. 0, or
 * -2 when `requester` is not the run's owner. */
int wizdark_abort(const char *requester);

/* A second browser takes the run over: it becomes the owner. */
void wizdark_take_over(const char *requester);

/* The running or last-run state as JSON: {id, running, phase, progress,
 * elapsed_s, log[], prompt{seq,id,kind,text,options[]}|null,
 * result{}|null, error, owned, mine}. `owned` says a session drives
 * the run; `mine` says it is the requester's. */
int wizdark_status_json(char *buf, size_t len, const char *requester);

/* Whether any dark wizard runs (the diagnostics and the recorder refuse
 * to start beside one), and whether the running one is a cooling
 * wizard driving a diagnostic tool, which the tool's own start allows. */
int wizdark_running(void);
int wizdark_wraps_diag(void);

/* The cameras wizard keeps the last snapshot of each camera for the
 * page: returns a malloc'd copy or NULL. cam is "lid" or "head". */
uint8_t *wizdark_shot(const char *cam, size_t *len);

#endif
