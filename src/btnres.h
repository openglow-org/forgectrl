/*
 * btnres.h - the machine's button reserved for a prompt (see btnres.c)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_BTNRES_H
#define FORGECTRL_BTNRES_H
#include <stddef.h>

/* A panel dialog renews its reservation while it stands, and a package
 * its ask; a closed tab, or a package that stops asking, leaves it for
 * this long at most. */
#define BTNRES_PANEL_TTL_S 10.0
/* The longest any reservation is taken for in one call, and the longest
 * one ask of a package stands, however often it is renewed. */
#define BTNRES_TTL_MAX_S 900.0
/* The time the reservation must have stood on file (cool.h,
 * cool_button_on_file_s) before a prompt asks for the press: the GRBL
 * controller reads the verdict every 0.5 s and the cloud client every
 * 0.1 s, so both have read it by then. */
#define BTNRES_READY_S 1.0
#define BTNRES_MAX 16
#define BTNRES_NONCE_MAX 32
#define BTNRES_PKG_MAX 64
#define BTNRES_WHO_MAX (6 + BTNRES_PKG_MAX + 1 + BTNRES_NONCE_MAX + 1)
/* How often the button is read while a package's ask stands. */
#define BTNRES_ASK_POLL_MS 40
/* This many readings in a row with the button unreadable end the ask. */
#define BTNRES_ASK_UNREAD_MAX 3
/* The packages turned off on the control panel that are remembered. */
#define BTNRES_BAR_MAX 32

/* Open @who's reservation for @ttl_s seconds (at most BTNRES_TTL_MAX_S),
 * or renew it. @who names one prompt: "panel:<nonce>" (one hold dialog,
 * the nonce 1 to BTNRES_NONCE_MAX of letters, digits, '-' and '_') or
 * "check:<package>" (a package's check prompt, one at a time; that
 * package's own jobs are not withheld, btnres_pkg_holds). It ends a
 * package's ask that stands: the operator's own prompt comes first.
 * 0, or -1 with the reason (a name it cannot take, every slot in use). */
int btnres_open(const char *who, double ttl_s, char *why, size_t len);
/* Whether @who has one of those forms. */
int btnres_name_ok(const char *who);
/* End @who's reservation, and no other. */
void btnres_close(const char *who);
/* 1 while @who's reservation stands ("ask:<package>" names a package's
 * ask and the linger after its press). */
int btnres_stands(const char *who);
/* 1 while any reservation whose name starts with @prefix stands. */
int btnres_any(const char *prefix);

/* The cooling engine's question (cool.h, cool_button_reserved): 1 while a
 * reservation stands that the job of the armed window may not take, with
 * whose prompt it is in @why. A package's check does not withhold that
 * package's own jobs while it keeps the machine for them; a package's ask
 * withholds every job, its own included. */
int btnres_withheld(char *why, size_t len);

/* Whether @pkg keeps the machine for its own jobs: main.c answers it (the
 * package keeps the Grbl sender out, on the running GRBL controller's own
 * word, so no other party's job can be the one waiting). NULL: no package
 * does. */
extern int (*btnres_pkg_holds)(const char *pkg);

/* ---- a package's ask for a press ----
 *
 * The one way a package gets a press of the machine's button. @pkg asks,
 * or keeps asking: every call renews its reservation for
 * BTNRES_PANEL_TTL_S, so a package that stops calling (it ended, crashed
 * or hangs) loses it by then, and one ask stands BTNRES_TTL_MAX_S at
 * most. A call renews nothing while btnres_session_reported answers: the
 * ask then ends BTNRES_PANEL_TTL_S after the last call that renewed it,
 * however often the package calls. Returns 0 with @state:
 *   "waiting"  reserved, and not yet on file for BTNRES_READY_S; or in
 *              force with btnres_session_reported answering, where a
 *              press would not be said to the package;
 *   "ready"    in force: the next press, begun from here on, is its own;
 *   "pressed"  that press, said once: the ask is over, and its
 *              reservation stands WIZPKG_LINGER_S (wizpkg.h) more.
 * A "pressed" not collected inside those WIZPKG_LINGER_S is gone with
 * the reservation: the package calls more often than that while it waits.
 * -1 with who holds the button in @why when it is refused: another
 * reservation stands (a panel dialog's, a check's, another package's ask
 * or the linger after it), btnres_ask_refused answers, the package is
 * turned off (btnres_ask_bar), or the button cannot be read. An ask that
 * stands when the operator's prompt reserves the button ends there. -2
 * with @why when @pkg is no package id. */
int btnres_ask(const char *pkg, const char **state, char *why, size_t len);
/* End @pkg's ask unpressed (it withdrew it). The linger after a press
 * stands its time. */
void btnres_ask_end(const char *pkg);
/* @pkg was turned off or removed on the control panel: its ask ends as
 * btnres_ask_end ends it, and every ask of its own is refused from here
 * on, whatever its service still calls, until btnres_ask_unbar (the
 * operator turned it on again, or installed it anew). */
void btnres_ask_bar(const char *pkg);
/* The same from the operator's click, while the extension host is still
 * asked: @pkg's ask ends and its asks are refused, in words that say it
 * is being turned off. Returns 1 when @pkg was not barred already, and
 * the caller then lifts the entry (btnres_ask_unbar) if the host refuses
 * or does not answer, or makes it stand (btnres_ask_bar) on its yes; 0
 * for a package already barred, whose entry stays as it is. */
int btnres_ask_bar_pending(const char *pkg);
void btnres_ask_unbar(const char *pkg);
/* One reading of the button for the ask that stands: 1 pressed, 0
 * released, -1 unreadable. The reader thread calls it every
 * BTNRES_ASK_POLL_MS (a test, in its place). The press is the asker's
 * only if btnres_ask_refused still names nobody at that reading: an ask
 * it refuses there ends, the press unsaid, and its reservation stands
 * WIZPKG_LINGER_S as after a press that is said. BTNRES_ASK_UNREAD_MAX
 * unreadable readings in a row end an ask that has had no press. */
void btnres_ask_step(int button);

/* Whether anybody else waits on the button when @pkg asks, with who in
 * @why: main.c answers (a job that waits for its press, is inside its
 * armed window, or runs or is paused; a press this daemon waits for
 * itself; the machine's lease in another holder's hands). Asked when an
 * ask starts, again when it comes in force, and at the press. NULL:
 * nobody. */
extern int (*btnres_ask_refused)(const char *pkg, char *why, size_t len);
/* Whether this daemon has come to wait for a press of its own, which ends
 * an ask that stands. NULL: never. */
extern int (*btnres_daemon_waits)(void);
/* Whether a controller reports its armed session (cool.h, cool_armed): a
 * call that keeps a standing ask does not extend it meanwhile. NULL:
 * never. */
extern int (*btnres_session_reported)(void);
/* How long the reservation has stood on file, in seconds (cool.h,
 * cool_button_on_file_s). NULL: no ask comes in force. */
extern double (*btnres_on_file_s)(void);
/* The button's level for the reader thread: 1, 0, or -1 unreadable. NULL:
 * no thread is started (a test steps the ask itself). */
extern int (*btnres_button)(void);

/* The clock reservations run on, in seconds: NULL is CLOCK_MONOTONIC (a
 * test sets its own). */
extern double (*btnres_clock)(void);
#endif
