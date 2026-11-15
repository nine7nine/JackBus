/*
 jack2-nspa: several cards bridged at once, against one master (no JACK server, no sound).

 Each card is opened in duplex (all its channels) with its own card thread; one master thread
 runs ideal cycles on CLOCK_MONOTONIC and, as the server will, runs every card's capture
 direction, then every card's playback direction (silence), timing each card's share of the
 cycle. Every few seconds and at the end: per card, its drift, delay error, underruns, resyncs,
 xruns, timer wakes, and the graph side's cost per cycle (mean and worst, in microseconds).

   g++ -std=gnu++11 -faligned-new -O2 -march=x86-64 -Icommon common/JackBridgeCore.cpp common/JackBridgeStream.cpp \
       common/JackBridgeResampler.cpp common/JackBridgeAlsa.cpp nspa/tests/bridge/multi_test.cpp \
       -lasound -lpthread -o /tmp/bridge_multi
   /tmp/bridge_multi <seconds> <master period> <card period> <periods> hw:PCH hw:U44 hw:Headset ...
*/

#include "JackBridgeAlsa.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>

using namespace Jack;

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

struct Card
{
    const char* name;
    JackBridgeAlsa alsa;
    JackBridgeCapture cap;
    JackBridgePlayback play;
    int cch, pch;
    std::vector<std::vector<float> > in, out;
    std::vector<const float*> inp;
    std::vector<float*> outp;
    // graph side cost (master thread only)
    double cost_sum;
    int64_t cost_max;
    int64_t cost_n;
};

struct Slow   // a cycle whose share for one card cost more than 200 us
{
    double at;   // s since the start
    int period;
    int card;
    int64_t ns;
    int cap_state, play_state;
};

struct Master
{
    Slow slow[256];
    int nslow;
    std::vector<Card*> cards;
    std::vector<int> periods;   // BRIDGE_PERIODS=256,64,1024: the master's buffer size, in turn
    double switch_seconds;      // every this long
    int n;
    double rate;
    double seconds;
    volatile bool done;
};

static void* MasterThread(void* arg)
{
    Master* m = (Master*)arg;
    int64_t cycle_ns = int64_t(llround(m->n * 1e9 / m->rate));
    int64_t t0 = Now() + 100000000;
    const int64_t end = t0 + int64_t(m->seconds * 1e9);
    size_t which = 0;
    int64_t next_switch = m->periods.size() > 1 ? t0 + int64_t(m->switch_seconds * 1e9) : end;
    int64_t base = 0;
    for (int64_t k = 0; ; k++) {
        int64_t t_c = t0 + (k - base) * cycle_ns;
        if (t_c >= end) {
            break;
        }
        if (t_c >= next_switch) {
            // the server changes its buffer size: the master stops, the directions are told,
            // then cycles go on at the new size
            which = (which + 1) % m->periods.size();
            struct timespec pause = { 0, 50000000 };
            nanosleep(&pause, NULL);
            m->n = m->periods[which];
            for (size_t i = 0; i < m->cards.size(); i++) {
                Card* c = m->cards[i];
                if (c->cch) c->cap.SetMasterPeriod(m->n);
                if (c->pch) c->play.SetMasterPeriod(m->n);
            }
            printf("master period now %d\n", m->n);
            cycle_ns = int64_t(llround(m->n * 1e9 / m->rate));
            t0 = Now() + 1000000;
            base = k;
            t_c = t0;
            next_switch = t0 + int64_t(m->switch_seconds * 1e9);
        }
        struct timespec ts = { time_t(t_c / 1000000000LL), long(t_c % 1000000000LL) };
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        const int64_t t_raw = ToRaw(t_c);
        // as in a server cycle: every card's capture before the graph, playback after it
        int64_t spent[16] = { 0 };
        for (size_t i = 0; i < m->cards.size(); i++) {
            Card* c = m->cards[i];
            const int64_t a = Now();
            if (c->cch) c->cap.Cycle(t_raw, m->n, 1e9 / m->rate, &c->outp[0]);
            spent[i] += Now() - a;
        }
        for (size_t i = 0; i < m->cards.size(); i++) {
            Card* c = m->cards[i];
            const int64_t a = Now();
            if (c->pch) c->play.Cycle(t_raw, m->n, 1e9 / m->rate, &c->inp[0]);
            spent[i] += Now() - a;
        }
        for (size_t i = 0; i < m->cards.size(); i++) {
            if (spent[i] > 200000 && m->nslow < 256) {
                Card* c = m->cards[i];
                Slow& w = m->slow[m->nslow++];
                w.at = double(t_c - (t0 - base * cycle_ns)) * 1e-9;
                w.period = m->n;
                w.card = int(i);
                w.ns = spent[i];
                w.cap_state = c->cap.Stats().fState;
                w.play_state = c->play.Stats().fState;
            }
        }
        if (k > int64_t(2 * m->rate / 256)) {   // after the first seconds (allocation-free anyway)
            for (size_t i = 0; i < m->cards.size(); i++) {
                Card* c = m->cards[i];
                c->cost_sum += spent[i];
                c->cost_n++;
                if (spent[i] > c->cost_max) c->cost_max = spent[i];
            }
        }
    }
    m->done = true;
    return NULL;
}

static void Report(Master& m, int s)
{
    for (size_t i = 0; i < m.cards.size(); i++) {
        Card* c = m.cards[i];
        const JackBridgeStats& cs = c->cap.Stats();
        const JackBridgeStats& ps = c->play.Stats();
        JackBridgeClockState k;
        const bool have = c->cch ? c->cap.Clock().Snapshot(&k) : c->play.Clock().Snapshot(&k);
        printf("t=%3ds %-12s %d/%dch clk %+8.2f ppm | cap st=%d err=%+6.2f | play st=%d err=%+6.2f | und %lld/%lld rsy %lld/%lld xruns %ld twakes %ld | cost %.1f us mean, %.1f max\n",
               s, c->name, c->cch, c->pch, have ? (1e9 / k.fPeriod / c->alsa.Rate() - 1.0) * 1e6 : 0.0,
               cs.fState, cs.fError, ps.fState, ps.fError, (long long)cs.fUnderruns, (long long)ps.fUnderruns,
               (long long)cs.fResyncs, (long long)ps.fResyncs, c->alsa.Xruns(), c->alsa.TimerWakes(),
               c->cost_n ? c->cost_sum / c->cost_n / 1e3 : 0.0, c->cost_max / 1e3);
    }
    fflush(stdout);
}

int main(int argc, char** argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: %s seconds master_period card_period periods hw:A [hw:B ...]\n", argv[0]);
        return 2;
    }
    Master m;
    m.seconds = atof(argv[1]);
    m.n = atoi(argv[2]);
    const int card_period = atoi(argv[3]);
    const int periods = atoi(argv[4]);
    m.rate = 48000;
    m.done = false;
    m.nslow = 0;
    m.switch_seconds = getenv("BRIDGE_SWITCH") ? atof(getenv("BRIDGE_SWITCH")) : 10.0;
    m.periods.push_back(m.n);
    if (getenv("BRIDGE_PERIODS")) {
        m.periods.clear();
        for (char* q = getenv("BRIDGE_PERIODS"); *q; ) {
            m.periods.push_back(atoi(q));
            while (*q && *q != ',') q++;
            if (*q) q++;
        }
        m.n = m.periods[0];
    }
    for (int a = 5; a < argc && m.cards.size() < 16; a++) {
        Card* c = new Card();
        c->name = argv[a];
        c->cost_sum = 0;
        c->cost_max = 0;
        c->cost_n = 0;
        // hw:NAME[:period][@auto|@timer|@irq]: the card's period and wake mode
        int period = card_period;
        char* colon = strchr(argv[a] + 3, ':');
        char* at = strchr(argv[a], '@');
        if (colon) {
            period = atoi(colon + 1);
            *colon = 0;
        }
        int wake = kWakeAuto;
        if (at) {
            *at = 0;
            wake = !strcmp(at + 1, "irq") ? kWakeIrq : !strcmp(at + 1, "timer") ? kWakeTimer : kWakeAuto;
        }
        JackBridgeAlsaParams ap;
        ap.fWake = wake;
        ap.fCapture = argv[a];
        ap.fPlayback = argv[a];
        ap.fRate = 48000;
        ap.fPeriod = period;
        ap.fPeriods = periods;
        ap.fPriority = 70;
        if (!c->alsa.Open(ap)) {
            fprintf(stderr, "cannot open %s\n", argv[a]);
            return 2;
        }
        c->cch = c->alsa.CaptureChannels();
        c->pch = c->alsa.PlaybackChannels();
        JackBridgeParams p;
        p.fMasterRate = 48000;
        p.fCardRate = c->alsa.Rate();
        p.fMasterPeriod = m.n;
        p.fMaxMasterPeriod = 4096;
        p.fCardPeriod = c->alsa.Period();
        p.fCardBuffer = c->alsa.Period() * periods;
        p.fMargin = 96;
        JackBridgeParams cp = p, pp = p;
        cp.fChannels = c->cch;
        pp.fChannels = c->pch;
        if ((c->cch && !c->cap.Init(cp)) || (c->pch && !c->play.Init(pp))) {
            fprintf(stderr, "bridge init failed for %s\n", argv[a]);
            return 2;
        }
        c->in.assign(c->pch, std::vector<float>(4096, 0.0f));
        c->out.assign(c->cch, std::vector<float>(4096, 0.0f));
        c->inp.resize(c->pch + 1);
        c->outp.resize(c->cch + 1);
        for (int i = 0; i < c->pch; i++) c->inp[i] = &c->in[i][0];
        for (int i = 0; i < c->cch; i++) c->outp[i] = &c->out[i][0];
        printf("%s: %u Hz, period %d, batch %d, %d/%d channels\n", argv[a], c->alsa.Rate(), c->alsa.Period(),
               c->alsa.Batch(), c->cch, c->pch);
        m.cards.push_back(c);
    }
    for (size_t i = 0; i < m.cards.size(); i++) {
        Card* c = m.cards[i];
        if (!c->alsa.Start(c->cch ? &c->cap : NULL, c->pch ? &c->play : NULL)) {
            fprintf(stderr, "cannot start %s\n", c->name);
            return 2;
        }
    }
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
    for (int s = 1; !m.done; s++) {
        struct timespec one = { 1, 0 };
        nanosleep(&one, NULL);
        if (s % 10 == 0) {
            Report(m, s);
        }
    }
    pthread_join(th, NULL);
    printf("end:\n");
    Report(m, int(m.seconds));
    printf("cycles over 200 us for one card: %d\n", m.nslow);
    for (int i = 0; i < m.nslow && i < 40; i++) {
        printf("  %8.3f s period %4d %-12s %7.1f us  states cap %d play %d\n", m.slow[i].at, m.slow[i].period,
               m.cards[m.slow[i].card]->name, m.slow[i].ns / 1e3, m.slow[i].cap_state, m.slow[i].play_state);
    }
    for (size_t i = 0; i < m.cards.size(); i++) {
        m.cards[i]->alsa.Stop();
    }
    return 0;
}
