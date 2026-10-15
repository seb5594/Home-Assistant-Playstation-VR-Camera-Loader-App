#include "speech_detector.h"

#include <math.h>
#include <string.h>

static const float HISTORY_SECONDS = 5.0f;
static const float FLOOR_PERCENTILE = 0.10f;
static const float ONSET_SECONDS = 0.048f;
static const float HANGOVER_SECONDS = 0.2f;
static const float SILENCE_DBFS = -96.0f;     /* below one LSB: no microphone is that quiet */
static const float MIN_SPEECH_DBFS = -70.0f;

/* dB over the floor: { start, hold } without and with RNNoise */
static const float START_DB = 8.0f, HOLD_DB = 4.0f;
static const float DENOISED_START_DB = 3.0f, DENOISED_HOLD_DB = 1.0f;
static const float VOICE_START = 0.6f, VOICE_HOLD = 0.3f;

static const float BIN_DB = 0.5f;
static const float LOWEST_DBFS = -120.0f;

static int level_bin(float dbfs)
{
    int bin = (int)((dbfs - LOWEST_DBFS) / BIN_DB);
    if (bin < 0) return 0;
    return bin >= SPEECH_LEVEL_BINS ? SPEECH_LEVEL_BINS - 1 : bin;
}

void speech_detector_init(speech_detector_t *d, int sample_rate, int block)
{
    float block_seconds = (float)block / sample_rate;
    memset(d, 0, sizeof(*d));
    d->history_length = (int)lroundf(HISTORY_SECONDS / block_seconds);
    if (d->history_length > SPEECH_HISTORY_MAX) d->history_length = SPEECH_HISTORY_MAX;
    if (d->history_length < 1) d->history_length = 1;
    d->onset_blocks = (int)lroundf(ONSET_SECONDS / block_seconds);
    if (d->onset_blocks < 1) d->onset_blocks = 1;
    d->hangover_blocks = (int)lroundf(HANGOVER_SECONDS / block_seconds);
    d->level_dbfs = d->floor_dbfs = LOWEST_DBFS;
}

/* Sliding-window histogram: forget the oldest block once the window is full. */
static void remember(speech_detector_t *d, float dbfs)
{
    int bin = level_bin(dbfs);
    if (d->count == d->history_length) {
        d->histogram[d->history[d->head]]--;
    } else {
        d->count++;
    }
    d->history[d->head] = (uint8_t)bin;
    d->histogram[bin]++;
    d->head = (d->head + 1) % d->history_length;
}

static float percentile_dbfs(const speech_detector_t *d, float fraction)
{
    int wanted = (int)(fraction * d->count) + 1, seen = 0;
    for (int bin = 0; bin < SPEECH_LEVEL_BINS; bin++) {
        seen += d->histogram[bin];
        if (seen >= wanted) return LOWEST_DBFS + (bin + 0.5f) * BIN_DB;
    }
    return 0.0f;
}

int speech_detector_process(speech_detector_t *d, const float *samples, int count, float voice_probability,
                            int gap)
{
    double energy = 0.0;
    for (int i = 0; i < count; i++) energy += (double)samples[i] * samples[i];
    float mean_square = count > 0 ? (float)(energy / count) : 0.0f;
    d->level_dbfs = 10.0f * log10f(mean_square / (32768.0f * 32768.0f) + 1e-14f);

    if (gap || d->level_dbfs < SILENCE_DBFS) {   /* nothing was heard: end speech, keep the floor */
        d->loud_run = 0;
        d->hangover = 0;
        d->speech = 0;
        return 0;
    }
    remember(d, d->level_dbfs);
    d->floor_dbfs = percentile_dbfs(d, FLOOR_PERCENTILE);

    int denoised = voice_probability >= 0.0f;
    float over_floor = d->level_dbfs - d->floor_dbfs;
    int starts = over_floor >= (denoised ? DENOISED_START_DB : START_DB) &&
                 d->level_dbfs >= MIN_SPEECH_DBFS && (!denoised || voice_probability > VOICE_START);
    int holds = over_floor >= (denoised ? DENOISED_HOLD_DB : HOLD_DB) &&
                (!denoised || voice_probability > VOICE_HOLD);

    d->loud_run = starts ? d->loud_run + 1 : 0;
    if (d->loud_run >= d->onset_blocks || (d->speech && holds)) {
        d->speech = 1;
        d->hangover = d->hangover_blocks;
    } else if (d->speech && --d->hangover <= 0) {
        d->speech = 0;
    }
    return d->speech;
}
