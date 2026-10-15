#include "control.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void join(char *out, size_t size, const char *dir, const char *name)
{
    snprintf(out, size, "%s/%s", dir, name);
}

int control_request_fresh(const char *dir, const char *name, double max_age_s)
{
    char path[512];
    struct stat st;
    join(path, sizeof(path), dir, name);
    if (stat(path, &st) < 0) return 0;
    return difftime(time(NULL), st.st_mtime) < max_age_s;
}

int control_request_pending(const char *dir, const char *name)
{
    char path[512];
    join(path, sizeof(path), dir, name);
    return access(path, F_OK) == 0;
}

void control_request_done(const char *dir, const char *name)
{
    char path[512];
    join(path, sizeof(path), dir, name);
    unlink(path);
}

int control_write_atomic(const char *dir, const char *name, const void *data, size_t size)
{
    char path[512], temporary[520];
    join(path, sizeof(path), dir, name);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *f = fopen(temporary, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, size, f) == size;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(temporary, path) < 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

int control_write_wav(const char *dir, const char *name, const int16_t *samples, size_t count, int rate)
{
    uint8_t header[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0";
    uint32_t data_bytes = (uint32_t)(count * 2);
    char path[512], temporary[520];

    put_u32(header + 4, 36 + data_bytes);
    put_u32(header + 24, (uint32_t)rate);
    put_u32(header + 28, (uint32_t)rate * 2);
    header[32] = 2, header[33] = 0, header[34] = 16, header[35] = 0;
    memcpy(header + 36, "data", 4);
    put_u32(header + 40, data_bytes);

    join(path, sizeof(path), dir, name);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *f = fopen(temporary, "wb");
    if (!f) return -1;
    int ok = fwrite(header, 1, sizeof(header), f) == sizeof(header);
    /* Samples are host order; every supported target (amd64, aarch64) is little endian. */
    ok = ok && fwrite(samples, 2, count, f) == count;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(temporary, path) < 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}
