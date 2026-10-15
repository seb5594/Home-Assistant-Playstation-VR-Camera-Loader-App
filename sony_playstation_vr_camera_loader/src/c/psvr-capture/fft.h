/* Small in-place radix-2 FFT; enough for 256..1024 point audio frames. */
#ifndef PSVR_FFT_H
#define PSVR_FFT_H

#include <complex.h>

typedef float complex cfloat;

typedef struct {
    int size;
    int *bit_reverse;
    cfloat *twiddles;   /* size / 2 forward twiddles */
} fft_plan_t;

int fft_plan_init(fft_plan_t *plan, int size);   /* size must be a power of two */
void fft_plan_free(fft_plan_t *plan);
void fft_forward(const fft_plan_t *plan, cfloat *data);
/* Unscaled inverse; divide by size to undo fft_forward. */
void fft_inverse(const fft_plan_t *plan, cfloat *data);

#endif
