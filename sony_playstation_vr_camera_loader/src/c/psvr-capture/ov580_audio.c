#include "ov580_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

enum {
    PROBE_ROWS = 12000,        /* about half a second at 1280x800/30 fps */
    SIGNIFICANT = 8,           /* ignore near-zero values for equality tests */
    MIN_COMPARISONS = 2000,
};

/* Value of one capsule at one slot under the given ordering. */
static inline int16_t value_at(const int16_t *values, ov580_order_t order, int channel, int slot)
{
    return order == OV580_ORDER_CAPSULE ? values[channel * OV580_SLOTS + slot]
                                        : values[slot * OV580_CHANNELS + channel];
}

#define VALUE(values, channel, slot) value_at((values), order, (channel), (slot))

static void read_row(const uint8_t *row, int16_t *values)
{
    const uint8_t *p = row + OV580_ROW_HEADER_BYTES;
    for (int i = 0; i < OV580_CHANNELS * OV580_SLOTS; i++)
        values[i] = (int16_t)(p[2 * i] | (p[2 * i + 1] << 8));
}

/* Observed markers are 0x02, 0x20, 0x22, 0x44 and 0x88, always doubled. */
static int marker_valid(const uint8_t *row)
{
    return row[1] == row[2] && row[1] != 0;
}

/* First value of the row: the oldest sample of capsule 0 under both orderings. */
static float row_level(const int16_t *values)
{
    return values[0];
}

static int default_samples_per_row(int image_rows, double fps)
{
    /* Assume a 48 kHz converter until the data tells otherwise. */
    double rows_per_second = image_rows * fps;
    int guess = (int)lround(48000.0 / (rows_per_second > 1 ? rows_per_second : 1));
    if (guess < 1) guess = 1;
    if (guess > OV580_SLOTS) guess = OV580_SLOTS;
    return guess;
}

static void describe_layout(ov580_audio_t *audio, const char *note)
{
    const ov580_layout_t *l = &audio->layout;
    const char *order = l->order == OV580_ORDER_TIME ? ", interleaved" : "";
    if (l->kind == OV580_LAYOUT_SLIDING)
        snprintf(audio->summary, sizeof(audio->summary), "sliding window, %d new sample(s)/row%s%s%s",
                 l->samples_per_row, order, l->include_trailer ? ", trailer rows" : "", note);
    else if (l->dynamic)
        snprintf(audio->summary, sizeof(audio->summary), "packed, variable samples/row%s%s%s",
                 order, l->include_trailer ? ", trailer rows" : "", note);
    else
        snprintf(audio->summary, sizeof(audio->summary), "packed, %d sample(s)/row%s%s%s%s",
                 l->samples_per_row, l->stride > 1 ? " (repeated slots)" : "", order,
                 l->include_trailer ? ", trailer rows" : "", note);
}

int ov580_init(ov580_audio_t *audio, int width, int height, double fps)
{
    memset(audio, 0, sizeof(*audio));
    if (width * 2 < OV580_ROW_HEADER_BYTES + OV580_ROW_AUDIO_BYTES || height <= OV580_TRAILER_ROWS)
        return -1;
    audio->row_bytes = width * 2;
    audio->rows = height;
    audio->image_rows = height - OV580_TRAILER_ROWS;
    audio->expected_per_row = default_samples_per_row(audio->image_rows, fps);
    audio->probing = 1;
    audio->layout.kind = OV580_LAYOUT_PACKED;
    audio->layout.dynamic = 1;
    audio->layout.stride = 1;
    audio->layout.samples_per_row = audio->expected_per_row;
    audio->layout.check_markers = 1;
    describe_layout(audio, " (measuring)");
    return 0;
}

size_t ov580_max_samples_per_frame(const ov580_audio_t *audio)
{
    return (size_t)audio->rows * OV580_MAX_SAMPLES_PER_ROW;
}

void ov580_mark_discontinuity(ov580_audio_t *audio)
{
    audio->have_previous = 0;
}

/* ---- layout probe ------------------------------------------------------ */

static void probe_order(ov580_order_stats_t *s, ov580_order_t order,
                        const int16_t *row, const int16_t *previous)
{
    for (int k = 0; k < OV580_SLOTS; k++) {
        int active = 0, stale = previous != NULL, silent = 1;
        for (int c = 0; c < OV580_CHANNELS; c++) {
            int16_t v = VALUE(row, c, k);
            if (v) active = 1, silent = 0;
            if (previous && v != VALUE(previous, c, k)) stale = 0;
        }
        s->slot_active[k] += active;
        s->slot_stale[k] += stale && !silent;
    }
    for (int c = 0; c < OV580_CHANNELS; c++) {
        for (int k = 0; k + 1 < OV580_SLOTS; k++) {
            if (abs(VALUE(row, c, k)) < SIGNIFICANT) continue;
            s->neighbours_total[k]++;
            s->neighbours_equal[k] += VALUE(row, c, k) == VALUE(row, c, k + 1);
        }
        if (!previous) continue;
        for (int n = 1; n < OV580_SLOTS; n++) {
            for (int k = 0; k + n < OV580_SLOTS; k++) {
                /* newest-last: the window moved towards slot 0 */
                if (abs(VALUE(previous, c, k + n)) >= SIGNIFICANT) {
                    s->shift_total[0][n]++;
                    s->shift_equal[0][n] += VALUE(row, c, k) == VALUE(previous, c, k + n);
                }
                /* newest-first: the window moved towards slot 7 */
                if (abs(VALUE(previous, c, k)) >= SIGNIFICANT) {
                    s->shift_total[1][n]++;
                    s->shift_equal[1][n] += VALUE(row, c, k + n) == VALUE(previous, c, k);
                }
            }
        }
    }
}

static void probe_row(ov580_probe_t *p, const int16_t *row, const int16_t *previous)
{
    p->rows++;
    for (int order = 0; order < OV580_ORDERS; order++)
        probe_order(&p->order[order], (ov580_order_t)order, row, previous);
}

static double ratio(uint64_t part, uint64_t whole)
{
    return whole ? (double)part / (double)whole : 0.0;
}

static int repeat_stride(const ov580_order_stats_t *s)
{
    static const int strides[] = {8, 4, 2};
    for (size_t i = 0; i < sizeof(strides) / sizeof(strides[0]); i++) {
        int stride = strides[i], ok = 1;
        for (int k = 0; k + 1 < OV580_SLOTS && ok; k++) {
            if ((k + 1) % stride == 0) continue;  /* boundary between two samples */
            if (s->neighbours_total[k] < MIN_COMPARISONS / 8 ||
                ratio(s->neighbours_equal[k], s->neighbours_total[k]) < 0.8)
                ok = 0;
        }
        if (ok) return stride;
    }
    return 1;
}

typedef struct {
    int found;
    ov580_order_t order;
    int new_per_row;
    int newest_last;
    double match;
} window_candidate_t;

/*
 * A sliding window shows up as exact repeats of the previous row's values.
 * Interleaved frames sliding by one sample also look like a capsule-grouped
 * window sliding by four, so the candidate that leaves the fewest values
 * unexplained (smallest number of new samples) wins.
 */
static window_candidate_t best_window(const ov580_probe_t *p)
{
    window_candidate_t best = {0};
    for (int order = 0; order < OV580_ORDERS; order++) {
        const ov580_order_stats_t *s = &p->order[order];
        for (int dir = 0; dir < 2; dir++) {
            for (int n = 1; n < OV580_SLOTS; n++) {
                if (s->shift_total[dir][n] < MIN_COMPARISONS) continue;
                double r = ratio(s->shift_equal[dir][n], s->shift_total[dir][n]);
                if (r < 0.8) continue;
                if (!best.found || n < best.new_per_row || (n == best.new_per_row && r > best.match)) {
                    best.found = 1;
                    best.order = (ov580_order_t)order;
                    best.new_per_row = n;
                    best.newest_last = dir == 0;
                    best.match = r;
                }
            }
        }
    }
    return best;
}

typedef struct {
    int leading;       /* active, non-stale slots from slot 0 */
    int tail_silent;   /* all later slots stay zero */
} packing_t;

static packing_t packing_of(const ov580_order_stats_t *s, uint64_t rows)
{
    packing_t pk = {0, 1};
    while (pk.leading < OV580_SLOTS && ratio(s->slot_active[pk.leading], rows) > 0.5 &&
           ratio(s->slot_stale[pk.leading], rows) < 0.5)
        pk.leading++;
    for (int k = pk.leading; k < OV580_SLOTS; k++)
        if (ratio(s->slot_active[k], rows) > 0.05) pk.tail_silent = 0;
    return pk;
}

static void decide_layout(ov580_audio_t *audio)
{
    ov580_probe_t *p = &audio->probe;
    ov580_layout_t layout = audio->layout;
    double active_max = 0.0;

    for (int k = 0; k < OV580_SLOTS; k++)
        active_max = fmax(active_max, ratio(p->order[OV580_ORDER_CAPSULE].slot_active[k], p->rows));
    if (active_max < 0.05) {
        /* ADC not running yet: emit silence at the expected rate and keep watching. */
        layout.dynamic = 0;
        layout.samples_per_row = audio->expected_per_row;
        audio->layout = layout;
        describe_layout(audio, " (no microphone data yet)");
        memset(p, 0, sizeof(*p));
        return;
    }

    window_candidate_t window = best_window(p);
    int stride[OV580_ORDERS];
    for (int order = 0; order < OV580_ORDERS; order++) stride[order] = repeat_stride(&p->order[order]);

    layout.stride = 1;
    layout.order = OV580_ORDER_CAPSULE;
    if (stride[OV580_ORDER_CAPSULE] > 1 || stride[OV580_ORDER_TIME] > 1) {
        layout.order = stride[OV580_ORDER_CAPSULE] >= stride[OV580_ORDER_TIME] ? OV580_ORDER_CAPSULE
                                                                               : OV580_ORDER_TIME;
        layout.stride = stride[layout.order];
        layout.kind = OV580_LAYOUT_PACKED;
        layout.dynamic = 0;
        layout.samples_per_row = OV580_SLOTS / layout.stride;
    } else if (window.found) {
        layout.kind = OV580_LAYOUT_SLIDING;
        layout.order = window.order;
        layout.dynamic = 0;
        layout.samples_per_row = window.new_per_row;
        layout.newest_last = window.newest_last;
    } else {
        /* Prefer the ordering whose zero padding forms a clean tail. */
        packing_t capsule = packing_of(&p->order[OV580_ORDER_CAPSULE], p->rows);
        packing_t time = packing_of(&p->order[OV580_ORDER_TIME], p->rows);
        int use_time = time.tail_silent && time.leading > 0 && time.leading < OV580_SLOTS &&
                       !(capsule.tail_silent && capsule.leading > 0 && capsule.leading < OV580_SLOTS);
        packing_t chosen = use_time ? time : capsule;
        layout.order = use_time ? OV580_ORDER_TIME : OV580_ORDER_CAPSULE;
        layout.kind = OV580_LAYOUT_PACKED;
        layout.dynamic = chosen.tail_silent || chosen.leading == 0;
        layout.samples_per_row = chosen.leading ? chosen.leading : audio->expected_per_row;
    }

    /* Trailer rows count as audio only if they are live and continue the waveform. */
    if (p->steps_inner > 1000 && p->steps_entry >= 8 && p->steps_trailer >= 8) {
        double inner = p->step_inner / p->steps_inner;
        double entry = p->step_entry / p->steps_entry;
        double within = p->step_trailer / p->steps_trailer;
        layout.include_trailer = ratio(p->trailer_active, p->trailer_rows) > 0.5 &&
                                 entry < 1.5 * inner && within < 1.5 * inner && within > 0.2 * inner;
    }
    if (ratio(p->marker_bad, p->rows) > 0.2) layout.check_markers = 0;

    audio->layout = layout;
    audio->probing = 0;
    describe_layout(audio, "");
    const ov580_order_stats_t *s = &p->order[layout.order];
    log_info("Microphone layout: %s (slot activity %.2f/%.2f/%.2f/%.2f/%.2f/%.2f/%.2f/%.2f, "
             "window match %.2f, marker errors %.1f %%)", audio->summary,
             ratio(s->slot_active[0], p->rows), ratio(s->slot_active[1], p->rows),
             ratio(s->slot_active[2], p->rows), ratio(s->slot_active[3], p->rows),
             ratio(s->slot_active[4], p->rows), ratio(s->slot_active[5], p->rows),
             ratio(s->slot_active[6], p->rows), ratio(s->slot_active[7], p->rows),
             window.found ? window.match : 0.0, 100.0 * ratio(p->marker_bad, p->rows));
    if (!layout.check_markers)
        log_warning("Row header markers differ from known PS4 values; marker check disabled");
}

/* ---- extraction -------------------------------------------------------- */

static size_t emit(const int16_t *row, ov580_order_t order, int slot, float *out)
{
    for (int c = 0; c < OV580_CHANNELS; c++) out[c] = (float)VALUE(row, c, slot);
    return 1;
}

/*
 * How far the window moved since the previous row. When the row cannot tell
 * (a quiet room leaves too few significant values, or a value glitched), the
 * layout's usual step is used: re-emitting the whole window would duplicate
 * samples and make the measured rate drift upwards.
 */
static int window_shift(const int16_t *row, const int16_t *previous, ov580_order_t order,
                        int newest_last, int hint)
{
    for (int attempt = 0; attempt < OV580_SLOTS; attempt++) {
        int n = attempt == 0 ? hint : attempt;
        int compared = 0, equal = 0;
        if (attempt > 0 && attempt == hint) continue;
        for (int c = 0; c < OV580_CHANNELS; c++) {
            for (int k = 0; k + n < OV580_SLOTS; k++) {
                int16_t old = newest_last ? VALUE(previous, c, k + n) : VALUE(previous, c, k);
                int16_t now = newest_last ? VALUE(row, c, k) : VALUE(row, c, k + n);
                if (abs(old) < SIGNIFICANT) continue;
                compared++;
                equal += old == now;
            }
        }
        if (compared >= 4 && equal * 4 >= compared * 3) return n;
    }
    return hint;
}

static size_t emit_row(ov580_audio_t *audio, const int16_t *row, float *out)
{
    const ov580_layout_t *l = &audio->layout;
    const ov580_order_t order = l->order;
    size_t count = 0;

    if (l->kind == OV580_LAYOUT_SLIDING) {
        int n = audio->have_previous
            ? window_shift(row, audio->previous, order, l->newest_last, l->samples_per_row)
            : OV580_SLOTS;
        if (l->newest_last) {
            for (int k = OV580_SLOTS - n; k < OV580_SLOTS; k++)
                count += emit(row, order, k, out + count * OV580_CHANNELS);
        } else {
            for (int k = n - 1; k >= 0; k--)
                count += emit(row, order, k, out + count * OV580_CHANNELS);
        }
        return count;
    }

    int samples = l->samples_per_row;
    if (l->dynamic) {
        samples = 0;
        for (int k = OV580_SLOTS - 1; k >= 0 && !samples; k--)
            for (int c = 0; c < OV580_CHANNELS; c++)
                if (VALUE(row, c, k)) {
                    samples = k / l->stride + 1;
                    break;
                }
    }
    for (int i = 0; i < samples; i++)
        count += emit(row, order, i * l->stride, out + count * OV580_CHANNELS);
    return count;
}

size_t ov580_extract(ov580_audio_t *audio, const uint8_t *frame, size_t bytes_used,
                     float *out, size_t capacity)
{
    int available = (int)(bytes_used / (size_t)audio->row_bytes);
    int limit = audio->layout.include_trailer || audio->probing ? audio->rows : audio->image_rows;
    size_t produced = 0;
    int16_t row_values[OV580_CHANNELS * OV580_SLOTS];
    float previous_level = 0.0f;

    if (limit > available) limit = available;
    for (int r = 0; r < limit; r++) {
        const uint8_t *row = frame + (size_t)r * audio->row_bytes;
        int trailer = r >= audio->image_rows;

        read_row(row, row_values);
        if (audio->probing) {
            ov580_probe_t *p = &audio->probe;
            float level = row_level(row_values);
            p->marker_seen[row[1]]++;
            if (!trailer) {
                if (!marker_valid(row)) p->marker_bad++;
                probe_row(p, row_values, audio->have_previous ? audio->previous : NULL);
                if (r > 0) {
                    p->step_inner += fabsf(level - previous_level);
                    p->steps_inner++;
                }
            } else {
                int active = 0;
                for (int i = 0; i < OV580_CHANNELS * OV580_SLOTS; i++) active |= row_values[i] != 0;
                p->trailer_rows++;
                p->trailer_active += active;
                if (r == audio->image_rows) {
                    p->step_entry += fabsf(level - previous_level);
                    p->steps_entry++;
                } else {
                    p->step_trailer += fabsf(level - previous_level);
                    p->steps_trailer++;
                }
            }
            previous_level = level;
            if (trailer) continue;  /* the probe never emits trailer rows */
        }
        if (audio->layout.check_markers && !marker_valid(row)) {
            audio->rows_skipped++;
            audio->have_previous = 0;
            continue;
        }
        if (produced + OV580_MAX_SAMPLES_PER_ROW > capacity) break;
        produced += emit_row(audio, row_values, out + produced * OV580_CHANNELS);
        memcpy(audio->previous, row_values, sizeof(row_values));
        audio->have_previous = 1;
        audio->rows_parsed++;
    }
    audio->samples += produced;
    if (audio->probing && audio->probe.rows >= PROBE_ROWS) decide_layout(audio);
    return produced;
}
