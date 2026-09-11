/*
 * lens.h - the lens frame: the head's one reference and the window around it
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Z is the focal point's height above the tray. The head's one position
 * reference is the hall sensor's rising edge; the focal height when the
 * lens sits on it is the focus card's number (lens_hall_edge_z_mm), and
 * the free travel around it, in half-steps of the lens screw, is the
 * card's stop count (lens_stop_below_steps, lens_stop_above_steps). All
 * three live in the shared settings file. The controller reads the same
 * three keys when it starts and opens its Z soft limit to that window;
 * every Z the daemon sends or shows comes from these functions over the
 * same keys, so the two frames are one by construction. Until the card
 * has run, or when the stops could not be found on a head, the fallback
 * window stands: the travel every head reaches without touching a stop.
 */
#ifndef FORGECTRL_LENS_H
#define FORGECTRL_LENS_H

/* The bench reference machine's edge, until this head's is measured. */
#define LENS_EDGE_Z_DEFAULT 3.35
/* The fallback window: half-steps below and above the edge. */
#define LENS_WINDOW_DOWN 10
#define LENS_WINDOW_UP   12
/* A stop count the settings accept. */
#define LENS_STOP_STEPS_MAX 40
/* The screw's scale, the controller's $102: half-steps per millimeter,
 * 36 over the carriage's 12.32 mm. Every head shares the screw. */
#define LENS_STEPS_PER_MM 2.922

/* The focal height at the edge: the settings, else the default. */
double lens_edge_z(void);

/* The window: the settings (1 to LENS_STOP_STEPS_MAX each), else the
 * fallback. Returns 1 when both counts are the focus card's. */
int lens_window(int *down, int *up);

/* Half-steps from the edge (up positive) into Z, over the scale `spm`. */
double lens_z_of_k(double edge_z, double spm, double k);

/* The reach: the Z the lens goes to at the bottom and the top of the
 * window. Every Z the daemon commands lies in it. */
void lens_reach(double edge_z, double spm, int down, int up, double *lo, double *hi);

/* The controller's Z soft limit in the daemon's frame (a program declares
 * G92 Z<edge_z> with the lens on the edge): the reach and one half-step
 * of slack at each end. The reach lies inside it whatever step the
 * controller placed the edge on, since the G92 relabels that step. */
void lens_envelope(double edge_z, double spm, int down, int up, double *lo, double *hi);

/* The focus ladder: n lines spread over the window on whole half-steps,
 * the first at the top of the window and the last at the bottom. */
void lens_ladder_k(int down, int up, int n, int *k);

/* A program line that moves Z (G0 or G1 with a Z word): its Z in *z, 1;
 * else 0. G92 and every other line are not moves. */
int lens_line_z(const char *line, double *z);

#endif
