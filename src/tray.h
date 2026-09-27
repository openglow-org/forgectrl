/*
 * tray.h - the crumb tray, in or out
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The crumb tray can be removed. The GRBL controller keeps the mode (M103
 * P0 or P1, or the port's tray op) and persists it as the marker tray.out
 * in the data directory: present means the tray is out, and the
 * controller is its only writer. With the tray out, the controller's Z is
 * the focal height above the floor of the cut area: its Z position and its
 * Z range move up by tray_offset_mm, on the lens step grid (the same
 * whole half-steps the controller shifts by). The daemon reads the marker
 * for the status and holds two rules on it: the offset changes only while
 * the tray is in, so the offset in force never changes under the
 * controller's live frame; and the setup cards run only with the tray in,
 * because the setup sheet is burned on the tray and the focus card
 * measures the tray-in frame.
 */
#ifndef FORGECTRL_TRAY_H
#define FORGECTRL_TRAY_H

#include <stddef.h>

/* 1.35 in: the floor of the cut area below the tray on the bench
 * reference. Held to 13 to 60 mm: above the lens's travel, so the Z ranges
 * of the two modes never overlap. */
#define TRAY_OFFSET_DEFAULT_MM 34.29
#define TRAY_OFFSET_MIN_MM     13.0
#define TRAY_OFFSET_MAX_MM     60.0

/* 1 while the tray is out (the controller's marker). */
int tray_is_out(void);

/* The offset setting as the controller reads it: the value, held to its
 * range; the default when it is unset or not a number. */
double tray_offset_mm(void);

/* The offset on the lens step grid, as the controller applies it. */
double tray_grid_mm(void);

/* The Z shift in force: 0 with the tray in, the grid offset with it out. */
double tray_shift_mm(void);

/* A value tray_offset_mm takes: a number from 13 to 60. */
int tray_offset_valid(const char *v);

/* A settings request that would change tray_offset_mm (v, NULL when the
 * request does not carry it; an empty value unsets it) while the tray is
 * out: 1, with the words in why. 0 when it may go ahead. */
int tray_offset_refusal(const char *v, char *why, size_t len);

/* The setup cards need the tray in: 1 with the words in why while it is
 * out, else 0. */
int tray_setup_refusal(char *why, size_t len);

#endif
