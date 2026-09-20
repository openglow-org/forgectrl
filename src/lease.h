/*
 * lease.h - the machine lease: one answer to "who has the machine?"
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Diagnostics, the setup wizards, update jobs, the dose-curve recorder,
 * and the log export each know whether they themselves are running. The
 * lease is where each of them asks about all the others: whoever runs
 * takes it, whoever wants to start is refused while another holds it, and
 * the refusal names the holder. Every route that must not act under
 * somebody else's feet asks it too, and /status shows the holder.
 *
 * An owner is a short name, "<what>:<which>": diag:flow-verify,
 * wizard:motion, update:apply, job:panel, recorder, logs.export. A holder
 * may let one owner in under itself (the cooling wizards run a diagnostic
 * inside their own hold, the sheet wizards a job): the inner owner names
 * the holder it runs under, and releases before it.
 *
 * What the lease does not hold it still reports, because an operator
 * asking "why won't it start" is asking about these too: a Grbl client on
 * the socket (LightBurn holds TCP 23 outside any grant, and cannot be
 * revoked), and the X and Y motors released. A hold of the sender kind is
 * refused while a client is connected.
 *
 * Every owner today is a thread of this daemon, whose every exit path
 * releases. There is no timeout: revoking a hold does not stop the thread
 * that has the hardware, so a timeout would only make the lease lie.
 */
#ifndef FORGECTRL_LEASE_H
#define FORGECTRL_LEASE_H

#include <stddef.h>

typedef enum {
    LEASE_HARDWARE = 0,     /* the owner drives the machine itself; the controller is its to stop */
    LEASE_SENDER,           /* the owner is the Grbl sender for a job of its own */
    LEASE_SYSTEM,           /* the owner changes the system under everything: an update job */
    LEASE_EXPORT            /* the owner only reads the machine at rest: the log export */
} lease_kind_t;

#define LEASE_OWNER_MAX 48

/* Takes the lease. under is NULL, or the owner the caller runs inside
 * of, which must be the current holder. 0, or -1 with the reason in why:
 * the holder in words, or the connected client for a sender hold. */
int lease_take(const char *owner, lease_kind_t kind, const char *under, char *why, size_t len);

/* Gives it back. Only the innermost owner can; anything else is ignored. */
void lease_release(const char *owner);

/* For a route that must not act under a holder: 1 with the holder in
 * words, 0 when the machine is nobody's. */
int lease_refusal(char *why, size_t len);

/* The same, for a caller that may itself be the holder (a wizard that
 * switches the mode inside its own hold): as is its owner name, or NULL. */
int lease_refusal_for(const char *as, char *why, size_t len);

/* The same, for the controls a holder locks (the settings, the
 * controller's start, the quiet hold): every holder but a log export,
 * which only reads, and which a settings write does not disturb. */
int lease_refusal_locks(char *why, size_t len);

/* The innermost holder's name: 1 and the name, or 0. */
int lease_holder(char *owner, size_t len);

/* "lease":{...} for /status, without a trailing comma: the holder (owner,
 * kind, for_s, and under when nested) or null, and what is observed
 * outside the lease. */
int lease_json(char *buf, size_t len);

/* An owner in words, for a refusal or a panel: "a diagnostic
 * (flow-verify)", "the dose-curve recorder". */
void lease_words(const char *owner, char *buf, size_t len);

/* What is observed outside the lease; each may be NULL. Set at start. */
extern int (*lease_sender_connected)(void);     /* nonzero: a client is on the Grbl socket, or it cannot be told */
extern int (*lease_motors_released)(void);

#endif
