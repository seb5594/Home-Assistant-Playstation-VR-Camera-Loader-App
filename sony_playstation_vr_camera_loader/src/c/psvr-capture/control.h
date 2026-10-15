/*
 * File-based control channel with the Python services in /run/ps-camera.
 *
 *   preview.request   touched by viewers; preview runs while it is < 10 s old
 *   compare.request   record 4 s before/after processing as two WAV files
 *   rows.request      dump 2 s of raw row headers + audio bytes for analysis
 *   engine.json       status, rewritten once per second
 */
#ifndef PSVR_CONTROL_H
#define PSVR_CONTROL_H

#include <stddef.h>
#include <stdint.h>

int control_request_fresh(const char *dir, const char *name, double max_age_s);
int control_request_pending(const char *dir, const char *name);
void control_request_done(const char *dir, const char *name);
int control_write_atomic(const char *dir, const char *name, const void *data, size_t size);
int control_write_wav(const char *dir, const char *name, const int16_t *samples, size_t count, int rate);

#endif
