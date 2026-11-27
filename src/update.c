/*
 * update.c - forgectrl: firmware update manager
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Slot inventory (ffboot -l), boot-target selection, ForgeFIRM release
 * check / download / apply, dev-archive upload, and factory restore
 * from the /data archive. The A/B slot scheme and its invariants are
 * documented at
 * https://docs.forgefirm.org/technical/forgefirm/install-and-update/.
 *
 * Long operations run on a single detached worker (diag.c's model):
 * one job at a time, status polled from the UI. Every flash write
 * requires a machine free of every job (machine_job_free) and no running
 * diagnostic, takes the update lock (flock on
 * /data/forgefirm/update.lock, which ffboot takes for a switch too),
 * verifies the archive signature before writing, writes only a slot that
 * is NOT the booted root and not the next boot, marks the slot
 * incomplete before the write and clears the mark only once the written
 * filesystem verified from the media. Switching the boot target never
 * happens implicitly: it is its own explicit action, probe-gated by
 * ffboot itself, and refused for a slot marked incomplete.
 *
 * The release check (relcheck.c) asks the GitHub releases API for the
 * latest release: once a day on its own, and on request. The last
 * answer is kept here and served to the panel, which raises an alert
 * for a release newer than the installed version until the operator
 * dismisses that release. Only the download requests the firmware
 * file's URL, once, so GitHub's download counter counts installs. A
 * probe download runs the same job on the release's acceptance record
 * instead, into a file of its own, and keeps nothing.
 */
#define _GNU_SOURCE
#include "update.h"
#include "slotcalc.h"
#include "fwproduct.h"
#include "auth.h"
#include "diag.h"
#include "fflog.h"
#include "hooks.h"
#include "jobstream.h"
#include "lease.h"
#include "paths.h"
#include "relcheck.h"
#include "settings.h"
#include "sha256.h"
#include "status.h"
#include "stopsig.h"
#include "super.h"
#include "testhook.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <linux/fs.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FFBOOT      "/usr/sbin/ffboot"
/* The update's files, in the data directory (paths.h: FORGECTRL_DATA_DIR,
 * /data/forgefirm when unset). Made once by update_init, before any route
 * or job reads them. */
static char archive_dir[200], dl_fw[200], dl_probe[200], up_fw[200], lock_file[200], verify_dir[200];
#define ARCHIVE_DIR archive_dir
#define DL_FW       dl_fw
#define DL_PROBE    dl_probe
#define UP_FW       up_fw
#define LOCK_FILE   lock_file
/* Where a written slot is mounted read-only for its verification: the run
 * directory's ffverify. */
#define VERIFY_DIR  verify_dir
/* A download lands here and takes DL_FW's name only once verified: a cut
 * or refused download never stands as a staged archive. */
#define PART_SUFFIX ".part"
/* The reboot that follows a change made with the button held comes this
 * many seconds after the answer, which tells the operator to let go:
 * U-Boot reads a button held at power-on as a request for the factory
 * recovery. */
#define RELEASE_DELAY_S 5
#define KEY_RELEASE "/etc/forgefirm/keys/forgefirm-release.pub"
#define KEY_GF_DIR  "/etc/forgefirm/keys/gf"
/* An upload larger than any plausible archive is cut off (slot is
 * 200 MiB; a .fw compresses well below that). */
#define UPLOAD_MAX  (256UL * 1024 * 1024)
/* The same bound on the release download, enforced by curl before the
 * signature check can discard an oversized file. */
#define DL_MAX_BYTES "268435456"
/* The installed version, as the image wrote it. */
#define VERSION_FILE "/etc/forgefirm-version"
/* The release the operator dismissed the alert for (a settings key). */
#define DISMISSED_KEY "update_dismissed"
/* The release check's own clock: the first check after the daemon
 * starts waits for the network to come up; a check that got an answer
 * is repeated daily; one that did not is retried hourly. */
#define RELCHECK_FIRST_S  120
#define RELCHECK_DAILY_S  (24 * 3600)
#define RELCHECK_RETRY_S  3600

/* ------------------------------------------------------------- state */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int    job_running;
static char   job_owner[LEASE_OWNER_MAX];     /* the machine lease the running job holds */
static char   job_kind[24];
static char   job_phase[96];
static time_t job_started;
static char   job_result[768];      /* JSON object, or "" */
static char   progress_file[232];   /* file whose growth is progress */
static int    job_probe;            /* the job is a probe download */
static unsigned job_seq;            /* the number of the last job started, for the page that started it */
static char   dl_tag[48];           /* the release tag the last download verified its archive against */
static char   rel_current[48];      /* the installed version, read once at start */

static pthread_mutex_t up_mu = PTHREAD_MUTEX_INITIALIZER;
static FILE  *up_fp;
static const struct _u_request *up_owner;   /* the request streaming into up_fp */
static double up_last;                      /* its last chunk, on mono_s()'s clock */
static char   up_refusal[80];               /* why the sink refused, for the handler */
static uint64_t up_bytes;
static int    up_error;
static int    up_opened;                    /* up_owner opened the staging file: its own to discard */

/* A slot write's child runs in a process group of its own, so the daemon's
 * stop can end it (update_shutdown). */
static pthread_mutex_t wr_mu = PTHREAD_MUTEX_INITIALIZER;
static pid_t  wr_pgid;                      /* the running write's group, 0 when none */
static int    wr_stopping;                  /* the daemon stops: no write starts */

/* ----------------------------------------------------------- helpers */

/* The clock for timeouts (an upload's minute of silence): CLOCK_MONOTONIC,
 * which no step of the wall clock moves. */
static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static int reply_json(struct _u_response *res, unsigned status,
                      const char *body)
{
    ulfius_set_string_body_response(res, status, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

static int reply_err(struct _u_response *res, unsigned status,
                     const char *msg)
{
    char body[256];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    return reply_json(res, status, body);
}

/* Copy src into dst JSON-safely: printable chars minus quote/backslash. */
static void jsan(char *dst, size_t len, const char *src)
{
    size_t o = 0;
    for (size_t i = 0; src && src[i] && o + 1 < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= ' ' && c != '"' && c != '\\' && c < 0x7f)
            dst[o++] = (char)c;
        else if (c == '\n' && o && dst[o - 1] != ' ')
            dst[o++] = ' ';
    }
    while (o > 0 && dst[o - 1] == ' ')
        o--;
    dst[o] = '\0';
}

/* Run a command, capture combined output (sanitized), return exit code
 * (negative on popen failure). */
static int run_cmd(char *out, size_t outlen, const char *cmd)
{
    if (out && outlen)
        out[0] = '\0';
    FILE *p = popen(cmd, "r");
    if (!p)
        return -1;
    char raw[2048];
    size_t n = fread(raw, 1, sizeof(raw) - 1, p);
    raw[n] = '\0';
    while (fgetc(p) != EOF)
        ;                               /* drain so the child can exit */
    int st = pclose(p);
    if (out && outlen)
        jsan(out, outlen, raw);
    if (st < 0)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Run a slot write's command as run_cmd does, in a process group of its
 * own that update_shutdown() ends if the daemon stops meanwhile: the exit
 * code, -1 when it could not be run, -2 once the daemon stops. The update
 * lock's descriptor is the child's too, so a write that outlives the
 * daemon (a crash) still holds the lock until it ends. */
static int run_write(char *out, size_t outlen, const char *cmd)
{
    int fd[2];
    if (out && outlen)
        out[0] = '\0';
    if (pipe2(fd, O_CLOEXEC) != 0)
        return -1;
    pthread_mutex_lock(&wr_mu);
    if (wr_stopping) {
        pthread_mutex_unlock(&wr_mu);
        close(fd[0]);
        close(fd[1]);
        return -2;
    }
    pid_t pid = stopsig_fork();
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fd[1], 1);
        dup2(fd[1], 2);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        setpgid(pid, pid);              /* both sides, so the group stands before either runs on */
        wr_pgid = pid;
    }
    pthread_mutex_unlock(&wr_mu);
    close(fd[1]);
    if (pid < 0) {
        close(fd[0]);
        return -1;
    }
    char raw[2048], sink[512];
    size_t n = 0;
    for (;;) {
        /* Past the buffer, read on into the sink: drained, so the child
         * can exit. */
        int full = n + 1 >= sizeof(raw);
        ssize_t r = read(fd[0], full ? sink : raw + n, full ? sizeof(sink) : sizeof(raw) - 1 - n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        if (!full)
            n += (size_t)r;
    }
    raw[n] = '\0';
    close(fd[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    pthread_mutex_lock(&wr_mu);
    wr_pgid = 0;
    int stopping = wr_stopping;
    pthread_mutex_unlock(&wr_mu);
    if (out && outlen)
        jsan(out, outlen, raw);
    if (stopping)
        return -2;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* The host tests' slot seam (testhook.h): GF_SLOT_DIR names a directory
 * that stands for the slot table in a host-test run. Each slot's device
 * is the file of its device's name there (mmcblk2p1, ...), the booted
 * root is the file `root` is a link to, and a slot's size is
 * sys/<device>/size, as /sys/class/block gives it. Its value goes into
 * command lines, so a directory with any character but a letter, a
 * digit, '/', '.', '_' or '-' is not taken. NULL on a machine. */
static const char *slot_dir(void)
{
    const char *d = ff_test_hook("GF_SLOT_DIR");
    if (!d)
        return NULL;
    for (const char *p = d; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '/' || *p == '.' || *p == '_' || *p == '-'))
            return NULL;
    return d;
}

/* The boot-slot tool: ffboot, or under the slot seam the seam's own
 * stand-in, <dir>/ffboot. */
static const char *ffboot_path(void)
{
    static char path[220];
    const char *sd = slot_dir();
    if (!sd)
        return FFBOOT;
    snprintf(path, sizeof(path), "%s/ffboot", sd);
    return path;
}

/* Whether dev is the running root's device, by device number: 1, 0, or
 * -1 when either cannot be read. Under the slot seam a slot is a plain
 * file and the root a link to one, compared by inode. */
static int dev_is_root(const char *dev)
{
    const char *sd = slot_dir();
    char root[200];
    snprintf(root, sizeof(root), "%s/root", sd ? sd : "");
    struct stat r, d;
    if (stat(sd ? root : "/", &r) != 0)
        return -1;
    if (stat(dev, &d) != 0 || !(sd ? S_ISREG(d.st_mode) : S_ISBLK(d.st_mode)))
        return -1;
    return sd ? r.st_dev == d.st_dev && r.st_ino == d.st_ino : r.st_dev == d.st_rdev;
}

/* True when dev is the block device the running root filesystem lives
 * on - or when that cannot be determined. Device-number comparison, not
 * string matching: root= may be spelled PARTUUID=, UUID=, /dev/root, or
 * any alias, and a guard that fails to recognize the spelling must
 * REFUSE the write into the running slot, never permit it. */
static int is_booted_root(const char *dev)
{
    return dev_is_root(dev) != 0;       /* cannot determine: fail closed */
}

/* The root= spelling from the kernel command line, for display only
 * (the write guard above never trusts it). */
static const char *booted_root(void)
{
    static char root[32];
    if (!root[0]) {
        FILE *f = fopen("/proc/cmdline", "r");
        char line[512] = "";
        if (f) {
            if (!fgets(line, sizeof(line), f))
                line[0] = '\0';
            fclose(f);
        }
        char *p = strstr(line, "root=");
        if (p) {
            p += 5;
            size_t o = 0;
            while (*p && *p != ' ' && o + 1 < sizeof(root))
                root[o++] = *p++;
            root[o] = '\0';
        }
    }
    return root;
}

struct slot_target {
    const char *name;
    const char *dev;
    const char *ffboot_arg;         /* boot-select argument */
    const char *task;               /* fwup upgrade task, or NULL */
};
static const struct slot_target targets[] = {
    { "sd",     "/dev/mmcblk1p1", "-s",  NULL },
    { "a",      "/dev/mmcblk2p1", "-e1", "upgrade.a" },
    { "b",      "/dev/mmcblk2p2", "-e2", "upgrade.b" },
    { "legacy", "/dev/mmcblk2p4", "-e4", NULL },
};
#define N_TARGETS (sizeof(targets) / sizeof(*targets))

/* Each target's device as this run reads and writes it: the machine's,
 * or under the slot seam the file of its name in the seam's directory.
 * Made once by update_init. */
static char target_dev[N_TARGETS][200];

static const char *dev_of(const struct slot_target *t)
{
    return target_dev[t - targets];
}

/* The target the running root is on, by device number; NULL when it is
 * on none of them, or that cannot be read. */
static const struct slot_target *booted_target(void)
{
    for (size_t i = 0; i < N_TARGETS; i++)
        if (dev_is_root(dev_of(&targets[i])) == 1)
            return &targets[i];
    return NULL;
}

/* Its name, for the log: "" when there is none. */
static const char *booted_slot(void)
{
    const struct slot_target *t = booted_target();
    return t ? t->name : "";
}

static const struct slot_target *find_target(const char *name)
{
    for (size_t i = 0; name && i < N_TARGETS; i++)
        if (!strcmp(targets[i].name, name))
            return &targets[i];
    return NULL;
}

/* A write target must be the factory 200 MiB slot geometry (409600
 * 512-byte sectors) - the same check the installer makes. This refuses
 * a repartitioned or absent slot before a raw image write, so it cannot
 * overflow the partition or land on an unexpected layout. */
static int slot_geometry_ok(const struct slot_target *t)
{
    const char *base = strrchr(t->dev, '/');
    base = base ? base + 1 : t->dev;
    const char *sd = slot_dir();
    char path[260], buf[24];
    if (sd)
        snprintf(path, sizeof(path), "%s/sys/%s/size", sd, base);
    else
        snprintf(path, sizeof(path), "/sys/class/block/%s/size", base);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    int ok = fgets(buf, sizeof(buf), f) && atol(buf) == 409600;
    fclose(f);
    return ok;
}

static const char *param(const struct _u_request *req, const char *key)
{
    const char *v = u_map_get(req->map_post_body, key);
    return v ? v : u_map_get(req->map_url, key);
}

/* fwup -m meta-version of an archive (empty string if unreadable). */
static void fw_meta_version(const char *file, char *out, size_t len)
{
    char cmd[512], raw[192];
    out[0] = '\0';
    snprintf(cmd, sizeof(cmd),
             "fwup -m -i %s 2>/dev/null | sed -n 's/^meta-version=\"\\{0,1\\}\\([^\"]*\\).*/\\1/p'",
             file);
    if (run_cmd(raw, sizeof(raw), cmd) == 0)
        jsan(out, len, raw);
}

/* Signature classification: 2 = ForgeFIRM release key, 1 = a Glowforge
 * factory key, 0 = valid fwup archive but neither key, -1 = not a
 * usable fwup archive. */
static int fw_classify(const char *file)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "fwup -V -i %s -p %s >/dev/null 2>&1",
             file, KEY_RELEASE);
    if (run_cmd(NULL, 0, cmd) == 0)
        return 2;
    DIR *d = opendir(KEY_GF_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.')
                continue;
            snprintf(cmd, sizeof(cmd),
                     "fwup -V -i %s -p " KEY_GF_DIR "/%s >/dev/null 2>&1",
                     file, e->d_name);
            if (run_cmd(NULL, 0, cmd) == 0) {
                closedir(d);
                return 1;
            }
        }
        closedir(d);
    }
    snprintf(cmd, sizeof(cmd), "fwup -l -i %s >/dev/null 2>&1", file);
    return run_cmd(NULL, 0, cmd) == 0 ? 0 : -1;
}

/* Semantic version an archive was recorded with, from the manifest's
 * "ver=<semantic>" field. Empty if the archive is not in the manifest,
 * has no ver= (older installer), or ver=unknown. */
static void archive_manifest_version(const char *file, char *out, size_t len)
{
    out[0] = '\0';
    char manifest[256];
    snprintf(manifest, sizeof(manifest), "%s/manifest", ARCHIVE_DIR);
    FILE *f = fopen(manifest, "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, file))
            continue;
        char *v = strstr(line, "ver=");
        if (v) {
            v += 4;
            char raw[48];
            size_t o = 0;
            while (v[o] && v[o] != ' ' && v[o] != '\n' && o + 1 < sizeof(raw)) {
                raw[o] = v[o];
                o++;
            }
            raw[o] = '\0';
            if (strcmp(raw, "unknown") != 0)
                jsan(out, len, raw);
        }
        break;
    }
    fclose(f);
}

/* The SHA-256 of a file, in hex: 0, or -1 when it cannot be read whole. */
static int file_sha256(const char *path, char hex[2 * SHA256_LEN + 1])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    sha256_ctx c;
    unsigned char buf[16384], d[SHA256_LEN];
    size_t n;
    sha256_init(&c);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        sha256_update(&c, buf, n);
    int bad = ferror(f);
    fclose(f);
    sha256_final(&c, d);
    sha256_hex(d, SHA256_LEN, hex);
    return bad ? -1 : 0;
}

/* What a staged archive is, for the inventory and the dialogs: its
 * version, its signature class (fw_classify) and its SHA-256, kept per
 * file and read again only when the file changed (its inode, size or
 * modification time). */
struct staged_info {
    dev_t dev;
    ino_t ino;
    off_t size;
    struct timespec mtime;
    char version[48];
    int cls;
    char sha256[2 * SHA256_LEN + 1];
};
static pthread_mutex_t staged_mu = PTHREAD_MUTEX_INITIALIZER;
static struct staged_info staged_cache[2];  /* the download, the upload */

/* 0 with *out filled, or -1 when the file is not there or cannot be read. */
static int staged_read(int which, const char *path, struct staged_info *out)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size == 0)
        return -1;
    pthread_mutex_lock(&staged_mu);
    struct staged_info *c = &staged_cache[which];
    int same = c->sha256[0] && c->dev == st.st_dev && c->ino == st.st_ino && c->size == st.st_size &&
               c->mtime.tv_sec == st.st_mtim.tv_sec && c->mtime.tv_nsec == st.st_mtim.tv_nsec;
    if (same)
        *out = *c;
    pthread_mutex_unlock(&staged_mu);
    if (same)
        return 0;
    struct staged_info fresh = { .dev = st.st_dev, .ino = st.st_ino, .size = st.st_size, .mtime = st.st_mtim };
    if (file_sha256(path, fresh.sha256) != 0)
        return -1;
    fw_meta_version(path, fresh.version, sizeof(fresh.version));
    fresh.cls = fw_classify(path);
    pthread_mutex_lock(&staged_mu);
    staged_cache[which] = fresh;
    pthread_mutex_unlock(&staged_mu);
    *out = fresh;
    return 0;
}

/* The signature class in the inventory's words. */
static const char *class_word(int cls)
{
    return cls == 2 ? "forgefirm" : cls == 1 ? "glowforge" : cls == 0 ? "unsigned" : "unreadable";
}

/* --------------------------------------------- the incomplete-write mark */

/* A slot write leaves <data dir>/slot-<name>.incomplete from before its
 * first byte until the written filesystem verified from the media: a write
 * that failed, was cut by a power loss or by the daemon's stop, keeps it.
 * The path is the contract with ffboot, which refuses to select a marked
 * slot (-f included) and lists it, and with the installer, which marks
 * its own write the same way. A rewrite that verifies clears it. */
static void marker_path(const struct slot_target *t, char *buf, size_t len)
{
    snprintf(buf, len, "%s/slot-%s.incomplete", ff_data_dir(), t->name);
}

static int slot_marked(const struct slot_target *t)
{
    char path[260];
    marker_path(t, path, sizeof(path));
    return access(path, F_OK) == 0;
}

static void sync_data_dir(void)
{
    int fd = open(ff_data_dir(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

/* 0 once the mark is on the media (the file and its directory synced),
 * -1 when it could not be made: then nothing is written. */
static int marker_set(const struct slot_target *t)
{
    char path[260], line[96];
    marker_path(t, path, sizeof(path));
    mkdir(ff_data_dir(), 0755);
    int n = snprintf(line, sizeof(line), "an update wrote slot %s and did not verify it\n", t->name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    int ok = fd >= 0 && write(fd, line, (size_t)n) == n && fsync(fd) == 0;
    if (fd >= 0)
        close(fd);
    if (!ok)
        return -1;
    sync_data_dir();
    return 0;
}

static void marker_clear(const struct slot_target *t)
{
    char path[260];
    marker_path(t, path, sizeof(path));
    if (unlink(path) == 0)
        sync_data_dir();
}

/* The written slot out to the media and out of the page cache, so the
 * verification that follows reads what the media holds: 0, or -1. A slot
 * that is not a block device (the host tests' slot seam) has no cache of
 * its own to drop. */
static int slot_flush(const char *dev)
{
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat st;
    int rc = fstat(fd, &st) == 0 && fsync(fd) == 0 ? 0 : -1;
    if (rc == 0 && S_ISBLK(st.st_mode) && ioctl(fd, BLKFLSBUF, 0) != 0)
        rc = -1;
    close(fd);
    return rc;
}

/* ----------------------------------------------------- no job runs */

/* A reboot is scheduled (schedule_reboot): from then on nothing starts
 * that the reboot would cut, and no press is taken for a second consent.
 * It stands until the reboot. In a host-test run the removal of the test
 * hook's file is the test's restart, and clears it (testhook.h). */
static pthread_mutex_t reboot_mu = PTHREAD_MUTEX_INITIALIZER;
static int reboot_scheduled;

static int reboot_pending(void)
{
    const char *hook = ff_test_hook("GF_REBOOT_FILE");
    pthread_mutex_lock(&reboot_mu);
    if (hook && access(hook, F_OK) != 0)
        reboot_scheduled = 0;
    int set = reboot_scheduled;
    pthread_mutex_unlock(&reboot_mu);
    return set;
}

/* Whether the machine is free of every job, for a change that would take
 * the system from under one (an update job's start, the boot selection,
 * a reboot, the reset, the factory return's write, selection and reboot,
 * a downgrade's or unsigned install's press): no reboot pending
 * (reboot_pending); the kernel idle; no job that waits for its press, sits
 * in its armed window, runs or is paused, nor a GRBL controller whose
 * state cannot be vouched for (job_takes_button); and no Grbl sender
 * connected to the running GRBL controller, whose next move nothing here
 * can see coming. 1, or 0 with the refusal in why. */
static int machine_job_free(char *why, size_t len)
{
    if (reboot_pending()) {
        snprintf(why, len, "the machine is restarting");
        return 0;
    }
    if (!machine_is_idle()) {
        snprintf(why, len, "machine is not idle");
        return 0;
    }
    if (job_takes_button(why, len))
        return 0;
    if (super_grbl_running() && jobstream_sender_connected() == 1) {
        snprintf(why, len, "a Grbl sender is connected to the machine: finish its job and close the sender first");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------ job machinery */

int update_job_running(void)
{
    pthread_mutex_lock(&mu);
    int r = job_running;
    pthread_mutex_unlock(&mu);
    return r;
}

static void job_set_phase(const char *phase)
{
    pthread_mutex_lock(&mu);
    snprintf(job_phase, sizeof(job_phase), "%s", phase);
    pthread_mutex_unlock(&mu);
}

/* A job's outcome in the forgectrl log, from its result: the record of
 * every download, write, restore, and factory return, whatever the
 * panel showed. A probe's own refusal is its expected end, not a
 * warning. */
static void log_outcome(const char *kind, int probe, const char *result)
{
    const char *what = probe ? "probe download"
                     : !strcmp(kind, "factory-return") ? "factory return" : kind;
    json_t *o = json_loads(result, 0, NULL);
    if (!json_is_object(o)) {
        fflog(LOG_WARNING, "update: the %s ended: %s", what, result);
        json_decref(o);
        return;
    }
    if (json_is_true(json_object_get(o, "ok"))) {
        char facts[512] = "";
        const char *k;
        json_t *v;
        json_object_foreach(o, k, v) {
            char one[160];
            if (!strcmp(k, "ok"))
                continue;
            if (json_is_string(v))
                snprintf(one, sizeof(one), "%s %s", k, json_string_value(v));
            else if (json_is_boolean(v))
                snprintf(one, sizeof(one), "%s %s", k, json_is_true(v) ? "yes" : "no");
            else if (json_is_integer(v))
                snprintf(one, sizeof(one), "%s %lld", k, (long long)json_integer_value(v));
            else
                continue;
            if (facts[0])
                strncat(facts, ", ", sizeof(facts) - strlen(facts) - 1);
            strncat(facts, one, sizeof(facts) - strlen(facts) - 1);
        }
        fflog(LOG_NOTICE, "update: the %s finished: %s", what, facts);
    } else {
        const char *err = json_string_value(json_object_get(o, "error"));
        const char *detail = json_string_value(json_object_get(o, "detail"));
        int has_detail = detail && *detail;
        /* A probe ends refused by design: what it fetches is the
         * release's acceptance record, which the verification discards
         * (or, signed, the probe does). A probe that ends any other way,
         * a failed fetch among them, is a failure like a download's. */
        int expected = probe && err && (strstr(err, "signature verification failed") ||
                                        strstr(err, "a probe keeps nothing"));
        fflog(expected ? LOG_NOTICE : LOG_WARNING, "update: the %s failed: %s%s%s%s", what,
              err ? err : "no reason given", has_detail ? " (" : "",
              has_detail ? detail : "", has_detail ? ")" : "");
    }
    json_decref(o);
}

/* GF_RUN_END_GAP_MS (testhook.h): a host test's pause once a job reads
 * ended, so it can start the next one there. */
static void run_end_gap(void)
{
    const char *ms = ff_test_hook("GF_RUN_END_GAP_MS");
    if (ms)
        usleep((useconds_t)atoi(ms) * 1000);
}

static void job_finish(const char *fmt, ...)
{
    char kind[sizeof(job_kind)], result[sizeof(job_result)];
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&mu);
    vsnprintf(job_result, sizeof(job_result), fmt, ap);
    snprintf(kind, sizeof(kind), "%s", job_kind);
    snprintf(result, sizeof(result), "%s", job_result);
    int probe = job_probe;
    /* The lease goes back while the job still reads running: a start
     * that comes once it reads idle finds the lease free, and job_owner
     * is the ending job's (mu, then the lease's mutex, as in job_start). */
    lease_release(job_owner);
    job_running = 0;
    job_phase[0] = '\0';
    progress_file[0] = '\0';
    pthread_mutex_unlock(&mu);
    va_end(ap);
    run_end_gap();
    log_outcome(kind, probe, result);
}

/* Start a job: 0 ok, -1 busy, -2 a job runs on the machine
 * (machine_job_free; the refusal in why), -3 the machine lease is
 * somebody else's (the holder in why), -4 an upload is streaming. */
static int job_start(const char *kind, void *(*worker)(void *), void *arg, char *why, size_t len)
{
    if (!machine_job_free(why, len))
        return -2;
    pthread_mutex_lock(&up_mu);
    int uploading = up_fp != NULL && mono_s() - up_last < 60;
    pthread_mutex_unlock(&up_mu);
    if (uploading)
        return -4;
    ff_test_pause("job_start");
    pthread_mutex_lock(&mu);
    if (job_running) {
        pthread_mutex_unlock(&mu);
        return -1;
    }
    /* A diagnostic, a wizard, the recorder, or a log export refuses the
     * job: none of them should find the system changed under it. */
    snprintf(job_owner, sizeof(job_owner), "update:%s", kind);
    if (lease_take(job_owner, LEASE_SYSTEM, NULL, why, len) != 0) {
        pthread_mutex_unlock(&mu);
        return -3;
    }
    /* Asked again under the hold: a reboot route holds the same lease
     * from its check to its schedule, so a reboot scheduled since the
     * check above is seen here, and the job does not start under it. */
    if (reboot_pending()) {
        lease_release(job_owner);
        pthread_mutex_unlock(&mu);
        snprintf(why, len, "the machine is restarting");
        return -2;
    }
    job_running = 1;
    job_seq++;
    snprintf(job_kind, sizeof(job_kind), "%s", kind);
    job_phase[0] = '\0';
    job_result[0] = '\0';
    progress_file[0] = '\0';
    job_probe = 0;
    job_started = time(NULL);
    pthread_mutex_unlock(&mu);

    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&th, &at, worker, arg);
    pthread_attr_destroy(&at);
    if (rc != 0) {
        /* As job_finish: the lease goes back while job_owner is still
         * this job's, before a start can rewrite it. */
        pthread_mutex_lock(&mu);
        lease_release(job_owner);
        job_running = 0;
        pthread_mutex_unlock(&mu);
        return -1;
    }
    return 0;
}

/* Reply for a job_start return code. On 0 the worker owns `arg`; on
 * any error the CALLER still owns it and must free. */
static int job_start_reply(struct _u_response *res, int rc, const char *why)
{
    switch (rc) {
    case 0: {
        /* The job's number: GET /update/status carries it, so a page
         * acts on the result of the job it started, and no other. */
        char body[64];
        pthread_mutex_lock(&mu);
        snprintf(body, sizeof(body), "{\"started\":true,\"job\":%u}", job_seq);
        pthread_mutex_unlock(&mu);
        return reply_json(res, 202, body);
    }
    case -1:
        return reply_err(res, 409, "an update job is already running");
    case -4:
        return reply_err(res, 409, "an upload is still streaming into the staging file");
    default:
        return reply_err(res, 409, why);
    }
}

/* The update lock: a slot write holds it exclusively (this daemon's jobs),
 * so does ffboot for a switch, and the inventory's probe takes it shared,
 * so a probe never mounts a slot a write is writing. The installer, which
 * runs on the factory firmware, takes no lock. */
static int lock_fd = -1;

/* Taken exclusively, waiting out an inventory's probe (a few seconds) for
 * up to LOCK_WAIT_S: 0, or -1 when something else still holds it. The
 * descriptor is not close-on-exec: the write's child holds the lock too. */
#define LOCK_WAIT_S 30
static int take_lock(void)
{
    mkdir(ff_data_dir(), 0755);
    lock_fd = open(LOCK_FILE, O_CREAT | O_RDWR, 0644);
    if (lock_fd < 0)
        return -1;
    for (int i = 0; flock(lock_fd, LOCK_EX | LOCK_NB) != 0; i++) {
        if (i >= LOCK_WAIT_S * 10) {
            close(lock_fd);
            lock_fd = -1;
            return -1;
        }
        usleep(100000);
    }
    return 0;
}

/* The lock taken shared for the inventory's probe, not waited for: the
 * descriptor, or -1 while a write or a switch holds it (or the file
 * cannot be opened). */
static int lock_shared(void)
{
    mkdir(ff_data_dir(), 0755);
    int fd = open(LOCK_FILE, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_SH | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Whether anything holds the lock now: a write of this daemon's, one that
 * outlived an earlier one, or a switch at the console. */
static int lock_busy(void)
{
    int fd = open(LOCK_FILE, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return 0;                       /* no lock file: nobody ever took it */
    int busy = flock(fd, LOCK_EX | LOCK_NB) != 0;
    close(fd);                          /* a lock taken here goes with the descriptor */
    return busy;
}

static void drop_lock(void)
{
    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        lock_fd = -1;
    }
}

/* --------------------------------------------------------- inventory */

struct slot_info {
    char present[8], state[16], type[16], version[64], kernel[8];
    int booted, next;
};

/* Bounded append for a JSON body: off never passes the buffer, so a
 * body that outgrows it is truncated rather than written past. */
static void bput(char *buf, size_t size, size_t *off, const char *fmt, ...)
{
    if (*off >= size)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, size - *off, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    *off += (size_t)n;
    if (*off >= size)
        *off = size - 1;
}

/* The slot inventory as `ffboot -l` prints it, into buf: 0, or -1 when
 * ffboot could not be run. In a host-test run the test hook GF_SLOT_LIST
 * names a file that holds it (testhook.h). */
static int slot_list(char *buf, size_t len)
{
    const char *hook = ff_test_hook("GF_SLOT_LIST");
    FILE *p = hook ? fopen(hook, "r") : popen(FFBOOT " -l 2>/dev/null", "r");
    if (!p)
        return -1;
    size_t n = fread(buf, 1, len - 1, p);
    buf[n] = '\0';
    if (hook)
        fclose(p);
    else
        pclose(p);
    return 0;
}

/* The value of the inventory's line <key>=<value> (key whole, at a line's
 * start), into out: 1, or 0 with out empty. ffboot keeps each value to
 * one line, so no value can stand for another key. */
static int inv_get(const char *raw, const char *key, char *out, size_t len)
{
    size_t k = strlen(key);
    out[0] = '\0';
    for (const char *p = raw; p && *p;) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        if (n > k && !strncmp(p, key, k) && p[k] == '=') {
            size_t v = n - k - 1 < len ? n - k - 1 : len - 1;
            memcpy(out, p + k + 1, v);
            out[v] = '\0';
            return 1;
        }
        p = eol ? eol + 1 : NULL;
    }
    return 0;
}

/* Schedule a reboot delay_s seconds on, so the reply goes out first: 0,
 * or -1 when it could not be scheduled. A scheduled reboot stands pending
 * (reboot_pending), which machine_job_free refuses. Every caller holds
 * LEASE_SYSTEM from its own machine_job_free to here (the reboot route,
 * the reset, the factory return's job), so no update job starts between
 * the check and the schedule (job_start asks again under its hold). In a
 * host-test run the test hook GF_REBOOT_FILE names a file the reboot is
 * written to in its place, and GF_PAUSE_DIR may hold it here first
 * (testhook.h). */
static int schedule_reboot(int delay_s)
{
    const char *hook = ff_test_hook("GF_REBOOT_FILE");
    int rc;
    ff_test_pause("reboot");
    if (hook) {
        FILE *f = fopen(hook, "a");
        if (!f)
            return -1;
        fprintf(f, "reboot in %d s\n", delay_s);
        rc = fclose(f) == 0 ? 0 : -1;
    } else {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "(sleep %d; reboot) >/dev/null 2>&1 &", delay_s);
        rc = system(cmd) == 0 ? 0 : -1;
    }
    if (rc == 0) {
        pthread_mutex_lock(&reboot_mu);
        reboot_scheduled = 1;
        pthread_mutex_unlock(&reboot_mu);
    }
    return rc;
}

/* Is this slot the one the saved environment boots next (ffboot -l)?
 * A write into the next-boot slot has no revert behind it: a failed
 * or interrupted write there leaves the next boot on a half-written
 * root. Unreadable counts as next. */
static int slot_is_next(const struct slot_target *t)
{
    char raw[4096], key[48], v[8];
    if (slot_list(raw, sizeof(raw)) != 0)
        return 1;
    snprintf(key, sizeof(key), "slot.%s.next", t->name);
    return inv_get(raw, key, v, sizeof(v)) && !strcmp(v, "yes");
}

int cb_slots(const struct _u_request *req, struct _u_response *res,
             void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;

    /* ffboot -l mounts every slot it probes; while an update job runs, or
     * anything holds the update lock (a write, or a switch at the
     * console), the inventory is answered from the last probe instead.
     * The probe holds the lock shared, so no write starts under it. */
    static char cached[8192];
    static pthread_mutex_t cache_mu = PTHREAD_MUTEX_INITIALIZER;
    int lfd = update_job_running() ? -1 : lock_shared();
    if (lfd < 0) {
        pthread_mutex_lock(&cache_mu);
        if (cached[0]) {
            int rc = reply_json(res, 200, cached);
            pthread_mutex_unlock(&cache_mu);
            return rc;
        }
        pthread_mutex_unlock(&cache_mu);
        return reply_err(res, 409, "slot inventory unavailable during an update");
    }

    char raw[4096];
    int listed = slot_list(raw, sizeof(raw));
    close(lfd);
    if (listed != 0)
        return reply_err(res, 500, "cannot run ffboot");

    char env_json[512] = "";
    size_t eo = 0;
    struct slot_info si[N_TARGETS];
    memset(si, 0, sizeof(si));

    char *save = NULL;
    for (char *ln = strtok_r(raw, "\n", &save); ln;
         ln = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(ln, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char val[96];
        jsan(val, sizeof(val), eq + 1);
        if (!strncmp(ln, "env.", 4)) {
            if (eo < sizeof(env_json)) {
                int need = snprintf(NULL, 0, "%s\"%s\":\"%s\"", eo ? "," : "",
                                    ln + 4, val);
                if (need > 0 && eo + (size_t)need < sizeof(env_json))
                    eo += (size_t)snprintf(env_json + eo, sizeof(env_json) - eo,
                                           "%s\"%s\":\"%s\"", eo ? "," : "",
                                           ln + 4, val);
                /* else: a value that does not fit is left out whole */
            }
            continue;
        }
        if (strncmp(ln, "slot.", 5))
            continue;
        char *dot = strchr(ln + 5, '.');
        if (!dot)
            continue;
        *dot = '\0';
        const char *field = dot + 1;
        for (size_t t = 0; t < N_TARGETS; t++) {
            if (strcmp(ln + 5, targets[t].name))
                continue;
            /* Explicit precisions: ffboot's values are truncated to
             * each field's capacity by design. */
            if (!strcmp(field, "present"))
                snprintf(si[t].present, sizeof(si[t].present), "%.7s", val);
            else if (!strcmp(field, "state"))
                snprintf(si[t].state, sizeof(si[t].state), "%.15s", val);
            else if (!strcmp(field, "type"))
                snprintf(si[t].type, sizeof(si[t].type), "%.15s", val);
            else if (!strcmp(field, "version"))
                snprintf(si[t].version, sizeof(si[t].version), "%.63s", val);
            else if (!strcmp(field, "kernel"))
                snprintf(si[t].kernel, sizeof(si[t].kernel), "%.7s", val);
            else if (!strcmp(field, "booted"))
                si[t].booted = 1;
            else if (!strcmp(field, "next"))
                si[t].next = 1;
        }
    }

    char body[8192];
    size_t off = 0;
    bput(body, sizeof(body), &off,
                            "{\"booted\":\"%s\",\"env\":{%s},\"slots\":{",
                            booted_root(), env_json);
    /* Always show the two firmware slots (a/b). Only surface sd and the
     * legacy partition when they actually exist: on a normal install the
     * legacy partition was reclaimed at first boot, so a "legacy: not
     * present" line is just noise - it should appear only if for some
     * reason it could not be removed. */
    int emitted = 0;
    for (size_t t = 0; t < N_TARGETS; t++) {
        int present = si[t].present[0] && !strcmp(si[t].present, "yes");
        int always = !strcmp(targets[t].name, "a") ||
                     !strcmp(targets[t].name, "b");
        if (!always && !present)
            continue;
        bput(body, sizeof(body), &off,
            "%s\"%s\":{\"device\":\"%s\",\"present\":\"%s\","
            "\"state\":\"%s\",\"type\":\"%s\",\"version\":\"%s\","
            "\"kernel\":\"%s\",\"booted\":%s,\"next\":%s,\"incomplete\":%s}",
            emitted ? "," : "", targets[t].name, targets[t].dev,
            si[t].present[0] ? si[t].present : "no",
            si[t].state, si[t].type, si[t].version, si[t].kernel,
            si[t].booted ? "true" : "false",
            si[t].next ? "true" : "false",
            slot_marked(&targets[t]) ? "true" : "false");
        emitted = 1;
    }
    bput(body, sizeof(body), &off,
                            "},\"archives\":[");

    /* Only factory-rootfs archives are user-restorable; the recovery
     * boot blobs are not something a restore writes, so they are left
     * out of the list. The semantic version comes from
     * the manifest's ver= field (written by the installer); without it
     * (older archives) the display falls back to the build date parsed
     * from the filename. */
    DIR *d = opendir(ARCHIVE_DIR);
    int first = 1;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && off + 320 < sizeof(body)) {
            if (strncmp(e->d_name, "factory-rootfs-", 15))
                continue;
            char path[512];
            struct stat st;
            snprintf(path, sizeof(path), "%s/%s", ARCHIVE_DIR, e->d_name);
            if (stat(path, &st) != 0)
                continue;
            char name[160], ver[48] = "", date[24] = "";
            jsan(name, sizeof(name), e->d_name);
            archive_manifest_version(e->d_name, ver, sizeof(ver));
            /* filename is factory-rootfs-<YYYYMMDDHHMMSS>.img.gz */
            const char *ds = e->d_name + 15;
            if (strlen(ds) >= 8)
                snprintf(date, sizeof(date), "%.4s-%.2s-%.2s",
                         ds, ds + 4, ds + 6);
            bput(body, sizeof(body), &off,
                "%s{\"file\":\"%s\",\"bytes\":%ld,\"version\":\"%s\","
                "\"date\":\"%s\"}",
                first ? "" : ",", name, (long)st.st_size, ver, date);
            first = 0;
        }
        closedir(d);
    }
    bput(body, sizeof(body), &off,
                            "],\"staged\":{");
    /* Each staged archive with what its install dialog shows: the
     * version, whether it is newer than the installed one (an install that
     * is not takes downgrade=1 and the button), its signature class and its
     * SHA-256 (carried back into the install, which refuses another file). */
    const struct { const char *key; const char *path; } staged[] = {
        { "download", DL_FW }, { "upload", UP_FW },
    };
    for (size_t s = 0; s < 2; s++) {
        struct staged_info si_ = { 0 };
        int have = staged_read((int)s, staged[s].path, &si_) == 0;
        bput(body, sizeof(body), &off,
            "%s\"%s\":{\"present\":%s,\"bytes\":%ld,\"version\":\"%s\",\"newer\":%s,"
            "\"signature\":\"%s\",\"sha256\":\"%s\"}",
            s ? "," : "", staged[s].key, have ? "true" : "false",
            have ? (long)si_.size : 0, have ? si_.version : "",
            have && relcheck_is_newer(si_.version, rel_current) ? "true" : "false",
            have ? class_word(si_.cls) : "", have ? si_.sha256 : "");
    }
    snprintf(body + off, sizeof(body) - off, "}}");
    pthread_mutex_lock(&cache_mu);
    snprintf(cached, sizeof(cached), "%s", body);
    pthread_mutex_unlock(&cache_mu);
    return reply_json(res, 200, body);
}

/* ------------------------------------------------------- boot select */

int cb_boot_select(const struct _u_request *req, struct _u_response *res,
                   void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char held[160];
    if (lease_refusal(held, sizeof(held)))
        return reply_err(res, 409, held);
    if (!machine_job_free(held, sizeof(held)))
        return reply_err(res, 409, held);

    const struct slot_target *t = find_target(param(req, "target"));
    if (!t)
        return reply_err(res, 400,
                         "target must be sd, a, b, or legacy");
    /* A slot an update left half-written, or a write still holding the
     * lock (one that outlived an earlier daemon, a switch at the
     * console): neither is a slot to boot. ffboot refuses both too. */
    if (slot_marked(t))
        return reply_err(res, 409, "that slot holds an update that did not complete: write it again first");
    if (lock_busy())
        return reply_err(res, 409, "an update holds the update lock: wait for it to end");

    char cmd[300], out[512];
    snprintf(cmd, sizeof(cmd), "%s %s -n 2>&1", ffboot_path(), t->ffboot_arg);
    int rc = run_cmd(out, sizeof(out), cmd);
    if (rc != 0) {
        fflog(LOG_WARNING, "update: selecting slot %s for the next boot failed: %s",
              t->name, out);
        char body[640];
        snprintf(body, sizeof(body),
                 "{\"error\":\"boot selection failed\",\"detail\":\"%s\"}",
                 out);
        return reply_json(res, 409, body);
    }
    fflog(LOG_NOTICE, "update: the next boot is slot %s", t->name);
    char body[640];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"target\":\"%s\",\"detail\":\"%s\"}",
             t->name, out);
    return reply_json(res, 200, body);
}

int cb_system_reboot(const struct _u_request *req, struct _u_response *res,
                     void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    /* The machine's lease from the check to the schedule (schedule_reboot):
     * no update job starts in between. */
    char held[160];
    if (lease_take("update:reboot", LEASE_SYSTEM, NULL, held, sizeof(held)) != 0)
        return reply_err(res, 409, held);
    int rc;
    const char *c = param(req, "confirm");
    if (!machine_job_free(held, sizeof(held)))
        rc = reply_err(res, 409, held);
    else if (!c || strcmp(c, "1"))
        rc = reply_err(res, 400, "confirm=1 required");
    else {
        fflog(LOG_NOTICE, "update: rebooting on request");
        sync();
        rc = schedule_reboot(1) != 0 ? reply_err(res, 500, "cannot schedule reboot")
                                     : reply_json(res, 200, "{\"rebooting\":true}");
    }
    lease_release("update:reboot");
    return rc;
}

/* Every setting back to a fresh install: the request is left in the data
 * directory (reset-requested) and the machine reboots. forgefirm-data
 * carries it out at the next boot, before anything reads the directory,
 * and keeps what an install keeps (the factory archive, the sshd host
 * keys, the TLS key and certificate, the boot state); the setup then runs
 * as after an install. It takes the machine button held, as the other
 * changes that need an operator at the machine do, an idle machine, and
 * no update job or lease. Every refusal that is not the button's comes
 * before the button is read: the operator is told to hold the button only
 * when the hold is all that is missing, and never while a job would take
 * the hold as its own press. The machine's lease is held from the check to
 * the schedule (schedule_reboot), so no update job starts in between: the
 * hold the reset takes serves no downgrade as well. */
static int system_reset_held(struct _u_response *res);

int cb_system_reset(const struct _u_request *req, struct _u_response *res,
                    void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *c = param(req, "confirm");
    if (!c || strcmp(c, "1"))
        return reply_err(res, 400, "confirm=1 required");
    char held[160];
    if (lease_take("update:reset", LEASE_SYSTEM, NULL, held, sizeof(held)) != 0)
        return reply_err(res, 409, held);
    int rc = system_reset_held(res);
    lease_release("update:reset");
    return rc;
}

static int system_reset_held(struct _u_response *res)
{
    char held[160];
    /* No job (machine_job_free): a reboot already scheduled, the kernel, a
     * job that would take the hold as its press, a connected Grbl sender. */
    if (!machine_job_free(held, sizeof(held)))
        return reply_err(res, 409, held);
    if (update_job_running())
        return reply_err(res, 409, "an update job is running");
    /* The request is the next boot's to carry out (forgefirm-data), so it
     * is only taken when that boot comes back here: the other slot set for
     * the next boot would leave it for some later boot to find. */
    {
        char inv[4096], why[96];
        if (slot_list(inv, sizeof(inv)) != 0)
            inv[0] = '\0';
        if (!slots_next_boot_returns(inv, why, sizeof(why)))
            return reply_err(res, 409, why);
    }
    /* "button" names the refusal for the panel's dialog, which stays open
     * and says the button is not held. */
    if (!operator_present())
        return reply_json(res, 403, "{\"error\":\"hold the machine button to reset every setting\","
                                    "\"button\":\"not held\"}");
    if (!panel_press_reserved(held, sizeof(held)))
        return reply_err(res, 409, held);
    char path[256], ts[32], line[64];
    snprintf(path, sizeof(path), "%s/reset-requested", ff_data_dir());
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
    int n = snprintf(line, sizeof(line), "requested %s\n", ts);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    int ok = fd >= 0 && write(fd, line, (size_t)n) == n && fsync(fd) == 0;
    if (fd >= 0)
        close(fd);
    if (!ok) {
        fflog(LOG_ERR, "update: the reset request could not be written: %s", strerror(errno));
        unlink(path);
        return reply_err(res, 500, "cannot write the reset request");
    }
    fflog(LOG_WARNING, "update: every setting is reset at the next boot; rebooting in %d s (the button was held)",
          RELEASE_DELAY_S);
    sync();
    if (schedule_reboot(RELEASE_DELAY_S) != 0) {
        unlink(path);
        return reply_err(res, 500, "cannot schedule reboot");
    }
    return reply_json(res, 200, "{\"rebooting\":true,\"reset\":true,\"release\":true}");
}

/* ------------------------------------------------------ release check */

/* The last answer of the release check, and when it was had. The
 * check runs on its own clock (rel_thread) and on request; both go
 * through release_refresh, one at a time. */
static pthread_mutex_t rel_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rel_fetch_mu = PTHREAD_MUTEX_INITIALIZER;
static struct relcheck_result rel;
static time_t rel_checked;              /* 0: never answered */

/* The protocols curl may use, a redirect's included: HTTPS alone. A host
 * test's own release server is plain HTTP on 127.0.0.1, and the test hook
 * GF_RELEASE_HTTP (testhook.h) lets curl take it in a host-test run. */
static const char *curl_proto(void)
{
    return ff_test_hook("GF_RELEASE_HTTP") ? "--proto =http,https --proto-redir =http,https"
                                           : "--proto =https --proto-redir =https";
}

/* Fetch a URL with curl: the body, raw, and the status from curl's own
 * report on the last line. Negative when curl got no answer at all. */
static int fetch_url(const char *url, char *body, size_t len, int *http)
{
    char cmd[700];
    snprintf(cmd, sizeof(cmd),
             "curl -s %s -H 'Accept: application/vnd.github+json' "
             "--max-time 20 --max-filesize %zu -w '\\n%%{http_code}' "
             "'%s' 2>/dev/null", curl_proto(), len - 1, url);
    body[0] = '\0';
    *http = 0;
    FILE *p = popen(cmd, "r");
    if (!p)
        return -1;
    size_t n = fread(body, 1, len - 1, p);
    body[n] = '\0';
    while (fgetc(p) != EOF)
        ;
    int st = pclose(p);
    if (st < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
        return -1;
    char *nl = strrchr(body, '\n');
    if (!nl)
        return -1;
    *http = atoi(nl + 1);
    *nl = '\0';
    return *http > 0 ? 0 : -1;
}

/* Run the check and keep its answer. 0 when the server answered
 * (available or not), -1 when it could not be reached (the last answer
 * stays). */
static int release_refresh(void)
{
    struct relcheck_result *fresh = calloc(1, sizeof(*fresh));
    if (!fresh)
        return -1;
    pthread_mutex_lock(&rel_fetch_mu);
    int rc = relcheck(fresh, fetch_url);
    if (rc == 0) {
        pthread_mutex_lock(&rel_mu);
        rel = *fresh;
        rel_checked = time(NULL);
        pthread_mutex_unlock(&rel_mu);
        if (fresh->available)
            fflog(LOG_INFO, "update: the latest release is %s (%s)",
                  fresh->version,
                  relcheck_is_newer(fresh->version, rel_current)
                      ? "newer than the installed version"
                      : "not newer than the installed version");
        else
            fflog(LOG_INFO, "update: release check: %s", fresh->detail);
    } else {
        fflog(LOG_NOTICE, "update: release check: the release server "
                          "could not be reached");
    }
    pthread_mutex_unlock(&rel_fetch_mu);
    free(fresh);
    return rc;
}

static void *rel_thread(void *arg)
{
    (void)arg;
    sleep(RELCHECK_FIRST_S);
    for (;;) {
        int rc = release_refresh();
        pthread_mutex_lock(&rel_mu);
        int answered = rc == 0 && (rel.http == 200 || rel.http == 404);
        pthread_mutex_unlock(&rel_mu);
        sleep(answered ? RELCHECK_DAILY_S : RELCHECK_RETRY_S);
    }
    return NULL;
}

int update_newer_release(char *out, size_t len)
{
    pthread_mutex_lock(&rel_mu);
    int newer = rel_checked && rel.available && relcheck_is_newer(rel.version, rel_current);
    snprintf(out, len, "%s", newer ? rel.version : "");
    pthread_mutex_unlock(&rel_mu);
    return newer;
}

/* The release the alert is dismissed for, "" when none. */
static void dismissed_release(char *out, size_t len)
{
    if (settings_get(DISMISSED_KEY, out, len) != 0)
        out[0] = '\0';
}

/* The reply of /update/release and /update/check: the last answer,
 * the installed version, whether the release is newer, and whether the
 * operator dismissed the alert for it. Built by the encoder, so the
 * notes and the API's detail are carried as data whatever they hold. */
static int reply_release(struct _u_response *res)
{
    char dismissed[48];
    dismissed_release(dismissed, sizeof(dismissed));
    pthread_mutex_lock(&rel_mu);
    int newer = rel.available && relcheck_is_newer(rel.version, rel_current);
    json_t *o = json_pack(
        "{s:b, s:s, s:s, s:b, s:s, s:I, s:s, s:s, s:I, s:b}",
        "available", rel.available,
        "version", rel.version,
        "current", rel_current,
        "new", newer,
        "published", rel.published,
        "bytes", (json_int_t)rel.bytes,
        "notes", rel.notes,
        "detail", rel_checked ? rel.detail : "not checked yet",
        "checked", (json_int_t)rel_checked,
        "dismissed", rel.available && !strcmp(dismissed, rel.version));
    pthread_mutex_unlock(&rel_mu);
    char *s = o ? json_dumps(o, JSON_COMPACT) : NULL;
    if (o)
        json_decref(o);
    if (!s)
        return reply_err(res, 500, "cannot encode the release");
    int rc = reply_json(res, 200, s);
    free(s);
    return rc;
}

int cb_update_release(const struct _u_request *req, struct _u_response *res,
                      void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    return reply_release(res);
}

int cb_update_check(const struct _u_request *req, struct _u_response *res,
                    void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    if (release_refresh() != 0)
        return reply_err(res, 502, "release check failed (offline?)");
    return reply_release(res);
}

int cb_update_dismiss(const struct _u_request *req, struct _u_response *res,
                      void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *v = param(req, "version");
    if (!v)
        return reply_err(res, 400, "version required (empty to undo)");
    if (*v && (!relcheck_tag_ok(v) || strlen(v) >= 48))
        return reply_err(res, 400, "version is not a release tag");
    if (settings_set(DISMISSED_KEY, v) != 0)
        return reply_err(res, 500, "cannot record the dismissal");
    return reply_release(res);
}

/* --------------------------------------------------- download worker */

/* What a download's worker is handed. A probe fetches the release's
 * acceptance record into a file of its own, so a download the operator
 * staged is never touched; the verification refuses the record, as it
 * refuses every file that is not a signed ForgeFIRM archive, and a probe
 * keeps nothing either way. A download fetches the release only when it is
 * newer than the installed version, unless the request says downgrade=1
 * (a reinstall or an older release: its install then takes downgrade=1
 * and the button held as well). */
struct dl_args {
    int probe;
    int downgrade;
};

static void *dl_worker(void *arg)
{
    struct dl_args *a = arg;
    int probe = a->probe, downgrade = a->downgrade;
    free(a);
    const char *path = probe ? DL_PROBE : DL_FW;
    char part[220];
    snprintf(part, sizeof(part), "%s" PART_SUFFIX, path);
    pthread_mutex_lock(&mu);
    job_probe = probe;
    pthread_mutex_unlock(&mu);
    /* The file of the release the last check found; a download before
     * any check runs the check first. The tag was whitelisted by the
     * check, so it is safe on the command line. */
    pthread_mutex_lock(&rel_mu);
    int known = rel.available;
    pthread_mutex_unlock(&rel_mu);
    if (!known) {
        job_set_phase("checking for a release");
        release_refresh();
    }
    char tag[48];
    pthread_mutex_lock(&rel_mu);
    known = rel.available;
    snprintf(tag, sizeof(tag), "%s", rel.version);
    pthread_mutex_unlock(&rel_mu);
    if (!known) {
        job_finish("{\"ok\":false,\"error\":\"no published release to "
                   "download\"}");
        return NULL;
    }
    /* Before the request: a release that is not newer is fetched only on
     * the operator's word, and a refused one is never counted as a
     * download. */
    if (!probe && !downgrade && !relcheck_is_newer(tag, rel_current)) {
        job_finish("{\"ok\":false,\"error\":\"release %s is not newer than the installed version "
                   "(%s): a reinstall or a downgrade takes downgrade=1\"}",
                   tag, rel_current[0] ? rel_current : "unknown");
        return NULL;
    }
    char url[256];
    if (relcheck_download_url(url, sizeof(url), tag, probe) != 0) {
        job_finish("{\"ok\":false,\"error\":\"the release's tag does not "
                   "name a file\"}");
        return NULL;
    }
    if (probe)
        fflog(LOG_NOTICE, "update: a probe download of %s", url);
    else
        fflog(LOG_NOTICE, "update: downloading release %s%s", tag,
              downgrade ? " (downgrade=1: not newer than the installed version)" : "");

    job_set_phase("downloading");
    pthread_mutex_lock(&mu);
    snprintf(progress_file, sizeof(progress_file), "%s", part);
    pthread_mutex_unlock(&mu);

    /* The file lands under a name of its own and takes the staged name
     * only once every check below passed: a download cut by a power loss,
     * or refused, never stands as a staged archive. */
    mkdir(ff_data_dir(), 0755);
    unlink(path);
    unlink(part);
    if (!probe) {
        pthread_mutex_lock(&mu);
        dl_tag[0] = '\0';
        pthread_mutex_unlock(&mu);
    }
    char cmd[768], out[512];
    snprintf(cmd, sizeof(cmd),
             "curl -fSL %s --max-time 600 --max-filesize " DL_MAX_BYTES
             " -o %s %s 2>&1", curl_proto(), part, url);
    int rc = run_cmd(out, sizeof(out), cmd);
    if (rc != 0) {
        unlink(part);
        job_finish("{\"ok\":false,\"error\":\"download failed\","
                   "\"detail\":\"%s\"}", out);
        return NULL;
    }

    job_set_phase("verifying signature");
    if (fw_classify(part) != 2) {
        unlink(part);
        job_finish("{\"ok\":false,\"error\":\"signature verification "
                   "failed - archive discarded\"}");
        return NULL;
    }
    if (probe) {
        unlink(part);
        job_finish("{\"ok\":false,\"error\":\"a probe keeps nothing - "
                   "archive discarded\"}");
        return NULL;
    }
    const char *not_fw = fwproduct_gate(part, FWCLASS_RELEASE);
    if (not_fw) {
        unlink(part);
        job_finish("{\"ok\":false,\"error\":\"%s - archive discarded\"}", not_fw);
        return NULL;
    }
    /* The archive is the release it was fetched as: a genuine signature
     * over another (an older) release served under this tag is refused. */
    char ver[48];
    fw_meta_version(part, ver, sizeof(ver));
    if (!relcheck_same_version(ver, tag)) {
        unlink(part);
        job_finish("{\"ok\":false,\"error\":\"the archive is version %s, not the release's %s - "
                   "archive discarded\"}", ver[0] ? ver : "(none)", tag);
        return NULL;
    }
    if (rename(part, DL_FW) != 0) {
        unlink(part);
        job_finish("{\"ok\":false,\"error\":\"the verified archive could not be staged\"}");
        return NULL;
    }
    pthread_mutex_lock(&mu);
    snprintf(dl_tag, sizeof(dl_tag), "%s", tag);
    pthread_mutex_unlock(&mu);
    struct stat st;
    long sz = stat(DL_FW, &st) == 0 ? (long)st.st_size : 0;
    job_finish("{\"ok\":true,\"file\":\"download\",\"version\":\"%s\","
               "\"bytes\":%ld}", ver, sz);
    return NULL;
}

int cb_update_download(const struct _u_request *req,
                       struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *p = param(req, "probe"), *d = param(req, "downgrade");
    if (p && *p && strcmp(p, "0") != 0 && strcmp(p, "1") != 0)
        return reply_err(res, 400, "probe is 1 or 0");
    if (d && *d && strcmp(d, "0") != 0 && strcmp(d, "1") != 0)
        return reply_err(res, 400, "downgrade is 1 or 0");
    struct dl_args *a = calloc(1, sizeof(*a));
    if (!a)
        return reply_err(res, 500, "out of memory");
    a->probe = p && !strcmp(p, "1");
    a->downgrade = d && !strcmp(d, "1");
    char why[160];
    int rc = job_start("download", dl_worker, a, why, sizeof(why));
    if (rc != 0)
        free(a);
    return job_start_reply(res, rc, why);
}

/* ------------------------------------------------------ apply worker */

struct apply_args {
    const struct slot_target *slot;
    char file[200];                 /* the staged archive's path */
    int from_download;              /* the release download, not an upload */
    int allow_unsigned;
    int downgrade;                  /* the operator's word for a version not newer than the installed one */
    char sha256[2 * SHA256_LEN + 1]; /* the archive the request named, or "" */
};

/* The slot guards every write asks, first in the request's answer and
 * again in the job under the update lock and the job's lease (which
 * refuses a boot selection), since the answer's judgment can be overtaken
 * meanwhile: NULL, or the refusal. */
static const char *slot_guard(const struct slot_target *t)
{
    if (is_booted_root(dev_of(t)))
        return "refusing to write the booted root slot";
    if (slot_is_next(t))
        return "refusing to write the slot selected for the next boot - "
               "point the next boot back at the running slot first";
    if (!slot_geometry_ok(t))
        return "target slot is not the 200 MiB factory geometry";
    return NULL;
}

/* The written slot verified from the media: flushed and its cache dropped
 * (slot_flush), mounted read-only with no journal replay, its version file
 * (etc/<version_file>) holding a line and its kernel there. 0 with the
 * version's first line in out; otherwise nonzero and the reason. */
static int verify_slot(const char *dev, const char *version_file, char *out, size_t len)
{
    if (slot_flush(dev) != 0) {
        snprintf(out, len, "the written slot could not be flushed to the media");
        return -1;
    }
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "mkdir -p %s && mount -o ro,noload -t ext4 %s %s 2>&1 && "
             "{ test -s %s/etc/%s && head -n 1 %s/etc/%s && test -f %s/boot/zImage; }; "
             "rc=$?; umount %s 2>/dev/null; exit $rc",
             VERIFY_DIR, dev, VERIFY_DIR, VERIFY_DIR, version_file, VERIFY_DIR, version_file,
             VERIFY_DIR, VERIFY_DIR);
    return run_cmd(out, len, cmd);
}

/* The release tag a staged download stands for: the one its download
 * verified it against, or after a restart the release the last check
 * found. "" when neither is known. */
static void download_tag(char *out, size_t len)
{
    pthread_mutex_lock(&mu);
    snprintf(out, len, "%s", dl_tag);
    pthread_mutex_unlock(&mu);
    if (out[0])
        return;
    pthread_mutex_lock(&rel_mu);
    if (rel.available)
        snprintf(out, len, "%s", rel.version);
    pthread_mutex_unlock(&rel_mu);
}

static void *apply_worker(void *argp)
{
    struct apply_args *a = argp;
    const char *dev = dev_of(a->slot);
    char out[512], cmd[700];

    fflog(LOG_NOTICE, "update: writing the %s archive into slot %s%s%s",
          a->from_download ? "downloaded" : "uploaded", a->slot->name,
          a->allow_unsigned ? " (unsigned allowed: the button was held)" : "",
          a->downgrade ? " (downgrade=1: the button was held)" : "");
    job_set_phase("taking update lock");
    if (take_lock() != 0) {
        job_finish("{\"ok\":false,\"error\":\"update lock is held "
                   "(another update in progress?)\"}");
        free(a);
        return NULL;
    }
    const char *guard = slot_guard(a->slot);
    if (guard) {
        drop_lock();
        job_finish("{\"ok\":false,\"error\":\"%s\"}", guard);
        free(a);
        return NULL;
    }

    job_set_phase("verifying archive");
    /* The archive is the one the install was asked for (and the dialog
     * showed), not one staged over it since. */
    if (a->sha256[0]) {
        char got[2 * SHA256_LEN + 1];
        if (file_sha256(a->file, got) != 0 || strcmp(got, a->sha256) != 0) {
            drop_lock();
            job_finish("{\"ok\":false,\"error\":\"the staged archive is not the one the install was asked "
                       "for: stage it again\"}");
            free(a);
            return NULL;
        }
    }
    int cls = fw_classify(a->file);
    int use_key = cls == 2;
    if (cls < 0) {
        drop_lock();
        job_finish("{\"ok\":false,\"error\":\"not a usable fwup archive\"}");
        free(a);
        return NULL;
    }
    if (a->from_download && cls != 2) {
        drop_lock();
        job_finish("{\"ok\":false,\"error\":\"the downloaded archive is not signed with the ForgeFIRM "
                   "release key - a download installs only with it\"}");
        free(a);
        return NULL;
    }
    if (cls != 2 && !a->allow_unsigned) {
        drop_lock();
        job_finish("{\"ok\":false,\"error\":\"archive is not signed with "
                   "the ForgeFIRM release key (confirm_unsigned=1 to "
                   "apply anyway)\"}");
        free(a);
        return NULL;
    }
    /* The product gate, before fwup -a: a signature says who made the
     * archive, not what it is, and fwup applies whatever task it finds. */
    const char *not_fw = fwproduct_gate(a->file, cls);
    if (not_fw) {
        drop_lock();
        job_finish("{\"ok\":false,\"error\":\"%s\"}", not_fw);
        free(a);
        return NULL;
    }
    /* The version, as the archive itself says it: a download is still the
     * release it was downloaded as, and an archive not newer than the
     * installed version installs only on the operator's word. */
    char ver[48];
    fw_meta_version(a->file, ver, sizeof(ver));
    if (a->from_download) {
        char tag[48];
        download_tag(tag, sizeof(tag));
        if (!tag[0] || !relcheck_same_version(ver, tag)) {
            drop_lock();
            job_finish("{\"ok\":false,\"error\":\"the staged download is version %s, not the release "
                       "%s: download it again\"}", ver[0] ? ver : "(none)", tag[0] ? tag : "(not known)");
            free(a);
            return NULL;
        }
    }
    if (!a->downgrade && !relcheck_is_newer(ver, rel_current)) {
        drop_lock();
        job_finish("{\"ok\":false,\"error\":\"the archive (%s) is not newer than the installed version "
                   "(%s): installing it takes downgrade=1 and the machine's button held\"}",
                   ver[0] ? ver : "no version", rel_current[0] ? rel_current : "unknown");
        free(a);
        return NULL;
    }

    job_set_phase("unmounting target");
    snprintf(cmd, sizeof(cmd),
             "for m in $(sed -n 's|^%s \\([^ ]*\\).*|\\1|p' /proc/mounts); "
             "do umount \"$m\" 2>/dev/null; done", dev);
    run_cmd(NULL, 0, cmd);

    job_set_phase("marking the slot incomplete");
    if (marker_set(a->slot) != 0) {
        drop_lock();
        job_finish("{\"ok\":false,\"error\":\"slot %s could not be marked incomplete before its "
                   "write: nothing was written\"}", a->slot->name);
        free(a);
        return NULL;
    }
    job_set_phase("writing slot");
    snprintf(cmd, sizeof(cmd),
             "fwup -a -q -d %s -i %s -t %s%s%s 2>&1",
             dev, a->file, a->slot->task,
             use_key ? " -p " : "", use_key ? KEY_RELEASE : "");
    int rc = run_write(out, sizeof(out), cmd);
    if (rc != 0) {
        drop_lock();
        job_finish("{\"ok\":false,\"error\":\"fwup apply failed - slot %s "
                   "is undefined until rewritten\",\"detail\":\"%s\"}",
                   a->slot->name, out);
        free(a);
        return NULL;
    }

    job_set_phase("verifying written slot");
    rc = verify_slot(dev, "forgefirm-version", out, sizeof(out));
    if (rc == 0)
        marker_clear(a->slot);
    drop_lock();
    if (rc != 0) {
        job_finish("{\"ok\":false,\"error\":\"written slot failed "
                   "verification\",\"detail\":\"%s\"}", out);
        free(a);
        return NULL;
    }
    job_finish("{\"ok\":true,\"slot\":\"%s\",\"version\":\"%s\","
               "\"signed\":%s}",
               a->slot->name, out, cls == 2 ? "true" : "false");
    free(a);
    return NULL;
}

int cb_update_apply(const struct _u_request *req, struct _u_response *res,
                    void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;

    const struct slot_target *t = find_target(param(req, "slot"));
    if (!t || !t->task)
        return reply_err(res, 400, "slot must be a or b");
    const char *file = param(req, "file");
    const char *path = NULL;
    if (file && !strcmp(file, "download"))
        path = DL_FW;
    else if (file && !strcmp(file, "upload"))
        path = UP_FW;
    else
        return reply_err(res, 400, "file must be download or upload");
    const char *sha = param(req, "sha256");
    if (sha && *sha && (strlen(sha) != 2 * SHA256_LEN || strspn(sha, "0123456789abcdef") != 2 * SHA256_LEN))
        return reply_err(res, 400, "sha256 is the archive's SHA-256 in lowercase hex");
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size == 0)
        return reply_err(res, 404, "staged archive not found");

    const char *cu = param(req, "confirm_unsigned"), *dg = param(req, "downgrade");
    int allow_unsigned = cu && !strcmp(cu, "1");
    int downgrade = dg && !strcmp(dg, "1");
    /* Skipping the signature check, and installing an older or the same
     * version (downgrade=1), require an operator physically at the
     * machine, not just a token: the button must be held, inside the
     * panel's reservation of it, and the request must name the archive the
     * dialog showed (its SHA-256), so the consent is to that archive. Any
     * job (machine_job_free: the job's start asks it again) refuses it
     * before the button is read, so the operator is told to hold the
     * button only when the hold is all that is missing, and never while a
     * job would take the hold as its own press. An archive not newer than the installed version is
     * refused by the job without downgrade=1 (after the signature and
     * product gates); the upload's answer and GET /slots say "newer", so
     * the panel asks for the button before it applies. */
    if (allow_unsigned || downgrade) {
        char taken[160];
        if (!machine_job_free(taken, sizeof(taken)))
            return reply_err(res, 409, taken);
        if (!operator_present())
            return reply_json(res, 403, allow_unsigned
                                  ? "{\"error\":\"hold the machine button to install unsigned firmware\","
                                    "\"button\":\"not held\"}"
                                  : "{\"error\":\"hold the machine button to install an older version\","
                                    "\"button\":\"not held\"}");
        if (!panel_press_reserved(taken, sizeof(taken)))
            return reply_err(res, 409, taken);
        if (!sha || !*sha)
            return reply_err(res, 400, "an install on the button's word names its archive: sha256 is required");
    }
    const char *guard = slot_guard(t);
    if (guard)
        return reply_err(res, 409, guard);

    struct apply_args *a = calloc(1, sizeof(*a));
    if (!a)
        return reply_err(res, 500, "out of memory");
    a->slot = t;
    snprintf(a->file, sizeof(a->file), "%s", path);
    a->from_download = path == DL_FW;
    a->allow_unsigned = allow_unsigned;
    a->downgrade = downgrade;
    snprintf(a->sha256, sizeof(a->sha256), "%s", sha ? sha : "");

    char why[160];
    int rc = job_start("apply", apply_worker, a, why, sizeof(why));
    if (rc != 0)
        free(a);
    return job_start_reply(res, rc, why);
}

/* ----------------------------------------------------------- upload */

int update_upload_sink(const struct _u_request *req, const char *key,
                       const char *filename, const char *content_type,
                       const char *transfer_encoding, const char *data,
                       uint64_t off, size_t size, void *user_data)
{
    (void)key;
    (void)filename;
    (void)content_type;
    (void)transfer_encoding;
    (void)user_data;
    if (!req->http_url || strncmp(req->http_url, "/update/upload", 14) ||
        (req->http_url[14] != '\0' && req->http_url[14] != '?'))
        return U_OK;                    /* not ours (the route itself, not a longer path): ignore */
    /* Unauthorized bytes touch nothing: not the staged archive, not an
     * authorized upload in flight. The handler answers them. */
    if (!auth_write_permitted(req))
        return U_OK;

    pthread_mutex_lock(&up_mu);
    if (off == 0) {
        /* One upload at a time: a second one while the first still
         * streams is refused, not adopted. An upload whose sender went
         * away mid-stream is abandoned after a minute of silence. */
        if (up_fp && up_owner != req && mono_s() - up_last < 60) {
            pthread_mutex_unlock(&up_mu);
            return U_OK;                /* the handler reports the refusal */
        }
        if (up_fp) {
            fclose(up_fp);              /* bound any leak from a prior upload */
            up_fp = NULL;
        }
        up_owner = req;
        up_refusal[0] = '\0';
        up_opened = 0;
        /* Gate the flash-staging write: machine idle, and no diagnostic
         * or update job running (the apply worker reads the same file -
         * an ungated upload could truncate it mid-flash). */
        if (lease_refusal(up_refusal, sizeof(up_refusal)))
            ;                           /* the holder, in words */
        else if (!machine_is_idle())
            snprintf(up_refusal, sizeof(up_refusal), "the machine is not idle");
        if (up_refusal[0]) {
            up_error = 1;
            up_bytes = 0;
        } else {
            mkdir(ff_data_dir(), 0755);
            up_fp = fopen(UP_FW, "wb");
            up_bytes = 0;
            up_error = up_fp ? 0 : 1;
            up_opened = up_fp != NULL;
            if (up_error)
                snprintf(up_refusal, sizeof(up_refusal), "cannot open the staging file");
        }
    }
    if (up_fp && up_owner == req && !up_error) {
        up_last = mono_s();
        if (up_bytes + size > UPLOAD_MAX) {
            up_error = 1;
            snprintf(up_refusal, sizeof(up_refusal), "the archive exceeds the size limit");
        } else if (size && fwrite(data, 1, size, up_fp) != size) {
            up_error = 1;
            snprintf(up_refusal, sizeof(up_refusal), "writing the staging file failed");
        } else {
            up_bytes += size;
        }
    }
    pthread_mutex_unlock(&up_mu);
    return U_OK;
}

int cb_update_upload(const struct _u_request *req, struct _u_response *res,
                     void *user_data)
{
    (void)user_data;
    /* The sink staged nothing for an unauthorized request; the staged
     * archive and any authorized upload in flight are not this
     * request's to discard. */
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;

    pthread_mutex_lock(&up_mu);
    if (up_fp && up_owner != req) {
        /* Another upload is streaming: this one was refused at its first
         * chunk and staged nothing. */
        pthread_mutex_unlock(&up_mu);
        return reply_err(res, 409, "another upload is in progress");
    }
    if (up_fp) {
        fclose(up_fp);
        up_fp = NULL;
    }
    /* What the sink did for this request: a request it refused at its
     * first chunk opened nothing, and the staged archive (which an apply
     * job may be reading) is not its to discard. */
    int mine = up_owner == req, opened = mine && up_opened;
    up_owner = NULL;
    up_opened = 0;
    int err = mine ? up_error : 1;
    uint64_t bytes = mine ? up_bytes : 0;
    char why[80];
    snprintf(why, sizeof(why), "%s", mine && up_refusal[0] ? up_refusal : "no file data received");
    pthread_mutex_unlock(&up_mu);

    if (err || bytes == 0) {
        if (opened)
            unlink(UP_FW);
        fflog(LOG_NOTICE, "update: an upload was refused: %s", why);
        return reply_err(res, 400, why);
    }

    int cls = fw_classify(UP_FW);
    if (cls < 0) {
        unlink(UP_FW);
        fflog(LOG_NOTICE, "update: an upload was refused: not a usable fwup archive");
        return reply_err(res, 400, "not a usable fwup archive");
    }
    const char *not_fw = fwproduct_gate(UP_FW, cls);
    if (not_fw) {
        unlink(UP_FW);
        fflog(LOG_NOTICE, "update: an upload was refused: %s", not_fw);
        return reply_err(res, 400, not_fw);
    }
    /* What the install dialog shows and the install names again: the
     * version against the installed one, and the archive's SHA-256. */
    struct staged_info si = { 0 };
    if (staged_read(1, UP_FW, &si) != 0) {
        unlink(UP_FW);
        return reply_err(res, 500, "the staged archive cannot be read back");
    }
    int newer = relcheck_is_newer(si.version, rel_current);
    fflog(LOG_NOTICE, "update: an upload staged version %s (%llu bytes, signature %s%s)",
          si.version[0] ? si.version : "unknown", (unsigned long long)bytes,
          cls == 2 ? "forgefirm" : cls == 1 ? "glowforge" : "none",
          newer ? "" : "; not newer than the installed version");
    json_t *o = json_pack("{s:b, s:s, s:I, s:s, s:s, s:b, s:s, s:s}", "ok", 1, "file", "upload",
                          "bytes", (json_int_t)bytes, "version", si.version,
                          "signature", cls == 2 ? "forgefirm" : cls == 1 ? "glowforge" : "unsigned",
                          "newer", newer, "installed", rel_current, "sha256", si.sha256);
    char *s = o ? json_dumps(o, JSON_COMPACT) : NULL;
    json_decref(o);
    int rc = s ? reply_json(res, 200, s) : reply_err(res, 500, "out of memory");
    free(s);
    return rc;
}

/* --------------------------------------------------- factory restore */

struct restore_args {
    const struct slot_target *slot;
    char file[160];                  /* archive basename */
};

/* Write a factory archive into a slot and verify it. Runs the phases on
 * the current job; on failure the job is finished with the reason and
 * -1 is returned. On success `version` holds the restored factory
 * version and the job is still running for the caller to finish. */
static int restore_run(const struct slot_target *slot, const char *file,
                       char *version, size_t vlen)
{
    char cmd[640], out[512];
    const char *dev = dev_of(slot);

    job_set_phase("taking update lock");
    if (take_lock() != 0) {
        job_finish("{\"ok\":false,\"error\":\"update lock is held\"}");
        return -1;
    }
    const char *guard = slot_guard(slot);
    if (guard) {
        drop_lock();
        job_finish("{\"ok\":false,\"error\":\"%s\"}", guard);
        return -1;
    }

    /* The manifest's md5 protects a years-old archive against bit rot
     * before it overwrites a slot. */
    job_set_phase("verifying archive checksum");
    snprintf(cmd, sizeof(cmd),
             "M=$(sed -n 's|.* %s md5=\\([0-9a-f]*\\)$|\\1|p' "
             "%s/manifest | head -n 1); "
             "[ -n \"$M\" ] || exit 2; "
             "echo \"$M  %s/%s\" | md5sum -c - >/dev/null 2>&1",
             file, ARCHIVE_DIR, ARCHIVE_DIR, file);
    int rc = run_cmd(out, sizeof(out), cmd);
    if (rc == 2) {
        drop_lock();
        job_finish("{\"ok\":false,\"error\":\"archive not in manifest\"}");
        return -1;
    }
    if (rc != 0) {
        drop_lock();
        job_finish("{\"ok\":false,\"error\":\"archive checksum MISMATCH - "
                   "not restoring from a corrupt archive\"}");
        return -1;
    }

    job_set_phase("unmounting target");
    snprintf(cmd, sizeof(cmd),
             "for m in $(sed -n 's|^%s \\([^ ]*\\).*|\\1|p' /proc/mounts); "
             "do umount \"$m\" 2>/dev/null; done", dev);
    run_cmd(NULL, 0, cmd);

    job_set_phase("marking the slot incomplete");
    if (marker_set(slot) != 0) {
        drop_lock();
        job_finish("{\"ok\":false,\"error\":\"slot %s could not be marked incomplete before its "
                   "write: nothing was written\"}", slot->name);
        return -1;
    }
    job_set_phase("writing factory image");
    /* gzip writes the slot itself, so the exit status is the write's
     * whole story (no pipeline to hide a failed stage). The archive name
     * is charset-restricted at the endpoint. */
    snprintf(cmd, sizeof(cmd), "gzip -dc %s/%s 2>&1 >%s", ARCHIVE_DIR, file, dev);
    rc = run_write(out, sizeof(out), cmd);
    if (rc != 0) {
        drop_lock();
        job_finish("{\"ok\":false,\"error\":\"restore write failed - "
                   "slot %s is undefined until rewritten\","
                   "\"detail\":\"%s\"}", slot->name, out);
        return -1;
    }

    job_set_phase("verifying written slot");
    rc = verify_slot(dev, "version", out, sizeof(out));
    if (rc == 0)
        marker_clear(slot);
    drop_lock();
    if (rc != 0) {
        job_finish("{\"ok\":false,\"error\":\"restored slot failed "
                   "verification\",\"detail\":\"%s\"}", out);
        return -1;
    }
    snprintf(version, vlen, "%s", out);
    return 0;
}

static void *restore_worker(void *argp)
{
    struct restore_args *a = argp;
    char version[512];
    fflog(LOG_NOTICE, "update: restoring the factory archive %s into slot %s",
          a->file, a->slot->name);
    if (restore_run(a->slot, a->file, version, sizeof(version)) == 0)
        job_finish("{\"ok\":true,\"slot\":\"%s\",\"factory_version\":\"%s\"}",
                   a->slot->name, version);
    free(a);
    return NULL;
}

/* --------------------------------------------- return to the factory */

/* The setup's exit, and a one-press way back at any time: the machine
 * goes back to the factory firmware it ran before ForgeFIRM. If the slot
 * that is not running still holds a factory image, only the boot
 * selection moves; if an update reused it, the newest archived factory
 * image is restored into it first. Then the machine reboots. A reinstall
 * of ForgeFIRM is the install process again, from the console; the
 * archive and ForgeFIRM's files under /data stay. It takes the machine's
 * button held inside the panel's reservation, as every change does that
 * needs an operator at the machine: the factory firmware it boots may
 * update itself over the other slot, which ends ForgeFIRM on the machine
 * until a console reinstall. */
struct return_args {
    const struct slot_target *slot;
    const struct slot_target *booted;   /* the slot running now, to point the next boot back at */
    char file[160];                 /* archive to restore, or "" */
};

/* The next boot pointed at a slot: 0, or -1 with ffboot's words in out. */
static int select_next(const struct slot_target *t, char *out, size_t len)
{
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "%s %s -n 2>&1", ffboot_path(), t->ffboot_arg);
    return run_cmd(out, len, cmd) == 0 ? 0 : -1;
}

static void *factory_return_worker(void *argp)
{
    struct return_args *a = argp;
    char version[512] = "", out[512], why[160];

    if (a->file[0])
        fflog(LOG_NOTICE, "update: returning to the factory firmware: restoring %s into "
              "slot %s, then booting it", a->file, a->slot->name);
    else
        fflog(LOG_NOTICE, "update: returning to the factory firmware in slot %s",
              a->slot->name);
    if (a->file[0]) {
        /* The slot the restore writes must not be the next boot (a cut
         * write there has no revert behind it): an earlier selection that
         * was never booted is pointed back at the running slot first. */
        if (slot_is_next(a->slot)) {
            job_set_phase("pointing the next boot back at the running slot");
            if (select_next(a->booted, out, sizeof(out)) != 0) {
                job_finish("{\"ok\":false,\"error\":\"the next boot could not be pointed back at the "
                           "running slot before the restore: nothing was written\",\"detail\":\"%s\"}", out);
                free(a);
                return NULL;
            }
        }
        /* A job started meanwhile keeps the machine: nothing is written
         * under it. */
        if (!machine_job_free(why, sizeof(why))) {
            job_finish("{\"ok\":false,\"error\":\"%s: nothing was written; return again once the "
                       "machine is free\"}", why);
            free(a);
            return NULL;
        }
        if (restore_run(a->slot, a->file, version, sizeof(version)) != 0) {
            free(a);
            return NULL;            /* restore_run finished the job */
        }
    }

    /* The selection and the reboot only while no job runs: one started
     * during the restore is not cut by the reboot. */
    if (!machine_job_free(why, sizeof(why))) {
        job_finish("{\"ok\":false,\"error\":\"%s: the next boot was not moved; return again once "
                   "the machine is free\"}", why);
        free(a);
        return NULL;
    }
    job_set_phase("selecting the factory slot for the next boot");
    if (select_next(a->slot, out, sizeof(out)) != 0) {
        job_finish("{\"ok\":false,\"error\":\"boot selection failed\","
                   "\"detail\":\"%s\"}", out);
        free(a);
        return NULL;
    }
    /* Asked once more after the selection: a job that came up during it
     * puts the next boot back on the running slot, and nothing reboots. */
    if (!machine_job_free(why, sizeof(why))) {
        char back[512];
        int moved = select_next(a->booted, back, sizeof(back)) == 0;
        job_finish("{\"ok\":false,\"error\":\"%s: %s; return again once the machine is free\"}", why,
                   moved ? "the next boot is the running slot again"
                         : "the next boot could not be moved back, and the factory slot is the next boot");
        free(a);
        return NULL;
    }

    job_set_phase("rebooting into the factory firmware");
    sync();
    if (schedule_reboot(RELEASE_DELAY_S) != 0) {
        job_finish("{\"ok\":false,\"error\":\"cannot schedule the reboot\"}");
        free(a);
        return NULL;
    }
    job_finish("{\"ok\":true,\"slot\":\"%s\",\"restored\":%s,"
               "\"factory_version\":\"%s\",\"rebooting\":true}",
               a->slot->name, a->file[0] ? "true" : "false", version);
    free(a);
    return NULL;
}

/* The slot the factory return writes or selects, among a and b, with what
 * it holds per ffboot ("factory", "forgefirm", or ""): the one that is not
 * the booted root. Booted from the card, both are candidates: the one
 * that holds a factory image, else the one the request names (named,
 * "a" or "b"); a request that names neither then is refused, since a
 * restore over a ForgeFIRM slot is the operator's to choose. NULL with the
 * refusal in why (and *status its HTTP status). */
static const struct slot_target *return_slot(const char *named, char *type, size_t tlen,
                                             char *why, size_t wlen, unsigned *status)
{
    char raw[4096];
    *status = 409;
    if (slot_list(raw, sizeof(raw)) != 0) {
        *status = 500;
        snprintf(why, wlen, "cannot read the slot inventory");
        return NULL;
    }
    const struct slot_target *cand[2];
    char ctype[2][16];
    int n = 0;
    for (size_t t = 0; t < N_TARGETS && n < 2; t++) {
        if (!targets[t].task || is_booted_root(dev_of(&targets[t])))
            continue;
        char key[48];
        snprintf(key, sizeof(key), "slot.%s.type", targets[t].name);
        inv_get(raw, key, ctype[n], sizeof(ctype[n]));
        /* A slot an update left marked incomplete holds no factory image,
         * whatever its probe reads: it is restored, never just selected
         * (ffboot refuses to select it). */
        if (slot_marked(&targets[t]))
            ctype[n][0] = '\0';
        cand[n++] = &targets[t];
    }
    int pick = -1;
    if (named && *named) {
        for (int i = 0; i < n; i++)
            if (!strcmp(cand[i]->name, named))
                pick = i;
        if (pick < 0) {
            snprintf(why, wlen, "slot %s is not one the factory firmware can be returned to (the running "
                     "slot, or no slot of this machine)", named);
            return NULL;
        }
    } else if (n == 1) {
        pick = 0;
    } else if (n == 2) {
        pick = !strcmp(ctype[0], "factory") ? 0 : !strcmp(ctype[1], "factory") ? 1 : -1;
        if (pick < 0) {
            snprintf(why, wlen, "booted from the card, and neither slot holds the factory firmware: name the "
                     "slot to restore it into (slot=a or slot=b)");
            return NULL;
        }
    } else {
        *status = 500;
        snprintf(why, wlen, "no slot to return to: the running slot cannot be told");
        return NULL;
    }
    snprintf(type, tlen, "%.15s", ctype[pick]);
    return cand[pick];
}

/* The newest factory archive by its build date in the name. */
static int newest_archive(char *out, size_t len)
{
    out[0] = '\0';
    DIR *d = opendir(ARCHIVE_DIR);
    if (!d)
        return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "factory-rootfs-", 15))
            continue;
        int ok = 1;
        for (const char *c = e->d_name; *c; c++)
            if (!isalnum((unsigned char)*c) && *c != '.' && *c != '_' && *c != '-')
                ok = 0;
        if (!ok || strstr(e->d_name, ".."))
            continue;
        if (!out[0] || strcmp(e->d_name, out) > 0)
            snprintf(out, len, "%.159s", e->d_name);
    }
    closedir(d);
    return out[0] ? 0 : -1;
}

int cb_restore_factory_return(const struct _u_request *req,
                              struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *c = param(req, "confirm");
    if (!c || strcmp(c, "1"))
        return reply_err(res, 400, "confirm=1 required");
    char held[160];
    if (lease_refusal(held, sizeof(held)))
        return reply_err(res, 409, held);
    /* No job, before the button is read: a job that waits for its press
     * would take the hold as its own. */
    if (!machine_job_free(held, sizeof(held)))
        return reply_err(res, 409, held);

    char type[16];
    unsigned status;
    const struct slot_target *t = return_slot(param(req, "slot"), type, sizeof(type), held, sizeof(held), &status);
    if (!t)
        return reply_err(res, status, held);
    const struct slot_target *booted = booted_target();
    if (!booted)
        return reply_err(res, 500, "the running slot cannot be told, so the next boot could not be put back on it");
    if (!slot_geometry_ok(t))
        return reply_err(res, 409,
                         "the other slot is not the 200 MiB factory geometry");

    struct return_args *a = calloc(1, sizeof(*a));
    if (!a)
        return reply_err(res, 500, "out of memory");
    a->slot = t;
    a->booted = booted;
    if (strcmp(type, "factory") != 0) {
        if (newest_archive(a->file, sizeof(a->file)) != 0) {
            free(a);
            return reply_err(res, 409,
                "no archived factory image on this machine - use the "
                "factory recovery mode instead");
        }
    }
    /* The operator at the machine: the button held inside the panel's
     * reservation of it, after every other refusal, so the hold is asked
     * for only when it is all that is missing. */
    if (!operator_present()) {
        free(a);
        return reply_json(res, 403, "{\"error\":\"hold the machine button to go back to the factory firmware\","
                                    "\"button\":\"not held\"}");
    }
    if (!panel_press_reserved(held, sizeof(held))) {
        free(a);
        return reply_err(res, 409, held);
    }
    char why[160];
    int rc = job_start("factory-return", factory_return_worker, a, why, sizeof(why));
    if (rc != 0)
        free(a);
    return job_start_reply(res, rc, why);
}

int cb_restore_factory(const struct _u_request *req,
                       struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;

    const char *source = param(req, "source");
    if (source && !strcmp(source, "cloud"))
        return reply_err(res, 501,
            "cloud restore is not implemented yet - use an archive");

    const struct slot_target *t = find_target(param(req, "slot"));
    if (!t || !t->task)
        return reply_err(res, 400, "slot must be a or b");
    const char *guard = slot_guard(t);
    if (guard)
        return reply_err(res, 409, guard);

    const char *file = param(req, "file");
    if (!file || strncmp(file, "factory-rootfs-", 15))
        return reply_err(res, 400,
                         "file must be a factory-rootfs archive name");
    /* Charset-restrict the name: it is interpolated into a shell
     * command line, so allow only the characters a real archive name
     * uses and reject path traversal and every shell metacharacter. */
    for (const char *p = file; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' &&
            *p != '-')
            return reply_err(res, 400, "invalid archive name");
    if (strstr(file, ".."))
        return reply_err(res, 400, "invalid archive name");
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", ARCHIVE_DIR, file);
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size == 0)
        return reply_err(res, 404, "archive not found");

    struct restore_args *a = calloc(1, sizeof(*a));
    if (!a)
        return reply_err(res, 500, "out of memory");
    a->slot = t;
    snprintf(a->file, sizeof(a->file), "%s", file);

    char why[160];
    int rc = job_start("restore", restore_worker, a, why, sizeof(why));
    if (rc != 0)
        free(a);
    return job_start_reply(res, rc, why);
}

/* ------------------------------------------------------------ status */

int cb_update_status(const struct _u_request *req, struct _u_response *res,
                     void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char body[1280];
    pthread_mutex_lock(&mu);
    long prog = -1;
    if (job_running && progress_file[0]) {
        struct stat st;
        if (stat(progress_file, &st) == 0)
            prog = (long)st.st_size;
    }
    snprintf(body, sizeof(body),
             "{\"running\":%s,\"job\":%u,\"kind\":\"%s\",\"phase\":\"%s\","
             "\"elapsed\":%ld,\"progress_bytes\":%ld,\"result\":%s}",
             job_running ? "true" : "false", job_seq, job_kind, job_phase,
             job_running ? (long)(time(NULL) - job_started) : 0, prog,
             job_result[0] ? job_result : "null");
    pthread_mutex_unlock(&mu);
    return reply_json(res, 200, body);
}

/* -------------------------------------------------------------- init */

void update_init(void)
{
    const char *dd = ff_data_dir();
    snprintf(archive_dir, sizeof(archive_dir), "%s/archive", dd);
    snprintf(dl_fw, sizeof(dl_fw), "%s/download.fw", dd);
    snprintf(dl_probe, sizeof(dl_probe), "%s/download-probe", dd);
    snprintf(up_fw, sizeof(up_fw), "%s/upload.fw", dd);
    snprintf(lock_file, sizeof(lock_file), "%s/update.lock", dd);
    snprintf(verify_dir, sizeof(verify_dir), "%s/ffverify", ff_run_dir());
    mkdir(dd, 0755);
    /* The slots' devices: the machine's, or the slot seam's files. */
    const char *sd = slot_dir();
    for (size_t i = 0; i < N_TARGETS; i++) {
        const char *base = strrchr(targets[i].dev, '/');
        if (sd)
            snprintf(target_dev[i], sizeof(target_dev[i]), "%s/%s", sd, base ? base + 1 : targets[i].dev);
        else
            snprintf(target_dev[i], sizeof(target_dev[i]), "%s", targets[i].dev);
    }
    /* A stale lock file from a crash is harmless: flock state dies with
     * the holder. A download cut by a power loss is left under its own
     * name, never the staged one, and goes now. */
    char part[220];
    snprintf(part, sizeof(part), "%s" PART_SUFFIX, dl_fw);
    unlink(part);

    /* The installed version, read once: the file lives on the rootfs
     * and does not change while the daemon runs. A host test names its
     * own (GF_VERSION_FILE, testhook.h). */
    const char *vf = ff_test_hook("GF_VERSION_FILE");
    FILE *f = fopen(vf ? vf : VERSION_FILE, "r");
    if (f) {
        char line[96] = "";
        if (fgets(line, sizeof(line), f))
            jsan(rel_current, sizeof(rel_current), line);
        fclose(f);
    }
    /* Every start records what runs, so the log shows each change of
     * firmware, by whatever route it came (the panel, or fwup and ffboot
     * at the console). */
    const char *slot = booted_slot();
    fflog(LOG_NOTICE, "update: running %s from slot %s (root=%s)",
          rel_current[0] ? rel_current : "an unknown version",
          slot[0] ? slot : "unknown", booted_root());
    for (size_t i = 0; i < N_TARGETS; i++)
        if (targets[i].task && slot_marked(&targets[i]))
            fflog(LOG_WARNING, "update: slot %s holds an update that did not complete: it is not "
                               "selected for a boot until it is written again", targets[i].name);

    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, rel_thread, NULL) != 0)
        fflog(LOG_ERR, "update: cannot start the release check; only "
                       "a manual check will run");
    pthread_attr_destroy(&at);
}

void update_shutdown(void)
{
    pthread_mutex_lock(&wr_mu);
    wr_stopping = 1;
    pid_t pg = wr_pgid;
    pthread_mutex_unlock(&wr_mu);
    if (pg <= 0)
        return;
    /* A slot write is ended rather than left to run on without the
     * daemon: its slot keeps the incomplete mark, so no boot selects it
     * until it is written again. TERM, then KILL after 2 s. */
    fflog(LOG_WARNING, "update: the daemon stops during a slot write: the write is ended, and the "
                       "slot stays marked incomplete");
    kill(-pg, SIGTERM);
    for (int i = 0; i < 20 && kill(-pg, 0) == 0; i++)
        usleep(100000);
    kill(-pg, SIGKILL);
}
