/* Layout detection and lossless extraction for every plausible slot layout. */
#include <stdint.h>
#include <string.h>

#include "ov580_audio.h"
#include "test_util.h"

enum { WIDTH = 3448, HEIGHT = 808, ROW_BYTES = WIDTH * 2, IMAGE_ROWS = 800 };

typedef enum {
    PACKED_TWO, SLIDING_ONE, REPEATED, FIFO_ALL_ROWS, PACKED_FOUR_STALE,
    INTERLEAVED_SLIDING_ONE, INTERLEAVED_SLIDING_TWO, INTERLEAVED_PACKED_TWO, SLIDING_FOUR,
} layout_t;

static int quiet_stretches;   /* truth() falls almost silent every third block of samples */

typedef struct {
    layout_t layout;
    long position;        /* next fresh sample index */
    int16_t window[4][8]; /* sliding window state */
    double fifo_credit;
} generator_t;

static int16_t truth(int channel, long t)
{
    if (quiet_stretches && (t / 64) % 3 == 0) return (int16_t)(1 + (t + channel) % 3);
    double v = 3000 * sin(2 * M_PI * 440.0 * t / 48000.0 + channel) +
               1200 * sin(2 * M_PI * 1730.0 * t / 48000.0) + 40 * sin(t * 0.7 + channel);
    int16_t s = (int16_t)lround(v);
    return s ? s : 1;   /* real ADC noise is never exactly zero on all capsules */
}

/* Capsule-grouped layout: c0 s0..s7, c1 s0..s7, ... */
static void put(uint8_t *row, int channel, int slot, int16_t value)
{
    uint8_t *p = row + 32 + (channel * 8 + slot) * 2;
    p[0] = (uint8_t)(value & 0xFF);
    p[1] = (uint8_t)((uint16_t)value >> 8);
}

/* Interleaved layout: t0 c0..c3, t1 c0..c3, ... */
static void put_frame(uint8_t *row, int channel, int slot, int16_t value)
{
    uint8_t *p = row + 32 + (slot * 4 + channel) * 2;
    p[0] = (uint8_t)(value & 0xFF);
    p[1] = (uint8_t)((uint16_t)value >> 8);
}

static int fill_row(generator_t *g, uint8_t *row, int trailer)
{
    memset(row + 32, 0, 64);
    row[1] = row[2] = 0x22;
    if (trailer && g->layout != FIFO_ALL_ROWS) {
        for (int c = 0; c < 4; c++)
            for (int k = 0; k < 8; k++) put(row, c, k, (int16_t)(uniform() * 65535 - 32768));
        return 0;
    }
    switch (g->layout) {
    case PACKED_TWO:
        for (int k = 0; k < 2; k++, g->position++)
            for (int c = 0; c < 4; c++) put(row, c, k, truth(c, g->position));
        return 2;
    case PACKED_FOUR_STALE:
        /* four fresh slots, the rest keep whatever the previous row had */
        for (int k = 0; k < 4; k++, g->position++)
            for (int c = 0; c < 4; c++) put(row, c, k, truth(c, g->position));
        for (int c = 0; c < 4; c++)
            for (int k = 4; k < 8; k++) put(row, c, k, (int16_t)(1000 + c * 10 + k));
        return 4;
    case REPEATED:
        for (int c = 0; c < 4; c++)
            for (int k = 0; k < 8; k++) put(row, c, k, truth(c, g->position));
        g->position++;
        return 1;
    case SLIDING_ONE:
        for (int c = 0; c < 4; c++) {
            memmove(g->window[c], g->window[c] + 1, 7 * sizeof(int16_t));
            g->window[c][7] = truth(c, g->position);
            for (int k = 0; k < 8; k++) put(row, c, k, g->window[c][k]);
        }
        g->position++;
        return 1;
    case INTERLEAVED_SLIDING_ONE:
    case INTERLEAVED_SLIDING_TWO: {
        int step = g->layout == INTERLEAVED_SLIDING_ONE ? 1 : 2;
        for (int c = 0; c < 4; c++) {
            for (int i = 0; i < step; i++) {
                memmove(g->window[c], g->window[c] + 1, 7 * sizeof(int16_t));
                g->window[c][7] = truth(c, g->position + i);
            }
            for (int k = 0; k < 8; k++) put_frame(row, c, k, g->window[c][k]);
        }
        g->position += step;
        return step;
    }
    case SLIDING_FOUR:
        /* capsule-grouped window of 8 that moves on by four samples every row */
        for (int c = 0; c < 4; c++) {
            memmove(g->window[c], g->window[c] + 4, 4 * sizeof(int16_t));
            for (int i = 0; i < 4; i++) g->window[c][4 + i] = truth(c, g->position + i);
            for (int k = 0; k < 8; k++) put(row, c, k, g->window[c][k]);
        }
        g->position += 4;
        return 4;
    case INTERLEAVED_PACKED_TWO:
        for (int k = 0; k < 2; k++, g->position++)
            for (int c = 0; c < 4; c++) put_frame(row, c, k, truth(c, g->position));
        return 2;
    case FIFO_ALL_ROWS: {
        g->fifo_credit += 48000.0 / (HEIGHT * 30.0);
        int n = (int)g->fifo_credit;
        g->fifo_credit -= n;
        for (int k = 0; k < n; k++, g->position++)
            for (int c = 0; c < 4; c++) put(row, c, k, truth(c, g->position));
        return n;
    }
    }
    return 0;
}

static void run_layout(layout_t layout, const char *expect_kind, int expect_trailer)
{
    static uint8_t frame[ROW_BYTES * HEIGHT];
    static float out[HEIGHT * 8 * 4];
    generator_t g = {.layout = layout};
    ov580_audio_t parser;
    int verified_frames = 0, mismatches = 0;

    test_rng = 12345;
    if (layout == SLIDING_FOUR) {
        for (int c = 0; c < 4; c++)
            for (int k = 0; k < 8; k++) g.window[c][k] = truth(c, k - 8);
    }
    if (layout == SLIDING_ONE || layout == INTERLEAVED_SLIDING_ONE || layout == INTERLEAVED_SLIDING_TWO) {
        /* prime the window so the first row already holds real history */
        for (int i = 0; i < 7; i++)
            for (int c = 0; c < 4; c++) {
                memmove(g.window[c], g.window[c] + 1, 7 * sizeof(int16_t));
                g.window[c][7] = truth(c, g.position);
            }
    }
    CHECK(ov580_init(&parser, WIDTH, HEIGHT, 30.0) == 0, "init");
    for (int f = 0; f < 30; f++) {
        long frame_start = g.position;
        for (int r = 0; r < HEIGHT; r++) fill_row(&g, frame + (size_t)r * ROW_BYTES, r >= IMAGE_ROWS);
        int was_probing = parser.probing;
        size_t n = ov580_extract(&parser, frame, sizeof(frame), out, HEIGHT * 8);
        if (was_probing) continue;
        long expected = g.position - frame_start;
        if ((long)n != expected) {
            mismatches++;
            fprintf(stderr, "    frame %d: %zu samples, expected %ld\n", f, n, expected);
            continue;
        }
        for (size_t i = 0; i < n; i++)
            for (int c = 0; c < 4; c++)
                if (out[i * 4 + c] != truth(c, frame_start + (long)i)) mismatches++;
        verified_frames++;
    }
    CHECK(!parser.probing, "probe finished");
    CHECK(strstr(parser.summary, expect_kind) != NULL, "layout '%s' detected as '%s'", expect_kind, parser.summary);
    CHECK(parser.layout.include_trailer == expect_trailer, "trailer decision %d for %s",
          parser.layout.include_trailer, parser.summary);
    CHECK(verified_frames >= 10, "verified %d frames", verified_frames);
    CHECK(mismatches == 0, "%d sample mismatches for %s", mismatches, parser.summary);
}

static void test_packed_two_samples_per_row(void) { run_layout(PACKED_TWO, "packed", 0); }
static void test_sliding_window_one_new_sample(void) { run_layout(SLIDING_ONE, "sliding window", 0); }
static void test_repeated_slots_give_one_sample(void) { run_layout(REPEATED, "repeated", 0); }
static void test_fifo_layout_uses_trailer_rows(void) { run_layout(FIFO_ALL_ROWS, "packed", 1); }
static void test_stale_tail_slots_are_ignored(void) { run_layout(PACKED_FOUR_STALE, "4 sample(s)/row", 0); }

/* What the PS4 camera most likely sends at 60 fps: 8 interleaved frames, one new per row. */
static void test_interleaved_window_one_new_sample(void) { run_layout(INTERLEAVED_SLIDING_ONE, "sliding window, 1 new sample(s)/row, interleaved", 0); }
static void test_interleaved_window_two_new_samples(void) { run_layout(INTERLEAVED_SLIDING_TWO, "sliding window, 2 new sample(s)/row, interleaved", 0); }
static void test_interleaved_packing(void) { run_layout(INTERLEAVED_PACKED_TWO, "interleaved", 0); }

/*
 * What alpha15 reported on real hardware: a capsule-grouped window moving by
 * four per row. In a quiet room many rows have too few significant values to
 * prove the shift; they must still give four samples, not the whole window.
 */
static void test_quiet_rows_keep_the_window_step(void)
{
    quiet_stretches = 1;
    run_layout(SLIDING_FOUR, "sliding window, 4 new sample(s)/row", 0);
    quiet_stretches = 0;
}

static void test_short_buffer_keeps_received_rows(void)
{
    static uint8_t frame[ROW_BYTES * HEIGHT];
    static float out[HEIGHT * 8 * 4];
    ov580_audio_t parser;
    generator_t g = {.layout = PACKED_TWO};

    ov580_init(&parser, WIDTH, HEIGHT, 30.0);
    for (int r = 0; r < HEIGHT; r++) fill_row(&g, frame + (size_t)r * ROW_BYTES, r >= IMAGE_ROWS);
    size_t n = ov580_extract(&parser, frame, (size_t)ROW_BYTES * 500 + 17, out, HEIGHT * 8);
    CHECK(n == 1000, "500 complete rows give 1000 samples, got %zu", n);
}

int main(void)
{
    printf("ov580_audio\n");
    RUN(test_packed_two_samples_per_row);
    RUN(test_sliding_window_one_new_sample);
    RUN(test_repeated_slots_give_one_sample);
    RUN(test_fifo_layout_uses_trailer_rows);
    RUN(test_stale_tail_slots_are_ignored);
    RUN(test_interleaved_window_one_new_sample);
    RUN(test_interleaved_window_two_new_samples);
    RUN(test_interleaved_packing);
    RUN(test_quiet_rows_keep_the_window_step);
    RUN(test_short_buffer_keeps_received_rows);
    return test_failures ? 1 : 0;
}
