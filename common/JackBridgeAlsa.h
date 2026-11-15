/*
 jack2-nspa: the card side of the sound card bridge for ALSA (see JackBridgeCore.h).

 One card, one or both directions, its own realtime thread. The thread waits on the card
 (poll), reads what the card captured into the capture FIFO and writes what the graph side
 placed into the card's playback buffer, and feeds each direction's clock with the position and
 kernel timestamp of the card's last update (snd_pcm_htimestamp: from the status page).
 An xrun starts a new epoch (the graph side places its delay again); a card that is gone
 (ENODEV, unplugged) ends the thread with the state kAlsaLost.
*/

#ifndef __JackBridgeAlsa__
#define __JackBridgeAlsa__

#include "JackBridgeStream.h"

#include <alsa/asoundlib.h>
#include <pthread.h>

namespace Jack
{

/* How the card thread wakes (per card). The timer costs nothing to arm (its deadline comes from
   the frames last handled, no system call) and the interrupt still wakes the thread when it is
   on time; what differs is whether a timeout asks the card for its position (hwsync). */
enum JackBridgeAlsaWake {
    kWakeAuto = 0,     // timer and hwsync on interrupt-driven cards, a backstop on batch ones
    kWakeTimer,        // timer and hwsync (not on batch cards: their position cannot move between transfers)
    kWakeIrq           // the interrupt only, with a long backstop that does not hwsync
};

enum JackBridgeAlsaState {
    kAlsaClosed = 0,
    kAlsaRunning,
    kAlsaLost,      // the card went away
    kAlsaFailed     // could not recover
};

struct JackBridgeAlsaParams
{
    const char* fCapture;      // PCM name, or NULL
    const char* fPlayback;     // PCM name, or NULL
    unsigned fRate;            // wanted (the master's): the card's actual one is reported
    int fPeriod;               // frames
    int fPeriods;
    int fCaptureChannels;      // 0: the card's
    int fPlaybackChannels;
    int fPriority;             // SCHED_FIFO priority of the card's thread
    bool fLink;                // start and stop capture and playback together (one card)
    int fWake;                 // JackBridgeAlsaWake
    int fWakeMargin;           // us after a period is due before the timer fires (0: 200)

    JackBridgeAlsaParams()
        : fCapture(NULL), fPlayback(NULL), fRate(48000), fPeriod(128), fPeriods(2),
          fCaptureChannels(0), fPlaybackChannels(0), fPriority(70), fLink(true),
          fWake(kWakeAuto), fWakeMargin(0)
    {}
};

class JackBridgeAlsa
{
    public:

        JackBridgeAlsa();
        ~JackBridgeAlsa();

        // non-RT: opens and configures the card (no thread yet). Afterwards Rate(), Period()
        // and the channel counts are the card's real ones.
        bool Open(const JackBridgeAlsaParams& params);
        unsigned Rate() const { return fRate; }
        int Period() const { return fPeriod; }
        int CaptureChannels() const { return fCapChannels; }
        int PlaybackChannels() const { return fPlayChannels; }
        bool Batch() const { return fBatch; }

        // connects the directions (either may be NULL), then starts the card and its thread
        bool Start(JackBridgeCapture* capture, JackBridgePlayback* playback);
        void Stop();     // non-RT: joins the thread, closes the card
        // non-RT, the card lost (unplugged) or failed: open it again with what Open was last
        // given and start it with the directions Start was last given, if it comes back as they
        // were set up for (the rate and channel counts Start saw). Positions go on counting;
        // the directions see a new epoch and place their delays again. 1: started (State() says
        // whether it runs), 0: the card is not there, -1: it could not be opened or started
        // (FailReason).
        int Reopen();
        // non-RT, the card closed (Stop) and not opened again as wanted (a new rate, while it is
        // away or busy): lost, for Reopen, with the directions set up for this card rate and the
        // channel counts they had
        void SetLost(JackBridgeCapture* capture, JackBridgePlayback* playback, unsigned rate);
        // what Open was last given (its names are the card's own copies)
        const JackBridgeAlsaParams& Params() const { return fParams; }
        int State() const { return fState.load(std::memory_order_acquire); }
        // why the card failed, or why Open or Reopen did not open it ("snd_pcm_start: Broken
        // pipe"); read after State() says kAlsaFailed, or on Open's or Reopen's thread
        void FailReason(char* buf, size_t size) const;
        long Xruns() const { return fXruns.load(std::memory_order_relaxed); }
        long TimerWakes() const { return fTimerWakes; }   // informational, card thread's

    private:

        static void* ThreadEntry(void* arg);
        void Run();
        bool Configure(snd_pcm_t* pcm, bool capture, int* channels, snd_pcm_format_t* format);
        int Recover(snd_pcm_t* pcm, int err, bool capture);
        bool ReadCapture();
        bool WritePlayback(bool force);
        bool StartStreams();
        bool Failed(const char* what, int err);   // records why (FailReason); false

        JackBridgeAlsaParams fParams;   // its names point at the two below
        char fCaptureName[256];         // the PCM names, kept: what Open was given need not last
        char fPlaybackName[256];
        snd_pcm_t* fCap;
        snd_pcm_t* fPlay;
        snd_pcm_format_t fCapFormat;
        snd_pcm_format_t fPlayFormat;
        unsigned fRate;
        int fPeriod;
        int fBufferSize;
        int fCapChannels;
        int fPlayChannels;
        bool fBatch;
        bool fLinked;

        JackBridgeCardCapture fCardCap;
        JackBridgeCardPlayback fCardPlay;
        bool fHaveCap;
        bool fHavePlay;
        JackBridgeCapture* fStartedCap;      // what Start was given, and what it saw (Reopen)
        JackBridgePlayback* fStartedPlay;
        unsigned fStartedRate;
        int fStartedCapChannels;
        int fStartedPlayChannels;
        int64_t fCapRead;          // card frames read (capture position of the next one)
        int64_t fPlayWritten;      // card frames written (playback position of the next one)
        // One card, one clock: when capture and playback are linked, the playback direction's
        // clock is fed from the capture side's positions, playback position = capture + this.
        // A USB card's playback position moves when its frames go into a transfer, not when they
        // are played, so its timestamps carry the transfer queue's jitter; capture positions
        // move when frames arrive. Set at each start (both streams start together).
        int64_t fPlayOffset;
        // The card side's own wakeup times (CLOCK_MONOTONIC ns): a card's period interrupt can
        // come late or not at all (Intel HDA), so the thread also wakes when, by the frames it
        // last handled, a period is due, and then asks the card for its position
        int64_t fLastRead;         // when capture was last read
        int64_t fLastFill;         // when playback was last filled
        int fLastQueued;           // playback frames queued after that fill
        long fTimerWakes;
        float* fConv;              // interleaved float scratch (one buffer)

        const char* fFailWhat;     // the call that failed, and its error (negative errno)
        int fFailErr;

        pthread_t fThread;
        bool fThreadRunning;
        std::atomic<bool> fRun;
        std::atomic<int> fState;
        std::atomic<long> fXruns;
};

} // end of namespace

#endif
