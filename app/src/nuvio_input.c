#include "nuvio_input.h"

#include "evo_boot_trace.h"

#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ScePadData, as EVO Player reads it. */
typedef struct {
    uint32_t buttons;
    uint8_t  sticks[4];      /* left x, left y, right x, right y; 128 = centre */
    uint8_t  analog[4];      /* L2, R2 */
    float    orientation[4];
    float    acceleration[3];
    float    angular_velocity[3];
    uint8_t  touch[24];
    uint8_t  rest[72];
} pad_data;

int scePadReadState(int handle, pad_data *data);
int scePadOpen(int user_id, int type, int index, void *param);
int scePadClose(int handle);

/* Held D-pad: first repeat after 400 ms, then every 90 ms. */
#define REPEAT_DELAY 0.40
#define REPEAT_EVERY 0.09
/* Stick deflection that counts as a D-pad press (of 127). */
#define STICK_ON  88
#define STICK_OFF 60

static int s_pad = -1;
static uint32_t s_last;
static uint32_t s_ignore;        /* down at open; ignored until released */
static uint32_t s_stick;         /* directions the left stick is holding */
static double s_dir_since;
static double s_next_repeat;

static pthread_mutex_t s_inject_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t s_inject_tap;    /* one-frame presses waiting */
static uint32_t s_inject_hold;   /* held until s_inject_until */
static double s_inject_until;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void nuvio_input_open(int user_id)
{
    pad_data pad;

    if (s_pad < 0)
        s_pad = scePadOpen(user_id, 0, 0, NULL);
    evo_bt("input: pad open -> %d", s_pad);
    memset(&pad, 0, sizeof pad);
    s_ignore = (s_pad >= 0 && scePadReadState(s_pad, &pad) == 0) ? pad.buttons : 0;
    s_last = s_ignore;
    s_stick = 0;
    s_dir_since = s_next_repeat = 0.0;
}

void nuvio_input_close(void)
{
    if (s_pad >= 0)
        scePadClose(s_pad);
    s_pad = -1;
    s_last = s_ignore = s_stick = 0;
}

void nuvio_input_inject(uint32_t button, int hold_ms)
{
    pthread_mutex_lock(&s_inject_lock);
    if (hold_ms > 0) {
        s_inject_hold |= button;
        s_inject_until = now_s() + hold_ms / 1000.0;
    } else {
        s_inject_tap |= button;
    }
    pthread_mutex_unlock(&s_inject_lock);
}

uint32_t nuvio_input_button_named(const char *name)
{
    static const struct { const char *name; uint32_t bit; } k[] = {
        {"cross", NUVIO_BTN_CROSS},       {"circle", NUVIO_BTN_CIRCLE},
        {"square", NUVIO_BTN_SQUARE},     {"triangle", NUVIO_BTN_TRIANGLE},
        {"up", NUVIO_BTN_UP},             {"down", NUVIO_BTN_DOWN},
        {"left", NUVIO_BTN_LEFT},         {"right", NUVIO_BTN_RIGHT},
        {"options", NUVIO_BTN_OPTIONS},   {"touchpad", NUVIO_BTN_TOUCHPAD},
        {"l1", NUVIO_BTN_L1},             {"r1", NUVIO_BTN_R1},
        {"l2", NUVIO_BTN_L2},             {"r2", NUVIO_BTN_R2},
        {"l3", NUVIO_BTN_L3},             {"r3", NUVIO_BTN_R3},
    };
    for (unsigned i = 0; name && i < sizeof k / sizeof k[0]; i++)
        if (!strcasecmp(name, k[i].name))
            return k[i].bit;
    return 0;
}

/* The left stick as a D-pad, with hysteresis so a stick resting near the
 * threshold does not chatter. */
static uint32_t stick_dirs(const pad_data *pad)
{
    const int x = (int)pad->sticks[0] - 128;
    const int y = (int)pad->sticks[1] - 128;
    uint32_t d = 0;
    int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    int on_x = (s_stick & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT)) ? STICK_OFF : STICK_ON;
    int on_y = (s_stick & (NUVIO_BTN_UP | NUVIO_BTN_DOWN)) ? STICK_OFF : STICK_ON;

    /* One axis at a time: the dominant one. */
    if (ax >= ay && ax > on_x)
        d = x < 0 ? NUVIO_BTN_LEFT : NUVIO_BTN_RIGHT;
    else if (ay > ax && ay > on_y)
        d = y < 0 ? NUVIO_BTN_UP : NUVIO_BTN_DOWN;
    s_stick = d;
    return d;
}

void nuvio_input_poll(nuvio_input_state *out)
{
    pad_data pad;
    uint32_t now_buttons = 0;
    const double t = now_s();

    memset(out, 0, sizeof *out);
    memset(&pad, 0, sizeof pad);
    if (s_pad >= 0 && scePadReadState(s_pad, &pad) == 0) {
        now_buttons = pad.buttons;
        if (!(now_buttons & NUVIO_BTN_DPAD))
            now_buttons |= stick_dirs(&pad);
    }

    /* Buttons held when the pad was opened stay ignored until let go. */
    s_ignore &= now_buttons;
    now_buttons &= ~s_ignore;

    pthread_mutex_lock(&s_inject_lock);
    if (s_inject_hold && t >= s_inject_until)
        s_inject_hold = 0;
    now_buttons |= s_inject_hold;
    {
        /* A tap is down for exactly one poll; one already down is re-pressed. */
        uint32_t tap = s_inject_tap;
        s_inject_tap = 0;
        out->pressed |= tap;
        now_buttons |= tap;
        s_last &= ~tap;
    }
    pthread_mutex_unlock(&s_inject_lock);

    out->pressed |= now_buttons & ~s_last;
    out->released = s_last & ~now_buttons;
    out->held = now_buttons;

    /* Auto-repeat for one held direction. */
    {
        const uint32_t dir = now_buttons & NUVIO_BTN_DPAD;
        if (out->pressed & NUVIO_BTN_DPAD) {
            s_dir_since = t;
            s_next_repeat = t + REPEAT_DELAY;
        } else if (dir && t >= s_next_repeat && s_dir_since > 0.0) {
            out->pressed |= dir;
            out->repeats |= dir;
            s_next_repeat = t + REPEAT_EVERY;
        }
        if (!dir)
            s_dir_since = 0.0;
        out->held_for = dir && s_dir_since > 0.0 ? t - s_dir_since : 0.0;
    }
    s_last = now_buttons;
}
