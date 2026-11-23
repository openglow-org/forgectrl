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

/* The Setup page's own wait for the acceptance press (wiz.c): it
 * withholds every job, and no route reaches it by name. */
#define BTNRES_SETUP "setup:press"

/* The longest one consent of the panel's stands from its registration,
 * however often it is renewed: a press, and its use, come within it. */
#define BTNRES_CONSENT_S 120.0
/* How often a consent that waits is judged again for coming in force
 * (who else waits on the button): a job's own wait reads files. */
#define BTNRES_CONSENT_JUDGE_S 0.2
#define BTNRES_KIND_MAX 16
#define BTNRES_SUBJECT_MAX 96
#define BTNRES_HOLDER_MAX 80
#define BTNRES_PEER_MAX 64

/* Open @who's reservation for @ttl_s seconds (at most BTNRES_TTL_MAX_S),
 * or renew it. @who names one prompt: "panel:<nonce>" (one hold dialog,
 * the nonce 1 to BTNRES_NONCE_MAX of letters, digits, '-' and '_'),
 * "check:<package>" (a package's check prompt, one at a time; that
 * package's own jobs are not withheld, btnres_pkg_holds) or BTNRES_SETUP
 * (the Setup page's press). It ends a
 * package's ask that stands: the operator's own prompt comes first. For
 * BTNRES_SETUP and "check:<package>" the ask's reservation lingers
 * WIZPKG_LINGER_S (btnres_ask).
 * 0, or -1 with the reason (a name it cannot take, every slot in use). */
int btnres_open(const char *who, double ttl_s, char *why, size_t len);
/* Whether @who has one of those forms. */
int btnres_name_ok(const char *who);
/* Move the end of @who's reservation to @ttl_s seconds from now (at
 * most BTNRES_TTL_MAX_S), only while it stands: 0, or -1 for one that has
 * ended, which stays ended. */
int btnres_renew(const char *who, double ttl_s);
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
 * stands when the operator's prompt reserves the button ends there; its
 * reservation lingers WIZPKG_LINGER_S when that prompt is a consent, the
 * Setup page's wait or a package's check, which do not come in force
 * beside it. A "pressed" not yet collected then counts for nobody: the
 * package is never told (its next call is refused while that prompt or
 * its linger stands, and opens a new ask after), and the operator presses
 * again. -2 with @why when @pkg is no package id. */
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
 * no thread is started (a test steps the ask and the consent itself). */
extern int (*btnres_button)(void);

/* ---- the panel's consent: one press for one guarded action ----
 *
 * An action the panel guards with the machine's button (an unverified
 * package's install, an owner's key, the fuse identity, the reset of every
 * setting, unsigned firmware, an older firmware's install, the factory
 * return) takes one press made for it. The panel's dialog registers the
 * consent when it opens (btnres_consent_open), and renews it while it
 * stands: what it is for (kind), what exactly (subject, main.c's
 * consent_subject: the archive's digest, the key's fingerprint, the
 * firmware's file, slot, SHA-256 and what it skips, the factory return's
 * slot, "" for none), and who asks (holder: the asking session, or the
 * address of a client that has none; peer, for the log). Its reservation is
 * "panel:<nonce>", against every job. At most one consent stands on the
 * machine: one registered while another stands refuses both, unless it is
 * the same holder's, which replaces its own. A consent comes in force once
 * its reservation has stood on file BTNRES_READY_S, and it has stood that
 * long itself, with nobody else waiting on the button (btnres_consent_
 * refused) and no other reservation standing; the press counts from a
 * release read in force and is two readings down in a row
 * (BTNRES_ASK_POLL_MS apart), asked again then, and a press somebody else
 * takes is not the consent's. The action takes the press once
 * (btnres_consent_take), for the same nonce, kind, subject and holder: a
 * take that names anything else ends the consent. The reservation stands
 * WIZPKG_LINGER_S after the press, whatever ends the consent then. A
 * consent ends when nothing renews it (BTNRES_PANEL_TTL_S), BTNRES_CONSENT_S
 * after its registration, when the button cannot be read, and with its
 * dialog (btnres_consent_close); its nonce is not registered again. */
typedef struct {
    const char *nonce;      /* the dialog's own name, as btnres_name_ok takes "panel:<nonce>" */
    const char *kind;       /* what the press is for: install, key, fuse, reset, firmware, factory */
    const char *subject;    /* what exactly; "" for none */
    const char *holder;     /* who asks: compared, never logged */
    const char *peer;       /* the asker's address, for the log */
} btnres_consent_t;

enum { BTNRES_CONSENT_WAITING, BTNRES_CONSENT_READY, BTNRES_CONSENT_PRESSED };

/* Register @c's consent or renew it: BTNRES_CONSENT_WAITING (not yet in
 * force, with who else waits in @why when somebody does), _READY (the next
 * press is its own), _PRESSED (that press, made). -1 with the refusal in
 * @why: another holder's consent stands (both end, and @other gets that one's
 * kind and peer for the log; "" when the refusal is another), this nonce's
 * consent has ended (in the words it ended with), or it stands for another
 * kind, subject or holder (it ends too). */
int btnres_consent_open(const btnres_consent_t *c, char *why, size_t len, char *other, size_t olen);
/* The dialog closed: its consent ends, and its reservation, but the
 * linger after a press. */
void btnres_consent_close(const char *nonce);
/* The action takes @c's press, once. BTNRES_TAKEN; BTNRES_NOT_PRESSED (it
 * stands, no press yet: the operator may still press); BTNRES_NOT_STANDING
 * (no consent stands under this nonce: never one, or it ended, its words in
 * @why); BTNRES_MISMATCH (it stands for another kind, subject or holder:
 * it ends). */
enum { BTNRES_TAKEN, BTNRES_NOT_PRESSED, BTNRES_NOT_STANDING, BTNRES_MISMATCH };
int btnres_consent_take(const btnres_consent_t *c, char *why, size_t len);
/* One reading of the button for the consent that stands, as
 * btnres_ask_step (the reader thread steps both). */
void btnres_consent_step(int button);
/* Whether anybody else waits on the button when a consent would come in
 * force or is pressed, with who in @why: main.c answers (a job that waits,
 * runs or is paused; this daemon's own wait; the machine's lease). NULL:
 * nobody. Asked under btnres.c's lock: it calls nothing of btnres. */
extern int (*btnres_consent_refused)(char *why, size_t len);

/* The clock reservations run on, in seconds: NULL is CLOCK_MONOTONIC (a
 * test sets its own). */
extern double (*btnres_clock)(void);
#endif
