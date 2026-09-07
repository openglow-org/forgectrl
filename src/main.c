/*
 * main.c - forgectrl: ForgeFIRM system control daemon
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The machine-services daemon of ForgeFIRM: the HTTP service on ports 80 and 443
 * that supervises the controller and brokers the pulse device, runs the
 * cooling engine, the cameras, telemetry, settings, diagnostics, logging,
 * the web control panel, and the A/B update system. The contract, every
 * route included, is the forgectrl page of the documentation site
 * (https://docs.forgefirm.org/technical/forgefirm/forgectrl/); the panel
 * sources are in src/ui/.
 *
 * Invocation: forgectrl [--render-syslog]. --render-syslog writes the
 * rsyslog rules and log directories from the settings and exits; the
 * boot sequence runs it before rsyslog starts (see logs.c).
 *
 * The two cameras share the hardware mux; the newest request wins it. A
 * STREAM request for the other camera preempts the current stream
 * clients (their streams end cleanly - viewers freeze on the last frame)
 * and switches. A SNAPSHOT of the other camera does not switch: the
 * engine borrows the mux for one frame and the stream freezes briefly.
 * Environment: FORGECTRL_PORT (80), FORGECTRL_TLS_PORT (443), FORGECTRL_STREAM_Q (75),
 * FORGECTRL_LAMP (132), FORGECTRL_STREAM_FPS (0 = sensor max; 1 or more
 * is also realized as CSI hardware frame skip), FORGECTRL_H264_KBPS
 * (1500), FORGECTRL_H264_GOP (30), and the fallback switches
 * FORGECTRL_NO_VPU, FORGECTRL_NO_H264, FORGECTRL_NO_GPU,
 * FORGECTRL_NO_HW_SKIP, FORGECTRL_NO_CACHED_BUFS, FORGECTRL_NO_NEON.
 *
 * ulfius runs libmicrohttpd in thread-per-connection mode, so each stream
 * callback may block waiting for the next frame.
 */
#define _GNU_SOURCE
#include "advisories.h"
#include "auth.h"
#include "button.h"
#include "cam.h"
#include "camkey.h"
#include "commission.h"
#include "mp4mux.h"
#include "cool.h"
#include "curverec.h"
#include "diag.h"
#include "fflog.h"
#include "gates.h"
#include "hooks.h"
#include "led.h"
#include "logs.h"
#include "paths.h"
#include "session.h"
#include "settings.h"
#include "sheetid.h"
#include "status.h"
#include "super.h"
#include "tls.h"
#include "ui.h"
#include "update.h"
#include "users.h"
#include "wiz.h"
#include "wizdark.h"

#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <microhttpd.h>
#include <ulfius.h>
#include <unistd.h>
#include <zlib.h>

#define DEFAULT_PORT 80             /* HTTP: the read-only routes, this host, the redirect */
#define DEFAULT_TLS_PORT 443        /* HTTPS: the login, the panel, every state change */
#define BOUNDARY     "forgectrl-frame"
#define SNAP_Q_DEF   75

static volatile sig_atomic_t quit = 0;
static unsigned tls_port = DEFAULT_TLS_PORT;
static int cb_http_redirect(const struct _u_request *req, struct _u_response *res,
                            void *user_data);

static void on_signal(int sig)
{
    (void)sig;
    quit = 1;
}

/* ------------------------------------------------------------- helpers */

static cam_id_t parse_cam(const struct _u_request *req, int *ok)
{
    const char *v = u_map_get(req->map_url, "cam");
    *ok = 1;
    if (!v || !strcmp(v, "lid"))
        return CAM_LID;
    if (!strcmp(v, "head"))
        return CAM_HEAD;
    *ok = 0;
    return CAM_LID;
}

static int reply_error(struct _u_response *res, unsigned status,
                       const char *msg)
{
    ulfius_set_string_body_response(res, status, msg);
    ulfius_add_header_to_response(res, "Content-Type", "text/plain");
    return U_CALLBACK_CONTINUE;
}

/* ------------------------------------------------------------ streaming */

struct stream_ctx {
    cam_client_t *cl;
    uint8_t      *chunk;    /* current multipart chunk being drained */
    size_t        chunk_cap;
    size_t        chunk_len;
    size_t        off;
};

static ssize_t stream_cb(void *cls, uint64_t pos, char *buf, size_t max)
{
    (void)pos;
    struct stream_ctx *sc = cls;

    if (sc->off >= sc->chunk_len) {
        const uint8_t *jpg;
        long len = cam_client_next(sc->cl, &jpg);
        if (len < 0)
            return U_STREAM_END;

        char head[128];
        int headlen = snprintf(head, sizeof(head),
                               "--" BOUNDARY "\r\n"
                               "Content-Type: image/jpeg\r\n"
                               "Content-Length: %ld\r\n\r\n", len);
        size_t need = (size_t)headlen + (size_t)len + 2;
        if (sc->chunk_cap < need) {
            uint8_t *nb = realloc(sc->chunk, need);
            if (!nb)
                return U_STREAM_ERROR;
            sc->chunk = nb;
            sc->chunk_cap = need;
        }
        memcpy(sc->chunk, head, (size_t)headlen);
        memcpy(sc->chunk + headlen, jpg, (size_t)len);
        memcpy(sc->chunk + headlen + len, "\r\n", 2);
        sc->chunk_len = need;
        sc->off = 0;
    }

    size_t n = sc->chunk_len - sc->off;
    if (n > max)
        n = max;
    memcpy(buf, sc->chunk + sc->off, n);
    sc->off += n;
    return (ssize_t)n;
}

static void stream_free_cb(void *cls)
{
    struct stream_ctx *sc = cls;
    cam_client_close(sc->cl);
    free(sc->chunk);
    free(sc);
}

/* ------------------------------------------------- H.264 (fMP4) stream */

static unsigned cam_err_status(const char *err);

struct h264_ctx {
    cam_h264_client_t *cl;
    mp4mux_t          *mux;
    uint32_t           frag_seq;
    uint64_t           pts_base;    /* first frame's clock: fragments are
                                     * zero-based so any player starts at
                                     * the top of its timeline */
    uint64_t           prev_pts;
    uint8_t           *chunk;       /* current fMP4 piece being drained */
    size_t             chunk_len;
    size_t             off;
    uint8_t           *pending;     /* init segment queued before frame 1 */
    size_t             pending_len;
};

/* Pull one access unit and wrap it as a moof+mdat chunk. */
static int h264_next_chunk(struct h264_ctx *hc)
{
    const uint8_t *au;
    uint64_t pts;
    int key;
    long len = cam_h264_next(hc->cl, &au, &pts, &key);
    if (len < 0)
        return -1;
    uint32_t dur = 90000 / 15;
    if (hc->frag_seq > 0 && pts > hc->prev_pts &&
        pts - hc->prev_pts < 90000)
        dur = (uint32_t)(pts - hc->prev_pts);
    hc->prev_pts = pts;
    free(hc->chunk);
    hc->chunk = mp4mux_fragment(hc->mux, ++hc->frag_seq,
                                pts - hc->pts_base, dur, key,
                                au, (size_t)len, &hc->chunk_len);
    hc->off = 0;
    return hc->chunk ? 0 : -1;
}

static ssize_t h264_cb(void *cls, uint64_t pos, char *buf, size_t max)
{
    (void)pos;
    struct h264_ctx *hc = cls;

    if (hc->off >= hc->chunk_len) {
        if (hc->pending) {
            free(hc->chunk);
            hc->chunk = hc->pending;
            hc->chunk_len = hc->pending_len;
            hc->pending = NULL;
            hc->off = 0;
        } else if (h264_next_chunk(hc)) {
            return U_STREAM_END;
        }
    }
    size_t n = hc->chunk_len - hc->off;
    if (n > max)
        n = max;
    memcpy(buf, hc->chunk + hc->off, n);
    hc->off += n;
    return (ssize_t)n;
}

static void h264_free_cb(void *cls)
{
    struct h264_ctx *hc = cls;
    cam_h264_client_close(hc->cl);
    mp4mux_free(hc->mux);
    free(hc->chunk);
    free(hc->pending);
    free(hc);
}

static int do_h264(cam_id_t cam, struct _u_response *res)
{
    char err[256];
    cam_h264_client_t *cl = cam_h264_client_open(cam, err, sizeof(err));
    if (!cl)
        return reply_error(res, cam_err_status(err), err);

    struct h264_ctx *hc = calloc(1, sizeof(*hc));
    if (!hc) {
        cam_h264_client_close(cl);
        return reply_error(res, 500, "out of memory");
    }
    hc->cl = cl;

    /* Block for the first access unit here, so the codec string (from
     * the SPS) can travel in a response header and the init segment
     * precedes frame one. A machine whose H.264 encoder is missing or
     * refused answers 503 instead of an empty stream. */
    const uint8_t *au;
    uint64_t pts;
    int key;
    long len = cam_h264_next(cl, &au, &pts, &key);
    uint8_t params[512];
    size_t plen = len < 0 ? 0 : cam_h264_params(params, sizeof(params));
    if (len < 0 || plen == 0) {
        h264_free_cb(hc);
        return reply_error(res, 503, "H.264 stream unavailable "
                           "(the MJPEG stream still works)");
    }
    struct cam_status st;
    cam_get_status(&st);
    hc->mux = mp4mux_new(st.stream_w, st.stream_h);
    if (!hc->mux || !mp4mux_feed_params(hc->mux, params, plen)) {
        h264_free_cb(hc);
        return reply_error(res, 500, "H.264 parameter sets unusable");
    }
    size_t init_len = 0, frag_len = 0;
    uint8_t *init = mp4mux_init_segment(hc->mux, &init_len);
    hc->pts_base = pts;
    hc->prev_pts = pts;
    uint8_t *frag = mp4mux_fragment(hc->mux, ++hc->frag_seq, 0,
                                    90000 / 15, key, au, (size_t)len,
                                    &frag_len);
    if (!init || !frag) {
        free(init);
        free(frag);
        h264_free_cb(hc);
        return reply_error(res, 500, "out of memory");
    }
    hc->chunk = init;
    hc->chunk_len = init_len;
    hc->off = 0;
    hc->pending = frag;
    hc->pending_len = frag_len;

    ulfius_add_header_to_response(res, "Content-Type", "video/mp4");
    ulfius_add_header_to_response(res, "X-H264-Codec",
                                  mp4mux_codec(hc->mux));
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    ulfius_set_stream_response(res, 200, h264_cb, h264_free_cb,
                               U_STREAM_SIZE_UNKNOWN, 64 * 1024, hc);
    return U_CALLBACK_CONTINUE;
}

/* Camera failures that reflect machine state rather than a fault answer
 * 409 (the request conflicts with how the machine is right now, and the
 * fix is to change that): the mux is held by another viewer, or the lid
 * is open and the privacy gate refuses to capture. Everything else is a
 * 503 - the camera could not be brought up. */
static unsigned cam_err_status(const char *err)
{
    return (strstr(err, "busy") || !strcmp(err, CAM_ERR_LID)) ? 409 : 503;
}

static int do_stream(cam_id_t cam, struct _u_response *res)
{
    char err[256];
    cam_client_t *cl = cam_client_open(cam, err, sizeof(err));
    if (!cl)
        return reply_error(res, cam_err_status(err), err);

    struct stream_ctx *sc = calloc(1, sizeof(*sc));
    if (!sc) {
        cam_client_close(cl);
        return reply_error(res, 500, "out of memory");
    }
    sc->cl = cl;

    ulfius_add_header_to_response(res, "Content-Type",
        "multipart/x-mixed-replace; boundary=" BOUNDARY);
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    ulfius_set_stream_response(res, 200, stream_cb, stream_free_cb,
                               U_STREAM_SIZE_UNKNOWN, 64 * 1024, sc);
    return U_CALLBACK_CONTINUE;
}

static int do_snapshot(cam_id_t cam, int full, int quality, int lamp,
                       struct _u_response *res)
{
    uint8_t *jpg = NULL;
    size_t len = 0;
    char err[256];
    if (cam_snapshot(cam, full, quality, lamp, &jpg, &len, err, sizeof(err)))
        return reply_error(res, cam_err_status(err), err);
    ulfius_set_binary_body_response(res, 200, (const char *)jpg, len);
    ulfius_add_header_to_response(res, "Content-Type", "image/jpeg");
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    free(jpg);
    return U_CALLBACK_CONTINUE;
}

/* ------------------------------------------------------------ callbacks */

static int cb_stream(const struct _u_request *req, struct _u_response *res,
                     void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    int ok;
    cam_id_t cam = parse_cam(req, &ok);
    if (!ok)
        return reply_error(res, 400, "cam must be 'lid' or 'head'");
    return do_stream(cam, res);
}

static int cb_h264(const struct _u_request *req, struct _u_response *res,
                   void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    int ok;
    cam_id_t cam = parse_cam(req, &ok);
    if (!ok)
        return reply_error(res, 400, "cam must be 'lid' or 'head'");
    return do_h264(cam, res);
}

static int cb_snapshot(const struct _u_request *req, struct _u_response *res,
                       void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    int ok;
    cam_id_t cam = parse_cam(req, &ok);
    if (!ok)
        return reply_error(res, 400, "cam must be 'lid' or 'head'");

    int full = 1;
    const char *v = u_map_get(req->map_url, "res");
    if (v) {
        if (!strcmp(v, "half"))
            full = 0;
        else if (strcmp(v, "full"))
            return reply_error(res, 400, "res must be 'full' or 'half'");
    }
    int quality = SNAP_Q_DEF;
    if ((v = u_map_get(req->map_url, "q")) != NULL) {
        quality = atoi(v);
        if (quality < 1 || quality > 100)
            return reply_error(res, 400, "q must be 1..100");
    }
    int lamp = -1;
    if ((v = u_map_get(req->map_url, "lamp")) != NULL) {
        lamp = atoi(v);
        if (lamp < 0 || lamp > 1023)
            return reply_error(res, 400, "lamp must be 0..1023");
    }
    return do_snapshot(cam, full, quality, lamp, res);
}

static int cb_status(const struct _u_request *req, struct _u_response *res,
                     void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    struct cam_status st;
    cam_get_status(&st);
    char body[768];
    snprintf(body, sizeof(body),
             "{\"running\":%s,\"cam\":\"%s\",\"clients\":%d,"
             "\"frames\":%llu,\"fps\":%.1f,\"fps_cap\":%.1f,"
             "\"hw_fps_skip\":%s,"
             "\"encoder\":\"%s\",\"convert\":\"%s\",\"buffers\":\"%s\","
             "\"sensor\":\"%s\","
             "\"stream\":{\"width\":%d,\"height\":%d},"
             "\"snapshot\":{\"width\":%d,\"height\":%d},"
             "\"h264\":{\"active\":%s,\"clients\":%d},"
             "\"health\":{\"captured\":%llu,\"corrupt\":%llu,"
             "\"restarts\":%u},"
             "\"capture_allowed\":%s,\"stopped_by_lid\":%s}",
             st.running ? "true" : "false", cam_name(st.cam), st.clients,
             (unsigned long long)st.seq, st.fps, st.fps_cap,
             st.hw_skip ? "true" : "false",
             st.vpu ? "vpu" : "software",
             st.gpu ? "gpu" : "cpu",
             st.cached ? "cached" : "uncached", st.sensor,
             st.stream_w, st.stream_h, st.snap_w, st.snap_h,
             st.h264_up ? "true" : "false", st.h264_clients,
             (unsigned long long)st.frames, (unsigned long long)st.corrupt,
             st.recoveries,
             st.lid_closed ? "true" : "false",
             st.lid_stopped ? "true" : "false");
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

/* ------------------------------------------------------------- settings */

/* Machine settings shared with the grblHAL-glowforge controller and the
 * gfhome homing runner through /data/forgefirm.conf. The controller
 * re-reads the file on every $H and the runner at every session start,
 * so changes apply without restarts. Every key is validated here; an
 * empty value removes the key (back to the built-in default) and must
 * arrive as a query parameter - zero-length form-body values never
 * reach the body map. Secret keys are write-only: GET reports
 * "<key>_set" instead of the value. */

static int valid_homing_mode(const char *v)
{
    return !strcmp(v, "none") || !strcmp(v, "gfcloud") ||
           !strcmp(v, "switches");
}

static int valid_controller_mode(const char *v)
{
    /* grbl = grblHAL over TCP:23; cloud = the Glowforge web-service stack
     * (gfcloud daemon). The two are mutually exclusive; the boot-time init
     * scripts dispatch on this key, so a change applies on the next
     * controller restart. */
    return !strcmp(v, "grbl") || !strcmp(v, "cloud");
}

/* Numeric values are short by construction; a long-but-valid string
 * (e.g. "000...0033.0") is rejected here so the settings-report buffer
 * math can never be driven to overflow by an accepted value. */
#define VALUE_MAX_LEN 16

static int valid_mm(const char *v)
{
    char *end;
    if (strlen(v) > VALUE_MAX_LEN)
        return 0;
    double f = strtod(v, &end);
    return end != v && *end == '\0' && f >= -1000.0 && f <= 1000.0;
}

static int valid_timeout(const char *v)
{
    char *end;
    if (strlen(v) > VALUE_MAX_LEN)
        return 0;
    long t = strtol(v, &end, 10);
    return end != v && *end == '\0' && t >= 30 && t <= 3600;
}

static int valid_serial(const char *v)
{
    size_t n = strlen(v);
    if (n < 1 || n > 12)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (v[i] < '0' || v[i] > '9')
            return 0;
    return 1;
}

static int valid_password(const char *v)
{
    if (strlen(v) != 64)
        return 0;
    for (int i = 0; i < 64; i++)
        if (!isxdigit((unsigned char)v[i]))
            return 0;
    return 1;
}

static int valid_units(const char *v)
{
    return !strcmp(v, "metric") || !strcmp(v, "imperial");
}

/* WiFi regulatory region: ISO 3166-1 alpha-2, or "00" for the world
 * domain */
static int valid_country(const char *v)
{
    if (!strcmp(v, "00"))
        return 1;
    return strlen(v) == 2 &&
           v[0] >= 'A' && v[0] <= 'Z' && v[1] >= 'A' && v[1] <= 'Z';
}

/* Cooling tunables (consumed by the controller per flood start). The
 * ranges are wide on purpose - these exist for per-machine calibration
 * (pump wear, replacement coolant) - but still bounded to values the
 * hardware can mean something by. */
static int valid_range(const char *v, double lo, double hi)
{
    char *end;
    if (strlen(v) > VALUE_MAX_LEN)
        return 0;
    double f = strtod(v, &end);
    return end != v && *end == '\0' && f >= lo && f <= hi;
}

/* The gate settings (the coolant ceiling and its resume gate, the flow
 * window and its fault rise) validate against the one table in gates.c:
 * a wide legal range whose far end turns the gate off by value, and a
 * recommended band the panel warns outside of. The rest of the cooling
 * keys are bounded to what the engine can mean by them. */
static int valid_gate(const char *key, const char *v)
{
    return gate_parse(gate_setting_find(key), v, NULL);
}
static int valid_rise_c(const char *v)     { return valid_gate("cool_flow_rise", v); }
static int valid_check_s(const char *v)    { return valid_gate("cool_flow_check_s", v); }
static int valid_temp_max(const char *v)   { return valid_gate("cool_temp_max", v); }
static int valid_temp_resume(const char *v){ return valid_gate("cool_temp_resume", v); }
static int valid_temp_critical(const char *v){ return valid_gate("cool_temp_critical_c", v); }
static int valid_temp_min(const char *v)   { return valid_gate("cool_temp_min", v); }
static int valid_temp_start(const char *v) { return valid_gate("cool_temp_start", v); }
static int valid_tec_on(const char *v)     { return valid_gate("cool_tec_on_c", v); }
static int valid_tec_off(const char *v)    { return valid_gate("cool_tec_off_c", v); }
static int valid_tec_present(const char *v){ return !strcmp(v, "0") || !strcmp(v, "1"); }
/* The coolant sensors' per-machine offset: a room thermometer's word,
 * within 5 C either way. */
static int valid_temp_offset(const char *v)
{
    char *end;
    double d = strtod(v, &end);
    return end != v && *end == '\0' && d >= -5.0 && d <= 5.0;
}
static int valid_fire_q1a(const char *v)   { return valid_gate("cool_fire_q1_alert", v); }
static int valid_fire_q1c(const char *v)   { return valid_gate("cool_fire_q1_critical", v); }
static int valid_fire_q2a(const char *v)   { return valid_gate("cool_fire_q2_alert", v); }
static int valid_fire_q2c(const char *v)   { return valid_gate("cool_fire_q2_critical", v); }
static int valid_accel_xa(const char *v)   { return valid_gate("cool_accel_x_alert", v); }
static int valid_accel_ya(const char *v)   { return valid_gate("cool_accel_y_alert", v); }
static int valid_accel_ab(const char *v)   { return valid_gate("cool_accel_abort", v); }
static int valid_exhaust_rpm(const char *v) { return valid_gate("cool_tach_exhaust_min_rpm", v); }
static int valid_intake_rpm(const char *v)  { return valid_gate("cool_tach_intake_min_rpm", v); }
static int valid_air_rpm(const char *v)     { return valid_gate("cool_tach_air_assist_min_rpm", v); }
static int valid_purge_cur(const char *v)   { return valid_gate("cool_purge_min_current", v); }
static int valid_grace_s(const char *v)     { return valid_gate("cool_fan_grace_s", v); }
static int valid_heater_pct(const char *v) { return valid_range(v, 0, 100); }
static int valid_recheck_s(const char *v)  { return valid_range(v, 0, 3600); }
static int valid_confirm_s(const char *v)  { return valid_range(v, 60, 3600); }
/* C per raw-second of pic/hv_current; the bench measured 3.06e-5. */
static int valid_laser_heat(const char *v) { return valid_range(v, 0, 2e-4); }
/* ADC counts at the air-assist run duty; the bench measured about 20. */
static int valid_aa_offset(const char *v)  { return valid_range(v, 0, 60); }
static int valid_cool_s(const char *v)     { return valid_range(v, 0, 1800); }

/* GRBL-mode tunables, read by the controller from the same file. The
 * button wait runs with the laser latch unlocked and the controller
 * refuses an unbounded value, so the range here matches its clamp; the
 * disarm grace and the rail settle are bounded to what the machine can
 * mean by them. */
static int valid_button_s(const char *v)   { return valid_range(v, 1, 3600); }
static int valid_disarm_s(const char *v)   { return valid_range(v, 1, 3600); }

/* The laser dose floor in percent of full (loaded into $35 at every
 * precompute: the lowest density that marks), and the density model's
 * base period and shortest pulse in machine ticks (the stream caps the
 * period at 1024). Density is the only dose model; the analog rendering
 * exists solely as the controller's host-test reference. */
static int valid_floor_pct(const char *v)  { return valid_range(v, 0, 100); }
/* The measured dose curve: "off" or density:light percent pairs
 * ("10:0.5,...,100:100"); the controller validates the shape and falls
 * back loudly, this only bounds the alphabet and the length. */
static int valid_corner_gamma(const char *v) { return valid_range(v, 0.25, 4); }
/* The focus model in the lens's half-steps from its bottom stop: the
 * step where the lens is 2 in from the bed (under it when negative)
 * and the half-steps per mm of material (the count over the carriage's
 * 12.32 mm of travel, about 2.8). */
static int valid_edge_z(const char *v)      { return valid_range(v, -20, 20); }
static int valid_park_z(const char *v)      { return valid_range(v, -5, 10); }
static int valid_stop_steps(const char *v)  { return valid_range(v, 1, 40); }
static int valid_dose_curve(const char *v)
{
    size_t n = strlen(v);
    if (n == 0 || n > 180)
        return 0;
    if (!strcmp(v, "off"))
        return 1;
    for (size_t i = 0; i < n; i++)
        if (!((v[i] >= '0' && v[i] <= '9') || v[i] == ':' || v[i] == ',' || v[i] == '.'))
            return 0;
    return 1;
}
static int valid_pulse_ticks(const char *v){ return valid_range(v, 1, 1024); }
static int valid_settle_s(const char *v)   { return valid_range(v, 0, 30); }

/* The lid lamp's idle level (PWM 0-255; unset = 236). Applied live. */
static int valid_lamp(const char *v)       { return valid_range(v, 0, 255); }

/* Cloud-mode print pause: pulse ticks retraced (laser off) on the button
 * press, and the laser-off lead the resume runs before re-enabling
 * (unset = the factory's 2000 / 1950). The kernel reserves 32 KiB of
 * ring for the backtrack; both stay well inside it. */
static int valid_ticks(const char *v)      { return valid_range(v, 0, 30000); }
/* How long a cloud print may be held on the cooling verdict before it is
 * canceled: the engine's fail tiers never offer a resume. */
static int valid_hold_s(const char *v)     { return valid_range(v, 60, 7200); }

/* Cloud-mode download guards: bytes of pulse body the client will hold in
 * memory (unset = 32 MiB warn, 128 MiB refuse; 0 lifts either). The body is
 * the job as the service compressed it, tens to one, so these bound this
 * machine's memory and not the length of a job: a job longer than the ring
 * is fed as it plays. A gigabyte is well past any real ceiling and is here
 * so a typo cannot ask for one. */
static int valid_pulse_bytes(const char *v) { return valid_range(v, 0, 1073741824); }

/* GRBL mode: what a lid or interlock open does to a running job -
 * "cancel" (the factory's abort + return to the job start; unset = this)
 * or "hold" (stock grblHAL door hold, cycle start resumes). */
static int valid_lid_policy(const char *v) { return !strcmp(v, "cancel") || !strcmp(v, "hold"); }

/* The XY microstep mode (8, 16 or 32; unset = 8). One number both
 * controllers read at their start: the GRBL controller derives
 * $100/$101, its machine tick and the kernel stop ramp from it. The
 * key is applied at the next controller start; a change from the panel
 * restarts an idle GRBL controller on the spot. Cloud mode runs at the
 * service's own x8 whatever the key says. */
static int valid_xy_microsteps(const char *v)
{
    return !strcmp(v, "8") || !strcmp(v, "16") || !strcmp(v, "32");
}

/* Two 0/1 switches. cloud_enabled: the owner's decision from the cloud
 * wizard; while it is 0 the cloud controller, the cloud homing method
 * and the cloud tab do not exist. panel_open_reads: the read-only
 * routes (status, the cameras) answer any LAN client (unset = 1, so
 * LightBurn reads the camera); 0 closes them to a login or this host. */
static int valid_bool(const char *v)       { return !strcmp(v, "0") || !strcmp(v, "1"); }

/* Logging: per-logger disk and remote levels (each off|error|warning|
 * notice|info|debug) and the remote syslog target. Read at boot by
 * `forgectrl --render-syslog` (rsyslog rules) and by each process for
 * its own emit level, so a change applies at the next reboot. */

static const struct {
    const char *key;
    int (*valid)(const char *);
    int secret;
} setting_defs[] = {
    { "controller_mode",        valid_controller_mode, 0 },
    { "homing_mode",            valid_homing_mode, 0 },
    { "gfcloud_home_x",         valid_mm,          0 },
    { "gfcloud_home_y",         valid_mm,          0 },
    { "gfcloud_home_timeout_s", valid_timeout,     0 },
    { "gf_serial",              valid_serial,      0 },
    { "gf_password",            valid_password,    1 },
    { "ui_units",               valid_units,       0 },
    { "wifi_country",           valid_country,     0 },
    { "cool_flow_rise",         valid_rise_c,      0 },
    { "cool_flow_heater_pct",   valid_heater_pct,  0 },
    { "cool_flow_check_s",      valid_check_s,     0 },
    { "cool_recheck_s",         valid_recheck_s,   0 },
    { "cool_confirm_max_s",     valid_confirm_s,   0 },
    { "cool_laser_heat_cw",     valid_laser_heat,  0 },
    { "cool_laser_heat_density", valid_laser_heat, 0 },
    { "cool_aa_offset_counts",  valid_aa_offset,   0 },
    { "cool_temp_max",          valid_temp_max,    0 },
    { "cool_temp_resume",       valid_temp_resume, 0 },
    { "cool_temp_critical_c",   valid_temp_critical, 0 },
    { "cool_temp_min",          valid_temp_min,    0 },
    { "cool_temp_start",        valid_temp_start,  0 },
    { "cool_tec_present",       valid_tec_present, 0 },
    { "cool_temp_offset_c",     valid_temp_offset, 0 },
    { "cool_tec_on_c",          valid_tec_on,      0 },
    { "cool_tec_off_c",         valid_tec_off,     0 },
    { "cool_fire_q1_alert",     valid_fire_q1a,    0 },
    { "cool_fire_q1_critical",  valid_fire_q1c,    0 },
    { "cool_fire_q2_alert",     valid_fire_q2a,    0 },
    { "cool_fire_q2_critical",  valid_fire_q2c,    0 },
    { "cool_accel_x_alert",     valid_accel_xa,    0 },
    { "cool_accel_y_alert",     valid_accel_ya,    0 },
    { "cool_accel_abort",       valid_accel_ab,    0 },
    { "cool_cooldown_s",        valid_cool_s,      0 },
    { "cool_cooldown_max_s",    valid_cool_s,      0 },
    { "cool_tach_exhaust_min_rpm",    valid_exhaust_rpm, 0 },
    { "cool_tach_intake_min_rpm",     valid_intake_rpm,  0 },
    { "cool_tach_air_assist_min_rpm", valid_air_rpm,     0 },
    { "cool_purge_min_current",       valid_purge_cur,   0 },
    { "cool_fan_grace_s",             valid_grace_s,     0 },
    { "laser_button_timeout_s", valid_button_s,    0 },
    { "laser_disarm_s",         valid_disarm_s,    0 },
    { "laser_floor_density",    valid_floor_pct,   0 },
    { "laser_dose_curve",       valid_dose_curve,  0 },
    { "laser_corner_gamma",     valid_corner_gamma, 0 },
    { "lens_hall_edge_z_mm",    valid_edge_z,      0 },
    { "lens_park_z_mm",         valid_park_z,      0 },
    { "lens_stop_below_steps",  valid_stop_steps,  0 },
    { "lens_stop_above_steps",  valid_stop_steps,  0 },
    { "laser_pulse_ticks",      valid_pulse_ticks, 0 },
    { "laser_pulse_min_ticks",  valid_pulse_ticks, 0 },
    { "rail_settle_s",          valid_settle_s,    0 },
    { "lid_lamp_idle",          valid_lamp,        0 },
    { "cloud_pause_backtrack_ticks", valid_ticks,  0 },
    { "cloud_resume_lead_ticks",     valid_ticks,  0 },
    { "cloud_hold_max_s",            valid_hold_s, 0 },
    { "pulse_warn_threshold_bytes",   valid_pulse_bytes, 0 },
    { "pulse_reject_threshold_bytes", valid_pulse_bytes, 0 },
    { "lid_policy",             valid_lid_policy,  0 },
    { "xy_microsteps",          valid_xy_microsteps, 0 },
    { "cloud_enabled",          valid_bool,        0 },
    { "panel_open_reads",       valid_bool,        0 },
    { "log_forgectrl_disk",     logs_valid_level,  0 },
    { "log_forgectrl_remote",   logs_valid_level,  0 },
    { "log_grblhal_disk",       logs_valid_level,  0 },
    { "log_grblhal_remote",     logs_valid_level,  0 },
    { "log_gfcloud_disk",       logs_valid_level,  0 },
    { "log_gfcloud_remote",     logs_valid_level,  0 },
    { "log_gfhome_disk",        logs_valid_level,  0 },
    { "log_gfhome_remote",      logs_valid_level,  0 },
    { "log_kernel_disk",        logs_valid_level,  0 },
    { "log_kernel_remote",      logs_valid_level,  0 },
    { "log_system_disk",        logs_valid_level,  0 },
    { "log_system_remote",      logs_valid_level,  0 },
    { "syslog_server",          logs_valid_server, 0 },
    { "syslog_port",            logs_valid_port,   0 },
    { "syslog_proto",           logs_valid_proto,  0 },
};
#define N_SETTINGS (sizeof(setting_defs) / sizeof(*setting_defs))

int setting_valid(const char *key, const char *val)
{
    if (!key || !val)
        return 0;
    for (size_t i = 0; i < N_SETTINGS; i++)
        if (!strcmp(setting_defs[i].key, key))
            return !val[0] || setting_defs[i].valid(val);
    return 0;
}

/* The settings as text for the log export: one "key = value" per line,
 * secrets shown only as set/unset. */
static void settings_snapshot(FILE *out)
{
    char val[192];            /* the longest legal value: a 180-character dose curve */
    for (size_t i = 0; i < N_SETTINGS; i++) {
        int have = settings_get(setting_defs[i].key, val, sizeof(val)) == 0 &&
                   setting_defs[i].valid(val);
        if (setting_defs[i].secret)
            fprintf(out, "%s = %s\n", setting_defs[i].key,
                    have ? "<set>" : "<unset>");
        else
            fprintf(out, "%s = %s\n", setting_defs[i].key, have ? val : "");
    }
}

/* The commissioning record for the log export, indented so the
 * sanitizer sees one value per line. */
static void record_snapshot(FILE *out)
{
    char *text = commission_record_dump(1);
    if (!text)
        return;
    fputs(text, out);
    fputc('\n', out);
    free(text);
}

/* WiFi radio policy, applied at startup and whenever the wifi_country
 * setting changes. The stored region (unset = "00", the world domain)
 * goes to cfg80211 as the user regulatory hint; power save is pinned
 * off - on a mains-powered machine it only adds latency. */
void apply_wifi(int set_region)
{
    char cc[8], cmd[48];
    if (settings_get("wifi_country", cc, sizeof(cc)) != 0 ||
        !valid_country(cc))
        snprintf(cc, sizeof(cc), "00");
    /* Reload the database first: when cfg80211 initialized before the
     * rootfs was mounted, its boot-time regulatory.db load failed and
     * stays failed until an explicit reload. */
    (void)!system("iw reg reload >/dev/null 2>&1");
    /* The kernel already defaults to the world domain; hinting 00 on
     * top of it only produces a cosmetic "country 98" intersection.
     * 00 is set only to revert from a previously applied region. */
    if (set_region || strcmp(cc, "00")) {
        snprintf(cmd, sizeof(cmd), "iw reg set %s >/dev/null 2>&1", cc);
        if (system(cmd) != 0)
            fflog(LOG_ERR, "iw reg set %s failed", cc);
    }
    if (system("iw dev wlan0 set power_save off >/dev/null 2>&1") != 0)
        fflog(LOG_ERR, "wifi power_save off failed");
}

/* Machine identity, derived from the i.MX6 OCOTP fuses exactly like
 * the factory firmware: the serial is fuse word HW_OCOTP_MAC0 (nvmem
 * word 34 on the mainline imx-ocotp driver; hex text on the legacy
 * fsl_otp path), and the factory hostname is that serial encoded
 * base-23 over the alphabet BCDFGHJKMQRTVWXY2346789, up to six
 * characters, split XXX-YYY. The GUI always identifies the machine by
 * this fuse identity - the gf_serial setting overrides only what is
 * sent to the Glowforge cloud. */
unsigned long fuse_serial(void)
{
    static unsigned long cached;
    static int tried;
    if (!tried) {
        tried = 1;
        FILE *f = fopen("/sys/bus/nvmem/devices/imx-ocotp0/nvmem", "rb");
        if (f) {
            unsigned char w[4];
            if (fseek(f, 136, SEEK_SET) == 0 && fread(w, 1, 4, f) == 4)
                cached = (unsigned long)w[0] | ((unsigned long)w[1] << 8) |
                         ((unsigned long)w[2] << 16) |
                         ((unsigned long)w[3] << 24);
            fclose(f);
        } else if ((f = fopen("/sys/fsl_otp/HW_OCOTP_MAC0", "r")) != NULL) {
            if (fscanf(f, "%lx", &cached) != 1)
                cached = 0;
            fclose(f);
        }
    }
    return cached;
}

/* Fuse password: the eight SRK words (nvmem bank 3 words 0-7, byte
 * offset 96) as 64 hex digits - what the machine signs in to the
 * Glowforge service with. Read on demand only (the fuse-identity
 * viewer), never included in routine polls. */
int fuse_password(char *buf, size_t len)
{
    buf[0] = '\0';
    if (len < 65)
        return -1;
    FILE *f = fopen("/sys/bus/nvmem/devices/imx-ocotp0/nvmem", "rb");
    if (f) {
        unsigned char w[32];
        int ok = fseek(f, 96, SEEK_SET) == 0 && fread(w, 1, 32, f) == 32;
        fclose(f);
        if (!ok)
            return -1;
        for (int i = 0; i < 8; i++)
            snprintf(buf + i * 8, 9, "%08lx",
                     (unsigned long)w[i * 4] |
                     ((unsigned long)w[i * 4 + 1] << 8) |
                     ((unsigned long)w[i * 4 + 2] << 16) |
                     ((unsigned long)w[i * 4 + 3] << 24));
        return 0;
    }
    for (int i = 0; i < 8; i++) {
        char path[48];
        unsigned long v;
        snprintf(path, sizeof(path), "/sys/fsl_otp/HW_OCOTP_SRK%d", i);
        if ((f = fopen(path, "r")) == NULL)
            return -1;
        int ok = fscanf(f, "%lx", &v) == 1;
        fclose(f);
        if (!ok)
            return -1;
        snprintf(buf + i * 8, 9, "%08lx", v);
    }
    return 0;
}

void machine_id(char *buf, size_t len)
{
    static char cached[16];
    if (!cached[0]) {
        unsigned long serial = fuse_serial();
        if (serial) {
            static const char alpha[] = "BCDFGHJKMQRTVWXY2346789";
            char enc[8];
            int n = 0;
            while (serial > 0 && n < 6) {
                enc[n++] = alpha[serial % 23];
                serial /= 23;
            }
            int o = 0;
            for (int i = n - 1; i >= 0; i--) {
                cached[o++] = enc[i];
                if (i == n - 3 && i > 0)
                    cached[o++] = '-';
            }
            cached[o] = '\0';
        }
    }
    snprintf(buf, len, "%s", cached);
}

/* /etc/forgefirm-version: written by the image build (release version,
 * or build timestamp + dev tag). Sanitized for direct JSON embedding. */
void read_fw_version(char *buf, size_t len)
{
    buf[0] = '\0';
    FILE *f = fopen("/etc/forgefirm-version", "r");
    if (!f)
        return;
    if (!fgets(buf, (int)len, f))
        buf[0] = '\0';
    fclose(f);
    size_t o = 0;
    for (size_t i = 0; buf[i]; i++)
        if (buf[i] >= ' ' && buf[i] != '"' && buf[i] != '\\')
            buf[o++] = buf[i];
    while (o > 0 && buf[o - 1] == ' ')
        o--;
    buf[o] = '\0';
}

/* Append into a fixed buffer, keeping the running offset within bounds:
 * snprintf returns the would-have-written length, so an unclamped
 * accumulator can run past the buffer and underflow the next
 * `size - off`. Clamped here so every subsequent append is a safe no-op
 * once the buffer is full. */
static void append(char *buf, size_t size, size_t *off, const char *fmt, ...)
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
        *off = size - 1;                /* truncated; keep off in range */
}

/* A gate setting's value as the file holds it, or its default. */
static double setting_gate_value(const gate_setting_t *g, void *ctx)
{
    (void)ctx;
    char v[128];
    double out;
    if (settings_get(g->key, v, sizeof(v)) == 0 && gate_parse(g, v, &out))
        return out;
    return g->def;
}

static int reply_settings(struct _u_response *res)
{
    char body[8192], val[192], mid[16], fwver[48];
    size_t off = 0;

    read_fw_version(fwver, sizeof(fwver));
    machine_id(mid, sizeof(mid));

    append(body, sizeof(body), &off, "{");
    for (size_t i = 0; i < N_SETTINGS; i++) {
        /* A value that fails its own validator (hand-edited file) is
         * reported as unset rather than leaking arbitrary bytes into
         * the JSON. */
        int have = settings_get(setting_defs[i].key, val, sizeof(val)) == 0 &&
                   setting_defs[i].valid(val);
        if (setting_defs[i].secret)
            append(body, sizeof(body), &off, "\"%s_set\":%s,",
                   setting_defs[i].key, have ? "true" : "false");
        else
            append(body, sizeof(body), &off, "\"%s\":\"%s\",",
                   setting_defs[i].key, have ? val : "");
    }
    /* The gate settings with their ranges, bands and states, from the
     * file's values (the default where unset): what the panel renders
     * its warnings from, and the record of any gate that is off by
     * value. The engine reports the same from its resolved tunables in
     * /cool/status and /status. */
    char gates[3072];
    if (gates_json(gates, sizeof(gates), setting_gate_value, NULL) > 0)
        append(body, sizeof(body), &off, "\"gates\":%s,", gates);
    append(body, sizeof(body), &off,
           "\"version\":\"%s\",\"machine_id\":\"%s\","
           "\"tls_fingerprint\":\"%s\"}", fwver, mid, tls_fingerprint());
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

static int cb_settings_get(const struct _u_request *req,
                           struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    return reply_settings(res);
}

static int cb_machine_status(const struct _u_request *req,
                             struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char body[3072], gates[128];
    cool_gates_off_json(gates, sizeof(gates));
    char extra[160];
    snprintf(extra, sizeof(extra), "\"gates_off\":%s", gates);
    machine_status_json(body, sizeof(body), extra);
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

static int cb_curve_record(const struct _u_request *req,
                           struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char err[96] = "";
    if (curverec_start(err, sizeof(err)) != 0)
        return reply_error(res, 409, err);
    char body[1024];
    curverec_status_json(body, sizeof(body));
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

static int cb_curve_stop(const struct _u_request *req,
                         struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    curverec_stop();
    char body[1024];
    curverec_status_json(body, sizeof(body));
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

static int cb_curve_status(const struct _u_request *req,
                           struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char body[1024];
    curverec_status_json(body, sizeof(body));
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

static int cb_curve_ladder(const struct _u_request *req,
                           struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char body[2048];
    if (curverec_ladder_gcode(body, sizeof(body)) != 0)
        return reply_error(res, 500, "ladder does not fit");
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "text/plain");
    ulfius_add_header_to_response(res, "Content-Disposition",
                                  "attachment; filename=\"dose-ladder.gcode\"");
    return U_CALLBACK_CONTINUE;
}

static int cb_grbl_settings(const struct _u_request *req,
                            struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char body[4096];
    if (grbl_settings_text(body, sizeof(body)) != 0)
        return reply_error(res, 404, "no grbl controller");
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "text/plain");
    return U_CALLBACK_CONTINUE;
}

static const char *setting_param(const struct _u_request *req,
                                 const char *key)
{
    const char *v = u_map_get(req->map_post_body, key);
    return v ? v : u_map_get(req->map_url, key);
}

/* Effective value of a cooling gate key for cross-field checks: the
 * request value if this POST sets it, else the persisted value, else the
 * gate table's compiled default. Always sets *out. */
static void effective_temp(const struct _u_request *req, const char *key,
                           double *out)
{
    const gate_setting_t *g = gate_setting_find(key);
    double dflt = g ? g->def : 0.0;
    const char *v = setting_param(req, key);
    char stored[128];
    if (v && v[0])
        *out = strtod(v, NULL);
    else if (v && !v[0])
        *out = dflt;                    /* this POST clears it */
    else if (settings_get(key, stored, sizeof(stored)) == 0 && stored[0])
        *out = strtod(stored, NULL);
    else
        *out = dflt;
}

static int cb_settings_post(const struct _u_request *req,
                            struct _u_response *res, void *user_data)
{
    (void)user_data;
    int present = 0;

    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;

    /* Settings are locked whenever the machine is not idle: the
     * controller and the homing runner both read this file mid-run.
     * A running diagnostic owns the hardware and locks them too. */
    if (diag_running())
        return reply_error(res, 409,
            "a diagnostic is running - settings are locked");
    if (!machine_is_idle())
        return reply_error(res, 409,
            "machine is not idle - settings are locked");

    /* Validate the whole request before writing anything. */
    for (size_t i = 0; i < N_SETTINGS; i++) {
        const char *v = setting_param(req, setting_defs[i].key);
        if (!v)
            continue;
        present++;
        if (v[0] && !setting_defs[i].valid(v)) {
            char err[96];
            snprintf(err, sizeof(err), "invalid value for %s",
                     setting_defs[i].key);
            return reply_error(res, 400, err);
        }
    }
    if (!present)
        return reply_error(res, 400, "no known setting in request");

    /* Cross-field cooling safety: the resume ceiling must not sit at or
     * above the over-temp ceiling, or the hysteresis inverts and the
     * machine can resume into an over-temperature it never leaves. The
     * per-key validators already cap each to a bounded range; this pins
     * their relationship across a multi-key POST. */
    /* Nothing may point at the cloud while cloud mode is off: the
     * effective cloud_enabled (this request's value, else the stored
     * one) must be 1 for controller_mode=cloud or homing_mode=gfcloud. */
    const char *ce = setting_param(req, "cloud_enabled");
    {
        /* The key is the owner's decision from the cloud step, never a
         * plain switch: turning it on here takes the step's typed
         * acknowledgment too. Re-sending 1 while it stands changes
         * nothing and asks nothing. */
        if (ce && !strcmp(ce, "1") && !settings_get_bool("cloud_enabled", 0)) {
            const char *phrase = setting_param(req, "phrase");
            if (!phrase || strcmp(phrase, WIZ_CLOUD_PHRASE))
                return reply_error(res, 400,
                    "type I UNDERSTAND to turn cloud mode on");
        }
        int enabled = ce && ce[0] ? !strcmp(ce, "1")
                                  : settings_get_bool("cloud_enabled", 0);
        const char *cm = setting_param(req, "controller_mode");
        const char *hm = setting_param(req, "homing_mode");
        if (!enabled && cm && !strcmp(cm, "cloud"))
            return reply_error(res, 409,
                "cloud mode is not enabled on this machine");
        if (!enabled && hm && !strcmp(hm, "gfcloud"))
            return reply_error(res, 409,
                "cloud homing needs cloud mode enabled");
    }

    double tmax, tresume, tcrit;
    effective_temp(req, "cool_temp_max", &tmax);
    effective_temp(req, "cool_temp_resume", &tresume);
    effective_temp(req, "cool_temp_critical_c", &tcrit);
    if (tresume >= tmax)
        return reply_error(res, 400,
            "cool_temp_resume must be below cool_temp_max");
    /* The critical line is the fail tier above the pause tier; at or
     * below the ceiling it would fail a job the ceiling meant to pause.
     * A ceiling at its off end is no ceiling (every gate is off by value
     * on its own), so the line is only held above a ceiling that gates. */
    const gate_setting_t *ceil = gate_setting_find("cool_temp_max");
    int ceiling_off = ceil && ceil->off_end > 0 && tmax >= ceil->hi;
    if (!ceiling_off && tcrit <= tmax)
        return reply_error(res, 400,
            "cool_temp_critical_c must be above cool_temp_max");
    /* The low side: floor under start under ceiling, each relation held
     * only between gates that are on (zero is a floor or a start gate
     * off, its own off end). */
    double tmin, tstart;
    effective_temp(req, "cool_temp_min", &tmin);
    effective_temp(req, "cool_temp_start", &tstart);
    const gate_setting_t *flo = gate_setting_find("cool_temp_min");
    const gate_setting_t *sta = gate_setting_find("cool_temp_start");
    int floor_off = flo && tmin <= flo->lo;
    int start_off = sta && tstart <= sta->lo;
    if (!floor_off && !start_off && tmin >= tstart)
        return reply_error(res, 400,
            "cool_temp_min must be below cool_temp_start");
    if (!floor_off && !ceiling_off && tmin >= tmax)
        return reply_error(res, 400,
            "cool_temp_min must be below cool_temp_max");
    /* The start gate is deliberately not pinned under the ceiling: a
     * bench drill drops the ceiling to the loop's own reading (the
     * gate-off trip leg, the critical-tier drill), and a start gate
     * above such a ceiling holds twice over - odd, and safe. */
    /* The TEC's hysteresis pair: off under on. The pair is not pinned
     * above the floor here - a bench drill raises the floor over the
     * live reading - because the engine's runtime clamp already forces
     * the TEC off within a degree of the floor. */
    double teon, teoff;
    effective_temp(req, "cool_tec_on_c", &teon);
    effective_temp(req, "cool_tec_off_c", &teoff);
    if (teoff >= teon)
        return reply_error(res, 400,
            "cool_tec_off_c must be below cool_tec_on_c");
    /* The fire watch: within each quartile the alert must sit under the
     * critical while both tiers are on (zero is a tier off). */
    double fa, fc;
    effective_temp(req, "cool_fire_q1_alert", &fa);
    effective_temp(req, "cool_fire_q1_critical", &fc);
    if (fa > 0 && fc > 0 && fa >= fc)
        return reply_error(res, 400,
            "cool_fire_q1_alert must be below cool_fire_q1_critical");
    effective_temp(req, "cool_fire_q2_alert", &fa);
    effective_temp(req, "cool_fire_q2_critical", &fc);
    if (fa > 0 && fc > 0 && fa >= fc)
        return reply_error(res, 400,
            "cool_fire_q2_alert must be below cool_fire_q2_critical");

    /* The XY microstep mode is read by the GRBL controller at its start
     * only: a change of the stored value restarts an idle GRBL
     * controller after the write, so the panel's save is the whole
     * change. Any other controller picks it up at its next start. */
    int restart_grbl = 0;
    {
        const char *xm = setting_param(req, "xy_microsteps");
        if (xm) {
            char cur[16];
            const char *now = settings_get("xy_microsteps", cur, sizeof(cur)) == 0
                              ? cur : "8";
            const char *want = xm[0] ? xm : "8";
            restart_grbl = strcmp(now, want) != 0 && super_grbl_running();
        }
    }

    /* One atomic write for the whole request: no reader (grblHAL at $H,
     * gfhome at session start) can observe it half-applied, and a
     * concurrent writer cannot interleave between the keys. */
    const char *keys[N_SETTINGS], *vals[N_SETTINGS];
    size_t nset = 0;
    for (size_t i = 0; i < N_SETTINGS; i++) {
        const char *v = setting_param(req, setting_defs[i].key);
        if (!v)
            continue;
        keys[nset] = setting_defs[i].key;
        vals[nset] = v;
        nset++;
    }
    /* Off takes the cloud choices down with it, as the cloud step does,
     * so nothing points at a cloud that is off: the driver's $H reads
     * homing_mode alone. A choice the request sets itself was checked
     * above and stands as sent. */
    const char *swept[2] = { NULL, NULL };
    if (ce && !strcmp(ce, "0")) {
        char cur[16];
        if (!setting_param(req, "homing_mode") &&
            settings_get("homing_mode", cur, sizeof(cur)) == 0 &&
            !strcmp(cur, "gfcloud")) {
            keys[nset] = "homing_mode";
            vals[nset] = "none";
            nset++;
            swept[0] = "homing_mode none";
        }
        if (!setting_param(req, "controller_mode") &&
            settings_get("controller_mode", cur, sizeof(cur)) == 0 &&
            !strcmp(cur, "cloud")) {
            keys[nset] = "controller_mode";
            vals[nset] = "grbl";
            nset++;
            swept[1] = "controller_mode grbl";
        }
    }
    if (settings_set_many(keys, vals, nset) != 0)
        return reply_error(res, 500, "cannot write settings file");
    for (size_t i = 0; i < N_SETTINGS; i++) {
        const char *v = setting_param(req, setting_defs[i].key);
        if (!v)
            continue;
        fflog(LOG_NOTICE, "%s %s", setting_defs[i].key,
              !v[0] ? "cleared" :
              setting_defs[i].secret ? "set" : v);
    }
    for (int i = 0; i < 2; i++)
        if (swept[i])
            fflog(LOG_NOTICE, "%s (cloud mode off)", swept[i]);
    if (setting_param(req, "wifi_country"))
        apply_wifi(1);
    if (setting_param(req, "lid_lamp_idle"))
        cam_lamp_apply_idle();
    if (restart_grbl) {
        /* Idle by the gate above; the stop is the supervisor's safed
         * exit, and the start hands supervision back so the controller
         * respawns with the new mode. A stop that fails leaves the
         * running controller in place: the key is stored and applies
         * at its next start. */
        if (super_controller_stop() == 0) {
            super_controller_start();
            fflog(LOG_NOTICE, "xy_microsteps: controller restarted");
        } else
            fflog(LOG_WARNING, "xy_microsteps: controller did not stop; "
                               "the mode applies at its next start");
    }
    return reply_settings(res);
}

/* Manual emergency lever (the controller init scripts route their
 * stop/start here): stop the active controller and HOLD supervision
 * suspended - a bare pkill would be safed and respawned seconds later.
 * Deliberately NOT idle-gated: an emergency stop must work mid-job (the
 * supervisor's exit safing writes cnc/stop + laser latch). */
static int cb_controller_stop(const struct _u_request *req,
                              struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    if (super_controller_stop() != 0)
        return reply_error(res, 500, "controller did not stop");
    ulfius_set_string_body_response(res, 200, "{\"stopped\":true}");
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

static int cb_controller_start(const struct _u_request *req,
                               struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    if (diag_running())
        return reply_error(res, 409, "a diagnostic owns the hardware");
    super_controller_start();
    ulfius_set_string_body_response(res, 200, "{\"started\":true}");
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

/* The burned-in identity, on demand for the GF Cloud tab's viewer.
 * The values are irrevocable (fuses), so they are fetched only when
 * the operator explicitly asks to see them. */
static int cb_fuse_identity(const struct _u_request *req,
                            struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    /* The SRK password is irrevocable and never rotates - reveal it only
     * to someone physically at the machine holding the button. */
    if (!operator_present())
        return reply_error(res, 403,
            "hold the machine button to reveal the fuse identity");
    char body[192], mid[16], pw[65];
    unsigned long serial = fuse_serial();
    machine_id(mid, sizeof(mid));
    fuse_password(pw, sizeof(pw));
    if (serial)
        snprintf(body, sizeof(body),
                 "{\"serial\":\"%lu\",\"hostname\":\"%s\","
                 "\"password\":\"%s\"}", serial, mid, pw);
    else
        snprintf(body, sizeof(body),
                 "{\"serial\":\"\",\"hostname\":\"\",\"password\":\"\"}");
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    return U_CALLBACK_CONTINUE;
}

/* ---------------------------------------------------------------- mode */

static int cb_mode_get(const struct _u_request *req,
                       struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char body[512];                     /* the gate's why alone can reach 300 */
    super_status_json(body, sizeof(body));
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

static int cb_mode_post(const struct _u_request *req,
                        struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *mode = setting_param(req, "controller");
    if (!mode)
        return reply_error(res, 400, "controller is required");
    char err[96];
    if (super_mode_switch(mode, err, sizeof(err)) != 0)
        return reply_error(res, strstr(err, "must be") ? 400 : 409, err);
    return cb_mode_get(req, res, NULL);
}

/* ------------------------------------------------------------- cooling */

/* Level-triggered job-state report from the active controller (~1 Hz).
 * Query/form parameters: mode=idle|run|cooldown, armed=0|1, and an
 * optional per-job run fan profile (air_assist, exhaust, intake). */
static int cb_cool_state(const struct _u_request *req,
                         struct _u_response *res, void *user_data)
{
    (void)user_data;
    /* The thermal-safety report channel: only the controller on this
     * same host writes it. Restricting it to a loopback peer keeps a LAN
     * client from spoofing a stand-down that drops the exhaust mid-cut. */
    if (!auth_loopback_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *mode = setting_param(req, "mode");
    if (!mode)
        return reply_error(res, 400, "mode is required");

    const char *v = setting_param(req, "armed");
    int armed = v && atoi(v) != 0;
    /* The dose model the controller is cutting with, when it says.
     * Density is the only model on a machine; the analog value is kept
     * for the host-test builds that still exercise the reference mode. */
    v = setting_param(req, "model");
    cool_state_model(!v ? -1 : !strcmp(v, "density") ? 1 : !strcmp(v, "analog") ? 0 : -1);
    long duty[3] = {-1, -1, -1};
    static const char *duty_key[3] = {"air_assist", "exhaust", "intake"};
    for (int i = 0; i < 3; i++)
        if ((v = setting_param(req, duty_key[i])) != NULL)
            duty[i] = atol(v);

    /* The job's limits, when the report carries them (cloud mode): each
     * a number the engine takes only where it is stricter than its own;
     * absent or unparsable reads as absent. */
    cool_limits_t lim = {-1, -1, -1, -1, -1};
    static const char *lim_key[5] = {"coolant_max_c", "coolant_min_c",
                                     "exhaust_min_rpm", "intake_min_rpm",
                                     "air_assist_min_rpm"};
    double *lim_val[5] = {&lim.coolant_max_c, &lim.coolant_min_c,
                          &lim.exhaust_min_rpm, &lim.intake_min_rpm,
                          &lim.air_assist_min_rpm};
    for (int i = 0; i < 5; i++) {
        if ((v = setting_param(req, lim_key[i])) == NULL)
            continue;
        char *end;
        double d = strtod(v, &end);
        if (end != v && *end == '\0')
            *lim_val[i] = d;
    }

    if (cool_state_report(mode, armed, duty[0], duty[1], duty[2], &lim) != 0)
        return reply_error(res, 400, "mode must be idle, run or cooldown");
    ulfius_set_string_body_response(res, 200, "{\"ok\":true}");
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

/* The quiet hold for a listening to the head accelerometer (the bench's
 * tools): on=1 takes every fan off, the hold the commissioning finder
 * uses; pump=1 with it takes the coolant pump and the TEC off too, the
 * machine silent. Taken only from an idle machine with no diagnostic
 * running; released here, or by the engine itself when a run session
 * opens or the hold runs out of time (cool.h), so a listener that died
 * cannot leave the machine quiet. */
static int cb_cool_quiet(const struct _u_request *req,
                         struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *on = setting_param(req, "on");
    const char *pump = setting_param(req, "pump");
    if (!on || (strcmp(on, "0") && strcmp(on, "1")))
        return reply_error(res, 400, "on must be 0 or 1");
    if (pump && strcmp(pump, "0") && strcmp(pump, "1"))
        return reply_error(res, 400, "pump must be 0 or 1");
    if (!strcmp(on, "1")) {
        if (diag_running())
            return reply_error(res, 409, "a diagnostic is running");
        if (!machine_is_idle())
            return reply_error(res, 409, "machine is not idle");
    }
    cool_quiet_hold_ex(!strcmp(on, "1"), pump && !strcmp(pump, "1"));
    char body[48];
    snprintf(body, sizeof(body), "{\"quiet_hold\":%s}",
             cool_quiet_held() ? "true" : "false");
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

static int cb_cool_status(const struct _u_request *req,
                          struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char body[COOL_STATUS_JSON_MAX];
    if (cool_status_json(body, sizeof(body)) < 0)
        return reply_error(res, 500, "cool status document too long");
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

/* --------------------------------------------------------- diagnostics */

static int cb_diag_start(const struct _u_request *req,
                         struct _u_response *res, void *user_data)
{
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    /* A diagnostic seizes the thermal hardware; refuse it while a
     * firmware job is mid-flight (the update path does not otherwise
     * share diag's busy check). */
    if (update_job_running())
        return reply_error(res, 409, "an update job is running");
    switch (diag_start((const char *)user_data)) {
    case 0:
        ulfius_set_string_body_response(res, 202, "{\"started\":true}");
        ulfius_add_header_to_response(res, "Content-Type",
                                      "application/json");
        return U_CALLBACK_CONTINUE;
    case -1:
        return reply_error(res, 409, "a diagnostic is already running");
    case -2:
        return reply_error(res, 409, "machine is not idle");
    default:
        return reply_error(res, 400, "unknown diagnostic");
    }
}

static int cb_diag_abort(const struct _u_request *req,
                         struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    diag_abort();
    ulfius_set_string_body_response(res, 200, "{\"aborting\":true}");
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

static int cb_diag_status(const struct _u_request *req,
                          struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char body[4096];
    diag_status_json(body, sizeof(body));
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}


/* --------------------------------------------------------------- pages */

/* The three pages: each an embedded gzip bundle inflated once, with the
 * per-machine token spliced in place of the __FFTOKEN__ placeholder.
 * Serving the token inside a page (rather than from an endpoint any LAN
 * client could call) is what lets the origin checks keep it out of a
 * rebinding attacker's reach; and the panel and the wizard are served
 * only into a session (or before an account exists), so the token
 * travels only where the login did. The splice happens on the inflated
 * text, never inside the compressed stream. */
enum { PAGE_PANEL = 0, PAGE_WIZARD, PAGE_LOGIN, PAGE_COUNT };

static const char *page_html(int which)
{
    static char *pages[PAGE_COUNT];
    static pthread_mutex_t page_mu = PTHREAD_MUTEX_INITIALIZER;
    static const char fallback[] =
        "<!doctype html><title>ForgeFIRM</title>"
        "forgectrl: the page could not be unpacked";
    const unsigned char *gz;
    unsigned gz_len, len;
    switch (which) {
    case PAGE_WIZARD: gz = wizard_html_gz; gz_len = wizard_html_gz_len; len = wizard_html_len; break;
    case PAGE_LOGIN:  gz = login_html_gz;  gz_len = login_html_gz_len;  len = login_html_len;  break;
    default:          gz = index_html_gz;  gz_len = index_html_gz_len;  len = index_html_len;  which = PAGE_PANEL; break;
    }
    /* One inflate per page for the daemon's life, whichever request
     * thread gets there first; the others wait rather than inflate. */
    pthread_mutex_lock(&page_mu);
    if (pages[which]) {
        pthread_mutex_unlock(&page_mu);
        return pages[which];
    }

    char *html = malloc((size_t)len + 1);
    if (!html) {
        pthread_mutex_unlock(&page_mu);
        return fallback;
    }
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, 15 + 16) != Z_OK) {   /* 16: gzip wrapper */
        free(html);
        pthread_mutex_unlock(&page_mu);
        return fallback;
    }
    zs.next_in = (Bytef *)gz;
    zs.avail_in = gz_len;
    zs.next_out = (Bytef *)html;
    zs.avail_out = len;
    int rc = inflate(&zs, Z_FINISH);
    size_t n = zs.total_out;
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) {
        free(html);
        pthread_mutex_unlock(&page_mu);
        return fallback;
    }
    html[n] = '\0';

    const char *mark = strstr(html, "__FFTOKEN__");
    const char *tok = auth_token();
    if (!mark || !tok[0]) {
        pages[which] = html;            /* no token: serve the page inert */
        pthread_mutex_unlock(&page_mu);
        return pages[which];
    }
    size_t pre = (size_t)(mark - html);
    size_t tlen = strlen(tok);
    size_t total = n - strlen("__FFTOKEN__") + tlen + 1;
    char *page = malloc(total);
    if (!page) {
        pages[which] = html;
        pthread_mutex_unlock(&page_mu);
        return pages[which];
    }
    memcpy(page, html, pre);
    memcpy(page + pre, tok, tlen);
    strcpy(page + pre + tlen, mark + strlen("__FFTOKEN__"));
    free(html);
    pages[which] = page;
    pthread_mutex_unlock(&page_mu);
    return page;
}

static int serve_page(struct _u_response *res, int which)
{
    ulfius_set_string_body_response(res, 200, page_html(which));
    ulfius_add_header_to_response(res, "Content-Type", "text/html; charset=utf-8");
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    return U_CALLBACK_CONTINUE;
}

static int redirect_to(struct _u_response *res, const char *path)
{
    ulfius_set_string_body_response(res, 302, "");
    ulfius_add_header_to_response(res, "Location", path);
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    return U_CALLBACK_CONTINUE;
}

/* A path the login may return to: this origin's own, absolute, printable. */
static int next_path_ok(const char *p)
{
    if (!p || p[0] != '/' || p[1] == '/' || p[1] == '\\' || strlen(p) > 200)
        return 0;
    for (const unsigned char *c = (const unsigned char *)p; *c; c++)
        if (*c < 0x21 || *c > 0x7e)
            return 0;
    return 1;
}

/* To the login, carrying the path (with its query) that asked, so the
 * login can return there. */
static int redirect_login(const struct _u_request *req, struct _u_response *res)
{
    const char *path = req->http_url ? req->http_url : "/";
    char loc[512] = "/login";
    if (next_path_ok(path) && strcmp(path, "/") != 0) {
        char enc[440];
        size_t n = 0;
        for (const unsigned char *c = (const unsigned char *)path; *c && n + 4 < sizeof(enc); c++) {
            if (isalnum(*c) || strchr("-._~/", *c))
                enc[n++] = (char)*c;
            else
                n += (size_t)snprintf(enc + n, sizeof(enc) - n, "%%%02X", *c);
        }
        enc[n] = '\0';
        snprintf(loc, sizeof(loc), "/login?next=%s", enc);
    }
    return redirect_to(res, loc);
}

/* Which page a browser at "/" gets: the wizard while the first run is
 * incomplete (into a session once an account exists), the login page
 * without a session, else the panel. */
static int cb_root_page(const struct _u_request *req, struct _u_response *res,
                        void *user_data)
{
    (void)user_data;
    if (!auth_origin_ok(req, res))
        return U_CALLBACK_COMPLETE;
    int session = auth_session_ok(req);
    if (commission_first_run() || users_reset_pending()) {
        if (users_exist() && !session)
            return redirect_login(req, res);
        return serve_page(res, PAGE_WIZARD);
    }
    if (!session && !auth_peer_local(req))
        return redirect_login(req, res);
    wiz_panel_opened();
    return serve_page(res, PAGE_PANEL);
}

/* The wizard on demand, after the first run (re-runs, the Commissioning
 * tab's "continue"). */
static int cb_setup_page(const struct _u_request *req, struct _u_response *res,
                         void *user_data)
{
    (void)user_data;
    if (!auth_origin_ok(req, res))
        return U_CALLBACK_COMPLETE;
    if (users_exist() && !auth_session_ok(req) && !auth_peer_local(req))
        return redirect_login(req, res);
    return serve_page(res, PAGE_WIZARD);
}

static int cb_login_page(const struct _u_request *req, struct _u_response *res,
                         void *user_data)
{
    (void)user_data;
    if (!auth_origin_ok(req, res))
        return U_CALLBACK_COMPLETE;
    if (!users_exist())
        return redirect_to(res, "/");
    if (auth_session_ok(req)) {
        const char *next = u_map_get(req->map_url, "next");
        return redirect_to(res, next_path_ok(next) ? next : "/");
    }
    return serve_page(res, PAGE_LOGIN);
}

/* POST /login: name and password; a session cookie on success. The
 * token is not required here (the page that carries it is behind the
 * login), so this is the one state-changing route without it; the
 * per-address throttle and the constant-time verify are its guards. */
static int cb_login(const struct _u_request *req, struct _u_response *res,
                    void *user_data)
{
    (void)user_data;
    if (!auth_origin_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char peer[64];
    auth_peer_text(req, peer, sizeof(peer));
    int left = 0;
    if (login_locked(peer, &left)) {
        char body[96];
        snprintf(body, sizeof(body),
                 "{\"error\":\"too many attempts; wait %d s\",\"wait\":%d}",
                 left, left);
        ulfius_set_string_body_response(res, 429, body);
        ulfius_add_header_to_response(res, "Content-Type", "application/json");
        return U_CALLBACK_CONTINUE;
    }
    const char *name = setting_param(req, "name");
    const char *pw = setting_param(req, "password");
    if (!users_verify(name, pw)) {
        login_failed(peer);
        fflog(LOG_WARNING, "login failed from %s", peer[0] ? peer : "?");
        return reply_error(res, 401, "wrong name or password");
    }
    login_succeeded(peer);
    char sid[SESSION_ID_HEX + 1], cookie[256];
    if (session_create(name, sid, sizeof(sid)) != 0)
        return reply_error(res, 500, "cannot create a session");
    session_cookie_set(sid, cookie, sizeof(cookie));
    ulfius_add_header_to_response(res, "Set-Cookie", cookie);
    fflog(LOG_NOTICE, "login: %s from %s", name, peer[0] ? peer : "?");
    ulfius_set_string_body_response(res, 200, "{\"ok\":true}");
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

static int cb_logout(const struct _u_request *req, struct _u_response *res,
                     void *user_data)
{
    (void)user_data;
    if (!auth_origin_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *cookie = u_map_get_case(req->map_header, "Cookie");
    char id[SESSION_ID_HEX + 1], clear[128];
    if (cookie && session_from_cookie(cookie, id, sizeof(id)))
        session_end(id);
    session_cookie_clear(clear, sizeof(clear));
    ulfius_add_header_to_response(res, "Set-Cookie", clear);
    ulfius_set_string_body_response(res, 200, "{\"ok\":true}");
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

/* --------------------------------------------------------- certificate */

/* The certificate, checkable before the browser's warning is accepted:
 * served over plain HTTP too, with no redirect and no login, so a
 * person can read the fingerprint from the machine's own address and
 * compare it with what the HTTPS warning shows. Origin checks only: the
 * fingerprint is public, and a page that needs a session cannot help
 * someone who has not trusted the certificate yet. */
static int cb_cert_page(const struct _u_request *req, struct _u_response *res,
                        void *user_data)
{
    (void)user_data;
    if (!auth_origin_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *fp = tls_fingerprint();
    char mid[16], body[3072];
    machine_id(mid, sizeof(mid));
    snprintf(body, sizeof(body),
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>ForgeFIRM certificate</title>"
        "<style>body{margin:0;font:15px/1.55 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;"
        "color:#222;background:#f0f1f4}main{max-width:720px;margin:0 auto;padding:28px 20px}"
        "h1{font-size:22px;color:#2b2b5e;margin:0 0 10px}p{margin:0 0 12px}"
        "a{color:#0088cc}dl{display:grid;grid-template-columns:max-content 1fr;gap:6px 16px}"
        "dt{color:#767a82}dd{margin:0}code{font-family:ui-monospace,Consolas,monospace;"
        "font-size:14px;word-break:break-all}"
        "@media(prefers-color-scheme:dark){body{color:#d9dce3;background:#15161c}"
        "h1{color:#b9bcd8}a{color:#2ea3e0}dt{color:#9296a2}}</style></head><body><main>"
        "<h1>This machine's certificate</h1>"
        "<p>The control panel is served over HTTPS with a certificate the machine "
        "signed itself. Your browser will warn once and ask you to continue. Before you "
        "do, compare the fingerprint the warning shows with this one. They must match; "
        "if they do not, you reached another device.</p>"
        "<dl><dt>Machine</dt><dd>%s</dd>"
        "<dt>SHA-256</dt><dd><code>%s</code></dd>"
        "<dt>Names</dt><dd>%s</dd>"
        "<dt>Valid from</dt><dd>%s</dd>"
        "<dt>Valid until</dt><dd>%s</dd></dl>"
        "<p>%s</p>"
        "<p>The same fingerprint is on the panel's System tab and on the setup's "
        "welcome screen once you are in.</p></main></body></html>",
        mid[0] ? mid : "(unknown)",
        fp[0] ? fp : "not available: this image serves HTTP only",
        tls_names()[0] ? tls_names() : "(none)",
        tls_valid_from()[0] ? tls_valid_from() : "(unknown)",
        tls_valid_until()[0] ? tls_valid_until() : "(unknown)",
        fp[0] ? "<a href=\"/cert.pem\">Download the certificate (PEM)</a> to add it to a "
                "browser or a device that should trust it." : "");
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "text/html; charset=utf-8");
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    return U_CALLBACK_CONTINUE;
}

static int cb_cert_pem(const struct _u_request *req, struct _u_response *res,
                       void *user_data)
{
    (void)user_data;
    if (!auth_origin_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *pem = tls_cert_pem();
    if (!pem)
        return reply_error(res, 404, "this image serves HTTP only");
    ulfius_set_string_body_response(res, 200, pem);
    ulfius_add_header_to_response(res, "Content-Type", "application/x-pem-file");
    ulfius_add_header_to_response(res, "Content-Disposition",
                                  "attachment; filename=\"forgefirm.pem\"");
    return U_CALLBACK_CONTINUE;
}

/* ------------------------------------------------------------ licenses */

/* The license texts ride with the software: the image packs its license
 * manifest (every installed package with its license) and the texts into
 * one tar.gz, and the panel serves it here so an owner can read or pass
 * on the texts the licenses ask to travel with the binaries. Read-only,
 * open to the LAN like the status: the bundle is public by nature. */
#define LICENSES_BUNDLE "/usr/share/forgefirm/licenses.tar.gz"
#define LICENSES_MAX (16UL * 1024 * 1024)

static const char *licenses_path(void)
{
    const char *p = getenv("FORGECTRL_LICENSES");
    return p && *p ? p : LICENSES_BUNDLE;
}

static int cb_licenses(const struct _u_request *req, struct _u_response *res,
                       void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    FILE *f = fopen(licenses_path(), "rb");
    if (!f)
        return reply_error(res, 404, "this image carries no license bundle");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n <= 0 || (unsigned long)n > LICENSES_MAX) {
        fclose(f);
        return reply_error(res, 500, "the license bundle has an unexpected size");
    }
    char *buf = malloc((size_t)n);
    if (!buf) {
        fclose(f);
        return reply_error(res, 500, "out of memory");
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        free(buf);
        return reply_error(res, 500, "cannot read the license bundle");
    }
    char fwver[48], fn[128];
    read_fw_version(fwver, sizeof(fwver));
    for (char *p = fwver; *p; p++)
        if (*p == ' ' || *p == '(' || *p == ')' || *p == '"')
            *p = '_';
    snprintf(fn, sizeof(fn), "attachment; filename=\"forgefirm-licenses-%s.tar.gz\"",
             fwver[0] ? fwver : "unknown");
    ulfius_set_binary_body_response(res, 200, buf, (size_t)n);
    free(buf);
    ulfius_add_header_to_response(res, "Content-Type", "application/gzip");
    ulfius_add_header_to_response(res, "Content-Disposition", fn);
    return U_CALLBACK_CONTINUE;
}

/* The manifest text, pulled out of the bundle into a malloc'd buffer
 * (NUL-terminated; *len excludes it). Returns 0, or -1 with no bundle
 * or no manifest. */
static int licenses_manifest_read(char **out, size_t *len)
{
    *out = NULL;
    *len = 0;
    if (access(licenses_path(), R_OK) != 0)
        return -1;
    char cmd[400];
    snprintf(cmd, sizeof(cmd),
             "tar -xzOf '%s' common-licenses/license.manifest 2>/dev/null",
             licenses_path());
    FILE *p = popen(cmd, "r");
    if (!p)
        return -1;
    size_t cap = 512 * 1024, n = 0;
    char *buf = malloc(cap);
    if (!buf) {
        pclose(p);
        return -1;
    }
    size_t got;
    while ((got = fread(buf + n, 1, cap - n - 1, p)) > 0) {
        n += got;
        if (n + 1 >= cap)
            break;
    }
    pclose(p);
    buf[n] = '\0';
    if (!n) {
        free(buf);
        return -1;
    }
    *out = buf;
    *len = n;
    return 0;
}

/* The manifest alone, as text. */
static int cb_licenses_manifest(const struct _u_request *req,
                                struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char *buf;
    size_t len;
    if (licenses_manifest_read(&buf, &len) != 0)
        return reply_error(res, 404, "this image carries no license bundle");
    ulfius_set_binary_body_response(res, 200, buf, len);
    free(buf);
    ulfius_add_header_to_response(res, "Content-Type", "text/plain; charset=utf-8");
    return U_CALLBACK_CONTINUE;
}

/* The Licenses page every page's footer links: the manifest, readable,
 * with the download of the whole bundle above it. */
static int cb_licenses_page(const struct _u_request *req,
                            struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char *man = NULL;
    size_t mlen = 0;
    int have = licenses_manifest_read(&man, &mlen) == 0;
    char fwver[48];
    read_fw_version(fwver, sizeof(fwver));
    /* Escape the manifest for HTML (it is plain text from the build). */
    size_t cap = mlen * 6 + 4096;
    char *page = malloc(cap);
    if (!page) {
        free(man);
        return reply_error(res, 500, "out of memory");
    }
    size_t o = (size_t)snprintf(page, cap,
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>ForgeFIRM licenses</title>"
        "<style>body{margin:0;font:14px/1.5 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;"
        "color:#222;background:#f0f1f4}main{max-width:900px;margin:0 auto;padding:24px 20px}"
        "h1{font-size:22px;color:#2b2b5e;margin:0 0 6px}p{margin:0 0 12px}"
        "a{color:#0088cc}pre{background:#fff;border:1px solid #dde0e6;border-radius:8px;"
        "padding:14px;overflow:auto;font-size:12.5px}"
        "@media(prefers-color-scheme:dark){body{color:#d9dce3;background:#15161c}"
        "h1{color:#b9bcd8}a{color:#2ea3e0}pre{background:#1e2028;border-color:#32353f}}"
        "</style></head><body><main><h1>Licenses</h1>"
        "<p>ForgeFIRM %s. Every software package on this machine, with its license, "
        "is listed below. The full text of every license is in the bundle: "
        "<a href=\"/system/licenses\">download the license bundle (.tar.gz)</a>. "
        "The bundle is what the licenses ask to travel with the software; you can pass it "
        "on with the machine. The project's own components and their licenses are in the "
        "Licenses and notices document of the setup.</p>",
        fwver[0] ? fwver : "");
    if (have) {
        o += (size_t)snprintf(page + o, cap - o, "<pre>");
        for (size_t i = 0; i < mlen && o + 8 < cap; i++) {
            char c = man[i];
            if (c == '<')
                o += (size_t)snprintf(page + o, cap - o, "&lt;");
            else if (c == '>')
                o += (size_t)snprintf(page + o, cap - o, "&gt;");
            else if (c == '&')
                o += (size_t)snprintf(page + o, cap - o, "&amp;");
            else
                page[o++] = c;
        }
        o += (size_t)snprintf(page + o, cap - o, "</pre>");
    } else
        o += (size_t)snprintf(page + o, cap - o,
                              "<p>This image carries no license bundle.</p>");
    snprintf(page + o, cap - o, "</main></body></html>");
    free(man);
    ulfius_set_string_body_response(res, 200, page);
    free(page);
    ulfius_add_header_to_response(res, "Content-Type", "text/html; charset=utf-8");
    return U_CALLBACK_CONTINUE;
}

/* ----------------------------------------------------------------- ssh */

/* SSH is off at boot and on until the next reboot when the owner turns
 * it on here: the flag in /run gates the sshd init script, and the
 * daemon starts or stops sshd through it. */
#define SSH_FLAG_NAME "ssh-enabled"

static int ssh_running(void)
{
    return system("pgrep -x sshd >/dev/null 2>&1") == 0;
}

static int ssh_reply(struct _u_response *res)
{
    char flag[256], body[160];
    snprintf(flag, sizeof(flag), "%s/" SSH_FLAG_NAME, ff_run_dir());
    int enabled = access(flag, F_OK) == 0;
    snprintf(body, sizeof(body),
             "{\"enabled\":%s,\"running\":%s,\"dev_image\":%s}",
             enabled ? "true" : "false", ssh_running() ? "true" : "false",
             auth_dev_image() ? "true" : "false");
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    return U_CALLBACK_CONTINUE;
}

/* ---------------------------------------------------------- camera key */

/* The key a camera consumer puts in its URL, with the URLs ready to
 * paste; behind the login, since the key is a read credential. */
static int camkey_reply(const struct _u_request *req, struct _u_response *res)
{
    const char *host = u_map_get_case(req->map_header, "Host");
    char hb[160] = "";
    if (host) {
        size_t o = 0;
        if (host[0] == '[') {
            const char *e = strchr(host, ']');
            size_t n = e ? (size_t)(e - host + 1) : strlen(host);
            snprintf(hb, sizeof(hb), "%.*s", (int)n, host);
        } else {
            for (; host[o] && host[o] != ':' && o + 1 < sizeof(hb); o++)
                hb[o] = host[o];
            hb[o] = '\0';
        }
    }
    const char *k = camkey_get();
    char body[768];
    snprintf(body, sizeof(body),
             "{\"key\":\"%s\",\"stream\":\"http://%s/?action=stream&key=%s\","
             "\"snapshot\":\"http://%s/cam/snapshot?cam=lid&key=%s\","
             "\"stream_https\":\"https://%s/cam/stream?cam=lid&key=%s\"}",
             k, hb, k, hb, k, hb, k);
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    return U_CALLBACK_CONTINUE;
}

static int cb_camkey_get(const struct _u_request *req, struct _u_response *res,
                         void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    if (!camkey_get()[0])
        return reply_error(res, 503, "no camera key on this machine");
    return camkey_reply(req, res);
}

static int cb_camkey_rotate(const struct _u_request *req, struct _u_response *res,
                            void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *r = setting_param(req, "rotate");
    if (!r || strcmp(r, "1"))
        return reply_error(res, 400, "rotate=1 required");
    if (camkey_rotate() != 0)
        return reply_error(res, 500, "cannot make a new key");
    return camkey_reply(req, res);
}

static int cb_ssh_get(const struct _u_request *req, struct _u_response *res,
                      void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    return ssh_reply(res);
}

static int cb_ssh_post(const struct _u_request *req, struct _u_response *res,
                       void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *e = setting_param(req, "enable");
    if (!e || (strcmp(e, "0") && strcmp(e, "1")))
        return reply_error(res, 400, "enable must be 0 or 1");
    char flag[256];
    snprintf(flag, sizeof(flag), "%s/" SSH_FLAG_NAME, ff_run_dir());
    if (!strcmp(e, "1")) {
        mkdir(ff_run_dir(), 0755);
        int fd = open(flag, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            return reply_error(res, 500, "cannot write the ssh flag");
        close(fd);
        if (system("/etc/init.d/sshd start >/dev/null 2>&1") != 0)
            fflog(LOG_ERR, "ssh: sshd did not start");
        fflog(LOG_NOTICE, "ssh: enabled until the next reboot");
    } else {
        unlink(flag);
        (void)!system("/etc/init.d/sshd stop >/dev/null 2>&1");
        fflog(LOG_NOTICE, "ssh: disabled");
    }
    return ssh_reply(res);
}

/* ---------------------------------------------------------------- logs */

static int cb_logs_list(const struct _u_request *req,
                        struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char *body = logs_list_json();
    if (!body)
        return reply_error(res, 500, "out of memory");
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    free(body);
    return U_CALLBACK_CONTINUE;
}

/* Log content is token-gated like a write: it can carry network
 * addresses and protocol detail the open status endpoints never do. */
static int cb_logs_tail(const struct _u_request *req,
                        struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *name = u_map_get(req->map_url, "name");
    const char *l = u_map_get(req->map_url, "lines");
    const char *f = u_map_get(req->map_url, "from");
    long lines = l ? atol(l) : 200;
    long long from = f ? atoll(f) : -1;
    if (!name)
        return reply_error(res, 400, "name required");
    char *body = logs_tail_json(name, lines, from);
    if (!body)
        return reply_error(res, 404, "unknown logger");
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    free(body);
    return U_CALLBACK_CONTINUE;
}

static ssize_t export_stream_cb(void *cls, uint64_t pos, char *buf,
                                size_t max)
{
    (void)pos;
    ssize_t n = logs_export_read(cls, buf, max);
    if (n < 0)
        return U_STREAM_ERROR;
    if (n == 0)
        return U_STREAM_END;
    return n;
}

static void export_stream_free(void *cls)
{
    logs_export_end(cls);
}

/* Bundle every logger's files plus a system snapshot into a tar.gz.
 * Sanitized by default (for public issue reports); ?sanitize=0 keeps
 * everything. Streams; the staging area is removed when the stream
 * ends, however it ends. */
static int cb_logs_export(const struct _u_request *req,
                          struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *sv = setting_param(req, "sanitize");
    int sanitize = !(sv && (!strcmp(sv, "0") || !strcmp(sv, "false") ||
                            !strcmp(sv, "no")));
    /* A CPU-bound pass over every log on the single core: not during a
     * cut, where the cooling engine shares the daemon's priority. */
    if (!machine_is_idle())
        return reply_error(res, 409, "the machine is busy; export logs when it is idle");
    char err[160];
    logs_export_t *e = logs_export_begin(sanitize, settings_snapshot, record_snapshot,
                                        err, sizeof(err));
    if (!e)
        return reply_error(res, !strcmp(err, "busy") ? 409 : 500, err);
    char fn[96], ts[24];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);
    snprintf(fn, sizeof(fn), "attachment; filename=\"forgefirm-logs-%s%s"
             ".tar.gz\"", ts, sanitize ? "" : "-full");
    ulfius_add_header_to_response(res, "Content-Type", "application/gzip");
    ulfius_add_header_to_response(res, "Content-Disposition", fn);
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    ulfius_set_stream_response(res, 200, export_stream_cb, export_stream_free,
                               U_STREAM_SIZE_UNKNOWN, 16 * 1024, e);
    return U_CALLBACK_CONTINUE;
}

/* "/" serves the UI (src/ui/), plus the mjpg-streamer-compatible
 * ?action=stream / ?action=snapshot aliases many clients expect. */
/* "/" on the HTTPS listener: the mjpg-streamer aliases, else a page. */
static int cb_root(const struct _u_request *req, struct _u_response *res,
                   void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *action = u_map_get(req->map_url, "action");
    if (action) {
        if (!strcmp(action, "stream"))
            return do_stream(CAM_LID, res);
        if (!strcmp(action, "snapshot"))
            return do_snapshot(CAM_LID, 1, SNAP_Q_DEF, -1, res);
        return reply_error(res, 400, "unknown action");
    }
    return cb_root_page(req, res, NULL);
}

/* "/" on the HTTP listener: the aliases stay (LightBurn's camera URL);
 * a browser is sent to HTTPS, where the pages live. */
static int cb_root_http(const struct _u_request *req, struct _u_response *res,
                        void *user_data)
{
    (void)user_data;
    const char *action = u_map_get(req->map_url, "action");
    if (action)
        return cb_root(req, res, NULL);
    /* A loopback peer gets every route on this listener, the page too. */
    if (auth_peer_local(req))
        return cb_root_page(req, res, NULL);
    return cb_http_redirect(req, res, NULL);
}

/* Everything not read-only on the HTTP listener from a non-local peer:
 * a redirect to the same path on HTTPS. */
static int cb_http_redirect(const struct _u_request *req, struct _u_response *res,
                            void *user_data)
{
    (void)user_data;
    const char *host = u_map_get_case(req->map_header, "Host");
    char hb[160] = "";
    if (host) {
        /* Strip a port: the HTTPS listener has its own. */
        size_t o = 0;
        if (host[0] == '[') {
            const char *e = strchr(host, ']');
            size_t n = e ? (size_t)(e - host + 1) : strlen(host);
            snprintf(hb, sizeof(hb), "%.*s", (int)n, host);
        } else {
            for (; host[o] && host[o] != ':' && o + 1 < sizeof(hb); o++)
                hb[o] = host[o];
            hb[o] = '\0';
        }
    }
    char loc[512];
    const char *path = req->http_url ? req->http_url : "/";
    if (tls_port == 443)
        snprintf(loc, sizeof(loc), "https://%s%s", hb, path);
    else
        snprintf(loc, sizeof(loc), "https://%s:%u%s", hb, tls_port, path);
    return redirect_to(res, loc);
}

/* A write route reached over HTTP: this host only (the init scripts,
 * the controllers, the acceptance tool); anyone else is sent to HTTPS. */
struct route {
    const char *method;
    const char *path;
    int (*cb)(const struct _u_request *, struct _u_response *, void *);
    void *ud;
    int read_only;                  /* served to the LAN on HTTP too */
};

static int cb_http_local(const struct _u_request *req, struct _u_response *res,
                         void *user_data)
{
    const struct route *r = user_data;
    if (!auth_peer_local(req))
        return cb_http_redirect(req, res, NULL);
    return r->cb(req, res, r->ud);
}

/* ------------------------------------------------------------------ main */

/* A requirement's fact outside the machine block: the cloud decision. */
static int commission_fact(const char *fact)
{
    if (!strcmp(fact, "cloud_enabled"))
        return settings_get_bool("cloud_enabled", 0);
    return 0;
}

/* libmicrohttpd's own messages (MHD_USE_ERROR_LOG): a client that drops
 * a stream, a TLS handshake a browser probes and abandons. Through fflog
 * at debug, not to stderr, where the service log took them as warnings. */
static void mhd_log(void *cls, const char *fmt, va_list ap)
{
    (void)cls;
    char msg[256];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    size_t n = strlen(msg);
    while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r'))
        msg[--n] = '\0';
    if (n)
        fflog(LOG_DEBUG, "mhd: %s", msg);
}

int main(int argc, char **argv)
{
    unsigned port = DEFAULT_PORT;
    const char *v = getenv("FORGECTRL_PORT");
    if (v && atoi(v) > 0 && atoi(v) < 65536)
        port = (unsigned)atoi(v);
    v = getenv("FORGECTRL_TLS_PORT");
    if (v && atoi(v) > 0 && atoi(v) < 65536)
        tls_port = (unsigned)atoi(v);

    /* Boot-time helper: render the rsyslog rules from the settings and
     * leave. Runs before rsyslog (and before this daemon) starts. */
    if (argc > 1 && !strcmp(argv[1], "--render-syslog")) {
        char err[160];
        if (logs_render(err, sizeof(err)) != 0) {
            fprintf(stderr, "forgectrl: render-syslog: %s\n", err);
            return 1;
        }
        return 0;
    }
    if (argc > 1) {
        fprintf(stderr, "usage: forgectrl [--render-syslog]\n");
        return 2;
    }

    fflog_init("forgectrl");

    /* Stay well below the motion feeder (SCHED_FIFO) and the controller;
     * best effort. */
    (void)!nice(5);

    /* The daemon is the dead-man for hung controllers and the sole
     * cooling-hardware writer: under memory pressure it must outlive
     * the processes it supervises. Controllers respawn at -500 (see
     * super.c), so they are reclaimed first and the daemon safes and
     * respawns them. */
    int ofd = open("/proc/self/oom_score_adj", O_WRONLY);
    if (ofd >= 0) {
        (void)!write(ofd, "-900", 4);
        close(ofd);
    }

    /* Raise the descriptor ceiling: the daemon is thread-per-connection,
     * so a connection flood must not exhaust the fd table and make
     * sysfs reads fail - machine_is_idle() fails closed on that now, but
     * a higher ceiling keeps the daemon serving through the flood. */
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < 4096) {
        rl.rlim_cur = rl.rlim_max < 4096 ? rl.rlim_max : 4096;
        (void)setrlimit(RLIMIT_NOFILE, &rl);
    }

    /* Installed before any thread or child exists: a termination that
     * lands during a slow init must still run the shutdown path. */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    auth_init();
    advisories_init();
    sheetid_init();
    camkey_init();
    users_init();
    {
        char sp[256];
        mkdir(ff_run_dir(), 0755);
        snprintf(sp, sizeof(sp), "%s/sessions", ff_run_dir());
        session_set_store(sp);
    }
    /* The account reset: the button held through the daemon's start,
     * for ten seconds, with the LED blinking amber while it counts.
     * The old password stops working at once; the wizard's account
     * step runs again. A press that is not a hold costs nothing. */
    if (button_held()) {
        led_set(LED_BLINK_AMBER);
        if (button_held_for(10)) {
            users_mark_reset();
            commission_init();
            commission_clear_account();
            fflog(LOG_WARNING, "account reset by the button hold at start");
        }
        led_release();
    }
    commission_init();
    int have_tls = tls_init() == 0;
    wiz_init();
    cam_engine_init();
    curverec_init();
    cool_init();
    super_init();
    diag_init();            /* its marker recovery drives the supervisor: after it */
    wizdark_init();
    commission_fact_hook = commission_fact;
    update_init();
    apply_wifi(0);
    cam_lamp_apply_idle();

    /* Every route, once. The HTTPS listener serves all of them; the
     * HTTP listener serves the read-only ones to the LAN (the camera
     * for LightBurn, the status), the write ones to this host only
     * (the init scripts, the controllers, the acceptance tool on the
     * board), and sends every other request to HTTPS. */
    static const struct route routes[] = {
        { "GET",  "/",                     cb_root,             NULL, 0 },
        { "GET",  "/setup",                cb_setup_page,       NULL, 0 },
        { "GET",  "/login",                cb_login_page,       NULL, 0 },
        { "POST", "/login",                cb_login,            NULL, 0 },
        { "POST", "/logout",               cb_logout,           NULL, 0 },
        { "GET",  "/cam/stream",           cb_stream,           NULL, 1 },
        { "GET",  "/cam/h264",             cb_h264,             NULL, 1 },
        { "GET",  "/cam/snapshot",         cb_snapshot,         NULL, 1 },
        { "GET",  "/cam/status",           cb_status,           NULL, 1 },
        { "GET",  "/settings",             cb_settings_get,     NULL, 1 },
        { "GET",  "/grbl/settings",        cb_grbl_settings,    NULL, 1 },
        { "POST", "/curve/record",         cb_curve_record,     NULL, 0 },
        { "POST", "/curve/stop",           cb_curve_stop,       NULL, 0 },
        { "GET",  "/curve/status",         cb_curve_status,     NULL, 1 },
        { "GET",  "/curve/ladder.gcode",   cb_curve_ladder,     NULL, 1 },
        { "POST", "/settings",             cb_settings_post,    NULL, 0 },
        { "GET",  "/status",               cb_machine_status,   NULL, 1 },
        { "GET",  "/fuse-identity",        cb_fuse_identity,    NULL, 0 },
        { "GET",  "/mode",                 cb_mode_get,         NULL, 1 },
        { "POST", "/mode",                 cb_mode_post,        NULL, 0 },
        { "POST", "/controller/stop",      cb_controller_stop,  NULL, 0 },
        { "POST", "/controller/start",     cb_controller_start, NULL, 0 },
        { "POST", "/cool/state",           cb_cool_state,       NULL, 0 },
        { "GET",  "/cool/status",          cb_cool_status,      NULL, 1 },
        { "POST", "/cool/quiet",           cb_cool_quiet,       NULL, 0 },
        { "POST", "/diag/flow-verify",     cb_diag_start,       "flow-verify", 0 },
        { "POST", "/diag/flow-calibrate",  cb_diag_start,       "flow-calibrate", 0 },
        { "POST", "/diag/aa-offset-calibrate", cb_diag_start,   "aa-offset-calibrate", 0 },
        { "POST", "/diag/abort",           cb_diag_abort,       NULL, 0 },
        { "GET",  "/diag/status",          cb_diag_status,      NULL, 1 },
        { "GET",  "/slots",                cb_slots,            NULL, 1 },
        { "POST", "/boot",                 cb_boot_select,      NULL, 0 },
        { "POST", "/system/reboot",        cb_system_reboot,    NULL, 0 },
        { "GET",  "/system/ssh",           cb_ssh_get,          NULL, 0 },
        { "POST", "/system/ssh",           cb_ssh_post,         NULL, 0 },
        { "GET",  "/system/camera-key",    cb_camkey_get,       NULL, 0 },
        { "GET",  "/cert",                 cb_cert_page,        NULL, 1 },
        { "GET",  "/cert.pem",             cb_cert_pem,         NULL, 1 },
        { "GET",  "/licenses",             cb_licenses_page,    NULL, 1 },
        { "GET",  "/system/licenses",      cb_licenses,         NULL, 1 },
        { "GET",  "/system/licenses/manifest", cb_licenses_manifest, NULL, 1 },
        { "POST", "/system/camera-key",    cb_camkey_rotate,    NULL, 0 },
        { "POST", "/update/check",         cb_update_check,     NULL, 0 },
        { "POST", "/update/download",      cb_update_download,  NULL, 0 },
        { "POST", "/update/apply",         cb_update_apply,     NULL, 0 },
        { "POST", "/update/upload",        cb_update_upload,    NULL, 0 },
        { "POST", "/restore/factory",      cb_restore_factory,  NULL, 0 },
        { "POST", "/restore/factory-return", cb_restore_factory_return, NULL, 0 },
        { "GET",  "/update/status",        cb_update_status,    NULL, 1 },
        { "GET",  "/logs",                 cb_logs_list,        NULL, 0 },
        { "GET",  "/logs/tail",            cb_logs_tail,        NULL, 0 },
        { "POST", "/logs/export",          cb_logs_export,      NULL, 0 },
        { "GET",  "/wiz",                  cb_wiz_status,       NULL, 1 },
        { "GET",  "/wiz/record",           cb_wiz_record,       NULL, 0 },
        { "GET",  "/wiz/record.html",      cb_wiz_record_html,  NULL, 0 },
        { "POST", "/wiz/changed",          cb_wiz_changed,      NULL, 0 },
        { "GET",  "/advisories/:id",       cb_advisory_get,    NULL, 1 },
        { "POST", "/wiz/advisories/accept", cb_wiz_agree,       NULL, 0 },
        { "POST", "/wiz/advisories/press", cb_wiz_press_start,  NULL, 0 },
        { "GET",  "/wiz/advisories/press", cb_wiz_press_status, NULL, 1 },
        { "POST", "/wiz/advisories/press/cancel", cb_wiz_press_cancel, NULL, 0 },
        { "POST", "/wiz/account",          cb_wiz_account,      NULL, 0 },
        { "POST", "/wiz/preferences",      cb_wiz_preferences,  NULL, 0 },
        { "POST", "/wiz/machine",          cb_wiz_machine,      NULL, 0 },
        { "POST", "/wiz/cloud",            cb_wiz_cloud,        NULL, 0 },
        { "POST", "/wiz/complete",         cb_wiz_complete,     NULL, 0 },
        { "GET",  "/wiz/dark",             cb_wiz_dark_status,  NULL, 1 },
        { "GET",  "/wiz/shot",             cb_wiz_shot,         NULL, 1 },
        { "GET",  "/wiz/sheet.svg",        cb_wiz_sheet_svg,    NULL, 1 },
        { "GET",  "/wiz/sheet.gcode",      cb_wiz_sheet_gcode,  NULL, 1 },
        { "POST", "/wiz/:id/start",        cb_wiz_dark_start,   NULL, 0 },
        { "POST", "/wiz/:id/answer",       cb_wiz_dark_answer,  NULL, 0 },
        { "POST", "/wiz/:id/abort",        cb_wiz_dark_abort,   NULL, 0 },
        { "POST", "/wiz/:id/takeover",     cb_wiz_dark_takeover, NULL, 0 },
    };

    struct _u_instance inst, tls;
    /* Dual-stack listeners: one socket each serves IPv4 and IPv6. */
    if (ulfius_init_instance_ipv6(&inst, port, NULL, U_USE_ALL, NULL) != U_OK) {
        fflog(LOG_ERR, "ulfius init failed");
        return 1;
    }
    if (have_tls &&
        ulfius_init_instance_ipv6(&tls, tls_port, NULL, U_USE_ALL, NULL) != U_OK) {
        fflog(LOG_ERR, "ulfius init failed (https)");
        have_tls = 0;
    }
    /* Cap the body the framework keeps in memory. ulfius accumulates
     * every POST body into its own buffer before any endpoint callback
     * runs, so before the token check; its default is 0, which means no
     * limit, and an unauthenticated client on the network could stream
     * until the board ran out of memory and took this process, and any
     * job with it. The ceiling bounds that to this much per connection,
     * against the 64-connection and 16-per-IP limits set below.
     *
     * Safe for the firmware upload, which is much larger than this: the
     * cap truncates only the framework's own copy, and its post
     * processor still receives every chunk in full, so the multipart
     * file parts reach update_upload_sink and stream to disk as before.
     * Nothing here reads that raw copy - the form routes read the
     * parsed body map, which the post processor fills either way.
     * The largest legitimate form body is a settings POST, whose
     * longest single value is a 180-character dose curve. */
    inst.max_post_body_size = 64 * 1024;
    tls.max_post_body_size = 64 * 1024;
    /* The path goes in as the endpoint's url_format, not its url_prefix:
     * ulfius matches both, but fills map_url with the :name parameters
     * of the format only (/advisories/:id). */
    for (size_t i = 0; i < sizeof(routes) / sizeof(*routes); i++) {
        const struct route *r = &routes[i];
        if (have_tls)
            ulfius_add_endpoint_by_val(&tls, r->method, NULL, r->path, 0, r->cb, r->ud);
        if (!strcmp(r->path, "/") && !strcmp(r->method, "GET"))
            ulfius_add_endpoint_by_val(&inst, "GET", NULL, "/", 0,
                                       have_tls ? cb_root_http : cb_root, NULL);
        else if (r->read_only || !have_tls)
            ulfius_add_endpoint_by_val(&inst, r->method, NULL, r->path, 0, r->cb, r->ud);
        else
            ulfius_add_endpoint_by_val(&inst, r->method, NULL, r->path, 0,
                                       cb_http_local, (void *)r);
    }
    if (have_tls) {
        ulfius_set_default_endpoint(&inst, cb_http_redirect, NULL);
        ulfius_set_upload_file_callback_function(&tls, &update_upload_sink, NULL);
    }
    ulfius_set_upload_file_callback_function(&inst, &update_upload_sink, NULL);

    /* The flags ulfius computes for this configuration (a thread per
     * connection, its error log, the internal polling thread, dual
     * stack from U_USE_ALL), reproduced verbatim so only the caps are
     * new: a connection ceiling and a per-IP ceiling bound a flood at
     * the accept side (uncapped, a 500-connection flood plateaued at
     * 379 fds). MHD's own messages (a client that drops a stream, a TLS
     * handshake a browser probes and abandons) go through fflog at
     * debug, not to stderr as warnings. */
    struct MHD_OptionItem mhd_ops[] = {
        /* ulfius' own connection plumbing, required by its dispatcher
         * and externalized for exactly this call: the per-request state
         * is created by its URI logger and freed by its completion
         * callback. */
        { MHD_OPTION_NOTIFY_COMPLETED, (intptr_t)mhd_request_completed, NULL },
        { MHD_OPTION_URI_LOG_CALLBACK, (intptr_t)ulfius_uri_logger, NULL },
        { MHD_OPTION_EXTERNAL_LOGGER, (intptr_t)mhd_log, NULL },
        { MHD_OPTION_CONNECTION_LIMIT, 64, NULL },
        { MHD_OPTION_PER_IP_CONNECTION_LIMIT, 16, NULL },
        { MHD_OPTION_END, 0, NULL },
    };
    unsigned int mhd_flags = MHD_USE_THREAD_PER_CONNECTION |
                             MHD_USE_ERROR_LOG |
                             MHD_USE_INTERNAL_POLLING_THREAD |
                             MHD_USE_DUAL_STACK;
    if (ulfius_start_framework_with_mhd_options(&inst, mhd_flags,
                                                mhd_ops) != U_OK) {
        fflog(LOG_ERR, "cannot start HTTP on port %u", port);
        ulfius_clean_instance(&inst);
        return 1;
    }
    fflog(LOG_NOTICE, "listening on port %u", port);
    if (have_tls) {
        /* The same options plus the key and the certificate; TLS is
         * libmicrohttpd's, whatever ulfius was built with. */
        struct MHD_OptionItem tls_ops[] = {
            { MHD_OPTION_NOTIFY_COMPLETED, (intptr_t)mhd_request_completed, NULL },
            { MHD_OPTION_URI_LOG_CALLBACK, (intptr_t)ulfius_uri_logger, NULL },
            { MHD_OPTION_EXTERNAL_LOGGER, (intptr_t)mhd_log, NULL },
            { MHD_OPTION_CONNECTION_LIMIT, 64, NULL },
            { MHD_OPTION_PER_IP_CONNECTION_LIMIT, 16, NULL },
            { MHD_OPTION_HTTPS_MEM_KEY, 0, (void *)tls_key_pem() },
            { MHD_OPTION_HTTPS_MEM_CERT, 0, (void *)tls_cert_pem() },
            { MHD_OPTION_END, 0, NULL },
        };
        if (ulfius_start_framework_with_mhd_options(&tls, mhd_flags | MHD_USE_TLS,
                                                    tls_ops) != U_OK) {
            fflog(LOG_ERR, "cannot start HTTPS on port %u", tls_port);
            ulfius_clean_instance(&tls);
            have_tls = 0;
        } else
            fflog(LOG_NOTICE, "listening on port %u (https, %s)", tls_port,
                  tls_fingerprint());
    }
    if (!have_tls)
        fflog(LOG_WARNING, "HTTPS unavailable: every route is on HTTP");

    while (!quit)
        pause();

    fflog(LOG_NOTICE, "shutting down");
    wiz_abort_all();
    wizdark_shutdown();
    ulfius_stop_framework(&inst);
    ulfius_clean_instance(&inst);
    if (have_tls) {
        ulfius_stop_framework(&tls);
        ulfius_clean_instance(&tls);
    }
    /* A recording in progress ends here and puts the floor and curve
     * it overrode back; left running, the raw curve would be in force
     * for every job after the restart. */
    curverec_stop();
    super_shutdown();
    cool_shutdown();
    cam_engine_shutdown();
    return 0;
}
