/*
 * Decides per 16 ms block whether someone is speaking, for the automatic gain
 * and the "speech detected" indicator.
 *
 * Noise floor: the 10th percentile of the block levels of the last five
 * seconds. Speech always has short pauses, so the floor stays at the room's
 * noise while someone talks, and a louder background (a TV switched on)
 * becomes the new floor within the window. A percentile, unlike a running
 * minimum, cannot be dragged down by a few odd blocks; blocks with the silence
 * that replaces frames lost on USB are left out altogether.
 *
 * Decision, with hysteresis:
 *   starts  3 blocks in a row at least 8 dB above the floor
 *   ends    200 ms below floor + 4 dB
 * With RNNoise, its voice probability must agree and the level thresholds
 * are lower, since RNNoise has already removed most of the noise.
 */
#ifndef PSVR_SPEECH_DETECTOR_H
#define PSVR_SPEECH_DETECTOR_H

#include <stdint.h>

#define SPEECH_HISTORY_MAX 512        /* blocks; 5 s at 16 ms needs 313 */
#define SPEECH_LEVEL_BINS 240         /* 0.5 dB bins from -120 to 0 dBFS */

typedef struct {
    uint8_t history[SPEECH_HISTORY_MAX];   /* level bin of each remembered block */
    uint16_t histogram[SPEECH_LEVEL_BINS];
    int history_length, count, head;
    int onset_blocks, hangover_blocks;
    int loud_run;                          /* consecutive blocks over the start threshold */
    int hangover;                          /* blocks left before speech ends */
    float level_dbfs;                      /* last block */
    float floor_dbfs;
    int speech;
} speech_detector_t;

void speech_detector_init(speech_detector_t *d, int sample_rate, int block);
/*
 * samples: one block on the 16-bit scale. voice_probability: RNNoise's
 * estimate for the block, or a negative value without RNNoise. gap: the block
 * contains silence that stands in for lost camera frames; it neither counts as
 * speech nor moves the floor. Returns 1 while speech is present.
 */
int speech_detector_process(speech_detector_t *d, const float *samples, int count, float voice_probability,
                            int gap);

#endif
