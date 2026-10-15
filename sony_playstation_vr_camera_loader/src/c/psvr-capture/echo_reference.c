#include "echo_reference.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"

enum {
    FIFO_TARGET = 2 * REFERENCE_BLOCK,
    FIFO_LIMIT = 8 * REFERENCE_BLOCK,     /* 128 ms backlog: the monitor runs ahead */
    ESTIMATE_EVERY = 125,                 /* blocks, about 2 s */
    MAX_LAG = 94,                         /* blocks, about 1.5 s */
    FILTER_MARGIN = 4,                    /* blocks of pre-echo kept inside the filter */
};

void echo_reference_init(echo_reference_t *ref, int fd)
{
    memset(ref, 0, sizeof(*ref));
    ref->fd = fd;
    ref->connected = fd >= 0;
    if (fd >= 0) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static void append(echo_reference_t *ref, float sample)
{
    if (ref->fifo_count == REFERENCE_FIFO) {
        memmove(ref->fifo, ref->fifo + REFERENCE_BLOCK, (REFERENCE_FIFO - REFERENCE_BLOCK) * sizeof(float));
        ref->fifo_count -= REFERENCE_BLOCK;
    }
    ref->fifo[ref->fifo_count++] = sample;
}

void echo_reference_poll(echo_reference_t *ref)
{
    uint8_t bytes[4096];

    while (ref->connected) {
        ssize_t got = read(ref->fd, bytes, sizeof(bytes));
        if (got < 0 && errno == EINTR) continue;
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (got <= 0) {
            log_warning("Echo reference stream ended; echo cancellation paused");
            ref->connected = 0;
            break;
        }
        ssize_t i = 0;
        if (ref->has_odd_byte) {
            append(ref, (float)(int16_t)(ref->odd_byte | (bytes[0] << 8)));
            ref->has_odd_byte = 0;
            i = 1;
        }
        for (; i + 1 < got; i += 2) append(ref, (float)(int16_t)(bytes[i] | (bytes[i + 1] << 8)));
        if (i < got) {
            ref->odd_byte = bytes[i];
            ref->has_odd_byte = 1;
        }
    }
    if (ref->fifo_count > FIFO_LIMIT) {
        int drop = ref->fifo_count - FIFO_TARGET;
        memmove(ref->fifo, ref->fifo + drop, FIFO_TARGET * sizeof(float));
        ref->fifo_count = FIFO_TARGET;
        ref->overruns++;
    }
}

void echo_reference_block(echo_reference_t *ref, float *out)
{
    int fresh = ref->fifo_count < REFERENCE_BLOCK ? ref->fifo_count : REFERENCE_BLOCK;

    if (fresh < REFERENCE_BLOCK && ref->connected) ref->underruns++;
    for (int i = 0; i < REFERENCE_BLOCK; i++) {
        float sample = i < fresh ? ref->fifo[i] : 0.0f;
        ref->history[(ref->written + i) & (REFERENCE_HISTORY - 1)] = sample;
    }
    memmove(ref->fifo, ref->fifo + fresh, (ref->fifo_count - fresh) * sizeof(float));
    ref->fifo_count -= fresh;
    ref->written += REFERENCE_BLOCK;

    long start = ref->written - REFERENCE_BLOCK - ref->delay;
    for (int i = 0; i < REFERENCE_BLOCK; i++)
        out[i] = start + i < 0 ? 0.0f : ref->history[(start + i) & (REFERENCE_HISTORY - 1)];
}

static float loudness(const float *block)
{
    double energy = 0.0;
    for (int i = 0; i < REFERENCE_BLOCK; i++) energy += (double)block[i] * block[i];
    return (float)log10(energy / REFERENCE_BLOCK + 1.0);
}

static int estimate_lag(const echo_reference_t *ref, float *best_correlation)
{
    const int window = REFERENCE_ENVELOPE - MAX_LAG;
    int best_lag = -1;
    *best_correlation = 0.0f;

    for (int lag = 0; lag <= MAX_LAG; lag++) {
        double sm = 0, sr = 0, smm = 0, srr = 0, smr = 0;
        for (int t = 0; t < window; t++) {
            int mic_index = (ref->envelope_pos - 1 - t + 2 * REFERENCE_ENVELOPE) % REFERENCE_ENVELOPE;
            int ref_index = (mic_index - lag + REFERENCE_ENVELOPE) % REFERENCE_ENVELOPE;
            double m = ref->envelope_mic[mic_index], r = ref->envelope_ref[ref_index];
            sm += m, sr += r, smm += m * m, srr += r * r, smr += m * r;
        }
        double cov = smr - sm * sr / window;
        double var = (smm - sm * sm / window) * (srr - sr * sr / window);
        if (var <= 1e-9) continue;
        float corr = (float)(cov / sqrt(var));
        if (corr > *best_correlation) {
            *best_correlation = corr;
            best_lag = lag;
        }
    }
    return best_lag;
}

int echo_reference_track(echo_reference_t *ref, const float *mic_block)
{
    /* The fresh (undelayed) reference block is the newest history entry. */
    float fresh[REFERENCE_BLOCK];
    for (int i = 0; i < REFERENCE_BLOCK; i++)
        fresh[i] = ref->history[(ref->written - REFERENCE_BLOCK + i) & (REFERENCE_HISTORY - 1)];
    ref->envelope_ref[ref->envelope_pos] = loudness(fresh);
    ref->envelope_mic[ref->envelope_pos] = loudness(mic_block);
    ref->envelope_pos = (ref->envelope_pos + 1) % REFERENCE_ENVELOPE;
    ref->blocks++;

    if (ref->blocks < REFERENCE_ENVELOPE || ref->blocks % ESTIMATE_EVERY) return 0;
    float correlation;
    int lag = estimate_lag(ref, &correlation);
    ref->correlation = correlation;
    if (lag < 0 || correlation < 0.5f) {
        ref->candidate_hits = 0;
        return 0;
    }
    if (ref->candidate_hits && abs(lag - ref->candidate) <= 1) ref->candidate_hits++;
    else ref->candidate = lag, ref->candidate_hits = 1;
    if (ref->candidate_hits < 2) return 0;

    int delay = (ref->candidate > FILTER_MARGIN ? ref->candidate - FILTER_MARGIN : 0) * REFERENCE_BLOCK;
    if (abs(delay - ref->delay) <= 2 * REFERENCE_BLOCK) return 0;
    log_info("Echo path delay about %d ms (envelope correlation %.2f); realigning reference",
             ref->candidate * REFERENCE_BLOCK * 1000 / REFERENCE_RATE, correlation);
    ref->delay = delay;
    return 1;
}
