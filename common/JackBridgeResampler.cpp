/*
Copyright (C) 2026 jack2-nspa

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

*/

#include "JackBridgeResampler.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__)
#define JACK_BRIDGE_X86_SIMD 1
#include <immintrin.h>
#endif

// the per-channel accumulators must stay in registers: unroll the short constant loops
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 8)
#define JACK_UNROLL _Pragma("GCC unroll 8")
#else
#define JACK_UNROLL
#endif

#ifdef __GNUC__
#define JACK_FLATTEN __attribute__((flatten))
#else
#define JACK_FLATTEN
#endif

namespace Jack
{

namespace
{

const int kFrac = JackBridgeResampler::kFracBits;
const uint64_t kOne = uint64_t(1) << kFrac;
const uint64_t kFracMask = kOne - 1;
const double kToDouble = 1.0 / (double)kOne;
const double kPi = 3.14159265358979323846;

/*
 The cutoff sits at the Nyquist frequency of the lower of the two rates, with
 the transition band symmetric around it. Images of passband content then
 land at or above (rate - passband edge), i.e. in the stop band, which gives
 far more attenuation per tap than a cutoff below Nyquist; what remains is
 partly attenuated content between the passband edge and Nyquist folding back
 into that same ultrasonic region. At ratio 1 it also makes the integer phase
 the identity (sinc zeros at the integers).

 half: taps on each side at ratio <= 1. beta: Kaiser parameter, the one knob
 between the far stop band (accuracy at low frequencies) and the transition
 width (accuracy at the top of the band); measured, not derived: for 0 and 1
 the best worst-case SNR up to 20 kHz at 48 kHz drift ratios, for 2 also up to
 20 kHz at 44.1 kHz. phase_bits: table phases, so the error of the linear
 interpolation between phases (about 3.3 * (f / phases)^2 at f cycles per
 input frame) stays below the filter error.
*/
struct Quality {
    int half;
    int phase_bits;
    double beta;
};

const Quality kQualities[3] = {
    { 16, 8, 8.0 },     // 32 taps, 256 phases
    { 28, 10, 14.25 },  // 56 taps, 1024 phases
    { 48, 11, 13.75 },  // 96 taps, 2048 phases
};

// sin(pi x) with exact zeros at the integers, so the ratio-1 kernel is exactly the identity there
double SinPi(double x)
{
    const double n = floor(x + 0.5);
    const double s = sin(kPi * (x - n));
    return (fmod(n, 2.0) != 0.0) ? -s : s;
}

double BesselI0(double x)
{
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    for (int k = 1; k < 500; k++) {
        term *= q / ((double)k * (double)k);
        sum += term;
        if (term < sum * 1e-21) {
            break;
        }
    }
    return sum;
}

void* AlignedAlloc(size_t bytes, void** raw)
{
    // 64 bytes: rows start on a cache line, which also satisfies the 32-byte AVX loads
    *raw = malloc(bytes + 64);
    if (!*raw) {
        return 0;
    }
    uintptr_t p = ((uintptr_t)*raw + 63) & ~(uintptr_t)63;
    return (void*)p;
}

/*
 One-pole step of the smoothed ratio towards the target, in integers so that
 Pull and NeededFor follow exactly the same sequence on every compiler and
 target (a floating-point recurrence could be contracted to FMA in one place
 and not in another). It never overshoots, and snaps to the target once the
 step rounds to zero (the residue is below 2^32 / k units, 1e-15 at k = 0.01).
*/
inline uint64_t SmoothStep(uint64_t ratio, uint64_t target, uint32_t k)
{
    if (ratio == target || k == 0) {
        return target;
    }
    uint64_t d = (target > ratio) ? target - ratio : ratio - target;
    uint64_t step = (d >> 32) * k + (((d & 0xffffffffu) * k) >> 32);
    if (step == 0) {
        return target;
    }
    return (target > ratio) ? ratio + step : ratio - step;
}

/*
 floor(index + frac / 2^56 + m * ratio / 2^56) exactly, for m < 2^20: the
 fraction of the ratio is split in two 28-bit halves so nothing overflows.
*/
inline int64_t FloorAfter(int64_t index, uint64_t frac, uint64_t ratio, uint64_t m)
{
    const uint64_t low28 = (uint64_t(1) << 28) - 1;
    const uint64_t f = ratio & kFracMask;
    const uint64_t hi = m * (f >> 28), lo = m * (f & low28);
    return index + (int64_t)(m * (ratio >> kFrac)) + (int64_t)(hi >> 28)
        + (int64_t)((frac + ((hi & low28) << 28) + lo) >> kFrac);
}

// hot state of one Produce call, kept in locals by the loop
struct Run
{
    const float* table;
    const float* history;
    size_t stride;
    int taps;
    int half;
    int phase_shift;    // kFrac - phase bits
    int channels;
    int64_t base;
    int64_t limit;      // frames pushed
    int64_t index;
    uint64_t frac;
    uint64_t ratio;
    uint64_t target;
    uint32_t k;
};

/*
 The kernels compute the coefficient row of the exact phase,
     row = c0 + a * (c1 - c0),
 one block of 8 taps at a time and use each block for C channels right away,
 so the row is never stored and each channel costs one load and one
 multiply-add per tap. C is a template parameter so the accumulators stay in
 registers.
*/
struct ScalarKernel
{
    enum { kGroup = 2 };    // more channels per pass run out of registers

    template <int C>
    static inline void Dot(const float* x, size_t stride, const float* c0, const float* c1,
                           float a, int taps, float* y)
    {
        // 8 partial sums per channel: the compiler may vectorize, and the summation
        // order matches the SIMD path
        float acc[C][8];
        JACK_UNROLL
        for (int c = 0; c < C; c++) {
            JACK_UNROLL
            for (int k = 0; k < 8; k++) {
                acc[c][k] = 0.f;
            }
        }
        for (int i = 0; i < taps; i += 8) {
            float row[8];
            JACK_UNROLL
            for (int k = 0; k < 8; k++) {
                row[k] = c0[i + k] + a * (c1[i + k] - c0[i + k]);
            }
            JACK_UNROLL
            for (int c = 0; c < C; c++) {
                const float* xc = x + c * stride + i;
                JACK_UNROLL
                for (int k = 0; k < 8; k++) {
                    acc[c][k] += xc[k] * row[k];
                }
            }
        }
        JACK_UNROLL
        for (int c = 0; c < C; c++) {
            const float* s = acc[c];
            y[c] = ((s[0] + s[4]) + (s[2] + s[6])) + ((s[1] + s[5]) + (s[3] + s[7]));
        }
    }
};

#ifdef JACK_BRIDGE_X86_SIMD

// 128 bits, SSE2 only (every x86-64 CPU): mul + add, 4 taps per vector
struct Sse2Kernel
{
    enum { kGroup = 8 };    // 8 accumulators of the 16 xmm registers

    template <int C>
    __attribute__((target("sse2")))
    static inline void Dot(const float* x, size_t stride, const float* c0, const float* c1,
                           float a, int taps, float* y)
    {
        // the add is the dependency chain: two accumulators for one or two channels
        const int kAcc = (C <= 2) ? 2 : 1;
        const __m128 va = _mm_set1_ps(a);
        __m128 acc[kAcc][C];
        JACK_UNROLL
        for (int j = 0; j < kAcc; j++) {
            JACK_UNROLL
            for (int c = 0; c < C; c++) {
                acc[j][c] = _mm_setzero_ps();
            }
        }
        // taps is a multiple of 8: no remainder
        for (int i = 0; i < taps; i += 4 * kAcc) {
            JACK_UNROLL
            for (int j = 0; j < kAcc; j++) {
                const __m128 r0 = _mm_load_ps(c0 + i + 4 * j);
                const __m128 row = _mm_add_ps(r0, _mm_mul_ps(va, _mm_sub_ps(_mm_load_ps(c1 + i + 4 * j), r0)));
                JACK_UNROLL
                for (int c = 0; c < C; c++) {
                    acc[j][c] = _mm_add_ps(acc[j][c], _mm_mul_ps(_mm_loadu_ps(x + c * stride + i + 4 * j), row));
                }
            }
        }
        JACK_UNROLL
        for (int c = 0; c < C; c++) {
            __m128 s = acc[0][c];
            if (kAcc > 1) {
                s = _mm_add_ps(s, acc[kAcc - 1][c]);
            }
            s = _mm_add_ps(s, _mm_movehl_ps(s, s));
            s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 1));
            y[c] = _mm_cvtss_f32(s);
        }
    }
};

struct Avx2Kernel
{
    enum { kGroup = 8 };

    template <int C>
    __attribute__((target("avx2,fma")))
    static inline void Dot(const float* x, size_t stride, const float* c0, const float* c1,
                           float a, int taps, float* y)
    {
        // with one or two channels a second accumulator per channel halves the
        // multiply-add dependency chain; from three on the channels overlap anyway
        const int kAcc = (C <= 2) ? 2 : 1;
        const __m256 va = _mm256_set1_ps(a);
        __m256 acc[kAcc][C];
        JACK_UNROLL
        for (int j = 0; j < kAcc; j++) {
            JACK_UNROLL
            for (int c = 0; c < C; c++) {
                acc[j][c] = _mm256_setzero_ps();
            }
        }
        int i = 0;
        for (; i + 8 * kAcc <= taps; i += 8 * kAcc) {
            JACK_UNROLL
            for (int j = 0; j < kAcc; j++) {
                const __m256 r0 = _mm256_load_ps(c0 + i + 8 * j);
                const __m256 row = _mm256_fmadd_ps(va, _mm256_sub_ps(_mm256_load_ps(c1 + i + 8 * j), r0), r0);
                JACK_UNROLL
                for (int c = 0; c < C; c++) {
                    acc[j][c] = _mm256_fmadd_ps(_mm256_loadu_ps(x + c * stride + i + 8 * j), row, acc[j][c]);
                }
            }
        }
        if (i < taps) {
            const __m256 r0 = _mm256_load_ps(c0 + i);
            const __m256 row = _mm256_fmadd_ps(va, _mm256_sub_ps(_mm256_load_ps(c1 + i), r0), r0);
            JACK_UNROLL
            for (int c = 0; c < C; c++) {
                acc[0][c] = _mm256_fmadd_ps(_mm256_loadu_ps(x + c * stride + i), row, acc[0][c]);
            }
        }
        JACK_UNROLL
        for (int c = 0; c < C; c++) {
            __m256 t = acc[0][c];
            if (kAcc > 1) {
                t = _mm256_add_ps(t, acc[kAcc - 1][c]);
            }
            __m128 s = _mm_add_ps(_mm256_castps256_ps128(t), _mm256_extractf128_ps(t, 1));
            s = _mm_add_ps(s, _mm_movehl_ps(s, s));
            s = _mm_add_ss(s, _mm_movehdup_ps(s));
            y[c] = _mm_cvtss_f32(s);
        }
    }
};

// 512 bits, AVX-512F: 16 taps per vector, the last block of 8 (taps % 16) masked
struct Avx512Kernel
{
    enum { kGroup = 8 };

    template <int C>
    __attribute__((target("avx512f")))
    static inline void Dot(const float* x, size_t stride, const float* c0, const float* c1,
                           float a, int taps, float* y)
    {
        const int kAcc = (C <= 2) ? 2 : 1;
        const __m512 va = _mm512_set1_ps(a);
        __m512 acc[kAcc][C];
        JACK_UNROLL
        for (int j = 0; j < kAcc; j++) {
            JACK_UNROLL
            for (int c = 0; c < C; c++) {
                acc[j][c] = _mm512_setzero_ps();
            }
        }
        // rows are 32-byte aligned only (taps a multiple of 8): unaligned loads
        int i = 0;
        for (; i + 16 * kAcc <= taps; i += 16 * kAcc) {
            JACK_UNROLL
            for (int j = 0; j < kAcc; j++) {
                const __m512 r0 = _mm512_loadu_ps(c0 + i + 16 * j);
                const __m512 row = _mm512_fmadd_ps(va, _mm512_sub_ps(_mm512_loadu_ps(c1 + i + 16 * j), r0), r0);
                JACK_UNROLL
                for (int c = 0; c < C; c++) {
                    acc[j][c] = _mm512_fmadd_ps(_mm512_loadu_ps(x + c * stride + i + 16 * j), row, acc[j][c]);
                }
            }
        }
        if (kAcc > 1 && i + 16 <= taps) {
            const __m512 r0 = _mm512_loadu_ps(c0 + i);
            const __m512 row = _mm512_fmadd_ps(va, _mm512_sub_ps(_mm512_loadu_ps(c1 + i), r0), r0);
            JACK_UNROLL
            for (int c = 0; c < C; c++) {
                acc[0][c] = _mm512_fmadd_ps(_mm512_loadu_ps(x + c * stride + i), row, acc[0][c]);
            }
            i += 16;
        }
        if (i < taps) {
            // 8 taps left: the masked-off lanes load nothing and add zero
            const __mmask16 m = 0x00ff;
            const __m512 r0 = _mm512_maskz_loadu_ps(m, c0 + i);
            const __m512 row = _mm512_fmadd_ps(va, _mm512_sub_ps(_mm512_maskz_loadu_ps(m, c1 + i), r0), r0);
            JACK_UNROLL
            for (int c = 0; c < C; c++) {
                acc[0][c] = _mm512_fmadd_ps(_mm512_maskz_loadu_ps(m, x + c * stride + i), row, acc[0][c]);
            }
        }
        JACK_UNROLL
        for (int c = 0; c < C; c++) {
            __m512 t = acc[0][c];
            if (kAcc > 1) {
                t = _mm512_add_ps(t, acc[kAcc - 1][c]);
            }
            const __m256 h = _mm256_add_ps(_mm512_castps512_ps256(t),
                                           _mm256_castpd_ps(_mm512_extractf64x4_pd(_mm512_castps_pd(t), 1)));
            __m128 s = _mm_add_ps(_mm256_castps256_ps128(h), _mm256_extractf128_ps(h, 1));
            s = _mm_add_ps(s, _mm_movehl_ps(s, s));
            s = _mm_add_ss(s, _mm_movehdup_ps(s));
            y[c] = _mm_cvtss_f32(s);
        }
    }
};

#endif

/*
 Produce up to n outputs for C channels, stopping when the next one would need
 a frame not yet pushed. One loop per channel count keeps everything in
 registers; wider layouts take several passes over the same positions.
*/
template <class K, int C>
inline int Loop(Run& r, const float* history, float* const* out, int n)
{
    const float* table = r.table;
    const size_t stride = r.stride;
    const int taps = r.taps;
    const int shift = r.phase_shift;
    const int64_t last = r.limit - r.half;  // the output at index i reads up to i + half
    const int64_t offset = r.base + (r.half - 1);
    const uint64_t target = r.target;
    const uint32_t k = r.k;
    int64_t index = r.index;
    uint64_t frac = r.frac;
    uint64_t ratio = r.ratio;
    float* dst[C];
    JACK_UNROLL
    for (int c = 0; c < C; c++) {
        dst[c] = out[c];
    }

    int o = 0;
    for (; o < n && index < last; o++) {
        const float* c0 = table + (size_t)(frac >> shift) * taps;
        // 24 bits of the position between this phase and the next
        const float a = (float)(int32_t)((frac >> (shift - 24)) & 0xffffff) * (1.0f / 16777216.0f);
        float y[C];
        K::template Dot<C>(history + (index - offset), stride, c0, c0 + taps, a, taps, y);
        JACK_UNROLL
        for (int c = 0; c < C; c++) {
            dst[c][o] = y[c];
        }
        ratio = SmoothStep(ratio, target, k);
        frac += ratio;
        index += (int64_t)(frac >> kFrac);
        frac &= kFracMask;
    }

    r.index = index;
    r.frac = frac;
    r.ratio = ratio;
    return o;
}

// Loop<K, count> for count in 1..C, chosen at run time
template <class K, int C>
struct Group
{
    static inline int Run(int count, ::Jack::Run& r, const float* history, float* const* out, int n)
    {
        return (count == C) ? Loop<K, C>(r, history, out, n) : Group<K, C - 1>::Run(count, r, history, out, n);
    }
};

template <class K>
struct Group<K, 0>
{
    static inline int Run(int, ::Jack::Run&, const float*, float* const*, int)
    {
        return 0;
    }
};

template <class K>
inline int Passes(Run& r, float* const* out, int n)
{
    const Run start = r;
    int done = 0;
    for (int ch = 0; ch < start.channels; ch += K::kGroup) {
        r = start;
        const int left = start.channels - ch;
        done = Group<K, K::kGroup>::Run((left < K::kGroup) ? left : (int)K::kGroup, r,
                                        start.history + ch * start.stride, out + ch, n);
    }
    return done;
}

// one function per instruction set, everything inlined into it
JACK_FLATTEN
int LoopScalar(Run& r, float* const* out, int n)
{
    return Passes<ScalarKernel>(r, out, n);
}

#ifdef JACK_BRIDGE_X86_SIMD
__attribute__((target("sse2"), flatten))
int LoopSse2(Run& r, float* const* out, int n)
{
    return Passes<Sse2Kernel>(r, out, n);
}

__attribute__((target("avx2,fma"), flatten))
int LoopAvx2(Run& r, float* const* out, int n)
{
    return Passes<Avx2Kernel>(r, out, n);
}

__attribute__((target("avx512f"), flatten))
int LoopAvx512(Run& r, float* const* out, int n)
{
    return Passes<Avx512Kernel>(r, out, n);
}
#endif

bool VariantSupported(int variant)
{
    if (variant == JackBridgeResampler::kPortable) {
        return true;
    }
#ifdef JACK_BRIDGE_X86_SIMD
    __builtin_cpu_init();
    switch (variant) {
        case JackBridgeResampler::kSse2:
            return __builtin_cpu_supports("sse2");
        case JackBridgeResampler::kAvx2:
            return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
        case JackBridgeResampler::kAvx512:
            return __builtin_cpu_supports("avx512f");
    }
#endif
    return false;
}

const char* const kVariantNames[4] = { "portable", "sse2", "avx2", "avx512" };

} // namespace

JackBridgeResampler::JackBridgeResampler()
    : fChannels(0), fTaps(0), fHalf(0), fPhaseBits(0), fCapacity(0), fStride(0), fTable(0),
      fHistory(0), fTableMem(0), fHistoryMem(0), fPushed(0), fBase(0), fIndex(0), fFrac(0),
      fRatio(kOne), fNominal(kOne), fMinRatio(kOne), fMaxRatio(kOne), fSmoothK(0),
      fSimd(false), fSimdVariant(kPortable)
{
    SetRatioSmoothing(100);
}

JackBridgeResampler::~JackBridgeResampler()
{
    Release();
}

void JackBridgeResampler::Release()
{
    free(fTableMem);
    free(fHistoryMem);
    fTableMem = fHistoryMem = 0;
    fTable = fHistory = 0;
    fChannels = 0;
}

bool JackBridgeResampler::Init(int channels, double nominal_ratio, int quality, int max_in, int max_out)
{
    Release();
    if (channels < 1 || channels > 4096 || max_in < 1 || max_out < 1
        || max_in > (1 << 20) || max_out > (1 << 20)
        || !(nominal_ratio >= 0.2 && nominal_ratio <= 5.0)) {
        return false;
    }
    if (quality < 0) {
        quality = 0;
    } else if (quality > 2) {
        quality = 2;
    }
    const Quality& q = kQualities[quality];

    // Downsampling: cutoff at the output Nyquist, kernel and taps stretched by the ratio.
    // Up to 2% above 1 the band lost above the passband edge is not worth the taps.
    const double scale = (nominal_ratio > 1.02) ? nominal_ratio : 1.0;
    int half = (int)ceil(q.half * scale);
    half = (half + 3) & ~3;  // taps a multiple of 8
    int phase_bits = q.phase_bits;
    // the stretched kernel is smoother: fewer phases keep the same interpolation error
    for (double s = scale; s >= 2.0 && phase_bits > 6; s /= 2.0) {
        phase_bits--;
    }
    const int taps = 2 * half;
    const int phases = 1 << phase_bits;

    fTable = (float*)AlignedAlloc((size_t)(phases + 1) * taps * sizeof(float), &fTableMem);
    if (!fTable) {
        Release();
        return false;
    }

    // Row p, tap j multiplies input frame floor(centre) - (half - 1) + j at the centre
    // fraction p / phases. Each row is normalized to unit DC gain, so the interpolated rows
    // are too and low frequencies do not ripple with the phase.
    double* row = (double*)malloc(taps * sizeof(double));
    if (!row) {
        Release();
        return false;
    }
    const double i0_beta = BesselI0(q.beta);
    for (int p = 0; p <= phases; p++) {
        double sum = 0.0;
        const double f = (double)p / phases;
        for (int j = 0; j < taps; j++) {
            const double t = j - (half - 1) - f;
            const double u = t / half;
            double h = 0.0;
            if (fabs(u) < 1.0) {
                const double w = BesselI0(q.beta * sqrt(1.0 - u * u)) / i0_beta;
                const double x = t / scale;
                h = (x == 0.0) ? 1.0 : SinPi(x) / (kPi * x);
                h *= w / scale;
            }
            row[j] = h;
            sum += h;
        }
        float* dst = fTable + (size_t)p * taps;
        for (int j = 0; j < taps; j++) {
            dst[j] = (float)(row[j] / sum);
        }
    }
    free(row);

    // Room for one full Pull at the largest ratio plus one Push on top of the filter span,
    // doubled so a Push rarely has to slide the history and some imbalance fits.
    const double max_ratio = nominal_ratio * 1.25;
    int64_t capacity = 2 * ((int64_t)taps + max_in + (int64_t)ceil(max_out * max_ratio) + 8);
    capacity = (capacity + 7) & ~(int64_t)7;
    fHistory = (float*)AlignedAlloc((size_t)capacity * channels * sizeof(float), &fHistoryMem);
    if (!fHistory) {
        Release();
        return false;
    }
    memset(fHistory, 0, (size_t)capacity * channels * sizeof(float));

    fChannels = channels;
    fTaps = taps;
    fHalf = half;
    fPhaseBits = phase_bits;
    fCapacity = (int)capacity;
    fStride = (size_t)capacity;
    fNominal = (uint64_t)(nominal_ratio * (double)kOne);
    fMinRatio = (uint64_t)(nominal_ratio * 0.8 * (double)kOne);
    fMaxRatio = (uint64_t)(max_ratio * (double)kOne);

    // AVX2 + FMA; else SSE2 on x86-64 (on i386 its 8 xmm registers are too few)
    fSimdVariant = VariantSupported(kAvx2) ? (int)kAvx2 : (int)kPortable;
#ifdef __x86_64__
    if (fSimdVariant == kPortable && VariantSupported(kSse2)) {
        fSimdVariant = kSse2;
    }
#endif
    // measurement: another variant by name
    if (const char* name = getenv("JACK_BRIDGE_RESAMPLER")) {
        for (int v = 0; v < 4; v++) {
            if (!strcmp(name, kVariantNames[v]) && VariantSupported(v)) {
                fSimdVariant = v;
            }
        }
    }
    fSimd = (fSimdVariant != kPortable);

    Reset();
    return true;
}

void JackBridgeResampler::Reset()
{
    fPushed = 0;
    fIndex = 0;
    fFrac = 0;
    fRatio = fNominal;
    // the half - 1 frames before position 0 that the first outputs read are silence
    fBase = -(int64_t)(fHalf - 1);
    for (int c = 0; c < fChannels; c++) {
        memset(fHistory + c * fStride, 0, (size_t)(fHalf - 1) * sizeof(float));
    }
}

void JackBridgeResampler::SetRatioSmoothing(int samples)
{
    if (samples <= 0) {
        fSmoothK = 0;
        return;
    }
    double k = (1.0 - exp(-1.0 / samples)) * 4294967296.0;
    fSmoothK = (k < 1.0) ? 1u : (k > 4294967295.0) ? 4294967295u : (uint32_t)(k + 0.5);
}

void JackBridgeResampler::ForceScalar(bool scalar)
{
    fSimd = (fSimdVariant != kPortable) && !scalar;
}

bool JackBridgeResampler::ForceVariant(int variant)
{
    if (variant < kPortable || variant > kAvx512 || !VariantSupported(variant)) {
        return false;
    }
    fSimdVariant = variant;
    fSimd = (variant != kPortable);
    return true;
}

const char* JackBridgeResampler::VariantName(int variant)
{
    return (variant >= kPortable && variant <= kAvx512) ? kVariantNames[variant] : "?";
}

int JackBridgeResampler::Push(const float* const* in, int frames)
{
    if (!fHistory || frames <= 0) {
        return 0;
    }
    int64_t end = fPushed - fBase;
    if (end + frames > fCapacity) {
        // slide the frames still needed (from the oldest tap of the next output) to the front
        int64_t keep = fIndex - (fHalf - 1);
        if (keep > fPushed) {
            keep = fPushed;
        }
        if (keep > fBase) {
            const size_t shift = (size_t)(keep - fBase);
            const size_t live = (size_t)(fPushed - keep);
            for (int c = 0; c < fChannels; c++) {
                float* row = fHistory + c * fStride;
                memmove(row, row + shift, live * sizeof(float));
            }
            fBase = keep;
            end = fPushed - fBase;
        }
        if (end + frames > fCapacity) {
            frames = (int)(fCapacity - end);
        }
    }
    for (int c = 0; c < fChannels; c++) {
        memcpy(fHistory + c * fStride + end, in[c], (size_t)frames * sizeof(float));
    }
    fPushed += frames;
    return frames;
}

uint64_t JackBridgeResampler::TargetExact(double ratio) const
{
    // clamp in double (also catches NaN); the product is an exact integer for ratio >= 1/16
    const double lo = (double)fMinRatio * kToDouble, hi = (double)fMaxRatio * kToDouble;
    if (!(ratio >= lo)) {
        return (ratio != ratio) ? fNominal : fMinRatio;
    }
    if (ratio >= hi) {
        return fMaxRatio;
    }
    return (uint64_t)(ratio * (double)kOne);
}

int64_t JackBridgeResampler::NeededFor(int n, double ratio) const
{
    return Needed(n, TargetExact(ratio));
}

int64_t JackBridgeResampler::Needed(int n, uint64_t target) const
{
    if (n <= 0) {
        return 0;
    }
    // The smoothed ratio moves monotonically from fRatio to the target, so the centre of
    // the last output lies between the centres at either constant ratio. When both floor
    // to the same frame that is the exact answer; otherwise walk the smoothing.
    const uint64_t m = (uint64_t)(n - 1);
    if (m < (uint64_t(1) << 20)) {
        const uint64_t lo = (fRatio < target) ? fRatio : target;
        const uint64_t hi = (fRatio < target) ? target : fRatio;
        const int64_t first = FloorAfter(fIndex, fFrac, lo, m);
        if (lo == hi || first == FloorAfter(fIndex, fFrac, hi, m)) {
            return first + fHalf + 1;
        }
    }
    int64_t index = fIndex;
    uint64_t frac = fFrac, r = fRatio;
    for (int i = 1; i < n; i++) {
        r = SmoothStep(r, target, fSmoothK);
        frac += r;
        index += (int64_t)(frac >> kFrac);
        frac &= kFracMask;
    }
    return index + fHalf + 1;
}

int JackBridgeResampler::Produce(float* const* out, int n, uint64_t target)
{
    Run r;
    r.table = fTable;
    r.history = fHistory;
    r.stride = fStride;
    r.taps = fTaps;
    r.half = fHalf;
    r.phase_shift = kFrac - fPhaseBits;
    r.channels = fChannels;
    r.base = fBase;
    r.limit = fPushed;
    r.index = fIndex;
    r.frac = fFrac;
    r.ratio = fRatio;
    r.target = target;
    r.k = fSmoothK;
    int done;
    switch (ActiveVariant()) {
#ifdef JACK_BRIDGE_X86_SIMD
        case kSse2:
            done = LoopSse2(r, out, n);
            break;
        case kAvx2:
            done = LoopAvx2(r, out, n);
            break;
        case kAvx512:
            done = LoopAvx512(r, out, n);
            break;
#endif
        default:
            done = LoopScalar(r, out, n);
            break;
    }
    fIndex = r.index;
    fFrac = r.frac;
    fRatio = r.ratio;
    return done;
}

int JackBridgeResampler::Pull(float* const* out, int n, double target_ratio)
{
    if (!fTable || n <= 0) {
        return 0;
    }
    const uint64_t target = TargetExact(target_ratio);
    if (Needed(n, target) > fPushed) {
        return 0;
    }
    return Produce(out, n, target);
}

int JackBridgeResampler::PullAvailable(float* const* out, int max_n, double target_ratio)
{
    if (!fTable || max_n <= 0) {
        return 0;
    }
    return Produce(out, max_n, TargetExact(target_ratio));
}

double JackBridgeResampler::Center() const
{
    return (double)fIndex + (double)fFrac * kToDouble;
}

double JackBridgeResampler::CurrentRatio() const
{
    return (double)fRatio * kToDouble;
}

} // end of namespace
