/*
 * logs_export_test.c - host test: the sanitized log export
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * In a scratch data directory and log tree: a panel token and a camera
 * key on file, loaded by auth_init and camkey_init, and an installer log
 * whose line carries both, a MAC address, and an IP address. A sanitized
 * export is staged and read as the route streams it, under the
 * logs.export lease, which the end drops. The archive holds the README,
 * the installer's directory with the addresses gone, and the extension
 * list as extpkg_status_json writes it; the token and the camera key are
 * nowhere in it, nor the account's name, which a line written by a
 * release that logged it carries.
 */
#define _GNU_SOURCE
#include "../src/auth.h"
#include "../src/camkey.h"
#include "../src/extpkg.h"
#include "../src/lease.h"
#include "../src/logs.h"

#include <ftw.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

/* What logs.c and auth.c ask of the rest of the daemon. */
unsigned long fuse_serial(void) { return 0; }
void machine_id(char *buf, size_t len) { if (len) buf[0] = '\0'; }
int fuse_password(char *buf, size_t len) { if (len) buf[0] = '\0'; return -1; }
int users_exist(void) { return 0; }
#define ACCOUNT "kerfmaker"
int users_name(char *buf, size_t len) { snprintf(buf, len, "%s", ACCOUNT); return 0; }
int session_valid(const char *id) { (void)id; return 0; }
int session_from_cookie(const char *cookie, char *id, size_t len) { (void)cookie; (void)id; (void)len; return 0; }

static int failures;

#define CHECK(cond, name) do { \
    if (cond) printf("ok   %s\n", name); \
    else { printf("FAIL %s\n", name); failures++; } } while (0)

#define TOKEN  "0123456789abcdef0123456789abcdef"
#define CAMKEY "fedcba9876543210fedcba9876543210"
#define MAC    "02:42:ac:11:00:02"
#define IP     "192.168.7.42"

static char dir[] = "/tmp/logs-export-test-XXXXXX";

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static int rm_cb(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)ftw;
    return (flag == FTW_DP || flag == FTW_D) ? rmdir(path) : unlink(path);
}

/* One member of the tar stream. */
struct member {
    char name[260];                     /* a ustar prefix, '/', and a name */
    char type;
    const char *data;
    size_t size;
};

static size_t untar(const char *buf, size_t len, struct member *m, size_t max)
{
    size_t n = 0;
    for (size_t off = 0; off + 512 <= len && n < max;) {
        const char *h = buf + off;
        if (!h[0])
            break;
        char name[101], prefix[156], sz[13];
        memcpy(name, h, 100);
        name[100] = '\0';
        memcpy(prefix, h + 345, 155);
        prefix[155] = '\0';
        memcpy(sz, h + 124, 12);
        sz[12] = '\0';
        size_t size = strtoul(sz, NULL, 8);
        if (!memcmp(h + 257, "ustar", 5) && prefix[0])
            snprintf(m[n].name, sizeof(m[n].name), "%s/%s", prefix, name);
        else
            snprintf(m[n].name, sizeof(m[n].name), "%s", name);
        m[n].type = h[156];
        m[n].data = h + 512;
        m[n].size = size;
        n++;
        off += 512 + (size + 511) / 512 * 512;
    }
    return n;
}

static const struct member *find(const struct member *m, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++)
        if (!strcmp(m[i].name, name))
            return &m[i];
    return NULL;
}

static int has(const char *data, size_t len, const char *s)
{
    return memmem(data, len, s, strlen(s)) != NULL;
}

int main(void)
{
    if (!mkdtemp(dir)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char data[256], root[256], p[512], gzpath[256];
    snprintf(data, sizeof(data), "%s/data", dir);
    snprintf(root, sizeof(root), "%s/log", dir);
    mkdir(data, 0700);
    mkdir(root, 0755);
    setenv("FORGECTRL_DATA_DIR", data, 1);
    snprintf(p, sizeof(p), "%s/forgefirm.conf", data);
    setenv("FORGECTRL_CONF", p, 1);
    setenv("GF_LOGS_ROOT", root, 1);
    snprintf(p, sizeof(p), "%s/stage", dir);
    setenv("GF_LOGS_STAGE", p, 1);
    snprintf(p, sizeof(p), "%s/no-rsyslog.conf", dir);
    setenv("GF_LOGS_RSYSLOG", p, 1);
    snprintf(p, sizeof(p), "%s/no-loglevels", dir);
    setenv("GF_LOGS_EFFECTIVE", p, 1);
    snprintf(p, sizeof(p), "%s/no-ext-status.json", dir);
    setenv("FORGECTRL_EXT_STATUS", p, 1);
    snprintf(p, sizeof(p), "%s/no-forgeext", dir);
    setenv("FORGECTRL_FORGEEXT", p, 1);
    setenv("FFLOG_SOCK", "/nonexistent/log", 1);

    /* The two secrets on file, at the lengths their files hold. */
    snprintf(p, sizeof(p), "%s/panel.token", data);
    put(p, TOKEN "\n");
    snprintf(p, sizeof(p), "%s/camera.key", data);
    put(p, CAMKEY "\n");
    auth_init();
    camkey_init();
    CHECK(!strcmp(auth_token(), TOKEN) && !strcmp(camkey_get(), CAMKEY),
          "auth_init and camkey_init load the token and the camera key from the data directory");

    /* The installer's log, a line carrying all four. */
    snprintf(p, sizeof(p), "%s/install", root);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/install/install.log", root);
    put(p, "install: probe token=" TOKEN " key=" CAMKEY " mac " MAC " ip " IP "\n"
           "users: account '" ACCOUNT "' created\n");

    char err[160] = "", owner[64] = "";
    logs_export_t *e = logs_export_begin(1, NULL, NULL, err, sizeof(err));
    CHECK(e != NULL, "a sanitized export begins");
    if (!e) {
        printf("  %s\n", err);
        nftw(dir, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
        return 1;
    }
    CHECK(lease_holder(owner, sizeof(owner)) && !strcmp(owner, "logs.export"),
          "the export holds the logs.export lease");
    snprintf(gzpath, sizeof(gzpath), "%s/export.tar.gz", dir);
    FILE *out = fopen(gzpath, "wb");
    char chunk[16384];
    ssize_t r;
    while ((r = logs_export_read(e, chunk, sizeof(chunk))) > 0)
        if (out)
            fwrite(chunk, 1, (size_t)r, out);
    if (out)
        fclose(out);
    logs_export_end(e);
    CHECK(!lease_holder(owner, sizeof(owner)), "the end drops the lease");

    /* The archive, as a reader unpacks it. */
    size_t cap = 1 << 20, len = 0;
    char *tar = malloc(cap);
    gzFile gz = gzopen(gzpath, "rb");
    int got;
    while (tar && gz && (got = gzread(gz, tar + len, (unsigned)(cap - len))) > 0) {
        len += (size_t)got;
        if (len == cap) {
            char *nt = realloc(tar, cap *= 2);
            if (!nt)
                break;
            tar = nt;
        }
    }
    if (gz)
        gzclose(gz);
    static struct member m[512];
    size_t n = tar ? untar(tar, len, m, 512) : 0;
    CHECK(n > 0, "the export is a gzip tar archive");

    const struct member *readme = find(m, n, "forgefirm-logs/README.txt");
    CHECK(readme && readme->type == '0', "forgefirm-logs/README.txt is there");
    const struct member *dirent = find(m, n, "forgefirm-logs/logs/install/");
    const struct member *install = find(m, n, "forgefirm-logs/logs/install/install.log");
    CHECK(dirent && dirent->type == '5' && install && has(install->data, install->size, "probe"),
          "forgefirm-logs/logs/install/ carries the installer's line");
    CHECK(install && !has(install->data, install->size, MAC) && !has(install->data, install->size, IP),
          "the line's MAC and IP addresses are gone");

    const struct member *ext = find(m, n, "forgefirm-logs/system/extensions.json");
    char *want_text = extpkg_status_json(0);
    json_t *want = want_text ? json_loads(want_text, 0, NULL) : NULL;
    json_t *have = ext ? json_loadb(ext->data, ext->size, 0, NULL) : NULL;
    CHECK(have && want && json_equal(have, want) && json_is_array(json_object_get(have, "packages")) &&
          json_array_size(json_object_get(have, "packages")) == 0,
          "forgefirm-logs/system/extensions.json is what extpkg_status_json writes (no packages)");
    json_decref(have);
    json_decref(want);
    free(want_text);

    CHECK(tar && !has(tar, len, TOKEN) && !has(tar, len, CAMKEY),
          "the token and the camera key are nowhere in the archive");
    CHECK(tar && !has(tar, len, ACCOUNT) && install &&
          has(install->data, install->size, "account '<ACCOUNT>' created"),
          "the account's name is nowhere in the archive: the old line reads <ACCOUNT>");

    free(tar);
    nftw(dir, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    printf(failures ? "logs_export_test: %d FAILED\n" : "logs_export_test: all ok\n", failures);
    return failures ? 1 : 0;
}
