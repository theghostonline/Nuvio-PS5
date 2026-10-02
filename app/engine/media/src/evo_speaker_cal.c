/*
 * Module: evo_speaker_cal - see media/include/evo_speaker_cal.h.
 */
#include "evo_speaker_cal.h"
#include "evo_data_path.h"
#include "evo_boot_log.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CAL_FILE      "speaker_calibration.cfg"
#define CAL_RATE      48000
#define CAL_DELAY_MAX ((int)(CAL_RATE * EVO_SPEAKER_CAL_MAX_DELAY_MS / 1000.0f) + 1)

/* UI-thread side: the published profile and its generation. */
static evo_speaker_cal_t s_published;
static volatile unsigned s_generation = 0;

/* Audio-thread side: the profile in use, and one delay line per channel. */
static unsigned s_applied_generation = ~0u;
static int      s_active = 0;
static float    s_gain[EVO_SPEAKER_CAL_CHANNELS];
static int      s_delay[EVO_SPEAKER_CAL_CHANNELS];
static int16_t  s_line[EVO_SPEAKER_CAL_CHANNELS][CAL_DELAY_MAX];
static int      s_line_pos = 0;
static volatile int s_reset_pending = 0;

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void sanitize(evo_speaker_cal_t *c)
{
    for (int i = 0; i < EVO_SPEAKER_CAL_CHANNELS; i++) {
        /* NaN fails every comparison, so clampf would pass it through */
        if (!(c->gain_db[i] == c->gain_db[i]))   c->gain_db[i] = 0.0f;
        if (!(c->delay_ms[i] == c->delay_ms[i])) c->delay_ms[i] = 0.0f;
        c->gain_db[i]  = clampf(c->gain_db[i], -EVO_SPEAKER_CAL_MAX_TRIM_DB,
                                EVO_SPEAKER_CAL_MAX_TRIM_DB);
        c->delay_ms[i] = clampf(c->delay_ms[i], 0.0f, EVO_SPEAKER_CAL_MAX_DELAY_MS);
    }
    c->enabled = c->enabled ? 1 : 0;
}

static void publish(const evo_speaker_cal_t *c)
{
    s_published = *c;
    sanitize(&s_published);
    __sync_synchronize();
    s_generation++;
}

void evo_speaker_cal_load(void)
{
    evo_speaker_cal_t c;
    memset(&c, 0, sizeof(c));

    FILE *f = fopen(evo_data_path(CAL_FILE), "r");
    if (f) {
        /* line 1: enabled; lines 2..9: "<gain_db> <delay_ms>" per channel */
        int ok = (fscanf(f, "%d", &c.enabled) == 1);
        for (int i = 0; ok && i < EVO_SPEAKER_CAL_CHANNELS; i++)
            ok = (fscanf(f, "%f %f", &c.gain_db[i], &c.delay_ms[i]) == 2);
        fclose(f);
        if (!ok) {
            evo_boot_log("speaker_cal: %s unreadable - calibration off", CAL_FILE);
            memset(&c, 0, sizeof(c));
        }
    }
    publish(&c);
    if (c.enabled)
        evo_boot_log("speaker_cal: profile loaded (FL %+.1f dB / %.2f ms ...)",
                     s_published.gain_db[0], s_published.delay_ms[0]);
}

int evo_speaker_cal_apply(const evo_speaker_cal_t *cal)
{
    if (!cal)
        return 0;
    publish(cal);

    FILE *f = fopen(evo_data_path(CAL_FILE), "w");
    if (!f) {
        evo_boot_log("speaker_cal: save failed");
        return 0;
    }
    fprintf(f, "%d\n", s_published.enabled);
    for (int i = 0; i < EVO_SPEAKER_CAL_CHANNELS; i++)
        fprintf(f, "%.2f %.3f\n", s_published.gain_db[i], s_published.delay_ms[i]);
    fclose(f);
    evo_boot_log("speaker_cal: profile applied + saved (enabled=%d)", s_published.enabled);
    return 1;
}

void evo_speaker_cal_get(evo_speaker_cal_t *out)
{
    if (out)
        *out = s_published;
}

void evo_speaker_cal_reset(void)
{
    s_reset_pending = 1;
}

static void pick_up_profile(void)
{
    const unsigned gen = s_generation;
    if (gen == s_applied_generation)
        return;
    __sync_synchronize();
    const evo_speaker_cal_t c = s_published;
    s_applied_generation = gen;

    s_active = 0;
    for (int i = 0; i < EVO_SPEAKER_CAL_CHANNELS; i++) {
        s_gain[i]  = c.enabled ? powf(10.0f, c.gain_db[i] / 20.0f) : 1.0f;
        s_delay[i] = c.enabled ? (int)lrintf(c.delay_ms[i] * CAL_RATE / 1000.0f) : 0;
        if (s_delay[i] >= CAL_DELAY_MAX)
            s_delay[i] = CAL_DELAY_MAX - 1;
        if (s_gain[i] != 1.0f || s_delay[i] != 0)
            s_active = 1;
    }
    s_reset_pending = 1;
}

void evo_speaker_cal_process(int16_t *pcm, int frames, int channels)
{
    if (!pcm || frames <= 0 || channels != EVO_SPEAKER_CAL_CHANNELS)
        return;
    pick_up_profile();
    if (!s_active)
        return;
    if (s_reset_pending) {
        memset(s_line, 0, sizeof(s_line));
        s_line_pos = 0;
        s_reset_pending = 0;
    }

    for (int n = 0; n < frames; n++) {
        int16_t *frame = pcm + (size_t)n * EVO_SPEAKER_CAL_CHANNELS;
        for (int ch = 0; ch < EVO_SPEAKER_CAL_CHANNELS; ch++) {
            float v = (float)frame[ch];
            if (s_delay[ch] > 0) {
                int rd = s_line_pos - s_delay[ch];
                if (rd < 0)
                    rd += CAL_DELAY_MAX;
                s_line[ch][s_line_pos] = frame[ch];
                v = (float)s_line[ch][rd];
            }
            v *= s_gain[ch];
            frame[ch] = (int16_t)(v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
        }
        if (++s_line_pos >= CAL_DELAY_MAX)
            s_line_pos = 0;
    }
}
