/*
 JackBus: the core of the sound card bridge (see JackBridgeCore.h).
*/

#include "JackBridgeCore.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace Jack
{

static const double kTwoPi = 6.283185307179586;

//----------------
// JackBridgeFifo
//----------------

JackBridgeFifo::JackBridgeFifo()
    : fChannels(0), fSize(0), fMask(0), fData(NULL), fWritten(0), fRead(0)
{}

JackBridgeFifo::~JackBridgeFifo()
{
    if (fData) {
        for (int c = 0; c < fChannels; c++) {
            free(fData[c]);
        }
        free(fData);
    }
}

bool JackBridgeFifo::Init(int channels, int min_frames)
{
    if (fData || channels <= 0 || min_frames <= 0) {
        return false;
    }
    int size = 64;
    while (size < min_frames) {
        size <<= 1;
    }
    fData = (float**)calloc(channels, sizeof(float*));
    if (!fData) {
        return false;
    }
    fChannels = channels;
    for (int c = 0; c < channels; c++) {
        void* p = NULL;
        if (posix_memalign(&p, 64, size * sizeof(float)) != 0) {
            return false;   // the destructor frees what was allocated
        }
        memset(p, 0, size * sizeof(float));
        fData[c] = (float*)p;
    }
    fSize = size;
    fMask = size - 1;
    return true;
}

void JackBridgeFifo::Reset()
{
    fWritten.store(0, std::memory_order_relaxed);
    fRead.store(0, std::memory_order_release);
}

int JackBridgeFifo::Available() const
{
    return int(fWritten.load(std::memory_order_acquire) - fRead.load(std::memory_order_relaxed));
}

int JackBridgeFifo::Space() const
{
    return fSize - int(fWritten.load(std::memory_order_relaxed) - fRead.load(std::memory_order_acquire));
}

int JackBridgeFifo::Write(const float* const* src, int frames)
{
    int n = Space();
    if (frames < n) {
        n = frames;
    }
    if (n <= 0) {
        return 0;
    }
    const int64_t w = fWritten.load(std::memory_order_relaxed);
    const int start = int(w & fMask);
    const int first = (start + n <= fSize) ? n : fSize - start;
    for (int c = 0; c < fChannels; c++) {
        memcpy(fData[c] + start, src[c], first * sizeof(float));
        if (n > first) {
            memcpy(fData[c], src[c] + first, (n - first) * sizeof(float));
        }
    }
    fWritten.store(w + n, std::memory_order_release);
    return n;
}

int JackBridgeFifo::WriteInterleaved(const float* src, int frames)
{
    int n = Space();
    if (frames < n) {
        n = frames;
    }
    if (n <= 0) {
        return 0;
    }
    const int64_t w = fWritten.load(std::memory_order_relaxed);
    for (int c = 0; c < fChannels; c++) {
        float* d = fData[c];
        const float* s = src + c;
        for (int i = 0; i < n; i++) {
            d[(w + i) & fMask] = s[i * fChannels];
        }
    }
    fWritten.store(w + n, std::memory_order_release);
    return n;
}

int JackBridgeFifo::WriteSilence(int frames)
{
    int n = Space();
    if (frames < n) {
        n = frames;
    }
    if (n <= 0) {
        return 0;
    }
    const int64_t w = fWritten.load(std::memory_order_relaxed);
    const int start = int(w & fMask);
    const int first = (start + n <= fSize) ? n : fSize - start;
    for (int c = 0; c < fChannels; c++) {
        memset(fData[c] + start, 0, first * sizeof(float));
        if (n > first) {
            memset(fData[c], 0, (n - first) * sizeof(float));
        }
    }
    fWritten.store(w + n, std::memory_order_release);
    return n;
}

int JackBridgeFifo::ReadTo(float* const* dst, int frames)
{
    int n = Available();
    if (frames < n) {
        n = frames;
    }
    if (n <= 0) {
        return 0;
    }
    const int64_t r = fRead.load(std::memory_order_relaxed);
    const int start = int(r & fMask);
    const int first = (start + n <= fSize) ? n : fSize - start;
    for (int c = 0; c < fChannels; c++) {
        memcpy(dst[c], fData[c] + start, first * sizeof(float));
        if (n > first) {
            memcpy(dst[c] + first, fData[c], (n - first) * sizeof(float));
        }
    }
    fRead.store(r + n, std::memory_order_release);
    return n;
}

int JackBridgeFifo::ReadInterleaved(float* dst, int frames)
{
    int n = Available();
    if (frames < n) {
        n = frames;
    }
    if (n <= 0) {
        return 0;
    }
    const int64_t r = fRead.load(std::memory_order_relaxed);
    for (int c = 0; c < fChannels; c++) {
        const float* s = fData[c];
        float* d = dst + c;
        for (int i = 0; i < n; i++) {
            d[i * fChannels] = s[(r + i) & fMask];
        }
    }
    fRead.store(r + n, std::memory_order_release);
    return n;
}

int JackBridgeFifo::Skip(int frames)
{
    int n = Available();
    if (frames < n) {
        n = frames;
    }
    if (n <= 0) {
        return 0;
    }
    fRead.store(fRead.load(std::memory_order_relaxed) + n, std::memory_order_release);
    return n;
}

//-----------------
// JackBridgeClock
//-----------------

JackBridgeClock::JackBridgeClock()
    : fNominalRate(48000.0), fBandwidth(1.0), fFast(1.0), fSlow(0.01), fNarrow(0.5),
      fElapsed(0.0), fErrMean(0.0), fSince(0.0), fRelocks(0), fPosRef(0), fTimeRef(0), fTimeRefFrac(0.0),
      fPeriod(1e9 / 48000.0), fErr2(0.0), fUpdates(0), fOutliers(0), fRun(0), fEpoch(0),
      fSeq(0), fPubPosRef(0), fPubTimeRef(0), fPubTimeRefFrac(0.0), fPubPeriod(1e9 / 48000.0),
      fPubEpoch(0), fPubValid(false)
{}

void JackBridgeClock::Start(double nominal_rate, double ppm_hint, uint32_t epoch)
{
    fNominalRate = nominal_rate;
    fPeriod = 1e9 / (nominal_rate * (1.0 + ppm_hint * 1e-6));
    fPosRef = 0;
    fTimeRef = 0;
    fTimeRefFrac = 0.0;
    fErr2 = 500e3 * 500e3;     // (500 us)^2 until measured: the gate starts wide
    fBandwidth = fFast;
    fElapsed = 0.0;
    fErrMean = 0.0;
    fSince = 0.0;
    fUpdates = 0;
    fOutliers = 0;
    fRun = 0;
    fEpoch = epoch;
    Publish();
}

double JackBridgeClock::ErrorSigma() const
{
    return sqrt(fErr2);
}

void JackBridgeClock::Update(int64_t pos, int64_t t_ns)
{
    if (t_ns <= 0) {
        return;     // no update yet: right after a start the kernel's timestamp is zero
    }
    if (fUpdates == 0) {
        fPosRef = pos;
        fTimeRef = t_ns;
        fTimeRefFrac = 0.0;
        fUpdates = 1;
        Publish();
        return;
    }
    const int64_t dp = pos - fPosRef;
    if (dp == 0) {
        return;     // no new position (a batch device between its steps)
    }
    if (dp < 0) {
        // the position went back within one epoch: start over from here, keep the period
        fPosRef = pos;
        fTimeRef = t_ns;
        fTimeRefFrac = 0.0;
        fUpdates = 1;
        Publish();
        return;
    }

    const double expected = fTimeRefFrac + double(dp) * fPeriod;    // ns after fTimeRef
    const double err = double(t_ns - fTimeRef) - expected;

    // A sample whose time did not move on with its position, or whose error is as large as
    // the whole span it covers, is not the clock: right after a start (a stale timestamp)
    // it becomes the new reference; later it is an outlier (an update the kernel handled late,
    // a USB transfer completed late) and is skipped, unless they keep coming
    if (t_ns <= fTimeRef || fabs(err) > expected) {
        if (fUpdates <= 2 || ++fRun >= 16) {
            fPosRef = pos;
            fTimeRef = t_ns;
            fTimeRefFrac = 0.0;
            fUpdates = 1;
            fRun = 0;
            Publish();
        } else {
            fOutliers++;
        }
        return;
    }

    // Gate: once the spread is known, an error far outside it is a kernel hiccup (a late
    // timestamp), not the clock: skip it, unless they keep coming (then the model is wrong).
    const double sigma = sqrt(fErr2);
    const double limit = (5.0 * sigma > 200e3) ? 5.0 * sigma : 200e3;
    if (fUpdates > 8 && fabs(err) > limit) {
        fOutliers++;
        if (++fRun < 16) {
            return;
        }
        fPosRef = pos;
        fTimeRef = t_ns;
        fTimeRefFrac = 0.0;
        fUpdates = 1;
        fRun = 0;
        Publish();
        return;
    }
    fRun = 0;

    // Second order loop: phase gain b = sqrt(2) w, period gain c = w^2, with w from the
    // bandwidth and the time this update spans (updates need not be regular).
    const double dt = double(dp) * fPeriod;
    double w = kTwoPi * fBandwidth * dt * 1e-9;
    if (w > 0.7) {
        w = 0.7;    // a long gap: do not overshoot
    }
    const double b = 1.4142135623730951 * w;
    const double c = w * w;

    const double rel = expected + b * err;
    const int64_t whole = (int64_t)floor(rel);
    fTimeRef += whole;
    fTimeRefFrac = rel - double(whole);
    fPeriod += c * err / double(dp);
    fPosRef = pos;

    const double a = (dt > 1e9) ? 1.0 : dt * 1e-9;     // about a one second average
    fErr2 += a * (err * err - fErr2);
    fErrMean += a * (err - fErrMean);
    fUpdates++;
    fElapsed += dt;
    // Narrow once locked. If the card's rate really changes (a clock switch, a big step), the
    // narrow loop follows slowly and its errors stop averaging out: their mean over about a
    // second moves beyond the spread of the errors themselves. Then the loop is made wide
    // again until it has followed. The test is against the spread, not its standard error over
    // the second: a USB card's errors are not independent (its transfer sizes vary in a
    // pattern), so their mean wanders more than white noise would. Slow drift (temperature)
    // stays within it; the narrow loop follows that by itself.
    // Narrow as k / t. A real change of the card's rate (a clock switch, a big step) shows
    // as errors whose mean over about a second moves beyond their own spread (not the spread's
    // standard error: a USB card's errors are not independent, its transfer sizes vary in a
    // pattern): then start the narrowing again. Slow drift (temperature) stays well within it.
    const double spread = sqrt(fErr2);
    const double since = (fElapsed - fSince) * 1e-9;
    if (since > 2.0 && fabs(fErrMean) > 3.0 * spread && fabs(fErrMean) > 50e3) {
        fSince = fElapsed;
        fRelocks++;
    }
    double bw = fNarrow / ((fElapsed - fSince) * 1e-9 + 1e-3);
    fBandwidth = bw > fFast ? fFast : (bw < fSlow ? fSlow : bw);
    Publish();
}

void JackBridgeClock::Publish()
{
    // Sequence lock, writer side (the card side only). The reader never waits for a writer:
    // it retries if the sequence moved, and a writer preempted half way through only makes
    // the reader use the copy it read before (see Snapshot).
    const uint32_t s = fSeq.load(std::memory_order_relaxed);
    fSeq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    fPubPosRef.store(fPosRef, std::memory_order_relaxed);
    fPubTimeRef.store(fTimeRef, std::memory_order_relaxed);
    fPubTimeRefFrac.store(fTimeRefFrac, std::memory_order_relaxed);
    fPubPeriod.store(fPeriod, std::memory_order_relaxed);
    fPubEpoch.store(fEpoch, std::memory_order_relaxed);
    fPubValid.store(fUpdates >= 2, std::memory_order_relaxed);
    fSeq.store(s + 2, std::memory_order_release);
}

bool JackBridgeClock::Snapshot(JackBridgeClockState* state) const
{
    // A few tries; a writer in the middle of an update (odd sequence) or one that finished
    // during the read makes the copy invalid. If the writer was preempted half way, the
    // reader gives up for this cycle (returns false) rather than spin: the graph side then
    // keeps its previous estimate.
    for (int tries = 0; tries < 4; tries++) {
        const uint32_t s1 = fSeq.load(std::memory_order_acquire);
        if (s1 & 1) {
            continue;
        }
        state->fPosRef = fPubPosRef.load(std::memory_order_relaxed);
        state->fTimeRef = fPubTimeRef.load(std::memory_order_relaxed);
        state->fTimeRefFrac = fPubTimeRefFrac.load(std::memory_order_relaxed);
        state->fPeriod = fPubPeriod.load(std::memory_order_relaxed);
        state->fEpoch = fPubEpoch.load(std::memory_order_relaxed);
        state->fValid = fPubValid.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (fSeq.load(std::memory_order_relaxed) == s1) {
            return state->fValid;
        }
    }
    return false;
}

//-------------------
// JackBridgeControl
//-------------------

JackBridgeControl::JackBridgeControl()
    : fCycle(256), fRate(48000.0), fMax(1e-3), fBandwidth(1.0), fW0(0.0), fKp(0.0), fKi(0.0),
      fZ1(0.0), fZ2(0.0), fIntegral(0.0), fCorrection(0.0)
{}

void JackBridgeControl::Init(int cycle_frames, double rate, double max_correction)
{
    fCycle = cycle_frames;
    fRate = rate;
    fMax = max_correction;
    Reset();
    SetBandwidth(fBandwidth);
}

void JackBridgeControl::Reset()
{
    fZ1 = 0.0;
    fZ2 = 0.0;
    fIntegral = 0.0;
    fCorrection = 0.0;
}

void JackBridgeControl::SetBandwidth(double hz)
{
    // w: the loop's frequency in radians per cycle; a first order loop on the delay error
    // (frames) with that bandwidth, giving a relative ratio change
    const double w = kTwoPi * hz * double(fCycle) / fRate;
    fBandwidth = hz;
    fW0 = 1.0 - exp(-20.0 * w);
    fKp = w / double(fCycle);
    fKi = 0.0;
}

double JackBridgeControl::Update(double error)
{
    fZ1 += fW0 * (error - fZ1);
    fZ2 += fW0 * (fZ1 - fZ2);
    const double integral = fIntegral + fKi * fZ2;
    double corr = fKp * fZ2 + integral;
    if (corr > fMax) {
        corr = fMax;
    } else if (corr < -fMax) {
        corr = -fMax;
    } else {
        fIntegral = integral;   // no windup while clamped
    }
    fCorrection = corr;
    return corr;
}

} // end of namespace
