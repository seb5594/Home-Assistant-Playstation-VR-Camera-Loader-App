#include "rate_tracker.h"

#include <math.h>
#include <string.h>

static const double POINT_SPACING_S = 1.0;
static const double MIN_SPAN_S = 3.0;
static const double MAX_DEVIATION = 0.02;   /* beyond 2 % something else changed */

void rate_tracker_reset(rate_tracker_t *rt, double nominal_rate)
{
    memset(rt, 0, sizeof(*rt));
    rt->nominal = nominal_rate;
    rt->rate = nominal_rate;
    rt->last_point = -1e9;
}

static void record_point(rate_tracker_t *rt, double timestamp)
{
    rt->times[rt->head] = timestamp;
    rt->totals[rt->head] = rt->total;
    rt->head = (rt->head + 1) % RATE_TRACKER_POINTS;
    if (rt->count < RATE_TRACKER_POINTS) rt->count++;
    rt->last_point = timestamp;
}

/* Least-squares slope of samples over time across all stored points. */
static double fitted_rate(const rate_tracker_t *rt, double *span)
{
    double mean_t = 0, mean_s = 0, stt = 0, sts = 0;
    int first = (rt->head - rt->count + RATE_TRACKER_POINTS) % RATE_TRACKER_POINTS;

    for (int i = 0; i < rt->count; i++) {
        int k = (first + i) % RATE_TRACKER_POINTS;
        mean_t += rt->times[k];
        mean_s += rt->totals[k];
    }
    mean_t /= rt->count;
    mean_s /= rt->count;
    for (int i = 0; i < rt->count; i++) {
        int k = (first + i) % RATE_TRACKER_POINTS;
        double dt = rt->times[k] - mean_t;
        stt += dt * dt;
        sts += dt * (rt->totals[k] - mean_s);
    }
    *span = rt->times[(rt->head - 1 + RATE_TRACKER_POINTS) % RATE_TRACKER_POINTS] - rt->times[first];
    return stt > 0 ? sts / stt : rt->rate;
}

void rate_tracker_add(rate_tracker_t *rt, double timestamp, double samples)
{
    if (timestamp - rt->last_point >= POINT_SPACING_S) {
        double span;
        record_point(rt, timestamp);
        double measured = fitted_rate(rt, &span);
        if (span >= MIN_SPAN_S) {
            if (fabs(measured / rt->nominal - 1.0) > MAX_DEVIATION) {
                /* Layout or frame rate changed: start over around the new value. */
                rate_tracker_reset(rt, measured);
                record_point(rt, timestamp);
            } else {
                rt->rate = measured;
            }
        }
    }
    rt->total += samples;
}

void rate_tracker_add_gap(rate_tracker_t *rt, double samples) { rt->total += samples; }
