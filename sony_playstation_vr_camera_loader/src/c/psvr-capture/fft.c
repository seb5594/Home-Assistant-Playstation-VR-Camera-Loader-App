#include "fft.h"

#include <math.h>
#include <stdlib.h>

int fft_plan_init(fft_plan_t *plan, int size)
{
    int bits = 0;

    if (size < 2 || (size & (size - 1))) return -1;
    while ((1 << bits) < size) bits++;
    plan->size = size;
    plan->bit_reverse = malloc(sizeof(int) * size);
    plan->twiddles = malloc(sizeof(cfloat) * (size / 2));
    if (!plan->bit_reverse || !plan->twiddles) {
        fft_plan_free(plan);
        return -1;
    }
    for (int i = 0; i < size; i++) {
        int reversed = 0;
        for (int b = 0; b < bits; b++)
            if (i & (1 << b)) reversed |= 1 << (bits - 1 - b);
        plan->bit_reverse[i] = reversed;
    }
    for (int i = 0; i < size / 2; i++) {
        double angle = -2.0 * M_PI * i / size;
        plan->twiddles[i] = (float)cos(angle) + I * (float)sin(angle);
    }
    return 0;
}

void fft_plan_free(fft_plan_t *plan)
{
    free(plan->bit_reverse);
    free(plan->twiddles);
    plan->bit_reverse = NULL;
    plan->twiddles = NULL;
}

static void transform(const fft_plan_t *plan, cfloat *data, int inverse)
{
    const int n = plan->size;

    for (int i = 0; i < n; i++) {
        int j = plan->bit_reverse[i];
        if (j > i) {
            cfloat swap = data[i];
            data[i] = data[j];
            data[j] = swap;
        }
    }
    for (int length = 2; length <= n; length <<= 1) {
        int half = length / 2, stride = n / length;
        for (int start = 0; start < n; start += length) {
            for (int k = 0; k < half; k++) {
                cfloat w = plan->twiddles[k * stride];
                if (inverse) w = conjf(w);
                cfloat odd = data[start + k + half] * w;
                data[start + k + half] = data[start + k] - odd;
                data[start + k] += odd;
            }
        }
    }
}

void fft_forward(const fft_plan_t *plan, cfloat *data) { transform(plan, data, 0); }
void fft_inverse(const fft_plan_t *plan, cfloat *data) { transform(plan, data, 1); }
