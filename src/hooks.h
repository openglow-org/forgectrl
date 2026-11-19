/*
 * hooks.h - functions main.c provides to the other modules
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_HOOKS_H
#define FORGECTRL_HOOKS_H
#include <stddef.h>

/* Validate one settings key and value against the settings table (the
 * same validators POST /settings runs). Returns 1 when key is known and
 * the value is legal, 0 otherwise; an empty value (clear) is legal for
 * every known key. */
int setting_valid(const char *key, const char *val);
/* Apply the wifi_country setting to the radio (set_region = 1 to push a
 * region change; 0 for the startup pass). */
void apply_wifi(int set_region);
/* The fuse serial (0 when unreadable) and the firmware version string. */
unsigned long fuse_serial(void);
void read_fw_version(char *buf, size_t len);
/* The dev bar for a page with its own styles (it carries them inline), or
 * "" on a release image. */
const char *page_dev_bar_inline(void);
/* Whether a job would take a press of the machine button as its own: a
 * job armed and waiting for its press (a GRBL arm, a cloud print) or
 * inside its armed window, or a GRBL job running or paused, where the
 * button is the pause and resume; and a live GRBL controller whose state
 * file cannot be read or is not current (jobstream.h), which could hide
 * any of those. Every route that asks the operator to
 * hold the button as proof of presence (operator_present, auth.h) asks
 * this first and answers 409 with @why, so the hold it would ask for is
 * never a job's press. Returns 1 with the refusal in @why, 0 with none. */
int job_takes_button(char *why, size_t len);
/* Whether the button held now was asked for inside the control panel's
 * own reservation (btnres.h, POST /button/reserve): the reservation stands
 * and has been on file for BTNRES_READY_S, so no controller could take a
 * press as its own when the dialog asked for it. Every route that reads the
 * button as proof of presence asks this after job_takes_button and
 * operator_present, and answers 409 with @why when it is not. Returns 1
 * when it was, 0 with the refusal in @why. */
int panel_press_reserved(char *why, size_t len);
#endif
