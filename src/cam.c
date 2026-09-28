/*
 * cam.c - persistent Glowforge camera capture engine
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Pipeline model (mainline imx-media): both sensors feed one video-mux ->
 * MIPI CSI-2 -> IPU CSI path to the 'ipu1_csi0 capture' video node. Camera
 * selection is which mux sink link is enabled; sensor controls live on the
 * sensor subdev; illumination is the per-camera LED driven via sysfs. Links
 * and pad formats are configured with media-ctl / v4l2-ctl (the same
 * sequences python3-gfhardware uses for one-shot grabs), then the capture
 * node is held open and streamed continuously.
 *
 * Two sensors ship in the field - the 5 MP OV5648 and, in "HD" machines, the
 * 8 MP OV8856 - and they differ in geometry, bit depth and control set, so
 * everything downstream of the media graph is driven from the sensor profile
 * that matches whichever driver bound (see sensor_profiles).
 *
 * PRIVACY GATE: the lid camera never captures unless the lid is closed:
 * an open lid points it into the room. The head camera looks at the bed
 * however the lid stands, and a local viewer (the panel, or an extension
 * package through the host) may use it with the lid open; every other
 * requester - in cloud mode the image request comes from a remote service -
 * keeps the lid rule for both. The check lives here, at the one process
 * that owns the capture path, rather than at each caller: the engine
 * refuses to start, a lid that opens mid-capture stops it (unless a local
 * viewer has the head camera, when only the others' streams end), and the
 * check fails closed (see machine_lid_closed()).
 *
 * Threading: a control mutex serializes engine start/stop/switch; the
 * engine lock covers frame data and counters. The worker thread only ever
 * takes the engine lock, so control paths may join it while holding the
 * control mutex.
 */
#define _GNU_SOURCE
#include "cam.h"
#include "camhealth.h"
#include "debayer.h"
#include "fflog.h"
#include "gpu_debayer.h"
#include "ipu_copy.h"
#include "mp4mux.h"
#include "settings.h"
#include "status.h"
#include "vpu_h264.h"
#include "vpu_jpeg.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <jpeglib.h>
#include <setjmp.h>    /* requires stdio.h first (FILE) */
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define HFLIP  1          /* factory image orientation (see debayer.h) */

#define N_BUFS          4
#define DQ_TIMEOUT_S    2   /* select() timeout per frame; also the idle tick */
#define MAX_DQ_TIMEOUTS 3   /* consecutive timeouts -> engine gives up */
#define IDLE_STOP_S     10  /* no clients/snapshots for this long -> teardown */
#define LAMP_SKIP_FRAMES 3  /* frames discarded after a lamp change (in-flight
                             * exposures predate the new scene lighting) */
#define SNAP_TIMEOUT_S  15
#define CLIENT_WAIT_S   5
#define SWITCH_GRACE_S  3   /* wait this long for clients to drain on switch */

#define CAPTURE_ENTITY "ipu1_csi0 capture"

struct camdef {
    const char *name;
    int         bus;       /* I2C bus: sensor entity resolved by <bus>-0036 */
    int         muxpad;    /* video-mux sink pad */
    const char *lamp;      /* sysfs illumination attribute */
};

static const struct camdef camdefs[2] = {
    [CAM_LID]  = { "lid",  0, 0, "/sys/glowforge/pic/lid_led"    },
    [CAM_HEAD] = { "head", 3, 1, "/sys/glowforge/head/white_led" },
};

/* ------------------------------------------------------ sensor profiles */

/* Everything that differs between the sensors the machine ships with. The
 * media entity name carries the driver that bound ("ov5648 0-0036" /
 * "ov8856 0-0036"), so the profile follows the hardware without a build-time
 * switch: one image covers both. */
struct sensor_profile {
    const char *driver;     /* media entity name prefix */
    const char *model;      /* reported by /cam/status */
    int         w, h;       /* raw frame the pipeline is configured for */
    int         fps;        /* the mode's sensor frame rate */
    const char *mbus;       /* media-ctl pad format, geometry appended */
    uint32_t    pixfmt;     /* V4L2 pixel format at the capture node */
    int         bpp;        /* bytes per raw sample at the capture node */
    int         shift;      /* right shift from sample to 8-bit value */
    /* Manual exposure/gain/white balance on the sensor subdev. */
    int       (*ctrls)(const char *subdev, cam_id_t cam);
};

static int ctrls_ov5648(const char *subdev, cam_id_t cam);
static int ctrls_ov8856(const char *subdev, cam_id_t cam);

static const struct sensor_profile sensor_profiles[] = {
    {
        /* 5 MP, 8-bit Bayer straight out of the CSI. */
        .driver = "ov5648", .model = "OV5648",
        .w = 2592, .h = 1944, .fps = 15,
        .mbus = "SBGGR8_1X8", .pixfmt = V4L2_PIX_FMT_SBGGR8,
        .bpp = 1, .shift = 0, .ctrls = ctrls_ov5648,
    },
    {
        /* 8 MP "HD" modules, full frame. The sensor's RAW10 full-resolution
         * 2-lane mode asks for 1.44 Gbps/lane and the i.MX6 CSI-2 D-PHY
         * stops at 1 Gbps, but its RAW8 one carries the same frame at half
         * that - so the capture word, the Bayer order and the whole
         * downstream path are the OV5648's, only larger.
         * UNPROVEN: no 8 MP machine has been on the bench. */
        .driver = "ov8856", .model = "OV8856",
        .w = 3264, .h = 2448, .fps = 15,
        .mbus = "SBGGR8_1X8", .pixfmt = V4L2_PIX_FMT_SBGGR8,
        .bpp = 1, .shift = 0, .ctrls = ctrls_ov8856,
    },
};

#define N_PROFILES ((int)(sizeof(sensor_profiles) / sizeof(sensor_profiles[0])))

/* Worst case over every profile, so the worker's scratch buffers are sized
 * once and survive a snapshot borrow of a differently-modeled camera. */
static size_t max_raw8_bytes(void)
{
    size_t m = 0;
    for (int i = 0; i < N_PROFILES; i++) {
        size_t n = (size_t)sensor_profiles[i].w * sensor_profiles[i].h;
        if (n > m)
            m = n;
    }
    return m;
}

static size_t max_half_rgb_bytes(void)
{
    size_t m = 0;
    for (int i = 0; i < N_PROFILES; i++) {
        size_t n = (size_t)(sensor_profiles[i].w / 2) *
                   (sensor_profiles[i].h / 2) * 3;
        if (n > m)
            m = n;
    }
    return m;
}

/* "ov8856 3-0036" -> the OV8856 profile. NULL for a driver we have no
 * profile for (the engine then refuses to start rather than guess). */
static const struct sensor_profile *profile_for(const char *entity)
{
    for (int i = 0; i < N_PROFILES; i++) {
        size_t n = strlen(sensor_profiles[i].driver);
        if (!strncmp(entity, sensor_profiles[i].driver, n) &&
            (entity[n] == ' ' || entity[n] == '\0'))
            return &sensor_profiles[i];
    }
    return NULL;
}

struct buffer {
    void  *start;
    size_t length;
    int    dmabuf;      /* exported for the GPU import; -1 until then */
};

static struct {
    /* control path (start/stop/switch) - taken before lock, never by the
     * worker thread */
    pthread_mutex_t ctl;

    /* engine state */
    pthread_mutex_t lock;
    pthread_cond_t  frame_cv;   /* new stream frame published */
    pthread_cond_t  snap_cv;    /* snapshot request completed */
    pthread_t       tid;
    int             tid_valid;
    int             running;    /* worker alive and capturing */
    int             stop_flag;
    cam_id_t        cam;        /* camera the pipeline is configured for
                                 * RIGHT NOW (a borrow flips it briefly) */
    cam_id_t        home_cam;   /* camera the engine serves for streaming -
                                 * what arbitration must compare against */
    /* Sensor that bound on each camera's I2C bus, last time one was
     * resolved; NULL until then. `prof` is the profile the pipeline is
     * configured for RIGHT NOW (it follows eng.cam through a borrow). */
    const struct sensor_profile *seen[2];
    const struct sensor_profile *prof;
    int             clients;
    uint64_t        kick_gen;   /* bumped to preempt all current stream
                                 * clients (their streams end cleanly) */
    struct timespec last_activity;

    /* published stream frame (half-res JPEG) */
    uint8_t        *stream_jpg;
    size_t          stream_len;
    size_t          stream_cap;
    uint64_t        seq;
    double          fps;

    /* published H.264 access unit (worker writes, H.264 clients read) */
    pthread_cond_t  h264_cv;
    uint8_t        *h264_au;
    size_t          h264_len;
    uint64_t        h264_seq;
    uint64_t        h264_pts;   /* 90 kHz, CLOCK_MONOTONIC based */
    int             h264_key;
    int             h264_clients;
    int             h264_up;        /* encoder open and delivering */
    int             h264_key_req;   /* a joining client needs an IDR */

    /* one pending snapshot request at a time (control mutex serializes).
     * snap_cam may differ from the streaming camera: the worker then
     * "borrows" the mux - pauses the stream, switches, grabs one frame,
     * switches back (stream clients see a few-second freeze). */
    int             snap_pending;   /* 1 = requested, 2 = done, 3 = failed */
    cam_id_t        snap_cam;
    int             snap_full;
    int             snap_quality;
    int             snap_lamp;      /* per-shot lamp override, -1 = default */
    uint8_t        *snap_jpg;       /* malloc'd result, taken by requester */
    size_t          snap_len;

    /* capture resources (worker/start/teardown only) */
    int             fd;
    struct buffer   bufs[N_BUFS];
    int             n_bufs;
    int             streaming;
    int             lid_stopped;    /* the worker exited on an open lid;
                                     * cleared when an engine start
                                     * succeeds (which needs a closed lid) */
    int             lamp_prev;      /* -1 = unknown, restore to 0 */
    int             head_open_ok;   /* a local viewer has the head camera: it may run with the lid open */
    int             snap_local;     /* the pending snapshot's requester is a local viewer */
    int             snap_lid;       /* the pending snapshot was refused for the lid */
    int             stream_lamp;    /* a stream client's lamp level, -1 = the engine's */
    int             cached_bufs;    /* capture mmaps are CPU-cached
                                     * (non-coherent); no bounce copy */
    /* Frame-health ladder (camhealth.h). Like the capture resources above
     * it is written before the worker exists or by the worker itself. */
    struct cam_health health;

    /* Frame health totals, for /cam/status and the logs. Cumulative since
     * the daemon started, so a machine with a marginal camera cable shows
     * up as a nonzero corrupt count days later. */
    uint64_t        frames;         /* dequeued */
    uint64_t        corrupt;        /* of those, flagged errored */
    unsigned        recoveries;     /* stream restarts */
    uint64_t        withheld;       /* unpublished: a neighbor was flagged */
    uint64_t        skipped;        /* passed over for a newer frame */

    /* Per-stage means for /cam/status, refreshed with the fps window. */
    double          t_latency, t_convert, t_copy, t_encode;

    /* config */
    int             stream_quality;
    int             lamp_level;
    double          fps_cap;        /* stream frames/s ceiling; 0 = sensor max */
    int             h264_kbps;
    int             h264_gop;
    int             vpu_active;     /* last stream frame went through the VPU */
    int             gpu_active;     /* last stream frame demosaiced on the GPU */
    int             hw_skip;        /* CSI frame skip realizes the fps cap */
} eng = {
    .ctl = PTHREAD_MUTEX_INITIALIZER,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .frame_cv = PTHREAD_COND_INITIALIZER,
    .snap_cv = PTHREAD_COND_INITIALIZER,
    .h264_cv = PTHREAD_COND_INITIALIZER,
    .fd = -1,
    .lamp_prev = -1,
    .stream_lamp = -1,
    .stream_quality = 75,
    .lamp_level = 132,
    .h264_kbps = 1500,
    .h264_gop = 30,
};

const char *cam_name(cam_id_t cam)
{
    return camdefs[cam].name;
}


/* CODA960 hardware JPEG encoder for the stream path; libjpeg remains the
 * fallback (and the snapshot path). Worker-thread use only. */
static vpu_jpeg_t *vpu;
static int vpu_disabled;

/* CODA960 H.264 encoder (the BIT processor, independent of the JPEG
 * unit), plus the engine-held parameter-set cache viewers that join
 * late take their SPS/PPS from. Worker-thread use only. */
static vpu_h264_t *h264;
static int h264_disabled;
static mp4mux_t *h264_params;
static pthread_mutex_t h264_params_mx = PTHREAD_MUTEX_INITIALIZER;

/* GC880 GPU demosaic for the stream path; the NEON path remains the
 * fallback (and always the snapshot path). The GPU renders into the IPU
 * stride-fix buffer and the IPU crops that into each encoder (the GPU's
 * row padding and the CODA's fixed stride are incompatible; ipu_copy.h).
 * Worker-thread use only. */
static gpu_debayer_t *gpu;
static ipu_copy_t *ipu;
static int gpu_disabled;
static int hw_skip_disabled;

/* Consecutive hard failures of each encoder in this engine run; three
 * drop that encoder until the next engine start. */
static int vpu_hard_fails, h264_hard_fails;

/* The FORGECTRL_NO_VPU / _NO_H264 / _NO_GPU switches. Every engine start
 * probes the hardware paths again from these, not from zero, so a
 * switch holds for the daemon's lifetime. */
static int env_no_vpu, env_no_h264, env_no_gpu;

/* The FORGECTRL_GPU_CHECK / _NEON_CHECK one-shot bench comparisons. */
static int gpu_check, neon_check;

/* FORGECTRL_FLAG_EVERY=N flags every Nth dequeued frame the way the
 * receiver flags a torn one: a bench drill for the frame gate and the
 * health ladder, never for serving pictures. */
static unsigned flag_every, flag_count;

/* FORGECTRL_NO_CACHED_BUFS: never request non-coherent capture buffers
 * (forces the uncached-mmap + bounce-copy path). */
static int cached_disabled;

/* Whether the next engine start expects the GPU to demosaic the stream;
 * the capture buffers follow it. When the GPU reads a frame over its
 * dmabuf nothing on the CPU does, so the buffers are requested
 * coherent: a non-coherent buffer is cache-maintained over its whole
 * 5 MB at every queue and dequeue, and the dequeue side runs in the
 * receiver's end-of-frame interrupt, with every other interrupt held
 * off for its duration (4.5 ms a frame on the bench reference). When the
 * CPU demosaics instead, the cached mapping is what makes its reads
 * fast, so a GPU that failed or never came up makes the next start ask
 * for cached buffers. A snapshot on coherent buffers takes the bounce
 * copy (prepare_raw8), tens of milliseconds against a still's seconds.
 * Per-buffer cache hints are no alternative: this kernel's vb2 keeps a
 * hint for good once it is set on a buffer, so a buffer the stream
 * hinted would hand a later snapshot stale cache lines. */
static int gpu_expect = 1;

/* ------------------------------------------------ stream frame pipeline */

/* A stream frame goes: dequeued -> produced into the encoders' OUTPUT
 * buffers (the GPU render then the IPU crop, or the NEON demosaic) ->
 * JPEG encoded -> published, but only once the frame dequeued after it
 * came back clean. The receiver flags the next buffer to complete after
 * it reports lost sync (imx-media-csi's NFB4EOF), which need not be the
 * buffer holding the torn frame. So a flagged frame withholds both its
 * neighbors, and waiting for the next frame's verdict costs only the
 * part of a frame period the pipeline does not already spend. H.264
 * encodes at publication, because a frame it has encoded cannot be
 * taken back.
 *
 * On the GPU path the render's completion is a file descriptor the
 * worker polls beside the capture queue. A frame goes into the GPU the
 * moment it arrives and the GPU is free; the finished render is then
 * cropped and encoded while the worker waits, after any frame that came
 * in meanwhile has gone into the GPU. The render, the crop and the
 * encode together fit in a frame period. The two IPU source slots
 * alternate.
 *
 * Capture buffers out of the queue at once: the one in the GPU render,
 * the newest one waiting for the encoders (an older one goes straight
 * back, so a frame is never served stale), and a snapshot candidate
 * waiting for its own verdict. Worker thread only; teardowns reset it,
 * because STREAMOFF takes every buffer back. */
struct pframe {
    int      valid;
    unsigned idx;       /* capture buffer */
    uint64_t ts_ns;     /* end of its capture, CLOCK_MONOTONIC */
};

static struct {
    struct pframe nxt;          /* dequeued, waiting for the encoders */
    struct pframe rnd;          /* in the GPU render */
    int      rnd_slot;          /* the IPU source slot it renders into */
    int      rnd_vetoed;        /* a neighbor came back flagged */
    int      rnd_confirmed;     /* a clean frame came back after it */
    uint64_t rnd_kick_ns;
    int      slot_next;
    /* rendered, waiting in its IPU source slot for the crop */
    struct {
        int      valid;
        int      slot;
        uint64_t ts_ns;
        int      vetoed;
        int      confirmed;
        int      raw_held;      /* the one-shot GPU check still reads it */
        unsigned raw_idx;
    } done;
    struct pframe snap;         /* snapshot candidate awaiting its verdict */
    int      taint;             /* the next clean frame follows a flagged one */
    /* produced and encoded, waiting for the next frame's verdict */
    int      held;
    uint64_t held_ts;
    uint8_t *held_jpg;
    size_t   held_len;
    int      held_vpu;          /* the JPEG came from the VPU */
    int      held_gpu;          /* the picture came from the GPU */
    int      held_h264;         /* the picture waits in the H.264 encoder */
    int      held_ok;           /* a clean frame came back after it */
    int      qbuf_failed;
    /* the fps and per-stage window */
    struct timespec win_t0;
    unsigned win_frames;
    unsigned warm;              /* frames still left out of the stage means */
    double   s_lat, s_conv, s_copy, s_enc;
    unsigned n_lat, n_conv, n_copy, n_enc;
} strm;

static void strm_drop_held(void)
{
    free(strm.held_jpg);
    strm.held_jpg = NULL;
    strm.held = 0;
    strm.held_h264 = 0;
    strm.held_ok = 0;
}

/* Forget the frames in flight without queueing them: the caller is about
 * to STREAMOFF, which takes every buffer back, and has closed or settled
 * the GPU. */
static void strm_reset(void)
{
    strm.done.valid = 0;
    strm_drop_held();
    strm.nxt.valid = strm.rnd.valid = strm.snap.valid = 0;
    strm.taint = 0;
}

/* ------------------------------------------------------------------ util */

static void now_ts(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static double ts_diff(const struct timespec *a, const struct timespec *b)
{
    return (double)(a->tv_sec - b->tv_sec) +
           (double)(a->tv_nsec - b->tv_nsec) / 1e9;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

/* ---------------------------------------------------- bounded children */

/* Pipeline configuration shells out to media-ctl and v4l2-ctl. A wedged
 * V4L2 pipeline can hang those forever, and with a thread per connection
 * a hung child would pin its request thread and the camera control lock
 * behind it. Every child therefore runs in its own process group under a
 * hard deadline: past it the whole group is killed and the call fails. */
#define CAM_CMD_TIMEOUT_S 10.0

static pid_t child_spawn(const char *cmd, int *outfd)
{
    int pfd[2] = {-1, -1};
    if (outfd && pipe2(pfd, O_CLOEXEC) < 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        if (outfd) {
            close(pfd[0]);
            close(pfd[1]);
        }
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0)
            dup2(devnull, STDIN_FILENO);
        if (outfd) {
            dup2(pfd[1], STDOUT_FILENO);
            close(pfd[0]);
            close(pfd[1]);
        } else if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
        }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (outfd) {
        close(pfd[1]);
        *outfd = pfd[0];
    }
    return pid;
}

/* Read until EOF, a full buffer, or the deadline. NUL-terminates and
 * returns the bytes read, or -1 on a timeout or a read error. */
static ssize_t child_read_all(int fd, char *buf, size_t len, double deadline_s)
{
    size_t off = 0;
    struct timespec t0, t;
    now_ts(&t0);
    while (off < len - 1) {
        now_ts(&t);
        double left = deadline_s - ts_diff(&t, &t0);
        if (left <= 0)
            return -1;
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        struct timeval tv;
        tv.tv_sec = (time_t)left;
        tv.tv_usec = (suseconds_t)((left - (double)tv.tv_sec) * 1e6);
        int s = select(fd + 1, &rf, NULL, NULL, &tv);
        if (s < 0 && errno == EINTR)
            continue;
        if (s <= 0)
            return -1;
        ssize_t n = read(fd, buf + off, len - 1 - off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return -1;
        if (n == 0)
            break;
        off += (size_t)n;
    }
    buf[off] = '\0';
    return (ssize_t)off;
}

/* Reap the child within the deadline; past it the group is killed.
 * Returns the exit status, or -1 for a kill, a signal, or an error. */
static int child_reap(pid_t pid, double deadline_s)
{
    struct timespec t0, t;
    int st;
    now_ts(&t0);
    for (;;) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid)
            return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (r < 0 && errno != EINTR)
            return -1;
        now_ts(&t);
        if (ts_diff(&t, &t0) >= deadline_s) {
            kill(-pid, SIGKILL);
            waitpid(pid, &st, 0);
            fflog(LOG_ERR, "cam: child past its %.0f s deadline - killed",
                  deadline_s);
            return -1;
        }
        usleep(50 * 1000);
    }
}

/* Run a shell command, logging and returning nonzero on failure. */
static int run(const char *fmt, ...)
{
    char cmd[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    pid_t pid = child_spawn(cmd, NULL);
    if (pid < 0) {
        fflog(LOG_ERR, "cam: cannot spawn: %s", cmd);
        return -1;
    }
    if (child_reap(pid, CAM_CMD_TIMEOUT_S) != 0) {
        fflog(LOG_ERR, "cam: command failed: %s", cmd);
        return -1;
    }
    return 0;
}

/* Run a command and capture its first line of output. */
static int run_read(char *out, size_t outlen, const char *fmt, ...)
{
    char cmd[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    int fd = -1;
    pid_t pid = child_spawn(cmd, &fd);
    if (pid < 0)
        return -1;
    char buf[1024];
    ssize_t n = child_read_all(fd, buf, sizeof(buf), CAM_CMD_TIMEOUT_S);
    close(fd);
    if (n < 0)
        kill(-pid, SIGKILL);
    int rc = child_reap(pid, 1.0);
    buf[n > 0 ? strcspn(buf, "\r\n") : 0] = '\0';
    if (n <= 0 || rc != 0 || !buf[0]) {
        fflog(LOG_ERR, "cam: command failed: %s", cmd);
        return -1;
    }
    snprintf(out, outlen, "%s", buf);
    return 0;
}

static int sysfs_read_int(const char *path, int *val)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int ok = fscanf(f, "%d", val) == 1;
    fclose(f);
    return ok ? 0 : -1;
}

static int sysfs_write_int(const char *path, int val)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    int ok = fprintf(f, "%d", val) > 0;
    fclose(f);
    return ok ? 0 : -1;
}

/* The lid lamp at idle. The PIC lights it at power-on; a warm reboot
 * leaves it dark (the module's remove path turns it off) and the cloud
 * client sets its own level, so the daemon asserts the configured idle
 * level itself: at start, on a settings change, and whenever the
 * supervisor spawns a controller. Setting lid_lamp_idle, 0-255. */
#define LAMP_IDLE_DEFAULT 236

int cam_lamp_idle_level(void)
{
    char v[16];
    if (settings_get("lid_lamp_idle", v, sizeof(v)) == 0 && v[0]) {
        char *end;
        long l = strtol(v, &end, 10);
        if (end != v && *end == '\0' && l >= 0 && l <= 255)
            return (int)l;
    }
    return LAMP_IDLE_DEFAULT;
}

void cam_lamp_apply_idle(void)
{
    int level = cam_lamp_idle_level();
    pthread_mutex_lock(&eng.ctl);
    pthread_mutex_lock(&eng.lock);
    int lid_capturing = eng.running && eng.cam == CAM_LID && eng.lamp_prev >= 0;
    pthread_mutex_unlock(&eng.lock);
    if (lid_capturing)
        eng.lamp_prev = level;      /* restored at the capture's teardown */
    else
        sysfs_write_int(camdefs[CAM_LID].lamp, level);
    pthread_mutex_unlock(&eng.ctl);
    fflog(LOG_INFO, "cam: lid lamp idle level %d%s", level,
          lid_capturing ? " (applies after the capture)" : "");
}

/* --------------------------------------------------- pipeline configure */

/* Resolve the sensor media entity on an I2C bus (e.g. "ov5648 0-0036") by
 * its address suffix, so OV5648 and OV8856 (HD model) both match. */
static int sensor_entity(int bus, char *out, size_t outlen)
{
    char needle[16];
    snprintf(needle, sizeof(needle), " %d-0036", bus);

    int fd = -1;
    pid_t pid = child_spawn("media-ctl -p", &fd);
    if (pid < 0)
        return -1;
    char *dump = malloc(32768);
    if (!dump) {
        close(fd);
        kill(-pid, SIGKILL);
        child_reap(pid, 1.0);
        return -1;
    }
    ssize_t got = child_read_all(fd, dump, 32768, CAM_CMD_TIMEOUT_S);
    close(fd);
    if (got < 0)
        kill(-pid, SIGKILL);
    child_reap(pid, 1.0);
    int found = 0;
    char *line = got > 0 ? dump : NULL;
    while (line && !found) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        char *e = strstr(line, "entity ");
        if (e) {
            char *colon = strchr(e, ':');
            char *hit = strstr(line, needle);
            if (colon && hit && hit >= colon) {
                char *start = colon + 2;
                char *end = hit + strlen(needle);
                if (end > start && (size_t)(end - start) < outlen) {
                    memcpy(out, start, (size_t)(end - start));
                    out[end - start] = '\0';
                    found = 1;
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(dump);
    return found ? 0 : -1;
}

/* Route the selected sensor through the video-mux to the capture node and
 * set the raw-Bayer format on every pad of the active path. Exactly one
 * mux sink link may be enabled, so the other camera's link (if that sensor
 * exists) is disabled first. */
static int configure_pipeline(cam_id_t cam, const char *sensor,
                              const char *other_sensor,
                              const struct sensor_profile *p)
{
    const struct camdef *c = &camdefs[cam];
    const struct camdef *o = &camdefs[cam == CAM_LID ? CAM_HEAD : CAM_LID];
    char fmt[64];

    /* 'field:none' is mandatory - without it link validation rejects
     * STREAMON with -EPIPE. */
    snprintf(fmt, sizeof(fmt), "%s/%dx%d field:none", p->mbus, p->w, p->h);

    if (other_sensor[0] &&
        run("media-ctl -l '\"%s\":0 -> \"video-mux\":%d [0]'",
            other_sensor, o->muxpad))
        return -1;

    if (run("media-ctl -l '\"%s\":0 -> \"video-mux\":%d [1]'",
            sensor, c->muxpad) ||
        run("media-ctl -l '\"video-mux\":2 -> \"imx6-mipi-csi2\":0 [1]'") ||
        run("media-ctl -l '\"imx6-mipi-csi2\":1 -> \"ipu1_csi0_mux\":0 [1]'") ||
        run("media-ctl -l '\"ipu1_csi0_mux\":5 -> \"ipu1_csi0\":0 [1]'") ||
        run("media-ctl -l '\"ipu1_csi0\":2 -> \"" CAPTURE_ENTITY "\":0 [1]'"))
        return -1;

    if (run("media-ctl -V '\"%s\":0 [fmt:%s]'", sensor, fmt) ||
        run("media-ctl -V '\"video-mux\":%d [fmt:%s]'", c->muxpad, fmt) ||
        run("media-ctl -V '\"video-mux\":2 [fmt:%s]'", fmt) ||
        run("media-ctl -V '\"imx6-mipi-csi2\":0 [fmt:%s]'", fmt) ||
        run("media-ctl -V '\"imx6-mipi-csi2\":1 [fmt:%s]'", fmt) ||
        run("media-ctl -V '\"ipu1_csi0_mux\":0 [fmt:%s]'", fmt) ||
        run("media-ctl -V '\"ipu1_csi0_mux\":5 [fmt:%s]'", fmt) ||
        run("media-ctl -V '\"ipu1_csi0\":0 [fmt:%s]'", fmt) ||
        run("media-ctl -V '\"ipu1_csi0\":2 [fmt:%s]'", fmt))
        return -1;

    /* An fps cap of at least 1 is realized in hardware where possible:
     * the IPU CSI's frame-skip table drops frames before they are ever
     * DMA-written, so a skipped frame costs no memory bandwidth and no
     * CPU (the software pacing in the worker still runs and passes what
     * arrives). The sink pad carries the sensor rate, the IDMAC source
     * pad the requested one; the driver picks the nearest skip pattern.
     * Best effort: a media-ctl without interval syntax leaves the cap to
     * software pacing alone. */
    int hw = 0;
    if (!hw_skip_disabled && eng.fps_cap >= 1.0 &&
        eng.fps_cap < (double)p->fps) {
        int n = (int)(eng.fps_cap + 0.5);
        char fmt_in[80], fmt_out[80];
        snprintf(fmt_in, sizeof(fmt_in), "%s/%dx%d@1/%d field:none",
                 p->mbus, p->w, p->h, p->fps);
        snprintf(fmt_out, sizeof(fmt_out), "%s/%dx%d@1/%d field:none",
                 p->mbus, p->w, p->h, n);
        if (run("media-ctl -V '\"ipu1_csi0\":0 [fmt:%s]'", fmt_in) == 0 &&
            run("media-ctl -V '\"ipu1_csi0\":2 [fmt:%s]'", fmt_out) == 0) {
            hw = 1;
            fflog(LOG_INFO, "cam: CSI hardware frame skip to ~%d fps", n);
        } else {
            fflog(LOG_WARNING, "cam: CSI frame-skip setup refused, the "
                  "fps cap stays software-paced");
        }
    }
    pthread_mutex_lock(&eng.lock);
    eng.hw_skip = hw;
    pthread_mutex_unlock(&eng.lock);
    return 0;
}

/* Manual exposure/gain/white-balance on the sensor subdev (factory values).
 * The auto-clusters must go manual before the manual values take effect.
 * The sensor flips stay off: HFLIP breaks imx-media CSI capture, so the
 * factory mirror is applied in software (debayer).
 *
 * Exposure is in 1/16-line units and cannot exceed the frame length: the
 * 2592x1944 mode is 1984 lines, so the usable ceiling is ~31600. Gain is in
 * 1/16 steps (16 = 1x). */
static int ctrls_ov5648(const char *subdev, cam_id_t cam)
{
    static const struct { int exposure, gain; } d[2] = {
        [CAM_LID]  = { 24000,  50 },
        [CAM_HEAD] = { 24000, 200 },
    };

    if (run("v4l2-ctl -d %s -c auto_exposure=1 -c gain_automatic=0"
            " -c white_balance_automatic=0", subdev))
        return -1;
    if (run("v4l2-ctl -d %s -c exposure=%d -c gain=%d -c red_balance=1100"
            " -c blue_balance=1400 -c horizontal_flip=0 -c vertical_flip=0",
            subdev, d[cam].exposure, d[cam].gain))
        return -1;
    return 0;
}

/* The OV8856 driver exposes a different set: exposure counts whole lines
 * (it shifts into the 1/16-line register itself) and is capped by the frame
 * length - 2482 lines in the 3264x2448 mode - analog gain is 128 = 1x,
 * and there are no auto-exposure, auto-gain or white-balance controls to
 * switch off, so the sensor comes up manual. The flips are still forced off
 * for the same reason as the OV5648.
 *
 * UNPROVEN: these are the OV5648 defaults translated into the OV8856's
 * units - the same fraction of the frame (76%) and the same gain multiple
 * (3.1x lid, 12.5x head). They are a starting point for setup on a
 * real 8 MP machine, not measured values, and the driver publishes no
 * red/blue balance controls at all, so white balance is uncorrected. */
static int ctrls_ov8856(const char *subdev, cam_id_t cam)
{
    static const struct { int exposure, gain; } d[2] = {
        [CAM_LID]  = { 1886,  400 },
        [CAM_HEAD] = { 1886, 1600 },
    };

    if (run("v4l2-ctl -d %s -c exposure=%d -c analogue_gain=%d"
            " -c digital_gain=1024",
            subdev, d[cam].exposure, d[cam].gain))
        return -1;
    return 0;
}

static int configure_sensor(const char *sensor, const struct sensor_profile *p,
                            cam_id_t cam)
{
    char subdev[64];
    if (run_read(subdev, sizeof(subdev), "media-ctl -e '%s'", sensor))
        return -1;
    return p->ctrls(subdev, cam);
}

/* ------------------------------------------------------- jpeg encoding */

struct jpeg_err_jmp {
    struct jpeg_error_mgr mgr;
    jmp_buf env;
};

static void jpeg_error_longjmp(j_common_ptr ci)
{
    struct jpeg_err_jmp *e = (struct jpeg_err_jmp *)ci->err;
    char msg[JMSG_LENGTH_MAX];
    e->mgr.format_message(ci, msg);
    fflog(LOG_ERR, "cam: jpeg: %s", msg);
    longjmp(e->env, 1);
}

static int jpeg_encode_rgb(const uint8_t *rgb, int w, int h, int quality,
                           int fast, uint8_t **out, size_t *outlen)
{
    struct jpeg_compress_struct ci;
    struct jpeg_err_jmp jerr;
    unsigned char *buf = NULL;
    unsigned long buflen = 0;

    /* libjpeg's default error_exit calls exit(): a fatal error (an
     * allocation that fails on a full-resolution frame) must end the
     * encode, not the daemon. */
    ci.err = jpeg_std_error(&jerr.mgr);
    jerr.mgr.error_exit = jpeg_error_longjmp;
    if (setjmp(jerr.env)) {
        jpeg_destroy_compress(&ci);
        free(buf);
        return -1;
    }
    jpeg_create_compress(&ci);
    jpeg_mem_dest(&ci, &buf, &buflen);
    ci.image_width = (JDIMENSION)w;
    ci.image_height = (JDIMENSION)h;
    ci.input_components = 3;
    ci.in_color_space = JCS_RGB;
    jpeg_set_defaults(&ci);
    jpeg_set_quality(&ci, quality, TRUE);
    if (fast)
        ci.dct_method = JDCT_FASTEST;
    jpeg_start_compress(&ci, TRUE);
    while (ci.next_scanline < ci.image_height) {
        JSAMPROW row = (JSAMPROW)(rgb + (long)ci.next_scanline * w * 3);
        jpeg_write_scanlines(&ci, &row, 1);
    }
    jpeg_finish_compress(&ci);
    jpeg_destroy_compress(&ci);
    *out = buf;
    *outlen = (size_t)buflen;
    return 0;
}

/* --------------------------------------------------- capture start/stop */

/* Release every capture resource and restore the lamp. Safe to call from
 * any state; called by the worker on exit and by a failed start. */
static void release_capture(void)
{
    /* The GPU holds imports of the capture buffers about to go away;
     * it re-imports on the next stream frame (the encoders and their
     * dmabufs survive, so only the raw side is redone). */
    if (gpu) {
        gpu_debayer_close(gpu);
        gpu = NULL;
    }
    strm_reset();
    if (eng.streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(eng.fd, VIDIOC_STREAMOFF, &type);
        eng.streaming = 0;
    }
    for (int i = 0; i < eng.n_bufs; i++) {
        if (eng.bufs[i].start) {
            munmap(eng.bufs[i].start, eng.bufs[i].length);
            eng.bufs[i].start = NULL;
        }
        if (eng.bufs[i].dmabuf >= 0) {
            close(eng.bufs[i].dmabuf);
            eng.bufs[i].dmabuf = -1;
        }
    }
    eng.n_bufs = 0;
    if (eng.fd >= 0) {
        close(eng.fd);
        eng.fd = -1;
    }
    if (eng.lamp_prev >= 0) {
        sysfs_write_int(camdefs[eng.cam].lamp, eng.lamp_prev);
        eng.lamp_prev = -1;
    }
}

/* Stop and restart the capture queue on the open node, leaving the media
 * graph, the sensor and the lamp alone. This is the recovery for a sensor
 * that has lost CSI-2 sync: the frames it produces come back flagged
 * errored until the receiver is re-synchronized, and cycling the queue is
 * what re-synchronizes it. Worker thread only. */
static int restream(void)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (xioctl(eng.fd, VIDIOC_STREAMOFF, &type) < 0)
        return -1;
    eng.streaming = 0;
    /* STREAMOFF returns every buffer to the dequeued state. */
    for (int i = 0; i < eng.n_bufs; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = (unsigned)i;
        if (xioctl(eng.fd, VIDIOC_QBUF, &buf) < 0)
            return -1;
    }
    if (xioctl(eng.fd, VIDIOC_STREAMON, &type) < 0)
        return -1;
    eng.streaming = 1;
    cam_health_restarted(&eng.health);
    pthread_mutex_lock(&eng.lock);
    eng.recoveries++;
    pthread_mutex_unlock(&eng.lock);
    return 0;
}

/* Configure the media graph and sensor, light the lamp, and bring up the
 * V4L2 capture node streaming. Called with ctl held, engine not running.
 * head_open: a local viewer asks for the head camera, which may start with
 * the lid open. */
static int start_capture(cam_id_t cam, int head_open, char *err, size_t errlen)
{
    const struct camdef *c = &camdefs[cam];
    char sensor[64], other[64] = "";

    /* Privacy gate, checked before the media graph is touched and before
     * the lamp is raised, so an open lid leaves no trace of an attempt. */
    if (!machine_lid_closed() && !(cam == CAM_HEAD && head_open)) {
        snprintf(err, errlen, "%s", CAM_ERR_LID);
        return -1;
    }

    if (sensor_entity(c->bus, sensor, sizeof(sensor))) {
        snprintf(err, errlen, "no camera sensor on i2c-%d", c->bus);
        return -1;
    }
    const struct sensor_profile *p = profile_for(sensor);
    if (!p) {
        snprintf(err, errlen, "unsupported camera sensor '%s'", sensor);
        return -1;
    }
    (void)sensor_entity(camdefs[cam == CAM_LID ? CAM_HEAD : CAM_LID].bus,
                        other, sizeof(other));

    if (configure_pipeline(cam, sensor, other, p)) {
        snprintf(err, errlen, "media pipeline configuration failed");
        return -1;
    }
    if (configure_sensor(sensor, p, cam)) {
        snprintf(err, errlen, "sensor configuration failed");
        return -1;
    }

    char dev[64];
    if (run_read(dev, sizeof(dev), "media-ctl -e '" CAPTURE_ENTITY "'")) {
        snprintf(err, errlen, "cannot resolve capture video node");
        return -1;
    }

    pthread_mutex_lock(&eng.lock);
    eng.cam = cam;
    eng.prof = p;
    eng.seen[cam] = p;
    eng.lid_stopped = 0;    /* the lid read closed, or a local viewer has the head camera */
    pthread_mutex_unlock(&eng.lock);

    /* Scene lighting for the duration; the previous level is restored at
     * teardown (raw register write - instant, no fade). */
    if (sysfs_read_int(c->lamp, &eng.lamp_prev))
        eng.lamp_prev = 0;
    sysfs_write_int(c->lamp, eng.lamp_level);

    /* O_CLOEXEC: a controller spawned while the capture is up must not
     * inherit this descriptor - the buffers stay allocated for as long as
     * any copy is open, and every later S_FMT fails with EBUSY. */
    eng.fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC, 0);
    if (eng.fd < 0) {
        snprintf(err, errlen, "open %s: %s", dev, strerror(errno));
        release_capture();
        return -1;
    }

    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = (unsigned)p->w;
    fmt.fmt.pix.height = (unsigned)p->h;
    fmt.fmt.pix.pixelformat = p->pixfmt;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(eng.fd, VIDIOC_S_FMT, &fmt) < 0) {
        snprintf(err, errlen, "S_FMT: %s", strerror(errno));
        release_capture();
        return -1;
    }
    if ((int)fmt.fmt.pix.width != p->w || (int)fmt.fmt.pix.height != p->h ||
        fmt.fmt.pix.pixelformat != p->pixfmt) {
        snprintf(err, errlen, "capture node gave %ux%u fourcc %.4s, "
                 "not %dx%d for %s", fmt.fmt.pix.width, fmt.fmt.pix.height,
                 (const char *)&fmt.fmt.pix.pixelformat, p->w, p->h,
                 p->model);
        release_capture();
        return -1;
    }

    struct v4l2_requestbuffers req = {0};
    req.count = N_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    /* Non-coherent = CPU-cached mappings when the CPU is to demosaic the
     * stream: the CSI DMA-writes the frames either way, but a cached
     * mapping lets the demosaic read them at cached speed (vb2
     * invalidates the CPU cache as each frame completes). Coherent ones
     * when the GPU is (see gpu_expect). A capture queue without
     * cache-hint support ignores the flag and omits the
     * MMAP_CACHE_HINTS capability; the bounce-copy path covers that. */
    int want_cached = !cached_disabled && !(gpu_expect && !env_no_gpu);
    if (want_cached)
        req.flags = V4L2_MEMORY_FLAG_NON_COHERENT;
    if (xioctl(eng.fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        snprintf(err, errlen, "REQBUFS: %s (device busy?)", strerror(errno));
        release_capture();
        return -1;
    }
    int cached = want_cached &&
        (req.capabilities & V4L2_BUF_CAP_SUPPORTS_MMAP_CACHE_HINTS) != 0;
    pthread_mutex_lock(&eng.lock);
    eng.cached_bufs = cached;
    pthread_mutex_unlock(&eng.lock);
    fflog(LOG_INFO, "cam: %s on %s, %dx%d %s, capture buffers %s",
          p->model, c->name, p->w, p->h, p->mbus,
          cached ? "cached (non-coherent)" :
          want_cached ? "uncached (bounce copy)" : "coherent (GPU path)");

    for (eng.n_bufs = 0; eng.n_bufs < (int)req.count; eng.n_bufs++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = (unsigned)eng.n_bufs;
        if (xioctl(eng.fd, VIDIOC_QUERYBUF, &buf) < 0) {
            snprintf(err, errlen, "QUERYBUF: %s", strerror(errno));
            release_capture();
            return -1;
        }
        eng.bufs[eng.n_bufs].length = buf.length;
        eng.bufs[eng.n_bufs].dmabuf = -1;
        eng.bufs[eng.n_bufs].start = mmap(NULL, buf.length,
                                          PROT_READ | PROT_WRITE, MAP_SHARED,
                                          eng.fd, buf.m.offset);
        if (eng.bufs[eng.n_bufs].start == MAP_FAILED) {
            eng.bufs[eng.n_bufs].start = NULL;
            snprintf(err, errlen, "mmap: %s", strerror(errno));
            release_capture();
            return -1;
        }
    }

    for (int i = 0; i < eng.n_bufs; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = (unsigned)i;
        if (xioctl(eng.fd, VIDIOC_QBUF, &buf) < 0) {
            snprintf(err, errlen, "QBUF: %s", strerror(errno));
            release_capture();
            return -1;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(eng.fd, VIDIOC_STREAMON, &type) < 0) {
        snprintf(err, errlen, "STREAMON: %s", strerror(errno));
        release_capture();
        return -1;
    }
    eng.streaming = 1;
    cam_health_started(&eng.health);
    return 0;
}

/* ---------------------------------------------------------- worker loop */

/* Encode the pending snapshot request from a raw frame and deliver the
 * result (success or failure) to the waiter. `raw` is 8-bit BGGR at the
 * geometry of the profile the pipeline is running (eng.prof). */
static void deliver_snap(const uint8_t *raw, uint8_t *rgb_half,
                         uint8_t **prgb_full, size_t *prgb_full_cap)
{
    uint8_t *jpg = NULL;
    size_t len = 0;
    int ok;

    pthread_mutex_lock(&eng.lock);
    int full = eng.snap_full;
    int q = eng.snap_quality;
    const struct sensor_profile *p = eng.prof;
    pthread_mutex_unlock(&eng.lock);

    const int w = p->w, h = p->h;

    if (full) {
        size_t need = (size_t)w * h * 3;
        if (*prgb_full_cap < need) {
            free(*prgb_full);
            *prgb_full = malloc(need);
            *prgb_full_cap = *prgb_full ? need : 0;
        }
        ok = *prgb_full != NULL;
        if (ok) {
            debayer_bggr_bilinear(raw, *prgb_full, w, h, HFLIP);
            ok = jpeg_encode_rgb(*prgb_full, w, h, q, 0, &jpg, &len) == 0;
        }
    } else {
        debayer_bggr_half(raw, rgb_half, w, h, HFLIP);
        ok = jpeg_encode_rgb(rgb_half, w / 2, h / 2, q, 0, &jpg, &len) == 0;
    }

    pthread_mutex_lock(&eng.lock);
    free(eng.snap_jpg);
    eng.snap_jpg = ok ? jpg : NULL;
    eng.snap_len = ok ? len : 0;
    eng.snap_pending = ok ? 2 : 3;
    now_ts(&eng.last_activity);
    pthread_cond_broadcast(&eng.snap_cv);
    pthread_mutex_unlock(&eng.lock);
}

/* Mark a pending snapshot failed (only if not already delivered). */
static void fail_snap(void)
{
    pthread_mutex_lock(&eng.lock);
    if (eng.snap_pending == 1) {
        eng.snap_pending = 3;
        pthread_cond_broadcast(&eng.snap_cv);
    }
    pthread_mutex_unlock(&eng.lock);
}

/* Present a dequeued capture buffer as the 8-bit BGGR frame every demosaic
 * takes. An 8-bit sensor on cached buffers needs nothing (the mapping is
 * read directly); an 8-bit sensor on uncached buffers is bulk-copied,
 * because byte reads from a coherent mapping each cost a bus transaction
 * (demosaicing in place measures ~340 ms/frame); a 10-bit sensor is
 * narrowed into `scratch`, which is also the copy. */
static const uint8_t *prepare_raw8(const void *cap, uint8_t *scratch,
                                   const struct sensor_profile *p)
{
    size_t n = (size_t)p->w * p->h;

    if (p->bpp == 1) {
        if (eng.cached_bufs)
            return (const uint8_t *)cap;
        memcpy(scratch, cap, n);
        return scratch;
    }

    uint16_t peak = debayer_narrow16(cap, scratch, n, p->shift);
    /* One line per engine start: on a sensor whose sample alignment has
     * not been measured, the peak says whether the shift is right (a lit
     * 10-bit frame peaks near 1023, not near 65535 or near 255). */
    static const struct sensor_profile *logged;
    if (logged != p) {
        logged = p;
        fflog(LOG_DEBUG, "cam: %s raw peak sample %u (>>%d)",
              p->model, peak, p->shift);
    }
    return scratch;
}

/* Count a dequeued frame and say what to do with it (camhealth.h). */
static cam_frame_action_t classify(const struct v4l2_buffer *buf)
{
    int errored = (buf->flags & V4L2_BUF_FLAG_ERROR) != 0;

    pthread_mutex_lock(&eng.lock);
    eng.frames++;
    if (errored)
        eng.corrupt++;
    pthread_mutex_unlock(&eng.lock);

    return cam_health_frame(&eng.health, errored);
}

/* Capture one frame from the currently-started pipeline and feed it to
 * deliver_snap. Used by the borrow path. The first `skip` dequeued frames
 * are requeued unused (frames already in flight predate a just-changed lamp
 * level), as are the stream's warm-up frames and any frame the queue flags
 * errored. The chosen frame is delivered only once the frame after it
 * comes back clean, and a flagged frame sends back the candidate before
 * it and the frame after it (see the stream pipeline note). A borrow that
 * cannot get a clean frame fails the snapshot rather than restarting
 * anything: the caller tears the borrowed pipeline down either way. */
static int grab_one_snap(uint8_t *raw_cached, uint8_t *rgb_half,
                         uint8_t **prgb_full, size_t *prgb_full_cap,
                         int skip)
{
    /* Enough iterations for the lamp drain, the warm-up drain, the bad
     * frames the ladder tolerates and the frames that vouch for a
     * candidate, plus MAX_DQ_TIMEOUTS empty waits. */
    int budget = skip + CAM_WARMUP_FRAMES + 2 * CAM_MAX_BAD_FRAMES + 2 +
                 MAX_DQ_TIMEOUTS;
    int cand = -1;      /* the chosen frame, awaiting the next one's verdict */
    int taint = 0;
    int rc = -1;

    for (int tries = 0; tries < MAX_DQ_TIMEOUTS && budget-- > 0; tries++) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(eng.fd, &fds);
        struct timeval tv = { .tv_sec = DQ_TIMEOUT_S };
        int r = select(eng.fd + 1, &fds, NULL, NULL, &tv);
        if (r == -1 && errno == EINTR) {
            tries--;
            continue;
        }
        if (r <= 0)
            continue;
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(eng.fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN || errno == EIO)
                continue;
            break;
        }
        cam_frame_action_t act = classify(&buf);
        if (act == CAM_FRAME_RESTART || act == CAM_FRAME_ABORT) {
            xioctl(eng.fd, VIDIOC_QBUF, &buf);
            break;
        }
        tries--;
        if (act == CAM_FRAME_DROP) {
            xioctl(eng.fd, VIDIOC_QBUF, &buf);
            if (cand >= 0) {
                struct v4l2_buffer cb = {0};
                cb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                cb.memory = V4L2_MEMORY_MMAP;
                cb.index = (unsigned)cand;
                xioctl(eng.fd, VIDIOC_QBUF, &cb);
                cand = -1;
            }
            taint = 1;
            continue;
        }
        /* A clean frame: a warm-up one, or the one after a flagged
         * frame, goes back unused. */
        if (act == CAM_FRAME_WARMUP || taint) {
            taint = 0;
            xioctl(eng.fd, VIDIOC_QBUF, &buf);
            continue;
        }
        if (cand >= 0) {
            /* This clean frame vouches for the candidate. */
            deliver_snap(prepare_raw8(eng.bufs[cand].start, raw_cached,
                                      eng.prof),
                         rgb_half, prgb_full, prgb_full_cap);
            xioctl(eng.fd, VIDIOC_QBUF, &buf);
            rc = 0;
            break;
        }
        if (skip > 0) {
            skip--;
            xioctl(eng.fd, VIDIOC_QBUF, &buf);
            continue;
        }
        cand = (int)buf.index;
    }
    if (cand >= 0) {
        struct v4l2_buffer cb = {0};
        cb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        cb.memory = V4L2_MEMORY_MMAP;
        cb.index = (unsigned)cand;
        xioctl(eng.fd, VIDIOC_QBUF, &cb);
    }
    return rc;
}

/* ---------------------------------------------- stream encoder plumbing */

/* Geometry the encoder set (worker thread only). */
static int enc_w, enc_h;

/* Drop the H.264 path for this engine run. Its viewers read the flag
 * under the engine lock and end their streams. */
static void h264_off(void)
{
    pthread_mutex_lock(&eng.lock);
    h264_disabled = 1;
    eng.h264_up = 0;
    pthread_cond_broadcast(&eng.h264_cv);
    pthread_mutex_unlock(&eng.lock);
}

/* Open (or re-open, on a geometry change) whichever encoders the current
 * clients need, and the GPU demosaic that feeds them. Every piece fails
 * soft: a missing encoder or GPU disables that path and the frame loop
 * uses what came up. */
static void ensure_encoders(int jpeg_want, int h264_want,
                            int half_w, int half_h, int hflip)
{
    if (enc_w != half_w || enc_h != half_h) {
        if (vpu) {
            vpu_jpeg_close(vpu);
            vpu = NULL;
        }
        if (h264) {
            vpu_h264_close(h264);
            h264 = NULL;
        }
        if (gpu) {
            gpu_debayer_close(gpu);
            gpu = NULL;
        }
        if (ipu) {
            ipu_copy_close(ipu);
            ipu = NULL;
        }
        pthread_mutex_lock(&h264_params_mx);
        mp4mux_free(h264_params);
        h264_params = NULL;
        pthread_mutex_unlock(&h264_params_mx);
        enc_w = half_w;
        enc_h = half_h;
    }

    if (jpeg_want && !vpu_disabled && !vpu) {
        vpu = vpu_jpeg_open(half_w, half_h, eng.stream_quality);
        strm.warm = 2;
        if (!vpu) {
            vpu_disabled = 1;
            fflog(LOG_WARNING, "cam: no VPU JPEG encoder, "
                  "using software encode");
        }
    }
    if (h264_want && !h264_disabled && !h264) {
        int fps = eng.fps_cap >= 1.0 ? (int)(eng.fps_cap + 0.5)
                                     : eng.prof->fps;
        strm.warm = 2;
        h264 = vpu_h264_open(half_w, half_h, fps, eng.h264_kbps * 1000,
                             eng.h264_gop);
        if (!h264) {
            h264_off();
            fflog(LOG_WARNING, "cam: no H.264 encoder, the stream "
                  "stays MJPEG only");
        } else {
            pthread_mutex_lock(&h264_params_mx);
            if (!h264_params)
                h264_params = mp4mux_new(half_w, half_h);
            pthread_mutex_unlock(&h264_params_mx);
        }
    }

    if (!gpu_disabled && !gpu && (vpu || h264)) {
        if (!ipu)
            ipu = ipu_copy_open(ipu_copy_src_width(half_w),
                                ipu_copy_src_height(half_h), half_w, half_h);
        if (!ipu) {
            gpu_disabled = 1;
            gpu_expect = 0;
            fflog(LOG_INFO, "cam: no IPU stride-fix crop, using the "
                  "NEON path");
            return;
        }
        gpu = gpu_debayer_open(eng.prof->w, eng.prof->h, hflip);
        strm.warm = 2;
        if (!gpu) {
            gpu_disabled = 1;
            gpu_expect = 0;
            fflog(LOG_INFO, "cam: no GPU demosaic, using the NEON path");
            return;
        }
        int ok = 1;
        for (int i = 0; ok && i < eng.n_bufs; i++) {
            if (eng.bufs[i].dmabuf < 0) {
                struct v4l2_exportbuffer exp = {
                    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                    .index = (unsigned)i,
                    .flags = O_CLOEXEC,
                };
                if (xioctl(eng.fd, VIDIOC_EXPBUF, &exp) < 0) {
                    fflog(LOG_INFO, "cam: capture dmabuf export refused: "
                          "%s", strerror(errno));
                    ok = 0;
                    break;
                }
                eng.bufs[i].dmabuf = exp.fd;
            }
            ok = gpu_debayer_attach_raw(gpu, i, eng.bufs[i].dmabuf) == 0;
        }
        for (int i = 0; ok && i < IPU_COPY_SRCS; i++) {
            int stride;
            size_t uv_off, len;
            int fd = ipu_copy_src_dmabuf(ipu, i, &stride, &uv_off, &len);
            ok = fd >= 0 &&
                 gpu_debayer_attach_dst(gpu, i, fd, stride, uv_off,
                                        len) == 0;
        }
        if (!ok) {
            gpu_debayer_close(gpu);
            gpu = NULL;
            gpu_disabled = 1;
            gpu_expect = 0;
            fflog(LOG_INFO, "cam: GPU import failed, using the NEON path");
        } else {
            gpu_expect = 1;
        }
    }
}

/* One-shot GPU-versus-CPU comparison on a live frame, FORGECTRL_GPU_CHECK.
 * The GPU is not bit-identical to the NEON path (different rounding), so
 * this reports the worst per-byte difference instead of demanding zero;
 * anything beyond a couple of counts means the shader indexing is wrong
 * on this GPU. Reads the write-combine encoder buffer, so it is slow and
 * runs once. */
static int gpu_checked;

static void gpu_check_once(const uint8_t *raw, int raw_w, int raw_h,
                           int hflip, vpu_jpeg_t *v, int slot)
{
    gpu_checked = 1;

    uint8_t *gy, *gu, *gv;
    int ys, uvs;
    vpu_jpeg_planes(v, &gy, &gu, &gv, &ys, &uvs);
    const int ow = raw_w / 2, oh = raw_h / 2;
    size_t ysz = (size_t)ys * oh, usz = (size_t)uvs * (oh / 2);
    uint8_t *ry = malloc(ysz), *ru = malloc(usz), *rv = malloc(usz);
    if (ry && ru && rv) {
        debayer_bggr_half_yuv420_scalar(raw, raw_w, raw_h, hflip,
                                        ry, ys, ru, rv, uvs);
        /* Luma is held to the CPU path within rounding; chroma is
         * reported separately, because the GPU point-samples where the
         * CPU box-filters (see gpu_debayer.c), so its deltas measure
         * scene chroma detail, not correctness. */
        int dmax = 0, cmax = 0;
        long bad = 0;
        double csum = 0;
        for (int r = 0; r < oh; r++)
            for (int x = 0; x < ow; x++) {
                int d = abs((int)gy[(size_t)r * ys + x] -
                            (int)ry[(size_t)r * ys + x]);
                if (d > dmax)
                    dmax = d;
                if (d > 2)
                    bad++;
            }
        for (size_t i = 0; i < usz; i++) {
            int du = abs((int)gu[i] - (int)ru[i]);
            int dv = abs((int)gv[i] - (int)rv[i]);
            if (du > cmax)
                cmax = du;
            if (dv > cmax)
                cmax = dv;
            csum += du + dv;
        }
        fflog(LOG_INFO, "cam: GPU/CPU compare: luma max delta %d, %ld "
              "samples off by more than 2; chroma vs box filter mean "
              "%.2f max %d", dmax, bad, csum / (double)(2 * usz), cmax);

        /* Attribute any bottom-row disagreement: the same row read from
         * the IPU's source (the GPU's own output, before the crop) says
         * whether the GPU rendered it wrong or the IPU copied it wrong. */
        const uint8_t *src = ipu ? ipu_copy_src_map(ipu, slot) : NULL;
        if (src) {
            int sstride;
            size_t suv, slen;
            ipu_copy_src_dmabuf(ipu, slot, &sstride, &suv, &slen);
            const uint8_t *pre = src + (size_t)(oh - 1) * sstride;
            const uint8_t *post = gy + (size_t)(oh - 1) * ys;
            const uint8_t *ref = ry + (size_t)(oh - 1) * ys;
            int dmax_pre = 0, dmax_post = 0;
            for (int x = 0; x < ow; x++) {
                int dp = abs((int)pre[x] - (int)ref[x]);
                int dq = abs((int)post[x] - (int)ref[x]);
                if (dp > dmax_pre)
                    dmax_pre = dp;
                if (dq > dmax_post)
                    dmax_post = dq;
            }
            fflog(LOG_INFO, "cam: bottom Y row: GPU-vs-CPU max %d, "
                  "post-IPU-vs-CPU max %d", dmax_pre, dmax_post);
        }
    }
    free(ry);
    free(ru);
    free(rv);
}

static uint64_t mono_ns(void)
{
    struct timespec t;
    now_ts(&t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* The end of a frame's capture: the receiver stamps it at the frame's
 * end-of-frame interrupt, on CLOCK_MONOTONIC. */
static uint64_t buf_ts_ns(const struct v4l2_buffer *b)
{
    return (uint64_t)b->timestamp.tv_sec * 1000000000ull +
           (uint64_t)b->timestamp.tv_usec * 1000ull;
}

static void requeue(unsigned idx)
{
    struct v4l2_buffer b = {0};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = idx;
    if (xioctl(eng.fd, VIDIOC_QBUF, &b) < 0) {
        fflog(LOG_ERR, "cam: QBUF: %s", strerror(errno));
        strm.qbuf_failed = 1;
    }
}

static void count_up(uint64_t *ctr)
{
    pthread_mutex_lock(&eng.lock);
    (*ctr)++;
    pthread_mutex_unlock(&eng.lock);
}

/* One stage time into the window's mean. The first frames after the
 * worker starts or an encoder or the GPU opens are left out (strm.warm):
 * their times include the bring-up, not the pipeline's steady state. */
static void stat_add(double *sum, unsigned *n, uint64_t t0, uint64_t t1)
{
    if (!strm.warm && t1 >= t0) {
        *sum += (double)(t1 - t0) / 1e6;
        (*n)++;
    }
}

/* The GPU path failed: close it, give back the capture buffer it held,
 * and have the next engine start ask for cached buffers again. */
static void gpu_fail(const char *why)
{
    if (strm.done.valid && strm.done.raw_held)
        requeue(strm.done.raw_idx);
    strm.done.valid = 0;
    gpu_debayer_close(gpu);     /* waits any pending fence */
    gpu = NULL;
    ipu_copy_close(ipu);
    ipu = NULL;
    gpu_disabled = 1;
    gpu_expect = 0;
    if (strm.rnd.valid) {
        requeue(strm.rnd.idx);
        strm.rnd.valid = 0;
    }
    fflog(LOG_WARNING, "cam: GPU pipeline failed (%s), falling back to "
          "the NEON path", why);
}

/* Encode the picture in the VPU JPEG encoder. NULL for a frame it
 * flagged (a single bad frame keeps the VPU) or a hard failure; three of
 * those in a row fall back to software for this engine run. */
static uint8_t *vpu_encode_jpeg(size_t *len)
{
    uint8_t *jpg = NULL;
    int rc = vpu_jpeg_encode(vpu, &jpg, len);
    if (rc >= 0) {
        vpu_hard_fails = 0;
        return rc == 0 ? jpg : NULL;
    }
    if (++vpu_hard_fails >= 3) {
        fflog(LOG_WARNING, "cam: repeated VPU encode failures, falling "
              "back to software");
        vpu_jpeg_close(vpu);
        vpu = NULL;
        vpu_disabled = 1;
    }
    return NULL;
}

/* Encode the picture waiting in the H.264 encoder and publish it. */
static void h264_emit(uint64_t ts_ns)
{
    pthread_mutex_lock(&eng.lock);
    int want_key = eng.h264_key_req;
    eng.h264_key_req = 0;
    pthread_mutex_unlock(&eng.lock);
    if (want_key)
        vpu_h264_force_key(h264);

    uint8_t *au = NULL;
    size_t aulen = 0;
    int key = 0;
    int rc = vpu_h264_encode(h264, &au, &aulen, &key);
    if (rc == 0) {
        h264_hard_fails = 0;
        pthread_mutex_lock(&h264_params_mx);
        if (h264_params)
            mp4mux_feed_params(h264_params, au, aulen);
        pthread_mutex_unlock(&h264_params_mx);
        pthread_mutex_lock(&eng.lock);
        free(eng.h264_au);
        eng.h264_au = au;
        eng.h264_len = aulen;
        eng.h264_key = key;
        eng.h264_pts = ts_ns * 9u / 100000u;    /* 90 kHz */
        eng.h264_seq++;
        eng.h264_up = 1;
        pthread_cond_broadcast(&eng.h264_cv);
        pthread_mutex_unlock(&eng.lock);
    } else if (rc < 0) {
        if (want_key) {
            pthread_mutex_lock(&eng.lock);
            eng.h264_key_req = 1;   /* not consumed */
            pthread_mutex_unlock(&eng.lock);
        }
        if (++h264_hard_fails >= 3) {
            fflog(LOG_WARNING, "cam: repeated H.264 encode failures, "
                  "dropping the H.264 stream");
            vpu_h264_close(h264);
            h264 = NULL;
            h264_off();
        }
    }
}

/* Publish a produced frame: the JPEG (ownership passes) to the MJPEG
 * viewers, and the picture waiting in the H.264 encoder, encoded now, to
 * the H.264 ones. */
static void publish(uint8_t *jpg, size_t len, int via_vpu, int gpu_made,
                    int h264_pending, uint64_t ts_ns)
{
    if (h264_pending && h264)
        h264_emit(ts_ns);
    if (!jpg && !h264_pending) {
        pthread_mutex_lock(&eng.lock);
        eng.gpu_active = gpu_made;
        pthread_mutex_unlock(&eng.lock);
        return;
    }
    stat_add(&strm.s_lat, &strm.n_lat, ts_ns, mono_ns());
    if (strm.warm)
        strm.warm--;
    strm.win_frames++;
    struct timespec t;
    now_ts(&t);
    double dt = ts_diff(&t, &strm.win_t0);

    pthread_mutex_lock(&eng.lock);
    if (jpg) {
        free(eng.stream_jpg);
        eng.stream_jpg = jpg;
        eng.stream_len = len;
        eng.vpu_active = via_vpu;
        eng.seq++;
        pthread_cond_broadcast(&eng.frame_cv);
    }
    eng.gpu_active = gpu_made;
    if (dt >= 2.0) {
        eng.fps = (double)strm.win_frames / dt;
        eng.t_latency = strm.n_lat ? strm.s_lat / strm.n_lat : 0;
        eng.t_convert = strm.n_conv ? strm.s_conv / strm.n_conv : 0;
        eng.t_copy = strm.n_copy ? strm.s_copy / strm.n_copy : 0;
        eng.t_encode = strm.n_enc ? strm.s_enc / strm.n_enc : 0;
        strm.win_frames = 0;
        strm.s_lat = strm.s_conv = strm.s_copy = strm.s_enc = 0;
        strm.n_lat = strm.n_conv = strm.n_copy = strm.n_enc = 0;
        strm.win_t0 = t;
    }
    pthread_mutex_unlock(&eng.lock);
}

/* Publish the held frame (a clean frame vouched for it). */
static void publish_held(void)
{
    uint8_t *jpg = strm.held_jpg;
    strm.held_jpg = NULL;
    strm.held = 0;
    strm.held_ok = 0;
    publish(jpg, strm.held_len, strm.held_vpu, strm.held_gpu,
            strm.held_h264, strm.held_ts);
    strm.held_h264 = 0;
}

/* Publish the held frame if a clean frame has vouched for it. This must
 * come before the next frame is written into the encoders' buffers: the
 * held frame's picture still waits in the H.264 encoder. */
static void publish_vouched(void)
{
    if (strm.held && strm.held_ok)
        publish_held();
}

/* Publish a produced frame now if the frame after it already came back
 * clean, or hold it for that verdict. */
static void hold_or_publish(uint8_t *jpg, size_t len, int via_vpu,
                            int gpu_made, int h264_pending, uint64_t ts_ns,
                            int confirmed)
{
    publish_vouched();
    if (confirmed) {
        publish(jpg, len, via_vpu, gpu_made, h264_pending, ts_ns);
        return;
    }
    strm_drop_held();
    strm.held = 1;
    strm.held_ok = 0;
    strm.held_ts = ts_ns;
    strm.held_jpg = jpg;
    strm.held_len = len;
    strm.held_vpu = via_vpu;
    strm.held_gpu = gpu_made;
    strm.held_h264 = h264_pending;
}

/* A clean frame came back: the frames before it are whole. The held one
 * is published by publish_vouched(), after a kick (so the H.264 encode
 * that goes with a publication never delays a render) or before the
 * encoders' buffers take the next picture, whichever comes first. */
static void vouch_for_older(void)
{
    if (strm.held)
        strm.held_ok = 1;
    if (strm.done.valid && !strm.done.vetoed)
        strm.done.confirmed = 1;
    if (strm.rnd.valid && !strm.rnd_vetoed)
        strm.rnd_confirmed = 1;
}

/* A flagged frame came back: nothing next to it is trusted. The held
 * frame and the one waiting are dropped, the render in flight is dropped
 * when it lands unless a clean frame already vouched for it, a snapshot
 * candidate goes back, and the next clean frame is dropped too. */
static void withhold_neighbors(void)
{
    if (strm.held && !strm.held_ok) {
        strm_drop_held();
        count_up(&eng.withheld);
    }
    if (strm.nxt.valid) {
        requeue(strm.nxt.idx);
        strm.nxt.valid = 0;
        count_up(&eng.withheld);
    }
    if (strm.done.valid && !strm.done.confirmed)
        strm.done.vetoed = 1;
    if (strm.rnd.valid && !strm.rnd_confirmed)
        strm.rnd_vetoed = 1;
    if (strm.snap.valid) {
        requeue(strm.snap.idx);
        strm.snap.valid = 0;
    }
    strm.taint = 1;
}

/* The render in flight landed: its capture buffer goes back to the
 * queue (after the one-shot GPU check has read it, when that is still
 * to run), and the picture waits in its IPU source slot for the crop. */
static void finish_render(void)
{
    struct pframe f = strm.rnd;
    strm.rnd.valid = 0;
    int rc = gpu_debayer_wait(gpu, strm.rnd_slot);
    uint64_t t1 = mono_ns();
    if (rc) {
        gpu_fail("the render did not finish");
        requeue(f.idx);
        return;
    }
    stat_add(&strm.s_conv, &strm.n_conv, strm.rnd_kick_ns, t1);
    strm.done.valid = 1;
    strm.done.slot = strm.rnd_slot;
    strm.done.ts_ns = f.ts_ns;
    strm.done.vetoed = strm.rnd_vetoed;
    strm.done.confirmed = strm.rnd_confirmed;
    strm.done.raw_held = gpu_check && !gpu_checked;
    strm.done.raw_idx = f.idx;
    if (!strm.done.raw_held)
        requeue(f.idx);
}

/* Crop the rendered picture into the wanted encoders, encode the JPEG,
 * and publish the frame or hold it for the next frame's verdict; a
 * render whose neighbor came back flagged is dropped instead. */
static void deliver_render(int jpeg_want, int h264_want, uint8_t *raw_cached)
{
    int slot = strm.done.slot;
    int confirmed = strm.done.confirmed;
    int raw_held = strm.done.raw_held;
    unsigned raw_idx = strm.done.raw_idx;
    uint64_t ts_ns = strm.done.ts_ns;
    int vetoed = strm.done.vetoed;
    strm.done.valid = 0;
    if (vetoed)
        count_up(&eng.withheld);
    if (vetoed || !(jpeg_want || h264_want) || !ipu) {
        if (raw_held)
            requeue(raw_idx);
        return;
    }

    uint64_t t1 = mono_ns();
    int jpeg_ok = 0, h264_ok = 0;
    if (jpeg_want && vpu) {
        int stride;
        size_t len;
        int fd = vpu_jpeg_out_dmabuf(vpu, &stride, &len);
        jpeg_ok = fd >= 0 && ipu_copy_run(ipu, slot, fd, len) == 0;
    }
    if (h264_want && h264) {
        int stride;
        size_t len;
        int fd = vpu_h264_out_dmabuf(h264, &stride, &len);
        h264_ok = fd >= 0 && ipu_copy_run(ipu, slot, fd, len) == 0;
    }
    uint64_t t2 = mono_ns();
    if ((jpeg_want && vpu && !jpeg_ok) || (h264_want && h264 && !h264_ok)) {
        if (raw_held)
            requeue(raw_idx);
        gpu_fail("the IPU crop failed");
        return;
    }
    stat_add(&strm.s_copy, &strm.n_copy, t1, t2);
    if (raw_held) {
        if (jpeg_ok)
            gpu_check_once(prepare_raw8(eng.bufs[raw_idx].start, raw_cached,
                                        eng.prof),
                           eng.prof->w, eng.prof->h, HFLIP, vpu, slot);
        requeue(raw_idx);
    }

    uint8_t *jpg = NULL;
    size_t len = 0;
    if (jpeg_ok) {
        jpg = vpu_encode_jpeg(&len);
        if (jpg)
            stat_add(&strm.s_enc, &strm.n_enc, t2, mono_ns());
    }
    hold_or_publish(jpg, len, jpg != NULL, 1, h264_ok, ts_ns, confirmed);
}

/* Finish the render in flight and, with no pollable fence (nothing then
 * overlaps anyway), deliver it at once. */
static void land_render(int jpeg_want, int h264_want, uint8_t *raw_cached,
                        int deliver)
{
    if (strm.done.valid)
        deliver_render(jpeg_want, h264_want, raw_cached);
    finish_render();
    if (deliver && strm.done.valid)
        deliver_render(jpeg_want, h264_want, raw_cached);
}

/* Produce a frame on the CPU: demosaic into each wanted encoder (twice
 * with both wanted - the GPU path is how that cost is meant to be paid),
 * encode the JPEG, and hold the frame for the next one's verdict. H.264
 * has no software fallback (the MJPEG stream is the fallback). */
static void produce_cpu(struct pframe *f, int jpeg_want, int h264_want,
                        uint8_t *raw_cached, uint8_t *rgb_half)
{
    const struct sensor_profile *p = eng.prof;
    const int raw_w = p->w, raw_h = p->h;
    const int half_w = raw_w / 2, half_h = raw_h / 2;
    uint64_t t0 = mono_ns();
    const uint8_t *raw = prepare_raw8(eng.bufs[f->idx].start, raw_cached, p);
    uint8_t *jpg = NULL;
    size_t len = 0;
    int via_vpu = 0, h264_pending = 0;
    uint64_t t1 = 0;

    if (jpeg_want && vpu) {
        uint8_t *yp, *up, *vp;
        int ys, uvs;
        vpu_jpeg_planes(vpu, &yp, &up, &vp, &ys, &uvs);
        debayer_bggr_half_yuv420(raw, raw_w, raw_h, HFLIP,
                                 yp, ys, up, vp, uvs);
#ifdef __ARM_NEON
        /* One-shot NEON-vs-scalar equivalence check on a live frame (the
         * paths are constructed to be bit-identical; this proves it on
         * real data). Stride == width here. */
        static int neon_checked;
        if (neon_check && !neon_checked) {
            neon_checked = 1;
            size_t ysz = (size_t)ys * half_h;
            size_t usz = (size_t)uvs * (half_h / 2);
            uint8_t *ry = malloc(ysz), *ru = malloc(usz), *rv = malloc(usz);
            if (ry && ru && rv) {
                debayer_bggr_half_yuv420_scalar(raw, raw_w, raw_h, HFLIP,
                                                ry, ys, ru, rv, uvs);
                fflog(LOG_DEBUG, "cam: NEON/scalar compare: %s",
                      (!memcmp(ry, yp, ysz) && !memcmp(ru, up, usz) &&
                       !memcmp(rv, vp, usz)) ? "IDENTICAL" : "MISMATCH");
            }
            free(ry);
            free(ru);
            free(rv);
        }
#endif
        t1 = mono_ns();
        jpg = vpu_encode_jpeg(&len);
        via_vpu = jpg != NULL;
    }
    if (jpeg_want && !vpu) {
        debayer_bggr_half(raw, rgb_half, raw_w, raw_h, HFLIP);
        t1 = mono_ns();
        if (jpeg_encode_rgb(rgb_half, half_w, half_h, eng.stream_quality, 1,
                            &jpg, &len))
            jpg = NULL;
    }
    if (jpg) {
        stat_add(&strm.s_conv, &strm.n_conv, t0, t1);
        stat_add(&strm.s_enc, &strm.n_enc, t1, mono_ns());
    }
    if (h264_want && h264) {
        uint8_t *yp, *up, *vp;
        int ys, uvs;
        vpu_h264_planes(h264, &yp, &up, &vp, &ys, &uvs);
        debayer_bggr_half_yuv420(raw, raw_w, raw_h, HFLIP,
                                 yp, ys, up, vp, uvs);
        h264_pending = 1;
    }
    requeue(f->idx);
    f->valid = 0;
    hold_or_publish(jpg, len, via_vpu, 0, h264_pending, f->ts_ns, 0);
}

/* Feed the newest clean frame to the encoders: into the GPU when every
 * wanted output has its hardware encoder (the render is finished when
 * its fence signals), else through the CPU. A held frame a clean one
 * vouched for is published first wherever the new frame reaches the
 * encoders' buffers now; after a kick it is published once the render
 * is under way (the caller), so the render never waits on it. */
static void feed_frame(int jpeg_want, int h264_want, uint8_t *raw_cached,
                       uint8_t *rgb_half)
{
    const struct sensor_profile *p = eng.prof;
    ensure_encoders(jpeg_want, h264_want, p->w / 2, p->h / 2, HFLIP);
    if (gpu && ipu && (!jpeg_want || vpu) && (!h264_want || h264)) {
        int slot = strm.slot_next;
        if (gpu_debayer_kick(gpu, (int)strm.nxt.idx, slot) == 0) {
            strm.slot_next ^= 1;
            strm.rnd = strm.nxt;
            strm.nxt.valid = 0;
            strm.rnd_slot = slot;
            strm.rnd_vetoed = strm.rnd_confirmed = 0;
            strm.rnd_kick_ns = mono_ns();
            /* Without a pollable fence the render is finished now. */
            if (gpu_debayer_fence_fd(gpu, slot) < 0) {
                publish_vouched();
                land_render(jpeg_want, h264_want, raw_cached, 1);
            }
            return;
        }
        gpu_fail("the render was refused");
    }
    publish_vouched();
    produce_cpu(&strm.nxt, jpeg_want, h264_want, raw_cached, rgb_half);
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "cam-engine");
    /* Sized for the largest profile so a snapshot borrow of a
     * differently-modeled camera reuses them. */
    uint8_t *rgb_half = malloc(max_half_rgb_bytes());
    uint8_t *rgb_full = NULL;   /* grown on first full-res snapshot */
    size_t rgb_full_cap = 0;
    /* The 8-bit BGGR frame the demosaic paths read - the narrowing target
     * for a 10-bit sensor and the bounce buffer for an 8-bit one on
     * uncached capture buffers (see prepare_raw8). */
    uint8_t *raw_cached = malloc(max_raw8_bytes());
    int dq_timeouts = 0;
    int lamp_skip = 0;      /* frames left to drain after a lamp override */
    int lamp_restore = -1;  /* engine lamp level to restore, -1 = none */
    int lamp_applied = eng.lamp_level;  /* what the served camera's lamp is at */
    /* FPS cap pacing (eng.fps_cap is set once at init) */
    double cap_period = eng.fps_cap > 0 ? 1.0 / eng.fps_cap : 0;
    double next_due = 0;
    now_ts(&strm.win_t0);
    strm.win_frames = 0;
    strm.warm = 2;
    strm.qbuf_failed = 0;

    if (!rgb_half || !raw_cached) {
        fflog(LOG_ERR, "cam: worker OOM");
        goto out;
    }

    for (;;) {
        struct timespec now;
        now_ts(&now);
        pthread_mutex_lock(&eng.lock);
        int stop = eng.stop_flag;
        int clients = eng.clients;
        int h264c = eng.h264_clients;
        int snap = eng.snap_pending == 1 && eng.snap_cam == eng.home_cam;
        int borrow = eng.snap_pending == 1 && eng.snap_cam != eng.home_cam;
        int snap_lamp = eng.snap_lamp;
        int snap_local = eng.snap_local;
        int open_ok = eng.cam == CAM_HEAD && eng.head_open_ok;
        int want_lamp = eng.stream_lamp >= 0 ? eng.stream_lamp : eng.lamp_level;
        int h264_off_now = h264_disabled;
        cam_id_t borrow_cam = eng.snap_cam;
        cam_id_t orig_cam = eng.home_cam;
        int idle = clients == 0 && h264c == 0 && !snap && !borrow &&
                   ts_diff(&now, &eng.last_activity) > IDLE_STOP_S;
        pthread_mutex_unlock(&eng.lock);

        if (stop || idle || strm.qbuf_failed)
            break;

        /* A snapshot candidate outlives no request: one given up or
         * answered elsewhere goes back. */
        if (!snap && strm.snap.valid) {
            requeue(strm.snap.idx);
            strm.snap.valid = 0;
        }

        /* Privacy gate, re-checked every frame: a lid opened mid-capture
         * tears the pipeline down within one frame time, so streams end
         * and the sensors stop rather than filming the room. A pending
         * snapshot fails with the same refusal (see the exit path). The
         * head camera a local viewer has goes on; the other viewers'
         * streams end in their next(), and a snapshot that is not a local
         * viewer's is refused here. */
        int lid_closed = machine_lid_closed();
        if (!lid_closed && !open_ok) {
            fflog(LOG_INFO, "cam: lid opened, stopping capture");
            pthread_mutex_lock(&eng.lock);
            eng.lid_stopped = 1;
            pthread_mutex_unlock(&eng.lock);
            break;
        }
        if (!lid_closed && (snap || borrow) && !snap_local) {
            pthread_mutex_lock(&eng.lock);
            eng.snap_lid = 1;
            pthread_mutex_unlock(&eng.lock);
            fail_snap();
            continue;
        }

        /* A stream client's lamp level, or the engine's again when none
         * asks, outside a snapshot's own relight. */
        if (lamp_restore < 0 && want_lamp != lamp_applied) {
            sysfs_write_int(camdefs[orig_cam].lamp, want_lamp);
            lamp_applied = want_lamp;
        }

        /* Same-camera snapshot with a lamp override: relight, then let
         * the in-flight frames drain before choosing one; the engine
         * level is restored after delivery (or if the requester gives
         * up). */
        if (snap && snap_lamp >= 0 && lamp_restore < 0) {
            sysfs_write_int(camdefs[orig_cam].lamp, snap_lamp);
            lamp_restore = lamp_applied;
            lamp_skip = LAMP_SKIP_FRAMES;
        } else if (!snap && lamp_restore >= 0) {
            sysfs_write_int(camdefs[orig_cam].lamp, lamp_restore);
            lamp_restore = -1;
        }

        /* Cross-camera snapshot: borrow the mux - pause the stream,
         * switch, grab one frame, switch back. Stream clients just see
         * the frame gap (a few seconds). */
        if (borrow) {
            char berr[128];
            release_capture();
            if (start_capture(borrow_cam, snap_local, berr, sizeof(berr)) == 0) {
                int skip = 0;
                if (snap_lamp >= 0) {
                    sysfs_write_int(camdefs[borrow_cam].lamp, snap_lamp);
                    skip = LAMP_SKIP_FRAMES;
                }
                if (grab_one_snap(raw_cached, rgb_half, &rgb_full,
                                  &rgb_full_cap, skip))
                    fail_snap();
                release_capture();
            } else {
                fflog(LOG_ERR, "cam: borrow start failed: %s", berr);
                fail_snap();
            }
            if (start_capture(orig_cam, open_ok, berr, sizeof(berr))) {
                fflog(LOG_ERR, "cam: restore after borrow failed: %s",
                      berr);
                break;  /* engine dies; streams end; reconnect heals */
            }
            lamp_applied = eng.lamp_level;      /* start_capture lit it at that */
            continue;
        }

        int jpeg_want = clients > 0;
        int h264_want = h264c > 0 && !h264_off_now;

        /* A render that has not landed in seconds is a hung GPU: the
         * wait gives up on it and the path falls back. */
        if (strm.rnd.valid && mono_ns() - strm.rnd_kick_ns > 3000000000ull)
            land_render(jpeg_want, h264_want, raw_cached, 0);

        /* Wait for a frame from the receiver or the render in flight. */
        struct pollfd pf[2] = {
            { .fd = eng.fd, .events = POLLIN },
            { .fd = -1, .events = POLLIN },
        };
        if (strm.rnd.valid)
            pf[1].fd = gpu_debayer_fence_fd(gpu, strm.rnd_slot);
        int r = poll(pf, pf[1].fd >= 0 ? 2 : 1, DQ_TIMEOUT_S * 1000);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            fflog(LOG_ERR, "cam: poll: %s", strerror(errno));
            break;
        }
        if (r == 0) {
            if (++dq_timeouts >= MAX_DQ_TIMEOUTS) {
                fflog(LOG_WARNING, "cam: %d consecutive frame timeouts, "
                      "stopping engine", dq_timeouts);
                break;
            }
            continue;
        }

        /* The render landed: finish it, so its capture buffer is back
         * before the drain and the GPU is free for the next frame. */
        if (pf[1].fd >= 0 && pf[1].revents && strm.rnd.valid)
            land_render(jpeg_want, h264_want, raw_cached, 0);

        /* Drain every finished frame, oldest first. */
        int stop_engine = 0;
        while (pf[0].revents) {
            struct v4l2_buffer buf = {0};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            if (xioctl(eng.fd, VIDIOC_DQBUF, &buf) < 0) {
                if (errno == EAGAIN)
                    break;
                /* EIO: the queue is in error (an end-of-frame timeout);
                 * only a fresh engine start clears that. */
                if (errno == EIO && ++dq_timeouts < MAX_DQ_TIMEOUTS)
                    break;
                fflog(LOG_ERR, "cam: DQBUF: %s", strerror(errno));
                stop_engine = 1;
                break;
            }
            dq_timeouts = 0;
            if (flag_every && ++flag_count % flag_every == 0)
                buf.flags |= V4L2_BUF_FLAG_ERROR;

            /* An errored frame is short, torn or off-sync; demosaicing
             * it would publish a corrupt image. The ladder drops it,
             * cycles the queue if they keep coming, and gives up if
             * cycling stops helping. */
            cam_frame_action_t act = classify(&buf);
            if (act == CAM_FRAME_WARMUP) {
                strm.taint = 0;
                requeue(buf.index);
                continue;
            }
            if (act != CAM_FRAME_USE) {
                withhold_neighbors();
                requeue(buf.index);
                if (act == CAM_FRAME_DROP)
                    continue;
                if (act == CAM_FRAME_ABORT) {
                    fflog(LOG_ERR, "cam: corrupt frames persist after %d "
                          "stream restarts, stopping engine",
                          CAM_MAX_RECOVERIES);
                    stop_engine = 1;
                    break;
                }
                /* A queue cycle requeues every buffer, including one
                 * held for a render or the GPU check: settle the render
                 * first so the pipeline restarts from empty. */
                if (strm.rnd.valid)
                    gpu_debayer_wait(gpu, strm.rnd_slot);
                strm_reset();
                if (restream()) {
                    fflog(LOG_ERR, "cam: stream restart failed: %s",
                          strerror(errno));
                    stop_engine = 1;
                    break;
                }
                pthread_mutex_lock(&eng.lock);
                unsigned n = eng.recoveries;
                uint64_t nbad = eng.corrupt, nall = eng.frames;
                pthread_mutex_unlock(&eng.lock);
                fflog(LOG_WARNING, "cam: corrupt frames, restarted the "
                      "stream (%u restarts, %llu of %llu frames bad)", n,
                      (unsigned long long)nbad, (unsigned long long)nall);
                break;
            }

            /* A clean frame. The one right after a flagged frame is not
             * trusted either; any other vouches for the frames before
             * it. */
            if (strm.taint) {
                strm.taint = 0;
                requeue(buf.index);
                count_up(&eng.withheld);
                continue;
            }
            vouch_for_older();
            struct pframe f = { 1, buf.index, buf_ts_ns(&buf) };

            /* A same-camera snapshot rides on the stream's frames: after
             * any lamp drain a frame becomes the candidate, and the next
             * clean frame vouches for it. */
            if (snap) {
                if (strm.snap.valid) {
                    deliver_snap(prepare_raw8(eng.bufs[strm.snap.idx].start,
                                              raw_cached, eng.prof),
                                 rgb_half, &rgb_full, &rgb_full_cap);
                    requeue(strm.snap.idx);
                    strm.snap.valid = 0;
                    snap = 0;
                    if (lamp_restore >= 0) {
                        sysfs_write_int(camdefs[orig_cam].lamp,
                                        lamp_restore);
                        lamp_restore = -1;
                    }
                } else if (lamp_skip > 0) {
                    lamp_skip--;
                } else {
                    strm.snap = f;
                    if (strm.nxt.valid) {
                        requeue(strm.nxt.idx);
                        strm.nxt.valid = 0;
                    }
                    continue;
                }
            }

            /* The newest clean frame is the stream's next one; an older
             * one still waiting goes back, so no frame is served stale. */
            if (strm.nxt.valid) {
                requeue(strm.nxt.idx);
                count_up(&eng.skipped);
            }
            strm.nxt = f;
        }
        if (stop_engine)
            break;

        /* Feed the newest frame to the encoders once the GPU is free.
         * The fps cap passes over a frame that arrives before its due
         * time (the sensor keeps its own pace); with the CSI hardware
         * skip active the rates already match and nothing is passed
         * over. */
        if (strm.nxt.valid && !strm.rnd.valid) {
            double t = (double)strm.nxt.ts_ns / 1e9;
            int due = 1;
            if (cap_period > 0) {
                if (t < next_due) {
                    due = 0;
                } else {
                    next_due += cap_period;
                    if (next_due <= t)      /* first frame or fell behind */
                        next_due = t + cap_period;
                }
            }
            if (due && (jpeg_want || h264_want)) {
                feed_frame(jpeg_want, h264_want, raw_cached, rgb_half);
            } else {
                requeue(strm.nxt.idx);
                strm.nxt.valid = 0;
            }
        }

        /* Crop and encode the render the GPU finished (after the feed,
         * so a frame that came in meanwhile is already rendering), and
         * publish a held frame a clean one vouched for. */
        if (strm.done.valid)
            deliver_render(jpeg_want, h264_want, raw_cached);
        publish_vouched();
    }

out:
    release_capture();
    free(rgb_half);
    free(rgb_full);
    free(raw_cached);
    pthread_mutex_lock(&eng.lock);
    eng.running = 0;
    eng.h264_up = 0;
    eng.head_open_ok = 0;
    /* fail any waiter: stream clients see running==0, a pending snapshot
     * is marked failed */
    if (eng.snap_pending == 1)
        eng.snap_pending = 3;
    pthread_cond_broadcast(&eng.frame_cv);
    pthread_cond_broadcast(&eng.snap_cv);
    pthread_cond_broadcast(&eng.h264_cv);
    pthread_mutex_unlock(&eng.lock);
    return NULL;
}

/* ------------------------------------------------------- engine control */

/* With ctl held: make the engine run on `cam`. Fails if clients hold the
 * other camera. local: the requester is a local viewer, which lets the
 * head camera run with the lid open. */
static int ensure_engine(cam_id_t cam, int local, char *err, size_t errlen)
{
    for (;;) {
        pthread_mutex_lock(&eng.lock);
        int running = eng.running;
        /* Compare against the HOME camera: during a snapshot borrow the
         * pipeline (eng.cam) is briefly on the other sensor, and a stream
         * request racing that window must not attach to it. */
        cam_id_t cur = eng.home_cam;
        int clients = eng.clients + eng.h264_clients;
        int tid_valid = eng.tid_valid;
        pthread_mutex_unlock(&eng.lock);

        if (running && cur == cam) {
            if (cam == CAM_HEAD && local) {
                pthread_mutex_lock(&eng.lock);
                eng.head_open_ok = 1;
                pthread_mutex_unlock(&eng.lock);
            }
            return 0;
        }

        if (running && cur != cam) {
            if (clients > 0) {
                /* Last request wins: preempt the current stream clients
                 * (single-operator machine - the newest ask is the
                 * operator). Kicked clients wake, end their streams
                 * cleanly (viewers freeze on their last frame), and
                 * release their pins; wait for that to drain. */
                pthread_mutex_lock(&eng.lock);
                eng.kick_gen++;
                pthread_cond_broadcast(&eng.frame_cv);
                pthread_cond_broadcast(&eng.h264_cv);
                pthread_mutex_unlock(&eng.lock);
                struct timespec t0, t;
                now_ts(&t0);
                do {
                    usleep(100 * 1000);
                    pthread_mutex_lock(&eng.lock);
                    clients = eng.clients + eng.h264_clients;
                    pthread_mutex_unlock(&eng.lock);
                    now_ts(&t);
                } while (clients > 0 && ts_diff(&t, &t0) < SWITCH_GRACE_S);
                if (clients > 0) {
                    snprintf(err, errlen,
                             "camera switch timed out: %d client(s) still "
                             "attached to %s", clients, camdefs[cur].name);
                    return -1;
                }
            }
            pthread_mutex_lock(&eng.lock);
            eng.stop_flag = 1;
            pthread_mutex_unlock(&eng.lock);
            /* worker notices at the next tick (<= DQ_TIMEOUT_S) */
        }

        if (tid_valid) {
            pthread_join(eng.tid, NULL);
            pthread_mutex_lock(&eng.lock);
            eng.tid_valid = 0;
            eng.stop_flag = 0;
            pthread_mutex_unlock(&eng.lock);
            continue;   /* re-evaluate from a clean state */
        }

        /* cold start */
        pthread_mutex_lock(&eng.lock);
        eng.home_cam = cam;
        pthread_mutex_unlock(&eng.lock);
        /* Every engine start probes the hardware paths again: one
         * transient VPU, H.264 or GPU failure must not demote the stream
         * to the CPU paths for the daemon's lifetime. */
        pthread_mutex_lock(&eng.lock);
        vpu_disabled = env_no_vpu;
        h264_disabled = env_no_h264;
        gpu_disabled = env_no_gpu;
        pthread_mutex_unlock(&eng.lock);
        vpu_hard_fails = h264_hard_fails = 0;
        if (start_capture(cam, local, err, errlen))
            return -1;
        pthread_mutex_lock(&eng.lock);
        eng.running = 1;
        eng.head_open_ok = cam == CAM_HEAD && local;
        eng.stop_flag = 0;
        eng.seq = 0;
        eng.fps = 0;
        now_ts(&eng.last_activity);
        if (pthread_create(&eng.tid, NULL, worker, NULL)) {
            eng.running = 0;
            pthread_mutex_unlock(&eng.lock);
            release_capture();
            snprintf(err, errlen, "worker thread creation failed");
            return -1;
        }
        eng.tid_valid = 1;
        pthread_mutex_unlock(&eng.lock);
        return 0;
    }
}

void cam_engine_init(void)
{
    /* The waits are timeouts, and timeouts never ride the wall clock on
     * this RTC-less board: the condvars wake on CLOCK_MONOTONIC. */
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&eng.frame_cv, &ca);
    pthread_cond_init(&eng.snap_cv, &ca);
    pthread_cond_init(&eng.h264_cv, &ca);
    pthread_condattr_destroy(&ca);
    const char *v;
    if ((v = getenv("FORGECTRL_STREAM_Q")) != NULL) {
        int q = atoi(v);
        if (q >= 1 && q <= 100)
            eng.stream_quality = q;
    }
    if ((v = getenv("FORGECTRL_LAMP")) != NULL) {
        int l = atoi(v);
        if (l >= 0 && l <= 1023)
            eng.lamp_level = l;
    }
    if ((v = getenv("FORGECTRL_STREAM_FPS")) != NULL) {
        double f = atof(v);
        if (f > 0 && f <= 60)
            eng.fps_cap = f;
    }
    if ((v = getenv("FORGECTRL_H264_KBPS")) != NULL) {
        int k = atoi(v);
        if (k >= 100 && k <= 20000)
            eng.h264_kbps = k;
    }
    if ((v = getenv("FORGECTRL_H264_GOP")) != NULL) {
        int gp = atoi(v);
        if (gp >= 1 && gp <= 300)
            eng.h264_gop = gp;
    }
    vpu_disabled = env_no_vpu = getenv("FORGECTRL_NO_VPU") != NULL;
    h264_disabled = env_no_h264 = getenv("FORGECTRL_NO_H264") != NULL;
    gpu_disabled = env_no_gpu = getenv("FORGECTRL_NO_GPU") != NULL;
    gpu_check = getenv("FORGECTRL_GPU_CHECK") != NULL;
    neon_check = getenv("FORGECTRL_NEON_CHECK") != NULL;
    if ((v = getenv("FORGECTRL_FLAG_EVERY")) != NULL && atoi(v) >= 2)
        flag_every = (unsigned)atoi(v);
    if (getenv("FORGECTRL_NO_HW_SKIP"))
        hw_skip_disabled = 1;
    if (getenv("FORGECTRL_NO_CACHED_BUFS"))
        cached_disabled = 1;

    /* Resolve which sensor bound on each bus now, so /cam/status can name
     * the model and its geometry before the engine has ever run. Absent or
     * unrecognized is not an error here - start_capture is where that
     * matters. Refreshed on every engine start. */
    for (int i = 0; i < 2; i++) {
        char entity[64];
        if (sensor_entity(camdefs[i].bus, entity, sizeof(entity)))
            continue;
        eng.seen[i] = profile_for(entity);
        fflog(LOG_INFO, "cam: %s camera is %s", camdefs[i].name,
              eng.seen[i] ? eng.seen[i]->model : entity);
    }
}

void cam_engine_shutdown(void)
{
    pthread_mutex_lock(&eng.ctl);
    pthread_mutex_lock(&eng.lock);
    int tid_valid = eng.tid_valid;
    eng.stop_flag = 1;
    pthread_mutex_unlock(&eng.lock);
    if (tid_valid) {
        pthread_join(eng.tid, NULL);
        pthread_mutex_lock(&eng.lock);
        eng.tid_valid = 0;
        pthread_mutex_unlock(&eng.lock);
    }
    if (vpu) {
        vpu_jpeg_close(vpu);
        vpu = NULL;
    }
    if (h264) {
        vpu_h264_close(h264);
        h264 = NULL;
    }
    if (ipu) {
        ipu_copy_close(ipu);
        ipu = NULL;
    }
    pthread_mutex_lock(&h264_params_mx);
    mp4mux_free(h264_params);
    h264_params = NULL;
    pthread_mutex_unlock(&h264_params_mx);
    pthread_mutex_unlock(&eng.ctl);
}

/* ------------------------------------------------------------ snapshots */

int cam_snapshot(cam_id_t cam, int full, int quality, int lamp, int local,
                 uint8_t **jpeg, size_t *len, char *err, size_t errlen)
{
    /* Refuse before taking the control mutex: an open lid is answered
     * immediately, not after the snapshot timeout. start_capture() and
     * the worker enforce the same rule, so a lid that opens during the
     * wait still ends the request. */
    if (!machine_lid_closed() && !(cam == CAM_HEAD && local)) {
        snprintf(err, errlen, "%s", CAM_ERR_LID);
        return -1;
    }

    pthread_mutex_lock(&eng.ctl);

    /* If the engine is streaming the OTHER camera for active clients,
     * don't switch it - post the request and let the worker borrow the
     * mux for one frame. Otherwise make the engine run on `cam`. */
    pthread_mutex_lock(&eng.lock);
    int streaming_other = eng.running && eng.home_cam != cam &&
                          eng.clients > 0;
    pthread_mutex_unlock(&eng.lock);

    if (!streaming_other && ensure_engine(cam, local, err, errlen)) {
        pthread_mutex_unlock(&eng.ctl);
        return -1;
    }

    pthread_mutex_lock(&eng.lock);
    eng.snap_pending = 1;
    eng.snap_local = local;
    eng.snap_lid = 0;
    eng.snap_cam = cam;
    eng.snap_full = full;
    eng.snap_quality = quality;
    eng.snap_lamp = lamp;
    now_ts(&eng.last_activity);

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += SNAP_TIMEOUT_S;
    int rc = 0;
    while (eng.snap_pending == 1) {
        if (pthread_cond_timedwait(&eng.snap_cv, &eng.lock, &deadline)
            == ETIMEDOUT) {
            rc = ETIMEDOUT;
            break;
        }
    }
    if (rc == 0 && eng.snap_pending == 2) {
        *jpeg = eng.snap_jpg;
        *len = eng.snap_len;
        eng.snap_jpg = NULL;
        eng.snap_len = 0;
        eng.snap_pending = 0;
    } else if (eng.lid_stopped || eng.snap_lid) {
        /* The lid opened while this snapshot was in flight: report the
         * refusal rather than a generic failure, so the caller sees the
         * same answer it would have got a moment earlier. */
        snprintf(err, errlen, "%s", CAM_ERR_LID);
        eng.snap_pending = 0;
        rc = -1;
    } else {
        if (eng.snap_pending == 1 || eng.snap_pending == 3)
            snprintf(err, errlen, rc == ETIMEDOUT ?
                     "snapshot timed out" : "snapshot capture failed");
        eng.snap_pending = 0;
        rc = -1;
    }
    now_ts(&eng.last_activity);
    pthread_mutex_unlock(&eng.lock);
    pthread_mutex_unlock(&eng.ctl);
    return rc == 0 ? 0 : -1;
}

/* --------------------------------------------------------- stream client */

struct cam_client {
    uint64_t last_seq;
    uint64_t gen;       /* kick generation at open; a bump ends the stream */
    uint8_t *buf;
    size_t   cap;
    int      local;     /* a local viewer: the head camera's frames reach it with the lid open */
    double   period;    /* at most one frame per this many seconds, 0 = every frame */
    double   next_due;  /* CLOCK_MONOTONIC */
};

static double mono_now(void)
{
    struct timespec ts;
    now_ts(&ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

cam_client_t *cam_client_open(cam_id_t cam, int local, double fps, int lamp,
                              char *err, size_t errlen)
{
    if (!machine_lid_closed() && !(cam == CAM_HEAD && local)) {
        snprintf(err, errlen, "%s", CAM_ERR_LID);
        return NULL;
    }
    pthread_mutex_lock(&eng.ctl);
    if (ensure_engine(cam, local, err, errlen)) {
        pthread_mutex_unlock(&eng.ctl);
        return NULL;
    }
    cam_client_t *c = calloc(1, sizeof(*c));
    if (!c) {
        pthread_mutex_unlock(&eng.ctl);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    c->local = local;
    c->period = fps > 0 ? 1.0 / fps : 0;
    pthread_mutex_lock(&eng.lock);
    eng.clients++;
    if (lamp >= 0)
        eng.stream_lamp = lamp;
    c->gen = eng.kick_gen;
    now_ts(&eng.last_activity);
    pthread_mutex_unlock(&eng.lock);
    pthread_mutex_unlock(&eng.ctl);
    return c;
}

/* A viewer that is not local sees nothing with the lid open. When the
 * engine runs on regardless (the head camera, held open for a local
 * viewer), such a viewer ends itself here; otherwise the worker stops the
 * capture within a frame, records that the lid stopped it, and the viewer
 * ends through that teardown like every other. */
static int lid_ends_viewer(int local)
{
    if (local || machine_lid_closed())
        return 0;
    pthread_mutex_lock(&eng.lock);
    int runs_on = eng.running && eng.cam == CAM_HEAD && eng.head_open_ok;
    pthread_mutex_unlock(&eng.lock);
    return runs_on;
}

long cam_client_next(cam_client_t *c, const uint8_t **jpeg)
{
    if (lid_ends_viewer(c->local))
        return -1;
    /* A paced client waits out its period first, then takes the newest
     * frame: the ones between are dropped for it alone. */
    double wait = c->period > 0 ? c->next_due - mono_now() : 0;
    if (wait > 0) {
        struct timespec ts = { (time_t)wait, (long)((wait - (double)(time_t)wait) * 1e9) };
        nanosleep(&ts, NULL);
    }
    pthread_mutex_lock(&eng.lock);
    int timeouts = 0;
    while (eng.running && c->gen == eng.kick_gen && eng.seq <= c->last_seq) {
        struct timespec deadline;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += CLIENT_WAIT_S;
        if (pthread_cond_timedwait(&eng.frame_cv, &eng.lock, &deadline)
            == ETIMEDOUT && ++timeouts >= 2)
            break;
    }
    if (!eng.running || c->gen != eng.kick_gen || eng.seq <= c->last_seq) {
        pthread_mutex_unlock(&eng.lock);
        return -1;
    }
    if (c->cap < eng.stream_len) {
        uint8_t *nb = realloc(c->buf, eng.stream_len);
        if (!nb) {
            pthread_mutex_unlock(&eng.lock);
            return -1;
        }
        c->buf = nb;
        c->cap = eng.stream_len;
    }
    memcpy(c->buf, eng.stream_jpg, eng.stream_len);
    long len = (long)eng.stream_len;
    c->last_seq = eng.seq;
    now_ts(&eng.last_activity);
    pthread_mutex_unlock(&eng.lock);
    if (c->period > 0)
        c->next_due = mono_now() + c->period;
    *jpeg = c->buf;
    return len;
}

void cam_client_close(cam_client_t *c)
{
    if (!c)
        return;
    pthread_mutex_lock(&eng.lock);
    if (eng.clients > 0)
        eng.clients--;
    if (eng.clients == 0)
        eng.stream_lamp = -1;
    now_ts(&eng.last_activity);
    pthread_mutex_unlock(&eng.lock);
    free(c->buf);
    free(c);
}

/* ---------------------------------------------------- H.264 stream client */

struct cam_h264_client {
    uint64_t last_seq;
    uint64_t gen;
    int      started;   /* first delivered unit must be an IDR */
    uint8_t *buf;
    size_t   cap;
    int      local;
};

cam_h264_client_t *cam_h264_client_open(cam_id_t cam, int local, char *err,
                                        size_t errlen)
{
    if (!machine_lid_closed() && !(cam == CAM_HEAD && local)) {
        snprintf(err, errlen, "%s", CAM_ERR_LID);
        return NULL;
    }
    pthread_mutex_lock(&eng.ctl);
    if (ensure_engine(cam, local, err, errlen)) {
        pthread_mutex_unlock(&eng.ctl);
        return NULL;
    }
    cam_h264_client_t *c = calloc(1, sizeof(*c));
    if (!c) {
        pthread_mutex_unlock(&eng.ctl);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    c->local = local;
    pthread_mutex_lock(&eng.lock);
    eng.h264_clients++;
    eng.h264_key_req = 1;   /* this viewer needs an IDR to start on */
    c->gen = eng.kick_gen;
    c->last_seq = eng.h264_seq;     /* only frames from now on */
    now_ts(&eng.last_activity);
    pthread_mutex_unlock(&eng.lock);
    pthread_mutex_unlock(&eng.ctl);
    return c;
}

long cam_h264_next(cam_h264_client_t *c, const uint8_t **au,
                   uint64_t *pts90k, int *key)
{
    if (lid_ends_viewer(c->local))
        return -1;
    pthread_mutex_lock(&eng.lock);
    for (;;) {
        int timeouts = 0;
        while (eng.running && !h264_disabled && c->gen == eng.kick_gen &&
               eng.h264_seq <= c->last_seq) {
            struct timespec deadline;
            clock_gettime(CLOCK_MONOTONIC, &deadline);
            deadline.tv_sec += CLIENT_WAIT_S;
            if (pthread_cond_timedwait(&eng.h264_cv, &eng.lock, &deadline)
                == ETIMEDOUT && ++timeouts >= 2)
                break;
        }
        if (!eng.running || h264_disabled || c->gen != eng.kick_gen ||
            eng.h264_seq <= c->last_seq) {
            pthread_mutex_unlock(&eng.lock);
            return -1;
        }
        c->last_seq = eng.h264_seq;
        if (!c->started && !eng.h264_key)
            continue;       /* wait for this viewer's IDR */
        break;
    }
    c->started = 1;
    if (c->cap < eng.h264_len) {
        uint8_t *nb = realloc(c->buf, eng.h264_len);
        if (!nb) {
            pthread_mutex_unlock(&eng.lock);
            return -1;
        }
        c->buf = nb;
        c->cap = eng.h264_len;
    }
    memcpy(c->buf, eng.h264_au, eng.h264_len);
    long len = (long)eng.h264_len;
    *pts90k = eng.h264_pts;
    *key = eng.h264_key;
    now_ts(&eng.last_activity);
    pthread_mutex_unlock(&eng.lock);
    *au = c->buf;
    return len;
}

void cam_h264_client_close(cam_h264_client_t *c)
{
    if (!c)
        return;
    pthread_mutex_lock(&eng.lock);
    if (eng.h264_clients > 0)
        eng.h264_clients--;
    now_ts(&eng.last_activity);
    pthread_mutex_unlock(&eng.lock);
    free(c->buf);
    free(c);
}

size_t cam_h264_params(uint8_t *buf, size_t cap)
{
    size_t n = 0;
    pthread_mutex_lock(&h264_params_mx);
    if (h264_params)
        n = mp4mux_params_annexb(h264_params, buf, cap);
    pthread_mutex_unlock(&h264_params_mx);
    return n;
}

/* --------------------------------------------------------------- status */

void cam_get_status(struct cam_status *st)
{
    pthread_mutex_lock(&eng.lock);
    st->running = eng.running;
    st->cam = eng.home_cam;
    st->clients = eng.clients;
    st->seq = eng.seq;
    st->fps = eng.fps;
    st->fps_cap = eng.fps_cap;
    st->vpu = eng.vpu_active;
    st->gpu = eng.gpu_active;
    st->hw_skip = eng.hw_skip;
    st->cached = eng.cached_bufs;
    st->h264_up = eng.h264_up;
    st->h264_clients = eng.h264_clients;
    /* Geometry follows the sensor the served camera actually carries. */
    const struct sensor_profile *p = eng.seen[eng.home_cam];
    st->sensor = p ? p->model : "unknown";
    st->snap_w = p ? p->w : 0;
    st->snap_h = p ? p->h : 0;
    st->stream_w = p ? p->w / 2 : 0;
    st->stream_h = p ? p->h / 2 : 0;
    st->lid_stopped = eng.lid_stopped;
    st->frames = eng.frames;
    st->corrupt = eng.corrupt;
    st->recoveries = eng.recoveries;
    st->withheld = eng.withheld;
    st->skipped = eng.skipped;
    int live = eng.running && (eng.clients + eng.h264_clients) > 0;
    st->latency_ms = live ? eng.t_latency : 0;
    st->convert_ms = live ? eng.t_convert : 0;
    st->copy_ms = live ? eng.t_copy : 0;
    st->encode_ms = live ? eng.t_encode : 0;
    pthread_mutex_unlock(&eng.lock);
    /* Read outside the lock: it opens a device, and nothing else here
     * depends on it being sampled at the same instant as the counters. */
    st->lid_closed = machine_lid_closed();
}
