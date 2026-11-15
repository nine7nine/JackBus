/*
 jack2-nspa: the two directions of the sound card bridge, graph side (see JackBridgeCore.h).

 Capture: the card side writes the card's frames to the FIFO and keeps the card's clock; each
 master cycle, the graph side resamples them into the port buffers.
 Playback: each master cycle, the graph side resamples the port buffers into card frames in the
 FIFO; the card side writes them to the card at the card position the graph side chose.

 Positions in card frames are absolute (since the card started). The delay D is measured at the
 start of each master cycle, in card frames:
 - capture: the card's position then, minus the card position of the resampler's next output
   (its centre, with its fraction): how old the sample is that starts the cycle's buffer;
 - playback: the card position that will play the cycle's first frame, minus the card's
   position then: how long until it is heard.
 The target delay is fixed when the direction starts; the loop holds D there.
*/

#ifndef __JackBridgeStream__
#define __JackBridgeStream__

#include "JackBridgeCore.h"
#include "JackBridgeResampler.h"

#include <stddef.h>

namespace Jack
{

enum JackBridgeState {
    kBridgeIdle = 0,       // no card data yet
    kBridgeLearning,       // measuring how late the card side's frames come (capture)
    kBridgeStarting,       // about to place the delay (pointer jump)
    kBridgeAcquire,        // wide loop, just placed
    kBridgeLocked,         // narrow loop
    kBridgeLost            // the card side stopped (unplugged, stopped): quiet until it moves again
};

struct JackBridgeParams
{
    int fChannels;
    double fMasterRate;        // nominal, Hz
    double fCardRate;          // nominal, Hz
    int fMasterPeriod;         // frames per master cycle
    int fMaxMasterPeriod;      // the largest it may become (SetMasterPeriod): sizes are for it
    int fCardPeriod;           // frames per card update (period, or URB for batch devices)
    int fCardBuffer;           // frames the card side keeps queued for playback (0: 2 periods)
    int fQuality;              // resampler quality (0, 1, 2)
    double fMargin;            // extra delay, card frames (jitter of the card side's wakeups)
    double fMaxPpm;            // largest correction
    double fAcquireBandwidth;  // Hz, loop bandwidth while locking
    double fLockedBandwidth;   // Hz, once locked
    double fAcquireSeconds;    // how long the wide loop runs
    double fLearnSeconds;      // capture: how long the card side's lateness is measured first
    double fMaxDelay;          // card frames: the delay target never grows beyond this

    JackBridgeParams()
        : fChannels(2), fMasterRate(48000.0), fCardRate(48000.0), fMasterPeriod(256), fMaxMasterPeriod(4096),
          fCardPeriod(256), fCardBuffer(0), fQuality(1), fMargin(48.0), fMaxPpm(1000.0),
          fAcquireBandwidth(1.0), fLockedBandwidth(0.05), fAcquireSeconds(4.0),
          fLearnSeconds(1.0), fMaxDelay(0.0)
    {}
};

/*!
\brief The anchor that ties FIFO indexes to card positions: FIFO frame k is card position
 fPos + (k - fIndex), for the epoch fEpoch. Written by one side, read by the other.
*/
class JackBridgeAnchor
{
    public:

        JackBridgeAnchor(): fSeq(0), fEpoch(0), fIndex(0), fPos(0), fValid(false) {}

        void Set(uint32_t epoch, int64_t index, int64_t pos);
        void Clear(uint32_t epoch);
        bool Get(uint32_t* epoch, int64_t* index, int64_t* pos) const;

    private:

        alignas(64) std::atomic<uint32_t> fSeq;
        std::atomic<uint32_t> fEpoch;
        std::atomic<int64_t> fIndex;
        std::atomic<int64_t> fPos;
        std::atomic<bool> fValid;
};

class JackBridgeDirection
{
    public:

        JackBridgeDirection();
        virtual ~JackBridgeDirection();

        bool Init(const JackBridgeParams& params);   // non-RT
        void Stop();                                 // graph side: back to idle
        // The master's buffer size changed. Non-RT, while the master is stopped (the card side
        // may go on): no allocation (Init sized everything for fMaxMasterPeriod); the delay is
        // placed again, keeping what was learned of the card side's lateness.
        bool SetMasterPeriod(int n);

        // shared with the card side
        JackBridgeFifo& Fifo() { return fFifo; }
        JackBridgeClock& Clock() { return fClock; }
        JackBridgeAnchor& Anchor() { return fAnchor; }

        double TargetDelay() const { return fTarget; }     // card frames
        double Latency() const;                            // master frames: the delay + the filter's
        const JackBridgeStats& Stats() const { return fStats; }   // graph side's own thread

        // for other threads (published at the end of each cycle)
        double PublishedLatency() const { return fPubLatency.load(std::memory_order_relaxed); }
        int PublishedState() const { return fPubState.load(std::memory_order_relaxed); }
        int64_t PublishedResyncs() const { return fPubResyncs.load(std::memory_order_relaxed); }
        int64_t PublishedLost() const { return fPubLost.load(std::memory_order_relaxed); }
        int64_t PublishedUnderruns() const { return fPubUnderruns.load(std::memory_order_relaxed); }
        void PublishStats();

    protected:

        virtual double NominalRatio() const = 0;   // resampler input frames per output frame
        void Schedule();
        // the card's frame period from its clock, if within 1% of the nominal one (else the
        // nominal one: a clock model that far off is wrong, not the card)
        double CardPeriod(const JackBridgeClockState& cs);

        JackBridgeParams fParams;
        JackBridgeFifo fFifo;
        JackBridgeClock fClock;
        JackBridgeAnchor fAnchor;
        JackBridgeResampler fResampler;
        JackBridgeControl fControl;
        JackBridgeStats fStats;
        JackBridgeClockState fLast;
        bool fHaveLast;
        uint32_t fEpoch;
        int fState;
        int64_t fStateCycles;      // cycles in the current state
        int fFadeIn;               // frames of fade-in left (after each placement)
        bool fPeriodChanged = false;   // SetMasterPeriod: targets to compute again

        // the card's clock has not moved for a while: the card side stopped
        bool Stale(const JackBridgeClockState& cs, int64_t t_ns) const;
        void Placed();             // a placement: fade in from here
        double Discontinuity(int n) const;   // card frames of delay error that mean a break
        void FadeIn(float* const* buf, int channels, int offset, int n);
        double fTarget;            // card frames
        double fRatio;             // last target ratio
        int64_t fBase;             // card position of the resampler's input 0 (capture)
                                   // or output 0 (playback)
        float* fScratch[64];       // one chunk per channel: FIFO <-> resampler (allocated in Init)
        std::atomic<double> fPubLatency;
        std::atomic<int> fPubState;
        std::atomic<int64_t> fPubResyncs;
        std::atomic<int64_t> fPubLost;
        std::atomic<int64_t> fPubUnderruns;

        // publishes the stats however Cycle returns
        struct Publisher
        {
            JackBridgeDirection* fDir;
            explicit Publisher(JackBridgeDirection* dir): fDir(dir) {}
            ~Publisher() { fDir->PublishStats(); }
        };
};

class JackBridgeCapture : public JackBridgeDirection
{
    public:

        // graph side, RT: t_ns is the cycle's start (CLOCK_MONOTONIC), n the frames to make,
        // master_ns_per_frame the master's measured frame period (its DLL)
        void Cycle(int64_t t_ns, int n, double master_ns_per_frame, float* const* out);

    protected:

        double NominalRatio() const { return fParams.fCardRate / fParams.fMasterRate; }

    private:

        bool Place(const JackBridgeClockState& cs, double p, uint32_t epoch, int64_t index, int64_t pos);
        int Feed(int64_t need, uint32_t epoch, int64_t index, int64_t pos);
        void SetTarget(int n);

        double fLate = 0.0;        // card frames: the lateness the delay covers
        double fLateMin = 0.0;     // while learning: the least and most lateness seen
        double fLateMax = 0.0;
        double fLateNow = 0.0;     // this cycle's lateness
        double fPendingLate = 0.0; // an underrun's lateness, applied once the card has shown it
                                   // kept running (not if it went away or restarted)
        int64_t fPendingWritten = 0;   // the FIFO's frames written at that underrun
        int64_t fLearnCycles = 0;
};

class JackBridgePlayback : public JackBridgeDirection
{
    public:

        JackBridgePlayback(): fWritten(0) {}

        // graph side, RT
        void Cycle(int64_t t_ns, int n, double master_ns_per_frame, const float* const* in);

        // card side: the card position the FIFO's frame Read() is for, if placed
        bool Placement(uint32_t* epoch, int64_t* index, int64_t* pos) const { return fAnchor.Get(epoch, index, pos); }

    protected:

        double NominalRatio() const { return fParams.fMasterRate / fParams.fCardRate; }

    private:

        int64_t fWritten;          // card frames the resampler made since the start
};

/*!
\brief Card side of a capture direction (the card's I/O thread; no ALSA here, so the simulator
 uses it too). Positions are the card's absolute frame counts.
*/
class JackBridgeCardCapture
{
    public:

        JackBridgeCardCapture(): fDir(NULL), fEpoch(0), fNext(0), fRate(48000.0), fPpm(0.0) {}

        void Attach(JackBridgeCapture* dir, double rate, double ppm_hint) { fDir = dir; fRate = rate; fPpm = ppm_hint; }
        // (re)start at card position `next` (the first frame Write will get): a new epoch
        void Start(int64_t next);
        // frames read from the card, interleaved float, the next ones in card order; a gap
        // (frames the card lost) is given as a new Start
        void Write(const float* frames, int count);
        // the card's position (frames captured) at the kernel timestamp t_ns
        void Position(int64_t pos, int64_t t_ns) { fDir->Clock().Update(pos, t_ns); }
        uint32_t Epoch() const { return fEpoch; }
        int64_t Overflows() const { return fOverflows; }

    private:

        JackBridgeCapture* fDir;
        uint32_t fEpoch;
        int64_t fNext;
        double fRate;
        double fPpm;
        int64_t fOverflows = 0;
};

/*!
\brief Card side of a playback direction.
*/
class JackBridgeCardPlayback
{
    public:

        JackBridgeCardPlayback(): fDir(NULL), fEpoch(0), fRate(48000.0), fPpm(0.0) {}

        void Attach(JackBridgePlayback* dir, double rate, double ppm_hint) { fDir = dir; fRate = rate; fPpm = ppm_hint; }
        void Start();                      // a new epoch (the card restarted)
        // Fill interleaved float frames for card positions from `pos`, up to `count`: the
        // graph side's frames placed there, as many as it has made. If that is fewer than
        // `must` (what the card needs queued to last until the next wakeup), silence makes up
        // the difference; once placed, those positions are counted as missing. Returns the
        // frames filled (at least `must`, at most `count`).
        int Fill(float* frames, int count, int64_t pos, int must);
        void Position(int64_t pos, int64_t t_ns) { fDir->Clock().Update(pos, t_ns); }
        int64_t Missing() const { return fMissing; }

    private:

        JackBridgePlayback* fDir;
        uint32_t fEpoch;
        double fRate;
        double fPpm;
        int64_t fMissing = 0;
};

} // end of namespace

#endif
