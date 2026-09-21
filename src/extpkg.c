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

int extpkg_cli(const char *const argv_in[], char **out)
{
    const char *argv[32];
    int n = 0, pfd[2];
    *out = NULL;
    argv[n++] = bin();
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
    double end = mono() + EXTPKG_TIMEOUT_S;
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
            json_decref(j);
            *status = 400;
            snprintf(why, wlen, "this package is signed by a key you added, not by OpenGlow: type " EXTPKG_PHRASE " to install it");
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
