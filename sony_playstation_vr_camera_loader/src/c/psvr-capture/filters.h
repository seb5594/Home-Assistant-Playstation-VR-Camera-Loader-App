/* Small filter building blocks shared by the audio stages. */
#ifndef PSVR_FILTERS_H
#define PSVR_FILTERS_H

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} biquad_t;

void biquad_highpass(biquad_t *f, double corner_hz, double sample_rate);
void biquad_process(biquad_t *f, float *samples, int count, int stride);

/*
 * Integer-factor rate converter (polyphase FIR) for the 16 kHz <-> 48 kHz hop
 * around RNNoise. The low-pass sits just below the 16 kHz Nyquist frequency.
 */
#define RATE3_TAPS_PER_PHASE 24
#define RATE3_FACTOR 3

typedef struct {
    float taps[RATE3_FACTOR * RATE3_TAPS_PER_PHASE];
    /* Mirrored ring buffer: a contiguous window always starts at position. */
    float history[2 * RATE3_FACTOR * RATE3_TAPS_PER_PHASE];
    int position;
    int phase;
} rate3_t;

void rate3_init(rate3_t *r);
/* Writes count * 3 samples. */
void rate3_upsample(rate3_t *r, const float *in, int count, float *out);
/* count must be a multiple of 3; writes count / 3 samples. */
void rate3_downsample(rate3_t *r, const float *in, int count, float *out);

#endif
