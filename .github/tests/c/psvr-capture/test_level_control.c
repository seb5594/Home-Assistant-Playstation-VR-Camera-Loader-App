/*
 * Speech detector and automatic gain.
 *
 * The rooms are generated block by block: steady noise at a given level,
 * optionally with "speech" (noise shaped by a 4 Hz syllable envelope) on top,
 * and optionally with blocks of digital silence such as the capture engine
 * inserts for frames lost on USB.
 */
#include <string.h>

#include "level_control.h"
#include "pipeline.h"
#include "test_util.h"

enum { RATE = 16000, BLOCK = 256 };
static const double BLOCK_SECONDS = (double)BLOCK / RATE;

typedef struct {
    double noise_dbfs;
    double speech_dbfs;          /* level of a speech burst; 0 disables speech */
    double speech_on_seconds, speech_off_seconds;
    int lost_every;              /* every n-th block has a lost frame in it; 0 disables */
} room_t;

typedef struct {
    long blocks, speech_blocks;
    long quiet_blocks, false_speech;     /* blocks without speech, flagged as speech */
    long talk_blocks, heard;             /* blocks inside bursts, flagged as speech */
} tally_t;

static double amplitude(double dbfs) { return 32768.0 * pow(10.0, dbfs / 20.0); }

/* Is "speech" playing at time t, and how loud is its envelope? */
static double speech_envelope(const room_t *room, double t)
{
    if (room->speech_dbfs >= 0) return 0.0;
    double period = room->speech_on_seconds + room->speech_off_seconds;
    double in_period = fmod(t, period);
    if (in_period >= room->speech_on_seconds) return 0.0;
    double syllables = 0.5 + 0.5 * sin(2 * M_PI * 4.0 * in_period);     /* 0 .. 1 */
    return amplitude(room->speech_dbfs) * sqrt(2.0) * syllables;
}

/*
 * Returns 1 for blocks touched by a lost frame. As in the real chain, the gap
 * is half a block of silence followed by a block the filters have smeared.
 */
static int make_block(const room_t *room, long index, float *x)
{
    double noise = amplitude(room->noise_dbfs);
    for (int i = 0; i < BLOCK; i++) {
        double t = (index * BLOCK + i) / (double)RATE;
        x[i] = (float)(noise * gaussian() + speech_envelope(room, t) * gaussian());
    }
    if (!room->lost_every) return 0;
    int phase = (int)(index % room->lost_every);
    if (phase == 0) memset(x + BLOCK / 2, 0, BLOCK / 2 * sizeof(float));
    if (phase == 1)
        for (int i = 0; i < BLOCK; i++) x[i] *= 0.3f;
    return phase <= 1;
}

/* Runs the room for `seconds`; blocks before `settle` seconds are not counted. */
static tally_t run_room(level_control_t *lc, const room_t *room, double seconds, double settle, long *clock)
{
    tally_t tally = {0};
    float x[BLOCK];
    long blocks = (long)(seconds / BLOCK_SECONDS);
    for (long b = 0; b < blocks; b++, (*clock)++) {
        double t = *clock * BLOCK_SECONDS;
        int gap = make_block(room, *clock, x);
        int talking = speech_envelope(room, t + 0.5 * BLOCK_SECONDS) > 0;
        level_control_process(lc, x, BLOCK, -1.0f, gap);
        if (b * BLOCK_SECONDS < settle) continue;
        tally.blocks++;
        tally.speech_blocks += lc->speech;
        /* the detector may still hold speech for its hangover after a burst */
        double since_burst = room->speech_dbfs < 0
            ? fmod(t, room->speech_on_seconds + room->speech_off_seconds) - room->speech_on_seconds : 1e9;
        if (talking) {
            tally.talk_blocks++;
            tally.heard += lc->speech;
        } else if (since_burst > 0.3 || since_burst < 0) {
            tally.quiet_blocks++;
            tally.false_speech += lc->speech;
        }
    }
    return tally;
}

static double percent(long part, long whole) { return whole ? 100.0 * part / whole : 0.0; }

static void test_steady_noise_is_not_speech(void)
{
    static level_control_t lc;
    long clock = 0;
    level_control_init(&lc, 1, RATE, BLOCK);
    room_t room = {.noise_dbfs = -40};
    tally_t t = run_room(&lc, &room, 20, 2, &clock);
    printf("       noise -40 dBFS: speech %.1f %% of blocks, floor %.1f dBFS, gain %+.1f dB\n",
           percent(t.speech_blocks, t.blocks), level_control_noise_floor_dbfs(&lc), level_control_gain_db(&lc));
    CHECK(percent(t.speech_blocks, t.blocks) < 1.0, "steady noise flagged as speech");
    CHECK(fabs(level_control_noise_floor_dbfs(&lc) + 40.5) < 2.0, "floor %.1f", level_control_noise_floor_dbfs(&lc));
}

static void test_lost_frames_do_not_fake_speech(void)
{
    /* The old detector followed the running minimum: one silent block pulled the
     * floor to zero and it took minutes to recover, so the room "spoke" all the time. */
    static level_control_t lc;
    long clock = 0;
    level_control_init(&lc, 1, RATE, BLOCK);
    room_t room = {.noise_dbfs = -40, .lost_every = 6};
    tally_t t = run_room(&lc, &room, 20, 2, &clock);
    printf("       noise with a lost frame every 6th block: speech %.1f %%, gain %+.1f dB\n",
           percent(t.speech_blocks, t.blocks), level_control_gain_db(&lc));
    CHECK(percent(t.speech_blocks, t.blocks) < 1.0, "silent gaps made noise look like speech");
    CHECK(level_control_gain_db(&lc) < 0.5, "noise was amplified");
}

static void test_speech_bursts_are_detected(void)
{
    static level_control_t lc;
    long clock = 0;
    level_control_init(&lc, 1, RATE, BLOCK);
    room_t room = {.noise_dbfs = -55, .speech_dbfs = -35, .speech_on_seconds = 1.5, .speech_off_seconds = 2.5};
    tally_t t = run_room(&lc, &room, 40, 5, &clock);
    printf("       speech 20 dB over noise: heard %.0f %% of speech blocks, %.1f %% false in pauses\n",
           percent(t.heard, t.talk_blocks), percent(t.false_speech, t.quiet_blocks));
    CHECK(percent(t.heard, t.talk_blocks) > 70.0, "speech missed");
    CHECK(percent(t.false_speech, t.quiet_blocks) < 2.0, "pauses flagged as speech");
}

static void test_louder_background_becomes_the_floor(void)
{
    static level_control_t lc;
    long clock = 0;
    level_control_init(&lc, 1, RATE, BLOCK);
    room_t quiet = {.noise_dbfs = -60}, tv = {.noise_dbfs = -45};
    run_room(&lc, &quiet, 10, 0, &clock);
    run_room(&lc, &tv, 6, 0, &clock);                     /* the TV is switched on */
    tally_t later = run_room(&lc, &tv, 10, 0, &clock);
    printf("       steady TV 15 dB louder: speech %.1f %% after 6 s, floor %.1f dBFS\n",
           percent(later.speech_blocks, later.blocks), level_control_noise_floor_dbfs(&lc));
    CHECK(percent(later.speech_blocks, later.blocks) < 1.0, "the new background still counts as speech");
}

static void test_gain_lifts_distant_speech_in_a_quiet_room(void)
{
    static level_control_t lc;
    long clock = 0;
    level_control_init(&lc, 1, RATE, BLOCK);
    room_t room = {.noise_dbfs = -75, .speech_dbfs = -42, .speech_on_seconds = 2, .speech_off_seconds = 2};
    run_room(&lc, &room, 30, 0, &clock);
    printf("       quiet room, speech at -42 dBFS: gain %+.1f dB\n", level_control_gain_db(&lc));
    CHECK(level_control_gain_db(&lc) > 15.0, "gain %.1f dB", level_control_gain_db(&lc));
    CHECK(level_control_gain_db(&lc) <= 18.01, "gain above the maximum");
}

static void test_gain_never_lifts_noise_over_the_ceiling(void)
{
    static level_control_t lc;
    long clock = 0;
    level_control_init(&lc, 1, RATE, BLOCK);
    room_t room = {.noise_dbfs = -58, .speech_dbfs = -40, .speech_on_seconds = 2, .speech_off_seconds = 2};
    run_room(&lc, &room, 30, 0, &clock);
    double floor_after_gain = level_control_noise_floor_dbfs(&lc) + level_control_gain_db(&lc);
    printf("       noise -58 dBFS: gain %+.1f dB, noise reaches Assist at %.1f dBFS\n",
           level_control_gain_db(&lc), floor_after_gain);
    CHECK(floor_after_gain < -49.0, "noise lifted to %.1f dBFS", floor_after_gain);
    CHECK(level_control_gain_db(&lc) > 5.0, "speech should still get some gain");
}

static void test_gain_eases_down_when_the_room_gets_louder(void)
{
    static level_control_t lc;
    long clock = 0;
    level_control_init(&lc, 1, RATE, BLOCK);
    room_t quiet = {.noise_dbfs = -75, .speech_dbfs = -42, .speech_on_seconds = 2, .speech_off_seconds = 2};
    room_t loud = {.noise_dbfs = -45};
    run_room(&lc, &quiet, 30, 0, &clock);
    run_room(&lc, &loud, 10, 0, &clock);
    printf("       after a fan starts at -45 dBFS: gain %+.1f dB\n", level_control_gain_db(&lc));
    CHECK(level_control_gain_db(&lc) < 1.0, "gain %.1f dB still amplifies the fan", level_control_gain_db(&lc));
}

static void test_digital_silence_is_never_speech(void)
{
    static level_control_t lc;
    float x[BLOCK] = {0};
    level_control_init(&lc, 1, RATE, BLOCK);
    long speech = 0;
    for (int b = 0; b < 400; b++) {
        level_control_process(&lc, x, BLOCK, -1.0f, 0);
        speech += lc.speech;
    }
    CHECK(speech == 0, "silence flagged as speech");
    CHECK(level_control_gain_db(&lc) < 0.01, "gain moved on silence");
}

/* The whole chain: lost frames arrive as exact zeros on all four capsules. */
static void test_pipeline_ignores_lost_frames(void)
{
    static pipeline_t p;
    pipeline_config_t config = {BEAM_FOCUS, 0, 75, 1, -1};
    float in[PIPELINE_BLOCK * PIPELINE_CHANNELS], out[PIPELINE_BLOCK], raw[PIPELINE_BLOCK];
    long speech = 0, counted = 0;
    double noise = amplitude(-40);

    CHECK(pipeline_init(&p, &config) == 0, "init");
    for (long b = 0; b < (long)(20 / BLOCK_SECONDS); b++) {
        for (int i = 0; i < PIPELINE_BLOCK * PIPELINE_CHANNELS; i++) in[i] = (float)(noise * gaussian());
        if (b % 8 == 0)                       /* one 60 fps frame: 267 samples of silence */
            memset(in + PIPELINE_BLOCK / 2 * PIPELINE_CHANNELS, 0, PIPELINE_BLOCK / 2 * PIPELINE_CHANNELS * sizeof(float));
        if (b % 8 == 1) memset(in, 0, 11 * PIPELINE_CHANNELS * sizeof(float));
        pipeline_process(&p, in, out, raw);
        if (b * BLOCK_SECONDS >= 3) speech += p.level.speech, counted++;
    }
    printf("       full chain, noise -40 dBFS and a lost frame every 8th block: speech %.1f %%, gain %+.1f dB\n",
           percent(speech, counted), level_control_gain_db(&p.level));
    CHECK(percent(speech, counted) < 1.0, "lost frames made noise look like speech");
    CHECK(level_control_gain_db(&p.level) < 0.5, "noise was amplified");
    pipeline_free(&p);
}

int main(void)
{
    printf("level_control\n");
    RUN(test_steady_noise_is_not_speech);
    RUN(test_lost_frames_do_not_fake_speech);
    RUN(test_speech_bursts_are_detected);
    RUN(test_louder_background_becomes_the_floor);
    RUN(test_gain_lifts_distant_speech_in_a_quiet_room);
    RUN(test_gain_never_lifts_noise_over_the_ceiling);
    RUN(test_gain_eases_down_when_the_room_gets_louder);
    RUN(test_digital_silence_is_never_speech);
    RUN(test_pipeline_ignores_lost_frames);
    return test_failures ? 1 : 0;
}
