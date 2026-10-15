/*
 * Combines the four capsules into one voice channel.
 *
 * "sum" is the classic array average: no latency, a few dB against room
 * noise.
 *
 * "adaptive" learns where *persistent* sound comes from (a TV, a speaker
 * playing music) and steers a spatial notch at it, per frequency:
 *
 *   1. Every 16 ms frame updates a slow spatial covariance per bin. Onsets
 *      that rise well above the slow background (someone starts talking) are
 *      not learned, so a short command never becomes "the noise".
 *   2. The strongest eigenvectors of that covariance describe the persistent
 *      sources. When one dominates (high coherence), it is projected out.
 *   3. A broadside guard caps the notch so a talker in front of the camera
 *      keeps at least half of their energy even if the TV sits in line.
 *
 * "focus" (experimental) goes one step further and also learns where the
 * *talker* is, from the moment a voice rises above the persistent sound -
 * usually the wake word:
 *
 *   4. Energy that the persistent sources cannot explain (what is left after
 *      projecting out their directions) marks speech. A louder TV scene, or
 *      dialogue from the TV, stays in the TV's direction and does not count.
 *   5. While someone speaks, a second covariance collects the sound field.
 *      Its strongest direction relative to the background (generalised
 *      eigenvector) is the talker; an MVDR beam keeps that direction intact
 *      and minimises everything the background covariance describes.
 *   6. The talker direction is kept after the command, so the next wake word
 *      already starts focused. Until a talker was heard, and wherever the
 *      estimate is weak, the adaptive notch is used.
 *
 * No array geometry is needed; the covariances carry it implicitly.
 */
#ifndef PSVR_BEAMFORMER_H
#define PSVR_BEAMFORMER_H

#include "fft.h"

#define BEAM_CHANNELS 4
#define BEAM_FFT 512
#define BEAM_HOP 256
#define BEAM_BINS (BEAM_FFT / 2 + 1)
#define BEAM_MATRIX (BEAM_CHANNELS * BEAM_CHANNELS)

typedef enum { BEAM_SUM, BEAM_ADAPTIVE, BEAM_FOCUS } beam_mode_t;

/* Talker tracking used by BEAM_FOCUS. */
typedef struct {
    cfloat speech[BEAM_BINS][BEAM_MATRIX];   /* sound field while someone speaks */
    cfloat talker[BEAM_BINS][BEAM_CHANNELS]; /* generalised eigenvector, warm start */
    float heard[BEAM_BINS];                  /* 0..1: how much speech this bin has seen */
    float confidence[BEAM_BINS];             /* 0..1: weight of the talker beam */
    float presence;                          /* broadband speech presence 0..1 */
    float novelty_db;                        /* unexplained energy over the background */
    float busy_seconds;                      /* continuous presence; long means new background */
    int new_background;                      /* a "voice" that never paused: learn it as background */
    /* What the talker estimate was before the current stretch of presence, so a
     * fan or vacuum that started as "speech" can be undone once it turns out
     * to be persistent. */
    cfloat speech_before[BEAM_BINS][BEAM_MATRIX];
    float heard_before[BEAM_BINS];
    float alpha_speech, release;
} speech_focus_t;

typedef struct {
    beam_mode_t mode;
    int frozen;         /* keep current weights (used to measure components in tests) */
    int sample_rate;
    fft_plan_t fft;
    float window[BEAM_FFT];
    float input[BEAM_CHANNELS][BEAM_FFT];
    float overlap[BEAM_HOP];
    cfloat covariance[BEAM_BINS][BEAM_MATRIX];   /* persistent sound field */
    cfloat eigen[BEAM_BINS][2][BEAM_CHANNELS];
    float eigenvalue[BEAM_BINS][2];
    float dominance[BEAM_BINS][2];   /* 0..1: eigenvector stands out of the diffuse floor */
    float strength[BEAM_BINS][2];    /* applied notch depth, after the broadside guard */
    cfloat notch[BEAM_BINS][BEAM_CHANNELS];
    cfloat weights[BEAM_BINS][BEAM_CHANNELS];
    float power_short[BEAM_BINS];
    float power_slow[BEAM_BINS];
    double band_short, band_slow;   /* broadband onset detector */
    float learn_gate;               /* 0..1 from the broadband detector */
    int bin_low, bin_high;
    int speech_low, speech_high;    /* band used to detect a talker */
    long frames;
    float alpha_short, alpha_slow, alpha_learn, alpha_warmup;
    speech_focus_t focus;
    /* Status for the UI. */
    double energy_sum, energy_out;
    float suppression_db;
    float notched_fraction;
    float focused_fraction;         /* share of bins steered at a learned talker */
} beamformer_t;

int beamformer_init(beamformer_t *bf, beam_mode_t mode, int sample_rate);
void beamformer_free(beamformer_t *bf);
/* in: BEAM_HOP interleaved 4-channel frames; out: BEAM_HOP mono samples. */
void beamformer_process(beamformer_t *bf, const float *in, float *out);
const char *beamformer_mode_name(beam_mode_t mode);
/* "sum", "adaptive" or "focus"; anything else gives sum. */
beam_mode_t beamformer_mode_from_name(const char *name);

#endif
