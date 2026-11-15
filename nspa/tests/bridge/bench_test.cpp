/*
 jack2-nspa: the sound card bridge's graph side cost (no hardware, no threads).

 A synthetic card side keeps one card's FIFOs and clocks fed, exactly in step, while the graph
 side runs capture and playback cycles; only the graph side's two Cycle() calls are timed (what
 the master's cycle pays per card). Per channel count: ns per cycle and per channel-frame
 (median of several runs). Run under `perf stat -e cycles` for core cycles (a laptop's clock
 moves), or `perf record` for where the time goes.

   g++ -std=gnu++11 -faligned-new -O2 -march=x86-64 -g -Icommon common/JackBridgeCore.cpp \
       common/JackBridgeStream.cpp common/JackBridgeResampler.cpp nspa/tests/bridge/bench_test.cpp \
       -o /tmp/bridge_bench && taskset -c 2 /tmp/bridge_bench [channels...]

 JACK_BRIDGE_RESAMPLER=portable|sse2|avx2|avx512 picks the resampler's inner loop (if the CPU has
 it), for A/B measurements.
*/

#include "JackBridgeStream.h"

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>

using namespace Jack;

static int64_t Now()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// one run: `cycles` master cycles of n frames, a card of `ch` channels at +20 ppm
static double Run(int ch, int n, int cycles, bool capture, bool playback)
{
    JackBridgeParams p;
    p.fChannels = ch;
    p.fMasterPeriod = n;
    p.fCardPeriod = 128;
    p.fCardBuffer = 256;
    p.fLearnSeconds = 0.1;
    JackBridgeCapture cap;
    JackBridgePlayback play;
    cap.Init(p);
    play.Init(p);
    JackBridgeCardCapture ccap;
    JackBridgeCardPlayback cplay;
    ccap.Attach(&cap, 48000, 0);
    cplay.Attach(&play, 48000, 0);
    ccap.Start(0);
    cplay.Start();

    std::vector<float> inter(ch * 4096, 0.25f);
    std::vector<std::vector<float> > bufs(ch, std::vector<float>(n, 0.1f));
    std::vector<float*> outp(ch);
    std::vector<const float*> inp(ch);
    for (int c = 0; c < ch; c++) {
        outp[c] = &bufs[c][0];
        inp[c] = &bufs[c][0];
    }

    const double rate = 48000.0 * (1.0 + 20e-6);
    const double cycle = n / 48000.0;
    int64_t card_pos = 0, play_written = 0;
    int64_t spent = 0, timed = 0;
    for (int k = 1; k <= cycles; k++) {
        const double t = k * cycle;
        // the card side, in step: everything up to the master cycle's start
        const int64_t upto = int64_t(t * rate);
        while (card_pos + 128 <= upto) {
            ccap.Write(&inter[0], 128);
            card_pos += 128;
            const int64_t ts = int64_t((card_pos / rate) * 1e9);
            ccap.Position(card_pos, ts);
            cplay.Position(card_pos, ts);
            if (play_written < card_pos) {
                play_written = card_pos;
            }
            const int space = 256 - int(play_written - card_pos);
            if (space > 0) {
                play_written += cplay.Fill(&inter[0], space, play_written, space);
            }
        }
        const int64_t t_ns = int64_t(t * 1e9);
        const int64_t a = Now();
        if (capture) {
            cap.Cycle(t_ns, n, 1e9 / 48000.0, &outp[0]);
        }
        if (playback) {
            play.Cycle(t_ns, n, 1e9 / 48000.0, &inp[0]);
        }
        const int64_t b = Now();
        if (k > cycles / 5) {   // past the learning and the lock
            spent += b - a;
            timed++;
        }
    }
    if (cap.Stats().fUnderruns || play.Stats().fUnderruns) {
        fprintf(stderr, "warning: underruns %lld/%lld\n", (long long)cap.Stats().fUnderruns,
                (long long)play.Stats().fUnderruns);
    }
    return double(spent) / double(timed);   // ns per cycle
}

int main(int argc, char** argv)
{
    std::vector<int> chans;
    for (int i = 1; i < argc; i++) {
        chans.push_back(atoi(argv[i]));
    }
    if (chans.empty()) {
        chans = { 1, 2, 4, 8 };
    }
    const int n = 256;
    JackBridgeResampler probe;
    probe.Init(1, 1.0, 1, n, n);
    printf("resampler: %s\n", JackBridgeResampler::VariantName(probe.ActiveVariant()));
    printf("%4s %12s %12s %12s %16s\n", "ch", "capture", "playback", "both", "both ns/ch-frame");
    for (size_t i = 0; i < chans.size(); i++) {
        const int ch = chans[i];
        double r[3][5];
        for (int rep = 0; rep < 5; rep++) {
            r[0][rep] = Run(ch, n, 4000, true, false);
            r[1][rep] = Run(ch, n, 4000, false, true);
            r[2][rep] = Run(ch, n, 4000, true, true);
        }
        double med[3];
        for (int j = 0; j < 3; j++) {
            std::sort(r[j], r[j] + 5);
            med[j] = r[j][2];
        }
        printf("%4d %10.2fus %10.2fus %10.2fus %16.2f\n", ch, med[0] / 1e3, med[1] / 1e3, med[2] / 1e3,
               med[2] / (2.0 * ch * n));
    }
    return 0;
}
