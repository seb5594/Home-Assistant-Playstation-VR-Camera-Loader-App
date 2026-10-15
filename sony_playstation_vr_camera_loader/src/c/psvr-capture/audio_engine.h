/*
 * Frame-to-PCM path: OV580 rows -> clock-corrected 16 kHz -> voice pipeline
 * -> non-blocking stdout (PulseAudio via pacat).
 */
#ifndef PSVR_AUDIO_ENGINE_H
#define PSVR_AUDIO_ENGINE_H

#include <stdint.h>
#include <stdio.h>

#include "decimator.h"
#include "frame_source.h"
#include "ov580_audio.h"
#include "pipeline.h"
#include "rate_tracker.h"
#include "resampler.h"

#define OUTPUT_RING_BYTES (PIPELINE_RATE * 2)   /* one second of PCM */
#define COMPARE_SECONDS 4

typedef struct {
    uint8_t data[OUTPUT_RING_BYTES];
    size_t head;
    size_t count;
    long dropped_bytes;
} output_ring_t;

typedef struct {
    int active;
    size_t length;
    int16_t raw[PIPELINE_RATE * COMPARE_SECONDS];
    int16_t processed[PIPELINE_RATE * COMPARE_SECONDS];
} compare_recorder_t;

typedef struct {
    int output_fd;
    double fps;
    ov580_audio_t parser;
    rate_tracker_t clock;
    int rate_reset_pending;      /* frames until the clock estimate restarts after the probe */
    decimator_t decimator;       /* ~194 kHz capsule samples -> ~48 kHz before resampling */
    float *decimated;
    resampler_t resampler;
    pipeline_t pipeline;
    pipeline_config_t config;
    float *extracted;
    size_t extracted_capacity;
    float block[PIPELINE_BLOCK * PIPELINE_CHANNELS];
    int block_fill;
    output_ring_t output;
    compare_recorder_t compare;
    uint32_t last_sequence;
    int have_sequence;
    double samples_per_frame;
    long frames;
    long dropped_frames;
    long damaged_frames;
    int broken_pipe;
} audio_engine_t;

int audio_engine_init(audio_engine_t *ae, const pipeline_config_t *config,
                      int width, int height, double fps, int output_fd);
void audio_engine_free(audio_engine_t *ae);
void audio_engine_frame(audio_engine_t *ae, const frame_t *frame);
/* Blocks until queued PCM is written (offline runs only). */
void audio_engine_finish(audio_engine_t *ae);
/* Starts a before/after recording; the files appear in run_dir when done. */
void audio_engine_start_compare(audio_engine_t *ae);
/* Returns 1 once when a finished comparison should be written. */
int audio_engine_compare_ready(audio_engine_t *ae);
/* Appends the "audio" JSON object to out; returns characters written. */
int audio_engine_status(audio_engine_t *ae, char *out, size_t size);

#endif
