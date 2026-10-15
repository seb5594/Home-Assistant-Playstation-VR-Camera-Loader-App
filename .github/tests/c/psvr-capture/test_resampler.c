/* Band-limited resampling quality and camera clock tracking. */
#include <string.h>

#include "rate_tracker.h"
#include "resampler.h"
#include "test_util.h"

static size_t run_tone(double in_rate, double tone, double amplitude, int seconds, float *out, size_t out_cap)
{
    resampler_t rs;
    float chunk[800 * 4];
    size_t produced = 0;
    long t = 0;

    resampler_init(&rs, 4, in_rate, 16000);
    for (int block = 0; block < seconds * in_rate / 800; block++) {
        for (int i = 0; i < 800; i++, t++)
            for (int c = 0; c < 4; c++)
                chunk[i * 4 + c] = (float)(amplitude * sin(2 * M_PI * tone * t / in_rate));
        resampler_push(&rs, chunk, 800);
        float frame[4 * 256];
        size_t got;
        while ((got = resampler_pull(&rs, frame, 256)) > 0) {
            for (size_t i = 0; i < got && produced < out_cap; i++) out[produced++] = frame[i * 4 + 1];
        }
    }
    resampler_free(&rs);
    return produced;
}

static void test_speech_band_tone_is_clean(void)
{
    static float out[16000 * 3];
    size_t n = run_tone(48000, 1000, 10000, 3, out, 16000 * 3);
    CHECK(n > 47000 && n <= 48000, "3 s at 16 kHz, got %zu", n);
    double residual = tone_residual_db(out + 8000, 32000, 1000, 16000);
    CHECK(residual < -60, "1 kHz residual %.1f dB", residual);
    double energy = 0;
    for (int i = 8000; i < 40000; i++) energy += out[i] * out[i];
    double amplitude = sqrt(2 * energy / 32000);
    CHECK(fabs(amplitude - 10000) < 100, "amplitude %.0f", amplitude);
}

static void test_alias_above_8khz_is_rejected(void)
{
    static float out[16000 * 2];
    size_t n = run_tone(48000, 12000, 10000, 2, out, 16000 * 2);
    double energy = 0;
    for (size_t i = 4000; i < n; i++) energy += out[i] * out[i];
    double level = 10 * log10(energy / (n - 4000) / (10000.0 * 10000 / 2) + 1e-20);
    CHECK(level < -60, "12 kHz leaks at %.1f dB", level);
}

static void test_24khz_row_clock_also_works(void)
{
    static float out[16000 * 3];
    run_tone(24000, 3000, 8000, 3, out, 16000 * 3);
    double residual = tone_residual_db(out + 8000, 32000, 3000, 16000);
    CHECK(residual < -55, "3 kHz from 24 kHz residual %.1f dB", residual);
}

static void test_drift_correction_keeps_output_on_host_clock(void)
{
    /* Camera runs 0.05 % fast: 48024 samples per host second. */
    resampler_t rs;
    float chunk[1601 * 4] = {0}, frame[4 * 512];
    size_t produced = 0;
    resampler_init(&rs, 4, 48000, 16000);
    resampler_set_input_rate(&rs, 48024);
    for (int second = 0; second < 60; second++) {
        for (int f = 0; f < 30; f++) {
            int n = 1600 + (f < 24 ? 1 : 0);   /* 48024 per second */
            resampler_push(&rs, chunk, (size_t)n);
            size_t got;
            while ((got = resampler_pull(&rs, frame, 512)) > 0) produced += got;
        }
    }
    resampler_free(&rs);
    CHECK(llabs((long long)produced - 960000) < 64, "60 s produced %zu samples (want 960000)", produced);
}

static void test_rate_tracker_measures_camera_crystal(void)
{
    rate_tracker_t rt;
    rate_tracker_reset(&rt, 48000);
    double fps = 30.0145, t = 100.0;
    for (int f = 0; f < 30 * 40; f++) {
        rate_tracker_add(&rt, t + (uniform() - 0.5) * 0.002, 1600);
        t += 1.0 / fps;
    }
    CHECK(fabs(rt.rate - 1600 * fps) < 2.0, "measured %.2f, want %.2f", rt.rate, 1600 * fps);
}

static void test_rate_tracker_follows_layout_change(void)
{
    rate_tracker_t rt;
    rate_tracker_reset(&rt, 48000);
    double t = 0;
    for (int f = 0; f < 30 * 10; f++, t += 1.0 / 30) rate_tracker_add(&rt, t, 800);
    CHECK(fabs(rt.rate - 24000) < 20, "re-centred on 24 kHz, got %.1f", rt.rate);
}

int main(void)
{
    printf("resampler\n");
    RUN(test_speech_band_tone_is_clean);
    RUN(test_alias_above_8khz_is_rejected);
    RUN(test_24khz_row_clock_also_works);
    RUN(test_drift_correction_keeps_output_on_host_clock);
    RUN(test_rate_tracker_measures_camera_crystal);
    RUN(test_rate_tracker_follows_layout_change);
    return test_failures ? 1 : 0;
}
