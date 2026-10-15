#include "filters.h"

#include <math.h>
#include <string.h>

void biquad_highpass(biquad_t *f, double corner_hz, double sample_rate)
{
    /* RBJ cookbook, Butterworth Q. */
    double w0 = 2.0 * M_PI * corner_hz / sample_rate;
    double alpha = sin(w0) / (2.0 * M_SQRT1_2);
    double c = cos(w0), a0 = 1.0 + alpha;

    memset(f, 0, sizeof(*f));
    f->b0 = (float)((1.0 + c) / 2.0 / a0);
    f->b1 = (float)(-(1.0 + c) / a0);
    f->b2 = f->b0;
    f->a1 = (float)(-2.0 * c / a0);
    f->a2 = (float)((1.0 - alpha) / a0);
}

void biquad_process(biquad_t *f, float *samples, int count, int stride)
{
    float z1 = f->z1, z2 = f->z2;
    for (int i = 0; i < count; i++) {
        float x = samples[i * stride];
        float y = f->b0 * x + z1;
        z1 = f->b1 * x - f->a1 * y + z2;
        z2 = f->b2 * x - f->a2 * y;
        samples[i * stride] = y;
    }
    /* Flush denormals during long silences. */
    f->z1 = fabsf(z1) < 1e-20f ? 0.0f : z1;
    f->z2 = fabsf(z2) < 1e-20f ? 0.0f : z2;
}

void rate3_init(rate3_t *r)
{
    const int length = RATE3_FACTOR * RATE3_TAPS_PER_PHASE;
    const double center = (length - 1) / 2.0, cutoff = 0.92 / RATE3_FACTOR, beta = 6.0;
    double i0beta = 0, term = 1;

    memset(r, 0, sizeof(*r));
    for (int k = 0; k < 25; k++) {
        if (k) term *= (beta / (2.0 * k)) * (beta / (2.0 * k));
        i0beta += term;
    }
    for (int n = 0; n < length; n++) {
        double x = n - center, ratio = x / (center + 1);
        double sinc = fabs(x) < 1e-9 ? 1.0 : sin(M_PI * cutoff * x) / (M_PI * cutoff * x);
        double arg = beta * sqrt(fmax(0.0, 1.0 - ratio * ratio)), i0 = 0, t = 1;
        for (int k = 0; k < 25; k++) {
            if (k) t *= (arg / (2.0 * k)) * (arg / (2.0 * k));
            i0 += t;
        }
        r->taps[n] = (float)(cutoff * sinc * i0 / i0beta);
    }
}

/* Newest sample first; window covers `length` consecutive history entries. */
static void push(rate3_t *r, float sample, int length)
{
    r->position = (r->position + length - 1) % length;
    r->history[r->position] = sample;
    r->history[r->position + length] = sample;
}

void rate3_upsample(rate3_t *r, const float *in, int count, float *out)
{
    const int length = RATE3_TAPS_PER_PHASE;
    for (int i = 0; i < count; i++) {
        push(r, in[i], length);
        const float *window = r->history + r->position;
        for (int p = 0; p < RATE3_FACTOR; p++) {
            /* Polyphase branch p of the zero-stuffed signal; x3 restores the level. */
            float acc = 0.0f;
            for (int j = 0; j < length; j++) acc += r->taps[p + RATE3_FACTOR * j] * window[j];
            out[i * RATE3_FACTOR + p] = acc * RATE3_FACTOR;
        }
    }
}

void rate3_downsample(rate3_t *r, const float *in, int count, float *out)
{
    const int length = RATE3_FACTOR * RATE3_TAPS_PER_PHASE;
    int produced = 0;
    for (int i = 0; i < count; i++) {
        push(r, in[i], length);
        if (++r->phase < RATE3_FACTOR) continue;
        r->phase = 0;
        const float *window = r->history + r->position;
        float acc = 0.0f;
        for (int n = 0; n < length; n++) acc += r->taps[n] * window[n];
        out[produced++] = acc;
    }
}
