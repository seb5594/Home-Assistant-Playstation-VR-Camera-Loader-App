/*
 * Linear algebra on 4-element complex vectors and 4x4 Hermitian matrices,
 * stored row-major. Small enough to inline everywhere it is used.
 */
#ifndef PSVR_CMATRIX4_H
#define PSVR_CMATRIX4_H

#include <math.h>
#include <string.h>

#include "fft.h"

#define CM4 4

static inline void cm4_mat_vec(const cfloat *m, const cfloat *v, cfloat *out)
{
    for (int r = 0; r < CM4; r++) {
        cfloat acc = 0;
        for (int c = 0; c < CM4; c++) acc += m[r * CM4 + c] * v[c];
        out[r] = acc;
    }
}

static inline cfloat cm4_dot(const cfloat *a, const cfloat *b)   /* a^H b */
{
    cfloat acc = 0;
    for (int c = 0; c < CM4; c++) acc += conjf(a[c]) * b[c];
    return acc;
}

static inline float cm4_norm2(const cfloat *v) { return crealf(cm4_dot(v, v)); }

static inline float cm4_normalize(cfloat *v)
{
    float norm = sqrtf(cm4_norm2(v));
    if (norm < 1e-30f) return 0.0f;
    for (int c = 0; c < CM4; c++) v[c] /= norm;
    return norm;
}

static inline float cm4_trace(const cfloat *m)
{
    float t = 0.0f;
    for (int c = 0; c < CM4; c++) t += crealf(m[c * CM4 + c]);
    return t;
}

/* m += weight * (x x^H - m): exponential averaging of an outer product. */
static inline void cm4_track(cfloat *m, const cfloat *x, float weight)
{
    for (int r = 0; r < CM4; r++)
        for (int c = 0; c < CM4; c++)
            m[r * CM4 + c] += weight * (x[r] * conjf(x[c]) - m[r * CM4 + c]);
}

/*
 * Cholesky factor L (lower triangle, row-major) of m + load * I.
 * Returns 0 if the matrix is not positive definite even with the loading.
 */
static inline int cm4_cholesky(const cfloat *m, float load, cfloat *l)
{
    memset(l, 0, sizeof(cfloat) * CM4 * CM4);
    for (int j = 0; j < CM4; j++) {
        float diagonal = crealf(m[j * CM4 + j]) + load;
        for (int k = 0; k < j; k++) diagonal -= crealf(l[j * CM4 + k] * conjf(l[j * CM4 + k]));
        if (diagonal <= 1e-30f) return 0;
        float root = sqrtf(diagonal);
        l[j * CM4 + j] = root;
        for (int i = j + 1; i < CM4; i++) {
            cfloat acc = m[i * CM4 + j];
            for (int k = 0; k < j; k++) acc -= l[i * CM4 + k] * conjf(l[j * CM4 + k]);
            l[i * CM4 + j] = acc / root;
        }
    }
    return 1;
}

/* Solves (L L^H) x = b with the factor from cm4_cholesky. */
static inline void cm4_solve(const cfloat *l, const cfloat *b, cfloat *x)
{
    cfloat y[CM4];
    for (int i = 0; i < CM4; i++) {           /* L y = b */
        cfloat acc = b[i];
        for (int k = 0; k < i; k++) acc -= l[i * CM4 + k] * y[k];
        y[i] = acc / crealf(l[i * CM4 + i]);
    }
    for (int i = CM4 - 1; i >= 0; i--) {      /* L^H x = y */
        cfloat acc = y[i];
        for (int k = i + 1; k < CM4; k++) acc -= conjf(l[k * CM4 + i]) * x[k];
        x[i] = acc / crealf(l[i * CM4 + i]);
    }
}

#endif
