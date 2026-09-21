/*
 * events_test.c - host test for the event stream
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Three parts. The cap: three streams in all, an address that already
 * holds one given the same slot again (its older stream is replaced), a
 * released slot reused. The edge detector: a table of state steps and
 * the events each one is (the machine lease's among them), a step that
 * changes nothing being no event.
 * The stream end to end over a scripted state: the greeting, an edge
 * arriving as a numbered event, the fourth client refused in words, a
 * second stream from one address ending the first with a "bye" and
 * leaving the count where it was, a reader a whole ring behind told
 * what it lost, no state read while nobody listens, and every open
 * stream ending at shutdown. The extension host's stream: its own slot,
 * taken while the three are full and giving none of them up, and a
 * second host stream replacing the first.
 */
#include "../src/events.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

/* ---- the edge detector ---- */

static char got[512];

static void collect(void *ctx, const char *name, const char *data)
{
    (void)ctx;
    size_t n = strlen(got);
    snprintf(got + n, sizeof(got) - n, "%s%s %s", n ? "|" : "", name, data);
}

static events_snap_t base_snap(void)
{
    events_snap_t s;
    memset(&s, 0, sizeof(s));
    s.switches = 1ul << 3;                      /* lid closed, interlock satisfied */
    snprintf(s.mode, sizeof(s.mode), "grbl");
    snprintf(s.controller, sizeof(s.controller), "running");
    snprintf(s.verdict, sizeof(s.verdict), "OK");
    s.fire_ok = 1;
    snprintf(s.gstate, sizeof(s.gstate), "Idle");
    s.homed_axes = 4;
    snprintf(s.home_source, sizeof(s.home_source), "startup");
    return s;
}

static void step(const char *what, const events_snap_t *a, const events_snap_t *b, const char *want)
{
    got[0] = '\0';
    events_diff(a, b, collect, NULL);
    CHECK(!strcmp(got, want), "%s: got '%s', want '%s'", what, got, want);
}

static void test_diff(void)
{
    events_snap_t a = base_snap(), b = a;
    step("nothing changed", &a, &b, "");

    b.switches &= ~(1ul << 3);
    step("lid opened", &a, &b, "lid {\"closed\":false}");
    step("lid closed", &b, &a, "lid {\"closed\":true}");

    b = a;
    b.switches |= 1ul << 5;
    step("interlock opened", &a, &b, "interlock {\"ok\":false}");

    b = a;
    b.switches |= 1ul << 2;
    step("the button alone", &a, &b, "");

    b = a;
    snprintf(b.mode, sizeof(b.mode), "cloud");
    step("mode", &a, &b, "mode.changed {\"mode\":\"cloud\"}");

    b = a;
    snprintf(b.controller, sizeof(b.controller), "stopped");
    step("controller stopped", &a, &b, "controller.stopped {\"mode\":\"grbl\",\"state\":\"stopped\"}");
    step("controller started", &b, &a, "controller.started {\"mode\":\"grbl\",\"state\":\"running\"}");
    events_snap_t c = b;
    snprintf(c.controller, sizeof(c.controller), "motion-fault");
    step("stopped to a fault", &b, &c, "controller.stopped {\"mode\":\"grbl\",\"state\":\"motion-fault\"}");

    b = a;
    snprintf(b.verdict, sizeof(b.verdict), "TEMP_HIGH");
    b.fire_ok = 0;
    step("verdict", &a, &b, "cooling.verdict {\"verdict\":\"TEMP_HIGH\",\"fire_ok\":false}");

    /* A job: the button wait, the window, a lid hold and its end, a
     * cooling hold, the end; and an end in alarm. */
    b = a;
    b.arming = 1;
    step("arming", &a, &b, "job.arming {}");
    c = b;
    c.arming = 0;
    c.armed = 1;
    snprintf(c.gstate, sizeof(c.gstate), "Run");
    step("armed", &b, &c, "job.armed {}");
    events_snap_t d = c;
    snprintf(d.gstate, sizeof(d.gstate), "Door");
    step("paused by the lid", &c, &d, "job.paused {\"reason\":\"lid\"}");
    step("resumed", &d, &c, "job.resumed {}");
    d = c;
    snprintf(d.gstate, sizeof(d.gstate), "Hold");
    step("paused by a hold", &c, &d, "job.paused {\"reason\":\"hold\"}");
    d.fire_ok = 0;
    snprintf(d.verdict, sizeof(d.verdict), "FLOW");
    step("paused by cooling", &c, &d,
         "cooling.verdict {\"verdict\":\"FLOW\",\"fire_ok\":false}|job.paused {\"reason\":\"cooling\"}");
    d = c;
    d.armed = 0;
    snprintf(d.gstate, sizeof(d.gstate), "Idle");
    step("ended", &c, &d, "job.ended {\"result\":\"ended\"}");
    d.alarm = 3;
    snprintf(d.gstate, sizeof(d.gstate), "Alarm");
    step("ended in alarm", &c, &d, "job.ended {\"result\":\"alarm\"}|alarm {\"code\":3}");
    step("an alarm cleared is no event", &d, &a, "");

    /* Homing: a session that ends homed, one that does not, and a manual
     * home, which has no session. */
    b = a;
    snprintf(b.gstate, sizeof(b.gstate), "Home");
    step("homing started", &a, &b, "homing.started {}");
    c = a;
    c.homed_axes = 7;
    snprintf(c.home_source, sizeof(c.home_source), "gfcloud");
    step("homing completed", &b, &c, "homing.completed {\"source\":\"gfcloud\",\"axes\":7}");
    step("homing failed", &b, &a, "homing.failed {}");
    snprintf(c.home_source, sizeof(c.home_source), "manual");
    step("a manual home", &a, &c, "homing.completed {\"source\":\"manual\",\"axes\":7}");

    b = a;
    b.released = 1;
    step("released", &a, &b, "motors.released {}");
    step("energized", &b, &a, "motors.energized {}");

    /* The machine lease: taken, passed to an owner under the holder, given back. */
    b = a;
    snprintf(b.lease, sizeof(b.lease), "wizard:cooling.flow");
    step("lease taken", &a, &b, "lease.changed {\"owner\":\"wizard:cooling.flow\"}");
    c = b;
    snprintf(c.lease, sizeof(c.lease), "diag:flow-calibrate");
    step("lease nested", &b, &c, "lease.changed {\"owner\":\"diag:flow-calibrate\"}");
    step("lease given back", &b, &a, "lease.changed {\"owner\":null}");
}

/* ---- the cap ---- */

static void test_admit(void)
{
    events_slots_t s;
    int rep;
    memset(&s, 0, sizeof(s));
    int a = events_admit(&s, "10.0.0.1", &rep), b = events_admit(&s, "10.0.0.2", &rep),
        c = events_admit(&s, "::1", &rep);
    CHECK(a >= 0 && b >= 0 && c >= 0 && a != b && b != c && a != c && !rep, "three peers: %d %d %d", a, b, c);
    CHECK(events_admit(&s, "10.0.0.9", &rep) == EVENTS_ADMIT_FULL, "the fourth stream was admitted");
    CHECK(events_admit(&s, "10.0.0.2", &rep) == b && rep, "an address that holds a slot did not get it again");
    CHECK(events_admit(&s, "10.0.0.9", &rep) == EVENTS_ADMIT_FULL, "a replacement made room for a fourth");
    events_release(&s, b);
    CHECK(events_admit(&s, "10.0.0.1", &rep) == a && rep, "one per address, with a slot free");
    CHECK(events_admit(&s, "10.0.0.9", &rep) == b && !rep, "a released slot was not reused");
}

/* ---- the stream ---- */

static events_snap_t scripted;
static pthread_mutex_t smu = PTHREAD_MUTEX_INITIALIZER;
static volatile int gathers;

static void gather(events_snap_t *s)
{
    pthread_mutex_lock(&smu);
    *s = scripted;
    pthread_mutex_unlock(&smu);
    gathers++;
}

static void *blocked_reader(void *arg)
{
    char buf[512];
    long n;
    while ((n = events_next(arg, buf, sizeof(buf))) >= 0) { }
    return NULL;
}

static void test_stream(void)
{
    char buf[512], why[128];
    scripted = base_snap();
    events_gather = gather;
    events_init();

    usleep(500 * 1000);
    CHECK(gathers == 0, "the state was read %d times with nobody listening", gathers);

    events_client_t *c1 = events_open("10.0.0.1", why, sizeof(why));
    CHECK(c1 != NULL, "the first stream was refused: %s", why);
    if (!c1)
        return;
    long n = events_next(c1, buf, sizeof(buf));
    CHECK(n > 0 && strstr(buf, "event: hello\n") && strstr(buf, "\"max_streams\":3"), "the greeting: '%s'", buf);

    usleep(600 * 1000);                         /* the baseline is taken: no event for it */
    CHECK(gathers >= 2, "the sampler did not start with a listener (%d reads)", gathers);
    pthread_mutex_lock(&smu);
    scripted.switches &= ~(1ul << 3);
    pthread_mutex_unlock(&smu);
    n = events_next(c1, buf, sizeof(buf));
    CHECK(n > 0 && !strcmp(buf, "id: 1\nevent: lid\ndata: {\"closed\":false}\n\n"), "the first edge: '%s'", buf);

    /* One per address, by replacement: the older stream gets its "bye" and
     * ends, the newer one reads on, and the older one's close does not
     * give away the place the newer one holds. */
    events_client_t *old = c1;
    c1 = events_open("10.0.0.1", why, sizeof(why));
    CHECK(c1 != NULL, "a second stream from one address was refused: %s", why);
    n = events_next(old, buf, sizeof(buf));
    CHECK(n > 0 && !strcmp(buf, "event: bye\ndata: {\"reason\":\"replaced\"}\n\n"), "the replaced stream: '%s'", buf);
    CHECK(events_next(old, buf, sizeof(buf)) < 0, "the replaced stream did not end");
    events_close(old);
    n = events_next(c1, buf, sizeof(buf));
    CHECK(n > 0 && strstr(buf, "event: hello\n"), "the replacing stream's greeting: '%s'", buf);

    /* The cap, in words. */
    events_client_t *c2 = events_open("10.0.0.2", why, sizeof(why));
    events_client_t *c3 = events_open("10.0.0.3", why, sizeof(why));
    CHECK(c2 && c3, "the second and third streams");
    events_client_t *c4 = events_open("10.0.0.4", why, sizeof(why));
    CHECK(!c4 && strstr(why, "every event stream is taken"), "the fourth stream: '%s'", why);

    /* The extension host's slot is its own: it is there with the three
     * taken, it takes none of them, and one curl on the machine cannot
     * have it. */
    events_client_t *host = events_open_host(why, sizeof(why));
    CHECK(host != NULL, "the extension host's stream was refused with the three full: %s", why);
    if (host) {
        CHECK(events_open("10.0.0.5", why, sizeof(why)) == NULL,
              "the host's stream made room for a fourth ordinary client");
        long hn = events_next(host, buf, sizeof(buf));
        CHECK(hn > 0 && strstr(buf, "event: hello\n"), "the host's greeting: '%s'", buf);
        events_client_t *host2 = events_open_host(why, sizeof(why));
        CHECK(host2 != NULL, "a restarted host was refused its slot: %s", why);
        hn = events_next(host, buf, sizeof(buf));
        CHECK(hn > 0 && !strcmp(buf, "event: bye\ndata: {\"reason\":\"replaced\"}\n\n"),
              "the older host stream was not replaced: '%s'", buf);
        events_close(host);
        events_close(host2);
        CHECK(events_open("10.0.0.5", why, sizeof(why)) == NULL,
              "the host's slot was given away to an ordinary client when it let go");
    }

    events_close(c3);
    c3 = events_open("10.0.0.4", why, sizeof(why));
    CHECK(c3 != NULL, "a closed stream's place was not given to the next client: %s", why);

    /* A reader a whole ring behind is told what it lost, then reads on. */
    events_next(c2, buf, sizeof(buf));          /* its greeting */
    for (int i = 0; i < 70; i++)
        events_publish("test", "{}");
    n = events_next(c2, buf, sizeof(buf));
    CHECK(n > 0 && buf[0] == ':' && strstr(buf, "6 events were lost"), "a slow reader: '%s'", buf);
    n = events_next(c2, buf, sizeof(buf));
    CHECK(n > 0 && !strncmp(buf, "id: 8\nevent: test\n", 18), "the oldest event still held: '%s'", buf);

    /* Shutdown ends a stream that is blocked waiting. */
    events_next(c3, buf, sizeof(buf));          /* its greeting; it has read nothing else */
    events_client_t *waiting = c1;
    while (events_next(waiting, buf, sizeof(buf)) > 0 && strncmp(buf, "id: 71\n", 7)) { }
    pthread_t th;
    pthread_create(&th, NULL, blocked_reader, waiting);
    usleep(200 * 1000);
    events_shutdown();
    pthread_join(th, NULL);
    CHECK(events_next(c2, buf, sizeof(buf)) < 0, "a stream outlived the shutdown");
    CHECK(events_open("10.0.0.9", why, sizeof(why)) == NULL, "a stream opened after the shutdown");
    events_close(c1);
    events_close(c2);
    events_close(c3);
}

int main(void)
{
    test_admit();
    test_diff();
    test_stream();
    printf(fails ? "events_test: %d FAILED\n" : "events_test: all passed\n", fails);
    return fails ? 1 : 0;
}
