#include "decimator.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const double TARGET_RATE = 44000.0;   /* stay at or above this after decimation */

int decimator_factor_for(double input_rate)
{
    int factor = (int)(input_rate / TARGET_RATE);
    if (factor < 1) factor = 1;
    if (factor > DECIMATOR_MAX_FACTOR) factor = DECIMATOR_MAX_FACTOR;
    return factor;
}

int decimator_init(decimator_t *d, int channels, int factor)
{
    memset(d, 0, sizeof(*d));
    if (channels < 1 || factor < 1 || factor > DECIMATOR_MAX_FACTOR) return -1;
    d->channels = channels;
    d->factor = factor;
    if (factor == 1) return 0;
    /*
     * Blackman-windowed sinc, corner at 40 % of the new Nyquist band edge:
     * flat to well beyond the 8 kHz that survive the final conversion, and
     * everything that could fold back into 0-8 kHz is at least ~70 dB down.
     */
    d->taps = factor * DECIMATOR_TAPS_PER_FACTOR;
    double cutoff = 0.4 / factor, sum = 0.0, middle = (d->taps - 1) / 2.0;
    for (int n = 0; n < d->taps; n++) {
        double x = n - middle;
        double sinc = fabs(x) < 1e-9 ? 2.0 * cutoff : sin(2.0 * M_PI * cutoff * x) / (M_PI * x);
        double window = 0.42 - 0.5 * cos(2.0 * M_PI * n / (d->taps - 1)) + 0.08 * cos(4.0 * M_PI * n / (d->taps - 1));
        d->coefficients[n] = (float)(sinc * window);
        sum += d->coefficients[n];
    }
    for (int n = 0; n < d->taps; n++) d->coefficients[n] = (float)(d->coefficients[n] / sum);
    d->length = (size_t)d->taps - 1;          /* start from silence */
    d->next = d->length;
    d->capacity = 4096;
    d->buffer = calloc(d->capacity * channels, sizeof(float));
    return d->buffer ? 0 : -1;
}

void decimator_free(decimator_t *d)
{
    free(d->buffer);
    memset(d, 0, sizeof(*d));
}

size_t decimator_process(decimator_t *d, const float *in, size_t count, float *out)
{
    const int channels = d->channels, taps = d->taps;

    if (d->factor == 1) {
        memcpy(out, in, count * channels * sizeof(float));
        return count;
    }
    if (d->length + count > d->capacity) {
        size_t capacity = d->capacity;
        while (capacity < d->length + count) capacity *= 2;
        float *grown = realloc(d->buffer, capacity * channels * sizeof(float));
        if (!grown) return 0;
        d->buffer = grown;
        d->capacity = capacity;
    }
    memcpy(d->buffer + d->length * channels, in, count * channels * sizeof(float));
    d->length += count;

    size_t produced = 0;
    for (; d->next < d->length; d->next += (size_t)d->factor, produced++) {
        const float *newest = d->buffer + d->next * channels;
        for (int c = 0; c < channels; c++) {
            float acc = 0.0f;
            for (int n = 0; n < taps; n++) acc += d->coefficients[n] * newest[c - n * channels];
            out[produced * channels + c] = acc;
        }
    }
    /* Keep the last taps-1 frames for the next call. */
    size_t keep = (size_t)taps - 1, drop = d->length - keep;
    memmove(d->buffer, d->buffer + drop * channels, keep * channels * sizeof(float));
    d->length = keep;
    d->next -= drop;
    return produced;
}
