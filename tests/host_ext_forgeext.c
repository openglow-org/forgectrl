/*
 * host_ext_forgeext.c - the extension host's stand-in for host_ext.sh
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * host_ext.sh compiles this with RAN_FILE and KEY_DIR defined and points
 * FORGECTRL_FORGEEXT at the binary. Run as "hold" alone, it is the host
 * that is alive: it writes FORGECTRL_EXT_STATUS with its own pid and one
 * running service, and sleeps until it is killed, so the daemon's check of
 * /proc/<pid>/exe against FORGECTRL_FORGEEXT finds this binary. Run as any
 * other command, it appends its arguments to RAN_FILE as one line, answers
 * on standard output as the host's command line does for one package,
 * org.example.hostext, and exits.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ID "org.example.hostext"

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = malloc(1 << 20);
    size_t n = buf ? fread(buf, 1, (1 << 20) - 1, f) : 0;
    fclose(f);
    if (buf)
        buf[n] = '\0';
    return buf;
}

static int answer(const char *json, int rc)
{
    puts(json);
    return rc;
}

static int hold(void)
{
    const char *path = getenv("FORGECTRL_EXT_STATUS");
    char tmp[4096];
    if (!path || snprintf(tmp, sizeof(tmp), "%s.new", path) >= (int)sizeof(tmp))
        return 2;
    FILE *f = fopen(tmp, "w");
    if (!f)
        return 2;
    fprintf(f, "{\"pid\": %d, \"enabled\": true, \"services\": [{\"id\": \"" ID "\", \"state\": \"running\"}]}\n",
            (int)getpid());
    if (fclose(f) != 0 || rename(tmp, path) != 0)
        return 2;
    for (;;)
        pause();
}

static int wizard(const char *verb)
{
    if (!strcmp(verb, "state")) {
        char *ran = slurp(RAN_FILE);
        int done = ran && strstr(ran, "wizard " ID " answer ");
        free(ran);
        return answer(done ? "{\"ok\": true, \"id\": \"" ID "\", \"status\": 200, "
                             "\"body\": {\"title\": \"forgetest check\", \"done\": true}}"
                           : "{\"ok\": true, \"id\": \"" ID "\", \"status\": 200, "
                             "\"body\": {\"title\": \"forgetest check\", \"done\": false}}", 0);
    }
    if (!strcmp(verb, "start"))
        return answer("{\"ok\": true, \"id\": \"" ID "\", \"status\": 200, \"body\": {\"log\": [\"the check begins\"], "
                      "\"phase\": \"ready\", \"progress\": 50, \"prompt\": {\"kind\": \"confirm\", \"id\": \"ready\", "
                      "\"text\": \"Is the machine ready?\"}}}", 0);
    if (!strcmp(verb, "answer"))
        return answer("{\"ok\": true, \"id\": \"" ID "\", \"status\": 200, \"body\": {\"log\": [\"answered\"], "
                      "\"result\": {\"ok\": true, \"summary\": \"ready\"}}}", 0);
    if (!strcmp(verb, "abort"))
        return answer("{\"ok\": true, \"id\": \"" ID "\", \"status\": 200, \"body\": {}}", 0);
    return answer("{\"ok\": false, \"error\": \"the stand-in host has no such step\"}", 1);
}

static int key_add(const char *name, const char *file)
{
    char path[4096];
    char *key = slurp(file);
    snprintf(path, sizeof(path), "%s/%s.pub", KEY_DIR, name);
    FILE *f = key ? fopen(path, "w") : NULL;
    if (f) {
        fputs(key, f);
        fclose(f);
    }
    free(key);
    return f ? answer("{\"ok\": true}", 0) : answer("{\"ok\": false, \"error\": \"the stand-in host cannot copy the key\"}", 1);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    if (!strcmp(argv[1], "hold") && argc == 2)    /* the host's own hold <id> <how> names a package */
        return hold();
    int fd = open(RAN_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        char line[8192];
        size_t n = 0;
        for (int i = 1; i < argc && n < sizeof(line) - 1; i++)
            n += (size_t)snprintf(line + n, sizeof(line) - n, "%s%s", argv[i], i + 1 < argc ? " " : "\n");
        if (n > sizeof(line) - 1)
            n = sizeof(line) - 1;
        if (write(fd, line, n) < 0)
            return 2;
        close(fd);
    }
    const char *cmd = argv[1], *id = argc > 2 ? argv[2] : "";
    if (!strcmp(cmd, "inspect")) {
        char *bytes = argc > 2 ? slurp(argv[2]) : NULL;
        int refused = bytes && strstr(bytes, "refuse");
        free(bytes);
        if (refused)
            return answer("{\"ok\": false, \"error\": \"the stand-in host will not take these bytes\"}", 1);
        return answer("{\"ok\": true, \"tier\": \"unverified\", \"needs_grant\": [], "
                      "\"package\": {\"id\": \"" ID "\", \"version\": \"1.0.0\"}}", 0);
    }
    if (!strcmp(cmd, "key-add") && argc > 3)
        return key_add(argv[2], argv[3]);
    if (!strcmp(cmd, "keys"))
        return answer("{\"ok\": true, \"keys\": []}", 0);
    if (!strcmp(cmd, "list"))
        return answer("{\"ok\": true, \"packages\": [{\"id\": \"" ID "\", \"tier\": \"community\", \"enabled\": true, "
                      "\"hold\": \"advisory\", \"grants\": [\"hold\"], \"effective\": [\"wizard\"], "
                      "\"package\": {\"name\": \"forgetest check\", \"capabilities\": [\"wizard\"]}}]}", 0);
    if (!strcmp(cmd, "enable") || !strcmp(cmd, "disable") || !strcmp(cmd, "remove") || !strcmp(cmd, "hold") ||
        !strcmp(cmd, "wizard")) {
        if (strcmp(id, ID) != 0)
            return answer("{\"ok\": false, \"error\": \"that package is not installed\"}", 1);
        return strcmp(cmd, "wizard") ? answer("{\"ok\": true}", 0) : wizard(argc > 3 ? argv[3] : "");
    }
    return answer("{\"ok\": false, \"error\": \"the stand-in host answers no such command\"}", 1);
}
