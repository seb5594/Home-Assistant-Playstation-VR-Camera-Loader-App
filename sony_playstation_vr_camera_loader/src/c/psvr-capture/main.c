/*
 * psvr-capture: the only process that opens the PlayStation Camera.
 *
 * It streams raw frames, turns the microphone rows into 16 kHz voice PCM on
 * stdout and writes preview JPEGs while someone is watching. All other
 * services (web UI, MQTT camera, go2rtc) read its files in /run/ps-camera.
 */
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio_engine.h"
#include "control.h"
#include "frame_source.h"
#include "log.h"
#include "preview.h"

enum {
    STATUS_INTERVAL_S = 1,
    PREVIEW_REQUEST_AGE_S = 10,
    IDLE_STOP_S = 20,            /* camera-only mode: stop USB streaming when nobody watches */
    STALL_LIMIT_S = 5,
    ROWS_DUMP_SECONDS = 2,
};

typedef struct {
    frame_source_config_t source;
    int audio;
    pipeline_config_t pipeline;
    const char *preview_preset;
    int preview_fps;
    const char *run_dir;
} options_t;

static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static double monotonic_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void usage(FILE *out, const char *program)
{
    fprintf(out,
            "Usage: %s --device /dev/videoN [options]\n"
            "  --geometry WxH           stream size, e.g. raw PS4 3448x808 / 1748x408 / 898x200,\n"
            "                           raw PS5 5148x1088 or a plain mode such as 3840x1080\n"
            "  --fps N                  camera frame rate\n"
            "  --buffers N              V4L2 buffers (default 4)\n"
            "  --file                   read raw frames from the --device path instead\n"
            "  --audio                  decode the microphones to 16 kHz PCM on stdout\n"
            "  --beamformer sum|adaptive|focus\n"
            "  --noise-suppression off|rnnoise\n"
            "  --noise-strength 0-100\n"
            "  --auto-gain\n"
            "  --echo-reference-fd N    16 kHz mono s16le playback reference\n"
            "  --preview native|hd|sd|low  write still.jpg while requested\n"
            "  --preview-fps N          preview frame rate limit\n"
            "  --run-dir DIR            status and request files (default /run/ps-camera);\n"
            "                           picture controls are read from DIR/video.json\n",
            program);
}

static int parse_options(int argc, char **argv, options_t *o)
{
    static const struct option longs[] = {
        {"device", required_argument, 0, 'd'}, {"geometry", required_argument, 0, 'g'},
        {"fps", required_argument, 0, 'f'}, {"buffers", required_argument, 0, 'b'},
        {"file", no_argument, 0, 'F'}, {"audio", no_argument, 0, 'a'},
        {"beamformer", required_argument, 0, 'B'}, {"noise-suppression", required_argument, 0, 'n'},
        {"noise-strength", required_argument, 0, 's'}, {"auto-gain", no_argument, 0, 'G'},
        {"echo-reference-fd", required_argument, 0, 'e'}, {"preview", required_argument, 0, 'p'},
        {"preview-fps", required_argument, 0, 'r'}, {"run-dir", required_argument, 0, 'R'},
        {"help", no_argument, 0, 'h'}, {0, 0, 0, 0},
    };
    int opt;

    memset(o, 0, sizeof(*o));
    o->source.width = 3448;
    o->source.height = 808;
    o->source.fps = 30;
    o->source.buffers = 4;
    o->pipeline.beam_mode = BEAM_SUM;
    o->pipeline.noise_strength = 75;
    o->pipeline.reference_fd = -1;
    o->preview_fps = 5;
    o->run_dir = "/run/ps-camera";

    while ((opt = getopt_long(argc, argv, "", longs, NULL)) != -1) {
        switch (opt) {
        case 'd': o->source.device = optarg; break;
        case 'g':
            if (sscanf(optarg, "%dx%d", &o->source.width, &o->source.height) != 2) return -1;
            break;
        case 'f': o->source.fps = atoi(optarg); break;
        case 'b': o->source.buffers = atoi(optarg); break;
        case 'F': o->source.from_file = 1; break;
        case 'a': o->audio = 1; break;
        case 'B': o->pipeline.beam_mode = beamformer_mode_from_name(optarg); break;
        case 'n': o->pipeline.noise_suppression = !strcmp(optarg, "rnnoise"); break;
        case 's': o->pipeline.noise_strength = atoi(optarg); break;
        case 'G': o->pipeline.auto_gain = 1; break;
        case 'e': o->pipeline.reference_fd = atoi(optarg); break;
        case 'p': o->preview_preset = optarg; break;
        case 'r': o->preview_fps = atoi(optarg); break;
        case 'R': o->run_dir = optarg; break;
        case 'h': return 1;
        default: return -1;
        }
    }
    if (!o->source.device || o->source.fps < 1 || o->source.buffers < 2) return -1;
    if (!o->audio && !o->preview_preset) return -1;
    return 0;
}

/* ---- raw row dump for layout analysis ---------------------------------- */

typedef struct {
    FILE *file;
    long frames_left;
} rows_dump_t;

static void rows_dump_start(rows_dump_t *dump, const options_t *o, double fps)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/rows.bin.tmp", o->run_dir);
    dump->file = fopen(path, "wb");
    if (!dump->file) return;
    /* header: magic, width, height, fps*1000, bytes kept per row */
    int32_t header[4] = {o->source.width, o->source.height, (int32_t)(fps * 1000),
                         OV580_ROW_HEADER_BYTES + OV580_ROW_AUDIO_BYTES};
    fwrite("PSVRROW1", 1, 8, dump->file);
    fwrite(header, sizeof(header), 1, dump->file);
    dump->frames_left = (long)(fps * ROWS_DUMP_SECONDS);
    log_info("Recording %d s of raw microphone rows for analysis", ROWS_DUMP_SECONDS);
}

static void rows_dump_frame(rows_dump_t *dump, const options_t *o, const frame_t *frame)
{
    const int row_bytes = o->source.width * 2, keep = OV580_ROW_HEADER_BYTES + OV580_ROW_AUDIO_BYTES;
    int32_t info[2] = {(int32_t)frame->sequence, (int32_t)frame->bytes_used};
    uint8_t empty[OV580_ROW_HEADER_BYTES + OV580_ROW_AUDIO_BYTES] = {0};

    fwrite(info, sizeof(info), 1, dump->file);
    for (int r = 0; r < o->source.height; r++) {
        size_t offset = (size_t)r * row_bytes;
        fwrite(offset + keep <= frame->bytes_used ? frame->data + offset : empty, 1, keep, dump->file);
    }
    if (--dump->frames_left > 0) return;
    char from[512], to[512];
    fclose(dump->file);
    dump->file = NULL;
    snprintf(from, sizeof(from), "%s/rows.bin.tmp", o->run_dir);
    snprintf(to, sizeof(to), "%s/rows.bin", o->run_dir);
    rename(from, to);
    control_request_done(o->run_dir, "rows.request");
    log_info("Raw microphone rows saved for download");
}

/* ---- status ------------------------------------------------------------ */

/* Preview frames actually written per second, measured between status updates. */
typedef struct {
    long published;
    double since;
    double fps;
} preview_rate_t;

static void preview_rate_update(preview_rate_t *rate, const preview_t *preview, double now)
{
    double elapsed = now - rate->since;
    if (rate->since > 0 && elapsed > 0) rate->fps = (preview->published - rate->published) / elapsed;
    rate->published = preview->published;
    rate->since = now;
}

static void write_status(const options_t *o, frame_source_t *source, audio_engine_t *audio,
                         long frames, const preview_t *preview, int preview_active, double preview_fps)
{
    char audio_json[2048] = "null", preview_json[256] = "null", json[3072];

    if (o->audio) audio_engine_status(audio, audio_json, sizeof(audio_json));
    if (o->preview_preset)
        snprintf(preview_json, sizeof(preview_json),
                 "{\"active\":%s,\"size\":\"%dx%d\",\"fps_limit\":%d,\"fps\":%.1f,\"published\":%ld}",
                 preview_active ? "true" : "false", preview->width, preview->height,
                 o->preview_fps, preview_active ? preview_fps : 0.0, preview->published);
    int length = snprintf(json, sizeof(json),
        "{\"updated\":%ld,\"device\":\"%s\",\"geometry\":\"%dx%d\",\"fps\":%.2f,"
        "\"streaming\":%s,\"frames\":%ld,\"audio\":%s,\"preview\":%s}\n",
        (long)time(NULL), o->source.device, o->source.width, o->source.height,
        frame_source_fps(source), frame_source_streaming(source) ? "true" : "false",
        frames, audio_json, preview_json);
    if (length > 0 && length < (int)sizeof(json))
        control_write_atomic(o->run_dir, "engine.json", json, (size_t)length);
}

/* ---- main loop --------------------------------------------------------- */

int main(int argc, char **argv)
{
    options_t o;
    audio_engine_t *audio = NULL;
    preview_t preview;
    rows_dump_t dump = {0};
    preview_rate_t preview_rate = {0};
    int preview_enabled = 0, preview_active = 0, exit_code = 0;
    long frames_received = 0;
    double last_status = 0, last_frame = monotonic_now(), last_viewer = monotonic_now();
    char still_path[512], tuning_path[512];

    log_init("Capture");
    int parsed = parse_options(argc, argv, &o);
    if (parsed) {
        usage(parsed > 0 ? stdout : stderr, argv[0]);
        return parsed > 0 ? 0 : 64;
    }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    frame_source_t *source = frame_source_open(&o.source);
    if (!source) return 2;
    double fps = frame_source_fps(source);

    if (o.audio) {
        audio = calloc(1, sizeof(*audio));
        if (!audio || audio_engine_init(audio, &o.pipeline, o.source.width, o.source.height, fps,
                                        STDOUT_FILENO)) {
            log_error("Audio pipeline could not be initialized");
            return 3;
        }
        char noise[24] = "off";
        if (o.pipeline.noise_suppression) snprintf(noise, sizeof(noise), "RNNoise %d %%", o.pipeline.noise_strength);
        log_info("Voice chain: beamformer %s, noise reduction %s, auto gain %s, echo cancellation %s",
                 beamformer_mode_name(o.pipeline.beam_mode), noise,
                 o.pipeline.auto_gain ? "on" : "off", o.pipeline.reference_fd >= 0 ? "on" : "off");
    }
    if (o.preview_preset) {
        snprintf(still_path, sizeof(still_path), "%s/still.jpg", o.run_dir);
        snprintf(tuning_path, sizeof(tuning_path), "%s/video.json", o.run_dir);
        preview_enabled = preview_init(&preview, o.source.width, o.source.height, o.preview_preset,
                                       o.preview_fps, still_path, tuning_path) == 0;
    }
    if (!preview_enabled) memset(&preview, 0, sizeof(preview));

    /* Audio needs the stream all the time; video alone starts on demand. */
    if (o.audio && frame_source_start(source)) return 2;

    while (!stop_requested) {
        double now = monotonic_now();

        if (now - last_status >= STATUS_INTERVAL_S) {
            last_status = now;
            int was_active = preview_active;
            preview_active = preview_enabled &&
                             control_request_fresh(o.run_dir, "preview.request", PREVIEW_REQUEST_AGE_S);
            if (preview_active && !was_active) preview_restart(&preview);   /* no stale history */
            if (preview_active) {
                last_viewer = now;
                preview_refresh_tuning(&preview);
            }
            if (preview_enabled) preview_rate_update(&preview_rate, &preview, now);
            if (o.audio && control_request_pending(o.run_dir, "compare.request") && !audio->compare.active) {
                control_request_done(o.run_dir, "compare.request");
                audio_engine_start_compare(audio);
                log_info("Recording %d s before/after comparison", COMPARE_SECONDS);
            }
            if (!dump.file && control_request_pending(o.run_dir, "rows.request") &&
                frame_source_streaming(source))
                rows_dump_start(&dump, &o, fps);
            if (!o.audio) {
                if (preview_active && !frame_source_streaming(source)) {
                    if (frame_source_start(source)) { exit_code = 2; break; }
                    last_frame = now;
                } else if (!preview_active && frame_source_streaming(source) && now - last_viewer > IDLE_STOP_S) {
                    frame_source_stop(source);
                    log_info("No viewers; camera stream paused");
                }
            }
            write_status(&o, source, audio, frames_received, &preview, preview_active, preview_rate.fps);
        }

        if (!frame_source_streaming(source)) {
            usleep(250000);
            continue;
        }

        frame_t frame;
        int got = frame_source_next(source, &frame, 500);
        if (got < 0) {
            if (o.source.from_file) break;      /* end of recording */
            log_error("Camera stream ended (device unplugged or reset)");
            exit_code = 2;
            break;
        }
        if (got == 0) {
            if (monotonic_now() - last_frame > STALL_LIMIT_S) {
                log_error("No camera frames for %d s; restarting capture", STALL_LIMIT_S);
                exit_code = 2;
                break;
            }
            continue;
        }
        last_frame = monotonic_now();
        frames_received++;
        if (audio) {
            audio_engine_frame(audio, &frame);
            if (audio->broken_pipe) {
                log_error("PulseAudio writer closed the PCM pipe");
                exit_code = 4;
                frame_source_release(source, &frame);
                break;
            }
            if (audio_engine_compare_ready(audio)) {
                size_t n = audio->compare.length;
                control_write_wav(o.run_dir, "compare_raw.wav", audio->compare.raw, n, PIPELINE_RATE);
                control_write_wav(o.run_dir, "compare_processed.wav", audio->compare.processed, n, PIPELINE_RATE);
                log_info("Before/after comparison ready");
            }
        }
        if (preview_active && !frame.damaged) preview_offer(&preview, frame.data, frame.bytes_used, last_frame);
        if (dump.file) rows_dump_frame(&dump, &o, &frame);
        frame_source_release(source, &frame);
    }

    if (o.source.from_file && audio) {
        audio_engine_finish(audio);
        write_status(&o, source, audio, frames_received, &preview, preview_active, preview_rate.fps);
    }
    if (dump.file) fclose(dump.file);
    if (audio) {
        audio_engine_free(audio);
        free(audio);
    }
    if (preview_enabled) preview_free(&preview);
    frame_source_close(source);
    return exit_code;
}
