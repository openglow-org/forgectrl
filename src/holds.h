/*
 * holds.h - the holds an extension package has on a job, as the engine reads them
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * A hold withholds fire and never permits it: it joins the verdict's pause
 * tier and nothing else. The extension host (forgeext) owns the holds. It
 * keeps one file per package that has the operator's hold grant under
 * HOLDS_DIR and rewrites each with a fresh monotonic timestamp for as long
 * as it runs, so a package that is frozen for the job changes nothing:
 *
 *   {"id": "org.example.badge", "required": true, "raised": false,
 *    "reason": "no badge presented", "ts_mono": 1234.567}
 *
 * The engine reads the directory once a tick:
 *
 *   fresh, raised        the hold stands, reason "<id>: <reason>"
 *   fresh, not raised    nothing
 *   stale, required      the hold stands whatever it last said: the host
 *                        that speaks for the package is not answering
 *   stale, advisory      dropped, and counted
 *   cannot be read       the hold stands: a file here that does not parse
 *                        is a defect, and what it meant is not known
 *
 *   no file at all, and the package is named in the required directory
 *                        the hold stands: the host has not spoken for it
 *                        since the boot, or has taken its word away
 *
 * The files live on a tmpfs and are gone with a reboot, and a host that
 * never comes up writes none. So the host also keeps, on /data, one empty
 * file named after each package whose hold the operator marked required
 * (HOLDS_REQUIRED_DIR); only the names are read.
 *
 * Fresh is HOLDS_FRESH_S, the rule the controllers apply to the verdict.
 * Whether holds are read at all is the caller's: with extensions off, or in
 * safe mode, the engine does not look, and that exit does not depend on the
 * host.
 */
#ifndef HOLDS_H
#define HOLDS_H

#define HOLDS_DIR_DEFAULT   "/run/forgefirm/holds"
#define HOLDS_REQUIRED_DIR  "/data/forgefirm/ext/required-holds"
#define HOLDS_FRESH_S       2.0
#define HOLDS_MAX           32          /* the account pool's size: more files than that is a defect */
#define HOLDS_REASON_MAX    112         /* COOL_REASON_MAX */
#define HOLDS_FILE_MAX      1024        /* bytes of one hold file */

typedef struct {
    int standing;                       /* 1: fire is withheld */
    int files;                          /* hold files read */
    int raised;                         /* fresh ones that are raised */
    int stale_required;                 /* required ones that stand because they are stale, or have no file */
    int stale_advisory;                 /* advisory ones dropped because they are stale */
    int unreadable;                     /* files that did not parse */
    char reason[HOLDS_REASON_MAX];      /* of the first standing hold, by file name */
} holds_t;

/* Read every hold under dir at the monotonic time now, and hold for every
 * package named under required_dir that has no file there. A directory
 * that does not exist holds nothing. */
void holds_read(const char *dir, const char *required_dir, double now, holds_t *out);

#endif
