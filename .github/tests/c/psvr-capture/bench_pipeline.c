/*
 * CPU cost of the engine with frames already in memory (as with V4L2 mmap),
 * for the system requirements in DOCS.md.
 *
 *   make bench
 *
 * Audio: the layout the PS4 camera reported on real hardware in profile
 * High (3448x808 @ 60 fps, every row a window of 8 values per capsule that
 * moves on by four, about 194 kHz), with a talker over a steady noise source
 * so the speech focus does its full work.
 * Preview: one JPEG per call with all picture processing on.
 */
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio_engine.h"
#include "preview.h"
#include "test_util.h"

enum { WIDTH = 3448, HEIGHT = 808, FPS = 60, SECONDS = 20, NEW_PER_ROW = 4 };

static uint8_t frames[FPS][WIDTH * 2 * HEIGHT];

static double cpu_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* One second of frames: a steady source and an on/off talker, capsule-grouped window. */
static void synthesise(void)
{
    int16_t window[4][8] = {{0}};
    long t = 0;
    for (int f = 0; f < FPS; f++) {
        for (int r = 0; r < HEIGHT; r++) {
            uint8_t *row = frames[f] + (size_t)r * WIDTH * 2;
            row[1] = row[2] = 0x22;
            for (int c = 0; c < 4; c++) {
                memmove(window[c], window[c] + NEW_PER_ROW, (8 - NEW_PER_ROW) * sizeof(int16_t));
                for (int i = 0; i < NEW_PER_ROW; i++) {
                    long n = t + i;
                    double talker = (n / 40000) % 2 ? 2500 * sin(n * 0.031 + c * 0.4) * sin(n * 0.00013) : 0;
                    window[c][8 - NEW_PER_ROW + i] = (int16_t)(gaussian() * 900 + talker);
                }
                for (int k = 0; k < 8; k++) {
                    uint8_t *p = row + 32 + (c * 8 + k) * 2;
                    p[0] = (uint8_t)window[c][k];
                    p[1] = (uint8_t)((uint16_t)window[c][k] >> 8);
                }
            }
            t += NEW_PER_ROW;
            uint8_t *image = row + 96;     /* some picture content for the preview */
            for (int x = 0; x < 2560; x += 2) image[x] = (uint8_t)(40 + (x / 16 + r / 8) % 120), image[x + 1] = 128;
        }
    }
}

static double run_audio(beam_mode_t beam, int rnnoise, int auto_gain, int reference)
{
    static audio_engine_t engine;
    pipeline_config_t config = {beam, rnnoise, 75, auto_gain, -1};
    int sink = open("/dev/null", O_WRONLY), fds[2] = {-1, -1};

    if (reference && pipe(fds) == 0) {
        config.reference_fd = fds[0];
        fcntl(fds[1], F_SETFL, O_NONBLOCK);
    }
    audio_engine_init(&engine, &config, WIDTH, HEIGHT, FPS, sink);
    double start = cpu_seconds();
    for (int f = 0; f < SECONDS * FPS; f++) {
        frame_t fr = {.data = frames[f % FPS], .bytes_used = sizeof(frames[0]), .sequence = (uint32_t)f,
                      .timestamp = (double)f / FPS};
        if (reference) {
            int16_t playback[16000 / FPS];
            for (size_t i = 0; i < sizeof(playback) / sizeof(playback[0]); i++) playback[i] = (int16_t)(gaussian() * 500);
            if (write(fds[1], playback, sizeof(playback)) < 0) { /* full pipe: fine for a benchmark */ }
        }
        audio_engine_frame(&engine, &fr);
    }
    double used = cpu_seconds() - start;
    audio_engine_free(&engine);
    close(sink);
    if (fds[0] >= 0) close(fds[0]), close(fds[1]);
    return 1000.0 * used / SECONDS;   /* ms of CPU per second */
}

static double run_preview(const char *preset)
{
    preview_t pv;
    char path[] = "/tmp/psvr-bench-still.jpg";
    if (preview_init(&pv, WIDTH, HEIGHT, preset, 1000, path, NULL)) return -1;
    int n = 100;
    double start = cpu_seconds();
    for (int i = 0; i < n; i++) preview_offer(&pv, frames[i % FPS], sizeof(frames[0]), i + 1.0);
    double used = cpu_seconds() - start;
    preview_free(&pv);
    unlink(path);
    return 1000.0 * used / n;          /* ms per picture */
}

int main(void)
{
    synthesise();
    printf("Audio, ms of CPU per second (= tenths of a percent of one core), 3448x808 @ 60 fps:\n");
    printf("  sum                                  %6.1f\n", run_audio(BEAM_SUM, 0, 1, 0));
    printf("  adaptive                             %6.1f\n", run_audio(BEAM_ADAPTIVE, 0, 1, 0));
    printf("  focus                                %6.1f\n", run_audio(BEAM_FOCUS, 0, 1, 0));
    printf("  focus + RNNoise                      %6.1f\n", run_audio(BEAM_FOCUS, 1, 1, 0));
    printf("  focus + RNNoise + echo cancellation  %6.1f\n", run_audio(BEAM_FOCUS, 1, 1, 1));
    printf("Preview, ms of CPU per picture:\n");
    const char *presets[] = {"native", "hd", "sd", "low"};
    for (int i = 0; i < 4; i++) printf("  %-6s                               %6.2f\n", presets[i], run_preview(presets[i]));
    return 0;
}
