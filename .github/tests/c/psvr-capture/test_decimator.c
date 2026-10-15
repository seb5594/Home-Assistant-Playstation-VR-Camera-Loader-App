/* The 194 kHz -> 48 kHz decimation in front of the resampler. */
#include <stdlib.h>
#include <string.h>

#include "decimator.h"
#include "test_util.h"

enum { RATE = 193920, FACTOR = 4, SECONDS = 1 };

/* Level (dB) of a tone at `hz` after decimation, measured at the decimated rate. */
static double tone_gain_db(double hz, double measure_hz)
{
    static float in[RATE * SECONDS * 2], out[RATE * SECONDS * 2 / FACTOR + 4];
    decimator_t d;
    decimator_init(&d, 2, FACTOR);
    for (int i = 0; i < RATE * SECONDS; i++) in[2 * i] = in[2 * i + 1] = (float)(1000 * sin(2 * M_PI * hz * i / RATE));
    size_t n = 0;
    for (int start = 0; start < RATE * SECONDS; start += 3232)   /* one camera frame of samples at a time */
        n += decimator_process(&d, in + 2 * start, 3232, out + 2 * n);
    decimator_free(&d);
    double c = 0, s = 0, rate = (double)RATE / FACTOR;
    size_t skip = 200;
    for (size_t i = skip; i < n; i++) {
        c += out[2 * i] * cos(2 * M_PI * measure_hz * i / rate);
        s += out[2 * i] * sin(2 * M_PI * measure_hz * i / rate);
    }
    double amplitude = 2 * sqrt(c * c + s * s) / (n - skip);
    return 20 * log10(amplitude / 1000 + 1e-12);
}

static void test_speech_band_passes_flat(void)
{
    const double tones[] = {100, 1000, 4000, 7000};   /* the 16 kHz output keeps up to ~7.2 kHz */
    for (int i = 0; i < 4; i++) {
        double g = tone_gain_db(tones[i], tones[i]);
        CHECK(fabs(g) < 0.1, "%.0f Hz changed by %.2f dB", tones[i], g);
    }
}

static void test_what_would_fold_into_speech_is_removed(void)
{
    /* 45 kHz folds to 3.48 kHz, 52 kHz to 3.52 kHz, 92 kHz to 4.96 kHz at 48.48 kHz */
    const double tones[][2] = {{45000, 48480 - 45000}, {52000, 52000 - 48480}, {92000, 96960 - 92000}};
    for (int i = 0; i < 3; i++) {
        double g = tone_gain_db(tones[i][0], tones[i][1]);
        printf("    %.0f Hz -> alias at %.0f Hz: %.0f dB\n", tones[i][0], tones[i][1], g);
        CHECK(g < -60, "%.0f Hz leaks into %.0f Hz at %.1f dB", tones[i][0], tones[i][1], g);
    }
}

static void test_factor_choice(void)
{
    CHECK(decimator_factor_for(193920) == 4, "194 kHz");
    CHECK(decimator_factor_for(48480) == 1, "48 kHz stays");
    CHECK(decimator_factor_for(97000) == 2, "97 kHz");
    CHECK(decimator_factor_for(1e7) == DECIMATOR_MAX_FACTOR, "capped");
}

int main(void)
{
    printf("decimator\n");
    RUN(test_speech_band_passes_flat);
    RUN(test_what_would_fold_into_speech_is_removed);
    RUN(test_factor_choice);
    return test_failures ? 1 : 0;
}
