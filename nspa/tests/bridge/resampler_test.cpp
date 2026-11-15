/*
 jack2-nspa: JackBridgeResampler quality, continuity, accounting, delay and speed.

 Build and run from the repository root (about 40 s; --quick: about 8 s):

   g++ -std=gnu++11 -O2 -march=x86-64 -Icommon common/JackBridgeResampler.cpp nspa/tests/bridge/resampler_test.cpp -o /tmp/resampler_test
   taskset -c 2 /tmp/resampler_test [--quick] [--no-speed]

 No hardware, no server. Every measurement compares the output with the ideal: the
 input signal evaluated in double precision at the exact absolute input position
 (CenterExact()) of each output, so ratio changes, smoothing and block splitting are
 all accounted for and only the resampler's own error remains.

  1. accuracy: sines at -1 dBFS, SNR against the ideal and THD+N (after fitting the
     amplitude and phase, i.e. without the linear passband error), all qualities;
     PASS/FAIL against the targets for quality 1 at drift ratios. Ratio 1.0 is
     measured at centre fraction 0.5, the worst case (at fraction 0 the output is
     the input, bit for bit, see 5.)
  2. passband ripple, alias rejection (96k to 48k) and image rejection (16k to 48k)
  3. continuity: the target ratio stepped every 64 outputs, and a 1% jump
  4. accounting: 10 million frames in random blocks at a random ratio; the centre is
     the exact sum of the smoothed ratios (an independent model of the documented
     recurrence), NeededFor matches the model, Pull and NeededFor agree, and the
     output is bit-identical to one made one frame at a time with input pushed just
     in time (so Pull never reads beyond Pushed())
  5. delay: an impulse comes out at the output whose centre is its position
  6. speed: ns and CPU cycles per frame and channel, median of several runs, pinned
     to one core

 Exits non-zero if a check fails.
*/

#include "JackBridgeResampler.h"

#include <algorithm>
#include <linux/perf_event.h>
#include <math.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <vector>

using namespace Jack;

static int gPass = 0, gFail = 0;
static bool gQuick = false;

static void Check(bool ok, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void Check(bool ok, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("%s  ", ok ? "PASS" : "FAIL");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    (ok ? gPass : gFail)++;
}

static const double kTwoPi = 6.283185307179586476925;
static const double kAmp = 0.89125093813374556;     // -1 dBFS
static const double kFracScale = 1.0 / 72057594037927936.0;   // 2^-56

static double Db(double x)
{
    return (x > 0) ? 10.0 * log10(x) : -999.0;
}

// absolute input position of the next output, in frames
static void Where(const JackBridgeResampler& r, int64_t* frames, double* frac)
{
    uint64_t f;
    r.CenterExact(frames, &f);
    *frac = (double)f * kFracScale;
}

//------------------------------------------------------------------------------
// A sine through the resampler, one output at a time so each output's exact
// centre is known (test 4 shows Pull(1) n times equals Pull(n)).

struct SineRun
{
    int quality;
    double nominal;
    double target;
    double w;           // radians per input frame
    double phase;       // initial fractional position, nominal 1 only
    bool simd;
    int skip;           // warm-up outputs
    int count;          // measured outputs
};

struct SineResult
{
    double snr;     // against the ideal sine, dB
    double thdn;    // after fitting amplitude and phase, dB
    double gain;    // fitted amplitude / input amplitude
    double rms;     // output power / input power, dB (alias rejection)
};

static SineResult RunSine(const SineRun& s)
{
    JackBridgeResampler r;
    r.Init(1, s.nominal, s.quality, 256, 256);
    r.ForceScalar(!s.simd);
    std::vector<float> in(256);
    const float* ip[1] = { &in[0] };
    float y;
    float* op[1] = { &y };
    int64_t pushed = 0;

    // fractional start: two outputs at 1 + phase / 2 without smoothing
    if (s.phase > 0) {
        r.SetRatioSmoothing(0);
    }

    std::vector<double> out, ideal, sn, cs;
    out.reserve(s.count);
    ideal.reserve(s.count);
    sn.reserve(s.count);
    cs.reserve(s.count);
    const int total = s.skip + s.count;
    for (int k = 0; k < total; k++) {
        double t = s.target;
        if (s.phase > 0) {
            if (k < 2) {
                t = 1.0 + s.phase / 2;
            } else if (k == 2) {
                t = 1.0;
            } else if (k == 3) {
                r.SetRatioSmoothing(100);
            }
        }
        while (r.Pushed() < r.NeededFor(1, t)) {
            for (int i = 0; i < 256; i++) {
                in[i] = (float)(kAmp * sin(s.w * (double)(pushed + i)));
            }
            pushed += r.Push(ip, 256);
        }
        int64_t frames;
        double frac;
        Where(r, &frames, &frac);
        if (r.Pull(op, 1, t) != 1) {
            printf("\nFAIL  Pull(1) refused although Pushed() >= NeededFor(1)\n");
            exit(1);
        }
        if (k < s.skip) {
            continue;
        }
        const double th = s.w * (double)frames + s.w * frac;
        out.push_back(y);
        sn.push_back(sin(th));
        cs.push_back(cos(th));
        ideal.push_back(kAmp * sin(th));
    }

    // least-squares fit y = a sin + b cos
    double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0;
    for (size_t i = 0; i < out.size(); i++) {
        ss += sn[i] * sn[i];
        cc += cs[i] * cs[i];
        sc += sn[i] * cs[i];
        ys += out[i] * sn[i];
        yc += out[i] * cs[i];
    }
    const double det = ss * cc - sc * sc;
    const double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
    double p_ideal = 0, p_err = 0, p_fit = 0, p_res = 0, p_out = 0;
    for (size_t i = 0; i < out.size(); i++) {
        const double e = out[i] - ideal[i];
        const double f = a * sn[i] + b * cs[i];
        p_ideal += ideal[i] * ideal[i];
        p_err += e * e;
        p_fit += f * f;
        p_res += (out[i] - f) * (out[i] - f);
        p_out += out[i] * out[i];
    }
    SineResult res;
    res.snr = Db(p_ideal / p_err);
    res.thdn = Db(p_fit / p_res);
    res.gain = sqrt(a * a + b * b) / kAmp;
    res.rms = Db(p_out / p_ideal);
    return res;
}

static SineRun Sine(int quality, double nominal, double target, double hz, double in_rate)
{
    SineRun s;
    s.quality = quality;
    s.nominal = nominal;
    s.target = target;
    s.w = kTwoPi * hz / in_rate;
    s.phase = 0;
    s.simd = true;
    s.skip = 4096;
    s.count = gQuick ? 32768 : 131072;
    return s;
}

//------------------------------------------------------------------------------
// 1. accuracy

struct Case
{
    const char* name;
    double in_rate, out_rate, drift;    // nominal = in / out, target = nominal * drift
    double phase;
    bool target;                        // the quality-1 targets apply
};

static void Accuracy()
{
    printf("\n== 1. accuracy: sine at -1 dBFS, SNR against the ideal / THD+N (fitted), dB ==\n");
    const Case cases[] = {
        { "48k ratio 1.0 (phase 0.5)", 48000, 48000, 1.0, 0.5, true },
        { "48k ratio 1.0001", 48000, 48000, 1.0001, 0, true },
        { "48k ratio 0.9997", 48000, 48000, 0.9997, 0, true },
        { "48k ratio 1.0005", 48000, 48000, 1.0005, 0, true },
        { "44.1k -> 48k", 44100, 48000, 1.0, 0, false },
        { "48k -> 44.1k", 48000, 44100, 1.0, 0, false },
        { "16k -> 48k", 16000, 48000, 1.0, 0, false },
    };
    const double freqs[5] = { 100, 1000, 10000, 18000, 20000 };
    const double freqs16[5] = { 100, 1000, 3000, 6000, 7000 };
    // quality 1 at drift ratios
    const double target_db[5] = { 110, 110, 110, 100, 90 };

    for (int q = 0; q < 3; q++) {
        JackBridgeResampler info;
        info.Init(1, 1.0, q, 256, 256);
        printf("\nquality %d: %d taps, %d phases\n", q, info.Taps(), info.Phases());
        printf("  %-26s", "case \\ Hz");
        for (int i = 0; i < 5; i++) {
            printf("  %15.0f", freqs[i]);
        }
        printf("\n");
        for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            const Case& k = cases[c];
            const double* f = (k.in_rate == 16000) ? freqs16 : freqs;
            const double nominal = k.in_rate / k.out_rate;
            printf("  %-26s", k.name);
            SineResult res[5];
            for (int i = 0; i < 5; i++) {
                SineRun s = Sine(q, nominal, nominal * k.drift, f[i], k.in_rate);
                s.phase = k.phase;
                res[i] = RunSine(s);
                if (k.in_rate == 16000) {
                    printf("  %5.0f:%4.0f/%4.0f", f[i], res[i].snr, res[i].thdn);
                } else {
                    printf("  %7.1f / %5.1f", res[i].snr, res[i].thdn);
                }
            }
            printf("\n");
            if (q == 1 && k.target) {
                for (int i = 0; i < 5; i++) {
                    Check(res[i].snr >= target_db[i], "q1 %s %5.0f Hz: SNR %.1f dB (target %.0f)",
                          k.name, f[i], res[i].snr, target_db[i]);
                }
            }
        }
    }

}

//------------------------------------------------------------------------------
// 2. passband, aliases and images

static void Spectrum()
{
    printf("\n== 2. passband ripple, alias and image rejection ==\n");
    for (int q = 0; q < 3; q++) {
        // gain over 20 Hz .. 20 kHz at 48k drift (all phases), from the fitted amplitude
        double worst = 0, worst_hz = 0, edge = 0;
        const int points = gQuick ? 40 : 100;
        for (int i = 0; i <= points; i++) {
            const double hz = (i < points / 2) ? 20.0 * pow(10000.0 / 20.0, i / (points / 2.0))
                              : 10000.0 + 10000.0 * (i - points / 2) / (points - points / 2);
            SineRun s = Sine(q, 1.0, 1.0001, hz, 48000);
            s.count = gQuick ? 16384 : 32768;
            const double g = 20.0 * log10(RunSine(s).gain);
            if (fabs(g) > fabs(worst)) {
                worst = g;
                worst_hz = hz;
            }
        }
        // the frequency where the gain leaves +-0.001 dB
        for (double hz = 15000; hz <= 24000; hz += 100) {
            SineRun s = Sine(q, 1.0, 1.0001, hz, 48000);
            s.count = 16384;
            if (fabs(20.0 * log10(RunSine(s).gain)) > 0.001) {
                edge = hz;
                break;
            }
        }
        printf("quality %d: passband ripple 20 Hz..20 kHz %+.6f dB (at %.0f Hz), +-0.001 dB up to %.1f kHz\n",
               q, worst, worst_hz, edge / 1000.0);
        if (q == 1) {
            Check(fabs(worst) < 0.001, "q1 passband ripple %.6f dB < 0.001 dB up to 20 kHz", fabs(worst));
        }
    }

    for (int q = 0; q < 3; q++) {
        // 96k -> 48k: a 30 kHz tone would alias to 18 kHz; the worst tone that aliases below 20 kHz
        SineRun s = Sine(q, 2.0, 2.0, 30000, 96000);
        const double at30 = RunSine(s).rms;
        double worst = -999, worst_hz = 0;
        for (double hz = 28000; hz <= 47000; hz += (gQuick ? 2000 : 500)) {
            SineRun t = Sine(q, 2.0, 2.0, hz, 96000);
            t.count = 16384;
            const double a = RunSine(t).rms;
            if (a > worst) {
                worst = a;
                worst_hz = hz;
            }
        }
        // 16k -> 48k: images of a 1 kHz and a 6 kHz tone (15/17 kHz, 10/22 kHz)
        SineResult i1 = RunSine(Sine(q, 1.0 / 3, 1.0 / 3, 1000, 16000));
        SineResult i6 = RunSine(Sine(q, 1.0 / 3, 1.0 / 3, 6000, 16000));
        printf("quality %d: 96k->48k alias of 30 kHz %.1f dB, worst 28..47 kHz %.1f dB (%.1f kHz); "
               "16k->48k images of 1 kHz %.1f dB, of 6 kHz %.1f dB\n",
               q, at30, worst, worst_hz / 1000.0, -i1.thdn, -i6.thdn);
        if (q == 1) {
            Check(at30 < -100.0, "q1 96k->48k: 30 kHz alias at %.1f dB (< -100)", at30);
            Check(-i1.thdn < -100.0, "q1 16k->48k: 1 kHz images at %.1f dB (< -100)", -i1.thdn);
        }
    }
}

//------------------------------------------------------------------------------
// 3. continuity

static void Continuity(const char* name, int smoothing, int steps, double (*target)(int))
{
    JackBridgeResampler r;
    r.Init(1, 1.0, 1, 256, 256);
    r.SetRatioSmoothing(smoothing);
    const double w = kTwoPi * 1000.0 / 48000.0;
    std::vector<float> in(256);
    const float* ip[1] = { &in[0] };
    float y;
    float* op[1] = { &y };
    int64_t pushed = 0;
    double worst = 0, worst_step = 0, worst_dratio = 0, last_e = 0, last_ratio = r.CurrentRatio();
    for (int k = 0; k < steps; k++) {
        const double t = target(k);
        while (r.Pushed() < r.NeededFor(1, t)) {
            for (int i = 0; i < 256; i++) {
                in[i] = (float)(kAmp * sin(w * (double)(pushed + i)));
            }
            pushed += r.Push(ip, 256);
        }
        int64_t frames;
        double frac;
        Where(r, &frames, &frac);
        r.Pull(op, 1, t);
        worst_dratio = std::max(worst_dratio, fabs(r.CurrentRatio() - last_ratio));
        last_ratio = r.CurrentRatio();
        if (k < 1000) {
            continue;
        }
        const double e = y - kAmp * sin(w * (double)frames + w * frac);
        worst = std::max(worst, fabs(e));
        if (k > 1000) {
            worst_step = std::max(worst_step, fabs(e - last_e));
        }
        last_e = e;
    }
    printf("%-44s worst error %.1f dBFS, worst error step %.1f dBFS, largest ratio step per output %.2g\n",
           name, 20 * log10(worst), 20 * log10(worst_step), worst_dratio);
    if (smoothing > 0) {
        Check(20 * log10(worst) < -100.0, "%s: no click (worst error %.1f dBFS < -100)", name, 20 * log10(worst));
    }
}

static double Alternate(int k)
{
    return ((k / 64) & 1) ? 1.0 + 500e-6 : 1.0 - 500e-6;
}

static double Jump(int k)
{
    return (k >= 30000 && k < 90000) ? 1.01 : 1.0;
}

static void Continuity()
{
    printf("\n== 3. continuity: 1 kHz at 48k, quality 1, error against the ideal at the exact positions ==\n"
           "(without smoothing the output stays exact too, but the ratio, i.e. the pitch, steps by the\n"
           "full 1000 ppm at once: the zipper the smoothing removes)\n");
    Continuity("1 -+ 500 ppm every 64 outputs, smoothing 100", 100, 200000, Alternate);
    Continuity("1 -+ 500 ppm every 64 outputs, no smoothing", 0, 200000, Alternate);
    Continuity("1.0 -> 1.01 -> 1.0, smoothing 100", 100, 120000, Jump);
}

//------------------------------------------------------------------------------
// 4. accounting

static uint64_t gSeed = 0x2545f4914f6cdd1dull;

static uint64_t Rand()
{
    gSeed ^= gSeed << 13;
    gSeed ^= gSeed >> 7;
    gSeed ^= gSeed << 17;
    return gSeed;
}

static double Uniform()
{
    return (double)(Rand() >> 11) * (1.0 / 9007199254740992.0);
}

// the input: a deterministic function of the absolute frame and channel
static float Noise(int64_t frame, int channel)
{
    uint64_t x = (uint64_t)frame * 0x9e3779b97f4a7c15ull + (uint64_t)channel * 0xbf58476d1ce4e5b9ull;
    x ^= x >> 31;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 29;
    return (float)((double)(x >> 40) / 8388608.0 - 1.0) * 0.5f;
}

struct Feeder
{
    int channels;
    int64_t next;
    std::vector<float> buf[8];

    Feeder(int c, int max) : channels(c), next(0)
    {
        for (int i = 0; i < c; i++) {
            buf[i].resize(max);
        }
    }

    int Push(JackBridgeResampler& r, int n)
    {
        const float* p[8];
        for (int c = 0; c < channels; c++) {
            for (int i = 0; i < n; i++) {
                buf[c][i] = Noise(next + i, c);
            }
            p[c] = &buf[c][0];
        }
        const int got = r.Push(p, n);
        next += got;
        return got;
    }
};

// the documented recurrence, independently: smooth, then advance by the new ratio
struct Model
{
    int64_t index;
    uint64_t frac;      // 2^-56
    uint64_t ratio;
    uint32_t k;

    void Step(uint64_t target)
    {
        if (ratio != target && k != 0) {
            const uint64_t d = (target > ratio) ? target - ratio : ratio - target;
            const unsigned __int128 step = ((unsigned __int128)d * k) >> 32;
            if (step == 0) {
                ratio = target;
            } else {
                ratio = (target > ratio) ? ratio + (uint64_t)step : ratio - (uint64_t)step;
            }
        } else {
            ratio = target;
        }
        const unsigned __int128 sum = (unsigned __int128)frac + ratio;
        index += (int64_t)(sum >> 56);
        frac = (uint64_t)(sum & (((unsigned __int128)1 << 56) - 1));
    }
};

static void Accounting(double nominal, int64_t frames_total, bool simd)
{
    const int channels = 2, max_in = 256, max_out = 256;
    JackBridgeResampler a, b;
    a.Init(channels, nominal, 1, max_in, max_out);
    b.Init(channels, nominal, 1, max_in, max_out);
    a.ForceScalar(!simd);
    b.ForceScalar(!simd);
    const int half = a.HalfLength();
    Feeder fa(channels, max_in), fb(channels, max_in);
    std::vector<float> oa[2], ob[2];
    for (int c = 0; c < channels; c++) {
        oa[c].resize(max_out);
        ob[c].resize(1);
    }
    float* pa[2] = { &oa[0][0], &oa[1][0] };
    float* pb[2] = { &ob[0][0], &ob[1][0] };

    Model m;
    m.index = 0;
    m.frac = 0;
    m.ratio = a.CurrentRatioExact();
    m.k = a.SmoothingCoefficient();
    long double naive = 0;          // what a floating-point accumulator would say
    double naive_d = 0;
    int64_t produced = 0, pulls = 0, refused = 0, mismatch = 0, model_bad = 0, needed_bad = 0;
    int64_t overread = 0, jit_refused_bad = 0, full = 0, max_fill = 0, needed_model_bad = 0;
    double t = nominal, drift = 0;

    while (produced < frames_total) {
        // random walk within +-500 ppm, a 1% jump now and then
        drift += (Uniform() - 0.5) * 20e-6;
        drift = std::max(-500e-6, std::min(500e-6, drift));
        t = nominal * (1.0 + drift + ((Rand() % 1000) == 0 ? 0.01 : 0.0));
        const uint64_t target = a.TargetExact(t);

        if (Rand() % 2) {
            const int n = 1 + (int)(Rand() % max_in);
            if (fa.Push(a, n) < n) {
                full++;
            }
            max_fill = std::max(max_fill, a.Pushed() - (int64_t)floor(a.Center()));
            continue;
        }

        const bool available = (Rand() % 4) == 0;
        const int n = 1 + (int)(Rand() % max_out);
        int64_t f0;
        uint64_t r0;
        a.CenterExact(&f0, &r0);
        const int64_t need = a.NeededFor(n, t);
        {
            // the model's own answer: the centre of the n-th output, plus the half length
            Model c = m;
            for (int i = 1; i < n; i++) {
                c.Step(target);
            }
            if (need != c.index + half + 1) {
                needed_model_bad++;
            }
        }
        int got;
        if (available) {
            got = a.PullAvailable(pa, n, t);
        } else {
            got = a.Pull(pa, n, t);
            if (a.Pushed() >= need ? got != n : got != 0) {
                needed_bad++;
            }
            if (got == 0) {
                int64_t f1;
                uint64_t r1;
                a.CenterExact(&f1, &r1);
                if (f1 != f0 || r1 != r0) {
                    needed_bad++;
                }
                refused++;
                continue;
            }
        }
        pulls++;

        // the model: every output's newest tap must have been pushed, the centre must match
        for (int i = 0; i < got; i++) {
            if (m.index + half >= a.Pushed()) {
                overread++;
            }
            m.Step(target);
            naive += (long double)m.ratio / 72057594037927936.0L;
            naive_d += (double)m.ratio * kFracScale;
        }
        if (available && got < n && m.index + half < a.Pushed()) {
            needed_bad++;   // stopped although the next output's input was there
        }
        int64_t f1;
        uint64_t r1;
        a.CenterExact(&f1, &r1);
        if (f1 != m.index || r1 != m.frac || a.CurrentRatioExact() != m.ratio) {
            model_bad++;
        }

        // the same outputs one at a time, input pushed just in time
        for (int i = 0; i < got; i++) {
            const int64_t need1 = b.NeededFor(1, t);
            if (b.Pushed() < need1) {
                if (need1 - b.Pushed() > 1) {
                    fb.Push(b, (int)(need1 - b.Pushed() - 1));
                }
                // one frame short: must be refused without a change
                if ((Rand() % 16) == 0 && b.Pull(pb, 1, t) != 0) {
                    jit_refused_bad++;
                }
                fb.Push(b, 1);
            }
            if (b.Pull(pb, 1, t) != 1) {
                jit_refused_bad++;
                break;
            }
            for (int c = 0; c < channels; c++) {
                if (memcmp(&ob[c][0], &oa[c][i], sizeof(float)) != 0) {
                    mismatch++;
                }
            }
        }
        produced += got;
    }

    const double center = a.Center();
    printf("%s, ratio %.6f, %s: %lld outputs in %lld pulls (%lld refused), %lld pushes cut short, "
           "most input held %lld frames\n",
           simd ? "SIMD" : "portable", nominal, "quality 1", (long long)produced, (long long)pulls,
           (long long)refused, (long long)full, (long long)max_fill);
    printf("  centre %.6f; a long double sum of the same ratios is off by %.3g frames, a double sum by %.3g\n",
           center, (double)((long double)m.index + (long double)m.frac / 72057594037927936.0L - naive),
           (double)((long double)m.index + (long double)m.frac / 72057594037927936.0L - (long double)naive_d));
    Check(model_bad == 0, "centre and ratio equal the exact sum of the smoothed ratios after every pull (%lld off)",
          (long long)model_bad);
    Check(needed_model_bad == 0, "NeededFor(n) equals the model's floor(centre of output n) + HalfLength() + 1 (%lld off)",
          (long long)needed_model_bad);
    Check(needed_bad == 0, "Pull succeeds exactly when Pushed() >= NeededFor(), changes nothing when refused (%lld bad)",
          (long long)needed_bad);
    Check(overread == 0, "no output needed a frame beyond Pushed() (%lld)", (long long)overread);
    Check(mismatch == 0 && jit_refused_bad == 0,
          "random blocks == one frame at a time with input just in time, bit for bit (%lld differ, %lld bad refusals)",
          (long long)mismatch, (long long)jit_refused_bad);
}

//------------------------------------------------------------------------------
// every channel layout and both paths compute the same filter: each channel of
// a C-channel run against a mono portable run of that channel's input

static double Layout(int channels, int quality, double nominal, bool simd)
{
    JackBridgeResampler r;
    std::vector<JackBridgeResampler> mono(channels);
    r.Init(channels, nominal, quality, 256, 256);
    r.ForceScalar(!simd);
    for (int c = 0; c < channels; c++) {
        mono[c].Init(1, nominal, quality, 256, 256);
        mono[c].ForceScalar(true);
    }
    std::vector<float> in[32], out[32], ref(256);
    const float* ip[32];
    float* op[32];
    for (int c = 0; c < channels; c++) {
        in[c].resize(256);
        out[c].resize(256);
        ip[c] = &in[c][0];
        op[c] = &out[c][0];
    }
    float* rp[1] = { &ref[0] };
    double worst = 0, drift = 0;
    int64_t at = 0;
    for (int cycle = 0; cycle < 400; cycle++) {
        const int n = 1 + (int)(Rand() % 256);
        for (int c = 0; c < channels; c++) {
            for (int i = 0; i < n; i++) {
                in[c][i] = Noise(at + i, c);
            }
            const float* one[1] = { &in[c][0] };
            mono[c].Push(one, n);
        }
        r.Push(ip, n);
        at += n;
        drift = std::max(-500e-6, std::min(500e-6, drift + (Uniform() - 0.5) * 50e-6));
        const double t = nominal * (1.0 + drift);
        const int got = r.PullAvailable(op, 256, t);
        for (int c = 0; c < channels; c++) {
            if (mono[c].PullAvailable(rp, 256, t) != got) {
                return 1.0;
            }
            for (int i = 0; i < got; i++) {
                worst = std::max(worst, (double)fabs(out[c][i] - ref[i]));
            }
        }
    }
    return worst;
}

static void Layouts()
{
    printf("\n== channel layouts and paths: each channel against a mono portable run ==\n");
    const int layouts[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 16, 17 };
    const struct { int quality; double nominal; } cfg[2] = { { 1, 1.0 }, { 0, 48000.0 / 44100.0 } };
    for (int k = 0; k < 2; k++) {
        JackBridgeResampler info;
        info.Init(1, cfg[k].nominal, cfg[k].quality, 256, 256);
        for (int s = info.UsesSimd() ? 1 : 0; s >= 0; s--) {
            double worst = 0;
            for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
                worst = std::max(worst, Layout(layouts[i], cfg[k].quality, cfg[k].nominal, s != 0));
            }
            // inputs within +-0.5: anything but rounding is far above 1e-6
            Check(worst < 1e-6, "q%d ratio %.4f (%d taps) %s, 1..17 channels: largest difference %.2g",
                  cfg[k].quality, cfg[k].nominal, info.Taps(), s ? "AVX2+FMA" : "portable", worst);
        }
    }
}

static void Accounting()
{
    printf("\n== 4. exact accounting ==\n");
    Accounting(1.0, gQuick ? 1000000 : 10000000, true);
    Accounting(48000.0 / 44100.0, gQuick ? 300000 : 2000000, true);
    Accounting(1.0, gQuick ? 300000 : 1000000, false);
}

//------------------------------------------------------------------------------
// 5. delay

static void Delay()
{
    printf("\n== 5. delay ==\n");
    printf("convention: output k is the input interpolated at Center() (input frames) before Pull made it;\n"
           "zero group delay relative to Center(); it needs input up to floor(Center()) + HalfLength()\n");
    const int64_t at = 1000;
    for (int q = 0; q < 3; q++) {
        // ratio exactly 1, integer centres: the impulse comes out exactly once, unchanged
        JackBridgeResampler r;
        r.Init(1, 1.0, q, 256, 256);
        std::vector<float> in(4096, 0.f);
        in[at] = 1.0f;
        const float* ip[1] = { &in[0] };
        r.Push(ip, 4096);
        float y;
        float* op[1] = { &y };
        int64_t hit = -1, others = 0;
        for (int k = 0; k < 2000; k++) {
            int64_t frames;
            double frac;
            Where(r, &frames, &frac);
            r.Pull(op, 1, 1.0);
            if (y == 1.0f && frac == 0) {
                hit = frames;
            } else if (y != 0.f) {
                others++;
            }
        }
        Check(hit == at && others == 0, "q%d ratio 1.0: impulse at frame %lld is output %lld exactly, %lld other outputs non-zero",
              q, (long long)at, (long long)hit, (long long)others);
    }
    // fractional positions: the response is centred on the impulse (centroid of the response)
    const double ratios[4] = { 1.0001, 44100.0 / 48000.0, 48000.0 / 44100.0, 1.0 / 3 };
    for (int i = 0; i < 4; i++) {
        double worst = 0;
        for (int trial = 0; trial < 8; trial++) {
            JackBridgeResampler r;
            r.Init(1, (i == 0) ? 1.0 : ratios[i], 1, 4096, 256);
            r.SetRatioSmoothing(0);
            std::vector<float> in(4096, 0.f);
            const int64_t imp = at + trial * 37;
            in[imp] = 1.0f;
            const float* ip[1] = { &in[0] };
            r.Push(ip, 4096);
            float y;
            float* op[1] = { &y };
            double m0 = 0, m1 = 0;
            for (;;) {
                int64_t frames;
                double frac;
                Where(r, &frames, &frac);
                if (r.Pull(op, 1, ratios[i]) != 1) {
                    break;
                }
                const double c = (double)frames + frac - (double)imp;
                m0 += y;
                m1 += y * c;
            }
            worst = std::max(worst, fabs(m1 / m0));
        }
        Check(worst < 1e-3, "q1 ratio %.6f: impulse response centred on the impulse, centroid within %.2g frames",
              ratios[i], worst);
    }
}

//------------------------------------------------------------------------------
// 6. speed

static double Now()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

// user-space CPU cycles of this thread, when perf events are allowed: ns move with the clock
static int gCycles = -2;

static uint64_t Cycles()
{
    if (gCycles == -2) {
        struct perf_event_attr a;
        memset(&a, 0, sizeof(a));
        a.type = PERF_TYPE_HARDWARE;
        a.size = sizeof(a);
        a.config = PERF_COUNT_HW_CPU_CYCLES;
        a.exclude_kernel = 1;
        a.exclude_hv = 1;
        gCycles = (int)syscall(__NR_perf_event_open, &a, 0, -1, -1, 0);
    }
    uint64_t v = 0;
    if (gCycles < 0 || read(gCycles, &v, sizeof(v)) != sizeof(v)) {
        return 0;
    }
    return v;
}

// median ns and cycles per frame and channel of Push + Pull
static void Speed(int quality, int channels, bool simd, double* ns, double* cycles)
{
    const int block = 256, cycles_per_run = gQuick ? 400 : 2000, runs = gQuick ? 3 : 7;
    JackBridgeResampler r;
    r.Init(channels, 1.0, quality, block, block);
    r.ForceScalar(!simd);
    std::vector<float> in[8], out[8];
    const float* ip[8];
    float* op[8];
    for (int c = 0; c < channels; c++) {
        in[c].resize(block);
        out[c].resize(block);
        for (int i = 0; i < block; i++) {
            in[c][i] = Noise(i, c);
        }
        ip[c] = &in[c][0];
        op[c] = &out[c][0];
    }
    std::vector<double> t, cy;
    for (int run = -1; run < runs; run++) {
        const uint64_t c0 = Cycles();
        const double t0 = Now();
        for (int i = 0; i < cycles_per_run; i++) {
            r.Push(ip, block);
            while (r.Pull(op, block, 1.0001) == 0) {
                r.Push(ip, block);
            }
        }
        const double t1 = Now();
        const uint64_t c1 = Cycles();
        if (run >= 0) {     // the first run warms up
            const double frames = (double)cycles_per_run * block * channels;
            t.push_back((t1 - t0) / frames);
            cy.push_back((double)(c1 - c0) / frames);
        }
    }
    std::sort(t.begin(), t.end());
    std::sort(cy.begin(), cy.end());
    *ns = t[t.size() / 2];
    *cycles = cy[cy.size() / 2];
}

static void Speed()
{
    printf("\n== 6. speed: per frame and channel (Push + Pull), ratio 1.0001, blocks of 256, median ==\n");
    cpu_set_t set;
    CPU_ZERO(&set);
    const int cpu = sched_getcpu();
    CPU_SET(cpu, &set);
    if (cpu >= 0 && sched_setaffinity(0, sizeof(set), &set) == 0) {
        printf("pinned to CPU %d\n", cpu);
    }
    JackBridgeResampler probe;
    probe.Init(1, 1.0, 1, 256, 256);
    const bool simd = probe.UsesSimd();
    const bool have_cycles = Cycles() != 0;
    printf("%-26s %15s %15s %15s %15s\n", "", "1 ch", "2 ch", "4 ch", "8 ch");
    printf("%-26s %15s %15s %15s %15s\n", "quality, path", have_cycles ? "ns / cycles" : "ns",
           have_cycles ? "ns / cycles" : "ns", have_cycles ? "ns / cycles" : "ns", have_cycles ? "ns / cycles" : "ns");
    for (int q = 0; q < 3; q++) {
        JackBridgeResampler info;
        info.Init(1, 1.0, q, 256, 256);
        for (int s = simd ? 1 : 0; s >= 0; s--) {
            printf("q%d (%2d taps) %-13s", q, info.Taps(), s ? "AVX2+FMA" : "portable");
            const int chs[4] = { 1, 2, 4, 8 };
            for (int i = 0; i < 4; i++) {
                double ns, cy;
                Speed(q, chs[i], s != 0, &ns, &cy);
                if (have_cycles) {
                    printf("   %5.2f / %5.1f", ns, cy);
                } else {
                    printf(" %15.2f", ns);
                }
            }
            printf("\n");
        }
    }
    if (!simd) {
        printf("(no AVX2 + FMA on this CPU: portable path only)\n");
    }
}

//------------------------------------------------------------------------------

static void Api()
{
    printf("\n== interface ==\n");
    JackBridgeResampler r;
    Check(!r.Init(0, 1.0, 1, 256, 256) && !r.Init(2, 0.1, 1, 256, 256) && !r.Init(2, 1.0, 1, 0, 256),
          "Init refuses bad arguments");
    Check(r.Init(2, 1.0, 1, 256, 256), "Init(2 channels, ratio 1, quality 1)");
    const int half = r.HalfLength();
    Check(r.Taps() == 2 * half && r.Taps() % 8 == 0, "taps %d = 2 * HalfLength() and a multiple of 8", r.Taps());
    Check(r.NeededFor(1, 1.0) == half + 1, "first output needs %lld frames (HalfLength() + 1)",
          (long long)r.NeededFor(1, 1.0));
    Check(r.Center() == 0 && r.Pushed() == 0 && r.CurrentRatio() == 1.0, "Reset state: centre 0, nothing pushed, ratio nominal");
    float a[256], b[256];
    float* o[2] = { a, b };
    Check(r.Pull(o, 1, 1.0) == 0 && r.PullAvailable(o, 256, 1.0) == 0, "nothing comes out of an empty resampler");
    Check(fabs(r.CurrentRatio() - 1.0) < 1e-15 && r.TargetExact(2.0) == r.TargetExact(1.25)
          && r.TargetExact(0.1) == r.TargetExact(0.8),
          "targets are clamped to nominal * [0.8, 1.25]");
}

int main(int argc, char** argv)
{
    bool speed = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--quick")) {
            gQuick = true;
        } else if (!strcmp(argv[i], "--no-speed")) {
            speed = false;
        }
    }
    Api();
    Accuracy();
    Layouts();
    Spectrum();
    Continuity();
    Accounting();
    Delay();
    if (speed) {
        Speed();
    }
    printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
