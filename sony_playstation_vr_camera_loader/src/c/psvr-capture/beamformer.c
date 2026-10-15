#include "beamformer.h"

#include <math.h>
#include <string.h>

#include "cmatrix4.h"

static const float LEARN_SECONDS = 4.0f;     /* covariance memory */
static const float WARMUP_SECONDS = 2.0f;    /* learn everything at the start */
static const float SHORT_SECONDS = 0.2f;     /* onset detector, fast level */
static const float SLOW_SECONDS = 15.0f;     /* onset detector, background level */
static const float ONSET_FREE = 1.25f;       /* broadband: below +1 dB learn normally */
static const float ONSET_BLOCKED = 2.0f;     /* broadband: above +3 dB do not learn */
static const float BIN_ONSET_FREE = 2.0f;    /* per bin, catches narrow-band onsets */
static const float BIN_ONSET_BLOCKED = 4.0f;
static const float LOW_HZ = 300.0f;          /* the array cannot resolve lower */
static const float HIGH_HZ = 7000.0f;
static const float COHERENCE_START = 0.55f;  /* share of energy in one direction */
static const float COHERENCE_FULL = 0.85f;
static const float BROADSIDE_KEEP = 0.71f;   /* front talker response >= -3 dB per bin */
static const float WEIGHT_SMOOTHING = 0.75f;
static const int EIGEN_INTERVAL = 4;         /* refresh each bin every 4th frame */

/* speech focus */
static const float SPEECH_LOW_HZ = 300.0f;   /* where voices carry their energy */
static const float SPEECH_HIGH_HZ = 3400.0f;
static const float PRESENCE_START_DB = 3.0f; /* unexplained energy over the background */
static const float PRESENCE_FULL_DB = 9.0f;
static const float PRESENCE_RELEASE_SECONDS = 0.3f;
static const float BIN_NOVELTY_START = 2.0f; /* a bin joins the talker estimate ... */
static const float BIN_NOVELTY_FULL = 6.0f;  /* ... when it carries clearly new energy */
static const float SPEECH_SECONDS = 1.0f;    /* talker covariance memory, counted while speaking */
static const float NEW_BACKGROUND_SECONDS = 6.0f;  /* nobody talks this long without a pause */
static const float TALKER_RATIO_START = 1.5f;      /* talker over background in its direction */
static const float TALKER_RATIO_FULL = 4.0f;
static const float MAX_NOISE_GAIN = 1.0f;    /* |w|^2; the plain average has 0.25 */
static const float DOMINANT_START = 4.0f;    /* eigenvalue over the diffuse floor */
static const float DOMINANT_FULL = 16.0f;

static float smoothstep(float x, float edge0, float edge1)
{
    float t = (x - edge0) / (edge1 - edge0);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static float alpha_for(float seconds, int sample_rate)
{
    return expf(-(float)BEAM_HOP / (sample_rate * seconds));
}

static int bin_of(float hz, int sample_rate) { return (int)(hz * BEAM_FFT / sample_rate); }

const char *beamformer_mode_name(beam_mode_t mode)
{
    return mode == BEAM_FOCUS ? "focus" : mode == BEAM_ADAPTIVE ? "adaptive" : "sum";
}

beam_mode_t beamformer_mode_from_name(const char *name)
{
    if (!strcmp(name, "focus")) return BEAM_FOCUS;
    if (!strcmp(name, "adaptive")) return BEAM_ADAPTIVE;
    return BEAM_SUM;
}

int beamformer_init(beamformer_t *bf, beam_mode_t mode, int sample_rate)
{
    memset(bf, 0, sizeof(*bf));
    bf->mode = mode;
    bf->sample_rate = sample_rate;
    if (fft_plan_init(&bf->fft, BEAM_FFT)) return -1;
    for (int n = 0; n < BEAM_FFT; n++)
        bf->window[n] = sqrtf(0.5f - 0.5f * cosf(2.0f * (float)M_PI * n / BEAM_FFT));
    bf->bin_low = bin_of(LOW_HZ, sample_rate);
    bf->bin_high = bin_of(HIGH_HZ, sample_rate);
    if (bf->bin_high > BEAM_BINS - 2) bf->bin_high = BEAM_BINS - 2;
    bf->speech_low = bin_of(SPEECH_LOW_HZ, sample_rate);
    bf->speech_high = bin_of(SPEECH_HIGH_HZ, sample_rate);
    bf->alpha_short = alpha_for(SHORT_SECONDS, sample_rate);
    bf->alpha_slow = alpha_for(SLOW_SECONDS, sample_rate);
    bf->alpha_learn = alpha_for(LEARN_SECONDS, sample_rate);
    bf->alpha_warmup = alpha_for(0.5f, sample_rate);
    bf->focus.alpha_speech = alpha_for(SPEECH_SECONDS, sample_rate);
    bf->focus.release = alpha_for(PRESENCE_RELEASE_SECONDS, sample_rate);
    for (int k = 0; k < BEAM_BINS; k++) {
        for (int c = 0; c < BEAM_CHANNELS; c++) {
            bf->eigen[k][0][c] = 0.5f;                       /* broadside */
            bf->eigen[k][1][c] = (c & 1) ? -0.5f : 0.5f;     /* orthogonal start */
            bf->notch[k][c] = bf->weights[k][c] = 1.0f / BEAM_CHANNELS;
            bf->focus.talker[k][c] = 0.5f;
        }
    }
    return 0;
}

void beamformer_free(beamformer_t *bf) { fft_plan_free(&bf->fft); }

/* ---- persistent sources and their notch (adaptive and focus) ----------- */

/* Two warm-started power iterations per call track slowly moving sources. */
static void refresh_notch(beamformer_t *bf, int k)
{
    const cfloat *r = bf->covariance[k];
    cfloat *v1 = bf->eigen[k][0], *v2 = bf->eigen[k][1], tmp[BEAM_CHANNELS];
    cfloat broadside[BEAM_CHANNELS] = {0.5f, 0.5f, 0.5f, 0.5f};
    float trace = cm4_trace(r), lambda1, lambda2, s1, s2;

    if (trace <= 1e-12f) return;

    for (int it = 0; it < 2; it++) {
        cm4_mat_vec(r, v1, tmp);
        memcpy(v1, tmp, sizeof(tmp));
        if (cm4_normalize(v1) == 0.0f) memcpy(v1, broadside, sizeof(broadside));
    }
    cm4_mat_vec(r, v1, tmp);
    lambda1 = crealf(cm4_dot(v1, tmp));

    for (int it = 0; it < 2; it++) {
        cm4_mat_vec(r, v2, tmp);
        cfloat proj = cm4_dot(v1, v2);
        for (int c = 0; c < BEAM_CHANNELS; c++) tmp[c] -= lambda1 * v1[c] * proj;  /* deflate */
        proj = cm4_dot(v1, tmp);
        for (int c = 0; c < BEAM_CHANNELS; c++) tmp[c] -= v1[c] * proj;            /* stay orthogonal */
        memcpy(v2, tmp, sizeof(tmp));
        if (cm4_normalize(v2) == 0.0f) return;
    }
    cm4_mat_vec(r, v2, tmp);
    lambda2 = crealf(cm4_dot(v2, tmp)) - lambda1 * crealf(cm4_dot(v2, v1) * cm4_dot(v1, v2));
    bf->eigenvalue[k][0] = lambda1;
    bf->eigenvalue[k][1] = lambda2 > 0.0f ? lambda2 : 0.0f;

    float c1 = lambda1 / trace, c2 = lambda2 / trace;
    s1 = smoothstep(c1, COHERENCE_START, COHERENCE_FULL);
    /* A second notch only for a clear second source, e.g. stereo TV speakers. */
    s2 = smoothstep(c2, 0.18f, 0.32f) * smoothstep(c1 + c2, 0.75f, 0.9f);
    /* For talker detection: every direction that stands out of the diffuse floor is "background". */
    float floor = fmaxf(0.5f * (trace - lambda1 - bf->eigenvalue[k][1]), 1e-6f * trace);
    bf->dominance[k][0] = smoothstep(lambda1 / floor, DOMINANT_START, DOMINANT_FULL);
    bf->dominance[k][1] = smoothstep(bf->eigenvalue[k][1] / floor, DOMINANT_START, DOMINANT_FULL);

    float loss1 = crealf(cm4_dot(v1, broadside) * cm4_dot(broadside, v1));
    float loss2 = crealf(cm4_dot(v2, broadside) * cm4_dot(broadside, v2));
    float loss = s1 * loss1 + s2 * loss2;
    if (loss > 1.0f - BROADSIDE_KEEP) {
        float scale = (1.0f - BROADSIDE_KEEP) / loss;
        s1 *= scale;
        s2 *= scale;
    }
    bf->strength[k][0] = 0.6f * bf->strength[k][0] + 0.4f * s1;
    bf->strength[k][1] = 0.6f * bf->strength[k][1] + 0.4f * s2;

    /* w = P b / sqrt(M), with P = I - s1 v1 v1^H - s2 v2 v2^H */
    cfloat a1 = cm4_dot(v1, broadside), a2 = cm4_dot(v2, broadside);
    for (int c = 0; c < BEAM_CHANNELS; c++) {
        cfloat target = (broadside[c] - bf->strength[k][0] * v1[c] * a1 -
                         bf->strength[k][1] * v2[c] * a2) * 0.5f;
        bf->notch[k][c] = WEIGHT_SMOOTHING * bf->notch[k][c] + (1.0f - WEIGHT_SMOOTHING) * target;
    }
}

static void learn_background(beamformer_t *bf, int k, const cfloat *x, int warmup, float gate)
{
    float power = cm4_norm2(x), weight;

    bf->power_short[k] = bf->alpha_short * bf->power_short[k] + (1.0f - bf->alpha_short) * power;
    bf->power_slow[k] = bf->alpha_slow * bf->power_slow[k] + (1.0f - bf->alpha_slow) * power;

    if (bf->focus.new_background) {
        weight = 1.0f - bf->alpha_learn;      /* a new steady source: take it in right away */
    } else if (warmup) {
        weight = 1.0f - bf->alpha_warmup;
    } else {
        float onset = bf->power_short[k] / (bf->power_slow[k] + 1e-9f);
        weight = (1.0f - bf->alpha_learn) * gate * (1.0f - smoothstep(onset, BIN_ONSET_FREE, BIN_ONSET_BLOCKED));
    }
    if (weight > 0.0f) cm4_track(bf->covariance[k], x, weight);
}

/* ---- speech focus -------------------------------------------------------- */

/*
 * Energy of x that the persistent sources do not explain, and what the
 * background alone would leave there. Projecting out the dominant directions
 * makes this blind to a louder TV, but not to a voice from somewhere else.
 */
static void unexplained(const beamformer_t *bf, int k, const cfloat *x, float *energy, float *expected)
{
    cfloat residual[BEAM_CHANNELS];
    memcpy(residual, x, sizeof(residual));
    float background = cm4_trace(bf->covariance[k]);
    for (int i = 0; i < 2; i++) {
        float s = bf->dominance[k][i];
        if (s <= 0.0f) continue;
        const cfloat *v = bf->eigen[k][i];
        cfloat projection = cm4_dot(v, x);
        for (int c = 0; c < BEAM_CHANNELS; c++) residual[c] -= s * v[c] * projection;
        background -= (2.0f * s - s * s) * bf->eigenvalue[k][i];
    }
    *energy = cm4_norm2(residual);
    *expected = fmaxf(background, 1e-4f * cm4_trace(bf->covariance[k]) + 1e-12f);
}

static float update_presence(beamformer_t *bf, cfloat spectrum[BEAM_CHANNELS][BEAM_BINS], int warmup)
{
    speech_focus_t *f = &bf->focus;
    double energy = 0.0, expected = 0.0;

    for (int k = bf->speech_low; k <= bf->speech_high; k++) {
        cfloat x[BEAM_CHANNELS];
        float e, n;
        for (int c = 0; c < BEAM_CHANNELS; c++) x[c] = spectrum[c][k];
        unexplained(bf, k, x, &e, &n);
        energy += e;
        expected += n;
    }
    f->novelty_db = expected > 0 ? (float)(10.0 * log10(energy / expected + 1e-12)) : 0.0f;
    float target = warmup ? 0.0f : smoothstep(f->novelty_db, PRESENCE_START_DB, PRESENCE_FULL_DB);

    /*
     * A "voice" that never pauses is a new persistent source (fan, vacuum,
     * kettle): undo what it taught the talker estimate and let the background
     * take it in, until the unexplained energy is gone.
     */
    float seconds = (float)BEAM_HOP / bf->sample_rate;
    if (target > 0.5f) {
        if (f->busy_seconds == 0.0f && !f->new_background) {
            memcpy(f->speech_before, f->speech, sizeof(f->speech));
            memcpy(f->heard_before, f->heard, sizeof(f->heard));
        }
        f->busy_seconds += seconds;
    } else {
        f->busy_seconds = 0.0f;
    }
    if (f->busy_seconds > NEW_BACKGROUND_SECONDS && !f->new_background) {
        memcpy(f->speech, f->speech_before, sizeof(f->speech));
        memcpy(f->heard, f->heard_before, sizeof(f->heard));
        f->new_background = 1;
    }
    if (f->new_background && target < 0.3f) f->new_background = 0;
    if (f->new_background) target = 0.0f;

    f->presence = f->new_background ? 0.0f : fmaxf(target, f->presence * f->release);
    return f->presence;
}

static void learn_talker(beamformer_t *bf, int k, const cfloat *x)
{
    speech_focus_t *f = &bf->focus;
    float e, n;
    if (f->presence <= 0.01f) return;
    unexplained(bf, k, x, &e, &n);
    float weight = (1.0f - f->alpha_speech) * f->presence * smoothstep(e / n, BIN_NOVELTY_START, BIN_NOVELTY_FULL);
    if (weight <= 0.0f) return;
    cm4_track(f->speech[k], x, weight);
    f->heard[k] += weight * (1.0f - f->heard[k]);
}

/*
 * Talker beam for one bin: v = principal generalised eigenvector of
 * (speech, background), found by power iteration on background^-1 * speech.
 * The talker's transfer function is h = background * v; the MVDR weights
 * w = beta * v pass h like the plain capsule average does and suppress the
 * background as much as four capsules allow.
 */
static float refresh_talker(beamformer_t *bf, int k, cfloat *w)
{
    speech_focus_t *f = &bf->focus;
    const cfloat *noise = bf->covariance[k], *speech = f->speech[k];
    cfloat factor[BEAM_MATRIX], *v = f->talker[k], z[BEAM_CHANNELS], h[BEAM_CHANNELS];
    float trace = cm4_trace(noise);

    if (f->heard[k] < 0.05f || trace <= 1e-12f) return 0.0f;
    if (!cm4_cholesky(noise, 1e-3f * trace / BEAM_CHANNELS + 1e-9f, factor)) return 0.0f;
    for (int it = 0; it < 2; it++) {
        cm4_mat_vec(speech, v, z);
        cm4_solve(factor, z, v);
        if (cm4_normalize(v) == 0.0f) {
            for (int c = 0; c < BEAM_CHANNELS; c++) v[c] = 0.5f;
            return 0.0f;
        }
    }
    cm4_mat_vec(noise, v, h);
    cm4_mat_vec(speech, v, z);
    float background = crealf(cm4_dot(v, h)), during_speech = crealf(cm4_dot(v, z));
    if (background <= 1e-30f) return 0.0f;

    cfloat average = 0;                          /* what the capsule average hears of the talker */
    for (int c = 0; c < BEAM_CHANNELS; c++) average += h[c];
    average /= BEAM_CHANNELS;
    cfloat beta = conjf(average / background);   /* v^H h = background, real */
    for (int c = 0; c < BEAM_CHANNELS; c++) w[c] = beta * v[c];

    float confidence = f->heard[k] * smoothstep(during_speech / background, TALKER_RATIO_START, TALKER_RATIO_FULL);
    float noise_gain = cm4_norm2(w);
    if (noise_gain > MAX_NOISE_GAIN) confidence *= MAX_NOISE_GAIN / noise_gain;   /* do not amplify hiss */
    return confidence;
}

static void refresh_bin(beamformer_t *bf, int k)
{
    refresh_notch(bf, k);
    if (bf->mode != BEAM_FOCUS) {
        memcpy(bf->weights[k], bf->notch[k], sizeof(bf->weights[k]));
        return;
    }
    cfloat talker[BEAM_CHANNELS] = {0};
    float confidence = refresh_talker(bf, k, talker);
    bf->focus.confidence[k] = confidence;
    for (int c = 0; c < BEAM_CHANNELS; c++) {
        cfloat target = confidence * talker[c] + (1.0f - confidence) * bf->notch[k][c];
        bf->weights[k][c] = WEIGHT_SMOOTHING * bf->weights[k][c] + (1.0f - WEIGHT_SMOOTHING) * target;
    }
}

/* ---- framing ---------------------------------------------------------------- */

static void spectra(beamformer_t *bf, cfloat spectrum[BEAM_CHANNELS][BEAM_BINS])
{
    cfloat buffer[BEAM_FFT];
    for (int pair = 0; pair < BEAM_CHANNELS / 2; pair++) {
        const float *a = bf->input[2 * pair], *b = bf->input[2 * pair + 1];
        for (int n = 0; n < BEAM_FFT; n++) buffer[n] = bf->window[n] * (a[n] + I * b[n]);
        fft_forward(&bf->fft, buffer);
        /* Two real signals share one complex FFT; split them by symmetry. */
        for (int k = 0; k < BEAM_BINS; k++) {
            cfloat z = buffer[k], zm = conjf(buffer[(BEAM_FFT - k) % BEAM_FFT]);
            spectrum[2 * pair][k] = 0.5f * (z + zm);
            spectrum[2 * pair + 1][k] = -0.5f * I * (z - zm);
        }
    }
}

static void sum_channels(const float *in, float *out)
{
    for (int i = 0; i < BEAM_HOP; i++) {
        float acc = 0.0f;
        for (int c = 0; c < BEAM_CHANNELS; c++) acc += in[i * BEAM_CHANNELS + c];
        out[i] = acc / BEAM_CHANNELS;
    }
}

void beamformer_process(beamformer_t *bf, const float *in, float *out)
{
    cfloat spectrum[BEAM_CHANNELS][BEAM_BINS], output[BEAM_FFT];
    int warmup = bf->frames * BEAM_HOP < (long)(WARMUP_SECONDS * bf->sample_rate);
    double energy_sum = 0.0, energy_out = 0.0;
    int notched = 0, focused = 0;

    if (bf->mode == BEAM_SUM) {
        sum_channels(in, out);
        bf->frames++;
        return;
    }

    for (int c = 0; c < BEAM_CHANNELS; c++) {
        memmove(bf->input[c], bf->input[c] + BEAM_HOP, (BEAM_FFT - BEAM_HOP) * sizeof(float));
        for (int i = 0; i < BEAM_HOP; i++)
            bf->input[c][BEAM_FFT - BEAM_HOP + i] = in[i * BEAM_CHANNELS + c];
    }
    spectra(bf, spectrum);

    /* Broadband onset: speech starts across many bins at once. */
    double band = 0.0;
    for (int k = bf->bin_low; k <= bf->bin_high; k++)
        for (int c = 0; c < BEAM_CHANNELS; c++)
            band += crealf(spectrum[c][k] * conjf(spectrum[c][k]));
    bf->band_short = bf->alpha_short * bf->band_short + (1.0 - bf->alpha_short) * band;
    bf->band_slow = bf->alpha_slow * bf->band_slow + (1.0 - bf->alpha_slow) * band;
    bf->learn_gate = 1.0f - smoothstep((float)(bf->band_short / (bf->band_slow + 1e-9)),
                                       ONSET_FREE, ONSET_BLOCKED);
    float gate = bf->learn_gate;
    if (bf->mode == BEAM_FOCUS && !bf->frozen)
        gate = fminf(gate, 1.0f - update_presence(bf, spectrum, warmup));   /* never learn a talker as noise */
    if (bf->focus.new_background) gate = 1.0f;

    for (int k = 0; k < BEAM_BINS; k++) {
        cfloat x[BEAM_CHANNELS], y = 0, sum = 0;
        for (int c = 0; c < BEAM_CHANNELS; c++) x[c] = spectrum[c][k];
        if (k >= bf->bin_low && k <= bf->bin_high && !bf->frozen) {
            if (bf->mode == BEAM_FOCUS) learn_talker(bf, k, x);
            learn_background(bf, k, x, warmup, gate);
            if ((k + bf->frames) % EIGEN_INTERVAL == 0) refresh_bin(bf, k);
            notched += bf->strength[k][0] > 0.3f;
            focused += bf->focus.confidence[k] > 0.5f;
        }
        for (int c = 0; c < BEAM_CHANNELS; c++) {
            y += conjf(bf->weights[k][c]) * x[c];
            sum += x[c];
        }
        sum *= 1.0f / BEAM_CHANNELS;
        if (k >= bf->bin_low && k <= bf->bin_high) {
            energy_sum += crealf(sum * conjf(sum));
            energy_out += crealf(y * conjf(y));
        }
        output[k] = y;
    }
    for (int k = 1; k < BEAM_BINS - 1; k++) output[BEAM_FFT - k] = conjf(output[k]);
    output[0] = crealf(output[0]);
    output[BEAM_BINS - 1] = crealf(output[BEAM_BINS - 1]);
    fft_inverse(&bf->fft, output);

    for (int i = 0; i < BEAM_HOP; i++)
        out[i] = bf->overlap[i] + crealf(output[i]) * bf->window[i] / BEAM_FFT;
    for (int i = 0; i < BEAM_HOP; i++)
        bf->overlap[i] = crealf(output[BEAM_HOP + i]) * bf->window[BEAM_HOP + i] / BEAM_FFT;

    bf->energy_sum = 0.98 * bf->energy_sum + energy_sum;
    bf->energy_out = 0.98 * bf->energy_out + energy_out;
    if (bf->energy_out > 0 && bf->energy_sum > 0)
        bf->suppression_db = (float)(10.0 * log10(bf->energy_sum / bf->energy_out));
    bf->notched_fraction = (float)notched / (bf->bin_high - bf->bin_low + 1);
    bf->focused_fraction = (float)focused / (bf->bin_high - bf->bin_low + 1);
    bf->frames++;
}
