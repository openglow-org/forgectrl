/*
 * jobpost.c - the job runner's routes: POST /job, GET /job, POST /job/abort
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * See jobpost.h. One upload at a time: the sink stages the program as it
 * arrives, the route hands the staged file to the runner, and the runner
 * opens it and takes its name away, so the next upload never writes
 * under a job that plays.
 */
#define _GNU_SOURCE
#include "jobpost.h"
#include "auth.h"
#include "jobrun.h"
#include "lease.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LIT_WITHIN_MAX_S 3600.0
#define TIMEOUT_MAX_S    86400.0

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *up_fp;
static const void *up_owner;            /* the request that streams, by identity */
static double up_last;
static size_t up_bytes;
static int up_error;                    /* the HTTP status of the refusal, or 0 */
static char up_refusal[160];

static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void staging_path(char *buf, size_t len)
{
    const char *d = getenv("GF_RUN_DIR");
    snprintf(buf, len, "%s/job.upload", d && *d ? d : "/run/forgefirm");
}

static int is_job_post(const struct _u_request *req)
{
    const char *u = req->http_url;
    return u && !strncmp(u, "/job", 4) && (u[4] == '\0' || u[4] == '?');
}

static int reply_text(struct _u_response *res, unsigned status, const char *msg)
{
    ulfius_set_string_body_response(res, status, msg);
    ulfius_add_header_to_response(res, "Content-Type", "text/plain");
    return U_CALLBACK_CONTINUE;
}

static int reply_record(struct _u_response *res)
{
    char body[768];
    if (jobrun_status_json(body, sizeof(body)) != 0)
        return reply_text(res, 500, "the job's record does not fit");
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

static void refuse_locked(int status, const char *why)
{
    up_error = status;
    snprintf(up_refusal, sizeof(up_refusal), "%s", why);
    if (up_fp) {
        fclose(up_fp);
        up_fp = NULL;
        char path[192];
        staging_path(path, sizeof(path));
        unlink(path);
    }
}

int jobpost_sink(const struct _u_request *req, const char *key, const char *data,
                 uint64_t off, size_t size)
{
    if (!is_job_post(req))
        return 0;
    /* Unauthorized bytes touch nothing: not the staging file, not an
     * authorized upload in flight. The route answers them. */
    if (!auth_write_permitted_cap(req, "motion.job") || !key || strcmp(key, "program"))
        return 1;

    pthread_mutex_lock(&mu);
    if (off == 0) {
        /* A second upload while the first still streams is refused, not
         * adopted; one whose client went away mid-stream is abandoned
         * after a minute of silence. */
        if (up_owner && up_owner != req && mono_s() - up_last < 60) {
            pthread_mutex_unlock(&mu);
            return 1;                   /* the route reports the refusal */
        }
        if (up_fp)
            fclose(up_fp);
        up_fp = NULL;
        up_owner = req;
        up_bytes = 0;
        up_error = 0;
        up_refusal[0] = '\0';
        /* No point staging megabytes the runner is going to refuse. The
         * extension host's upload under an extension's own hold is the
         * route's to judge by its name. */
        char held[128], path[192], h[LEASE_OWNER_MAX];
        staging_path(path, sizeof(path));
        int ext_held = lease_holder(h, sizeof(h)) && !strncmp(h, "ext:", 4);
        if (!(auth_by_host() && ext_held) && lease_refusal(held, sizeof(held)))
            refuse_locked(409, held);
        else if (!(up_fp = fopen(path, "wb")))
            refuse_locked(500, "cannot open the staging file");
    }
    if (up_owner == req && !up_error) {
        up_last = mono_s();
        if (up_bytes + size > JOBRUN_PROGRAM_MAX)
            refuse_locked(413, "the program exceeds the size limit");
        else if (size && fwrite(data, 1, size, up_fp) != size)
            refuse_locked(500, "writing the staging file failed");
        else
            up_bytes += size;
    }
    pthread_mutex_unlock(&mu);
    return 1;
}

static double field_s(const struct _u_request *req, const char *key, double max, int *bad)
{
    const char *v = u_map_get(req->map_post_body, key);
    if (!v || !*v)
        return 0;
    char *end;
    double d = strtod(v, &end);
    if (*end || !(d >= 0) || d > max)
        *bad = 1;
    return d;
}

int cb_job_post(const struct _u_request *req, struct _u_response *res, void *user_data)
{
    (void)user_data;
    /* The sink staged nothing for an unauthorized request, and an
     * authorized upload in flight is not this request's to discard. */
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;

    char path[192], why[160];
    staging_path(path, sizeof(path));
    pthread_mutex_lock(&mu);
    if (up_owner != req) {
        int streaming = up_owner != NULL;
        pthread_mutex_unlock(&mu);
        return streaming ? reply_text(res, 409, "another program is being uploaded")
                         : reply_text(res, 400, "no program was received (the file part is `program`)");
    }
    if (up_fp)
        fclose(up_fp);
    up_fp = NULL;
    up_owner = NULL;
    int status = up_error;
    size_t bytes = up_bytes;
    snprintf(why, sizeof(why), "%s", up_refusal);
    pthread_mutex_unlock(&mu);

    if (status)
        return reply_text(res, (unsigned)status, why);
    if (bytes == 0) {
        unlink(path);
        return reply_text(res, 400, "the program is empty");
    }
    const char *name = u_map_get(req->map_post_body, "name");
    int bad = 0;
    double lit_s = field_s(req, "lit_within_s", LIT_WITHIN_MAX_S, &bad);
    double run_s = field_s(req, "timeout_s", TIMEOUT_MAX_S, &bad);
    if (bad) {
        unlink(path);
        return reply_text(res, 400, "lit_within_s is 0 to 3600 and timeout_s 0 to 86400, in seconds");
    }
    if (!name || !*name || jobrun_name_ok(name) != 0) {
        unlink(path);
        return reply_text(res, 400, "name says who sends the job: 1 to 63 of letters, digits, "
                                    "'.', '_' and '-'");
    }
    int lines = 0;
    if (jobrun_program_check(path, &lines, why, sizeof(why)) != 0) {
        unlink(path);
        return reply_text(res, 400, why);
    }
    /* An extension that keeps the Grbl sender out holds the machine as
     * ext:<id>, and the job its host sends in its name runs under that
     * hold. The name is the host's word, and no one else's. */
    char ext_owner[LEASE_OWNER_MAX], holder[LEASE_OWNER_MAX];
    const char *under = NULL;
    snprintf(ext_owner, sizeof(ext_owner), "ext:%s", name);
    if (auth_by_host() && lease_holder(holder, sizeof(holder)) && !strcmp(holder, ext_owner))
        under = ext_owner;
    const char *unlock = u_map_get(req->map_post_body, "unlock");
    if (jobrun_program_start(path, name, under, lit_s, run_s, unlock && !strcmp(unlock, "1"),
                             why, sizeof(why)) != 0)
        return reply_text(res, 409, why);
    return reply_record(res);
}

int cb_job_status(const struct _u_request *req, struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    return reply_record(res);
}

int cb_job_abort(const struct _u_request *req, struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char why[160];
    if (jobrun_program_abort(why, sizeof(why)) != 0)
        return reply_text(res, 409, why);
    return reply_record(res);
}
