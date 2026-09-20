/*
 * jobpost.h - the job runner's routes: POST /job, GET /job, POST /job/abort
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * POST /job is a multipart form: the file part `program` is the G-code,
 * streamed to a staging file in the run directory as it arrives (the
 * framework keeps no copy of it), and the fields are `name` (who sends
 * it: the job holds the machine as job:<name>), `lit_within_s` (above 0:
 * the program must show a discharge within that long, the press
 * included, or fail), `timeout_s` (above 0: the whole run's budget), and
 * `unlock` (1: the runner sends $X ahead of the program, to clear the
 * alarm an aborted job left).
 * The answer is the job's record, which GET /job serves while it plays
 * and after. POST /job/abort stops a posted program with a soft reset.
 */
#ifndef FORGECTRL_JOBPOST_H
#define FORGECTRL_JOBPOST_H

#include <ulfius.h>

/* The file sink's share of an upload: 1 when the request was a job's
 * (and is dealt with), 0 when it is somebody else's. */
int jobpost_sink(const struct _u_request *req, const char *key, const char *data,
                 uint64_t off, size_t size);

int cb_job_post(const struct _u_request *req, struct _u_response *res, void *user_data);
int cb_job_status(const struct _u_request *req, struct _u_response *res, void *user_data);
int cb_job_abort(const struct _u_request *req, struct _u_response *res, void *user_data);

#endif
