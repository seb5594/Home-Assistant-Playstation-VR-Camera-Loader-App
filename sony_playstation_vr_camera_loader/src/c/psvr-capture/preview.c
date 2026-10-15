#include "preview.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <turbojpeg.h>
#include <unistd.h>

#include "log.h"

enum { IMAGE_OFFSET_BYTES = 96 };  /* 32 header + 64 audio bytes precede the picture */

static const float STATS_SMOOTHING = 0.2f;     /* per preview frame */
/*
 * Automatic exposure levels. The camera's own exposure leaves dim rooms in
 * a few dozen luma steps, so the stretch may reach about 6x; on top, an
 * exposure gamma lifts the scene median towards EXPOSURE_TARGET.
 */
static const float MIN_LEVEL_RANGE = 36.0f;
static const float EXPOSURE_TARGET = 0.30f;
static const float MAX_EXPOSURE_GAMMA = 2.2f;
/*
 * White balance looks only at pixels that are already close to grey, so a
 * blue wall or a red sofa does not get "corrected" towards grey itself.
 * Which pixels count as mid-tones follows the scene, not fixed values, or
 * a dark room would have none.
 */
static const int NEUTRAL_RADIUS = 24;          /* |U| + |V| distance, limited-range units */
static const float WHITE_BALANCE_STRENGTH = 0.9f;
static const float WHITE_BALANCE_LIMIT = 24.0f;

/* ---- tuning ------------------------------------------------------------- */

void picture_tuning_defaults(picture_tuning_t *t)
{
    t->brightness = 0;
    t->contrast = 100;
    t->gamma = 100;
    t->saturation = 160;   /* the camera ISP mutes colour, much more so in dim light */
    t->auto_levels = 1;
    t->white_balance = 1;
    t->denoise = 1;
}

static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Finds "key": <number|true|false> in a flat JSON object. */
static int json_int(const char *json, const char *key, int *value)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return 0;
    while (isspace((unsigned char)*++p)) {}
    if (!strncmp(p, "true", 4)) *value = 1;
    else if (!strncmp(p, "false", 5)) *value = 0;
    else {
        char *end;
        long parsed = strtol(p, &end, 10);
        if (end == p) return 0;
        *value = (int)parsed;
    }
    return 1;
}

int picture_tuning_load(picture_tuning_t *t, const char *path)
{
    char json[2048];
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[n] = '\0';
    json_int(json, "brightness", &t->brightness);
    json_int(json, "contrast", &t->contrast);
    json_int(json, "gamma", &t->gamma);
    json_int(json, "saturation", &t->saturation);
    json_int(json, "auto_levels", &t->auto_levels);
    json_int(json, "white_balance", &t->white_balance);
    json_int(json, "denoise", &t->denoise);
    t->brightness = clamp_int(t->brightness, -100, 100);
    t->contrast = clamp_int(t->contrast, 50, 200);
    t->gamma = clamp_int(t->gamma, 50, 250);
    t->saturation = clamp_int(t->saturation, 0, 400);
    t->auto_levels = t->auto_levels != 0;
    t->white_balance = t->white_balance != 0;
    t->denoise = clamp_int(t->denoise, 0, 2);
    return 0;
}

void preview_refresh_tuning(preview_t *pv)
{
    struct stat st;
    if (!pv->tuning_path[0] || stat(pv->tuning_path, &st) < 0) return;
    if (st.st_mtim.tv_sec == pv->tuning_mtime.tv_sec && st.st_mtim.tv_nsec == pv->tuning_mtime.tv_nsec &&
        (unsigned long)st.st_ino == pv->tuning_inode)
        return;
    pv->tuning_mtime = st.st_mtim;
    pv->tuning_inode = (unsigned long)st.st_ino;
    if (picture_tuning_load(&pv->tuning, pv->tuning_path) == 0)
        log_debug("Picture: brightness %d, contrast %d %%, gamma %d %%, saturation %d %%, auto levels %s, "
                  "white balance %s, noise filter %d", pv->tuning.brightness, pv->tuning.contrast,
                  pv->tuning.gamma, pv->tuning.saturation, pv->tuning.auto_levels ? "on" : "off",
                  pv->tuning.white_balance ? "on" : "off", pv->tuning.denoise);
}

void preview_restart(preview_t *pv)
{
    pv->have_history = 0;
    pv->have_stats = 0;
}

/* ---- setup -------------------------------------------------------------- */

/*
 * Raw modes put a 32-byte header and 64 audio bytes in front of every row,
 * then both eyes and a downscaled pyramid: width = 48 + 85 * (eye width / 32),
 * plus 8 rows of trailer data (3448x808 -> 1280x800, 5148x1088 -> 1920x1080).
 * Anything else is a plain picture: both eyes side by side, or a single one.
 */
static void describe_geometry(preview_t *pv, int raw_width, int raw_height)
{
    int payload = raw_width - IMAGE_OFFSET_BYTES / 2;
    if (payload > 0 && payload % 85 == 0 && raw_height > 8) {
        pv->image_offset = IMAGE_OFFSET_BYTES;
        pv->sensor_width = payload / 85 * 32;
        pv->sensor_height = raw_height - 8;
    } else {
        pv->image_offset = 0;
        pv->sensor_width = raw_width * 5 > raw_height * 12 ? raw_width / 2 : raw_width;
        pv->sensor_height = raw_height;
    }
    pv->sensor_width &= ~1;
}

int preview_init(preview_t *pv, int raw_width, int raw_height, const char *preset, int fps,
                 const char *path, const char *tuning_path)
{
    memset(pv, 0, sizeof(*pv));
    pv->row_bytes = raw_width * 2;
    describe_geometry(pv, raw_width, raw_height);
    if (pv->sensor_width < 64 || pv->sensor_height < 64) {
        log_error("Preview: unknown raw geometry %dx%d", raw_width, raw_height);
        return -1;
    }
    pv->crop_width = pv->sensor_width;
    pv->crop_height = pv->sensor_height;
    pv->scale = 1;
    if (!strcmp(preset, "hd")) {
        pv->crop_height = (pv->sensor_width * 9 / 16) & ~1;
        if (pv->crop_height > pv->sensor_height) pv->crop_height = pv->sensor_height;
    } else if (!strcmp(preset, "sd")) {
        pv->scale = 2;
    } else if (!strcmp(preset, "low")) {
        pv->scale = 4;
    }
    pv->crop_y = (pv->sensor_height - pv->crop_height) / 2;
    pv->width = (pv->crop_width / pv->scale) & ~1;
    pv->height = (pv->crop_height / pv->scale) & ~1;
    pv->quality = 85;
    pv->interval = 1.0 / (fps > 0 ? fps : 1);
    picture_tuning_defaults(&pv->tuning);
    if (tuning_path) {
        snprintf(pv->tuning_path, sizeof(pv->tuning_path), "%s", tuning_path);
        preview_refresh_tuning(pv);
    }
    size_t plane_bytes = (size_t)pv->width * pv->height * 3 / 2;
    pv->planes = malloc(plane_bytes);
    pv->history = malloc(plane_bytes);
    pv->encoder = tjInitCompress();
    if (!pv->planes || !pv->history || !pv->encoder) {
        preview_free(pv);
        return -1;
    }
    snprintf(pv->path, sizeof(pv->path), "%s", path);
    snprintf(pv->temporary, sizeof(pv->temporary), "%s.tmp", path);
    return 0;
}

void preview_free(preview_t *pv)
{
    if (pv->encoder) tjDestroy(pv->encoder);
    free(pv->planes);
    free(pv->history);
    pv->encoder = NULL;
    pv->planes = pv->history = NULL;
}

/* ---- image processing ---------------------------------------------------- */

static const uint8_t *source_row(const preview_t *pv, const uint8_t *frame, int y)
{
    return frame + (size_t)(pv->crop_y + y) * pv->row_bytes + pv->image_offset + 2 * pv->crop_x;
}

/* YUYV -> planar 4:2:0 at full eye resolution: the common "native" case, kept branch-free. */
static void convert_native(preview_t *pv, const uint8_t *frame)
{
    const int w = pv->width, h = pv->height, cw = w / 2;
    uint8_t *y_plane = pv->planes, *u_plane = y_plane + w * h, *v_plane = u_plane + cw * (h / 2);

    for (int y = 0; y < h; y += 2) {
        const uint8_t *upper = source_row(pv, frame, y), *lower = source_row(pv, frame, y + 1);
        uint8_t *y_upper = y_plane + y * w, *y_lower = y_upper + w;
        uint8_t *u = u_plane + (y / 2) * cw, *v = v_plane + (y / 2) * cw;
        for (int x = 0; x < w; x++) {
            y_upper[x] = upper[2 * x];
            y_lower[x] = lower[2 * x];
        }
        for (int pair = 0; pair < cw; pair++) {
            u[pair] = (uint8_t)((upper[4 * pair + 1] + lower[4 * pair + 1] + 1) >> 1);
            v[pair] = (uint8_t)((upper[4 * pair + 3] + lower[4 * pair + 3] + 1) >> 1);
        }
    }
}

/*
 * Box-scaled conversion for the reduced presets. Inlined with a constant
 * scale, so the compiler unrolls the inner loops for 2 and 4.
 */
static inline __attribute__((always_inline)) void convert_scaled(preview_t *pv, const uint8_t *frame, const int s)
{
    const int w = pv->width, h = pv->height, cw = w / 2;
    uint8_t *y_plane = pv->planes, *u_plane = y_plane + w * h, *v_plane = u_plane + cw * (h / 2);
    const unsigned luma_count = (unsigned)(s * s), chroma_count = (unsigned)(2 * s * s);

    for (int oy = 0; oy < h; oy++) {
        for (int ox = 0; ox < w; ox++) {
            unsigned sum = 0;
            for (int dy = 0; dy < s; dy++) {
                const uint8_t *row = source_row(pv, frame, oy * s + dy) + 2 * ox * s;
                for (int dx = 0; dx < s; dx++) sum += row[2 * dx];
            }
            y_plane[oy * w + ox] = (uint8_t)(sum / luma_count);
        }
    }
    /* one chroma sample per 2x2 output pixels = 2s x 2s source pixels = s pairs per row */
    for (int oy = 0; oy < h / 2; oy++) {
        for (int ox = 0; ox < cw; ox++) {
            unsigned su = 0, sv = 0;
            for (int dy = 0; dy < 2 * s; dy++) {
                const uint8_t *row = source_row(pv, frame, oy * 2 * s + dy) + 4 * ox * s;
                for (int pair = 0; pair < s; pair++) {
                    su += row[4 * pair + 1];
                    sv += row[4 * pair + 3];
                }
            }
            u_plane[oy * cw + ox] = (uint8_t)(su / chroma_count);
            v_plane[oy * cw + ox] = (uint8_t)(sv / chroma_count);
        }
    }
}

static void convert(preview_t *pv, const uint8_t *frame)
{
    switch (pv->scale) {
    case 1: convert_native(pv, frame); break;
    case 2: convert_scaled(pv, frame, 2); break;
    default: convert_scaled(pv, frame, 4); break;
    }
}

/*
 * Recursive filter that follows the previous output closely while the change
 * is small (sensor noise) and lets real motion through immediately.
 */
static void denoise(preview_t *pv)
{
    const size_t bytes = (size_t)pv->width * pv->height * 3 / 2;
    int16_t step[511];         /* how far to follow each possible difference, index diff + 255 */

    if (pv->tuning.denoise == 0 || !pv->have_history) {
        memcpy(pv->history, pv->planes, bytes);
        pv->have_history = 1;
        return;
    }
    const float floor = pv->tuning.denoise == 1 ? 0.45f : 0.25f;
    const float threshold = pv->tuning.denoise == 1 ? 20.0f : 28.0f;
    for (int diff = -255; diff <= 255; diff++) {
        float follow = floor + (1.0f - floor) * fminf(1.0f, abs(diff) / threshold);
        step[diff + 255] = (int16_t)lroundf(diff * follow);
    }
    uint8_t *restrict current = pv->planes, *restrict history = pv->history;
    for (size_t i = 0; i < bytes; i++) {
        int previous = history[i];
        history[i] = current[i] = (uint8_t)(previous + step[current[i] - previous + 255]);
    }
}

static float smooth(float current, float target, int first)
{
    return first ? target : current + STATS_SMOOTHING * (target - current);
}

static float clamp_float(float v, float limit) { return v < -limit ? -limit : v > limit ? limit : v; }

/* Luma value below which `share` of the histogram lies. */
static int percentile(const unsigned histogram[256], unsigned samples, double share)
{
    unsigned target = (unsigned)(samples * share), seen = 0;
    for (int i = 0; i < 256; i++) {
        seen += histogram[i];
        if (seen > target) return i;
    }
    return 255;
}

static void measure_levels(preview_t *pv, const unsigned histogram[256], unsigned samples, int first)
{
    int black = percentile(histogram, samples, 0.005), white = percentile(histogram, samples, 0.995) + 1;
    int median = percentile(histogram, samples, 0.5);

    /* keep the black point (that is what removes the fog) and limit the gain via white */
    if (white - black < MIN_LEVEL_RANGE) white = (int)fminf(255.0f, black + MIN_LEVEL_RANGE);
    if (white - black < MIN_LEVEL_RANGE) black = (int)(white - MIN_LEVEL_RANGE);
    float mid = fmaxf(0.01f, (median - black) / (float)(white - black));
    float gamma = mid < EXPOSURE_TARGET ? logf(mid) / logf(EXPOSURE_TARGET) : 1.0f;
    pv->black = smooth(pv->black, (float)black, first);
    pv->white = smooth(pv->white, (float)white, first);
    pv->exposure_gamma = smooth(pv->exposure_gamma, fminf(gamma, MAX_EXPOSURE_GAMMA), first);
}

static void measure_white_balance(preview_t *pv, int lowest, int first)
{
    const int w = pv->width, h = pv->height, cw = w / 2;
    const uint8_t *y_plane = pv->planes, *u_plane = y_plane + w * h, *v_plane = u_plane + cw * (h / 2);
    const int neutral_u = 128 - (int)lroundf(pv->shift_u), neutral_v = 128 - (int)lroundf(pv->shift_v);
    unsigned samples = 0, grey_samples = 0;
    double sum_u = 0, sum_v = 0;

    for (int y = 0; y < h; y += 4) {
        for (int x = 0; x < w; x += 4) {
            int luma = y_plane[y * w + x];
            if (luma < lowest || luma > 230) continue;   /* dark noise and clipped highlights lie */
            samples++;
            int u = u_plane[(y / 2) * cw + x / 2], v = v_plane[(y / 2) * cw + x / 2];
            if (abs(u - neutral_u) + abs(v - neutral_v) >= NEUTRAL_RADIUS) continue;
            sum_u += u;
            sum_v += v;
            grey_samples++;
        }
    }
    if (grey_samples < 64 || grey_samples * 20 < samples) return;    /* no grey surface to trust */
    float shift_u = clamp_float((128.0f - (float)(sum_u / grey_samples)) * WHITE_BALANCE_STRENGTH, WHITE_BALANCE_LIMIT);
    float shift_v = clamp_float((128.0f - (float)(sum_v / grey_samples)) * WHITE_BALANCE_STRENGTH, WHITE_BALANCE_LIMIT);
    pv->shift_u = smooth(pv->shift_u, shift_u, first);
    pv->shift_v = smooth(pv->shift_v, shift_v, first);
}

static void measure_scene(preview_t *pv)
{
    const int w = pv->width, h = pv->height;
    unsigned histogram[256] = {0}, samples = 0;
    int first = !pv->have_stats;

    for (int y = 0; y < h; y += 2)
        for (int x = 0; x < w; x += 2, samples++) histogram[pv->planes[y * w + x]]++;
    measure_levels(pv, histogram, samples, first);

    /* mid-tones: above the lowest eighth of what this scene actually spans */
    int black = percentile(histogram, samples, 0.005), white = percentile(histogram, samples, 0.995);
    int lowest = black + (white - black) / 8;
    measure_white_balance(pv, lowest > 18 ? lowest : 18, first);
    pv->have_stats = 1;
}

static void tone_map(preview_t *pv)
{
    const picture_tuning_t *t = &pv->tuning;
    const int w = pv->width, h = pv->height;
    uint8_t luma[256], chroma_u[256], chroma_v[256];
    /* Sensor output is limited range (16..235 / 16..240); JPEG expects full range. */
    float black = t->auto_levels ? pv->black : 16.0f;
    float white = t->auto_levels ? pv->white : 235.0f;
    float gamma = t->gamma / 100.0f * (t->auto_levels ? pv->exposure_gamma : 1.0f);
    float contrast = t->contrast / 100.0f, brightness = t->brightness / 200.0f;
    float chroma_gain = t->saturation / 100.0f * (255.0f / 224.0f);
    float shift_u = t->white_balance ? pv->shift_u : 0.0f, shift_v = t->white_balance ? pv->shift_v : 0.0f;

    for (int i = 0; i < 256; i++) {
        float v = (i - black) / fmaxf(1.0f, white - black);
        v = powf(fminf(1.0f, fmaxf(0.0f, v)), 1.0f / gamma);
        v = contrast * (v - 0.5f) + 0.5f + brightness;
        luma[i] = (uint8_t)clamp_int((int)lroundf(v * 255.0f), 0, 255);
        chroma_u[i] = (uint8_t)clamp_int((int)lroundf(128.0f + (i - 128 + shift_u) * chroma_gain), 0, 255);
        chroma_v[i] = (uint8_t)clamp_int((int)lroundf(128.0f + (i - 128 + shift_v) * chroma_gain), 0, 255);
    }
    uint8_t *p = pv->planes;
    for (int i = 0; i < w * h; i++) p[i] = luma[p[i]];
    p += w * h;
    for (int i = 0; i < (w / 2) * (h / 2); i++) p[i] = chroma_u[p[i]];
    p += (w / 2) * (h / 2);
    for (int i = 0; i < (w / 2) * (h / 2); i++) p[i] = chroma_v[p[i]];
}

int preview_offer(preview_t *pv, const uint8_t *frame, size_t bytes_used, double now)
{
    unsigned char *jpeg = NULL;
    unsigned long size = 0;
    const unsigned char *planes[3];
    int strides[3] = {pv->width, pv->width / 2, pv->width / 2};

    if (now < pv->next_due) return 0;
    if (bytes_used < (size_t)(pv->crop_y + pv->crop_height) * pv->row_bytes) return 0;
    pv->next_due = now + pv->interval * 0.95;

    convert(pv, frame);
    denoise(pv);
    measure_scene(pv);
    tone_map(pv);
    planes[0] = pv->planes;
    planes[1] = planes[0] + pv->width * pv->height;
    planes[2] = planes[1] + (pv->width / 2) * (pv->height / 2);
    if (tjCompressFromYUVPlanes(pv->encoder, planes, pv->width, strides, pv->height, TJSAMP_420,
                                &jpeg, &size, pv->quality, TJFLAG_FASTDCT) < 0) {
        log_warning("Preview JPEG encoding failed: %s", tjGetErrorStr());
        tjFree(jpeg);
        return 0;
    }
    FILE *out = fopen(pv->temporary, "wb");
    int ok = out && fwrite(jpeg, 1, size, out) == size;
    if (out) ok = (fclose(out) == 0) && ok;
    tjFree(jpeg);
    if (!ok || rename(pv->temporary, pv->path) < 0) {
        unlink(pv->temporary);
        return 0;
    }
    pv->published++;
    return 1;
}
