/*
 jack2-nspa: the sound card bridge on snd-aloop, in real time (no JACK server, no sound).

 A master thread runs ideal cycles on CLOCK_MONOTONIC (as JACK's filtered cycle times would).
 A sine goes out through the bridge's playback direction to hw:Loopback,0,0; the loopback
 cable, shifted to a known rate ("PCM Rate Shift 100000" on both ends), brings it back on
 hw:Loopback,1,0 into the bridge's capture direction. The test reports, every few seconds,
 each direction's state, delay error and ratio (which must match the shift), and at the end the
 round trip's SNR (a least squares sine fit, so a constant latency is not an error), its
 largest deviation (clicks), resyncs and xruns. The shift changes half way.

   g++ -std=gnu++11 -faligned-new -O2 -march=x86-64 -Icommon common/JackBridgeCore.cpp common/JackBridgeStream.cpp \
       common/JackBridgeResampler.cpp common/JackBridgeAlsa.cpp nspa/tests/bridge/aloop_test.cpp \
       -lasound -lpthread -o /tmp/bridge_aloop
   /tmp/bridge_aloop [seconds=60] [ppm=50] [ppm2=-120] [master period=256] [card period=128]
   env: BRIDGE_WAKE=irq|timer, BRIDGE_REOPEN_AT=<s> (the card closed and opened again then)
*/

#include "JackBridgeAlsa.h"

#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <vector>

using namespace Jack;

// BRIDGE_CAPTURE / BRIDGE_PLAYBACK: another card than the loopback (no rate shift then, and
// BRIDGE_SILENT=1 sends silence: a real card is only measured, never made to play)
static bool Loopback()
{
    return getenv("BRIDGE_CAPTURE") == NULL && getenv("BRIDGE_PLAYBACK") == NULL;
}

static void SetShift(double ppm)
{
    if (!Loopback()) {
        return;
    }
    // steps of 10 ppm (100000 = 1.0); both ends of cable 0
    const long v = lrint(100000.0 * (1.0 + ppm * 1e-6));
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "amixer -q -c Loopback cset numid=1 %ld && amixer -q -c Loopback cset numid=57 %ld", v, v);
    if (system(cmd) != 0) {
        fprintf(stderr, "cannot set the loopback's rate shift\n");
    }
}

// the cards' timestamps are on CLOCK_MONOTONIC_RAW: a cycle start on CLOCK_MONOTONIC goes there by
// the offset measured now, as the server does (JackAudioDriver::RunBridgesPlayback)
static int64_t ToRaw(int64_t mono_ns)
{
    struct timespec r, m;
    clock_gettime(CLOCK_MONOTONIC_RAW, &r);
    clock_gettime(CLOCK_MONOTONIC, &m);
    return mono_ns + (int64_t(r.tv_sec) - int64_t(m.tv_sec)) * 1000000000LL + (r.tv_nsec - m.tv_nsec);
}

static int64_t Now()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

struct Master
{
    JackBridgeCapture* cap;
    JackBridgePlayback* play;
    int n;
    double rate;
    double seconds;
    int channels_cap, channels_play;
    double ppm, ppm2;
    // recorded round trip, once both are locked
    std::vector<double> t;
    std::vector<float> x;
    volatile bool done;
};

// per 0.1 s window: SNR (median) and the largest deviation; slow phase wander is not an error
static void Fit(const std::vector<double>& t, const std::vector<float>& x, double f, double* snr, double* worst)
{
    std::vector<double> snrs;
    double w = 0;
    const size_t win = 4800;
    for (size_t s0 = 0; s0 + win <= t.size(); s0 += win) {
        double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
        for (size_t i = s0; i < s0 + win; i++) {
            const double s = sin(2 * M_PI * f * t[i]), c = cos(2 * M_PI * f * t[i]);
            ss += s * s; cc += c * c; sc += s * c; xs += x[i] * s; xc += x[i] * c;
        }
        const double det = ss * cc - sc * sc;
        const double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
        double sig = 0, res = 0;
        for (size_t i = s0; i < s0 + win; i++) {
            const double y = a * sin(2 * M_PI * f * t[i]) + b * cos(2 * M_PI * f * t[i]);
            const double d = fabs(x[i] - y);
            sig += y * y;
            res += d * d;
            if (d > w) w = d;
        }
        snrs.push_back(res > 0 ? 10 * log10(sig / res) : 300);
    }
    std::sort(snrs.begin(), snrs.end());
    *snr = snrs.empty() ? -1 : snrs[snrs.size() / 2];
    *worst = w;
}

static void FitWhole(const std::vector<double>& t, const std::vector<float>& x, double f, double* snr, double* worst)
{
    double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
    for (size_t i = 0; i < t.size(); i++) {
        const double s = sin(2 * M_PI * f * t[i]), c = cos(2 * M_PI * f * t[i]);
        ss += s * s; cc += c * c; sc += s * c; xs += x[i] * s; xc += x[i] * c;
    }
    const double det = ss * cc - sc * sc;
    const double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
    double sig = 0, res = 0, w = 0;
    for (size_t i = 0; i < t.size(); i++) {
        const double y = a * sin(2 * M_PI * f * t[i]) + b * cos(2 * M_PI * f * t[i]);
        const double d = fabs(x[i] - y);
        sig += y * y;
        res += d * d;
        if (d > w) w = d;
    }
    *snr = res > 0 ? 10 * log10(sig / res) : 300;
    *worst = w;
}

static void* MasterThread(void* arg)
{
    Master* m = (Master*)arg;
    const double f = 997.0;
    const int64_t cycle_ns = int64_t(llround(m->n * 1e9 / m->rate));
    std::vector<std::vector<float> > in(m->channels_play, std::vector<float>(m->n));
    std::vector<std::vector<float> > out(m->channels_cap, std::vector<float>(m->n));
    std::vector<const float*> inp(m->channels_play + 1);
    std::vector<float*> outp(m->channels_cap + 1);
    for (int c = 0; c < m->channels_play; c++) inp[c] = &in[c][0];
    for (int c = 0; c < m->channels_cap; c++) outp[c] = &out[c][0];

    int64_t t0 = Now() + 100000000;
    int64_t frames = 0;
    const int64_t cycles = int64_t(m->seconds * m->rate / m->n);
    bool shifted = false;
    for (int64_t k = 0; k < cycles; k++) {
        const int64_t t_c = t0 + k * cycle_ns;
        struct timespec ts = { time_t(t_c / 1000000000LL), long(t_c % 1000000000LL) };
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        const int64_t t_raw = ToRaw(t_c);
        // capture first (the graph's inputs), then playback (its outputs)
        if (m->channels_cap) m->cap->Cycle(t_raw, m->n, 1e9 / m->rate, &outp[0]);
        const bool silent = getenv("BRIDGE_SILENT") != NULL;
        for (int i = 0; i < m->n; i++) {
            const float v = silent ? 0.0f : float(0.5 * sin(2 * M_PI * f * double(frames + i) / m->rate));
            for (int c = 0; c < m->channels_play; c++) in[c][i] = v;
        }
        if (m->channels_play) m->play->Cycle(t_raw, m->n, 1e9 / m->rate, &inp[0]);
        const JackBridgeStats& cs = m->cap->Stats();
        const JackBridgeStats& ps = m->play->Stats();
        if ((!m->channels_cap || cs.fState == kBridgeLocked) && (!m->channels_play || ps.fState == kBridgeLocked)
            && m->channels_cap && !shifted) {
            for (int i = 0; i < m->n; i++) {
                m->t.push_back(double(frames + i) / m->rate);
                m->x.push_back(out[0][i]);
            }
        }
        frames += m->n;
        if (!shifted && double(k) * m->n >= m->seconds * 0.5 * m->rate) {
            shifted = true;     // recorded: the first half (locked, before the shift)
        }
    }
    m->done = true;
    return NULL;
}

int main(int argc, char** argv)
{
    const double seconds = argc > 1 ? atof(argv[1]) : 60;
    const double ppm = argc > 2 ? atof(argv[2]) : 50;
    const double ppm2 = argc > 3 ? atof(argv[3]) : -120;
    const int n = argc > 4 ? atoi(argv[4]) : 256;
    const int card_period = argc > 5 ? atoi(argv[5]) : 128;

    SetShift(ppm);
    JackBridgeAlsa card;
    JackBridgeAlsaParams ap;
    ap.fCapture = Loopback() ? "hw:Loopback,1,0" : getenv("BRIDGE_CAPTURE");
    ap.fPlayback = Loopback() ? "hw:Loopback,0,0" : getenv("BRIDGE_PLAYBACK");
    // BRIDGE_MODE=capture|playback: one direction only (default both, linked on a real card)
    const char* mode = getenv("BRIDGE_MODE");
    if (mode && !strcmp(mode, "capture")) {
        ap.fPlayback = NULL;
    } else if (mode && !strcmp(mode, "playback")) {
        ap.fCapture = NULL;
    }
    ap.fRate = 48000;
    ap.fPeriod = card_period;
    ap.fPeriods = argc > 6 ? atoi(argv[6]) : 2;
    ap.fCaptureChannels = Loopback() ? 2 : 0;    // a real card: all its channels
    ap.fPlaybackChannels = Loopback() ? 2 : 0;
    ap.fPriority = 70;
    ap.fLink = !Loopback();   // two loopback devices: linked, an xrun on one keeps restarting both
    // BRIDGE_WAKE=irq|timer: the card thread's wakeups (default auto)
    const char* wake = getenv("BRIDGE_WAKE");
    ap.fWake = (wake && !strcmp(wake, "irq")) ? kWakeIrq : (wake && !strcmp(wake, "timer")) ? kWakeTimer : kWakeAuto;
    // the names as a driver has them: its own buffers, gone once the card is open (the card
    // must keep copies, for Reopen)
    char* cap_name = ap.fCapture ? strdup(ap.fCapture) : NULL;
    char* play_name = ap.fPlayback ? strdup(ap.fPlayback) : NULL;
    ap.fCapture = cap_name;
    ap.fPlayback = play_name;
    if (!card.Open(ap)) {
        fprintf(stderr, "cannot open the loopback card\n");
        return 2;
    }
    if (cap_name) {
        memset(cap_name, 0, strlen(cap_name));
        free(cap_name);
    }
    if (play_name) {
        memset(play_name, 0, strlen(play_name));
        free(play_name);
    }
    printf("card: %u Hz, period %d, batch %d, channels %d/%d\n", card.Rate(), card.Period(), card.Batch(),
           card.CaptureChannels(), card.PlaybackChannels());

    JackBridgeParams cp;
    cp.fChannels = card.CaptureChannels();
    cp.fMasterRate = 48000;
    cp.fCardRate = card.Rate();
    cp.fMasterPeriod = n;
    cp.fCardPeriod = card.Period();
    cp.fCardBuffer = card.Period() * ap.fPeriods;
    cp.fMargin = 96;
    JackBridgeParams pp = cp;
    pp.fChannels = card.PlaybackChannels();
    JackBridgeCapture cap;
    JackBridgePlayback play;
    if ((cp.fChannels && !cap.Init(cp)) || (pp.fChannels && !play.Init(pp))) {
        fprintf(stderr, "bridge init failed\n");
        return 2;
    }
    if (!card.Start(cp.fChannels ? &cap : NULL, pp.fChannels ? &play : NULL)) {
        fprintf(stderr, "cannot start the card thread\n");
        return 2;
    }

    Master m;
    m.cap = &cap;
    m.play = &play;
    m.n = n;
    m.rate = 48000;
    m.seconds = seconds;
    m.channels_cap = card.CaptureChannels();
    m.channels_play = card.PlaybackChannels();
    m.ppm = ppm;
    m.ppm2 = ppm2;
    m.done = false;
    m.t.reserve(size_t(seconds * 48000) + 4096);   // no allocation in the master thread
    m.x.reserve(size_t(seconds * 48000) + 4096);
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    struct sched_param sp;
    sp.sched_priority = 75;
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    pthread_attr_setschedparam(&attr, &sp);
    if (pthread_create(&th, &attr, MasterThread, &m) != 0 && pthread_create(&th, NULL, MasterThread, &m) != 0) {
        return 2;
    }

    const double expect1 = ppm, expect2 = ppm2;
    bool shifted = false;
    // BRIDGE_REOPEN_AT=<s>: the card is closed and opened again then (as after an unplug)
    const int reopen_at = getenv("BRIDGE_REOPEN_AT") ? atoi(getenv("BRIDGE_REOPEN_AT")) : -1;
    int reopen_failed = 0;
    for (int s = 0; !m.done; s++) {
        struct timespec one = { 1, 0 };
        nanosleep(&one, NULL);
        if (s + 1 == reopen_at) {
            const bool ok = card.Reopen() > 0;
            printf("t=%3ds reopen: %s (card state %d)\n", s + 1, ok ? "open again" : "FAILED", card.State());
            reopen_failed += !ok;
        }
        if (!shifted && s >= seconds / 2) {
            SetShift(ppm2);
            shifted = true;
        }
        if (getenv("BRIDGE_TRACE")) {
            JackBridgeClockState ks;
            if (cap.Clock().Snapshot(&ks)) {
                printf("trace t=%d clk_ppm=%+8.3f ratio_ppm=%+8.3f corr_ppm=%+8.3f err=%+7.3f\n", s + 1,
                       (1e9 / ks.fPeriod / card.Rate() - 1.0) * 1e6, (cap.Stats().fRatio - 1.0) * 1e6,
                       cap.Stats().fCorrection * 1e6, cap.Stats().fError);
            }
        }
        if (s % 5 == 4 || m.done) {
            const JackBridgeStats& c = cap.Stats();
            const JackBridgeStats& p = play.Stats();
            printf("t=%3ds cap: st=%d err=%+7.3f ppm=%+8.2f | play: st=%d err=%+7.3f ppm=%+8.2f | expect %+.0f | und %lld/%lld rsy %lld/%lld xruns %ld | clk relock %d/%d outl %d/%d sd %.0f/%.0fus\n",
                   s + 1, c.fState, c.fError, (c.fRatio - 1.0) * 1e6, p.fState, p.fError, (1.0 / p.fRatio - 1.0) * 1e6,
                   shifted ? expect2 : expect1, (long long)c.fUnderruns, (long long)p.fUnderruns,
                   (long long)c.fResyncs, (long long)p.fResyncs, card.Xruns(),
                   cap.Clock().Relocks(), play.Clock().Relocks(), cap.Clock().Outliers(), play.Clock().Outliers(),
                   cap.Clock().ErrorSigma() / 1e3, play.Clock().ErrorSigma() / 1e3);
            fflush(stdout);
        }
    }
    pthread_join(th, NULL);
    card.Stop();
    SetShift(0);

    if (getenv("BRIDGE_DUMP")) {   // the recorded round trip, raw float, for analysis
        FILE* f = fopen(getenv("BRIDGE_DUMP"), "wb");
        if (f) {
            fwrite(&m.x[0], sizeof(float), m.x.size(), f);
            fclose(f);
        }
    }
    double snr = 0, worst = 0;
    if (m.t.size() > 48000) {
        Fit(m.t, m.x, 997.0, &snr, &worst);
    }
    printf("round trip (locked, before the shift): %.1f s, SNR %.1f dB (median of 0.1 s windows), largest deviation %.2e; latency cap %.1f + play %.1f master frames\n",
           m.t.size() / 48000.0, snr, worst, cap.Latency(), play.Latency());
    printf("timer wakes %ld; underruns cap %lld play %lld, resyncs cap %lld play %lld, card xruns %ld\n", card.TimerWakes(),
           (long long)cap.Stats().fUnderruns, (long long)play.Stats().fUnderruns,
           (long long)cap.Stats().fResyncs, (long long)play.Stats().fResyncs, card.Xruns());
    return reopen_failed ? 1 : 0;
}
