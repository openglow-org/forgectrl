/*
 * jobrun.h - the job runner: one job at a time with the daemon as its sender
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * jobstream is the sender; this is everything around one of its runs
 * that is the same whoever asks for it. A run takes the machine lease as
 * a sender (so it is refused while a Grbl client is on the socket, and
 * while anything else holds the machine, in the holder's words), plays
 * on a thread of the runner's or on the caller's, is visible to
 * GET /job while it plays and after, and gives the lease back on every
 * way out. The dose-curve recorder starts its ladder through it, the
 * sheet wizards stream their cards through it inside their own hold, and
 * POST /job runs a program a client sends.
 *
 * A job is a sender like any other: the arm gates stand, the first
 * laser-on line waits for the press on the machine, and the emission
 * witnesses jobstream samples are in the job's record.
 *
 * A posted program is G-code and nothing else. It is checked whole
 * before a line of it goes out: no $ line (the controller's system
 * commands are not a job's to send), no realtime character outside a
 * comment, no byte that is not printable ASCII, no line longer than the
 * sender takes. Comments are stripped as the lines go out, and the
 * runner's own M2 ends the program if the program's own does not.
 *
 * An aborted job leaves the controller in its alarm state, as a Stop
 * from any sender does (the soft reset of a machine in motion), and a
 * program may not carry the $X that clears it. The caller of POST /job
 * asks for it instead: with `unlock`, the runner sends $X itself ahead
 * of the program's first line, the way the sheet wizards head theirs.
 * While the X and Y motors are released the controller refuses it, and
 * the job ends there.
 */
#ifndef FORGECTRL_JOBRUN_H
#define FORGECTRL_JOBRUN_H

#include "jobstream.h"

#include <stddef.h>

#define JOBRUN_PROGRAM_MAX (16u * 1024u * 1024u)    /* a posted program, in bytes */
#define JOBRUN_LINE_MAX    250                      /* one line of it, comments stripped */
#define JOBRUN_NAME_MAX    32

/* A started run's last preparation, once the lease is the run's and
 * before its thread exists, on the starter's thread: 0, or -1 with the
 * refusal in err, and the start is refused with nothing to undo but what
 * this did. It must not take a lock the starter holds. */
typedef int (*jobrun_ready_fn)(void *ctx, char *err, size_t elen);
/* The end of a started run, on the run's thread, with the lease still
 * held: rc and err are jobstream_run()'s. */
typedef void (*jobrun_done_fn)(void *ctx, int rc, const jobstream_run_t *run, const char *err);

typedef struct {
    const char *owner;          /* the lease owner: "job:<name>", "recorder" */
    const char *under;          /* the holder this run stands inside of, or NULL */
    jobstream_cfg_t stream;     /* abort_flag NULL: jobrun_stop() is the abort */
    jobrun_ready_fn ready;      /* started runs only; may be NULL */
    jobrun_done_fn done;        /* started runs only; may be NULL */
} jobrun_cfg_t;

/* Start a run on the runner's thread. 0, or -1 with the refusal in err
 * and nothing started. cfg is copied; what stream.ctx points at must
 * outlive the run. */
int jobrun_start(const jobrun_cfg_t *cfg, char *err, size_t elen);

/* The same run on the caller's thread: jobstream_run()'s return, or -1
 * with the refusal in err. */
int jobrun_sync(const jobrun_cfg_t *cfg, jobstream_run_t *run, char *err, size_t elen);

/* From a done callback: give the lease back now, ahead of whatever the
 * callback publishes after it, so nobody who reads "finished" there can
 * still be refused by this run's hold. */
void jobrun_lease_back(void);

/* Abort the started run that owner holds, and wait for it. Nothing
 * when the run is somebody else's or there is none. */
void jobrun_stop(const char *owner);

/* A posted program, staged at path. The check: 0 with the line count,
 * or -1 with the first offense and its line number in err. */
int jobrun_program_check(const char *path, int *lines, char *err, size_t elen);
/* Check it and start it as job:<name>. The file is the runner's from
 * here: it is opened and its name removed, started or refused. lit_timeout_s
 * above 0 says the program must show a discharge within that long (the
 * press included) or fail; unlock sends the runner's $X first. */
int jobrun_program_start(const char *path, const char *name, double lit_timeout_s,
                         double run_timeout_s, int unlock, char *err, size_t elen);
/* Abort a posted program: 0, or -1 with why not in err (nothing runs,
 * or the run is not a posted program's). */
int jobrun_program_abort(char *err, size_t elen);
/* A job's name from a client: 0 when it is usable as it is. */
int jobrun_name_ok(const char *name);

/* The job playing, or the last one: state (idle, running, done,
 * failed), owner, lines, sent, acked, elapsed_s, the witnesses (with the
 * number of samples they rest on), reason. */
int jobrun_status_json(char *buf, size_t len);

#endif
