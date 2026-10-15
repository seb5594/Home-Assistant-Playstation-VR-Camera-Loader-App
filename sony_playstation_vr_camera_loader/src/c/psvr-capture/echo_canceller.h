/*
 * Acoustic echo canceller for sound the Home Assistant host plays itself
 * (Music Assistant, TTS answers) on a known PulseAudio sink.
 *
 * A partitioned-block frequency-domain adaptive filter learns the path from
 * that sink's monitor to the camera microphones and subtracts the predicted
 * echo. Step size follows the residual-to-error ratio (the approach behind
 * Speex MDF), so speech from the room slows adaptation instead of corrupting
 * the filter. A light spectral stage then removes the leftover echo.
 */
#ifndef PSVR_ECHO_CANCELLER_H
#define PSVR_ECHO_CANCELLER_H

#include "fft.h"

#define AEC_BLOCK 256
#define AEC_FFT (2 * AEC_BLOCK)
#define AEC_BINS (AEC_BLOCK + 1)

typedef struct {
    int partitions;
    int sample_rate;
    fft_plan_t fft;
    cfloat *reference;          /* partitions x AEC_BINS, ring buffer */
    cfloat *filter;             /* partitions x AEC_BINS */
    int newest;
    float reference_previous[AEC_BLOCK];
    float reference_power[AEC_BINS];
    float error_smooth[AEC_BINS];
    float echo_smooth[AEC_BINS];
    double pey, pyy;
    float leak;
    int adapted;
    double adapt_sum;
    long blocks;
    /* residual echo suppressor */
    float window[AEC_FFT];
    float residual_in[AEC_FFT];
    float residual_echo[AEC_FFT];
    float residual_overlap[AEC_BLOCK];
    float residual_gain[AEC_BINS];
    /* status */
    int reference_active;
    double mic_energy, error_energy;
    float erle_db;
} echo_canceller_t;

int echo_canceller_init(echo_canceller_t *aec, int sample_rate, int filter_ms);
void echo_canceller_reset(echo_canceller_t *aec);
void echo_canceller_free(echo_canceller_t *aec);
/* mic, reference and out hold AEC_BLOCK samples in 16-bit scale. */
void echo_canceller_process(echo_canceller_t *aec, const float *mic, const float *reference, float *out);

#endif
