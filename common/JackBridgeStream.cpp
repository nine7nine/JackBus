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
    : fHaveLast(false), fEpoch(0), fState(kBridgeIdle), fStateCycles(0), fTarget(0.0),
      fRatio(1.0), fBase(0)
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
    // the FIFO holds a few cycles of either side
    const int fifo = 4 * (params.fMasterPeriod + params.fCardPeriod) + 4 * kChunk;
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
    const int cycle_in = int(ceil(params.fMasterPeriod * (ratio > 1.0 ? ratio : 1.0)));
    const int max_in = cycle_in + 2 * kChunk;
    const int max_out = (params.fMasterPeriod > kChunk) ? params.fMasterPeriod : kChunk;
    return fResampler.Init(params.fChannels, ratio, params.fQuality, max_in, max_out);
}

void JackBridgeDirection::Stop()
{
    fState = kBridgeIdle;
    fHaveLast = false;
}

double JackBridgeDirection::Latency() const
{
    return fTarget * fParams.fMasterRate / fParams.fCardRate;
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
    const double base = (n - 1) * r + fResampler.HalfLength() + 1;
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
    return true;
}

void JackBridgeCapture::Cycle(int64_t t_ns, int n, double master_ns_per_frame, float* const* out)
{
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
    const double p = cs.Predict(t_ns);
    // how late the card side's frames are: the card's position now, minus the newest frame in
    fLateNow = p - double(pos + (fFifo.Written() - index));

    if (fState == kBridgeIdle) {
        fEpoch = epoch;
        fState = kBridgeLearning;
        fLearnCycles = 0;
        fLateMin = fLateNow;
        fLateMax = fLateNow;
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
        fFifo.Skip(fFifo.Available());     // nothing is read yet: do not let it fill
        if (++fLearnCycles * n < int64_t(fParams.fLearnSeconds * fParams.fMasterRate)) {
            Silence(out, fParams.fChannels, n);
            return;
        }
        SetTarget(n);
        fState = kBridgeStarting;
    }
    if (epoch != fEpoch) {
        // the card restarted (an xrun): place again, with what was learned
        fStats.fResyncs++;
        fEpoch = epoch;
        fState = kBridgeStarting;
    }

    if (fState == kBridgeStarting && !Place(cs, p, epoch, index, pos)) {
        Silence(out, fParams.fChannels, n);
        return;
    }

    const double delay = p - (double(fBase) + fResampler.Center());
    const double error = delay - fTarget;
    const double corr = fControl.Update(error);
    const double ratio = (master_ns_per_frame / CardPeriod(cs)) * (1.0 + corr);

    if (Feed(fResampler.NeededFor(n, ratio), epoch, index, pos) < 0 || fResampler.Pull(out, n, ratio) != n) {
        // The card side's frames came later than the delay covers: cover this lateness too
        // (a quarter more), up to the limit, and place again
        Silence(out, fParams.fChannels, n);
        fStats.fUnderruns++;
        fStats.fResyncs++;
        const double late = (fLateNow > fLate ? fLateNow : fLate) * 1.25;
        if (late > fLate) {
            fLate = late;
        }
        SetTarget(n);
        fState = kBridgeStarting;
        return;
    }

    fRatio = ratio;
    fStats.fDelay = delay;
    fStats.fError = error;
    fStats.fRatio = ratio;
    fStats.fCorrection = corr;
    fStats.fBandwidth = fControl.Bandwidth();
    Schedule();
}

//--------------------
// JackBridgePlayback
//--------------------

void JackBridgePlayback::Cycle(int64_t t_ns, int n, double master_ns_per_frame, const float* const* in)
{
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
    if (fState == kBridgeIdle || cs.fEpoch != fEpoch) {
        if (fState != kBridgeIdle) {
            fStats.fResyncs++;
        }
        fEpoch = cs.fEpoch;
        fState = kBridgeStarting;
        // The card side keeps the card's buffer full, so the frames placed must reach past it:
        // the buffer, the filter's look-ahead (a cycle's last frames are made the next cycle)
        // and the margin (the card side's wakeup jitter)
        const double r = fParams.fMasterRate / fParams.fCardRate;
        const int buffer = fParams.fCardBuffer > 0 ? fParams.fCardBuffer : 2 * fParams.fCardPeriod;
        fTarget = buffer + fResampler.HalfLength() / r + fParams.fMargin;
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
    }

    // the card position that will play this cycle's first frame (master position Pushed()),
    // from the next card frame to make (fWritten, at master position Center())
    const double r_now = fResampler.CurrentRatio();
    const double first = double(fBase) + double(fWritten)
        + (double(fResampler.Pushed()) - fResampler.Center()) / r_now;
    const double delay = first - p;
    const double error = delay - fTarget;
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
    Schedule();
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
            fMissing += must - filled;
        }
        filled = must;
    }
    return filled;
}

} // end of namespace
