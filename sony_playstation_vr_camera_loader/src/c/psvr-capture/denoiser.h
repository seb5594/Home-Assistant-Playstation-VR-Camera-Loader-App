/*
 * RNNoise speech denoiser wrapped for the 16 kHz voice chain.
 *
 * RNNoise only runs at 48 kHz, so each 10 ms chunk is upsampled, denoised and
 * brought back down. The dry signal used for the strength blend is delayed by
 * RNNoise's own latency (960 samples = 20 ms, measured with v0.2). Mixing it
 * undelayed, as alpha10 did, produced a 50 Hz comb filter whenever the
 * strength was below 100 %.
 */
#ifndef PSVR_DENOISER_H
#define PSVR_DENOISER_H

#include "filters.h"

#define DENOISER_CHUNK 160          /* 10 ms at 16 kHz */
#define DENOISER_CHUNK_48K 480
#define DENOISER_LATENCY_48K 960

typedef struct DenoiseState DenoiseState;

typedef struct {
    DenoiseState *state;
    float strength;                 /* 0..1 share of the denoised signal */
    rate3_t up, down;
    float dry_delay[DENOISER_LATENCY_48K];
    float pending_in[DENOISER_CHUNK];
    int pending_count;
    float pending_out[2 * DENOISER_CHUNK + 512];
    int out_count;
    float voice_probability;
} denoiser_t;

int denoiser_init(denoiser_t *dn, int strength_percent);
void denoiser_free(denoiser_t *dn);
/* Streams any block length; output lags input by one 10 ms chunk. */
void denoiser_process(denoiser_t *dn, const float *in, float *out, int count);

#endif
