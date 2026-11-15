/*
 JackBus: the master driver's cycle trace (JackCycleTrace.h).
*/

#include "JackCycleTrace.h"
#include "JackConnectionManager.h"
#include "JackEngineControl.h"
#include "JackError.h"
#include "JackGraphManager.h"
#include "JackPort.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace Jack
{

JackCycleTrace* JackCycleTrace::fInstance = NULL;
int JackCycleTrace::fDumps = 0;

static int EnvInt(const char* name, int def, int lo, int hi)
{
    const char* s = getenv(name);
    if (!s || !*s) {
        return def;
    }
    long v = strtol(s, NULL, 10);
    return (v < lo) ? lo : (v > hi) ? hi : int(v);
}

JackCycleTrace::JackCycleTrace(JackGraphManager* graph, JackEngineControl* control, int size, int clients, const char* path)
    : fGraph(graph), fControl(control), fRing(NULL), fClients(NULL), fCur(NULL), fPrev(NULL), fCycles(0),
      fSize(size), fClientNum(clients)
{
    snprintf(fPath, sizeof(fPath), "%s", path);
    fRing = (JackCycleTraceRecord*)calloc(size_t(size), sizeof(JackCycleTraceRecord));
    fClients = (JackCycleTraceClient*)calloc(size_t(size) * size_t(clients), sizeof(JackCycleTraceClient));
    if (fRing && fClients) {
        // every page touched now (and locked, with mlockall), not in the RT thread
        memset(fRing, 0, size_t(size) * sizeof(JackCycleTraceRecord));
        for (size_t i = 0; i < size_t(size) * size_t(clients); i++) {
            fClients[i].fSignaled = fClients[i].fAwake = fClients[i].fFinished = INT32_MIN;
            fClients[i].fStatus = -1;
        }
    }
}

JackCycleTrace::~JackCycleTrace()
{
    free(fRing);
    free(fClients);
}

void JackCycleTrace::Setup(JackGraphManager* graph, JackEngineControl* control)
{
    const char* path = getenv("JACK_CYCLE_TRACE");
    if (!path || !*path || Get()) {
        return;
    }
    char name[512];
    if (fDumps == 0) {
        snprintf(name, sizeof(name), "%s", path);
    } else {
        snprintf(name, sizeof(name), "%s.%d", path, fDumps);
    }
    const int size = EnvInt("JACK_CYCLE_TRACE_CYCLES", 16384, 16, 1 << 22);
    const int clients = EnvInt("JACK_CYCLE_TRACE_CLIENTS", 48, 0, CLIENT_NUM);
    JackCycleTrace* trace = new JackCycleTrace(graph, control, size, clients, name);
    if (!trace->fRing || (clients > 0 && !trace->fClients)) {
        jack_error("cycle trace: cannot allocate %d cycles", size);
        delete trace;
        return;
    }
    jack_info("cycle trace: the last %d cycles go to %s when the server stops", size, name);
    __atomic_store_n(&fInstance, trace, __ATOMIC_RELEASE);
}

void JackCycleTrace::Dump()
{
    JackCycleTrace* trace = Get();
    if (!trace) {
        return;
    }
    __atomic_store_n(&fInstance, (JackCycleTrace*)NULL, __ATOMIC_RELEASE);
    trace->Write();
    fDumps++;
    delete trace;
}

void JackCycleTrace::SnapshotPrevious()
{
    if (!fPrev || fClientNum == 0) {
        return;
    }
    const jack_time_t base = fPrev->fBeginUsecs;
    JackCycleTraceClient* out = fClients + size_t(fPrev - fRing) * size_t(fClientNum);
    for (int i = 0; i < fClientNum; i++) {
        JackClientTiming* t = fGraph->GetClientTiming(i);
        const jack_time_t s = t->SignaledAt(), a = t->AwakeAt(), f = t->FinishedAt();
        out[i].fSignaled = s ? int32_t(int64_t(s) - int64_t(base)) : INT32_MIN;
        out[i].fAwake = a ? int32_t(int64_t(a) - int64_t(base)) : INT32_MIN;
        out[i].fFinished = f ? int32_t(int64_t(f) - int64_t(base)) : INT32_MIN;
        out[i].fStatus = int32_t(t->Status());
    }
    fPrev->fFlags |= kTraceFlagClients;
}

void JackCycleTrace::End(jack_time_t begin_usecs)
{
    if (!fCur) {
        return;
    }
    JackTimer timer;
    fControl->ReadFrameTime(&timer);
    fCur->fFrames = timer.CurFrame();
    fCur->fBeginUsecs = begin_usecs;
    fCur->fT[kTraceEnd] = Now();
    fPrev = fCur;
    fCur = NULL;
    fCycles++;
}

static void RefName(JackGraphManager* graph, JackConnectionManager* manager, int ref, char* name, size_t size)
{
    const jack_int_t* ports = manager->GetOutputPorts(ref);
    jack_int_t port = (ports && ports[0] != EMPTY) ? ports[0] : EMPTY;
    if (port == EMPTY) {
        ports = manager->GetInputPorts(ref);
        port = (ports && ports[0] != EMPTY) ? ports[0] : EMPTY;
    }
    if (port == EMPTY) {
        snprintf(name, size, "-");
        return;
    }
    snprintf(name, size, "%s", graph->GetPort(port)->GetName());
    char* colon = strchr(name, ':');
    if (colon) {
        *colon = 0;
    }
    for (char* c = name; *c; c++) {
        if (*c == ' ') {
            *c = '_';
        }
    }
}

void JackCycleTrace::Write()
{
    FILE* f = fopen(fPath, "w");
    if (!f) {
        jack_error("cycle trace: cannot write %s", fPath);
        return;
    }
    const uint64_t count = (fCycles < uint64_t(fSize)) ? fCycles : uint64_t(fSize);
    fprintf(f, "# JackBus cycle trace 1\n");
    fprintf(f, "rate %u period %u sync %d cycles %llu total %llu clients %d\n",
            fControl->fSampleRate, fControl->fBufferSize, fControl->fSyncMode ? 1 : 0,
            (unsigned long long)count, (unsigned long long)fCycles, fClientNum);
    static const char* names[kTracePoints] = {
        "entry", "wake", "read", "write", "bridge_playback", "process", "resume",
        "bridge_capture", "read_slaves", "write_slaves", "bridge_playback_late", "end"
    };
    fprintf(f, "points");
    for (int i = 0; i < kTracePoints; i++) {
        fprintf(f, " %s", names[i]);
    }
    fprintf(f, "\n");

    // the graph now: each refnum in use, its name, the refnums it signals
    JackConnectionManager* manager = fGraph->ReadCurrentState();
    char name[JACK_PORT_NAME_SIZE + 1];
    for (int ref = 0; ref < CLIENT_NUM; ref++) {
        bool used = false;
        for (int to = 0; to < CLIENT_NUM && !used; to++) {
            used = manager->IsDirectConnection(ref, to) || manager->IsDirectConnection(to, ref);
        }
        if (!used) {
            continue;
        }
        RefName(fGraph, manager, ref, name, sizeof(name));
        fprintf(f, "ref %d %s", ref, name);
        for (int to = 0; to < CLIENT_NUM; to++) {
            if (manager->IsDirectConnection(ref, to)) {
                fprintf(f, " %d", to);
            }
        }
        fprintf(f, "\n");
    }

    // c <frames> <begin µs> <flags> <bridges> <ns per point...>
    // t <ref> <status> <signalled> <awake> <finished> (µs from the cycle's begin; - : none)
    const uint64_t first = fCycles - count;
    for (uint64_t k = first; k < fCycles; k++) {
        const size_t idx = size_t(k % uint64_t(fSize));
        const JackCycleTraceRecord& r = fRing[idx];
        fprintf(f, "c %u %llu %u %u", r.fFrames, (unsigned long long)r.fBeginUsecs, r.fFlags, r.fBridges);
        for (int i = 0; i < kTracePoints; i++) {
            fprintf(f, " %lld", (long long)r.fT[i]);
        }
        fprintf(f, "\n");
        const JackCycleTraceClient* cl = fClients + idx * size_t(fClientNum);
        for (int i = 0; i < fClientNum && (r.fFlags & kTraceFlagClients); i++) {
            if (cl[i].fAwake == INT32_MIN && cl[i].fFinished == INT32_MIN && cl[i].fSignaled == INT32_MIN) {
                continue;
            }
            fprintf(f, "t %d %d", i, cl[i].fStatus);
            const int32_t v[3] = { cl[i].fSignaled, cl[i].fAwake, cl[i].fFinished };
            for (int j = 0; j < 3; j++) {
                if (v[j] == INT32_MIN) {
                    fprintf(f, " -");
                } else {
                    fprintf(f, " %d", v[j]);
                }
            }
            fprintf(f, "\n");
        }
    }
    fclose(f);
    jack_info("cycle trace: %llu cycles written to %s", (unsigned long long)count, fPath);
}

} // end of namespace
