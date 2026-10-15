/* Tiny assertion helpers shared by the engine tests. */
#ifndef PSVR_TEST_UTIL_H
#define PSVR_TEST_UTIL_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int test_failures __attribute__((unused));

#define CHECK(condition, ...)                                              \
    do {                                                                   \
        if (!(condition)) {                                                \
            test_failures++;                                               \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);         \
            fprintf(stderr, __VA_ARGS__);                                  \
            fprintf(stderr, "\n");                                         \
        }                                                                  \
    } while (0)

#define RUN(test)                                                          \
    do {                                                                   \
        int before = test_failures;                                        \
        test();                                                            \
        printf("  %s %s\n", test_failures == before ? "ok  " : "FAIL", #test); \
    } while (0)

static unsigned long long test_rng = 0x9E3779B97F4A7C15ull;

static inline double uniform(void)
{
    test_rng ^= test_rng << 13;
    test_rng ^= test_rng >> 7;
    test_rng ^= test_rng << 17;
    return (double)(test_rng >> 11) / 9007199254740992.0;
}

static inline double gaussian(void)
{
    double u = uniform() + 1e-12, v = uniform();
    return sqrt(-2.0 * log(u)) * cos(2.0 * M_PI * v);
}

/* Power of x after removing the best-fitting sine at freq (residual = distortion). */
static inline double tone_residual_db(const float *x, int n, double freq, double rate)
{
    double c = 0, s = 0, total = 0, fitted = 0;
    for (int i = 0; i < n; i++) {
        c += x[i] * cos(2 * M_PI * freq * i / rate);
        s += x[i] * sin(2 * M_PI * freq * i / rate);
        total += (double)x[i] * x[i];
    }
    fitted = 2.0 * (c * c + s * s) / n;
    double residual = total - fitted;
    if (residual < 1e-12) residual = 1e-12;
    return 10.0 * log10(residual / (fitted + 1e-12));
}

#endif
