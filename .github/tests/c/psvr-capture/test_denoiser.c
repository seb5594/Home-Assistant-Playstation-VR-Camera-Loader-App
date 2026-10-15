/* RNNoise wrapper: latency-aligned blend and the 16 <-> 48 kHz converters. */
#include <string.h>

#include "denoiser.h"
#include "filters.h"
#include "test_util.h"

enum { RATE = 16000, N = RATE * 4 };

/* Voiced, speech-like signal; the gliding pitch avoids periodic correlation peaks. */
static void make_voice(float *x, int n)
{
    double phase = 0;
    for (int i = 0; i < n; i++) {
        double t = (double)i / RATE, v = 0, formant = 700 + 400 * sin(2 * M_PI * 1.5 * t);
        double f0 = 120 + 60 * sin(2 * M_PI * 0.9 * t) + 20 * sin(2 * M_PI * 2.3 * t);
        phase += 2 * M_PI * f0 / RATE;
        for (int h = 1; h < 40 && f0 * h < 7000; h++)
            v += exp(-pow((f0 * h - formant) / 500.0, 2)) * sin(h * phase);
        x[i] = (float)(6000 * v * (0.6 + 0.4 * sin(2 * M_PI * 3 * t)));
    }
}

static void run(int strength, const float *in, float *out)
{
    static denoiser_t dn;
    CHECK(denoiser_init(&dn, strength) == 0, "init");
    for (int start = 0; start + 256 <= N; start += 256) denoiser_process(&dn, in + start, out + start, 256);
    denoiser_free(&dn);
}

static int best_lag(const float *a, const float *b)
{
    double best = -1e300;
    int lag_best = 0;
    for (int lag = -600; lag <= 600; lag++) {
        double acc = 0;
        for (int i = RATE; i < N - RATE; i++) acc += (double)a[i] * b[i + lag];
        if (acc > best) best = acc, lag_best = lag;
    }
    return lag_best;
}

static void test_wet_and_dry_paths_are_time_aligned(void)
{
    static float voice[N], dry[N], wet[N];
    make_voice(voice, N);
    run(0, voice, dry);
    run(100, voice, wet);
    int lag = best_lag(dry, wet);
    CHECK(lag == 0, "RNNoise output lags the dry path by %d samples at 16 kHz", lag);
}

static void test_dry_path_is_clean_delay(void)
{
    static float voice[N], dry[N];
    make_voice(voice, N);
    run(0, voice, dry);
    int lag = best_lag(voice, dry);
    double error = 0, total = 0;
    for (int i = RATE; i < N - RATE; i++) {
        double d = dry[i + lag] - voice[i];
        error += d * d;
        total += (double)voice[i] * voice[i];
    }
    printf("    dry path latency %d samples (%.1f ms), error %.1f dB\n", lag, lag / 16.0,
           10 * log10(error / total));
    CHECK(lag > 480 && lag < 540, "latency %d (chunk + 20 ms RNNoise + FIR)", lag);
    CHECK(10 * log10(error / total) < -40, "16->48->16 kHz error %.1f dB", 10 * log10(error / total));
}

int main(void)
{
    printf("denoiser\n");
    RUN(test_dry_path_is_clean_delay);
    RUN(test_wet_and_dry_paths_are_time_aligned);
    return test_failures ? 1 : 0;
}
