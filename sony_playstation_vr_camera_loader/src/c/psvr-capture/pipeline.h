/*
 * The 16 kHz voice chain, one 16 ms block at a time:
 *
 *   4 capsules -> 100 Hz high-pass -> level matching -> beamformer
 *              -> echo canceller (optional) -> RNNoise (optional)
 *              -> speech-gated gain + limiter -> mono output
 */
#ifndef PSVR_PIPELINE_H
#define PSVR_PIPELINE_H

#include "beamformer.h"
#include "denoiser.h"
#include "echo_canceller.h"
#include "echo_reference.h"
#include "filters.h"
#include "level_control.h"
#include "mic_calibration.h"

#define PIPELINE_RATE 16000
#define PIPELINE_BLOCK 256
#define PIPELINE_CHANNELS 4

typedef struct {
    beam_mode_t beam_mode;
    int noise_suppression;    /* RNNoise on/off */
    int noise_strength;       /* percent */
    int auto_gain;
    int reference_fd;         /* -1 disables echo cancellation */
} pipeline_config_t;

typedef struct {
    double channel_energy[PIPELINE_CHANNELS];
    double output_energy;
    long samples;
    long clipped;
    float peak;
    long speech_blocks;
    long blocks;
} pipeline_meter_t;

typedef struct {
    pipeline_config_t config;
    biquad_t highpass[PIPELINE_CHANNELS];
    mic_calibration_t calibration;
    beamformer_t beamformer;
    int echo_enabled;
    echo_canceller_t echo;
    echo_reference_t reference;
    int denoise_enabled;
    denoiser_t denoiser;
    level_control_t level;
    unsigned recent_gaps;     /* one bit per block, newest lowest: lost frames in the input */
    pipeline_meter_t meter;
} pipeline_t;

int pipeline_init(pipeline_t *p, const pipeline_config_t *config);
void pipeline_free(pipeline_t *p);
/*
 * in: PIPELINE_BLOCK interleaved 4-channel frames (16-bit scale).
 * out: processed mono block. raw: plain capsule average, for A/B listening.
 */
void pipeline_process(pipeline_t *p, const float *in, float *out, float *raw);
/* Returns and clears the level meter accumulated since the last call. */
pipeline_meter_t pipeline_take_meter(pipeline_t *p);

#endif
