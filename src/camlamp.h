/*
 * camlamp.h - a camera lamp write and its result
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The camera lamps are registers: the head white LED on the head board, the
 * lid LED on the PIC. The kernel reads a head register back after a write
 * and writes it again until it holds, and fails the write of one that never
 * does (the docs site, kernel-module.md). The write reaches the attribute
 * when the stream is flushed, at fclose, so that is where its result is.
 */
#ifndef FORGECTRL_CAMLAMP_H
#define FORGECTRL_CAMLAMP_H

/* Write @level to the lamp attribute at @path: 0, or the errno value the
 * write failed with (ENOENT for a lamp with no attribute). */
int camlamp_write(const char *path, int level);

#endif
