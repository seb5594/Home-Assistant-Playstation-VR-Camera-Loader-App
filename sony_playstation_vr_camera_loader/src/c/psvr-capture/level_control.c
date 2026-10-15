#include "level_control.h"

#include <math.h>
#include <string.h>

static const float TARGET_DBFS = -18.0f;
static const float MAX_GAIN_DB = 18.0f;
static const float MIN_GAIN_DB = 0.0f;      /* never quieter than the input; the limiter handles peaks */
static const float NOISE_CEILING_DBFS = -50.0f;
static const float GAIN_RISE_DB_PER_S = 3.0f;
static const float LIMIT_KNEE = 0.85f * 32767.0f;

static float db_to_gain(float db) { return powf(10.0f, db / 20.0f); }

void level_control_init(level_control_t *lc, int auto_gain, int sample_rate, int block)
{
    float seconds = (float)block / sample_rate;
    memset(lc, 0, sizeof(*lc));
    lc->auto_gain = auto_gain;
    lc->gain = 1.0f;
    lc->target = 32767.0f * db_to_gain(TARGET_DBFS);
    lc->max_gain = db_to_gain(MAX_GAIN_DB);
    lc->min_gain = db_to_gain(MIN_GAIN_DB);
    lc->rise_per_block = db_to_gain(GAIN_RISE_DB_PER_S * seconds);
    speech_detector_init(&lc->detector, sample_rate, block);
}

float level_control_gain_db(const level_control_t *lc) { return 20.0f * log10f(lc->gain); }

float level_control_noise_floor_dbfs(const level_control_t *lc) { return lc->detector.floor_dbfs; }

/* Highest gain that keeps the current noise floor below the ceiling. */
static float noise_limited_gain(const level_control_t *lc)
{
    float allowed = db_to_gain(NOISE_CEILING_DBFS - lc->detector.floor_dbfs);
    if (allowed > lc->max_gain) return lc->max_gain;
    return allowed < lc->min_gain ? lc->min_gain : allowed;
}

/* Soft knee above LIMIT_KNEE; never exceeds full scale. */
static float limit(float x)
{
    float magnitude = fabsf(x);
    if (magnitude <= LIMIT_KNEE) return x;
    float room = 32767.0f - LIMIT_KNEE;
    float shaped = LIMIT_KNEE + room * tanhf((magnitude - LIMIT_KNEE) / room);
    return x < 0 ? -shaped : shaped;
}

static void follow_speech(level_control_t *lc, float rms, float ceiling)
{
    /* fast attack, slow release on the measured speech level */
    float a = rms > lc->speech_level ? 0.6f : 0.05f;
    lc->speech_level = lc->speech_level > 0 ? (1 - a) * lc->speech_level + a * rms : rms;
    float desired = lc->target / lc->speech_level;
    if (desired > ceiling) desired = ceiling;
    if (desired < lc->min_gain) desired = lc->min_gain;
    if (desired < lc->gain)
        lc->gain = 0.7f * lc->gain + 0.3f * desired;
    else if (lc->gain * lc->rise_per_block < desired)
        lc->gain *= lc->rise_per_block;
    else
        lc->gain = desired;
}

void level_control_process(level_control_t *lc, float *samples, int count, float voice_probability, int gap)
{
    float previous_gain = lc->gain;

    if (count > 0) {
        lc->speech = speech_detector_process(&lc->detector, samples, count, voice_probability, gap);
        if (lc->auto_gain) {
            float ceiling = noise_limited_gain(lc);
            if (lc->speech) {
                float rms = 32768.0f * powf(10.0f, lc->detector.level_dbfs / 20.0f);
                follow_speech(lc, rms, ceiling);
            } else if (lc->gain > ceiling) {
                lc->gain = 0.9f * lc->gain + 0.1f * ceiling;   /* the room got louder: ease down */
            }
        }
    }
    /* Ramp across the block so gain steps never click. */
    for (int i = 0; i < count; i++) {
        float g = previous_gain + (lc->gain - previous_gain) * (float)(i + 1) / count;
        samples[i] = limit(samples[i] * g);
    }
}
