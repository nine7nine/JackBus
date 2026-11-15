/* JackBus: float -> integer conversion consistency (common/memops.c).
 *
 * The SIMD conversion functions process groups of 4 samples in a vector loop
 * and the remainder in a scalar tail. The same input value must produce the
 * same output bytes in either path. For every non-dithered conversion,
 * every length 1..23 and both packed and interleaved (stereo) strides,
 * each sample's output is compared with the output for that value produced
 * by the vector path (a 4-sample buffer filled with it).
 *
 * With -DWITH_REFERENCE, each function is also compared with the portable C
 * implementation of the same file (memops.c built with -U__SSE2__, symbols
 * renamed to ref_*): the SIMD paths must give bit-identical results.
 *
 * Build: cc -O2 <arch flags> -I common -I common/jack memops_consistency.c common/memops.c -lm
 */
#include <string.h>  /* memops.h needs memcpy */
#include "memops.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*move_fn)(char *, jack_default_audio_sample_t *, unsigned long, unsigned long, dither_state_t *);

#ifdef WITH_REFERENCE
#define REF(name) void ref_sample_move_##name(char *, jack_default_audio_sample_t *, unsigned long, unsigned long, dither_state_t *);
REF(d32_sSs) REF(d32_sS) REF(d32u24_sSs) REF(d32u24_sS) REF(d32l24_sSs) REF(d32l24_sS)
REF(d24_sSs) REF(d24_sS) REF(d16_sSs) REF(d16_sS)
#define ENTRY(name, bytes) {#name, sample_move_##name, ref_sample_move_##name, bytes}
#else
#define ENTRY(name, bytes) {#name, sample_move_##name, NULL, bytes}
#endif

static const struct { const char *name; move_fn fn; move_fn ref; int bytes; } fns[] = {
    ENTRY(d32_sSs, 4), ENTRY(d32_sS, 4), ENTRY(d32u24_sSs, 4), ENTRY(d32u24_sS, 4),
    ENTRY(d32l24_sSs, 4), ENTRY(d32l24_sS, 4), ENTRY(d24_sSs, 3), ENTRY(d24_sS, 3),
    ENTRY(d16_sSs, 2), ENTRY(d16_sS, 2),
};

static float sample_value(unsigned i)
{
    static const float edges[] = {0.f, 1.f, -1.f, 0.5f, -0.5f, 1.25f, -1.25f, 1e-7f, -1e-7f, 0.999999f,
                                  -0.f, __builtin_inff(), -__builtin_inff()};  /* NaN: undefined (paths differ) */
    if (i < sizeof edges / sizeof edges[0])
        return edges[i];
    return ((float)rand() / RAND_MAX) * 2.5f - 1.25f;
}

int main(void)
{
    static jack_default_audio_sample_t src[32] __attribute__((aligned(32)));
    static jack_default_audio_sample_t four[4] __attribute__((aligned(32)));
    static char out[32 * 8], ref[4 * 8], portable[32 * 8];
    dither_state_t state;
    int failures = 0, checks = 0, ref_failures = 0, ref_checks = 0;

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
                if (fns[f].ref) {
                    memset(&state, 0, sizeof state);
                    memset(portable, 0, sizeof portable);
                    fns[f].ref(portable, src, n, skip, &state);
                    for (unsigned long i = 0; i < n; i++) {
                        ref_checks++;
                        if (memcmp(out + i * skip, portable + i * skip, fns[f].bytes) != 0) {
                            if (ref_failures < 10)
                                printf("FAIL %s skip=%lu n=%lu i=%lu value=%.9g differs from the portable implementation\n",
                                       fns[f].name, skip, n, i, src[i]);
                            ref_failures++;
                        }
                    }
                }
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
    printf("%s: %d/%d conversions consistent between vector and tail paths", failures || ref_failures ? "FAIL" : "PASS",
           checks - failures, checks);
    if (ref_checks)
        printf(", %d/%d identical to the portable implementation", ref_checks - ref_failures, ref_checks);
    printf("\n");
    return (failures || ref_failures) ? 1 : 0;
}
