/*
 * logs_test.c - host test: the log settings, the list, and the tail
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The four paths are the machine's while their variables are unset (and
 * nothing is made at them); the level, port, protocol, and server
 * validators; GET /logs's configured and effective levels from a
 * settings file and an effective-levels file, with a reboot pending
 * exactly while they differ; and GET /logs/tail on a planted log, an
 * unknown logger refused. Everything else is a scratch tree under /tmp.
 */
#include "../src/logs.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* What logs.c and auth.c ask of the rest of the daemon. */
unsigned long fuse_serial(void) { return 0; }
void machine_id(char *buf, size_t len) { if (len) buf[0] = '\0'; }
int fuse_password(char *buf, size_t len) { if (len) buf[0] = '\0'; return -1; }
int users_exist(void) { return 0; }
int users_name(char *buf, size_t len) { if (len) buf[0] = '\0'; return -1; }
int session_valid(const char *id) { (void)id; return 0; }
int session_from_cookie(const char *cookie, char *id, size_t len) { (void)cookie; (void)id; (void)len; return 0; }

static int failures;

#define CHECK(cond, name) do { \
    if (cond) printf("ok   %s\n", name); \
    else { printf("FAIL %s\n", name); failures++; } } while (0)

static char dir[] = "/tmp/logs-test-XXXXXX";

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

/* The effective-levels file as a render writes it, with the system
 * logger's remote level given. */
static void effective(const char *path, const char *system_remote)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    for (size_t i = 0; i < logs_count; i++)
        fprintf(f, "log_%s_disk=info\nlog_%s_remote=%s\n", logs_names[i], logs_names[i],
                strcmp(logs_names[i], "system") ? "off" : system_remote);
    fprintf(f, "syslog_server=\nsyslog_port=514\nsyslog_proto=udp\n");
    fclose(f);
}

/* The system logger's entry and pending_reboot from GET /logs. */
static int listed(char *remote, char *eff_remote, size_t len, int *pending)
{
    char *text = logs_list_json();
    json_t *o = text ? json_loads(text, 0, NULL) : NULL;
    free(text);
    if (!o)
        return -1;
    size_t i;
    json_t *l;
    int found = -1;
    json_array_foreach(json_object_get(o, "loggers"), i, l)
        if (!strcmp(json_string_value(json_object_get(l, "name")) ?: "", "system")) {
            snprintf(remote, len, "%s", json_string_value(json_object_get(l, "remote")) ?: "");
            snprintf(eff_remote, len, "%s", json_string_value(json_object_get(l, "effective_remote")) ?: "");
            found = 0;
        }
    *pending = json_is_true(json_object_get(o, "pending_reboot"));
    json_decref(o);
    return found;
}

int main(void)
{
    /* 1. Unset, the paths are the machine's; nothing is made there. */
    unsetenv("GF_LOGS_ROOT");
    unsetenv("GF_LOGS_RSYSLOG");
    unsetenv("GF_LOGS_EFFECTIVE");
    unsetenv("GF_LOGS_STAGE");
    CHECK(!strcmp(logs_root(), "/data/log/forgefirm"), "unset, the log tree is /data/log/forgefirm");
    CHECK(!strcmp(logs_rsyslog(), "/data/forgefirm/rsyslog-forgefirm.conf"),
          "unset, the rules are /data/forgefirm/rsyslog-forgefirm.conf");
    CHECK(!strcmp(logs_effective(), "/var/run/forgefirm-loglevels"),
          "unset, the levels in force are /var/run/forgefirm-loglevels");
    CHECK(!strcmp(logs_stage(), "/data/forgefirm/tmp"), "unset, the export stages in /data/forgefirm/tmp");

    if (!mkdtemp(dir)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char conf[256], eff[256], root[256], d[300], log[400];
    snprintf(conf, sizeof(conf), "%s/forgefirm.conf", dir);
    snprintf(eff, sizeof(eff), "%s/loglevels", dir);
    snprintf(root, sizeof(root), "%s/log", dir);
    setenv("FORGECTRL_CONF", conf, 1);
    setenv("GF_LOGS_EFFECTIVE", eff, 1);
    setenv("GF_LOGS_ROOT", root, 1);
    setenv("FFLOG_SOCK", "/nonexistent/log", 1);

    /* 2. The validators. */
    static const char *const levels[] = { "off", "error", "warning", "notice", "info", "debug" };
    int all = 1;
    for (size_t i = 0; i < sizeof(levels) / sizeof(*levels); i++)
        all &= logs_valid_level(levels[i]);
    CHECK(all, "the six levels are valid");
    CHECK(!logs_valid_level("verbose") && !logs_valid_level("7"), "verbose and 7 are not levels");
    CHECK(!logs_valid_port("70000"), "port 70000 is refused");
    CHECK(logs_valid_port("1") && logs_valid_port("514") && logs_valid_port("65535"),
          "ports 1, 514, and 65535 are taken");
    CHECK(logs_valid_proto("udp") && logs_valid_proto("tcp") && !logs_valid_proto("tls"),
          "udp and tcp are protocols, tls is not");
    CHECK(!logs_valid_server("bad host name"), "a server name with spaces is refused");

    /* 3. Configured against effective: the system logger's remote level
     * set to warning, the levels in force from a boot that had it off. */
    put(conf, "log_system_remote = warning\n");
    effective(eff, "off");
    char remote[32], eff_remote[32];
    int pending = 0;
    CHECK(listed(remote, eff_remote, sizeof(remote), &pending) == 0 &&
          !strcmp(remote, "warning") && !strcmp(eff_remote, "off") && pending,
          "a remote level changed since boot: configured warning, effective off, reboot pending");
    effective(eff, "warning");
    CHECK(listed(remote, eff_remote, sizeof(remote), &pending) == 0 &&
          !strcmp(remote, "warning") && !strcmp(eff_remote, "warning") && !pending,
          "the levels in force match: no reboot pending");

    /* 4. The tail of a planted log; an unknown logger is no logger. */
    mkdir(root, 0755);
    snprintf(d, sizeof(d), "%s/forgectrl", root);
    mkdir(d, 0755);
    snprintf(log, sizeof(log), "%s/forgectrl.log", d);
    put(log, "2026-10-14T09:00:00 forgectrl[1] INFO first planted line\n"
             "2026-10-14T09:00:01 forgectrl[1] INFO second planted line\n");
    char *t = logs_tail_json("forgectrl", 200, -1);
    json_t *o = t ? json_loads(t, 0, NULL) : NULL;
    const char *text = json_string_value(json_object_get(o, "text"));
    CHECK(text && strstr(text, "second planted line"), "the tail carries a planted line");
    json_decref(o);
    free(t);
    t = logs_tail_json("no-such-logger", 200, -1);
    CHECK(t == NULL, "an unknown logger is refused");
    free(t);

    unlink(log);
    rmdir(d);
    rmdir(root);
    unlink(eff);
    unlink(conf);
    rmdir(dir);
    printf(failures ? "logs_test: %d FAILED\n" : "logs_test: all ok\n", failures);
    return failures ? 1 : 0;
}
