/*
 * Speech-gated automatic gain plus a soft limiter.
 *
 * Far-field speech reaches the camera 10-20 dB quieter than near-field speech.
 * The gain only rises while speech is present, so background noise is never
 * pumped up between commands, and it holds the last speech gain so the next
 * wake word starts at a sensible level.
 *
 * The gain is also limited by the room's noise: it never lifts the noise floor
 * above NOISE_CEILING_DBFS. In a room that is already loud (or with a high
 * capsule gain) the automatic gain therefore stays at 0 dB instead of handing
 * Assist amplified hiss.
 */
#ifndef PSVR_LEVEL_CONTROL_H
#define PSVR_LEVEL_CONTROL_H

#include "speech_detector.h"

typedef struct {
    int auto_gain;
    float gain;            /* current linear gain */
    float target;          /* desired speech RMS, 16-bit scale */
    float max_gain, min_gain;
    float speech_level;    /* smoothed RMS during speech */
    float rise_per_block;  /* max gain increase per block */
    speech_detector_t detector;
    int speech;            /* last decision, for status */
} level_control_t;

void level_control_init(level_control_t *lc, int auto_gain, int sample_rate, int block);
/*
 * voice_probability < 0 means "unknown" (no RNNoise); the level decides then.
 * gap: the block contains silence standing in for lost camera frames.
 */
void level_control_process(level_control_t *lc, float *samples, int count, float voice_probability, int gap);
float level_control_gain_db(const level_control_t *lc);
/* Noise floor of the signal before the gain, dBFS. */
float level_control_noise_floor_dbfs(const level_control_t *lc);

#endif
