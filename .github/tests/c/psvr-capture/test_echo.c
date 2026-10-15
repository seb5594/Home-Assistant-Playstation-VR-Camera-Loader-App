/* Echo canceller convergence, double talk, and bulk delay alignment. */
#include <string.h>
#include <unistd.h>

#include "echo_canceller.h"
#include "echo_reference.h"
#include "test_util.h"

enum { RATE = 16000, BLOCK = AEC_BLOCK, PATH_TAPS = 1200 };

static float path[PATH_TAPS];

static void make_path(int delay)
{
    memset(path, 0, sizeof(path));
    for (int k = delay; k < PATH_TAPS; k++)
        path[k] = (float)(gaussian() * 0.25 * exp(-(k - delay) / 300.0));
    path[delay] = 0.5f;
}

/* Music-like reference: band-limited noise with a slow loudness wobble. */
static void make_reference(float *x, int n, double amplitude)
{
    double lp = 0;
    for (int i = 0; i < n; i++) {
        lp += 0.5 * (gaussian() - lp);
        x[i] = (float)(amplitude * lp * (0.7 + 0.3 * sin(2 * M_PI * 0.7 * i / RATE)));
    }
}

static void convolve(const float *x, int n, float *y)
{
    for (int i = 0; i < n; i++) {
        double acc = 0;
        for (int k = 0; k < PATH_TAPS && k <= i; k++) acc += path[k] * x[i - k];
        y[i] = (float)acc;
    }
}

static double energy(const float *x, int n)
{
    double e = 0;
    for (int i = 0; i < n; i++) e += (double)x[i] * x[i];
    return e;
}

static void test_converges_and_survives_double_talk(void)
{
    enum { N = RATE * 16 };
    static float reference[N], echo[N], talker[N], mic[N], out[N];
    static echo_canceller_t aec;

    test_rng = 4242;
    make_path(40);
    make_reference(reference, N, 6000);
    convolve(reference, N, echo);
    for (int i = 0; i < N; i++) {
        int talking = i >= 8 * RATE && i < 11 * RATE;
        talker[i] = talking ? (float)(2500 * gaussian() * (0.6 + 0.4 * sin(2 * M_PI * 4 * i / RATE))) : 0.0f;
        mic[i] = echo[i] + talker[i] + (float)(gaussian() * 10);
    }
    echo_canceller_init(&aec, RATE, 256);
    for (int start = 0; start + BLOCK <= N; start += BLOCK)
        echo_canceller_process(&aec, mic + start, reference + start, out + start);

    double erle = 10 * log10(energy(echo + 6 * RATE, 2 * RATE) / energy(out + 6 * RATE, 2 * RATE));
    double after = 10 * log10(energy(echo + 13 * RATE, 3 * RATE) / energy(out + 13 * RATE, 3 * RATE));
    double talker_kept = 10 * log10(energy(out + 8 * RATE + 2000, 3 * RATE - 2000) /
                                    energy(talker + 8 * RATE + 2000, 3 * RATE - 2000));
    printf("    ERLE %.1f dB after 6 s, %.1f dB after double talk, talker level %+.1f dB\n",
           erle, after, talker_kept);
    CHECK(erle > 20, "ERLE %.1f dB", erle);
    CHECK(after > 15, "ERLE after double talk %.1f dB", after);
    CHECK(talker_kept > -6 && talker_kept < 2, "talker changed by %.1f dB", talker_kept);
    echo_canceller_free(&aec);
}

static void test_bulk_delay_beyond_filter_is_found(void)
{
    enum { N = RATE * 30, BULK = RATE * 35 / 100 };   /* 350 ms, longer than the filter */
    static float reference[N], delayed[N], echo[N], out[BLOCK], ref_block[BLOCK];
    static echo_canceller_t aec;
    static echo_reference_t ref;
    int fds[2];
    double echo_energy = 0, out_energy = 0;

    test_rng = 99;
    make_path(20);
    make_reference(reference, N, 6000);
    for (int i = 0; i < N; i++) delayed[i] = i >= BULK ? reference[i - BULK] : 0.0f;
    convolve(delayed, N, echo);
    CHECK(pipe(fds) == 0, "pipe");
    echo_reference_init(&ref, fds[0]);
    echo_canceller_init(&aec, RATE, 256);
    for (int start = 0; start + BLOCK <= N; start += BLOCK) {
        int16_t pcm[BLOCK];
        for (int i = 0; i < BLOCK; i++) pcm[i] = (int16_t)reference[start + i];
        CHECK(write(fds[1], pcm, sizeof(pcm)) == sizeof(pcm), "feed reference");
        echo_reference_poll(&ref);
        echo_reference_block(&ref, ref_block);
        if (echo_reference_track(&ref, echo + start)) echo_canceller_reset(&aec);
        echo_canceller_process(&aec, echo + start, ref_block, out);
        if (start >= 25 * RATE) {
            echo_energy += energy(echo + start, BLOCK);
            out_energy += energy(out, BLOCK);
        }
    }
    double erle = 10 * log10(echo_energy / out_energy);
    printf("    reference held back %d ms (true 350 ms), ERLE %.1f dB\n", ref.delay * 1000 / RATE, erle);
    CHECK(ref.delay > 200 * RATE / 1000 && ref.delay < 350 * RATE / 1000, "delay %d", ref.delay);
    CHECK(erle > 15, "ERLE %.1f dB", erle);
    close(fds[0]);
    close(fds[1]);
    echo_canceller_free(&aec);
}

int main(void)
{
    printf("echo\n");
    RUN(test_converges_and_survives_double_talk);
    RUN(test_bulk_delay_beyond_filter_is_found);
    return test_failures ? 1 : 0;
}
