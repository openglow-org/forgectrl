/*
 * extpkg.c - the operator's door to extension packages: forgectrl asks the extension host
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * See extpkg.h.
 */
#define _GNU_SOURCE
#include "extpkg.h"

#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sha256.h"

static const char *bin(void)
{
    const char *v = getenv("FORGECTRL_FORGEEXT");       /* a test's stand-in */
    return v && v[0] ? v : EXTPKG_BIN_DEFAULT;
}

int extpkg_id_ok(const char *id)
{
    if (!id)
        return 0;
    size_t n = strlen(id);
    if (n < 3 || n > 63 || id[0] == '.' || id[0] == '-' || id[n - 1] == '.' || !strchr(id, '.'))
        return 0;
    if (strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789.-") != n || strstr(id, ".."))
        return 0;
    return 1;
}

static double mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Run prog with these arguments and no shell: its standard output (and its
 * standard error too, with both) into *out, within timeout_s. The exit
 * status, or -1 when it could not be run, did not end in time, or said more
 * than EXTPKG_OUT_MAX. */
static int spawn(const char *prog, const char *const argv_in[], char **out, int timeout_s, int both)
{
    const char *argv[32];
    int n = 0, pfd[2];
    *out = NULL;
    argv[n++] = prog;
    for (int i = 0; argv_in[i] && n < 31; i++)
        argv[n++] = argv_in[i];
    argv[n] = NULL;
    if (pipe2(pfd, O_CLOEXEC) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, 0);
            dup2(null, 2);
        }
        dup2(pfd[1], 1);
        if (both)
            dup2(pfd[1], 2);
        /* Nothing of forgectrl's goes along: it holds the pulse device, and
         * that descriptor is inheritable on purpose, for the controllers. */
        if (close_range(3, ~0U, 0) != 0)
            for (int fd = 3; fd < 1024; fd++)
                close(fd);
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(pfd[1]);
    char *buf = malloc(EXTPKG_OUT_MAX + 1);
    size_t len = 0;
    int bad = buf == NULL;
    double end = mono() + timeout_s;
    while (!bad) {
        struct pollfd p = { pfd[0], POLLIN, 0 };
        double left = end - mono();
        if (left <= 0 || poll(&p, 1, (int)(left * 1000) + 1) <= 0) {
            bad = 1;
            break;
        }
        ssize_t got = read(pfd[0], buf + len, EXTPKG_OUT_MAX - len);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        len += (size_t)got;
        if (len >= EXTPKG_OUT_MAX)
            bad = 1;
    }
    close(pfd[0]);
    if (bad)
        kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (bad || !WIFEXITED(status) || WEXITSTATUS(status) == 127) {
        free(buf);
        return -1;
    }
    buf[len] = '\0';
    *out = buf;
    return WEXITSTATUS(status);
}

int extpkg_cli(const char *const argv[], char **out)
{
    return spawn(bin(), argv, out, EXTPKG_TIMEOUT_S, 0);
}

static int host_running(json_int_t pid)
{
    char link[64], exe[256];
    if (pid <= 1)
        return 0;
    snprintf(link, sizeof(link), "/proc/%lld/exe", (long long)pid);
    ssize_t n = readlink(link, exe, sizeof(exe) - 1);
    if (n <= 0)
        return 0;
    exe[n] = '\0';
    return strcmp(exe, bin()) == 0;
}

char *extpkg_status_json(int ext_enabled)
{
    json_t *top = json_object();
    if (!top)
        return NULL;
    json_object_set_new(top, "enabled", json_boolean(ext_enabled));
    json_object_set_new(top, "safe_mode", json_boolean(access(EXTPKG_SAFE_FILE, F_OK) == 0));

    /* The host's own word, when the host that wrote it is still there. */
    const char *sf = getenv("FORGECTRL_EXT_STATUS");
    json_t *host = json_load_file(sf && sf[0] ? sf : EXTPKG_STATUS_FILE, 0, NULL);
    int running = json_is_object(host) && host_running(json_integer_value(json_object_get(host, "pid")));
    if (!running) {
        json_decref(host);
        host = json_object();
    }
    json_object_set_new(host, "running", json_boolean(running));
    json_object_set_new(top, "host", host);

    char *text = NULL;
    const char *argv[] = { "list", NULL };
    json_t *listed = extpkg_cli(argv, &text) == 0 && text ? json_loads(text, 0, NULL) : NULL;
    free(text);
    json_t *pkgs = json_object_get(listed, "packages");
    if (json_is_array(pkgs)) {
        json_object_set(top, "packages", pkgs);
    } else {
        json_object_set_new(top, "packages", json_array());
        json_object_set_new(top, "error", json_string("the extension host's command line does not answer"));
    }
    json_decref(listed);
    text = NULL;
    const char *kargv[] = { "keys", NULL };
    json_t *klist = extpkg_cli(kargv, &text) == 0 && text ? json_loads(text, 0, NULL) : NULL;
    free(text);
    json_t *keys = json_object_get(klist, "keys");
    if (json_is_array(keys))
        json_object_set(top, "keys", keys);
    else
        json_object_set_new(top, "keys", json_array());
    json_decref(klist);
    char *doc = json_dumps(top, JSON_COMPACT);
    json_decref(top);
    return doc;
}

int extpkg_count(void)
{
    char *text = NULL;
    const char *argv[] = { "list", NULL };
    if (extpkg_cli(argv, &text) != 0 || !text) {
        free(text);
        return -1;
    }
    json_t *j = json_loads(text, 0, NULL);
    free(text);
    json_t *pkgs = json_object_get(j, "packages");
    int n = json_is_array(pkgs) ? (int)json_array_size(pkgs) : -1;
    json_decref(j);
    return n;
}

int extpkg_wipe(int *packages, int *keys, char *why, size_t wlen)
{
    char *text = NULL;
    const char *argv[] = { "wipe", NULL };
    *packages = *keys = 0;
    int rc = extpkg_cli(argv, &text);
    json_t *j = text ? json_loads(text, 0, NULL) : NULL;
    free(text);
    if (rc < 0 || !json_is_object(j)) {
        json_decref(j);
        snprintf(why, wlen, "the extension host's command line does not answer");
        return -1;
    }
    if (rc != 0 || !json_is_true(json_object_get(j, "ok"))) {
        const char *e = json_string_value(json_object_get(j, "error"));
        snprintf(why, wlen, "%s", e && e[0] ? e : "the extension host refused");
        json_decref(j);
        return -1;
    }
    *packages = (int)json_integer_value(json_object_get(j, "packages"));
    *keys = (int)json_integer_value(json_object_get(j, "keys"));
    json_decref(j);
    return 0;
}

int extpkg_action(const char *id, const char *action, int *status, char *why, size_t wlen)
{
    static const struct { const char *action; const char *argv[4]; int id_at; } acts[] = {
        { "enable",           { "enable",  NULL, NULL, NULL },           1 },
        { "disable",          { "disable", NULL, NULL, NULL },           1 },
        { "remove",           { "remove",  NULL, NULL, NULL },           1 },
        { "remove-keep-data", { "remove",  NULL, "--keep-data", NULL },  1 },
        { "hold-required",    { "hold",    NULL, "required", NULL },     1 },
        { "hold-advisory",    { "hold",    NULL, "advisory", NULL },     1 },
    };
    *status = 400;
    if (!extpkg_id_ok(id)) {
        snprintf(why, wlen, "id is a package id");
        return -1;
    }
    for (size_t i = 0; action && i < sizeof(acts) / sizeof(acts[0]); i++) {
        if (strcmp(action, acts[i].action) != 0)
            continue;
        const char *argv[5] = { acts[i].argv[0], acts[i].argv[1], acts[i].argv[2], acts[i].argv[3], NULL };
        argv[acts[i].id_at] = id;
        char *text = NULL;
        int rc = extpkg_cli(argv, &text);
        json_t *j = text ? json_loads(text, 0, NULL) : NULL;
        free(text);
        if (rc < 0 || !json_is_object(j)) {
            json_decref(j);
            *status = 502;
            snprintf(why, wlen, "the extension host's command line does not answer");
            return -1;
        }
        if (rc != 0 || !json_is_true(json_object_get(j, "ok"))) {
            const char *e = json_string_value(json_object_get(j, "error"));
            *status = 409;
            snprintf(why, wlen, "%s", e && e[0] ? e : "the extension host refused");
            json_decref(j);
            return -1;
        }
        json_decref(j);
        *status = 200;
        return 0;
    }
    snprintf(why, wlen, "action is enable, disable, remove, remove-keep-data, hold-required, or hold-advisory");
    return -1;
}

/* ---- installing ------------------------------------------------------------------ */

const char *extpkg_stage_path(void)
{
    const char *v = getenv("FORGECTRL_EXT_STAGE");      /* a test's staging file */
    return v && v[0] ? v : EXTPKG_STAGE_DEFAULT;
}

void extpkg_stage_discard(void)
{
    unlink(extpkg_stage_path());
}

/* The host's inspect of the staged file: the JSON object, or NULL with the
 * status and the words. */
static json_t *inspect(int *status, char *why, size_t wlen)
{
    const char *argv[] = { "inspect", extpkg_stage_path(), NULL };
    char *text = NULL;
    if (access(extpkg_stage_path(), R_OK) != 0) {
        *status = 409;
        snprintf(why, wlen, "no package is staged: upload one first");
        return NULL;
    }
    int rc = extpkg_cli(argv, &text);
    json_t *j = text ? json_loads(text, 0, NULL) : NULL;
    free(text);
    if (rc < 0 || !json_is_object(j)) {
        json_decref(j);
        *status = 502;
        snprintf(why, wlen, "the extension host's command line does not answer");
        return NULL;
    }
    if (rc != 0 || !json_is_true(json_object_get(j, "ok"))) {
        const char *e = json_string_value(json_object_get(j, "error"));
        *status = 400;
        snprintf(why, wlen, "%s", e && e[0] ? e : "the extension host refused the archive");
        json_decref(j);
        extpkg_stage_discard();                         /* what the host will not take is not kept */
        return NULL;
    }
    return j;
}

/* What installing a package of this tier takes beyond the login. A tier the
 * relay does not know takes the most. */
static const char *consent_of(const json_t *inspected)
{
    const char *tier = json_string_value(json_object_get(inspected, "tier"));
    if (tier && !strcmp(tier, "official"))
        return "login";
    if (tier && !strcmp(tier, "community"))
        return "typed";
    return "button";
}

char *extpkg_settings_json(const char *id, const char *patch, int *status, char *why, size_t wlen)
{
    char *out = NULL;
    const char *argv[] = { "settings", id, patch, NULL };

    *status = 500;
    why[0] = '\0';
    if (!extpkg_id_ok(id)) {
        *status = 400;
        snprintf(why, wlen, "id is a package id");
        return NULL;
    }
    /* The patch is one argument and is JSON; anything else the host
     * refuses, and it is the host that owns the schema. */
    if (patch && strlen(patch) > 4096) {
        *status = 400;
        snprintf(why, wlen, "that is more settings than a package has");
        return NULL;
    }
    /* A refusal exits 1 with the host's words on stdout: an exit status
     * that is not 0 is an answer, and only no answer at all is 502. */
    if (extpkg_cli(argv, &out) < 0 || !out) {
        free(out);
        *status = 502;
        snprintf(why, wlen, "the extension host did not answer");
        return NULL;
    }
    json_t *j = json_loads(out, 0, NULL);
    if (!json_is_object(j)) {
        json_decref(j);
        free(out);
        *status = 502;
        snprintf(why, wlen, "the extension host's answer is not a JSON object");
        return NULL;
    }
    if (!json_is_true(json_object_get(j, "ok"))) {
        const char *e = json_string_value(json_object_get(j, "error"));
        snprintf(why, wlen, "%s", e ? e : "the host refused");
        *status = 400;
        json_decref(j);
        free(out);
        return NULL;
    }
    json_decref(j);
    *status = 200;
    return out;
}

char *extpkg_ui_json(const char *id, int *status, char *why, size_t wlen)
{
    char *out = NULL;
    const char *argv[] = { "ui", id, NULL };

    *status = 500;
    why[0] = '\0';
    if (!extpkg_id_ok(id)) {
        *status = 400;
        snprintf(why, wlen, "id is a package id");
        return NULL;
    }
    /* A refusal exits 1 with its words, as for the settings. */
    if (extpkg_cli(argv, &out) < 0 || !out) {
        free(out);
        *status = 502;
        snprintf(why, wlen, "the extension host did not answer");
        return NULL;
    }
    json_error_t je;
    json_t *j = json_loads(out, 0, &je);
    if (!json_is_object(j)) {
        json_decref(j);
        free(out);
        *status = 502;
        snprintf(why, wlen, "the extension host's answer is not a JSON object");
        return NULL;
    }
    if (!json_is_true(json_object_get(j, "ok"))) {
        const char *e = json_string_value(json_object_get(j, "error"));
        snprintf(why, wlen, "%s", e ? e : "this package has no interface");
        *status = 404;
        json_decref(j);
        free(out);
        return NULL;
    }
    json_decref(j);
    *status = 200;
    return out;
}

int extpkg_dest(const char *id, const char *action, const char *dest, int *status, char *why, size_t wlen)
{
    char *out = NULL;
    *status = 400;
    why[0] = '\0';
    if (!extpkg_id_ok(id))
        return snprintf(why, wlen, "id is a package id"), -1;
    if (!action || (strcmp(action, "add") != 0 && strcmp(action, "remove") != 0))
        return snprintf(why, wlen, "action is add or remove"), -1;
    /* host:port in the form net.outbound takes; the host judges it
     * whole, and this keeps it from reading as one of its options. */
    if (!dest || !dest[0] || dest[0] == '-' || strlen(dest) > 95
        || strspn(dest, "abcdefghijklmnopqrstuvwxyz0123456789.-:[]") != strlen(dest))
        return snprintf(why, wlen, "a destination is host:port"), -1;
    const char *argv[] = { "dest", id, action, dest, NULL };
    if (extpkg_cli(argv, &out) < 0 || !out) {
        free(out);
        *status = 502;
        snprintf(why, wlen, "the extension host did not answer");
        return -1;
    }
    json_t *j = json_loads(out, 0, NULL);
    free(out);
    if (!json_is_object(j)) {
        json_decref(j);
        *status = 502;
        snprintf(why, wlen, "the extension host's answer is not a JSON object");
        return -1;
    }
    int ok = json_is_true(json_object_get(j, "ok"));
    if (!ok) {
        const char *e = json_string_value(json_object_get(j, "error"));
        snprintf(why, wlen, "%s", e ? e : "the host refused");
        *status = 409;
    }
    json_decref(j);
    if (!ok)
        return -1;
    *status = 200;
    return 0;
}

char *extpkg_call_json(const char *id, const char *method, const char *path, const char *body, int *status, char *why,
                       size_t wlen)
{
    char *out = NULL;
    const char *argv[] = { "call", id, method, path, body, NULL };

    *status = 400;
    why[0] = '\0';
    if (!extpkg_id_ok(id))
        return snprintf(why, wlen, "id is a package id"), NULL;
    if (!method || (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0))
        return snprintf(why, wlen, "a call is GET or POST"), NULL;
    /* The host holds the path and the body to their form; these two are
     * held here as well so that neither can read as one of its options. */
    if (!path || path[0] != '/' || strlen(path) > 200)
        return snprintf(why, wlen, "a call's path starts with '/'"), NULL;
    if (body && (body[0] != '{' || strlen(body) > EXTPKG_CALL_BODY_MAX))
        return snprintf(why, wlen, "a call's body is a JSON object of at most %d bytes", EXTPKG_CALL_BODY_MAX), NULL;
    if (body && strcmp(method, "GET") == 0)
        return snprintf(why, wlen, "a GET call has no body"), NULL;
    if (extpkg_cli(argv, &out) < 0 || !out) {
        free(out);
        *status = 502;
        snprintf(why, wlen, "the extension host did not answer");
        return NULL;
    }
    json_t *j = json_loads(out, 0, NULL);
    if (!json_is_object(j)) {
        json_decref(j);
        free(out);
        *status = 502;
        snprintf(why, wlen, "the extension host's answer is not a JSON object");
        return NULL;
    }
    if (!json_is_true(json_object_get(j, "ok"))) {
        const char *e = json_string_value(json_object_get(j, "error"));
        snprintf(why, wlen, "%s", e ? e : "the host refused the call");
        *status = 409;
        json_decref(j);
        free(out);
        return NULL;
    }
    json_decref(j);
    *status = 200;
    return out;
}

char *extpkg_inspect_json(int *status, char *why, size_t wlen)
{
    json_t *j = inspect(status, why, wlen);
    if (!j)
        return NULL;
    json_object_set_new(j, "consent", json_string(consent_of(j)));
    char *doc = json_dumps(j, JSON_COMPACT);
    json_decref(j);
    if (!doc) {
        *status = 500;
        snprintf(why, wlen, "out of memory");
    } else {
        *status = 200;
    }
    return doc;
}

static int grant_ok(const char *g, size_t n)
{
    if (n < 1 || n > 40)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!((g[i] >= 'a' && g[i] <= 'z') || g[i] == '_' || g[i] == '.'))
            return 0;
    return 1;
}

int extpkg_install(const char *grants, const char *phrase, int button_held, int *status, char *why, size_t wlen)
{
    static char held[EXTPKG_GRANTS_MAX][44];
    const char *argv[8 + 2 * EXTPKG_GRANTS_MAX];
    int n = 0, ng = 0;

    /* The grants, in the form of capability names, before anything runs. */
    *status = 400;
    for (const char *p = grants; p && *p;) {
        size_t len = strcspn(p, ",");
        if (ng >= EXTPKG_GRANTS_MAX || !grant_ok(p, len)) {
            snprintf(why, wlen, "grants is a comma-separated list of at most %d capability names", EXTPKG_GRANTS_MAX);
            return -1;
        }
        snprintf(held[ng++], sizeof(held[0]), "%.*s", (int)len, p);
        p += len + (p[len] == ',');
    }

    /* What the staged file is, from the host and not from the request. */
    json_t *j = inspect(status, why, wlen);
    if (!j)
        return -1;
    const char *consent = consent_of(j);
    const char *flag = NULL;
    if (!strcmp(consent, "typed")) {
        if (!phrase || strcmp(phrase, EXTPKG_PHRASE) != 0) {
            int endorsed = json_is_true(json_object_get(j, "endorsed"));
            json_decref(j);
            *status = 400;
            snprintf(why, wlen, "%s, not by OpenGlow: type " EXTPKG_PHRASE " to install it",
                     endorsed ? "this package is signed by its author's key, which OpenGlow's catalog names for it"
                              : "this package is signed by a key you added");
            return -1;
        }
        flag = "--consent-community";
    } else if (!strcmp(consent, "button")) {
        if (!button_held) {
            json_decref(j);
            *status = 409;
            snprintf(why, wlen, "nobody the machine trusts signed this package: hold the machine's button while you install it");
            return -1;
        }
        flag = "--consent-unverified";
    }
    json_decref(j);

    argv[n++] = "install";
    argv[n++] = extpkg_stage_path();
    for (int i = 0; i < ng; i++) {
        argv[n++] = "--grant";
        argv[n++] = held[i];
    }
    if (flag)
        argv[n++] = flag;
    argv[n] = NULL;
    char *text = NULL;
    int rc = extpkg_cli(argv, &text);
    j = text ? json_loads(text, 0, NULL) : NULL;
    free(text);
    if (rc < 0 || !json_is_object(j)) {
        json_decref(j);
        *status = 502;
        snprintf(why, wlen, "the extension host's command line does not answer");
        return -1;
    }
    if (rc != 0 || !json_is_true(json_object_get(j, "ok"))) {
        const char *e = json_string_value(json_object_get(j, "error"));
        *status = 409;
        snprintf(why, wlen, "%s", e && e[0] ? e : "the extension host refused");
        json_decref(j);
        return -1;
    }
    json_decref(j);
    extpkg_stage_discard();
    *status = 200;
    return 0;
}

/* ---- the owner's keys ------------------------------------------------------------ */

/* The host reads the key from its standard input, so nothing of it is ever
 * an argument. */
static int key_cli(const char *const argv_in[], const char *stdin_text, size_t slen, char **out)
{
    char tmp[] = "/tmp/forgectrl-key.XXXXXX";
    int fd = mkstemp(tmp), rc = -1;
    if (fd < 0)
        return -1;
    fchmod(fd, 0600);
    if (stdin_text && write(fd, stdin_text, slen) == (ssize_t)slen) {
        const char *argv[8];
        int n = 0;
        for (int i = 0; argv_in[i] && n < 7; i++)
            argv[n++] = argv_in[i];
        argv[n++] = tmp;
        argv[n] = NULL;
        rc = extpkg_cli(argv, out);
    }
    close(fd);
    unlink(tmp);
    return rc;
}

static int key_answer(int rc, char *text, int *status, char *why, size_t wlen)
{
    json_t *j = text ? json_loads(text, 0, NULL) : NULL;
    free(text);
    if (rc < 0 || !json_is_object(j)) {
        json_decref(j);
        *status = 502;
        snprintf(why, wlen, "the extension host's command line does not answer");
        return -1;
    }
    if (rc != 0 || !json_is_true(json_object_get(j, "ok"))) {
        const char *e = json_string_value(json_object_get(j, "error"));
        *status = 409;
        snprintf(why, wlen, "%s", e && e[0] ? e : "the extension host refused");
        json_decref(j);
        return -1;
    }
    json_decref(j);
    *status = 200;
    return 0;
}

/* A key's name, as the host takes it, checked before anything runs. */
static int key_name_ok(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (n < 1 || n > 48 || name[0] == '.' || name[0] == '-')
        return 0;
    return strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == n;
}

int extpkg_key_add(const char *name, const char *key, size_t klen, int button_held, int *status, char *why, size_t wlen)
{
    *status = 400;
    if (!key_name_ok(name)) {
        snprintf(why, wlen, "a key's name is letters, digits, dash, underscore, and dot, at most 48 bytes");
        return -1;
    }
    if (!key || klen == 0 || klen > EXTPKG_KEY_MAX) {
        snprintf(why, wlen, "key is the public key fwup wrote, at most %d bytes", EXTPKG_KEY_MAX);
        return -1;
    }
    if (!button_held) {
        *status = 409;
        snprintf(why, wlen, "a key you add is what this machine will trust: hold the machine's button while you add it");
        return -1;
    }
    const char *argv[] = { "key-add", name, NULL };
    char *text = NULL;
    int rc = key_cli(argv, key, klen, &text);       /* filled before it is read: C orders neither */
    return key_answer(rc, text, status, why, wlen);
}

int extpkg_key_remove(const char *name, int *status, char *why, size_t wlen)
{
    *status = 400;
    if (!key_name_ok(name)) {
        snprintf(why, wlen, "a key's name is letters, digits, dash, underscore, and dot, at most 48 bytes");
        return -1;
    }
    const char *argv[] = { "key-remove", name, NULL };
    char *text = NULL;
    int rc = extpkg_cli(argv, &text);
    return key_answer(rc, text, status, why, wlen);
}

/* ---- the M-codes packages answer ------------------------------------------------- */

void extpkg_mcode_table(char *out, size_t len)
{
    const char *sf = getenv("FORGECTRL_EXT_STATUS");
    json_t *host = json_load_file(sf && sf[0] ? sf : EXTPKG_STATUS_FILE, 0, NULL), *e;
    int bits[32] = { 0 }, any = 0;
    size_t i;
    snprintf(out, len, "-");
    if (!json_is_object(host) || !host_running(json_integer_value(json_object_get(host, "pid")))) {
        json_decref(host);
        return;
    }
    json_array_foreach(json_object_get(host, "mcodes"), i, e) {
        json_int_t c = json_integer_value(json_object_get(e, "code"));
        if (c >= 160 && c <= 179 && json_is_integer(json_object_get(e, "code")))
            bits[c - 160] = any = 1;
    }
    json_decref(host);
    if (!any)
        return;
    size_t n = 0;
    out[0] = '\0';
    for (int c = 0; c < 20 && n < len; c++)
        if (bits[c])
            n += (size_t)snprintf(out + n, len - n, "%s%d", n ? "," : "", 160 + c);
}

char *extpkg_mcode_json(int code, const char *words, char *why, size_t wlen)
{
    char num[8];
    if (code < 160 || code > 179 || !words || words[0] != '{' || strlen(words) > 200) {
        snprintf(why, wlen, "an M-code packages answer is one of M160 to M179, with its words");
        return NULL;
    }
    snprintf(num, sizeof(num), "%d", code);
    const char *argv[] = { "mcode", num, words, NULL };
    char *out = NULL;
    int rc = spawn(bin(), argv, &out, EXTPKG_MCODE_TIMEOUT_S, 0);
    json_t *j = out ? json_loads(out, 0, NULL) : NULL;
    if (rc < 0 || !json_is_object(j)) {
        json_decref(j);
        free(out);
        snprintf(why, wlen, "the extension host did not answer");
        return NULL;
    }
    if (!json_is_true(json_object_get(j, "ok"))) {
        const char *e = json_string_value(json_object_get(j, "error"));
        snprintf(why, wlen, "%s", e && e[0] ? e : "the extension host refused");
        json_decref(j);
        free(out);
        return NULL;
    }
    json_decref(j);
    return out;
}

/* ---- the catalog ----------------------------------------------------------------- */

static const char *curl_bin(void)
{
    const char *v = getenv("FORGECTRL_CURL");           /* a test's stand-in */
    return v && v[0] ? v : EXTPKG_CURL_DEFAULT;
}

static const char *index_url(void)
{
    const char *v = getenv("FORGECTRL_EXT_INDEX_URL");  /* a test's */
    return v && v[0] ? v : EXTPKG_INDEX_URL;
}

/* The directory the staged package goes in, made when it is not there. */
static void stage_dir(void)
{
    char dir[300];
    snprintf(dir, sizeof(dir), "%s", extpkg_stage_path());
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        (void)mkdir(dir, 0700);
    }
}

/* The index is fetched beside the staged package, and never kept there. */
static void index_stage(char *out, size_t len)
{
    const char *stage = extpkg_stage_path(), *slash = strrchr(stage, '/');
    if (slash)
        snprintf(out, len, "%.*s/ext-index.ffi", (int)(slash - stage), stage);
    else
        snprintf(out, len, "ext-index.ffi");
}

/* The host's answer to one command, a JSON object with "ok" true; NULL with
 * the status (refused for the host's refusal, 502 when it gives no answer)
 * and the words. */
static json_t *host_json(const char *const argv[], int refused, int *status, char *why, size_t wlen)
{
    char *text = NULL;
    int rc = extpkg_cli(argv, &text);
    json_t *j = text ? json_loads(text, 0, NULL) : NULL;
    free(text);
    if (rc < 0 || !json_is_object(j)) {
        json_decref(j);
        *status = 502;
        snprintf(why, wlen, "the extension host's command line does not answer");
        return NULL;
    }
    if (rc != 0 || !json_is_true(json_object_get(j, "ok"))) {
        const char *e = json_string_value(json_object_get(j, "error"));
        *status = refused;
        snprintf(why, wlen, "%s", e && e[0] ? e : "the extension host refused");
        json_decref(j);
        return NULL;
    }
    return j;
}

/* An https:// address into a file, with curl and no shell: https alone,
 * through redirects too, and bounded in bytes and in time. 0, or -1 with
 * curl's words and the file removed. */
static int fetch(const char *url, const char *path, unsigned long max_bytes, int max_s, char *why, size_t wlen)
{
    char size[24], secs[16];
    if (!url || strncmp(url, "https://", 8) != 0) {
        snprintf(why, wlen, "the address is not https://");
        return -1;
    }
    snprintf(size, sizeof(size), "%lu", max_bytes);
    snprintf(secs, sizeof(secs), "%d", max_s);
    const char *argv[] = { "-fsS", "-L", "--proto", "=https", "--proto-redir", "=https", "--max-redirs", "5",
                           "--max-time", secs, "--max-filesize", size, "-o", path, url, NULL };
    char *out = NULL;
    unlink(path);
    int rc = spawn(curl_bin(), argv, &out, max_s + 5, 1);
    if (rc != 0) {
        const char *t = out ? out : "";
        size_t n = strcspn(t, "\n");
        if (rc < 0)
            snprintf(why, wlen, "no answer in %d s", max_s);
        else
            snprintf(why, wlen, "%.*s", (int)(n < 200 ? n : 200), n ? t : "curl failed");
        free(out);
        unlink(path);
        return -1;
    }
    free(out);
    return 0;
}

static int file_sha256(const char *path, char hex[2 * SHA256_LEN + 1], long long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    sha256_ctx c;
    unsigned char buf[16384], d[SHA256_LEN];
    size_t n;
    *size = 0;
    sha256_init(&c);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        sha256_update(&c, buf, n);
        *size += (long long)n;
    }
    int bad = ferror(f);
    fclose(f);
    sha256_final(&c, d);
    sha256_hex(d, SHA256_LEN, hex);
    return bad ? -1 : 0;
}

char *extpkg_catalog_json(int *status, char *why, size_t wlen)
{
    const char *argv[] = { "index", NULL };
    json_t *j = host_json(argv, 502, status, why, wlen);
    if (!j)
        return NULL;
    json_object_del(j, "ok");
    json_object_set_new(j, "url", json_string(index_url()));
    char *doc = json_dumps(j, JSON_COMPACT);
    json_decref(j);
    if (!doc) {
        *status = 500;
        snprintf(why, wlen, "out of memory");
        return NULL;
    }
    *status = 200;
    return doc;
}

int extpkg_catalog_refresh(int *npkgs, char *version, size_t vlen, int *status, char *why, size_t wlen)
{
    char path[320], words[240];
    *npkgs = 0;
    if (version && vlen)
        version[0] = '\0';
    index_stage(path, sizeof(path));
    stage_dir();
    if (fetch(index_url(), path, EXTPKG_INDEX_FETCH_MAX, EXTPKG_INDEX_FETCH_S, words, sizeof(words)) != 0) {
        *status = 502;
        snprintf(why, wlen, "the catalog could not be fetched: %s", words);
        return -1;
    }
    const char *argv[] = { "index-verify", path, NULL };
    json_t *j = host_json(argv, 409, status, why, wlen);
    unlink(path);
    if (!j)
        return -1;
    *npkgs = (int)json_integer_value(json_object_get(j, "packages"));
    if (version && vlen)
        snprintf(version, vlen, "%s", json_string_value(json_object_get(j, "version")) ?: "");
    json_decref(j);
    *status = 200;
    return 0;
}

char *extpkg_catalog_get(const char *id, int *status, char *why, size_t wlen)
{
    char url[1100], sha[2 * SHA256_LEN + 1], got[2 * SHA256_LEN + 1], words[240];
    long long size = 0, have = 0;
    *status = 400;
    if (!extpkg_id_ok(id)) {
        snprintf(why, wlen, "id is a package id");
        return NULL;
    }

    /* Where it is and what it is, from the index the host verified, and
     * nothing of it from the request. */
    const char *argv[] = { "index", NULL };
    json_t *j = host_json(argv, 502, status, why, wlen), *idx = json_object_get(j, "index"), *entry = NULL, *e;
    if (!j)
        return NULL;
    if (!json_is_object(idx)) {
        json_decref(j);
        *status = 409;
        snprintf(why, wlen, "no catalog is kept here: fetch it first");
        return NULL;
    }
    size_t i;
    json_array_foreach(json_object_get(idx, "packages"), i, e) {
        const char *eid = json_string_value(json_object_get(e, "id"));
        if (eid && !strcmp(eid, id))
            entry = e;
    }
    if (!entry) {
        json_decref(j);
        *status = 404;
        snprintf(why, wlen, "%s is not in the catalog", id);
        return NULL;
    }
    const char *u = json_string_value(json_object_get(entry, "url")), *h = json_string_value(json_object_get(entry, "sha256"));
    size = json_integer_value(json_object_get(entry, "size"));
    int form = u && strlen(u) < sizeof(url) && !strncmp(u, "https://", 8) && h && strlen(h) == 2 * SHA256_LEN
               && size >= 1 && size <= (long long)EXTPKG_UPLOAD_MAX;
    if (form) {
        snprintf(url, sizeof(url), "%s", u);
        snprintf(sha, sizeof(sha), "%s", h);
    }
    json_decref(j);
    if (!form) {
        *status = 502;
        snprintf(why, wlen, "the catalog's entry for %s is out of form", id);
        return NULL;
    }

    /* The bytes, held to the size and the SHA-256 the index names before
     * the host reads a byte of them. */
    stage_dir();
    if (fetch(url, extpkg_stage_path(), (unsigned long)size, EXTPKG_PKG_FETCH_S, words, sizeof(words)) != 0) {
        *status = 502;
        snprintf(why, wlen, "%s could not be fetched: %s", id, words);
        return NULL;
    }
    if (file_sha256(extpkg_stage_path(), got, &have) != 0 || have != size || strcmp(got, sha) != 0) {
        extpkg_stage_discard();
        *status = 409;
        snprintf(why, wlen, "what was fetched for %s is not the archive the catalog names: its size or its SHA-256 differs",
                 id);
        return NULL;
    }

    /* And from here on it is an upload: the host says what it is. */
    json_t *in = inspect(status, why, wlen);
    if (!in)
        return NULL;
    const char *pid = json_string_value(json_object_get(json_object_get(in, "package"), "id"));
    if (!pid || strcmp(pid, id) != 0) {
        json_decref(in);
        extpkg_stage_discard();
        *status = 409;
        snprintf(why, wlen, "the archive the catalog names for %s is another package", id);
        return NULL;
    }
    json_object_set_new(in, "consent", json_string(consent_of(in)));
    json_object_set_new(in, "catalog", json_true());
    char *doc = json_dumps(in, JSON_COMPACT);
    json_decref(in);
    if (!doc) {
        *status = 500;
        snprintf(why, wlen, "out of memory");
        return NULL;
    }
    *status = 200;
    return doc;
}
