#include "denoiser.h"

#include <rnnoise.h>
#include <string.h>

int denoiser_init(denoiser_t *dn, int strength_percent)
{
    memset(dn, 0, sizeof(*dn));
    if (rnnoise_get_frame_size() != DENOISER_CHUNK_48K) return -1;
    dn->state = rnnoise_create(NULL);
    if (!dn->state) return -1;
    if (strength_percent < 0) strength_percent = 0;
    if (strength_percent > 100) strength_percent = 100;
    dn->strength = strength_percent / 100.0f;
    rate3_init(&dn->up);
    rate3_init(&dn->down);
    /* One chunk of silence keeps output available for every input block. */
    dn->out_count = DENOISER_CHUNK;
    return 0;
}

void denoiser_free(denoiser_t *dn)
{
    if (dn->state) rnnoise_destroy(dn->state);
    dn->state = NULL;
}

static void process_chunk(denoiser_t *dn)
{
    float upsampled[DENOISER_CHUNK_48K], denoised[DENOISER_CHUNK_48K];

    rate3_upsample(&dn->up, dn->pending_in, DENOISER_CHUNK, upsampled);
    dn->voice_probability = rnnoise_process_frame(dn->state, denoised, upsampled);
    /* The dry line holds the last two chunks: RNNoise lags by exactly that. */
    for (int i = 0; i < DENOISER_CHUNK_48K; i++)
        denoised[i] = dn->strength * denoised[i] + (1.0f - dn->strength) * dn->dry_delay[i];
    memmove(dn->dry_delay, dn->dry_delay + DENOISER_CHUNK_48K,
            (DENOISER_LATENCY_48K - DENOISER_CHUNK_48K) * sizeof(float));
    memcpy(dn->dry_delay + DENOISER_LATENCY_48K - DENOISER_CHUNK_48K, upsampled, sizeof(upsampled));
    rate3_downsample(&dn->down, denoised, DENOISER_CHUNK_48K, dn->pending_out + dn->out_count);
    dn->out_count += DENOISER_CHUNK;
}

void denoiser_process(denoiser_t *dn, const float *in, float *out, int count)
{
    for (int i = 0; i < count; i++) {
        dn->pending_in[dn->pending_count++] = in[i];
        if (dn->pending_count == DENOISER_CHUNK) {
            process_chunk(dn);
            dn->pending_count = 0;
        }
    }
    memcpy(out, dn->pending_out, count * sizeof(float));
    dn->out_count -= count;
    memmove(dn->pending_out, dn->pending_out + count, dn->out_count * sizeof(float));
}
