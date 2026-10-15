#include "echo_canceller.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const float MIN_LEAK = 0.005f;
static const float RESIDUAL_OVERESTIMATE = 2.0f;
static const float RESIDUAL_FLOOR = 0.06f;      /* at most -24 dB of extra suppression */
static const double ACTIVE_ENERGY = AEC_BLOCK * 1000.0;  /* about -60 dBFS */

int echo_canceller_init(echo_canceller_t *aec, int sample_rate, int filter_ms)
{
    memset(aec, 0, sizeof(*aec));
    aec->sample_rate = sample_rate;
    aec->partitions = (filter_ms * sample_rate / 1000 + AEC_BLOCK - 1) / AEC_BLOCK;
    if (aec->partitions < 2) aec->partitions = 2;
    if (aec->partitions > 64) aec->partitions = 64;
    if (fft_plan_init(&aec->fft, AEC_FFT)) return -1;
    aec->reference = calloc((size_t)aec->partitions * AEC_BINS, sizeof(cfloat));
    aec->filter = calloc((size_t)aec->partitions * AEC_BINS, sizeof(cfloat));
    if (!aec->reference || !aec->filter) return -1;
    for (int n = 0; n < AEC_FFT; n++)
        aec->window[n] = sqrtf(0.5f - 0.5f * cosf(2.0f * (float)M_PI * n / AEC_FFT));
    echo_canceller_reset(aec);
    return 0;
}

void echo_canceller_reset(echo_canceller_t *aec)
{
    memset(aec->reference, 0, (size_t)aec->partitions * AEC_BINS * sizeof(cfloat));
    memset(aec->filter, 0, (size_t)aec->partitions * AEC_BINS * sizeof(cfloat));
    memset(aec->reference_previous, 0, sizeof(aec->reference_previous));
    memset(aec->error_smooth, 0, sizeof(aec->error_smooth));
    memset(aec->echo_smooth, 0, sizeof(aec->echo_smooth));
    for (int k = 0; k < AEC_BINS; k++) {
        aec->reference_power[k] = 0.0f;
        aec->residual_gain[k] = 1.0f;
    }
    aec->pey = aec->pyy = 1.0;
    aec->leak = 1.0f;
    aec->adapted = 0;
    aec->adapt_sum = 0.0;
}

void echo_canceller_free(echo_canceller_t *aec)
{
    fft_plan_free(&aec->fft);
    free(aec->reference);
    free(aec->filter);
    aec->reference = aec->filter = NULL;
}

static cfloat *slot(cfloat *base, int partition) { return base + (size_t)partition * AEC_BINS; }

/* Spectrum of [zeros, block], the overlap-save error/echo basis. */
static void tail_spectrum(echo_canceller_t *aec, const float *block, cfloat *out)
{
    cfloat buffer[AEC_FFT];
    for (int n = 0; n < AEC_BLOCK; n++) {
        buffer[n] = 0;
        buffer[AEC_BLOCK + n] = block[n];
    }
    fft_forward(&aec->fft, buffer);
    memcpy(out, buffer, AEC_BINS * sizeof(cfloat));
}

/* Keep the time-domain filter causal (second half zero); see AUMDF. */
static void constrain(echo_canceller_t *aec, int partition)
{
    cfloat buffer[AEC_FFT], *w = slot(aec->filter, partition);
    for (int k = 0; k < AEC_BINS; k++) buffer[k] = w[k];
    for (int k = 1; k < AEC_BINS - 1; k++) buffer[AEC_FFT - k] = conjf(w[k]);
    fft_inverse(&aec->fft, buffer);
    for (int n = 0; n < AEC_FFT; n++)
        buffer[n] = n < AEC_BLOCK ? crealf(buffer[n]) / AEC_FFT : 0.0f;
    fft_forward(&aec->fft, buffer);
    memcpy(w, buffer, AEC_BINS * sizeof(cfloat));
}

static void suppress_residual(echo_canceller_t *aec, const float *error, const float *echo, float *out)
{
    cfloat buffer[AEC_FFT];

    memmove(aec->residual_in, aec->residual_in + AEC_BLOCK, AEC_BLOCK * sizeof(float));
    memmove(aec->residual_echo, aec->residual_echo + AEC_BLOCK, AEC_BLOCK * sizeof(float));
    memcpy(aec->residual_in + AEC_BLOCK, error, AEC_BLOCK * sizeof(float));
    memcpy(aec->residual_echo + AEC_BLOCK, echo, AEC_BLOCK * sizeof(float));
    for (int n = 0; n < AEC_FFT; n++)
        buffer[n] = aec->window[n] * (aec->residual_in[n] + I * aec->residual_echo[n]);
    fft_forward(&aec->fft, buffer);

    cfloat filtered[AEC_FFT];
    for (int k = 0; k < AEC_BINS; k++) {
        cfloat z = buffer[k], zm = conjf(buffer[(AEC_FFT - k) % AEC_FFT]);
        cfloat e = 0.5f * (z + zm), y = -0.5f * I * (z - zm);
        float error_power = crealf(e * conjf(e)), echo_power = crealf(y * conjf(y));
        float target = 1.0f;
        if (aec->reference_active)
            target = 1.0f - RESIDUAL_OVERESTIMATE * aec->leak * echo_power / (error_power + 1.0f);
        if (target < RESIDUAL_FLOOR) target = RESIDUAL_FLOOR;
        /* Close fast, open gently: avoids echo tails without pumping speech. */
        float g = aec->residual_gain[k];
        g = target < g ? target : 0.7f * g + 0.3f * target;
        aec->residual_gain[k] = g;
        filtered[k] = e * g;
    }
    for (int k = 1; k < AEC_BINS - 1; k++) filtered[AEC_FFT - k] = conjf(filtered[k]);
    filtered[0] = crealf(filtered[0]);
    filtered[AEC_BINS - 1] = crealf(filtered[AEC_BINS - 1]);
    fft_inverse(&aec->fft, filtered);
    for (int n = 0; n < AEC_BLOCK; n++)
        out[n] = aec->residual_overlap[n] + crealf(filtered[n]) * aec->window[n] / AEC_FFT;
    for (int n = 0; n < AEC_BLOCK; n++)
        aec->residual_overlap[n] = crealf(filtered[AEC_BLOCK + n]) * aec->window[AEC_BLOCK + n] / AEC_FFT;
}

void echo_canceller_process(echo_canceller_t *aec, const float *mic, const float *reference, float *out)
{
    const int P = aec->partitions;
    cfloat buffer[AEC_FFT], error_spectrum[AEC_BINS], echo_spectrum[AEC_BINS];
    float echo[AEC_BLOCK], error[AEC_BLOCK], step[AEC_BINS];
    double sxx = 0, see = 0, syy = 0, sey = 0, sdd = 0;
    const float smoothing = (float)AEC_BLOCK / aec->sample_rate;  /* Speex "spec_average" */

    /* 1. new reference spectrum over [previous block, current block] */
    for (int n = 0; n < AEC_BLOCK; n++) {
        buffer[n] = aec->reference_previous[n];
        buffer[AEC_BLOCK + n] = reference[n];
        sxx += (double)reference[n] * reference[n];
    }
    memcpy(aec->reference_previous, reference, sizeof(aec->reference_previous));
    fft_forward(&aec->fft, buffer);
    aec->newest = (aec->newest + 1) % P;
    memcpy(slot(aec->reference, aec->newest), buffer, AEC_BINS * sizeof(cfloat));
    aec->reference_active = sxx > ACTIVE_ENERGY;
    {
        const float ss = 0.35f / P;
        const cfloat *x = slot(aec->reference, aec->newest);
        for (int k = 0; k < AEC_BINS; k++)
            aec->reference_power[k] = (1.0f - ss) * aec->reference_power[k] + 1.0f +
                                      ss * crealf(x[k] * conjf(x[k]));
    }

    /* 2. echo estimate: sum over partitions, overlap-save */
    for (int k = 0; k < AEC_BINS; k++) {
        cfloat acc = 0;
        for (int p = 0; p < P; p++)
            acc += slot(aec->filter, p)[k] * slot(aec->reference, (aec->newest - p + P) % P)[k];
        buffer[k] = acc;
    }
    for (int k = 1; k < AEC_BINS - 1; k++) buffer[AEC_FFT - k] = conjf(buffer[k]);
    buffer[0] = crealf(buffer[0]);
    buffer[AEC_BINS - 1] = crealf(buffer[AEC_BINS - 1]);
    fft_inverse(&aec->fft, buffer);
    for (int n = 0; n < AEC_BLOCK; n++) {
        echo[n] = crealf(buffer[AEC_BLOCK + n]) / AEC_FFT;
        error[n] = mic[n] - echo[n];
        see += (double)error[n] * error[n];
        syy += (double)echo[n] * echo[n];
        sey += (double)error[n] * echo[n];
        sdd += (double)mic[n] * mic[n];
    }
    see += 1.0;

    /* 3. leakage: how much of the error still correlates with the echo */
    tail_spectrum(aec, error, error_spectrum);
    tail_spectrum(aec, echo, echo_spectrum);
    double pey = 0, pyy = 0;
    float error_power[AEC_BINS], echo_power[AEC_BINS];
    for (int k = 0; k < AEC_BINS; k++) {
        error_power[k] = crealf(error_spectrum[k] * conjf(error_spectrum[k]));
        echo_power[k] = crealf(echo_spectrum[k] * conjf(echo_spectrum[k]));
        double eh = error_power[k] - aec->error_smooth[k], yh = echo_power[k] - aec->echo_smooth[k];
        pey += eh * yh;
        pyy += yh * yh;
        aec->error_smooth[k] = (1 - smoothing) * aec->error_smooth[k] + smoothing * error_power[k];
        aec->echo_smooth[k] = (1 - smoothing) * aec->echo_smooth[k] + smoothing * echo_power[k];
    }
    pyy = sqrt(pyy);
    pey = pyy > 0 ? pey / pyy : 0.0;
    {
        double rate = 2.0 * smoothing * syy, cap = 0.5 * smoothing * see;
        double alpha = (rate > cap ? cap : rate) / see;
        aec->pey = (1 - alpha) * aec->pey + alpha * pey;
        aec->pyy = (1 - alpha) * aec->pyy + alpha * pyy;
        if (aec->pyy < 1) aec->pyy = 1;
        if (aec->pey < MIN_LEAK * aec->pyy) aec->pey = MIN_LEAK * aec->pyy;
        if (aec->pey > aec->pyy) aec->pey = aec->pyy;
        aec->leak = (float)(aec->pey / aec->pyy);
    }

    /* 4. step size: residual-to-error ratio, small during double talk */
    double rer = (0.0001 * sxx + 3.0 * aec->leak * syy) / see;
    double floor_rer = sey * sey / (1.0 + see * syy);
    if (rer < floor_rer) rer = floor_rer;
    if (rer > 0.5) rer = 0.5;
    if (!aec->adapted) {
        double rate = 0.0;
        if (aec->reference_active) {
            rate = 0.25 * sxx / see;
            if (rate > 0.25) rate = 0.25;
            aec->adapt_sum += rate;
            if (aec->adapt_sum > P && aec->leak > 0.03f) aec->adapted = 1;
        }
        for (int k = 0; k < AEC_BINS; k++) step[k] = (float)(rate / (aec->reference_power[k] + 10.0f));
    } else {
        for (int k = 0; k < AEC_BINS; k++) {
            float e = error_power[k] + 1.0f, r = aec->leak * echo_power[k];
            if (r > 0.5f * e) r = 0.5f * e;
            r = 0.7f * r + 0.3f * (float)rer * e;
            step[k] = r / (e * (aec->reference_power[k] + 10.0f));
        }
    }

    /* 5. proportionate update: partitions holding more of the echo adapt faster */
    if (aec->reference_active) {
        float prop[64], prop_sum = 1.0f, max_norm = 1.0f;
        int count = P;
        for (int p = 0; p < count; p++) {
            double energy = 0;
            const cfloat *w = slot(aec->filter, p);
            for (int k = 0; k < AEC_BINS; k++) energy += crealf(w[k] * conjf(w[k]));
            prop[p] = (float)sqrt(energy);
            if (prop[p] > max_norm) max_norm = prop[p];
        }
        for (int p = 0; p < count; p++) {
            prop[p] += 0.1f * max_norm;
            prop_sum += prop[p];
        }
        for (int p = 0; p < count; p++) {
            float share = 0.99f * prop[p] / prop_sum;
            cfloat *w = slot(aec->filter, p);
            const cfloat *x = slot(aec->reference, (aec->newest - p + P) % P);
            for (int k = 0; k < AEC_BINS; k++)
                w[k] += share * step[k] * conjf(x[k]) * error_spectrum[k];
        }
        constrain(aec, 0);
        if (P > 1) constrain(aec, 1 + (int)(aec->blocks % (P - 1)));
    }

    /* 6. leftover echo */
    suppress_residual(aec, error, echo, out);

    if (aec->reference_active) {
        aec->mic_energy = 0.95 * aec->mic_energy + sdd;
        aec->error_energy = 0.95 * aec->error_energy + see;
        aec->erle_db = (float)(10.0 * log10((aec->mic_energy + 1) / (aec->error_energy + 1)));
    }
    aec->blocks++;
}
