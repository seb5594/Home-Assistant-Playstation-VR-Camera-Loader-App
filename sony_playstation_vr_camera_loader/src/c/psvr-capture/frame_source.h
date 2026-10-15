/*
 * Raw camera frames, either straight from V4L2 memory-mapped buffers or from
 * a recorded file (used by the tests and for offline analysis).
 *
 * The V4L2 path reads the driver buffers in place. Only the audio columns and
 * the optional preview crop are touched, instead of piping 170 MB/s of video
 * through FFmpeg like earlier releases did.
 */
#ifndef PSVR_FRAME_SOURCE_H
#define PSVR_FRAME_SOURCE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *data;
    size_t bytes_used;
    uint32_t sequence;   /* driver frame counter; gaps mean dropped frames */
    double timestamp;    /* seconds on CLOCK_MONOTONIC */
    int damaged;         /* driver flagged the buffer as corrupted */
    int index;           /* internal buffer slot */
} frame_t;

typedef struct frame_source frame_source_t;

typedef struct {
    const char *device;  /* /dev/videoN, or a file path when from_file is set */
    int from_file;
    int width;
    int height;
    int fps;
    int buffers;         /* V4L2 buffers to allocate (each holds one frame) */
} frame_source_config_t;

frame_source_t *frame_source_open(const frame_source_config_t *config);
int frame_source_start(frame_source_t *source);
int frame_source_stop(frame_source_t *source);
int frame_source_streaming(const frame_source_t *source);
/* 1: frame delivered, 0: timeout, -1: device gone or end of file. */
int frame_source_next(frame_source_t *source, frame_t *frame, int timeout_ms);
void frame_source_release(frame_source_t *source, const frame_t *frame);
double frame_source_fps(const frame_source_t *source);
size_t frame_source_frame_bytes(const frame_source_t *source);
void frame_source_close(frame_source_t *source);

#endif
