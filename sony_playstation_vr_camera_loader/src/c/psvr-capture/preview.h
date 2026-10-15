/*
 * Still images for the ingress preview, MQTT camera and go2rtc.
 *
 * The left sensor image is cropped and box-scaled straight out of the raw
 * YUYV buffer into planar 4:2:0 and handed to TurboJPEG. On the way it gets
 * what a webcam ISP would normally do:
 *
 *   - limited-range YUV (16..235) expanded to full range, so black is black
 *   - auto levels: black and white points follow the scene
 *   - grey-world white balance, which removes the camera's green cast
 *   - saturation, gamma, contrast and brightness from the live controls
 *   - motion-adaptive temporal noise filter against low-light "fog"
 *
 * Controls live in <run dir>/video.json and are re-read whenever it changes.
 */
#ifndef PSVR_PREVIEW_H
#define PSVR_PREVIEW_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    int brightness;      /* -100..100 */
    int contrast;        /* percent, 50..200 */
    int gamma;           /* percent, 50..250 */
    int saturation;      /* percent, 0..400 */
    int auto_levels;     /* 0/1 */
    int white_balance;   /* 0/1 */
    int denoise;         /* 0 off, 1 light, 2 strong */
} picture_tuning_t;

void picture_tuning_defaults(picture_tuning_t *tuning);
/* Reads the flat JSON written by the web UI; missing keys keep their value. */
int picture_tuning_load(picture_tuning_t *tuning, const char *path);

typedef struct {
    int row_bytes;
    int image_offset;                  /* bytes before the picture in each row */
    int sensor_width, sensor_height;   /* left image inside the raw frame */
    int crop_x, crop_y, crop_width, crop_height;
    int scale;
    int width, height;                 /* JPEG size */
    picture_tuning_t tuning;
    char tuning_path[512];
    struct timespec tuning_mtime;      /* identifies the loaded file version */
    unsigned long tuning_inode;        /* the web UI replaces the file on every save */
    /* scene statistics, smoothed over frames */
    float black, white;                /* luma levels in the source */
    float exposure_gamma;              /* automatic shadow lift, >= 1 */
    float shift_u, shift_v;            /* white balance correction */
    int have_stats;
    uint8_t *planes;                   /* converted frame */
    uint8_t *history;                  /* previous filtered frame (denoise) */
    int have_history;
    void *encoder;
    int quality;
    double interval;
    double next_due;
    long published;
    char path[512];
    char temporary[520];
} preview_t;

/*
 * preset: native | hd | sd | low. Understands the OV580 raw layout (header,
 * audio, both eyes, pyramid) and plain side-by-side or single images.
 */
int preview_init(preview_t *pv, int raw_width, int raw_height, const char *preset, int fps,
                 const char *path, const char *tuning_path);
void preview_free(preview_t *pv);
/* Re-reads the tuning file if it changed; cheap enough to call every second. */
void preview_refresh_tuning(preview_t *pv);
/* Forget the previous frame and scene statistics, e.g. after the stream was paused. */
void preview_restart(preview_t *pv);
/* Encodes when the frame interval has elapsed; returns 1 if a JPEG was written. */
int preview_offer(preview_t *pv, const uint8_t *frame, size_t bytes_used, double now);

#endif
