/*
 * Integer-factor FIR decimator for interleaved multichannel float audio.
 *
 * The PS4 camera delivers about 194 kHz per capsule in its 60 fps mode. A
 * short low-pass that keeps only every 4th sample brings that down to about
 * 48 kHz first, so the band-limited resampler behind it needs a quarter of
 * the filter length: the whole conversion to 16 kHz costs roughly a third.
 */
#ifndef PSVR_DECIMATOR_H
#define PSVR_DECIMATOR_H

#include <stddef.h>

#define DECIMATOR_MAX_FACTOR 8
#define DECIMATOR_TAPS_PER_FACTOR 8

typedef struct {
    int channels;
    int factor;           /* 1 passes samples through untouched */
    int taps;
    float coefficients[DECIMATOR_MAX_FACTOR * DECIMATOR_TAPS_PER_FACTOR];
    float *buffer;        /* last taps-1 frames, then the new input */
    size_t capacity;      /* frames */
    size_t length;        /* frames in buffer */
    size_t next;          /* buffer index of the next output's newest frame */
} decimator_t;

/* Highest useful factor for an input rate when 16 kHz comes out in the end. */
int decimator_factor_for(double input_rate);
int decimator_init(decimator_t *d, int channels, int factor);
void decimator_free(decimator_t *d);
/* Returns frames written to out (at most count / factor + 1). */
size_t decimator_process(decimator_t *d, const float *in, size_t count, float *out);

#endif
