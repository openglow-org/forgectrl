/*
 * flowrun.h - forgectrl: the coolant flow watch's thread and heater
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Runs flowwatch.c when cool_flow_watch is 1 (opt-in; off by default): reads
 * the PIC sampler's 1 s means (pic/means), steps the watch, and writes the
 * loop heater it asks for. The watch enforces nothing and never touches the
 * pump (see flowwatch.h). With the setting off the thread reads no sensor
 * file and wakes only to read the setting again, every 5 s.
 *
 * A start rests before the first hold: the full minute unless a stamp in
 * the run directory says this daemon's heater has been off for longer
 * (the stamp stands only while the heater has been off since it).
 *
 * The cooling engine stays the owner of the thermal hardware: it hands the
 * watch its context every tick and takes the heater back at once
 * (flowrun_yield) before it writes it. A context more than 3 s old (an
 * engine that stopped ticking) counts as the engine taking it back.
 *
 * The kernel holds a heater write above zero only for its lease
 * (heater_lease_ms, 5 s): the watch writes its duty again every second.
 */
#ifndef FORGECTRL_FLOWRUN_H
#define FORGECTRL_FLOWRUN_H

#include "flowwatch.h"

#include <stddef.h>

typedef struct {
    double t;                   /* the engine tick's start, CLOCK_MONOTONIC seconds */
    int yield;                  /* the engine needs the heater */
    const char *yield_why;      /* a static string */
    int session;                /* a run session at the run fans */
    int fans;                   /* fw_fans_t */
    double offset_counts;       /* the engine's air-assist shift, counts (taken off as read_temp does) */
    double temp_offset_c;       /* the engine's per-machine correction, C */
} flowrun_ctx_t;

/* cool_init: the thread, and the watch begun. */
void flowrun_init(void);
/* Every engine tick. */
void flowrun_engine(const flowrun_ctx_t *c);
/* Synchronous: the heater off now, and no write of it until an engine tick
 * that began after this call lets go. */
void flowrun_yield(const char *why);
/* cool_shutdown: the thread stopped, the heater off. */
void flowrun_shutdown(void);
/* GET /cool/flow. */
int flowrun_status_json(char *buf, size_t len);

#endif
