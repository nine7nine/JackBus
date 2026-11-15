/*
 jack2-nspa: the two directions of the sound card bridge (see JackBridgeStream.h).
*/

#include "JackBridgeStream.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace Jack
{

// scratch for moving frames between the FIFO and the resampler (graph side, preallocated)
static const int kChunk = 1024;

//------------------
// JackBridgeAnchor
//------------------

void JackBridgeAnchor::Set(uint32_t epoch, int64_t index, int64_t pos)
{
    const uint32_t s = fSeq.load(std::memory_order_relaxed);
    fSeq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    fEpoch.store(epoch, std::memory_order_relaxed);
    fIndex.store(index, std::memory_order_relaxed);
    fPos.store(pos, std::memory_order_relaxed);
    fValid.store(true, std::memory_order_relaxed);
    fSeq.store(s + 2, std::memory_order_release);
}

void JackBridgeAnchor::Clear(uint32_t epoch)
{
    const uint32_t s = fSeq.load(std::memory_order_relaxed);
    fSeq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    fEpoch.store(epoch, std::memory_order_relaxed);
    fValid.store(false, std::memory_order_relaxed);
    fSeq.store(s + 2, std::memory_order_release);
}

bool JackBridgeAnchor::Get(uint32_t* epoch, int64_t* index, int64_t* pos) const
{
    for (int tries = 0; tries < 4; tries++) {
        const uint32_t s1 = fSeq.load(std::memory_order_acquire);
        if (s1 & 1) {
            continue;
        }
        *epoch = fEpoch.load(std::memory_order_relaxed);
        *index = fIndex.load(std::memory_order_relaxed);
        *pos = fPos.load(std::memory_order_relaxed);
        const bool valid = fValid.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (fSeq.load(std::memory_order_relaxed) == s1) {
            return valid;
        }
    }
    return false;
}

//---------------------
// JackBridgeDirection
//---------------------

JackBridgeDirection::JackBridgeDirection()
    : fHaveLast(false), fEpoch(0), fState(kBridgeIdle), fStateCycles(0), fFadeIn(0), fTarget(0.0),
      fRatio(1.0), fBase(0),
      fPubLatency(0.0), fPubState(kBridgeIdle), fPubResyncs(0), fPubLost(0), fPubUnderruns(0)
{
    memset(&fStats, 0, sizeof(fStats));
    memset(&fLast, 0, sizeof(fLast));
    memset(fScratch, 0, sizeof(fScratch));
}

JackBridgeDirection::~JackBridgeDirection()
{
    for (int c = 0; c < 64; c++) {
        free(fScratch[c]);
    }
}

bool JackBridgeDirection::Init(const JackBridgeParams& params)
{
    fParams = params;
    if (params.fChannels <= 0 || params.fChannels > 64) {
        return false;
    }
    if (fParams.fMaxMasterPeriod < params.fMasterPeriod) {
        fParams.fMaxMasterPeriod = params.fMasterPeriod;
    }
    const int maxn = fParams.fMaxMasterPeriod;
    // the FIFO holds a few cycles of either side
    const int fifo = 4 * (maxn + params.fCardPeriod) + 4 * kChunk;
    if (!fFifo.Init(params.fChannels, fifo)) {
        return false;
    }
    for (int c = 0; c < params.fChannels; c++) {
        void* p = NULL;
        if (posix_memalign(&p, 64, kChunk * sizeof(float)) != 0) {
            return false;
        }
        memset(p, 0, kChunk * sizeof(float));
        fScratch[c] = (float*)p;
    }
    fControl.Init(params.fMasterPeriod, params.fMasterRate, params.fMaxPpm * 1e-6);
    // input arrives in chunks until one cycle's worth is in; output is a cycle or a chunk
    const double ratio = NominalRatio();
    const int cycle_in = int(ceil(maxn * (ratio > 1.0 ? ratio : 1.0)));
    const int max_in = cycle_in + 2 * kChunk;
    const int max_out = (maxn > kChunk) ? maxn : kChunk;
    return fResampler.Init(params.fChannels, ratio, params.fQuality, max_in, max_out);
}

bool JackBridgeDirection::SetMasterPeriod(int n)
{
    if (n <= 0 || n > fParams.fMaxMasterPeriod) {
        return false;
    }
    fParams.fMasterPeriod = n;
    fControl.Init(n, fParams.fMasterRate, fParams.fMaxPpm * 1e-6);
    if (fState != kBridgeIdle && fState != kBridgeLearning) {
        fState = kBridgeStarting;   // the targets follow the cycle length: placed again
    }
    fPeriodChanged = true;
    return true;
}

void JackBridgeDirection::Stop()
{
    fState = kBridgeIdle;
    fHaveLast = false;
}

double JackBridgeDirection::Latency() const
{
    // where the delay is going (once tuned it moves there slowly: the ports say it once)
    return (fGoal > 0.0 ? fGoal : fTarget) * fParams.fMasterRate / fParams.fCardRate;
}

void JackBridgeDirection::NoteCardWorst(double frames)
{
    double cur = fCardWorst.load(std::memory_order_relaxed);
    while (frames > cur && !fCardWorst.compare_exchange_weak(cur, frames, std::memory_order_relaxed)) {
    }
}

/*
 Measured, not guessed. The delay first placed is a learned guess (capture: the lateness seen
 while learning) plus a fixed margin: safe, and longer than most cards need. Once locked, the
 card side reports the worst case it meets at each read or fill: the peak of the lateness, which
 the master's cycles only sample, at a phase that drifts slowly (minutes), while the card side
 sees every peak. The target follows that:
 - after kTuneSeconds locked, down to the worst case seen plus a safety, slowly (kSlewPpm: a
   ratio change far too small to hear, a frame a second; the loop follows a few frames behind,
   on the safe side, without a jump);
 - up whenever a worse case eats into the safety: by the same slow move if it is a quarter of
   the safety or less, by placing again (a short fade) if more, since a slow move would come
   too late.
 Underruns still place again with more (Cycle). The safety covers what the measurement cannot
 see coming: the master's own wakeup jitter and the rarer late wakeups of either side.
*/
static const double kTuneSeconds = 20.0;
static const double kSafetySeconds = 0.00075;
static const double kSlewPpm = 20.0;

bool JackBridgeDirection::Tune(double cover, int n)
{
    const double w = fCardWorst.exchange(-1.0, std::memory_order_relaxed);
    if (fState != kBridgeLocked) {
        return false;   // what the card side meets while placing and locking is not its normal
    }
    if (w > fWorst) {
        fWorst = w;
    }
    if (fWorst < 0.0) {
        return false;
    }
    const double safety = kSafetySeconds * fParams.fCardRate;
    double required = cover + fWorst + safety;
    if (fLimit > 0.0 && required > fLimit) {
        required = fLimit;
    }
    double goal = (fGoal > 0.0) ? fGoal : fTarget;
    const bool measured = double(fStateCycles) * n >= (fParams.fAcquireSeconds + kTuneSeconds) * fParams.fMasterRate;
    if (required > goal - 0.25 * safety) {
        goal = required;    // eating into the safety: up
    } else if (measured && required < goal - 2.0) {
        goal = required;    // measured long enough: down
    }
    fGoal = goal;
    if (goal > fTarget + 0.25 * safety) {
        fTarget = goal;     // too far to move slowly: place again
        return true;
    }
    const double step = n * (fParams.fCardRate / fParams.fMasterRate) * kSlewPpm * 1e-6;
    if (goal < fTarget) {
        fTarget = (fTarget - step > goal) ? fTarget - step : goal;
    } else if (goal > fTarget) {
        fTarget = (fTarget + step < goal) ? fTarget + step : goal;
    }
    return false;
}

void JackBridgeDirection::PublishStats()
{
    // only once a delay is placed does the latency mean anything
    const bool placed = fState == kBridgeAcquire || fState == kBridgeLocked;
    fPubLatency.store(placed ? Latency() : 0.0, std::memory_order_relaxed);
    fPubState.store(fState, std::memory_order_relaxed);
    fPubResyncs.store(fStats.fResyncs, std::memory_order_relaxed);
    fPubLost.store(fStats.fLost, std::memory_order_relaxed);
    fPubUnderruns.store(fStats.fUnderruns, std::memory_order_relaxed);
    const double nominal = NominalRatio();
    fPubRatio.store(placed && nominal > 0.0 ? fStats.fRatio / nominal : 1.0, std::memory_order_relaxed);
}

double JackBridgeDirection::CardPeriod(const JackBridgeClockState& cs)
{
    const double nominal = 1e9 / fParams.fCardRate;
    if (cs.fPeriod > nominal * 0.99 && cs.fPeriod < nominal * 1.01) {
        return cs.fPeriod;
    }
    fStats.fBadClock++;
    return nominal;
}

static const int kFade = 240;   // 5 ms at 48 kHz: no click where audio starts or stops

bool JackBridgeDirection::Stale(const JackBridgeClockState& cs, int64_t t_ns) const
{
    // the card side feeds the clock at every update of the card's position: a clock that has
    // not moved for 8 card periods (100 ms at least) means the card side stopped
    double limit = 8.0 * fParams.fCardPeriod * 1e9 / fParams.fCardRate;
    if (limit < 100e6) {
        limit = 100e6;
    }
    return double(t_ns - cs.fTimeRef) > limit;
}

double JackBridgeDirection::Discontinuity(int n) const
{
    // the delay error beyond which it is a break: half a master cycle (64 frames at least);
    // drift and jitter move it by a frame or two, a skipped master cycle by a whole one
    const double half = 0.5 * n * fParams.fCardRate / fParams.fMasterRate;
    return half > 64.0 ? half : 64.0;
}

void JackBridgeDirection::Placed()
{
    fFadeIn = kFade;
}

void JackBridgeDirection::FadeIn(float* const* buf, int channels, int offset, int n)
{
    if (fFadeIn <= 0) {
        return;
    }
    const int k = (n < fFadeIn) ? n : fFadeIn;
    const int done = kFade - fFadeIn;
    for (int c = 0; c < channels; c++) {
        float* b = buf[c] + offset;
        for (int i = 0; i < k; i++) {
            b[i] *= float(done + i) / float(kFade);
        }
    }
    fFadeIn -= k;
}

void JackBridgeDirection::Schedule()
{
    fStateCycles++;
    if (fState == kBridgeAcquire
        && double(fStateCycles) * fParams.fMasterPeriod >= fParams.fAcquireSeconds * fParams.fMasterRate) {
        fState = kBridgeLocked;
        fControl.SetBandwidth(fParams.fLockedBandwidth);
    }
    if (fState == kBridgeLocked) {
        fStats.fLockedCycles++;
    }
    fStats.fState = fState;
}

//-------------------
// JackBridgeCapture
//-------------------

static void Silence(float* const* out, int channels, int n)
{
    for (int c = 0; c < channels; c++) {
        memset(out[c], 0, n * sizeof(float));
    }
}

int JackBridgeCapture::Feed(int64_t need, uint32_t epoch, int64_t index, int64_t pos)
{
    float* chunk[64];
    for (int c = 0; c < fParams.fChannels; c++) {
        chunk[c] = fScratch[c];
    }
    while (fResampler.Pushed() < need) {
        // the next FIFO frame must be the next one the resampler expects
        const int64_t at = pos + (fFifo.Read() - index);
        if (at != fBase + fResampler.Pushed()) {
            return -1;
        }
        int64_t want = need - fResampler.Pushed();
        if (want > kChunk) {
            want = kChunk;
        }
        const int got = fFifo.ReadTo(chunk, int(want));
        if (got <= 0) {
            return -1;      // the card side is late: not in yet
        }
        if (fResampler.Push(chunk, got) != got) {
            return -1;      // no room (cannot happen at the sizes Init gives): place again
        }
    }
    (void)epoch;
    return 0;
}

void JackBridgeCapture::SetTarget(int n)
{
    // The delay must cover this cycle's input (the last of n output frames at the ratio), the
    // filter's look-ahead, and the card side's lateness, plus a margin: a quarter of the
    // lateness, at least the configured margin. Bounded: by default, four card periods and
    // two master cycles more than the minimum.
    const double r = fParams.fCardRate / fParams.fMasterRate;
    const double base = Base(n);
    double late = fLate > fParams.fCardPeriod ? fLate : fParams.fCardPeriod;
    double margin = late * 0.25;
    if (margin < fParams.fMargin) {
        margin = fParams.fMargin;
    }
    const double limit = (fParams.fMaxDelay > 0.0) ? fParams.fMaxDelay
        : base + 4.0 * fParams.fCardPeriod + 2.0 * n * r + fParams.fMargin;
    fTarget = base + late + margin;
    if (fTarget > limit) {
        fTarget = limit;
    }
    fLimit = limit;
    fGoal = 0.0;    // tuned again from here (Tune), with what was measured so far
}

double JackBridgeCapture::Base(int n) const
{
    // this cycle's input (the last of n output frames at the ratio), the filter's look-ahead
    return (n - 1) * (fParams.fCardRate / fParams.fMasterRate) + fResampler.HalfLength() + 1;
}

bool JackBridgeCapture::Place(const JackBridgeClockState& cs, double p, uint32_t epoch, int64_t index, int64_t pos)
{
    (void)cs;
    (void)epoch;
    const int64_t first = (int64_t)floor(p - fTarget);    // card position of the resampler's input 0
    if (fFifo.Read() < index) {
        fFifo.Skip(int(index - fFifo.Read()));     // frames from before this epoch
        if (fFifo.Read() < index) {
            return false;
        }
    }
    const int64_t at = pos + (fFifo.Read() - index);
    if (at < first) {
        const int64_t drop = first - at;
        if (fFifo.Skip(int(drop)) < drop) {
            return false;   // not in yet: next cycle
        }
    }
    fResampler.Reset();
    if (at > first) {
        // the card started after that position: silence up to its first frame
        float* chunk[64];
        for (int c = 0; c < fParams.fChannels; c++) {
            chunk[c] = fScratch[c];
            memset(chunk[c], 0, kChunk * sizeof(float));
        }
        int64_t pad = at - first;
        while (pad > 0) {
            const int k = (pad > kChunk) ? kChunk : int(pad);
            if (fResampler.Push(chunk, k) != k) {
                return false;
            }
            pad -= k;
        }
    }
    fBase = first;
    fControl.Reset();
    fControl.SetBandwidth(fParams.fAcquireBandwidth);
    fState = kBridgeAcquire;
    fStateCycles = 0;
    Placed();
    return true;
}

void JackBridgeCapture::Cycle(int64_t t_ns, int n, double master_ns_per_frame, float* const* out)
{
    Publisher publish(this);
    fStats.fCycles++;
    JackBridgeClockState cs;
    if (fClock.Snapshot(&cs)) {
        fLast = cs;
        fHaveLast = true;
    } else if (fHaveLast && fLast.fValid) {
        cs = fLast;     // the card side is mid-update: last cycle's model is good enough
    } else {
        Silence(out, fParams.fChannels, n);
        return;
    }

    uint32_t epoch;
    int64_t index, pos;
    if (!fAnchor.Get(&epoch, &index, &pos) || epoch != cs.fEpoch) {
        Silence(out, fParams.fChannels, n);
        return;     // the card side is between epochs: next cycle
    }
    if (Stale(cs, t_ns)) {
        // the card side stopped (unplugged, or stopped): quiet, no placing every cycle; back
        // to placing when its clock moves again
        if (fState != kBridgeLost && fState != kBridgeIdle) {
            fStats.fLost++;
        }
        fState = kBridgeLost;
        fStats.fState = fState;
        fPendingLate = 0.0;     // it went away: that was not lateness
        Silence(out, fParams.fChannels, n);
        return;
    }
    if (fState == kBridgeLost) {
        fState = kBridgeStarting;
    }

    const double p = cs.Predict(t_ns);
    // how late the card side's frames are: the card's position now, minus the newest frame in
    fLateNow = p - double(pos + (fFifo.Written() - index));

    if (fState == kBridgeIdle) {
        fEpoch = epoch;
        fState = kBridgeLearning;
        fLearnCycles = 0;
        fLateMin = fLateNow;
        fLateMax = fLateNow;
        fLearnWorst = -1.0;
        fCardWorst.store(-1.0, std::memory_order_relaxed);
    }
    if (fState == kBridgeLearning) {
        // Before placing anything, learn how late the card side's frames come. Lateness is the
        // phase between the card's updates and the master's cycles (0 to one card period: it
        // drifts through all of it, slowly) plus the jitter of the card side's wakeups and the
        // card's updates (its spread, measured here). The delay covers one card period plus
        // that spread: measured, not guessed.
        if (fLateNow < fLateMin) {
            fLateMin = fLateNow;
        }
        if (fLateNow > fLateMax) {
            fLateMax = fLateNow;
        }
        fLate = fParams.fCardPeriod + (fLateMax - fLateMin);
        // and the peak the card side meets itself: a card whose transfers are larger than its
        // period (USB) comes later than a period and the spread the master's cycles sample
        const double w = fCardWorst.exchange(-1.0, std::memory_order_relaxed);
        if (w > fLearnWorst) {
            fLearnWorst = w;
        }
        if (fLearnWorst > fLate) {
            fLate = fLearnWorst;
        }
        fFifo.Skip(fFifo.Available());     // nothing is read yet: do not let it fill
        if (++fLearnCycles * n < int64_t(fParams.fLearnSeconds * fParams.fMasterRate)) {
            Silence(out, fParams.fChannels, n);
            return;
        }
        SetTarget(n);
        fState = kBridgeStarting;
    }
    if (epoch != fEpoch) {
        // the card restarted (an xrun, a replug): place again, with what was learned
        fStats.fResyncs++;
        fEpoch = epoch;
        fState = kBridgeStarting;
        fPendingLate = 0.0;
    }
    if (fPeriodChanged) {
        fPeriodChanged = false;
        if (fState == kBridgeStarting) {
            SetTarget(n);
        }
    }
    if (fState == kBridgeStarting && fPendingLate > 0.0) {
        // After an underrun: wait for the card to show it is running (a card period more in),
        // then its frames really came that late, and the delay covers it. If it went away
        // instead, the lost state (or its restart, a new epoch) drops this.
        if (fFifo.Written() < fPendingWritten + fParams.fCardPeriod) {
            Silence(out, fParams.fChannels, n);
            return;
        }
        if (fPendingLate > fLate) {
            fLate = fPendingLate;
            SetTarget(n);
        }
        fPendingLate = 0.0;
    }

    if (fState == kBridgeStarting && !Place(cs, p, epoch, index, pos)) {
        Silence(out, fParams.fChannels, n);
        return;
    }

    const double delay = p - (double(fBase) + fResampler.Center());
    const double error = delay - fTarget;
    if (fabs(error) > Discontinuity(n)) {
        // a break, not drift (the master skipped cycles, the card's clock was reset): steering
        // back at the largest correction would take seconds; place again now (fades)
        fStats.fResyncs++;
        fState = kBridgeStarting;
        Silence(out, fParams.fChannels, n);
        return;
    }
    const double corr = fControl.Update(error);
    const double ratio = (master_ns_per_frame / CardPeriod(cs)) * (1.0 + corr);

    if (Feed(fResampler.NeededFor(n, ratio), epoch, index, pos) < 0 || fResampler.Pull(out, n, ratio) != n) {
        // The card side's frames came later than the delay covers: make what can be made from
        // the frames in and fade it out (no click), then silence; cover this lateness too (a
        // quarter more), up to the limit, and place again
        int made = fResampler.PullAvailable(out, n, ratio);
        if (made < 0) {
            made = 0;
        }
        for (int c = 0; c < fParams.fChannels; c++) {
            for (int i = 0; i < made; i++) {
                out[c][i] *= float(made - i) / float(made + 1);
            }
            memset(out[c] + made, 0, (n - made) * sizeof(float));
        }
        fStats.fUnderruns++;
        fStats.fResyncs++;
        fPendingLate = (fLateNow > fLate ? fLateNow : fLate) * 1.25;
        fPendingWritten = fFifo.Written();
        fState = kBridgeStarting;
        return;
    }

    FadeIn(out, fParams.fChannels, 0, n);
    fRatio = ratio;
    fStats.fDelay = delay;
    fStats.fError = error;
    fStats.fRatio = ratio;
    fStats.fCorrection = corr;
    fStats.fBandwidth = fControl.Bandwidth();
    if (Tune(Base(n), n)) {
        fStats.fResyncs++;
        fState = kBridgeStarting;   // the next cycle places the delay again, longer
        fStats.fState = fState;
        return;
    }
    Schedule();
}

//--------------------
// JackBridgePlayback
//--------------------

void JackBridgePlayback::Cycle(int64_t t_ns, int n, double master_ns_per_frame, const float* const* in)
{
    Publisher publish(this);
    fStats.fCycles++;
    JackBridgeClockState cs;
    if (fClock.Snapshot(&cs)) {
        fLast = cs;
        fHaveLast = true;
    } else if (fHaveLast && fLast.fValid) {
        cs = fLast;
    } else {
        return;
    }
    if (!cs.fValid) {
        return;
    }
    if (Stale(cs, t_ns)) {
        // the card side stopped: this cycle's frames go nowhere, no placing every cycle
        if (fState != kBridgeLost && fState != kBridgeIdle) {
            fStats.fLost++;
        }
        fState = kBridgeLost;
        fStats.fState = fState;
        return;
    }
    if (fState == kBridgeLost) {
        fState = kBridgeStarting;
    }
    if (fState == kBridgeIdle || cs.fEpoch != fEpoch || fPeriodChanged) {
        if (fState != kBridgeIdle && !fPeriodChanged) {
            fStats.fResyncs++;
        }
        // Learned once: a card that restarts (an xrun, a replug) or a master whose buffer size
        // changed is placed again at once, with what was learned and measured (how far ahead the
        // card side needs frames is the card's, not the epoch's); learning again after each
        // xrun of a card that xruns often meant a second of silence each time
        const bool learning = fState == kBridgeIdle || fState == kBridgeLearning;   // goes on
        fPeriodChanged = false;
        fEpoch = cs.fEpoch;
        if (!learning) {
            fState = kBridgeStarting;
        }
    }
    if (fState == kBridgeIdle) {
        // The card side keeps the card's buffer full, so the frames placed must reach past it:
        // the buffer, the filter's look-ahead (a cycle's last frames are made the next cycle)
        // and the margin (the card side's wakeup jitter). Before placing, how far ahead the card
        // side really needs them is measured (a USB card takes its frames into transfers up to
        // a buffer ahead of the buffer): placed short of that, it played silence until Tune
        // caught up, seconds of it.
        const int buffer = fParams.fCardBuffer > 0 ? fParams.fCardBuffer : 2 * fParams.fCardPeriod;
        fTarget = Cover() + buffer + fParams.fMargin;
        fLimit = fTarget + 4.0 * buffer;
        fGoal = 0.0;    // tuned again from here (Tune), with what was measured so far
        fState = kBridgeLearning;
        fLearnCycles = 0;
        fLearnWorst = -1.0;
        fCardWorst.store(-1.0, std::memory_order_relaxed);
    }
    if (fState == kBridgeLearning) {
        const double w = fCardWorst.exchange(-1.0, std::memory_order_relaxed);
        if (w > fLearnWorst) {
            fLearnWorst = w;
        }
        if (++fLearnCycles * n < int64_t(fParams.fLearnSeconds * fParams.fMasterRate)) {
            fStats.fState = fState;
            return;     // nothing placed yet: the card side plays silence
        }
        const double measured = Cover() + fLearnWorst + kSafetySeconds * fParams.fCardRate;
        if (measured > fTarget) {
            fTarget = (measured < fLimit) ? measured : fLimit;
        }
        fState = kBridgeStarting;
    }

    const double p = cs.Predict(t_ns);
    if (fState == kBridgeStarting) {
        fResampler.Reset();
        fControl.Reset();
        fControl.SetBandwidth(fParams.fAcquireBandwidth);
        fWritten = 0;
        fBase = (int64_t)ceil(p + fTarget);
        fAnchor.Set(fEpoch, fFifo.Written(), fBase);    // FIFO frame Written() plays at fBase
        fState = kBridgeAcquire;
        fStateCycles = 0;
        Placed();
    }

    // the card position that will play this cycle's first frame (master position Pushed()),
    // from the next card frame to make (fWritten, at master position Center())
    const double r_now = fResampler.CurrentRatio();
    const double first = double(fBase) + double(fWritten)
        + (double(fResampler.Pushed()) - fResampler.Center()) / r_now;
    const double delay = first - p;
    const double error = delay - fTarget;
    if (fabs(error) > Discontinuity(n)) {
        // a break (the master skipped cycles, the card's clock was reset): place again
        fStats.fResyncs++;
        fState = kBridgeStarting;
        return;
    }
    const double corr = fControl.Update(error);
    const double ratio = (CardPeriod(cs) / master_ns_per_frame) * (1.0 + corr);

    float* chunk[64];
    const float* src[64];
    for (int c = 0; c < fParams.fChannels; c++) {
        chunk[c] = fScratch[c];
    }
    int done = 0;
    bool overflow = false;
    while (done < n) {
        const int k = (n - done > kChunk) ? kChunk : n - done;
        for (int c = 0; c < fParams.fChannels; c++) {
            src[c] = in[c] + done;
        }
        if (fResampler.Push(src, k) != k) {
            overflow = true;
            break;
        }
        done += k;
        int made;
        while ((made = fResampler.PullAvailable(chunk, kChunk, ratio)) > 0) {
            FadeIn(chunk, fParams.fChannels, 0, made);
            if (fFifo.Write(chunk, made) < made) {
                overflow = true;
            }
            fWritten += made;
        }
    }
    if (overflow) {
        // the card side is not taking frames (stopped, or far behind): place again
        fStats.fUnderruns++;
        fStats.fResyncs++;
        fState = kBridgeStarting;
        return;
    }

    fRatio = ratio;
    fStats.fDelay = delay;
    fStats.fError = error;
    fStats.fRatio = ratio;
    fStats.fCorrection = corr;
    fStats.fBandwidth = fControl.Bandwidth();
    if (Tune(Cover(), n)) {
        fStats.fResyncs++;
        fState = kBridgeStarting;   // the next cycle places the delay again, longer
        fStats.fState = fState;
        return;
    }
    Schedule();
}

double JackBridgePlayback::Cover() const
{
    // a cycle's last frames are made the next cycle: the filter's look-ahead, in card frames
    return fResampler.HalfLength() * fParams.fCardRate / fParams.fMasterRate;
}

//-----------------------
// JackBridgeCardCapture
//-----------------------

void JackBridgeCardCapture::Start(int64_t next)
{
    fEpoch++;
    fNext = next;
    fDir->Anchor().Set(fEpoch, fDir->Fifo().Written(), next);
    fDir->Clock().Start(fRate, fPpm, fEpoch);
}

void JackBridgeCardCapture::Arriving(int64_t now_ns)
{
    JackBridgeClockState cs;
    if (fDir->Clock().Snapshot(&cs) && cs.fEpoch == fEpoch) {
        fDir->NoteCardWorst(cs.Predict(now_ns) - double(fNext));
    }
}

void JackBridgeCardCapture::Write(const float* frames, int count)
{
    const int put = fDir->Fifo().WriteInterleaved(frames, count);
    fNext += count;
    if (put < count) {
        // the graph side is not reading (stopped, or far behind): the FIFO and the card part
        // ways here, so start a new epoch at the next frame
        fOverflows++;
        Start(fNext);
    }
}

//------------------------
// JackBridgeCardPlayback
//------------------------

void JackBridgeCardPlayback::Start()
{
    fEpoch++;
    fDir->Clock().Start(fRate, fPpm, fEpoch);
}

void JackBridgeCardPlayback::Filled(int64_t next, int64_t now_ns)
{
    JackBridgeClockState cs;
    if (fDir->Clock().Snapshot(&cs) && cs.fEpoch == fEpoch) {
        fDir->NoteCardWorst(double(next) - cs.Predict(now_ns));
    }
}

int JackBridgeCardPlayback::Fill(float* frames, int count, int64_t pos, int must)
{
    JackBridgeFifo& fifo = fDir->Fifo();
    const int ch = fifo.Channels();
    if (must > count) {
        must = count;
    }
    uint32_t epoch;
    int64_t index, at;
    int filled = 0;
    bool placed = false;
    if (fDir->Placement(&epoch, &index, &at) && epoch == fEpoch) {
        placed = true;
        // FIFO frame index + (pos - at) is for card position pos
        const int64_t want = index + (pos - at);
        if (want > fifo.Read()) {
            fifo.Skip(int(want - fifo.Read()));     // made for positions already gone
        }
        const int64_t gap = fifo.Read() - want;     // > 0: placed later than pos
        if (gap > 0) {
            // positions before the first placed frame (the start): silence, only as needed
            if (gap >= must) {
                memset(frames, 0, must * ch * sizeof(float));
                return must;
            }
            memset(frames, 0, gap * ch * sizeof(float));
            filled = int(gap);
        }
        if (fifo.Read() == want + filled) {
            filled += fifo.ReadInterleaved(frames + filled * ch, count - filled);
        }
    }
    if (filled < must) {
        memset(frames + filled * ch, 0, (must - filled) * ch * sizeof(float));
        if (placed) {
            fMissing.fetch_add(must - filled, std::memory_order_relaxed);
        }
        filled = must;
    }
    return filled;
}

} // end of namespace
