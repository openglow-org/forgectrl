/*
 * extpkg.h - the operator's door to extension packages: forgectrl asks the extension host
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * forgectrl is the machine's one front door (TLS, the login, the origin
 * checks), and the extension host (forgeext) is the only program that
 * reads what a package brought. So the panel's package routes are a relay:
 * forgectrl runs the host's command line, which answers in JSON, takes the
 * host's lock around every change, and is what root at the console runs
 * too. The host's daemon picks a change up on its next turn.
 *
 * Nothing of a request reaches a shell: the command is an argument vector,
 * a package id is checked against the form of one before it is an
 * argument, and the actions are a closed list.
 */
#ifndef FORGECTRL_EXTPKG_H
#define FORGECTRL_EXTPKG_H

#include <stddef.h>

#define EXTPKG_BIN_DEFAULT      "/usr/bin/forgeext"
#define EXTPKG_STATUS_FILE      "/run/forgefirm/ext/status.json"
#define EXTPKG_SAFE_FILE        "/run/forgefirm/ext-safe"
#define EXTPKG_STAGE_DEFAULT    "/data/forgefirm/tmp/ext-upload.ffx"
#define EXTPKG_UPLOAD_MAX       (32UL * 1024 * 1024)    /* the host takes no larger archive */
#define EXTPKG_PHRASE           "I UNDERSTAND"
#define EXTPKG_GRANTS_MAX       8
#define EXTPKG_KEY_MAX          4096            /* a public key as fwup writes it, with room to spare */
#define EXTPKG_OUT_MAX          (256 * 1024)
#define EXTPKG_TIMEOUT_S        60

/* Lowercase reverse-DNS, as the host installs them: 1 when id has the form. */
int extpkg_id_ok(const char *id);

/* Run the host's command line with these arguments (argv[0] is filled in).
 * Its standard output, NUL-ended, into *out (the caller frees it). The
 * exit status, or -1 when it could not be run, did not end in time, or
 * said more than EXTPKG_OUT_MAX. */
int extpkg_cli(const char *const argv[], char **out);

/* The document of GET /ext/status, malloc'd: whether extensions are on,
 * safe mode, the host's own status file (or that no host is running), and
 * the installed packages and keys as the host lists them. NULL when out of
 * memory. Every panel tab and package page asks for it, and each list is a
 * run of the host's command line (a fork of this daemon), so the two lists
 * are kept EXTPKG_STATUS_CACHE_S on extpkg_clock once both were read: any
 * other command this daemon runs through the host (an install, a removal,
 * a switch, a key) drops them, so a change made through this daemon shows
 * at the next read, and one the host makes of its own shows within those
 * seconds. The status file is read at every request. */
#define EXTPKG_STATUS_CACHE_S   3.0
char *extpkg_status_json(int ext_enabled);

/* The clock the status lists are kept on: CLOCK_MONOTONIC seconds; a test
 * sets its own. */
extern double (*extpkg_clock)(void);

/* One action on one package: enable, disable, remove, remove-keep-data,
 * hold-required, hold-advisory. 0 when the host did it. Otherwise the HTTP
 * status to refuse with (400 for a request that has no such form, 409 for
 * the host's refusal, 502 when the host cannot be asked) and the words. */
int extpkg_action(const char *id, const char *action, int *status, char *why, size_t wlen);

/* One call from a package's page to its own service, which the bridge
 * makes for the page: the host's `call` command, whose answer is the
 * service's status and the JSON it answered (malloc'd). method is GET or
 * POST, path is a path, body a JSON object of at most EXTPKG_CALL_BODY_MAX
 * bytes (a POST only; NULL for none); the host judges the rest. NULL with
 * the HTTP status to refuse with (400 for a call out of form, 409 for the
 * host's refusal - not installed, no service running, frozen while a job
 * is armed, no answer in time - and 502 when the host cannot be asked)
 * and the words. */
#define EXTPKG_CALL_BODY_MAX    4096
char *extpkg_call_json(const char *id, const char *method, const char *path, const char *body, int *status, char *why,
                       size_t wlen);

/* A destination the operator names for a package that asks for them
 * (net.outbound.operator), or takes away: action is add or remove, dest
 * is host:port. The host judges the form, the package, and the limit. 0
 * when it did it; otherwise the status to refuse with (400 for a request
 * with no such form, 409 for the host's refusal, 502 when the host cannot
 * be asked) and the words. */
int extpkg_dest(const char *id, const char *action, const char *dest, int *status, char *why, size_t wlen);

/* Every package, its data, and the owner's keys, for a change of owner:
 * a package can hold the last owner's credentials, and a key they added
 * would go on making their packages read as trusted. The counts into
 * *packages and *keys. 0, or -1 with the words. This is the account
 * reset's alone; nothing an operator does reaches it. */
int extpkg_wipe(int *packages, int *keys, char *why, size_t wlen);

/* How many packages are installed, for the offer to wipe them: -1 when
 * the host cannot be asked. */
int extpkg_count(void);

/* Installing, in two requests. The archive is uploaded to one staging
 * file and the host is asked what it is (inspect changes nothing): the
 * answer goes to the operator with the consent its tier takes. Then the
 * install names the grants, the digest of the archive it was shown and the
 * consent, and forgectrl asks the host again what the staged file is,
 * because the tier is never the client's to say:
 *
 *   official      the login
 *   community     the login and the typed phrase
 *   unverified    the login and the machine's button pressed for this
 *                 install (btnres.h, the panel's consent), as for unsigned
 *                 firmware
 *
 * The staging file is one for every session: the install is of the archive
 * that was shown or of nothing. forgectrl's own inspect must name the
 * digest the install carries, and the host is given it to hold the bytes it
 * reads to (forgeext install --digest), so another archive staged at any
 * moment between the upload's answer and the host's read is refused.
 */
const char *extpkg_stage_path(void);
void extpkg_stage_discard(void);

/* The host's inspect of the staged archive with "consent" added ("login",
 * "typed", "button"), malloc'd; NULL with the status and the words when
 * the host refuses the archive (400, and the staged file is removed) or
 * cannot be asked (502). */
char *extpkg_inspect_json(int *status, char *why, size_t wlen);

/* A package's interface, as the host reads it out of the installed
 * package: the JSON the panel is given, or NULL with the status and the
 * words. The page is never markup this daemon composes - it goes to the
 * panel as a JSON string, and the panel is what puts it in a frame. */
char *extpkg_ui_json(const char *id, int *status, char *why, size_t wlen);

/* A package's own settings, read or patched on the operator's behalf.
 * patch is NULL to read. The host holds the schema and judges every
 * value; this only carries the request and the answer. */
char *extpkg_settings_json(const char *id, const char *patch, int *status, char *why, size_t wlen);

/* Install the staged archive, which must be the one whose digest the
 * operator was shown (@digest, the inspect's "digest"). grants is a
 * comma-separated list of capabilities the operator grants (or NULL);
 * @pressed, the operator's press for this install was taken. 0 when
 * installed (the staged file is removed), with "<id> <version>" in @what.
 * Otherwise the status (400 for a request with no such form or a missing
 * consent, 409 for an archive that is not the one shown, for the host's
 * refusal and for a button not pressed, 502) and the words; the staged file
 * stays for another try. */
int extpkg_install(const char *grants, const char *phrase, const char *digest, int pressed, char *what, size_t whlen,
                   int *status, char *why, size_t wlen);

/* Whether the staged file is the one whose SHA-256 (lowercase hex, as
 * the inspect document's "sha256" gives it) the install dialog showed: 0,
 * or 409 and the words when it is not, or nothing is staged. */
int extpkg_stage_matches(const char *sha256, int *status, char *why, size_t wlen);
/* Told the id of each package installed, as the host's inspect named it
 * (main.c: a package installed anew may ask for a press again, btnres.h).
 * NULL: nobody is told. */
extern void (*extpkg_installed)(const char *id);

/* The owner's keys, the trust anchor a community package is judged by.
 * Adding one is the operator's own act: forgectrl takes it only with the
 * machine's button pressed for it (@pressed), as for unsigned firmware. The
 * key is written through the extension host, which parses it before it
 * lands, so a file that is no Ed25519 public key never becomes a trust
 * anchor.
 *
 * 0 when the host did it, with the key's id as the host lists it in @kid
 * ("" when it lists none by that name); otherwise the status (400 for a
 * name or a key with no such form, 409 for the host's refusal and for a
 * button not pressed, 502 when the host cannot be asked) and the words. */
int extpkg_key_add(const char *name, const char *key, size_t klen, int pressed, char *kid, size_t kidlen, int *status,
                   char *why, size_t wlen);
int extpkg_key_remove(const char *name, int *status, char *why, size_t wlen);

/* The catalog: the signed index of the packages OpenGlow lists, and the
 * way to get one of them. It is fetched only when the operator asks (the
 * privacy advisory says so), from one fixed https:// address, with curl
 * and no shell, and the extension host verifies it - signed with the
 * OpenGlow extension key and by nothing else, and never older than the
 * one it keeps - and keeps it. The host judges each listed version against
 * this firmware when it reads the index, and offers the newest one it
 * runs; a package from it is that version, fetched from the address the
 * kept index names, and its bytes must be the size and the SHA-256 the
 * index names before the host reads them. Then it is staged as an upload
 * is, and the install that follows is the upload's, with the consent its
 * tier takes: the catalog changes where an archive comes from, and nothing
 * about how it is judged. The address names the index's schema (index-1):
 * a catalog that ever changes its form publishes a second file beside
 * this one, and this firmware keeps reading its own. */
#define EXTPKG_CURL_DEFAULT     "/usr/bin/curl"
#define EXTPKG_INDEX_URL \
    "https://github.com/openglow-org/forgefirm-extensions-catalog/releases/latest/download/index-1.ffi"
#define EXTPKG_INDEX_FETCH_MAX  (2UL * 1024 * 1024)
#define EXTPKG_INDEX_FETCH_S    30
#define EXTPKG_PKG_FETCH_S      240

/* GET /ext/catalog: the host's kept index ({"index": the document, or null
 * when none is kept}) and the address it is fetched from, malloc'd; NULL
 * with the status (502) and the words. */
char *extpkg_catalog_json(int *status, char *why, size_t wlen);

/* Fetch the index and have the host verify and keep it. 0 with its number
 * of packages and its version; otherwise the status (502 when it could not
 * be fetched or the host cannot be asked, 409 for the host's refusal: the
 * one it kept stays) and the words. */
int extpkg_catalog_refresh(int *npkgs, char *version, size_t vlen, int *status, char *why, size_t wlen);

/* Fetch the version of the package the kept index lists under id that the
 * host offers - the newest one this firmware runs - and stage it: the
 * host's inspect of it, with "consent" and "catalog" added, malloc'd. NULL
 * with the status (400 for an id with no such form, 409 with no index
 * kept, for a package with no version this firmware runs, and for bytes
 * that are not the ones it names, 404 for an id it does not list, 400 when
 * the host refuses the archive, 502 when it could not be fetched or the
 * host cannot be asked) and the words; nothing stays staged then. */
char *extpkg_catalog_get(const char *id, int *status, char *why, size_t wlen);

/* The M-codes extension packages answer now, from the host's own status
 * file (a running service that may run while a job is armed): "-" for
 * none, or the numbers in order, comma separated, as the GRBL controller's
 * port takes them. A host that is not running answers none. */
void extpkg_mcode_table(char *out, size_t len);

/* M<code> of a job, with its words (a JSON object of P, Q and R), to the
 * service that answers it: the host's mcode command, bounded by
 * EXTPKG_MCODE_TIMEOUT_S. Its answer (id, the service's status, and its
 * body), malloc'd; NULL with the host's refusal in its words, or that the
 * host could not be asked. */
#define EXTPKG_MCODE_TIMEOUT_S  25
char *extpkg_mcode_json(int code, const char *words, char *why, size_t wlen);

/* The packages that add a check of their own to the Setup page and whose
 * service runs now: [{id, name}], a new reference (an empty list when the
 * host cannot be asked). */
#include <jansson.h>
json_t *extpkg_wizard_list(void);

/* One step of a package's check: the host's wizard command, verb state,
 * start, answer (with body, the operator's answer as JSON), or abort. The
 * host's answer (id, the service's status and body), malloc'd; NULL with the
 * words. */
char *extpkg_wizard_json(const char *id, const char *verb, const char *body, char *why, size_t wlen);

#endif
