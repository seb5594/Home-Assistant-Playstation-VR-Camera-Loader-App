#include "resampler.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
    ZERO_CROSSINGS = 10,     /* kernel half-length in sinc lobes */
    TABLE_STEPS = 512,       /* table entries per lobe */
    MAX_CHANNELS = 8,
};

static const double KAISER_BETA = 7.0;

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 32; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}

static void build_table(float *table)
{
    const int entries = ZERO_CROSSINGS * TABLE_STEPS;
    const double norm = bessel_i0(KAISER_BETA);

    for (int i = 0; i <= entries; i++) {
        double x = (double)i / TABLE_STEPS;
        double sinc = i ? sin(M_PI * x) / (M_PI * x) : 1.0;
        double r = x / ZERO_CROSSINGS;
        double window = bessel_i0(KAISER_BETA * sqrt(fmax(0.0, 1.0 - r * r))) / norm;
        table[i] = (float)(sinc * window);
    }
    table[entries + 1] = 0.0f;
}

static float kernel(const float *table, double lobes)
{
    double index = lobes * TABLE_STEPS;
    int i = (int)index;

    if (i >= ZERO_CROSSINGS * TABLE_STEPS) return 0.0f;
    return table[i] + (float)(index - i) * (table[i + 1] - table[i]);
}

static int reserve(resampler_t *rs, size_t frames)
{
    if (frames <= rs->capacity) return 0;
    size_t capacity = rs->capacity ? rs->capacity : 1024;
    while (capacity < frames) capacity *= 2;
    float *grown = realloc(rs->history, capacity * rs->channels * sizeof(float));
    if (!grown) return -1;
    rs->history = grown;
    rs->capacity = capacity;
    return 0;
}

void resampler_set_input_rate(resampler_t *rs, double in_rate)
{
    rs->in_rate = in_rate;
    rs->step = in_rate / rs->out_rate;
    /* Leave 10 % guard band below the lower Nyquist frequency. */
    rs->cutoff = (in_rate > rs->out_rate ? rs->out_rate / in_rate : 1.0) * 0.9;
    rs->half_width = ZERO_CROSSINGS / rs->cutoff;
}

int resampler_init(resampler_t *rs, int channels, double in_rate, double out_rate)
{
    memset(rs, 0, sizeof(*rs));
    if (channels < 1 || channels > MAX_CHANNELS || in_rate <= 0 || out_rate <= 0) return -1;
    rs->channels = channels;
    rs->out_rate = out_rate;
    rs->table = malloc((ZERO_CROSSINGS * TABLE_STEPS + 2) * sizeof(float));
    if (!rs->table) return -1;
    build_table(rs->table);
    resampler_set_input_rate(rs, in_rate);
    /* Start with silence so the first output sample has full left context. */
    size_t lead = (size_t)ceil(rs->half_width) + 1;
    if (reserve(rs, lead)) return -1;
    memset(rs->history, 0, lead * channels * sizeof(float));
    rs->length = lead;
    rs->position = (double)lead - 1;
    return 0;
}

void resampler_free(resampler_t *rs)
{
    free(rs->history);
    free(rs->table);
    memset(rs, 0, sizeof(*rs));
}

int resampler_push(resampler_t *rs, const float *frames, size_t count)
{
    /* Drop history that no future output can reach. */
    double oldest_needed = rs->position - rs->half_width - 2;
    if (oldest_needed > 4096) {
        size_t drop = (size_t)oldest_needed;
        memmove(rs->history, rs->history + drop * rs->channels,
                (rs->length - drop) * rs->channels * sizeof(float));
        rs->length -= drop;
        rs->position -= (double)drop;
    }
    if (reserve(rs, rs->length + count)) return -1;
    memcpy(rs->history + rs->length * rs->channels, frames, count * rs->channels * sizeof(float));
    rs->length += count;
    return 0;
}

size_t resampler_pull(resampler_t *rs, float *out, size_t max_frames)
{
    const int channels = rs->channels;
    size_t produced = 0;

    while (produced < max_frames) {
        double t = rs->position;
        long first = (long)ceil(t - rs->half_width);
        long last = (long)floor(t + rs->half_width);
        float acc[MAX_CHANNELS] = {0};
        double weight_sum = 0.0;

        if (last >= (long)rs->length) break;
        if (first < 0) first = 0;
        for (long j = first; j <= last; j++) {
            float w = kernel(rs->table, fabs(t - (double)j) * rs->cutoff);
            const float *frame = rs->history + j * channels;
            weight_sum += w;
            for (int c = 0; c < channels; c++) acc[c] += w * frame[c];
        }
        /* Normalising by the tap sum gives exact unity gain at DC. */
        float scale = weight_sum > 1e-9 ? (float)(1.0 / weight_sum) : 0.0f;
        for (int c = 0; c < channels; c++) out[produced * channels + c] = acc[c] * scale;
        produced++;
        rs->position += rs->step;
    }
    return produced;
}
