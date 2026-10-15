#include "pipeline.h"

#include <math.h>
#include <string.h>

#include "log.h"

static const double HIGHPASS_HZ = 100.0;   /* rumble, mains hum, footsteps */
static const int ECHO_FILTER_MS = 256;
static const float CLIP_LEVEL = 32000.0f;
static const int GAP_FRAMES = PIPELINE_BLOCK / 8;   /* 2 ms of exact zeros on every capsule */
/* Blocks a gap stays visible in the output: beamformer overlap plus RNNoise delay. */
static const unsigned GAP_REACH_MASK = 0xF;

int pipeline_init(pipeline_t *p, const pipeline_config_t *config)
{
    memset(p, 0, sizeof(*p));
    p->config = *config;
    for (int c = 0; c < PIPELINE_CHANNELS; c++) biquad_highpass(&p->highpass[c], HIGHPASS_HZ, PIPELINE_RATE);
    mic_calibration_init(&p->calibration, PIPELINE_RATE, PIPELINE_BLOCK);
    if (beamformer_init(&p->beamformer, config->beam_mode, PIPELINE_RATE)) return -1;
    if (config->reference_fd >= 0) {
        if (echo_canceller_init(&p->echo, PIPELINE_RATE, ECHO_FILTER_MS)) return -1;
        echo_reference_init(&p->reference, config->reference_fd);
        p->echo_enabled = 1;
    }
    if (config->noise_suppression) {
        if (denoiser_init(&p->denoiser, config->noise_strength)) {
            log_error("RNNoise could not be initialized; continuing without noise reduction");
        } else {
            p->denoise_enabled = 1;
        }
    }
    level_control_init(&p->level, config->auto_gain, PIPELINE_RATE, PIPELINE_BLOCK);
    return 0;
}

void pipeline_free(pipeline_t *p)
{
    beamformer_free(&p->beamformer);
    if (p->echo_enabled) echo_canceller_free(&p->echo);
    if (p->denoise_enabled) denoiser_free(&p->denoiser);
}

/* Peaks and clipping of the raw capsules: that is where the ADC runs out of range. */
static void measure_peaks(pipeline_meter_t *m, const float *in)
{
    for (int i = 0; i < PIPELINE_BLOCK * PIPELINE_CHANNELS; i++) {
        float a = fabsf(in[i]);
        if (a > m->peak) m->peak = a;
        if (a >= CLIP_LEVEL) m->clipped++;
    }
    m->samples += PIPELINE_BLOCK;
}

/*
 * Lost camera frames are replaced by exact zeros. Real capsules never deliver
 * an exact zero on all four channels at once, so a run of them marks a gap.
 */
static int contains_gap(const float *in)
{
    int zero_frames = 0;
    for (int i = 0; i < PIPELINE_BLOCK; i++) {
        const float *frame = in + i * PIPELINE_CHANNELS;
        zero_frames += frame[0] == 0.0f && frame[1] == 0.0f && frame[2] == 0.0f && frame[3] == 0.0f;
    }
    return zero_frames >= GAP_FRAMES;
}

/* Capsule levels after the high-pass: DC offset and rumble would read as sound. */
static void measure_levels(pipeline_meter_t *m, const float *frames)
{
    for (int i = 0; i < PIPELINE_BLOCK; i++)
        for (int c = 0; c < PIPELINE_CHANNELS; c++) {
            float v = frames[i * PIPELINE_CHANNELS + c];
            m->channel_energy[c] += (double)v * v;
        }
}

void pipeline_process(pipeline_t *p, const float *in, float *out, float *raw)
{
    float frames[PIPELINE_BLOCK * PIPELINE_CHANNELS], voice[PIPELINE_BLOCK];
    float voice_probability = -1.0f;
    p->recent_gaps = ((p->recent_gaps << 1) | (unsigned)contains_gap(in)) & GAP_REACH_MASK;

    measure_peaks(&p->meter, in);
    for (int i = 0; i < PIPELINE_BLOCK; i++) {
        float sum = 0.0f;
        for (int c = 0; c < PIPELINE_CHANNELS; c++) sum += in[i * PIPELINE_CHANNELS + c];
        raw[i] = sum / PIPELINE_CHANNELS;
    }

    memcpy(frames, in, sizeof(frames));
    for (int c = 0; c < PIPELINE_CHANNELS; c++)
        biquad_process(&p->highpass[c], frames + c, PIPELINE_BLOCK, PIPELINE_CHANNELS);
    measure_levels(&p->meter, frames);
    mic_calibration_process(&p->calibration, frames, PIPELINE_BLOCK);
    beamformer_process(&p->beamformer, frames, voice);

    if (p->echo_enabled) {
        float reference[PIPELINE_BLOCK];
        echo_reference_poll(&p->reference);
        echo_reference_block(&p->reference, reference);
        if (echo_reference_track(&p->reference, voice)) echo_canceller_reset(&p->echo);
        echo_canceller_process(&p->echo, voice, reference, voice);
    }
    if (p->denoise_enabled) {
        denoiser_process(&p->denoiser, voice, voice, PIPELINE_BLOCK);
        voice_probability = p->denoiser.voice_probability;
    }
    level_control_process(&p->level, voice, PIPELINE_BLOCK, voice_probability, p->recent_gaps != 0);

    for (int i = 0; i < PIPELINE_BLOCK; i++) {
        out[i] = voice[i];
        p->meter.output_energy += (double)voice[i] * voice[i];
    }
    p->meter.speech_blocks += p->level.speech;
    p->meter.blocks++;
}

pipeline_meter_t pipeline_take_meter(pipeline_t *p)
{
    pipeline_meter_t taken = p->meter;
    memset(&p->meter, 0, sizeof(p->meter));
    return taken;
}
