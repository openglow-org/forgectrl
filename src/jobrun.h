/*
 * jobrun.h - the job runner: one job at a time with the daemon as its sender
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * jobstream is the sender; this is everything around one of its runs
 * that is the same whoever asks for it. A run takes the machine lease as
 * a sender (so it is refused while a Grbl sender is on the socket, and
 * while anything else holds the machine, in the holder's words), then
 * keeps the Grbl socket to this host for as long as it plays
 * (jobrun_sender_out: refused unless the controller is idle with nothing
 * planned, no jog, no M-code wait, no armed laser, no open envelope and a
 * quiet sender, so a sender that left with a line still running, or a
 * job of its held, never meets the run), plays
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
 * of the program's first line, the way the sheet wizards head theirs,
 * only while the runner's own stop still stands: its last run ended with
 * the runner's own soft reset (an abort, a timeout, a failed witness, a
 * lost connection), not with the controller's alarm, its alarm lock
 * (error:9) or its own reset; the controller took at least one of that
 * run's lines (an ok to it: no alarm stood when the run began, since a
 * soft reset at rest keeps one that stands); the controller showed that
 * stop at the run's end (its published state read alarm 3 with no
 * connection, within JOBRUN_STOP_SEEN_S: a stop at rest raises no alarm
 * of its own), with the driver's own count of the alarm-3 raises it made
 * itself (alarm3_raised: its emission witness, a refused arm) where it was
 * when the run began, and which controller process it was is read
 * before that state; nothing of this
 * daemon's has cleared or changed the alarm since (the port's home,
 * release, energize: jobrun_wire); and, once the new run keeps the
 * Grbl socket, the state still reads alarm 3, with the sender generation
 * and the driver's count it showed after that stop and from the same
 * controller process. Another alarm, another sender's session since, a
 * controller started since, an alarm 3 the driver raised itself (under the
 * run, inside the end's read, or while the runner's stands), or a state
 * that cannot be read: no $X, and the program's first
 * line meets whatever stands. Outside a
 * package's claim the run's keep is refused on an alarmed machine, so the
 * $X goes out only under a claim. While the X and Y motors are released
 * the controller refuses the $X, and the job ends there.
 */
#ifndef FORGECTRL_JOBRUN_H
#define FORGECTRL_JOBRUN_H

#include "jobstream.h"

#include <stddef.h>

#define JOBRUN_PROGRAM_MAX (16u * 1024u * 1024u)    /* a posted program, in bytes */
#define JOBRUN_LINE_MAX    250                      /* one line of it, comments stripped */
#define JOBRUN_NAME_MAX    63

/* A started run's last preparation, once the lease is the run's and
 * before its thread exists, on the starter's thread: 0, or -1 with the
 * refusal in err, and the start is refused with nothing to undo but what
 * this did. It must not take a lock the starter holds. */
typedef int (*jobrun_ready_fn)(void *ctx, char *err, size_t elen);
/* The end of a started run, on the run's thread, with the lease still
 * held: rc and err are jobstream_run()'s. */
typedef void (*jobrun_done_fn)(void *ctx, int rc, const jobstream_run_t *run, const char *err);

/* The Grbl socket kept to this host for a run, and given back after it.
 * A run's start asks for it once the lease is the run's and before
 * anything is sent: 0, or -1 with the refusal in err, and the start is
 * refused. Given back on every way out of a run that had it, before the
 * runner is free for the next. Set at start (main.c: senderout.h's, the
 * controller port's "sender out", which only an idle machine with a quiet
 * sender takes); NULL asks nothing (a host test of the runner alone). */
extern int (*jobrun_sender_out)(char *err, size_t elen);
extern void (*jobrun_sender_in)(void);

/* How long a run's end waits for the controller to publish the state
 * that shows the runner's own soft reset (alarm 3, no connection; it
 * publishes within a poll of its loop): a stop not seen in that time, a
 * stop at rest among them, leaves no mark, and no $X. */
#ifndef JOBRUN_STOP_SEEN_S
#define JOBRUN_STOP_SEEN_S 1.0
#endif

/* Forget the runner's stop: something of this daemon's has cleared or
 * changed the controller's alarm with no sender session. */
void jobrun_forget_stop(void);

/* The unlock's two links to the rest of the daemon, set at start
 * (main.c): controller, the running GRBL controller's identity (the
 * supervisor's pid of it, 0 for none), so the unlock above never clears
 * an alarm of a controller started since the runner's stop; and
 * *alarm_touched (the controller port's grblport_alarm_touched, called
 * before a home, a release or an energize goes out) set to
 * jobrun_forget_stop. Until it is called no stop is marked, and no $X
 * goes out. */
void jobrun_wire(long (*controller)(void), void (**alarm_touched)(void));

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
/* A word in one line, as the line goes out (comments gone), that changes
 * what the controller keeps past the program: G10 (the work offsets),
 * G20 (inches; G21 passes), G28.1 and G30.1 (the stored positions), G43
 * in every form (the tool length offset; G49, its cancel, passes), G51
 * (the scaling; G50, its cancel, passes), G92 in every form (the G92
 * offset), and M103 (the crumb tray's mode) unless it is M103 with one P
 * of exactly 0 or 1 that is tray, the mode in force (tray_mode(): 1 out,
 * 0 in, -1 not readable, which no M103 matches). Its name ("G92"), or
 * NULL. The words are read as the controller's parser reads them: spaces
 * anywhere dropped, letters in either case, the number read to its
 * value, so G010, G10.0 and "G 1 0" are all G10. */
const char *jobrun_line_kept_word(const char *line, int tray);
/* A package's program, staged at path, against the tray's mode tray: 0
 * when no line has such a word, or -1 with the first, its line number, in
 * err. */
int jobrun_program_check_package(const char *path, int tray, char *err, size_t elen);
/* Check it and start it as job:<name>. The file is the runner's from
 * here: it is opened and its name removed, started or refused. lit_timeout_s
 * above 0 says the program must show a discharge within that long (the
 * press included) or fail; unlock sends the runner's $X first, after the
 * runner's own stop only (above). */
int jobrun_program_start(const char *path, const char *name, const char *under, double lit_timeout_s,
                         double run_timeout_s, int unlock, char *err, size_t elen);
/* The same, with package 1 for a package's program (jobpost: the host's,
 * or a page's): each line jobrun_line_kept_word() would refuse with the
 * tray's mode unknown (an M103) is read again as it is about to go out,
 * against jobrun_tray_mode() then, and one that no longer passes ends the
 * run there, unsent, failed in words naming the line and the word. */
int jobrun_program_start_as(const char *path, const char *name, const char *under, double lit_timeout_s,
                            double run_timeout_s, int unlock, int package, char *err, size_t elen);
/* The tray's mode as tray_mode() gives it (1 out, 0 in, -1 not readable);
 * main.c sets it. NULL reads as -1, which no M103 matches. */
extern int (*jobrun_tray_mode)(void);
/* Abort a posted program: 0, or -1 with why not in err (nothing runs,
 * or the run is not a posted program's). */
int jobrun_program_abort(char *err, size_t elen);
/* The same, only for the posted program named name (job:<name>): a package
 * ends its own job and no one else's. -1 with why in err when another
 * party's runs. */
int jobrun_program_abort_of(const char *name, char *err, size_t elen);
/* A job's name from a client: 0 when it is usable as it is. */
int jobrun_name_ok(const char *name);

/* The job playing, or the last one: state (idle, running, done,
 * failed), owner, lines, sent, acked, elapsed_s, the witnesses (with the
 * number of samples they rest on), reason. */
int jobrun_status_json(char *buf, size_t len);

#endif
