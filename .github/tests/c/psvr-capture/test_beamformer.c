/*
 * Simulated room: a TV playing continuously off to one side and a talker
 * who starts a command later. Measures how much the adaptive beamformer
 * improves the talker-to-TV ratio over the plain capsule average.
 */
#include <string.h>

#include "beamformer.h"
#include "test_util.h"

enum { RATE = 16000, TAPS = 63, SECONDS = 16, SAMPLES = RATE * SECONDS };

static const double MIC_X[4] = {-0.075, -0.025, 0.025, 0.075};   /* metres, assumed geometry */
static const double SPEED_OF_SOUND = 343.0;

/* Band-limited noise with an optional syllable-like envelope. */
static double source_brightness = 0.35;

static double syllable_depth = 0.45;   /* envelope 0.55 +- depth: speech-like level changes */

static void make_source(float *out, int n, double syllable_hz, int start, int stop, double gain)
{
    double lp = 0, hp = 0, prev = 0;
    for (int i = 0; i < n; i++) {
        double x = gaussian();
        lp += source_brightness * (x - lp); /* one-pole low-pass, 0.35 ~ 0.9 kHz */
        hp = 0.97 * (hp + lp - prev);     /* ~80 Hz corner */
        prev = lp;
        double envelope = 1.0;
        if (syllable_hz > 0) envelope = 0.55 + syllable_depth * sin(2 * M_PI * syllable_hz * i / RATE);
        out[i] = (i >= start && i < stop) ? (float)(gain * hp * envelope) : 0.0f;
    }
}

/* Plane wave from azimuth (0 = straight ahead) onto the four capsules. */
static void place(const float *source, int n, double azimuth_deg, float *mics)
{
    for (int m = 0; m < 4; m++) {
        double delay = MIC_X[m] * sin(azimuth_deg * M_PI / 180.0) / SPEED_OF_SOUND * RATE;
        double taps[TAPS], center = TAPS / 2 + delay;
        for (int k = 0; k < TAPS; k++) {
            double x = k - center;
            double sinc = fabs(x) < 1e-9 ? 1.0 : sin(M_PI * x) / (M_PI * x);
            taps[k] = sinc * (0.5 - 0.5 * cos(2 * M_PI * (k + 0.5) / TAPS));
        }
        for (int i = 0; i < n; i++) {
            double acc = 0;
            for (int k = 0; k < TAPS; k++) {
                int j = i - k;
                if (j >= 0) acc += taps[k] * source[j];
            }
            mics[i * 4 + m] += (float)acc;
        }
    }
}

typedef struct { double talker, tv; } powers_t;

/*
 * Band power of x. Below ~1 kHz a 15 cm array barely resolves direction, so
 * the tests look at 300 Hz-7 kHz (overall) and 1-7 kHz (where nulls can work).
 */
static int band_low_hz = 300;

static double band_power(const float *x, int n)
{
    static fft_plan_t plan;
    static int ready;
    cfloat frame[512];
    double total = 0;
    if (!ready) ready = !fft_plan_init(&plan, 512);
    for (int start = 0; start + 512 <= n; start += 256) {
        for (int i = 0; i < 512; i++)
            frame[i] = x[start + i] * (0.5f - 0.5f * cosf(2 * (float)M_PI * i / 512));
        fft_forward(&plan, frame);
        for (int k = band_low_hz * 512 / RATE; k <= 7000 * 512 / RATE; k++)
            total += crealf(frame[k] * conjf(frame[k]));
    }
    return total;
}

/* Runs the mixture through `mode`, and the components through frozen copies. */
static powers_t evaluate(beam_mode_t mode, const float *mix, const float *talker, const float *tv,
                         int measure_from, int measure_to)
{
    static beamformer_t main_bf, talker_bf, tv_bf;
    static float talker_out[SAMPLES], tv_out[SAMPLES];
    float out[BEAM_HOP];
    powers_t p = {0, 0};

    beamformer_init(&main_bf, mode, RATE);
    beamformer_init(&talker_bf, mode, RATE);
    beamformer_init(&tv_bf, mode, RATE);
    talker_bf.frozen = tv_bf.frozen = 1;
    for (int start = 0; start + BEAM_HOP <= SAMPLES; start += BEAM_HOP) {
        beamformer_process(&main_bf, mix + start * 4, out);
        memcpy(talker_bf.weights, main_bf.weights, sizeof(main_bf.weights));
        memcpy(tv_bf.weights, main_bf.weights, sizeof(main_bf.weights));
        beamformer_process(&talker_bf, talker + start * 4, talker_out + start);
        beamformer_process(&tv_bf, tv + start * 4, tv_out + start);
    }
    p.talker = band_power(talker_out + measure_from, measure_to - measure_from);
    p.tv = band_power(tv_out + measure_from, measure_to - measure_from);
    beamformer_free(&main_bf);
    beamformer_free(&talker_bf);
    beamformer_free(&tv_bf);
    return p;
}

typedef struct { double sir_gain_db, talker_change_db, tv_change_db, upper_sir_gain_db; } result_t;

static result_t scenario(double tv_azimuth, double talker_azimuth, double tv_gain, int with_tv)
{
    static float talker_src[SAMPLES], tv_src[SAMPLES];
    static float talker[SAMPLES * 4], tv[SAMPLES * 4], mix[SAMPLES * 4];
    const int speech_start = 11 * RATE, speech_stop = 14 * RATE;

    test_rng = 777;
    memset(talker, 0, sizeof(talker));
    memset(tv, 0, sizeof(tv));
    make_source(talker_src, SAMPLES, 4.0, speech_start, speech_stop, 3000);
    make_source(tv_src, SAMPLES, 0.0, 0, with_tv ? SAMPLES : 0, tv_gain);
    place(talker_src, SAMPLES, talker_azimuth, talker);
    place(tv_src, SAMPLES, tv_azimuth, tv);
    for (int i = 0; i < SAMPLES * 4; i++)
        mix[i] = talker[i] + tv[i] + (float)(gaussian() * 15.0);   /* capsule self noise */

    /* outputs lag by one hop; skip the first 100 ms of the command */
    result_t r;
    for (int pass = 0; pass < 2; pass++) {
        band_low_hz = pass ? 1000 : 300;
        powers_t sum = evaluate(BEAM_SUM, mix, talker, tv, speech_start + RATE / 10, speech_stop);
        powers_t adaptive = evaluate(BEAM_ADAPTIVE, mix, talker, tv, speech_start + RATE / 10, speech_stop);
        double talker_change = 10 * log10(adaptive.talker / sum.talker);
        double tv_change = with_tv ? 10 * log10(adaptive.tv / sum.tv) : 0;
        if (pass) {
            r.upper_sir_gain_db = talker_change - tv_change;
        } else {
            r.talker_change_db = talker_change;
            r.tv_change_db = tv_change;
            r.sir_gain_db = talker_change - tv_change;
        }
    }
    band_low_hz = 300;
    return r;
}

static void test_tv_at_the_side_is_notched(void)
{
    result_t r = scenario(55, -10, 3000, 1);
    printf("    TV 55 deg, talker -10 deg: SIR %+.1f dB (talker %+.1f, TV %+.1f), 1-7 kHz %+.1f dB\n",
           r.sir_gain_db, r.talker_change_db, r.tv_change_db, r.upper_sir_gain_db);
    CHECK(r.sir_gain_db > 3.0, "SIR improvement %.1f dB", r.sir_gain_db);
    CHECK(r.upper_sir_gain_db > 10.0, "1-7 kHz SIR improvement %.1f dB", r.upper_sir_gain_db);
    CHECK(r.talker_change_db > -3.0, "talker loss %.1f dB", r.talker_change_db);
}

static void test_loud_tv_still_learned_during_command(void)
{
    result_t r = scenario(-40, 20, 6000, 1);
    printf("    TV -40 deg (+6 dB louder), talker 20 deg: SIR %+.1f dB (talker %+.1f, TV %+.1f), 1-7 kHz %+.1f dB\n",
           r.sir_gain_db, r.talker_change_db, r.tv_change_db, r.upper_sir_gain_db);
    CHECK(r.sir_gain_db > 2.0, "SIR improvement %.1f dB", r.sir_gain_db);
    CHECK(r.upper_sir_gain_db > 6.0, "1-7 kHz SIR improvement %.1f dB", r.upper_sir_gain_db);
}

static void test_quiet_room_leaves_talker_alone(void)
{
    result_t r = scenario(0, 30, 0, 0);
    printf("    no TV, talker 30 deg: talker %+.1f dB\n", r.talker_change_db);
    CHECK(fabs(r.talker_change_db) < 1.0, "talker changed by %.1f dB", r.talker_change_db);
}

static void test_broadside_guard_limits_damage(void)
{
    result_t r = scenario(0, 0, 3000, 1);
    printf("    TV and talker both ahead: talker %+.1f dB\n", r.talker_change_db);
    CHECK(r.talker_change_db > -3.6, "talker loss %.1f dB", r.talker_change_db);
}

static void test_reconstruction_is_transparent_without_notches(void)
{
    static beamformer_t bf;
    static float in[BEAM_HOP * 4 * 64];
    float out[BEAM_HOP], expected[BEAM_HOP * 64];
    double error = 0, energy = 0;

    beamformer_init(&bf, BEAM_ADAPTIVE, RATE);
    bf.frozen = 1;
    for (int i = 0; i < BEAM_HOP * 64; i++) {
        float v = (float)(1000 * sin(i * 0.05) + 300 * sin(i * 0.31));
        for (int c = 0; c < 4; c++) in[i * 4 + c] = v;
        expected[i] = v;
    }
    for (int b = 0; b < 64; b++) {
        beamformer_process(&bf, in + b * BEAM_HOP * 4, out);
        if (b < 4) continue;
        for (int i = 0; i < BEAM_HOP; i++) {
            /* one hop of latency */
            double d = out[i] - expected[(b - 1) * BEAM_HOP + i];
            error += d * d;
            energy += out[i] * out[i];
        }
    }
    beamformer_free(&bf);
    CHECK(10 * log10(error / energy) < -60, "reconstruction error %.1f dB", 10 * log10(error / energy));
}

/* ---- speech focus --------------------------------------------------------- */

#define NO_SOURCE (-999.0)

typedef struct {
    double tv_azimuth, tv2_azimuth;   /* tv2: second speaker of a stereo TV, or NO_SOURCE */
    double talker_azimuth;
    double tv_gain;
    int tv_dialogue;                  /* the TV carries speech-like level changes too */
    int wake_word;                    /* an earlier one-second utterance at 6 s */
    double tv_jump_db;                /* the TV gets louder at 8 s */
    double reverb;                    /* echo energy relative to the direct sound, 0 = free field */
    double fan_azimuth;               /* a fan / vacuum switched on at 4 s, or NO_SOURCE */
} room_t;

/*
 * Late reverberation: 240 sparse echoes per capsule between 5 and 400 ms with
 * an exponential decay (RT60 about 0.45 s), independent per capsule, so the
 * tail is diffuse like in a real room.
 */
static void reverberate(const float *source, int n, double energy, float *mics)
{
    enum { ECHOES = 240 };
    for (int m = 0; m < 4; m++) {
        int delay[ECHOES];
        double gain[ECHOES], total = 0;
        for (int e = 0; e < ECHOES; e++) {
            delay[e] = (int)(RATE * (0.005 + 0.395 * uniform()));
            gain[e] = gaussian() * exp(-6.9 * delay[e] / (0.45 * RATE));
            total += gain[e] * gain[e];
        }
        double scale = sqrt(energy / total);
        for (int e = 0; e < ECHOES; e++) {
            float g = (float)(gain[e] * scale);
            for (int i = delay[e]; i < n; i++) mics[i * 4 + m] += g * source[i - delay[e]];
        }
    }
}

typedef struct { double sir_db, talker_db; } mode_result_t;

/* SIR gain over the plain average for the command at 11-14 s, per mode. */
static void room_scenario(const room_t *room, mode_result_t result[3])
{
    static float talker_src[SAMPLES], tv_src[SAMPLES], tv2_src[SAMPLES];
    static float talker[SAMPLES * 4], tv[SAMPLES * 4], mix[SAMPLES * 4];
    const int command_start = 11 * RATE, command_stop = 14 * RATE;
    const beam_mode_t modes[3] = {BEAM_SUM, BEAM_ADAPTIVE, BEAM_FOCUS};

    test_rng = 4242;
    memset(talker, 0, sizeof(talker));
    memset(tv, 0, sizeof(tv));
    make_source(talker_src, SAMPLES, 4.0, command_start, command_stop, 3000);
    if (room->wake_word) {
        static float wake[SAMPLES];
        make_source(wake, SAMPLES, 4.0, 6 * RATE, 7 * RATE, 3000);
        for (int i = 0; i < SAMPLES; i++) talker_src[i] += wake[i];
    }
    syllable_depth = room->tv_dialogue ? 0.45 : 0.0;
    make_source(tv_src, SAMPLES, room->tv_dialogue ? 3.1 : 0.0, 0, room->tv_gain > 0 ? SAMPLES : 0, room->tv_gain);
    make_source(tv2_src, SAMPLES, room->tv_dialogue ? 2.3 : 0.0, 0,
                room->tv_gain > 0 && room->tv2_azimuth != NO_SOURCE ? SAMPLES : 0, room->tv_gain);
    syllable_depth = 0.45;
    if (room->tv_jump_db != 0) {
        double factor = pow(10.0, room->tv_jump_db / 20.0);
        for (int i = 8 * RATE; i < SAMPLES; i++) { tv_src[i] *= (float)factor; tv2_src[i] *= (float)factor; }
    }
    place(talker_src, SAMPLES, room->talker_azimuth, talker);
    place(tv_src, SAMPLES, room->tv_azimuth, tv);
    if (room->tv2_azimuth != NO_SOURCE) place(tv2_src, SAMPLES, room->tv2_azimuth, tv);
    if (room->reverb > 0) {
        reverberate(talker_src, SAMPLES, room->reverb, talker);
        reverberate(tv_src, SAMPLES, room->reverb, tv);
        if (room->tv2_azimuth != NO_SOURCE) reverberate(tv2_src, SAMPLES, room->reverb, tv);
    }
    if (room->fan_azimuth != NO_SOURCE) {      /* counts as interference, like the TV */
        static float fan[SAMPLES];
        source_brightness = 0.8;
        make_source(fan, SAMPLES, 0.0, 4 * RATE, SAMPLES, 2000);
        source_brightness = 0.35;
        place(fan, SAMPLES, room->fan_azimuth, tv);
    }
    for (int i = 0; i < SAMPLES * 4; i++) mix[i] = talker[i] + tv[i] + (float)(gaussian() * 15.0);

    powers_t p[3];
    for (int m = 0; m < 3; m++) p[m] = evaluate(modes[m], mix, talker, tv, command_start + RATE / 10, command_stop);
    for (int m = 0; m < 3; m++) {
        result[m].talker_db = 10 * log10(p[m].talker / p[0].talker);
        result[m].sir_db = p[0].tv > 0 ? result[m].talker_db - 10 * log10(p[m].tv / p[0].tv) : 0.0;
    }
}

static void report_room(const char *name, const room_t *room, mode_result_t r[3])
{
    room_scenario(room, r);
    printf("    %-44s SIR adaptive %+5.1f / focus %+5.1f dB, talker %+5.1f / %+5.1f dB\n", name,
           r[1].sir_db, r[2].sir_db, r[1].talker_db, r[2].talker_db);
}

static void test_focus_after_the_wake_word(void)
{
    mode_result_t r[3];
    room_t room = {55, NO_SOURCE, -10, 3000, 0, 1, 0, 0, NO_SOURCE};
    report_room("TV 55, talker -10, wake word first:", &room, r);
    CHECK(r[2].sir_db > r[1].sir_db + 1.0, "focus %.1f dB vs adaptive %.1f dB", r[2].sir_db, r[1].sir_db);
    CHECK(r[2].talker_db > -2.0, "talker loss %.1f dB", r[2].talker_db);
}

static void test_focus_hears_a_talker_beside_a_tv_in_front(void)
{
    mode_result_t r[3];
    room_t room = {0, NO_SOURCE, 60, 3000, 0, 1, 0, 0, NO_SOURCE};
    report_room("TV ahead, talker 60 deg:", &room, r);
    CHECK(r[2].sir_db > r[1].sir_db + 3.0, "focus %.1f dB vs adaptive %.1f dB", r[2].sir_db, r[1].sir_db);
    CHECK(r[2].talker_db > -2.0, "talker loss %.1f dB", r[2].talker_db);
}

static void test_focus_ignores_tv_dialogue(void)
{
    mode_result_t r[3];
    room_t room = {-35, NO_SOURCE, 30, 3000, 1, 1, 0, 0, NO_SOURCE};
    report_room("TV with dialogue -35, talker 30:", &room, r);
    CHECK(r[2].sir_db > r[1].sir_db, "focus %.1f dB vs adaptive %.1f dB", r[2].sir_db, r[1].sir_db);
    CHECK(r[2].talker_db > -2.0, "talker loss %.1f dB", r[2].talker_db);
}

static void test_focus_with_stereo_speakers(void)
{
    mode_result_t r[3];
    room_t room = {-50, 45, 5, 2200, 0, 1, 0, 0, NO_SOURCE};
    report_room("stereo TV -50/+45, talker 5:", &room, r);
    CHECK(r[2].sir_db > r[1].sir_db, "focus %.1f dB vs adaptive %.1f dB", r[2].sir_db, r[1].sir_db);
}

static void test_focus_survives_a_louder_tv_scene(void)
{
    mode_result_t r[3];
    room_t room = {40, NO_SOURCE, -25, 2500, 1, 1, 6.0, 0, NO_SOURCE};
    report_room("TV 40 jumps +6 dB at 8 s, talker -25:", &room, r);
    CHECK(r[2].sir_db > r[1].sir_db, "focus %.1f dB vs adaptive %.1f dB", r[2].sir_db, r[1].sir_db);
    CHECK(r[2].talker_db > -2.0, "talker loss %.1f dB", r[2].talker_db);
}

static void test_focus_in_a_reverberant_room(void)
{
    mode_result_t r[3];
    room_t room = {55, NO_SOURCE, -10, 3000, 1, 1, 0, 1.0, NO_SOURCE};
    report_room("reverberant room (echo = direct), TV 55:", &room, r);
    CHECK(r[2].sir_db > r[1].sir_db, "focus %.1f dB vs adaptive %.1f dB", r[2].sir_db, r[1].sir_db);
    CHECK(r[2].talker_db > -2.0, "talker loss %.1f dB", r[2].talker_db);
}

static void test_focus_learns_a_fan_as_background(void)
{
    /* the fan starts at 4 s and never pauses: it must end up notched, not followed */
    mode_result_t r[3];
    room_t room = {55, NO_SOURCE, -10, 2000, 0, 1, 0, 0, -60};
    report_room("TV 55 + fan -60 from 4 s, talker -10:", &room, r);
    CHECK(r[2].sir_db > r[1].sir_db, "focus %.1f dB vs adaptive %.1f dB", r[2].sir_db, r[1].sir_db);
}

static void test_focus_without_wake_word_is_no_worse(void)
{
    mode_result_t r[3];
    room_t room = {55, NO_SOURCE, -10, 3000, 0, 0, 0, 0, NO_SOURCE};
    report_room("TV 55, talker -10, first words cold:", &room, r);
    CHECK(r[2].sir_db > r[1].sir_db - 1.0, "focus %.1f dB vs adaptive %.1f dB", r[2].sir_db, r[1].sir_db);
}

static void test_focus_talker_in_line_with_the_tv(void)
{
    /* nothing spatial can separate them; focus must not make it worse */
    mode_result_t r[3];
    room_t room = {0, NO_SOURCE, 0, 3000, 1, 1, 0, 0, NO_SOURCE};
    report_room("talker standing in front of the TV:", &room, r);
    CHECK(r[2].talker_db > -3.6, "talker loss %.1f dB", r[2].talker_db);
}

static void test_focus_quiet_room_leaves_talker_alone(void)
{
    mode_result_t r[3];
    room_t room = {0, NO_SOURCE, 30, 0, 0, 1, 0, 0, NO_SOURCE};
    report_room("no TV, talker 30 deg:", &room, r);
    CHECK(fabs(r[2].talker_db) < 1.5, "talker changed by %.1f dB", r[2].talker_db);
}

static void report_bands(void)
{
    const double brightness[] = {0.35, 0.7, 0.95};
    for (int i = 0; i < 3; i++) {
        source_brightness = brightness[i];
        result_t r = scenario(55, -10, 3000, 1);
        printf("    brightness %.2f: SIR %+.1f dB (talker %+.1f, TV %+.1f), 1-7 kHz %+.1f dB\n", brightness[i],
               r.sir_gain_db, r.talker_change_db, r.tv_change_db, r.upper_sir_gain_db);
    }
    source_brightness = 0.35;
}

int main(int argc, char **argv)
{
    if (argc > 1) { report_bands(); return 0; }
    printf("beamformer\n");
    RUN(test_reconstruction_is_transparent_without_notches);
    RUN(test_tv_at_the_side_is_notched);
    RUN(test_loud_tv_still_learned_during_command);
    RUN(test_quiet_room_leaves_talker_alone);
    RUN(test_broadside_guard_limits_damage);
    printf("  speech focus (command at 11-14 s, gain over the plain average)\n");
    RUN(test_focus_after_the_wake_word);
    RUN(test_focus_hears_a_talker_beside_a_tv_in_front);
    RUN(test_focus_ignores_tv_dialogue);
    RUN(test_focus_with_stereo_speakers);
    RUN(test_focus_survives_a_louder_tv_scene);
    RUN(test_focus_in_a_reverberant_room);
    RUN(test_focus_learns_a_fan_as_background);
    RUN(test_focus_without_wake_word_is_no_worse);
    RUN(test_focus_talker_in_line_with_the_tv);
    RUN(test_focus_quiet_room_leaves_talker_alone);
    return test_failures ? 1 : 0;
}
