/*
 * Band-limited resampler for interleaved multichannel float audio.
 *
 * The input rate may be changed while running. The capture loop uses this to
 * follow the camera's crystal (which runs a little fast or slow compared to
 * the host clock) so PulseAudio never sees a growing or draining buffer.
 */
#ifndef PSVR_RESAMPLER_H
#define PSVR_RESAMPLER_H

#include <stddef.h>

typedef struct {
    int channels;
    double in_rate;
    double out_rate;
    double step;        /* input samples consumed per output sample */
    double cutoff;      /* low-pass corner as a fraction of the input Nyquist */
    double half_width;  /* kernel reach in input samples */
    double position;    /* next output time, in input samples from history[0] */
    float *history;
    size_t length;      /* frames currently stored */
    size_t capacity;    /* frames allocated */
    float *table;       /* windowed sinc, sampled over its zero crossings */
} resampler_t;

int resampler_init(resampler_t *rs, int channels, double in_rate, double out_rate);
void resampler_free(resampler_t *rs);
void resampler_set_input_rate(resampler_t *rs, double in_rate);
int resampler_push(resampler_t *rs, const float *frames, size_t count);
/* Returns the number of frames written, at most max_frames. */
size_t resampler_pull(resampler_t *rs, float *out, size_t max_frames);

#endif
