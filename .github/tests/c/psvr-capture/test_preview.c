/* Picture processing of the preview: range expansion, white balance, controls, noise filter. */
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "preview.h"
#include "test_util.h"

enum { RAW_WIDTH = 3448, RAW_HEIGHT = 808, ROW_BYTES = RAW_WIDTH * 2, IMAGE_OFFSET = 96 };

static uint8_t frame[(size_t)ROW_BYTES * RAW_HEIGHT];
static char run_dir[] = "/tmp/psvr-preview-XXXXXX";
static char still_path[256], tuning_path[256];

typedef struct { int y, u, v; } yuv_t;

/* Fills the left sensor image; colour() picks the YUYV value per pixel. */
static void paint(yuv_t (*colour)(int x, int y))
{
    memset(frame, 0, sizeof(frame));
    for (int y = 0; y < 800; y++) {
        uint8_t *row = frame + (size_t)y * ROW_BYTES + IMAGE_OFFSET;
        for (int x = 0; x < 1280; x += 2) {
            yuv_t c = colour(x, y);
            row[2 * x] = (uint8_t)c.y;
            row[2 * x + 1] = (uint8_t)c.u;
            row[2 * x + 2] = (uint8_t)c.y;
            row[2 * x + 3] = (uint8_t)c.v;
        }
    }
}

static void write_tuning(const char *json)
{
    FILE *f = fopen(tuning_path, "w");
    fputs(json, f);
    fclose(f);
}

static preview_t open_preview(void)
{
    preview_t pv;
    int ok = preview_init(&pv, RAW_WIDTH, RAW_HEIGHT, "low", 30, still_path, tuning_path);
    CHECK(ok == 0, "preview_init failed");
    CHECK(pv.width == 320 && pv.height == 200, "low preset is %dx%d", pv.width, pv.height);
    return pv;
}

static void offer(preview_t *pv, int frames)
{
    static double now = 1000.0;
    for (int i = 0; i < frames; i++) {
        now += 1.0;
        preview_offer(pv, frame, sizeof(frame), now);
    }
}

static yuv_t pixel(const preview_t *pv, int x, int y)
{
    const uint8_t *u_plane = pv->planes + pv->width * pv->height;
    const uint8_t *v_plane = u_plane + (pv->width / 2) * (pv->height / 2);
    int c = (y / 2) * (pv->width / 2) + x / 2;
    return (yuv_t){pv->planes[y * pv->width + x], u_plane[c], v_plane[c]};
}

/* ---- scenes ---- */

static yuv_t black_and_white_halves(int x, int y) { (void)y; return x < 640 ? (yuv_t){16, 128, 128} : (yuv_t){235, 128, 128}; }
static yuv_t green_cast_grey(int x, int y) { (void)x; (void)y; return (yuv_t){120, 118, 116}; }
static yuv_t blue_wall_with_grey_card(int x, int y)
{
    (void)y;
    /* 80 % of the scene is a strongly blue wall, the rest a grey card with the same cast */
    return x < 1024 ? (yuv_t){110, 165, 110} : (yuv_t){120, 122, 124};
}
static yuv_t coloured(int x, int y) { (void)x; (void)y; return (yuv_t){120, 150, 100}; }

/* ---- tests ---- */

static void test_limited_range_is_expanded(void)
{
    write_tuning("{\"auto_levels\": false, \"white_balance\": false, \"denoise\": 0}");
    preview_t pv = open_preview();
    paint(black_and_white_halves);
    offer(&pv, 1);
    yuv_t dark = pixel(&pv, 40, 100), bright = pixel(&pv, 280, 100);
    CHECK(dark.y <= 2, "limited black 16 became %d instead of ~0 (grey fog)", dark.y);
    CHECK(bright.y >= 253, "limited white 235 became %d instead of ~255", bright.y);
    CHECK(access(still_path, F_OK) == 0, "no JPEG written");
    preview_free(&pv);
}

static void test_white_balance_removes_cast(void)
{
    write_tuning("{\"white_balance\": true, \"saturation\": 100, \"denoise\": 0}");
    preview_t pv = open_preview();
    paint(green_cast_grey);
    offer(&pv, 30);
    yuv_t c = pixel(&pv, 160, 100);
    CHECK(abs(c.u - 128) <= 4 && abs(c.v - 128) <= 5, "grey with cast still U=%d V=%d", c.u, c.v);
    preview_free(&pv);
}

static void test_white_balance_keeps_coloured_walls(void)
{
    write_tuning("{\"white_balance\": true, \"saturation\": 100, \"denoise\": 0}");
    preview_t pv = open_preview();
    paint(blue_wall_with_grey_card);
    offer(&pv, 30);
    yuv_t wall = pixel(&pv, 40, 100), card = pixel(&pv, 300, 100);
    CHECK(wall.u > 165, "blue wall lost its colour: U=%d", wall.u);
    CHECK(abs(card.u - 128) <= 5 && abs(card.v - 128) <= 5, "grey card U=%d V=%d", card.u, card.v);
    preview_free(&pv);
}

static void test_saturation_control(void)
{
    write_tuning("{\"white_balance\": false, \"saturation\": 0, \"denoise\": 0}");
    preview_t pv = open_preview();
    paint(coloured);
    offer(&pv, 1);
    yuv_t grey = pixel(&pv, 160, 100);
    CHECK(grey.u == 128 && grey.v == 128, "saturation 0 left U=%d V=%d", grey.u, grey.v);

    write_tuning("{\"white_balance\": false, \"saturation\": 200, \"denoise\": 0}");
    preview_refresh_tuning(&pv);  /* a save within the same second must still be seen */
    offer(&pv, 1);
    yuv_t vivid = pixel(&pv, 160, 100);
    int expected_u = 128 + (int)((150 - 128) * 2.0 * 255 / 224 + 0.5);
    CHECK(abs(vivid.u - expected_u) <= 1, "saturation 200 gave U=%d, expected %d", vivid.u, expected_u);
    preview_free(&pv);
}

static void test_tuning_file_is_clamped(void)
{
    picture_tuning_t t;
    picture_tuning_defaults(&t);
    write_tuning("{\"gamma\": 999, \"contrast\": 10, \"brightness\": -500, \"saturation\": 150, "
                 "\"auto_levels\": false, \"denoise\": 7}");
    CHECK(picture_tuning_load(&t, tuning_path) == 0, "load failed");
    CHECK(t.gamma == 250 && t.contrast == 50 && t.brightness == -100, "not clamped: %d %d %d",
          t.gamma, t.contrast, t.brightness);
    CHECK(t.saturation == 150 && t.auto_levels == 0 && t.denoise == 2, "values: %d %d %d",
          t.saturation, t.auto_levels, t.denoise);
    CHECK(t.white_balance == 1, "missing key must keep its value");
    CHECK(picture_tuning_load(&t, "/nonexistent/video.json") < 0, "missing file accepted");
}

static void test_geometries(void)
{
    static const struct { int raw_width, raw_height, offset, eye_width, eye_height; } cases[] = {
        {3448, 808, 96, 1280, 800},   /* PS4 raw, 60 fps with microphones */
        {898, 200, 96, 320, 192},     /* PS4 raw, smallest */
        {5148, 1088, 96, 1920, 1080}, /* PS5 raw */
        {3840, 1080, 0, 1920, 1080},  /* plain side by side */
        {1920, 1080, 0, 1920, 1080},  /* single eye */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        preview_t pv;
        int ok = preview_init(&pv, cases[i].raw_width, cases[i].raw_height, "native", 5, still_path, NULL);
        CHECK(ok == 0 && pv.image_offset == cases[i].offset && pv.sensor_width == cases[i].eye_width &&
              pv.sensor_height == cases[i].eye_height, "%dx%d -> offset %d, eye %dx%d", cases[i].raw_width,
              cases[i].raw_height, pv.image_offset, pv.sensor_width, pv.sensor_height);
        if (ok == 0) preview_free(&pv);
    }
}

static yuv_t noisy_grey(int x, int y)
{
    (void)x; (void)y;
    return (yuv_t){(int)(120 + 6 * gaussian()), 128, 128};
}

static yuv_t bright_square(int x, int y)
{
    return x >= 400 && x < 800 && y >= 200 && y < 600 ? (yuv_t){220, 128, 128} : (yuv_t){120, 128, 128};
}

static double luma_noise(const preview_t *pv)
{
    double sum = 0, squares = 0;
    int n = pv->width * pv->height;
    for (int i = 0; i < n; i++) {
        sum += pv->planes[i];
        squares += (double)pv->planes[i] * pv->planes[i];
    }
    return sqrt(squares / n - (sum / n) * (sum / n));
}

static void test_noise_filter(void)
{
    double noise[2];
    for (int strength = 0; strength <= 1; strength++) {
        write_tuning(strength ? "{\"auto_levels\": false, \"white_balance\": false, \"denoise\": 2}"
                              : "{\"auto_levels\": false, \"white_balance\": false, \"denoise\": 0}");
        preview_t pv = open_preview();
        for (int i = 0; i < 12; i++) {
            paint(noisy_grey);
            offer(&pv, 1);
        }
        noise[strength] = luma_noise(&pv);
        if (strength) {
            /* real motion must appear in the very next frame */
            paint(bright_square);
            offer(&pv, 1);
            yuv_t inside = pixel(&pv, 160, 100);
            CHECK(inside.y > 225, "moving object smeared: Y=%d", inside.y);
        }
        preview_free(&pv);
    }
    CHECK(noise[1] < noise[0] * 0.6, "noise filter: %.2f -> %.2f", noise[0], noise[1]);
    printf("       luma noise %.2f -> %.2f with the strong filter\n", noise[0], noise[1]);
}

int main(void)
{
    if (!mkdtemp(run_dir)) return 1;
    snprintf(still_path, sizeof(still_path), "%s/still.jpg", run_dir);
    snprintf(tuning_path, sizeof(tuning_path), "%s/video.json", run_dir);

    RUN(test_limited_range_is_expanded);
    RUN(test_white_balance_removes_cast);
    RUN(test_white_balance_keeps_coloured_walls);
    RUN(test_saturation_control);
    RUN(test_tuning_file_is_clamped);
    RUN(test_noise_filter);
    RUN(test_geometries);

    unlink(still_path);
    unlink(tuning_path);
    rmdir(run_dir);
    return test_failures ? 1 : 0;
}
