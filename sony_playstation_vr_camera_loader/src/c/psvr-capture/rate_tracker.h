/*
 * Measures the real microphone sample rate against CLOCK_MONOTONIC.
 *
 * The camera crystal is not the host crystal: PSCam4Windows saw 60.029 fps
 * instead of 60. Uncorrected, 0.05 % adds up to almost two seconds of extra
 * latency per hour, or forced frame drops once buffers fill. The rate is
 * the slope of "samples received" over a 30 second window of driver
 * timestamps, which averages out USB jitter.
 */
#ifndef PSVR_RATE_TRACKER_H
#define PSVR_RATE_TRACKER_H

#define RATE_TRACKER_POINTS 32

typedef struct {
    double nominal;
    double rate;
    double total;                         /* samples counted so far */
    double times[RATE_TRACKER_POINTS];
    double totals[RATE_TRACKER_POINTS];
    int count;
    int head;
    double last_point;
} rate_tracker_t;

void rate_tracker_reset(rate_tracker_t *rt, double nominal_rate);
/* timestamp marks the start of the frame that delivered `samples`. */
void rate_tracker_add(rate_tracker_t *rt, double timestamp, double samples);
/* Silence inserted for dropped frames still counts towards the clock. */
void rate_tracker_add_gap(rate_tracker_t *rt, double samples);

#endif
