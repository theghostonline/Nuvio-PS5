#include "nuvio_control.h"

#include "nuvio_bridge.h"
#include "nuvio_input.h"
#include "nuvio_shot.h"

#include "evo_boot_trace.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_started;
static volatile int s_playing;
static volatile int s_stop;
static volatile int s_quit;

/* The report slot: the render loop writes it, the worker sends it. */
static struct {
    char id[64];
    double position;
    double duration;
    int dirty;
} s_report;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void handle(const nuvio_command *c)
{
    if (!strcmp(c->cmd, "key")) {
        const uint32_t bit = nuvio_input_button_named(c->button);
        if (bit && s_playing)
            nuvio_input_inject(bit, c->value);
    } else if (!strcmp(c->cmd, "stop")) {
        if (s_playing)       /* a stop for a playback that already ended is stale */
            s_stop = 1;
    } else if (!strcmp(c->cmd, "quit")) {
        s_quit = 1;
    } else if (!strcmp(c->cmd, "shot")) {
        nuvio_shot_post();
    }
    evo_bt("control: %s %s %d", c->cmd, c->button, c->value);
}

static void send_report(void)
{
    char id[64];
    double position, duration;

    pthread_mutex_lock(&s_lock);
    if (!s_report.dirty) {
        pthread_mutex_unlock(&s_lock);
        return;
    }
    memcpy(id, s_report.id, sizeof id);
    position = s_report.position;
    duration = s_report.duration;
    s_report.dirty = 0;
    pthread_mutex_unlock(&s_lock);

    nuvio_bridge_state(id, "playing", position, duration, NULL);
}

/* Loop count and what the worker is doing, for the player's status line. */
volatile unsigned nuvio_control_beats;
volatile int nuvio_control_stage;

static void *worker(void *arg)
{
    double next_report = 0.0;
    (void)arg;
    for (;;) {
        nuvio_control_beats++;
        nuvio_control_stage = 1;
        nuvio_command cmds[16];
        const int n = nuvio_bridge_commands(cmds, 16);
        nuvio_control_stage = 2;
        for (int i = 0; i < n; i++)
            handle(&cmds[i]);
        nuvio_control_stage = 3;

        const double t = now_s();
        if (t >= next_report) {
            nuvio_control_stage = 4;
            if (s_playing)
                send_report();
            next_report = t + 5.0;
        }
        nuvio_control_stage = 5;
        usleep(s_playing ? 100 * 1000 : 250 * 1000);
    }
    return NULL;
}

void nuvio_control_start(void)
{
    pthread_t t;
    if (s_started)
        return;
    if (pthread_create(&t, NULL, worker, NULL) == 0) {
        pthread_detach(t);
        s_started = 1;
    }
}

void nuvio_control_set_playing(int playing)
{
    s_playing = playing;
    s_stop = 0;
}

int nuvio_control_take_stop(void)
{
    if (!s_stop)
        return 0;
    s_stop = 0;
    return 1;
}

int nuvio_control_quit_requested(void)
{
    return s_quit;
}

void nuvio_control_report(const char *id, double position, double duration)
{
    pthread_mutex_lock(&s_lock);
    snprintf(s_report.id, sizeof s_report.id, "%s", id ? id : "");
    s_report.position = position;
    s_report.duration = duration;
    s_report.dirty = 1;
    pthread_mutex_unlock(&s_lock);
}
