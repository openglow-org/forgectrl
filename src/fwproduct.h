/*
 * fwproduct.h - the product gate on the firmware paths
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Firmware and extension packages are the same container: an fwup
 * archive, verified by the same program. A signature says who made an
 * archive, never what it is, and `fwup -a` applies whatever task it
 * finds. So every firmware path reads the archive's product before it
 * goes on, and takes firmware only: ForgeFIRM's under the release key or
 * unsigned, the factory's under a factory key or unsigned. The extension
 * host holds the other door the same way.
 */
#ifndef FWPRODUCT_H
#define FWPRODUCT_H

#include <stddef.h>

#define FWPRODUCT_FORGEFIRM "ForgeFIRM firmware"
#define FWPRODUCT_FACTORY   "Glowforge firmware"
#define FWPRODUCT_EXTENSION "ForgeFIRM extension"

/* Signature classes, as update.c's fw_classify gives them. */
#define FWCLASS_RELEASE  2      /* the ForgeFIRM release key */
#define FWCLASS_FACTORY  1      /* a Glowforge factory key */
#define FWCLASS_NONE     0      /* a valid archive under neither */

/* The value of meta-product in `fwup -m` output, quotes removed: 0, or -1
 * when the text has no such line (out is then ""). Only a line that
 * starts with the key counts. */
int fwproduct_parse(const char *fwup_m, char *out, size_t len);

/* NULL when a firmware path may go on with an archive of this product
 * and signature class; otherwise the words of the refusal. */
const char *fwproduct_refusal(const char *product, int cls);

/* The gate on an archive file: asks fwup for its metadata (the raw text,
 * its quotes and line ends intact) and judges the product it names. An
 * archive whose metadata fwup does not print has no product and is
 * refused. `file` is one of the daemon's own staging paths. */
const char *fwproduct_gate(const char *file, int cls);

#endif
