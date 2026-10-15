/*
 * Playback reference for the echo canceller.
 *
 * The supervisor connects a `parec` of the chosen sink's monitor (16 kHz mono
 * s16le) to a file descriptor. Samples arrive in bursts and on the sound
 * card's clock, so they pass a small jitter buffer first. The echo then
 * reaches the camera after the sink's buffering plus the acoustic path, often
 * more than the adaptive filter spans; the bulk delay is found by correlating
 * loudness envelopes of reference and microphone every two seconds.
 */
#ifndef PSVR_ECHO_REFERENCE_H
#define PSVR_ECHO_REFERENCE_H

#include <stdint.h>

#define REFERENCE_RATE 16000
#define REFERENCE_BLOCK 256
#define REFERENCE_FIFO 8192
#define REFERENCE_HISTORY 32768        /* > 2 s at 16 kHz, power of two */
#define REFERENCE_ENVELOPE 512         /* blocks of loudness history (~8 s) */

typedef struct {
    int fd;
    int connected;
    float fifo[REFERENCE_FIFO];
    int fifo_count;
    uint8_t odd_byte;
    int has_odd_byte;
    float history[REFERENCE_HISTORY];
    long written;
    int delay;                       /* samples the reference is held back */
    float envelope_ref[REFERENCE_ENVELOPE];
    float envelope_mic[REFERENCE_ENVELOPE];
    int envelope_pos;
    long blocks;
    int candidate;
    int candidate_hits;
    float correlation;
    long underruns;
    long overruns;
} echo_reference_t;

void echo_reference_init(echo_reference_t *ref, int fd);
/* Reads whatever the monitor delivered without blocking. */
void echo_reference_poll(echo_reference_t *ref);
/* Fills one block, already shifted by the current bulk delay. */
void echo_reference_block(echo_reference_t *ref, float *out);
/* Feeds loudness of the matching mic block; returns 1 if the delay changed. */
int echo_reference_track(echo_reference_t *ref, const float *mic_block);

#endif
