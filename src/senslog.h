/*
 * senslog.h - forgectrl: the sensors logger
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * A record of the machine's sensors and of what was commanded, written
 * to a logger of its own ("sensors", /data/log/forgefirm/sensors/) so
 * the data never competes with the event logs for room. Its own thread
 * samples every sensor once a second, apart from the cooling engine, so
 * nothing here can delay a verdict. It writes:
 *
 *   beat   every sensor, the commanded outputs and the engine's verdict:
 *          every 10 s while a session is open, cooling down or in
 *          diagnostics, every 60 s at idle, and on every phase change
 *   job    one per job, from the session's opening to the machine's
 *          return to idle
 *   inv    the machine's inventory; cfg the settings in force: at start,
 *          when the clock is set, when either changes, and daily
 *   clock  the wall clock stepped (the record that re-dates the lines
 *          written before it)
 *   fr     the flight recorder: on a warning from the engine, the 1 Hz
 *          samples from 120 s before it to 30 s after, one line a channel
 *
 * The formats are senslogfmt.h's. Nothing here is a secret or an
 * identity: no serial number, no network name or address.
 */
#ifndef FORGECTRL_SENSLOG_H
#define FORGECTRL_SENSLOG_H

#include "senslogfmt.h"

/* Start and stop the thread. senslog_start registers the logger's
 * program name with fflog and samples the cooling engine, so it comes
 * after fflog_init and cool_init; senslog_stop comes before
 * cool_shutdown. */
void senslog_start(void);
void senslog_stop(void);

/* A warning from the cooling engine: arms the flight recorder (its dump
 * follows SENSLOG_FR_POST_S later). Safe from any thread. */
void senslog_event(const char *msg);

#endif
