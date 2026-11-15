/*
 JackBus: an opt-in trace of the master driver's cycle, for measurements.

 JACK_CYCLE_TRACE=<file> turns it on when the server starts. The master then notes, in ns on
 CLOCK_MONOTONIC_RAW (the clock JACK's µs come from), when each part of its cycle ends: its
 wake-up, Read, Write, the bridges' playback and capture, Engine::Process, its own resume, the
 slaves' resume (the freewheel driver's: the first clients wake there) and the slaves' write
 (with -S, the wait for the graph's end). At the next cycle's start it also copies the clients'
 timing records of the cycle that ended (signalled, woken, finished: the µs every libjack writes),
 for the refnums below JACK_CYCLE_TRACE_CLIENTS (default 48).

 The cycles go into a ring preallocated at start (JACK_CYCLE_TRACE_CYCLES, default 16384), the
 oldest overwritten. When the server stops, after its master's thread, the ring is written to
 the file as text with the graph as it was then (each refnum's name, from its ports, and the
 refnums it signals). A server that starts again writes <file>.1, <file>.2 and so on.

 Off (the default), each mark is a load and a branch. Nothing here changes what clients see.
*/

#ifndef __JackCycleTrace__
#define __JackCycleTrace__

#include "JackCompilerDeps.h"
#include "types.h"

#include <stdint.h>
#include <time.h>

namespace Jack
{

class JackGraphManager;
struct JackEngineControl;

enum JackCycleTracePoint {
    kTraceEntry = 0,        // the driver's cycle starts (after the dummy driver's sleep)
    kTraceWake,             // ALSA: the wait for the card returned
    kTraceRead,
    kTraceWrite,
    kTraceBridgePlayback,
    kTraceProcess,          // Engine::Process
    kTraceResume,           // the master's own ResumeRefNum
    kTraceBridgeCapture,
    kTraceReadSlaves,       // the slaves' resume (the freewheel driver: the first wakes)
    kTraceWriteSlaves,      // -S: the wait for the graph's end
    kTraceEnd,
    kTracePoints
};

enum {
    kTraceFlagSync = 1,         // the cycle ran in sync mode
    kTraceFlagUnfinished = 2,   // Engine::Process: the graph had not finished
    kTraceFlagClients = 4       // the clients' records of this cycle were copied (at the next one)
};

struct JackCycleTraceClient
{
    int32_t fSignaled, fAwake, fFinished;   // µs from the cycle's fBeginUsecs; INT32_MIN: none
    int32_t fStatus;
};

struct JackCycleTraceRecord
{
    int64_t fT[kTracePoints];   // ns, CLOCK_MONOTONIC_RAW; 0: not reached in this cycle
    jack_time_t fBeginUsecs;    // the driver's fBeginDateUst
    jack_nframes_t fFrames;     // the cycle's frame time (jack_last_frame_time)
    uint16_t fBridges;
    uint16_t fFlags;
};

class SERVER_EXPORT JackCycleTrace
{
    public:

        // non-RT, before the master's thread starts / after it stopped
        static void Setup(JackGraphManager* graph, JackEngineControl* control);
        static void Dump();

        // RT, the master's thread
        static JackCycleTrace* Get() { return __atomic_load_n(&fInstance, __ATOMIC_RELAXED); }

        static int64_t Now()
        {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
            return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
        }

        void Begin()
        {
            fCur = &fRing[fCycles % fSize];
            for (int i = 0; i < kTracePoints; i++) {
                fCur->fT[i] = 0;
            }
            fCur->fT[kTraceEntry] = Now();
            fCur->fFlags = 0;
            fCur->fBridges = 0;
        }
        void Mark(int point)
        {
            if (fCur) {
                fCur->fT[point] = Now();
            }
        }
        void Flag(uint16_t flag)
        {
            if (fCur) {
                fCur->fFlags |= flag;
            }
        }
        void Bridges(int count)
        {
            if (fCur) {
                fCur->fBridges = uint16_t(count);
            }
        }
        // after Read: the clients of the cycle that ended are done (or late)
        void SnapshotPrevious();
        void End(jack_time_t begin_usecs);

    private:

        JackCycleTrace(JackGraphManager* graph, JackEngineControl* control, int size, int clients, const char* path);
        ~JackCycleTrace();
        void Write();

        static JackCycleTrace* fInstance;
        static int fDumps;

        JackGraphManager* fGraph;
        JackEngineControl* fControl;
        JackCycleTraceRecord* fRing;
        JackCycleTraceClient* fClients;     // fSize x fClientNum
        JackCycleTraceRecord* fCur;
        JackCycleTraceRecord* fPrev;
        uint64_t fCycles;
        int fSize;
        int fClientNum;
        char fPath[512];
};

// the marks: nothing unless the trace is on
static inline void JackTraceMark(int point)
{
    if (JackCycleTrace* t = JackCycleTrace::Get()) {
        t->Mark(point);
    }
}

} // end of namespace

#endif
