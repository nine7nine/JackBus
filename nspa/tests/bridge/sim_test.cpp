/*
 jack2-nspa: the sound card bridge in simulation (no hardware, no threads, deterministic).

 A master with an ideal clock runs cycles of n frames. A simulated card runs at its nominal rate
 times (1 + ppm): the kernel updates its position every `update` frames (a period, or a USB
 transfer for batch devices) and stamps each update late by up to `jitter`; the card's I/O
 thread wakes up to `wake` later and moves the frames. Both directions carry a sine; the output
 is compared with the ideal sine delayed by the measured (constant) latency, so timing jitter
 and resampler error show up together as one SNR.

   g++ -std=gnu++11 -faligned-new -O2 -march=x86-64 -Icommon common/JackBridgeCore.cpp \
       common/JackBridgeStream.cpp common/JackBridgeResampler.cpp \
       nspa/tests/bridge/sim_test.cpp -o /tmp/bridge_sim && /tmp/bridge_sim [scenario]
*/

#include "JackBridgeStream.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>

using namespace Jack;

struct Scenario
{
    const char* name;
    double master_rate, card_rate;
    int n;              // master period
    int update;         // card position update granularity (frames)
    double ppm;         // the card's true offset
    double ppm_step;    // added at step_at
    double step_at;     // s
    double jitter;      // s, timestamp lateness (uniform 0..jitter)
    double wake;        // s, I/O thread wakeup lateness (uniform 0..wake)
    double xrun_at;     // s, the card loses `xrun_frames` here (0: never)
    int xrun_frames;
    double pause_at;    // s, the card side stops here for pause_len s, then restarts (unplug, replug)
    double pause_len;
    double skip_at;     // s, the master skips `skip` cycles here (a master xrun), 0: never
    int skip;
    double seconds;
    int channels;
};

static const Scenario kScenarios[] = {
    // name                 master  card    n    upd   ppm    step  at   jitter  wake    xrun  frames pause len secs ch
    { "hda-like +50ppm",    48000, 48000, 256, 256,   50.0,  0.0,  0, 20e-6,  50e-6,   0,   0, 0, 0, 0, 0, 120, 2 },
    { "u44-like -120ppm",   48000, 48000, 128,  64, -120.0,  0.0,  0, 30e-6, 100e-6,   0,   0, 0, 0, 0, 0, 120, 4 },
    { "headset-like +300",  48000, 48000, 256,  48,  300.0,  0.0,  0, 150e-6, 300e-6,  0,   0, 0, 0, 0, 0, 120, 2 },
    { "small period 64",    48000, 48000,  64,  48,   80.0,  0.0,  0, 50e-6, 100e-6,   0,   0, 0, 0, 0, 0, 120, 2 },
    { "drift step +30",     48000, 48000, 256, 256,   50.0, 30.0, 60, 20e-6,  50e-6,   0,   0, 0, 0, 0, 0, 120, 2 },
    { "card xrun",          48000, 48000, 256, 256,   50.0,  0.0,  0, 20e-6,  50e-6,  40, 512, 0, 0, 0, 0, 120, 2 },
    { "44.1k card",         48000, 44100, 256, 220,   40.0,  0.0,  0, 20e-6,  50e-6,   0,   0, 0, 0, 0, 0, 120, 2 },
    { "card gone 2 s",      48000, 48000, 256, 256,   50.0,  0.0,  0, 20e-6,  50e-6,   0,   0, 40, 2.0, 0, 0, 120, 2 },
    { "master skips 3",     48000, 48000, 256, 256,   50.0,  0.0,  0, 20e-6,  50e-6,   0,   0,  0,   0, 40, 3, 120, 2 },
    { "16k mic",            48000, 16000, 256,  16,  200.0,  0.0,  0, 150e-6, 300e-6,  0,   0, 0, 0, 0, 0, 120, 1 },
};

static double urand(unsigned* s)
{
    *s = *s * 1103515245u + 12345u;
    return double((*s >> 8) & 0xffffff) / double(0x1000000);
}

struct Result
{
    double lock_s;          // first time |error| < 0.5 frame for 1 s
    double err_sigma;       // frames, after lock + 10 s
    double err_max;
    double ratio_ppm_sigma; // ratio jitter after lock + 10 s
    double snr_db;          // capture path audio, median over 0.1 s windows
    double snr_play_db;     // playback path audio
    double wander_cap;      // frames, the spread of the capture path's phase
    double wander_play;
    long long underruns, resyncs, lost;
};

// Least squares fits of a sin + b cos over short windows (0.1 s): the SNR per window shows the
// resampler and any fast timing jitter; slow phase wander (inaudible, a fraction of a frame
// over seconds) is reported apart, as the spread of the windows' phases in frames.
static void FitWindows(const std::vector<double>& t, const std::vector<float>& x, double f, double rate,
                       double* snr_median, double* wander_frames)
{
    std::vector<double> snrs, phases;
    const size_t w = size_t(rate / 10);
    for (size_t s0 = 0; s0 + w <= t.size(); s0 += w) {
        double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
        for (size_t i = s0; i < s0 + w; i++) {
            const double s = sin(2 * M_PI * f * t[i]), c = cos(2 * M_PI * f * t[i]);
            ss += s * s; cc += c * c; sc += s * c; xs += x[i] * s; xc += x[i] * c;
        }
        const double det = ss * cc - sc * sc;
        const double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
        double sig = 0, res = 0;
        for (size_t i = s0; i < s0 + w; i++) {
            const double y = a * sin(2 * M_PI * f * t[i]) + b * cos(2 * M_PI * f * t[i]);
            sig += y * y;
            res += (x[i] - y) * (x[i] - y);
        }
        snrs.push_back(res > 0 ? 10 * log10(sig / res) : 300);
        phases.push_back(atan2(b, a));
    }
    if (snrs.empty()) {
        *snr_median = -1;
        *wander_frames = -1;
        return;
    }
    std::vector<double> sorted = snrs;
    std::sort(sorted.begin(), sorted.end());
    *snr_median = sorted[sorted.size() / 2];
    // unwrap, then the spread, in frames at this frequency
    double m = 0, m2 = 0, prev = phases[0], acc = 0;
    for (size_t i = 0; i < phases.size(); i++) {
        double d = phases[i] - prev;
        while (d > M_PI) d -= 2 * M_PI;
        while (d < -M_PI) d += 2 * M_PI;
        acc += d;
        prev = phases[i];
        m += acc;
        m2 += acc * acc;
    }
    m /= phases.size();
    const double sd = sqrt(fmax(0.0, m2 / phases.size() - m * m));
    *wander_frames = sd / (2 * M_PI * f) * rate;
}

static Result Run(const Scenario& sc, bool verbose)
{
    JackBridgeParams params;
    params.fChannels = sc.channels;
    params.fMasterRate = sc.master_rate;
    params.fCardRate = sc.card_rate;
    params.fMasterPeriod = sc.n;
    params.fCardPeriod = sc.update;
    params.fMargin = (sc.wake + sc.jitter) * sc.card_rate + 8;
    params.fCardBuffer = 2 * sc.update + 2 * 64;      // play_buffer below

    JackBridgeCapture cap;
    JackBridgePlayback play;
    if (!cap.Init(params) || !play.Init(params)) {
        fprintf(stderr, "init failed\n");
        exit(2);
    }
    JackBridgeCardCapture ccap;
    JackBridgeCardPlayback cplay;
    ccap.Attach(&cap, sc.card_rate, 0.0);
    cplay.Attach(&play, sc.card_rate, 0.0);

    const double f_sig = 997.0;             // Hz, the test tone
    const int ch = sc.channels;
    std::vector<float> mbuf(ch * sc.n), inter(ch * 8192);
    std::vector<float*> outp(ch);
    std::vector<const float*> inp(ch);
    std::vector<std::vector<float> > mout(ch, std::vector<float>(sc.n)), min(ch, std::vector<float>(sc.n));
    for (int c = 0; c < ch; c++) {
        outp[c] = &mout[c][0];
        inp[c] = &min[c][0];
    }

    // the card's true clock: position p happens at time t_card(p)
    double ppm = sc.ppm;
    double t_origin = 0.0123;               // the card started a little after the master
    double p_origin = 0.0;
    double rate = sc.card_rate * (1.0 + ppm * 1e-6);
    bool stepped = false, xrun_done = false, paused = false, skipped = false;
    auto t_of = [&](double p) { return t_origin + (p - p_origin) / rate; };

    unsigned seed = 12345;
    int64_t upd_pos = 0;                    // last updated card position
    int64_t cap_read = 0;                   // capture: card frames moved to the FIFO
    int64_t play_written = 0;               // playback: card frames written to the card
    const int play_buffer = 2 * sc.update + 2 * 64;   // the card's playback buffer
    ccap.Start(0);
    cplay.Start();

    // the outputs once locked (time, sample), for a least squares sine fit at the end
    std::vector<double> cap_t, play_t;
    std::vector<float> cap_x, play_x;
    bool record = false;
    double lock_t = -1, good_since = -1;
    double e_sum = 0, e_sum2 = 0, e_max = 0, r_sum = 0, r_sum2 = 0;
    long long e_n = 0;

    const double cycle = sc.n / sc.master_rate;
    double next_cycle = cycle;
    int64_t master_frames = 0;
    double next_update_t = t_of(double(sc.update));

    while (next_cycle < sc.seconds) {
        // card position updates (kernel) and I/O wakeups happen before the next master cycle
        while (next_update_t < next_cycle) {
            const double t_upd = next_update_t;
            if (sc.pause_len > 0 && t_upd >= sc.pause_at && t_upd < sc.pause_at + sc.pause_len) {
                // the card side is gone: nothing read, nothing written, no positions
                upd_pos += sc.update;
                next_update_t = t_of(double(upd_pos + sc.update));
                paused = true;
                continue;
            }
            if (paused) {
                // back: a new card start, at the position it is at now
                paused = false;
                cap_read = upd_pos;
                ccap.Start(cap_read);
                cplay.Start();
                play_written = upd_pos;
            }
            upd_pos += sc.update;
            const double stamp = t_upd + urand(&seed) * sc.jitter;
            const double woke = stamp + urand(&seed) * sc.wake;
            (void)woke;     // the I/O thread's work below is ordered by time already
            // drift step
            if (!stepped && sc.ppm_step != 0.0 && t_upd >= sc.step_at) {
                p_origin = double(upd_pos);
                t_origin = t_upd;
                ppm += sc.ppm_step;
                rate = sc.card_rate * (1.0 + ppm * 1e-6);
                stepped = true;
            }
            // capture: the frames up to upd_pos
            if (sc.xrun_at > 0 && !xrun_done && t_upd >= sc.xrun_at) {
                // the card lost frames: the card side restarts its capture at a later position
                xrun_done = true;
                cap_read = upd_pos + sc.xrun_frames;
                upd_pos = cap_read;
                p_origin = double(upd_pos);
                t_origin = t_upd + sc.xrun_frames / rate;
                next_update_t = t_of(double(upd_pos + sc.update));
                ccap.Start(cap_read);
                cplay.Start();
                play_written = upd_pos;
                continue;
            }
            int todo = int(upd_pos - cap_read);
            while (todo > 0) {
                const int k = todo > 8192 / ch ? 8192 / ch : todo;
                for (int i = 0; i < k; i++) {
                    const double tc = t_of(double(cap_read + i));
                    for (int c = 0; c < ch; c++) {
                        inter[i * ch + c] = float(0.5 * sin(2 * M_PI * f_sig * tc + c));
                    }
                }
                ccap.Write(&inter[0], k);
                cap_read += k;
                todo -= k;
            }
            ccap.Position(upd_pos, int64_t(stamp * 1e9));
            // playback: the card has played up to upd_pos; keep its buffer full
            if (play_written < upd_pos) {
                play_written = upd_pos;     // it ran dry (start): those positions are gone
            }
            cplay.Position(upd_pos, int64_t(stamp * 1e9));
            const int space = play_buffer - int(play_written - upd_pos);
            if (space > 0) {
                const int count = space > 8192 / ch ? 8192 / ch : space;
                const int got = cplay.Fill(&inter[0], count, play_written, count);   // keep it full
                // what the card will play, at the card's true time
                for (int i = 0; i < got && record; i++) {
                    play_t.push_back(t_of(double(play_written + i)));
                    play_x.push_back(inter[i * ch]);
                }
                play_written += got;
            }
            next_update_t = t_of(double(upd_pos + sc.update));
        }

        // master cycle (a master xrun: it skips some, the graph does not run)
        if (sc.skip > 0 && !skipped && next_cycle >= sc.skip_at) {
            skipped = true;
            next_cycle += sc.skip * cycle;
            master_frames += sc.skip * sc.n;
            continue;
        }
        const double t_c = next_cycle;
        const int64_t t_ns = int64_t(t_c * 1e9);
        for (int i = 0; i < sc.n; i++) {
            const double tm = (master_frames + i) / sc.master_rate;
            for (int c = 0; c < ch; c++) {
                min[c][i] = float(0.5 * sin(2 * M_PI * f_sig * tm + c));
            }
        }
        cap.Cycle(t_ns, sc.n, 1e9 / sc.master_rate, &outp[0]);
        play.Cycle(t_ns, sc.n, 1e9 / sc.master_rate, &inp[0]);

        const JackBridgeStats& st = cap.Stats();
        if (st.fState >= kBridgeAcquire) {
            const double ae = fabs(st.fError);
            if (ae < 0.5) {
                if (good_since < 0) good_since = t_c;
                if (lock_t < 0 && t_c - good_since >= 1.0) lock_t = good_since;
            } else {
                good_since = -1;
            }
            if (lock_t >= 0 && t_c > lock_t + 10.0) {
                record = true;
                e_sum += st.fError; e_sum2 += st.fError * st.fError; e_n++;
                if (ae > e_max) e_max = ae;
                const double rp = (st.fRatio / (sc.card_rate / sc.master_rate) - 1.0) * 1e6;
                r_sum += rp; r_sum2 += rp * rp;
                for (int i = 0; i < sc.n; i++) {
                    cap_t.push_back((master_frames + i) / sc.master_rate);
                    cap_x.push_back(mout[0][i]);
                }
            }
        }
        if (verbose && (master_frames / sc.n) % int(5.0 / cycle) == 0) {
            const JackBridgeStats& ps = play.Stats();
            printf("  t=%6.1f cap st=%d err=%+8.3f ppm=%+9.3f | play st=%d err=%+8.3f delay=%7.1f ppm=%+9.3f missing=%lld\n",
                   t_c, st.fState, st.fError, (st.fRatio / (sc.card_rate / sc.master_rate) - 1.0) * 1e6,
                   ps.fState, ps.fError, ps.fDelay, (ps.fRatio / (sc.master_rate / sc.card_rate) - 1.0) * 1e6,
                   (long long)cplay.Missing());
        }
        master_frames += sc.n;
        next_cycle += cycle;
    }

    Result r;
    r.lock_s = lock_t;
    const double em = e_n ? e_sum / e_n : 0;
    r.err_sigma = e_n ? sqrt(e_sum2 / e_n - em * em) : -1;
    r.err_max = e_max;
    const double rm = e_n ? r_sum / e_n : 0;
    r.ratio_ppm_sigma = e_n ? sqrt(fmax(0.0, r_sum2 / e_n - rm * rm)) : -1;
    FitWindows(cap_t, cap_x, f_sig, sc.master_rate, &r.snr_db, &r.wander_cap);
    FitWindows(play_t, play_x, f_sig, sc.card_rate, &r.snr_play_db, &r.wander_play);
    r.underruns = cap.Stats().fUnderruns + play.Stats().fUnderruns;
    r.resyncs = cap.Stats().fResyncs + play.Stats().fResyncs;
    r.lost = cap.Stats().fLost + play.Stats().fLost;
    return r;
}

int main(int argc, char** argv)
{
    const int count = sizeof(kScenarios) / sizeof(kScenarios[0]);
    int only = -1;
    bool verbose = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = true;
        else only = atoi(argv[i]);
    }
    printf("%-20s %7s %9s %8s %10s %8s %8s %8s %8s %4s %4s\n", "scenario", "lock_s", "err_sd", "err_max",
           "ratio_sd", "snr_cap", "snr_pb", "wand_c", "wand_p", "und", "rsy");
    int fail = 0;
    for (int i = 0; i < count; i++) {
        if (only >= 0 && i != only) continue;
        const Scenario& sc = kScenarios[i];
        Result r = Run(sc, verbose);
        printf("%-20s %7.2f %9.4f %8.3f %9.3fp %7.1fdB %7.1fdB %8.4f %8.4f %4lld %4lld\n", sc.name, r.lock_s,
               r.err_sigma, r.err_max, r.ratio_ppm_sigma, r.snr_db, r.snr_play_db, r.wander_cap, r.wander_play,
               r.underruns, r.resyncs);
        // steady scenarios: tight; transients (a drift step, a card xrun): no underrun, the
        // delay stays well inside the margin, and only the card's own restarts resync
        const bool xrun = sc.xrun_at > 0 || sc.pause_len > 0 || sc.skip > 0;
        if (sc.pause_len > 0) {
            printf("  lost %lld (one per direction expected)\n", r.lost);
            if (r.lost != 2 || r.resyncs > 8) {
                printf("  ^ FAIL (lost/resync churn)\n");
                fail = 1;
            }
        }
        const bool transient = xrun || sc.ppm_step != 0.0;
        const bool bad = r.lock_s < 0 || r.lock_s > 5.0 || (!xrun && r.underruns > 0)
            || (!transient && r.err_sigma > 0.1) || r.err_max > 8.0 || (!xrun && r.resyncs > 0);
        if (bad) {
            printf("  ^ FAIL\n");
            fail = 1;
        }
    }
    printf("%s\n", fail ? "FAIL" : "PASS");
    return fail;
}
