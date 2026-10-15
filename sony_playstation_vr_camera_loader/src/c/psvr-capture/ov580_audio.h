/*
 * Microphone samples hidden in the PlayStation Camera (OV580) video rows.
 *
 * Every raw row starts with a 32 byte header followed by 64 audio bytes:
 * 32 little-endian 16-bit values, i.e. four capsules times eight slots. Two
 * things are undocumented: whether the values are grouped per capsule
 * (c0 c0 ... c1 c1 ...) or per time step (c0 c1 c2 c3, c0 c1 c2 c3, ...),
 * and how many slots are new in each row. Instead of hard coding a guess,
 * the parser measures the first half second of rows and picks the layout
 * that explains the data with the fewest new values per row.
 */
#ifndef PSVR_OV580_AUDIO_H
#define PSVR_OV580_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#define OV580_CHANNELS 4
#define OV580_SLOTS 8
#define OV580_ROW_HEADER_BYTES 32
#define OV580_ROW_AUDIO_BYTES 64
#define OV580_TRAILER_ROWS 8
#define OV580_MAX_SAMPLES_PER_ROW OV580_SLOTS

typedef enum {
    OV580_LAYOUT_PACKED,   /* fresh samples in slots 0..n-1, oldest first */
    OV580_LAYOUT_SLIDING,  /* row repeats the previous window shifted by n */
} ov580_layout_kind_t;

typedef enum {
    OV580_ORDER_CAPSULE,   /* value index = capsule * 8 + slot */
    OV580_ORDER_TIME,      /* value index = slot * 4 + capsule (interleaved frames) */
    OV580_ORDERS
} ov580_order_t;

typedef struct {
    ov580_layout_kind_t kind;
    ov580_order_t order;
    int samples_per_row;   /* fixed count; ignored for dynamic packing */
    int stride;            /* packed: distance between fresh slots (repeats skipped) */
    int dynamic;           /* packed: count the non-silent slots in each row */
    int newest_last;       /* sliding: new samples enter at the high slots */
    int include_trailer;   /* the 8 rows after the image also carry audio */
    int check_markers;     /* skip rows whose header marker looks broken */
} ov580_layout_t;

/* Statistics gathered once per value ordering. */
typedef struct {
    uint64_t slot_active[OV580_SLOTS];
    uint64_t slot_stale[OV580_SLOTS];
    uint64_t neighbours_equal[OV580_SLOTS], neighbours_total[OV580_SLOTS];
    uint64_t shift_equal[2][OV580_SLOTS], shift_total[2][OV580_SLOTS];
} ov580_order_stats_t;

typedef struct {
    uint64_t rows;
    ov580_order_stats_t order[OV580_ORDERS];
    double step_inner, step_entry, step_trailer;
    uint64_t steps_inner, steps_entry, steps_trailer;
    uint64_t trailer_rows, trailer_active;
    uint64_t marker_bad;
    uint32_t marker_seen[256];
} ov580_probe_t;

typedef struct {
    int row_bytes;
    int rows;              /* rows in a complete frame */
    int image_rows;        /* rows that belong to the picture (rows - trailer) */
    int expected_per_row;  /* guess from geometry and fps until the probe ends */
    int probing;
    ov580_layout_t layout;
    ov580_probe_t probe;
    int16_t previous[OV580_CHANNELS * OV580_SLOTS];
    int have_previous;
    uint64_t rows_parsed;
    uint64_t rows_skipped;
    uint64_t samples;
    char summary[160];
} ov580_audio_t;

/* width/height are the raw V4L2 geometry, e.g. 3448x808. */
int ov580_init(ov580_audio_t *audio, int width, int height, double fps);

/*
 * Appends interleaved float samples (channel-major per sample) to out and
 * returns how many sample frames were written. Rows beyond bytes_used are
 * treated as missing, so short or damaged buffers still yield their audio.
 */
size_t ov580_extract(ov580_audio_t *audio, const uint8_t *frame, size_t bytes_used,
                     float *out, size_t capacity);

/* Called after a dropped buffer so the sliding-window state restarts cleanly. */
void ov580_mark_discontinuity(ov580_audio_t *audio);

/* Upper bound of sample frames one buffer can produce. */
size_t ov580_max_samples_per_frame(const ov580_audio_t *audio);

#endif
