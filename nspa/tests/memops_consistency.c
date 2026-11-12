/* jack2-nspa: float -> integer conversion consistency (common/memops.c).
 *
 * The SIMD conversion functions process groups of 4 samples in a vector loop
 * and the remainder in a scalar tail. The same input value must produce the
 * same output bytes in either path. For every non-dithered conversion,
 * every length 1..23 and both packed and interleaved (stereo) strides,
 * each sample's output is compared with the output for that value produced
 * by the vector path (a 4-sample buffer filled with it).
 *
 * Build: cc -O2 <arch flags> -I common -I common/jack memops_consistency.c common/memops.c -lm
 */
#include <string.h>  /* memops.h needs memcpy */
#include "memops.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*move_fn)(char *, jack_default_audio_sample_t *, unsigned long, unsigned long, dither_state_t *);

static const struct { const char *name; move_fn fn; int bytes; } fns[] = {
    {"d32_sSs", sample_move_d32_sSs, 4},       {"d32_sS", sample_move_d32_sS, 4},
    {"d32u24_sSs", sample_move_d32u24_sSs, 4}, {"d32u24_sS", sample_move_d32u24_sS, 4},
    {"d32l24_sSs", sample_move_d32l24_sSs, 4}, {"d32l24_sS", sample_move_d32l24_sS, 4},
    {"d24_sSs", sample_move_d24_sSs, 3},       {"d24_sS", sample_move_d24_sS, 3},
    {"d16_sSs", sample_move_d16_sSs, 2},       {"d16_sS", sample_move_d16_sS, 2},
};

static float sample_value(unsigned i)
{
    static const float edges[] = {0.f, 1.f, -1.f, 0.5f, -0.5f, 1.25f, -1.25f, 1e-7f, -1e-7f, 0.999999f};
    if (i < sizeof edges / sizeof edges[0])
        return edges[i];
    return ((float)rand() / RAND_MAX) * 2.5f - 1.25f;
}

int main(void)
{
    static jack_default_audio_sample_t src[32] __attribute__((aligned(32)));
    static jack_default_audio_sample_t four[4] __attribute__((aligned(32)));
    static char out[32 * 8], ref[4 * 8];
    dither_state_t state;
    int failures = 0, checks = 0;

    srand(12345);
    for (size_t f = 0; f < sizeof fns / sizeof fns[0]; f++) {
        for (int interleave = 1; interleave <= 2; interleave++) {
            unsigned long skip = fns[f].bytes * interleave;
            for (unsigned long n = 1; n <= 23; n++) {
                for (unsigned long i = 0; i < n; i++)
                    src[i] = sample_value(i + n);
                memset(&state, 0, sizeof state);
                memset(out, 0, sizeof out);
                fns[f].fn(out, src, n, skip, &state);
                for (unsigned long i = 0; i < n; i++) {
                    for (int k = 0; k < 4; k++)
                        four[k] = src[i];
                    memset(&state, 0, sizeof state);
                    memset(ref, 0, sizeof ref);
                    fns[f].fn(ref, four, 4, skip, &state);
                    checks++;
                    if (memcmp(out + i * skip, ref, fns[f].bytes) != 0) {
                        if (failures < 10)
                            printf("FAIL %s skip=%lu n=%lu i=%lu (%s path) value=%g\n", fns[f].name, skip, n, i,
                                   i >= (n & ~3UL) ? "tail" : "vector", src[i]);
                        failures++;
                    }
                }
            }
        }
    }
    printf("%s: %d/%d conversions consistent between vector and tail paths\n",
           failures ? "FAIL" : "PASS", checks - failures, checks);
    return failures ? 1 : 0;
}
