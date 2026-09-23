/*
 * events.h - the machine's event stream
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * GET /events is server-sent events: one line of state when something
 * changes, in place of a client polling for it. An edge detector looks
 * at state the daemon already holds, five times a second and only while
 * somebody listens, and writes what changed into a ring every stream
 * reads from.
 *
 * The daemon is thread-per-connection and a stream holds its thread for
 * hours, so the streams are capped in code: EVENTS_MAX_STREAMS in all,
 * one per peer address, counted apart from the camera streams. A client
 * past the total is refused with the reason. A consumer that wants more
 * fans out from a hub off the machine.
 *
 * One per address is kept by replacement, not refusal: a new stream from
 * an address ends that address's older one (a last "bye" event, then the
 * end of the response). A client that went away is only noticed at the
 * server's next write, so a refusal would turn every page reload into an
 * error that an EventSource does not retry.
 *
 * The extension host's subscription is one more stream, outside that cap
 * and outside the replacement rule. It is one subscription that fans out
 * to every extension, so without it each extension that wants events
 * would want a stream of the three, and one curl on the machine would
 * blind every extension at once. It is claimed by a loopback peer that
 * asks for it by name (EVENTS_HOST_HEADER): an extension account cannot
 * reach a loopback address at all - the sandbox's netfilter rules refuse
 * everything it sends through lo before any allowlist is looked at - so a
 * loopback claim is this firmware's own software or somebody who is
 * already root on the machine. A second host stream replaces the first,
 * which is what a restarted host is.
 */
#ifndef FORGECTRL_EVENTS_H
#define FORGECTRL_EVENTS_H

#include <stddef.h>

#define EVENTS_MAX_STREAMS 3
#define EVENTS_HZ 5
/* The button is looked at more often than the rest: a press is shorter
 * than a 5 Hz look. Only the switch word is read at this rate. */
#define EVENTS_BUTTON_HZ 25
#define EVENTS_TELEMETRY_S 10           /* telemetry.tick, while somebody listens */
/* The extension host's stream: its slot, and how it asks for it. */
#define EVENTS_HOST_SLOT   EVENTS_MAX_STREAMS
#define EVENTS_HOST_HEADER "X-ForgeFIRM-Client"
#define EVENTS_HOST_CLIENT "extension-host"

/* ---- admission: the cap, as a table of peer addresses ---- */

typedef struct {
    char peer[EVENTS_MAX_STREAMS][64];
} events_slots_t;

#define EVENTS_ADMIT_FULL (-1)      /* every stream is taken */

/* A slot index, with *replaced set when the address already held that
 * slot (its older stream is to end), or EVENTS_ADMIT_FULL. */
int events_admit(events_slots_t *s, const char *peer, int *replaced);
void events_release(events_slots_t *s, int slot);

/* ---- the edge detector ---- */

/* What is looked at. A field the sampler could not read keeps its last
 * value, so a transient read error is never an edge. */
typedef struct {
    unsigned long switches;         /* machine_switch_bits() */
    char mode[8];                   /* grbl, cloud */
    char controller[16];            /* running, stopped, standby, waiting, gated, motion-fault */
    char verdict[24];               /* the cooling engine's */
    int fire_ok;
    int armed;                      /* the active controller's report */
    int arming;                     /* the GRBL controller waits for the button */
    char gstate[12];                /* the GRBL controller's state name, "" with none */
    int alarm;
    unsigned homed_axes;
    char home_source[16];
    int released;                   /* the X and Y motors */
    char lease[48];                 /* the machine lease's innermost holder, "" with none */
    int button_wait;                /* this daemon waits for a press (the setup's, an install's) */
    char update[48];                /* a release newer than the installed one, "" with none */
    int gate_open;                  /* the setup's controller gate */
    char gate_why[96];
    char phase[16];                 /* the cooling engine's, for the telemetry */
    double down_c, up_c;
} events_snap_t;

typedef void (*events_emit_fn)(void *ctx, const char *name, const char *data_json);

/* Every event the step from a to b is, in a fixed order. */
void events_diff(const events_snap_t *a, const events_snap_t *b, events_emit_fn emit, void *ctx);

/* The button, from one switch word to the next: a "button" event for each
 * edge, but only when nothing else is waiting for the press - not while a
 * job arms or runs, not under the machine lease (a wizard, a diagnostic),
 * and not while this daemon waits for it itself. The state is the last
 * full snapshot's. */
void events_button(unsigned long was, unsigned long now, const events_snap_t *state, events_emit_fn emit, void *ctx);

/* telemetry.tick's data: what the daemon already holds, no sensor read. */
void events_telemetry(const events_snap_t *s, char *out, size_t len);

/* ---- the stream ---- */

void events_init(void);
void events_shutdown(void);

/* One subscriber. NULL when the cap refuses it, with the reason in why.
 * It ends the older stream of the same address, if there is one. */
typedef struct events_client events_client_t;
events_client_t *events_open(const char *peer, char *why, size_t len);
/* The extension host's: the slot beyond the cap, never refused for want
 * of room, and replacing whatever held it. */
events_client_t *events_open_host(char *why, size_t len);
void events_close(events_client_t *c);

/* The next bytes of the stream into buf: an event, or a keep-alive
 * comment when nothing happened for a while. Blocks. -1 at shutdown and
 * after the "bye" of a stream that was replaced. */
long events_next(events_client_t *c, char *buf, size_t max);

/* Fills the snapshot from the daemon's state; set before events_init().
 * A field it cannot read is left as it came in. */
extern void (*events_gather)(events_snap_t *snap);
/* The switch word alone, for the button's faster look. */
extern unsigned long (*events_switches)(void);

/* For a caller that has an edge of its own to report. */
void events_publish(const char *name, const char *data_json);

#endif
