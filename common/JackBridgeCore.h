/*
 jack2-nspa: the core of the sound card bridge, which joins a card that runs on its own clock to
 the graph of the master card, with adaptive resampling.

 Two sides per card:
 - the card side: a realtime I/O thread that reads and writes the card. It feeds the card's
   clock model with the position and kernel timestamp of each update, and moves samples
   through a FIFO;
 - the graph side: runs in the master's cycle. Each cycle it predicts the card's position at
   the cycle's start, measures the delay between that position and the resampler's input
   position (with its fraction), and steers the ratio so that the delay stays on target.

 The delay is measured at one precise instant, from the clock model, rather than from a FIFO
 fill seen whenever a thread happens to run. The ratio has a feedforward part (the two clocks'
 rates) and a feedback part (a PI loop on the delay error). The loop is wide while it locks,
 then narrow. Nothing in the graph side allocates, locks, logs or makes a system call.

 C++11 (the project's standard).
*/

#ifndef __JackBridgeCore__
#define __JackBridgeCore__

#include <atomic>
#include <stdint.h>

namespace Jack
{

/*!
\brief Single producer, single consumer FIFO of planar float frames.

 Absolute 64-bit frame counters: Written() and Read() never wrap. The producer writes with a
 release store of the write counter, the consumer reads with a release store of the read
 counter. Allocation only in Init().
*/
class JackBridgeFifo
{
    public:

        JackBridgeFifo();
        ~JackBridgeFifo();

        bool Init(int channels, int min_frames);   // non-RT: capacity is the next power of 2
        void Reset();                              // both sides idle

        int Channels() const { return fChannels; }
        int Capacity() const { return fSize; }
        int64_t Written() const { return fWritten.load(std::memory_order_acquire); }
        int64_t Read() const { return fRead.load(std::memory_order_acquire); }
        int Available() const;                     // consumer side
        int Space() const;                         // producer side

        // producer
        int Write(const float* const* src, int frames);            // planar
        int WriteInterleaved(const float* src, int frames);        // interleaved
        int WriteSilence(int frames);
        // consumer
        int ReadTo(float* const* dst, int frames);                 // planar
        int ReadInterleaved(float* dst, int frames);               // interleaved
        int Skip(int frames);

    private:

        int fChannels;
        int fSize;
        int fMask;
        float** fData;
        alignas(64) std::atomic<int64_t> fWritten;
        alignas(64) std::atomic<int64_t> fRead;
};

/*!
\brief A snapshot of a card's clock: position = p_ref + (t - t_ref) / T.
*/
struct JackBridgeClockState
{
    int64_t fPosRef;        // frames
    int64_t fTimeRef;       // ns, CLOCK_MONOTONIC, of fPosRef (integer part)
    double fTimeRefFrac;    // ns, fractional part of the time of fPosRef
    double fPeriod;         // ns per frame
    uint32_t fEpoch;        // changes when the position was reset (an xrun, a restart)
    bool fValid;

    double Predict(int64_t t_ns) const   // the card's position at t_ns, fractional frames
    {
        return double(fPosRef) + (double(t_ns - fTimeRef) - fTimeRefFrac) / fPeriod;
    }
};

/*!
\brief A card's clock: a second order delay-locked loop on (position, time) pairs.

 Fed by the card side with the position the kernel last updated and the timestamp of that
 update (snd_pcm_htimestamp: from the status page, no system call). For batch devices (USB),
 only the pairs where the position moved carry new information. Each update predicts the time
 of the new position from the model, and corrects the reference time and the period by the
 error (gains from the bandwidth, scaled by the time since the last update, so irregular
 updates are fine). Outliers (a late wakeup cannot make an early timestamp, but a scheduling
 hiccup in the kernel can make a late one) are gated by the error's measured spread.

 Published to the graph side through a sequence lock: the writer is the card side only.
*/
class JackBridgeClock
{
    public:

        JackBridgeClock();

        // card side
        void Start(double nominal_rate, double ppm_hint, uint32_t epoch);   // forget everything
        void Update(int64_t pos, int64_t t_ns);
        // Wide while it locks (fast_seconds after a start), then medium (until medium_seconds),
        // then narrow: once locked, the timestamps' jitter, and the slow wander of a USB card's
        // arrival times (its transfer buffering), must not reach the period, which is the
        // bridge's ratio. The device's own clock is far steadier than its arrival times.
        void SetBandwidths(double fast, double medium, double slow, double fast_seconds, double medium_seconds)
        {
            fFast = fast;
            fMedium = medium;
            fSlow = slow;
            fFastSeconds = fast_seconds;
            fMediumSeconds = medium_seconds;
        }
        double Bandwidth() const { return fBandwidth; }
        int Outliers() const { return fOutliers; }
        double ErrorSigma() const;            // ns, the spread of the time error
        int Updates() const { return fUpdates; }
        int Relocks() const { return fRelocks; }

        // any side
        bool Snapshot(JackBridgeClockState* state) const;   // false while invalid

    private:

        void Publish();

        double fNominalRate;
        double fBandwidth;
        double fFast;
        double fMedium;
        double fSlow;
        double fFastSeconds;
        double fMediumSeconds;
        double fElapsed;     // ns of card time since the start
        double fErrMean;     // running mean of the error (ns), about one second
        double fFastUntil;   // fElapsed until which the loop stays wide (a start, a re-lock)
        double fMediumUntil; // then medium until this
        int fRelocks;        // times the error's bias made the loop wide again
        // the model (card side only)
        int64_t fPosRef;
        int64_t fTimeRef;
        double fTimeRefFrac;
        double fPeriod;
        double fErr2;        // running mean of the squared error (ns^2)
        int fUpdates;
        int fOutliers;
        int fRun;            // consecutive outliers
        uint32_t fEpoch;
        // published copy
        alignas(64) std::atomic<uint32_t> fSeq;
        std::atomic<int64_t> fPubPosRef;
        std::atomic<int64_t> fPubTimeRef;
        std::atomic<double> fPubTimeRefFrac;
        std::atomic<double> fPubPeriod;
        std::atomic<uint32_t> fPubEpoch;
        std::atomic<bool> fPubValid;
};

/*!
\brief The delay controller: two one-pole prefilters, then a proportional correction.

 Error in card frames per cycle, correction as a relative ratio change (1e-6 = 1 ppm). The rate
 comes from the card's clock model (the feedforward ratio): it is the one integrator. This loop
 only steers the delay back, proportionally, so the two never pull against each other (with an
 integrator here as well, the clock model's slow settling wound it up, and unwinding it swung
 the ratio by tens of ppm on a USB card). Kp follows from the bandwidth and the cycle length;
 the prefilters (about 20 times the loop's frequency) take out the measurement noise. A rate
 error left in the clock model leaves a small steady delay error (rate / Kp), well inside the
 margin. The correction is clamped.
*/
class JackBridgeControl
{
    public:

        JackBridgeControl();

        void Init(int cycle_frames, double rate, double max_correction);
        void Reset();
        void SetBandwidth(double hz);
        double Bandwidth() const { return fBandwidth; }
        double Update(double error);          // returns the correction
        double Correction() const { return fCorrection; }

    private:

        int fCycle;
        double fRate;
        double fMax;
        double fBandwidth;
        double fW0;
        double fKp;
        double fKi;
        double fZ1;
        double fZ2;
        double fIntegral;
        double fCorrection;
};

/*!
\brief Statistics a bridge direction keeps for tests and for the front end (read without lock;
 informational).
*/
struct JackBridgeStats
{
    double fError;          // last delay error, card frames
    double fDelay;          // last delay, card frames
    double fRatio;          // last target ratio
    double fCorrection;     // last loop correction
    double fBandwidth;      // loop bandwidth
    int64_t fCycles;
    int64_t fLockedCycles;
    int64_t fBadClock;      // cycles the card's clock model was not believable
    int64_t fUnderruns;     // cycles the card side could not feed (capture) or take (playback)
    int64_t fResyncs;
    int fState;
};

} // end of namespace

#endif
