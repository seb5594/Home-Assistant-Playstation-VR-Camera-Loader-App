#include "audio_engine.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"

static int16_t to_pcm(float v)
{
    long s = lroundf(v);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

static double dbfs(double energy, long samples)
{
    if (samples <= 0 || energy <= 0) return -120.0;
    double rms = sqrt(energy / samples);
    return 20.0 * log10(rms / 32768.0);
}

int audio_engine_init(audio_engine_t *ae, const pipeline_config_t *config,
                      int width, int height, double fps, int output_fd)
{
    memset(ae, 0, sizeof(*ae));
    ae->output_fd = output_fd;
    ae->fps = fps;
    ae->config = *config;
    if (ov580_init(&ae->parser, width, height, fps)) return -1;
    double nominal = (double)ae->parser.expected_per_row * ae->parser.image_rows * fps;
    rate_tracker_reset(&ae->clock, nominal);
    if (resampler_init(&ae->resampler, PIPELINE_CHANNELS, nominal, PIPELINE_RATE)) return -1;
    if (pipeline_init(&ae->pipeline, config)) return -1;
    ae->extracted_capacity = ov580_max_samples_per_frame(&ae->parser);
    ae->extracted = malloc(ae->extracted_capacity * PIPELINE_CHANNELS * sizeof(float));
    ae->decimated = malloc((ae->extracted_capacity + 1) * PIPELINE_CHANNELS * sizeof(float));
    if (!ae->extracted || !ae->decimated || decimator_init(&ae->decimator, PIPELINE_CHANNELS, 1)) return -1;
    fcntl(output_fd, F_SETFL, fcntl(output_fd, F_GETFL) | O_NONBLOCK);
    return 0;
}

void audio_engine_free(audio_engine_t *ae)
{
    resampler_free(&ae->resampler);
    decimator_free(&ae->decimator);
    pipeline_free(&ae->pipeline);
    free(ae->extracted);
    free(ae->decimated);
    ae->extracted = ae->decimated = NULL;
}

/* ---- output ring: never block the capture loop on PulseAudio ------------ */

static void ring_push(output_ring_t *ring, const uint8_t *bytes, size_t size)
{
    if (size > OUTPUT_RING_BYTES) {
        bytes += size - OUTPUT_RING_BYTES;
        size = OUTPUT_RING_BYTES;
    }
    if (ring->count + size > OUTPUT_RING_BYTES) {
        size_t drop = ring->count + size - OUTPUT_RING_BYTES;
        drop += drop & 1;  /* stay sample aligned */
        ring->head = (ring->head + drop) % OUTPUT_RING_BYTES;
        ring->count -= drop;
        ring->dropped_bytes += (long)drop;
    }
    for (size_t i = 0; i < size; i++)
        ring->data[(ring->head + ring->count + i) % OUTPUT_RING_BYTES] = bytes[i];
    ring->count += size;
}

static void flush_output(audio_engine_t *ae)
{
    output_ring_t *ring = &ae->output;
    while (ring->count > 0) {
        size_t contiguous = OUTPUT_RING_BYTES - ring->head;
        if (contiguous > ring->count) contiguous = ring->count;
        ssize_t written = write(ae->output_fd, ring->data + ring->head, contiguous);
        if (written < 0) {
            if (errno == EINTR) continue;
            if (errno == EPIPE) ae->broken_pipe = 1;
            return;
        }
        ring->head = (ring->head + (size_t)written) % OUTPUT_RING_BYTES;
        ring->count -= (size_t)written;
    }
}

/* ---- processing --------------------------------------------------------- */

static void process_block(audio_engine_t *ae)
{
    float out[PIPELINE_BLOCK], raw[PIPELINE_BLOCK];
    int16_t pcm[PIPELINE_BLOCK];

    pipeline_process(&ae->pipeline, ae->block, out, raw);
    for (int i = 0; i < PIPELINE_BLOCK; i++) pcm[i] = to_pcm(out[i]);
    ring_push(&ae->output, (const uint8_t *)pcm, sizeof(pcm));

    compare_recorder_t *cmp = &ae->compare;
    if (cmp->active) {
        size_t room = PIPELINE_RATE * COMPARE_SECONDS - cmp->length;
        size_t take = room < PIPELINE_BLOCK ? room : PIPELINE_BLOCK;
        for (size_t i = 0; i < take; i++) {
            cmp->raw[cmp->length + i] = to_pcm(raw[i]);
            cmp->processed[cmp->length + i] = pcm[i];
        }
        cmp->length += take;
    }
}

static void drain_resampler(audio_engine_t *ae)
{
    for (;;) {
        size_t want = PIPELINE_BLOCK - (size_t)ae->block_fill;
        size_t got = resampler_pull(&ae->resampler, ae->block + ae->block_fill * PIPELINE_CHANNELS, want);
        ae->block_fill += (int)got;
        if (ae->block_fill < PIPELINE_BLOCK) break;
        process_block(ae);
        ae->block_fill = 0;
    }
}

/* Capsule samples at the camera rate -> decimator -> resampler. */
static void push_samples(audio_engine_t *ae, const float *frames, size_t count)
{
    size_t n = decimator_process(&ae->decimator, frames, count, ae->decimated);
    resampler_push(&ae->resampler, ae->decimated, n);
}

/* Follow the measured camera rate; the decimation factor changes only with the layout. */
static void follow_clock(audio_engine_t *ae)
{
    int factor = decimator_factor_for(ae->clock.rate);
    if (factor != ae->decimator.factor) {
        decimator_free(&ae->decimator);
        if (decimator_init(&ae->decimator, PIPELINE_CHANNELS, factor)) decimator_init(&ae->decimator, PIPELINE_CHANNELS, 1);
        else log_info("Audio: %.0f Hz from the camera, decimated by %d before resampling", ae->clock.rate, factor);
    }
    resampler_set_input_rate(&ae->resampler, ae->clock.rate / ae->decimator.factor);
}

static void conceal_gap(audio_engine_t *ae, uint32_t frames)
{
    double per_frame = ae->samples_per_frame > 0 ? ae->samples_per_frame : ae->clock.rate / ae->fps;
    size_t remaining = (size_t)(frames * per_frame);
    size_t limit = (size_t)ae->clock.rate;   /* never invent more than a second */

    if (remaining > limit) remaining = limit;
    rate_tracker_add_gap(&ae->clock, (double)remaining);
    memset(ae->extracted, 0, ae->extracted_capacity * PIPELINE_CHANNELS * sizeof(float));
    while (remaining > 0) {
        size_t chunk = remaining < ae->extracted_capacity ? remaining : ae->extracted_capacity;
        push_samples(ae, ae->extracted, chunk);
        drain_resampler(ae);
        remaining -= chunk;
    }
    ov580_mark_discontinuity(&ae->parser);
    ae->dropped_frames += frames;
}

void audio_engine_frame(audio_engine_t *ae, const frame_t *frame)
{
    ae->frames++;
    if (frame->damaged) ae->damaged_frames++;
    if (ae->have_sequence) {
        uint32_t gap = frame->sequence - (ae->last_sequence + 1);
        if (gap > 0 && gap < 1000) conceal_gap(ae, gap);
    }
    ae->last_sequence = frame->sequence;
    ae->have_sequence = 1;

    int was_probing = ae->parser.probing;
    size_t count = ov580_extract(&ae->parser, frame->data, frame->bytes_used,
                                 ae->extracted, ae->extracted_capacity);
    /*
     * The frame that ends the probe is still parsed with the provisional
     * layout, and the next one starts against a row the probe skipped. The
     * second frame after the probe gives the exact starting rate.
     */
    if (ae->rate_reset_pending > 0 && count > 0 && --ae->rate_reset_pending == 0)
        rate_tracker_reset(&ae->clock, count * ae->fps);
    if (was_probing && !ae->parser.probing) ae->rate_reset_pending = 2;
    rate_tracker_add(&ae->clock, frame->timestamp, (double)count);
    ae->samples_per_frame = ae->samples_per_frame > 0 ? 0.95 * ae->samples_per_frame + 0.05 * count
                                                      : (double)count;
    follow_clock(ae);
    push_samples(ae, ae->extracted, count);
    drain_resampler(ae);
    flush_output(ae);
}

void audio_engine_finish(audio_engine_t *ae)
{
    fcntl(ae->output_fd, F_SETFL, fcntl(ae->output_fd, F_GETFL) & ~O_NONBLOCK);
    flush_output(ae);
}

void audio_engine_start_compare(audio_engine_t *ae)
{
    ae->compare.length = 0;
    ae->compare.active = 1;
}

int audio_engine_compare_ready(audio_engine_t *ae)
{
    if (!ae->compare.active || ae->compare.length < PIPELINE_RATE * COMPARE_SECONDS) return 0;
    ae->compare.active = 0;
    return 1;
}

int audio_engine_status(audio_engine_t *ae, char *out, size_t size)
{
    pipeline_t *p = &ae->pipeline;
    pipeline_meter_t m = pipeline_take_meter(p);
    long samples = m.samples > 0 ? m.samples : 1;
    char voice[24] = "null", echo[256] = "{\"enabled\":false}";

    if (p->denoise_enabled) snprintf(voice, sizeof(voice), "%.2f", p->denoiser.voice_probability);
    if (p->echo_enabled)
        snprintf(echo, sizeof(echo),
                 "{\"enabled\":true,\"connected\":%s,\"playing\":%s,\"erle_db\":%.1f,"
                 "\"delay_ms\":%d,\"converged\":%s,\"underruns\":%ld}",
                 p->reference.connected ? "true" : "false",
                 p->echo.reference_active ? "true" : "false", p->echo.erle_db,
                 p->reference.delay * 1000 / PIPELINE_RATE, p->echo.adapted ? "true" : "false",
                 p->reference.underruns);

    return snprintf(out, size,
        "{\"layout\":\"%s\",\"input_rate\":%.1f,"
        "\"channel_dbfs\":[%.1f,%.1f,%.1f,%.1f],\"active_mics\":%d,"
        "\"peak_dbfs\":%.1f,\"clipping_percent\":%.3f,\"output_dbfs\":%.1f,"
        "\"speech\":%s,\"voice_probability\":%s,\"noise_floor_dbfs\":%.1f,"
        "\"beamformer\":\"%s\",\"suppression_db\":%.1f,\"notched_percent\":%.0f,"
        "\"focus\":{\"presence\":%.2f,\"focused_percent\":%.0f},"
        "\"noise_suppression\":\"%s\",\"noise_strength\":%d,"
        "\"auto_gain\":%s,\"gain_db\":%.1f,\"echo\":%s,"
        "\"frames\":%ld,\"dropped_frames\":%ld,\"damaged_frames\":%ld,"
        "\"rows_skipped\":%llu,\"output_dropped_ms\":%ld}",
        ae->parser.summary, ae->clock.rate,
        dbfs(m.channel_energy[0], samples), dbfs(m.channel_energy[1], samples),
        dbfs(m.channel_energy[2], samples), dbfs(m.channel_energy[3], samples),
        mic_calibration_active_channels(&p->calibration),
        m.peak > 0 ? 20.0 * log10(m.peak / 32768.0) : -120.0,
        100.0 * m.clipped / (samples * PIPELINE_CHANNELS), dbfs(m.output_energy, samples),
        m.blocks && m.speech_blocks * 4 >= m.blocks ? "true" : "false", voice,
        level_control_noise_floor_dbfs(&p->level),
        beamformer_mode_name(ae->config.beam_mode),
        ae->config.beam_mode != BEAM_SUM ? p->beamformer.suppression_db : 0.0f,
        100.0f * p->beamformer.notched_fraction,
        p->beamformer.focus.presence, 100.0f * p->beamformer.focused_fraction,
        p->denoise_enabled ? "rnnoise" : "off", p->denoise_enabled ? ae->config.noise_strength : 0,
        ae->config.auto_gain ? "true" : "false", level_control_gain_db(&p->level), echo,
        ae->frames, ae->dropped_frames, ae->damaged_frames,
        (unsigned long long)ae->parser.rows_skipped,
        ae->output.dropped_bytes * 1000 / (PIPELINE_RATE * 2));
}
