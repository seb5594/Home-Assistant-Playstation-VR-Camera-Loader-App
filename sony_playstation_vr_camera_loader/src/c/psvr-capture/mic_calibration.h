/*
 * Keeps the four capsules at the same loudness.
 *
 * Capsule sensitivity differs from unit to unit, and the AK5703 has separate
 * volume registers per channel. Matching long-term levels makes the plain
 * average a real array sum and gives the beamformer a valid broadside model.
 * A capsule that stays far below the others is treated as dead and muted.
 */
#ifndef PSVR_MIC_CALIBRATION_H
#define PSVR_MIC_CALIBRATION_H

#define CALIBRATION_CHANNELS 4

typedef struct {
    double power[CALIBRATION_CHANNELS];
    float gain[CALIBRATION_CHANNELS];
    int active[CALIBRATION_CHANNELS];
    double smoothing;
    int warm;
} mic_calibration_t;

void mic_calibration_init(mic_calibration_t *cal, int sample_rate, int block);
/* frames is interleaved; gains are applied in place. */
void mic_calibration_process(mic_calibration_t *cal, float *frames, int count);
int mic_calibration_active_channels(const mic_calibration_t *cal);

#endif
