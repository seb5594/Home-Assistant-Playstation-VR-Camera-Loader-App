#include "mic_calibration.h"

#include <math.h>
#include <string.h>

static const double TIME_CONSTANT_S = 20.0;
static const float MAX_CORRECTION = 4.0f;       /* +-12 dB */
static const double DEAD_CHANNEL_RATIO = 1e-3;  /* 30 dB below the loudest capsule */
static const double CLIP_LEVEL = 30000.0;

void mic_calibration_init(mic_calibration_t *cal, int sample_rate, int block)
{
    memset(cal, 0, sizeof(*cal));
    cal->smoothing = exp(-(double)block / (sample_rate * TIME_CONSTANT_S));
    for (int c = 0; c < CALIBRATION_CHANNELS; c++) {
        cal->gain[c] = 1.0f;
        cal->active[c] = 1;
    }
}

int mic_calibration_active_channels(const mic_calibration_t *cal)
{
    int count = 0;
    for (int c = 0; c < CALIBRATION_CHANNELS; c++) count += cal->active[c];
    return count;
}

void mic_calibration_process(mic_calibration_t *cal, float *frames, int count)
{
    double power[CALIBRATION_CHANNELS] = {0}, peak = 0.0;

    for (int i = 0; i < count; i++) {
        for (int c = 0; c < CALIBRATION_CHANNELS; c++) {
            double v = frames[i * CALIBRATION_CHANNELS + c];
            power[c] += v * v;
            if (fabs(v) > peak) peak = fabs(v);
        }
    }
    /* Clipped blocks would pull the louder capsules down; skip them. */
    if (peak < CLIP_LEVEL && count > 0) {
        double a = cal->warm ? cal->smoothing : 0.0, loudest = 0.0, mean = 0.0;
        int active = 0;
        for (int c = 0; c < CALIBRATION_CHANNELS; c++) {
            cal->power[c] = a * cal->power[c] + (1.0 - a) * power[c] / count;
            if (cal->power[c] > loudest) loudest = cal->power[c];
        }
        cal->warm = 1;
        for (int c = 0; c < CALIBRATION_CHANNELS; c++) {
            cal->active[c] = loudest > 0 && cal->power[c] > loudest * DEAD_CHANNEL_RATIO;
            if (cal->active[c]) {
                mean += cal->power[c];
                active++;
            }
        }
        mean = active ? mean / active : 0.0;
        for (int c = 0; c < CALIBRATION_CHANNELS; c++) {
            float target = 0.0f;
            if (cal->active[c] && mean > 0) {
                target = (float)sqrt(mean / cal->power[c]);
                if (target > MAX_CORRECTION) target = MAX_CORRECTION;
                if (target < 1.0f / MAX_CORRECTION) target = 1.0f / MAX_CORRECTION;
            }
            cal->gain[c] = target;
        }
    }
    for (int i = 0; i < count; i++)
        for (int c = 0; c < CALIBRATION_CHANNELS; c++)
            frames[i * CALIBRATION_CHANNELS + c] *= cal->gain[c];
}
