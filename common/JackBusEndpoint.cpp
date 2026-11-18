/*
Copyright (C) 2026 JackBus

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#include "JackBusEndpoint.h"
#include "JackGraphManager.h"
#include "JackEngineControl.h"
#include "JackFrameTimer.h"
#include "JackError.h"
#include <new>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0008U
#endif

namespace Jack
{

// the layout jack/jackbus.h describes: the same in C (stdatomic) and C++ (std::atomic)
static_assert(offsetof(jackbus_endpoint_shared_t, w) == 64, "endpoint layout");
static_assert(offsetof(jackbus_endpoint_shared_t, r) == 128, "endpoint layout");
static_assert(offsetof(jackbus_endpoint_shared_t, run) == 192, "endpoint layout");
static_assert(offsetof(jackbus_endpoint_shared_t, gain) == 256, "endpoint layout");
static_assert(offsetof(jackbus_endpoint_shared_t, started) == 512, "endpoint layout");
static_assert(sizeof(jackbus_endpoint_shared_t) == 600, "endpoint layout");

namespace
{

const size_t kHeader = 4096;    // the shared header's page; the rings from there

uint64_t Ns(clockid_t clock)
{
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

} // namespace

JackBusEndpointSet::JackBusEndpointSet()
    : fCurrent(NULL), fCycles(0), fInCycle(0), fNextId(1), fFrame64(0), fLastFrames(0),
      fHaveFrames(false), fTimeNs(0), fRawNs(0), fNextNs(0), fPeriodNs(0), fSampleRate(0)
{}

JackBusEndpointSet::~JackBusEndpointSet()
{
    // the master is gone: nothing reads the list
    JackBusEndpointList* list = fCurrent.exchange(NULL);
    for (int i = 0; list && i < list->fCount; i++) {
        Free(list->fEndpoints[i]);
    }
    free(list);
}

void JackBusEndpointSet::Free(JackBusEndpoint* e)
{
    if (e->fShared) {
        munmap(e->fShared, e->fSize);
    }
    delete e;
}

/*
 After the exchange, a cycle that looks at the list sees the new one: the old one can only be held
 by a cycle that started before (fInCycle set before the list is read, both seq_cst). Wait for it
 to end; one stuck for 2 s keeps the old list (leaked, never freed under it).
*/
bool JackBusEndpointSet::Publish(JackBusEndpointList* next)
{
    JackBusEndpointList* old = fCurrent.exchange(next, std::memory_order_seq_cst);
    const uint64_t start = fCycles.load(std::memory_order_seq_cst);
    for (int i = 0; fInCycle.load(std::memory_order_seq_cst) && i < 20000; i++) {
        if (fCycles.load(std::memory_order_seq_cst) != start) {
            break;
        }
        struct timespec ts = { 0, 100 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (fInCycle.load(std::memory_order_seq_cst) && fCycles.load(std::memory_order_seq_cst) == start) {
        jack_error("JackBusEndpointSet: a cycle has run for 2 s: the old endpoint list is kept");
        return false;
    }
    free(old);
    return true;
}

int JackBusEndpointSet::Open(int refnum, const jack_port_id_t* ports, int count, bool playback, int ring_frames,
                             JackGraphManager* graph, int* fd)
{
    *fd = -1;
    if (count < 1 || count > JACKBUS_ENDPOINT_MAX_CHANNELS || ring_frames < 1 || ring_frames > (1 << 20)) {
        return -EINVAL;
    }
    JackBusEndpointList* cur = fCurrent.load(std::memory_order_seq_cst);
    if (cur && cur->fCount >= ENDPOINT_MAX) {
        jack_error("JackBusEndpointSet: no more than %d endpoints", ENDPOINT_MAX);
        return -ENOSPC;
    }
    uint32_t frames = 64;
    while (frames < (uint32_t)ring_frames) {
        frames <<= 1;
    }
    const size_t size = (kHeader + (size_t)count * frames * sizeof(float) + 4095) & ~(size_t)4095;

    int mfd = memfd_create("jackbus-endpoint", MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL);
    if (mfd < 0 && errno == EINVAL) {
        mfd = memfd_create("jackbus-endpoint", MFD_CLOEXEC | MFD_ALLOW_SEALING);   // older kernels
    }
    if (mfd < 0) {
        return -errno;
    }
    void* mem = MAP_FAILED;
    if (ftruncate(mfd, (off_t)size) < 0
        || fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0
        || (mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, mfd, 0)) == MAP_FAILED) {
        int err = errno;
        close(mfd);
        return -err;
    }
    mlock(mem, size);   // best effort (a realtime server is mlockall()ed anyway)

    JackBusEndpoint* e = new (std::nothrow) JackBusEndpoint();
    JackBusEndpointList* next = (JackBusEndpointList*)calloc(1, sizeof(JackBusEndpointList));
    if (!e || !next) {
        delete e;
        free(next);
        munmap(mem, size);
        close(mfd);
        return -ENOMEM;
    }
    jackbus_endpoint_shared_t* sh = new (mem) jackbus_endpoint_shared_t();
    sh->size = sizeof(jackbus_endpoint_shared_t);
    sh->channels = (uint32_t)count;
    sh->ring_frames = frames;
    sh->playback = playback ? 1 : 0;
    sh->ring_offset = kHeader;
    for (int c = 0; c < JACKBUS_ENDPOINT_MAX_CHANNELS; c++) {
        sh->gain[c].store(1.0f, std::memory_order_relaxed);
    }
    e->fId = fNextId++;
    e->fRefNum = refnum;
    e->fPlayback = playback;
    e->fChannels = count;
    e->fRingFrames = frames;
    memcpy(e->fPorts, ports, count * sizeof(jack_port_id_t));
    e->fShared = sh;
    e->fRing = reinterpret_cast<float*>(static_cast<char*>(mem) + kHeader);
    e->fSize = size;

    for (int i = 0; i < count; i++) {
        JackGraphManager::SetNoEdge(ports[i], true);
    }
    if (cur) {
        memcpy(next, cur, sizeof(JackBusEndpointList));
    }
    next->fEndpoints[next->fCount++] = e;
    Publish(next);
    jack_log("JackBusEndpointSet::Open ref = %d id = %d %s, %d channels, ring %u frames",
             refnum, e->fId, playback ? "playback" : "capture", count, frames);
    *fd = mfd;
    return e->fId;
}

int JackBusEndpointSet::Close(int refnum, int id, JackGraphManager* graph)
{
    JackBusEndpointList* cur = fCurrent.load(std::memory_order_seq_cst);
    if (!cur) {
        return -ENOENT;
    }
    JackBusEndpointList* next = (JackBusEndpointList*)calloc(1, sizeof(JackBusEndpointList));
    if (!next) {
        return -ENOMEM;
    }
    JackBusEndpoint* gone = NULL;
    for (int i = 0; i < cur->fCount; i++) {
        JackBusEndpoint* e = cur->fEndpoints[i];
        if (!gone && e->fRefNum == refnum && (id < 0 || e->fId == id)) {
            gone = e;
        } else {
            next->fEndpoints[next->fCount++] = e;
        }
    }
    if (!gone) {
        free(next);
        return -ENOENT;
    }
    if (!Publish(next)) {
        return -EBUSY;      // kept, as the list
    }
    // the ports stay edge-less until released (their connections were made without edges);
    // the outputs no longer filled: silent
    if (gone->fPlayback && graph) {
        for (int c = 0; c < gone->fChannels; c++) {
            // an output's own buffer (a port already released: the unused port 0's)
            memset(graph->GetBuffer(gone->fPorts[c], BUFFER_SIZE_MAX), 0,
                   BUFFER_SIZE_MAX * sizeof(jack_default_audio_sample_t));
        }
    }
    jack_log("JackBusEndpointSet::Close ref = %d id = %d", refnum, gone->fId);
    Free(gone);
    return 0;
}

void JackBusEndpointSet::CloseClient(int refnum, JackGraphManager* graph)
{
    while (Close(refnum, -1, graph) == 0) {
    }
}

bool JackBusEndpointSet::HasPort(jack_port_id_t port) const
{
    JackBusEndpointList* cur = fCurrent.load(std::memory_order_seq_cst);
    for (int i = 0; cur && i < cur->fCount; i++) {
        for (int c = 0; c < cur->fEndpoints[i]->fChannels; c++) {
            if (cur->fEndpoints[i]->fPorts[c] == port) {
                return true;
            }
        }
    }
    return false;
}

void JackBusEndpointSet::ClosePort(jack_port_id_t port, JackGraphManager* graph)
{
    JackBusEndpointList* cur = fCurrent.load(std::memory_order_seq_cst);
    for (int i = 0; cur && i < cur->fCount; i++) {
        JackBusEndpoint* e = cur->fEndpoints[i];
        for (int c = 0; c < e->fChannels; c++) {
            if (e->fPorts[c] == port) {
                Close(e->fRefNum, e->fId, graph);
                return;
            }
        }
    }
}

void JackBusEndpointSet::Silence(JackGraphManager* graph, jack_nframes_t frames)
{
    JackBusEndpointList* cur = fCurrent.load(std::memory_order_seq_cst);
    for (int i = 0; cur && i < cur->fCount; i++) {
        JackBusEndpoint* e = cur->fEndpoints[i];
        for (int c = 0; e->fPlayback && c < e->fChannels; c++) {
            memset(graph->GetBuffer(e->fPorts[c], frames), 0, frames * sizeof(jack_default_audio_sample_t));
        }
    }
}

/* -------------------------------------------------------------- the cycle */

void JackBusEndpointSet::Run(JackGraphManager* graph, JackEngineControl* control, jack_nframes_t n)
{
    fInCycle.store(1, std::memory_order_seq_cst);
    JackBusEndpointList* list = fCurrent.load(std::memory_order_seq_cst);
    if (list && list->fCount > 0) {
        // the cycle's time, as a node's cycle record has it (no system call: vDSO clocks)
        JackTimer timer;
        jack_nframes_t frames = 0;
        jack_time_t usecs = 0, next_usecs = 0;
        float period_usecs = 0.0f;
        control->ReadFrameTime(&timer);
        if (timer.GetCycleTimes(&frames, &usecs, &next_usecs, &period_usecs) == 0) {
            if (!fHaveFrames) {
                fFrame64 = frames;
                fHaveFrames = true;
            } else {
                fFrame64 += (uint32_t)(frames - fLastFrames);
            }
            fLastFrames = frames;
            const int64_t offset = (int64_t)Ns(CLOCK_MONOTONIC) - (int64_t)Ns(CLOCK_MONOTONIC_RAW);
            fRawNs = usecs * 1000ULL;
            fTimeNs = (uint64_t)((int64_t)fRawNs + offset);
            fNextNs = (uint64_t)((int64_t)(next_usecs * 1000ULL) + offset);
            fPeriodNs = (uint64_t)(period_usecs * 1000.0f);
        }
        fSampleRate = control->fSampleRate;
        for (int i = 0; i < list->fCount; i++) {
            RunOne(list->fEndpoints[i], graph, n);
        }
    }
    fCycles.fetch_add(1, std::memory_order_seq_cst);
    fInCycle.store(0, std::memory_order_seq_cst);
}

/*
 One endpoint, one cycle. The client's positions are never trusted: the ring's indexes are masked,
 and a position that would put more than the ring between w and r is clamped. Playback follows a
 PulseAudio stream's rules (the shims' realtime sides had them): prebuffering, a flush, a drain,
 underflows counted.
*/
void JackBusEndpointSet::RunOne(JackBusEndpoint* e, JackGraphManager* graph, jack_nframes_t n)
{
    jackbus_endpoint_shared_t* sh = e->fShared;
    const uint32_t size = e->fRingFrames, mask = size - 1;
    const int channels = e->fChannels;
    float* port[JACKBUS_ENDPOINT_MAX_CHANNELS];
    for (int c = 0; c < channels; c++) {
        port[c] = static_cast<float*>(graph->GetBuffer(e->fPorts[c], n));
    }
    const uint64_t before = sh->moved.load(std::memory_order_relaxed);
    uint32_t moved = 0;
    const bool run = sh->run.load(std::memory_order_acquire) != 0;

    if (e->fPlayback) {
        uint64_t w = sh->w.load(std::memory_order_acquire);
        uint64_t r = sh->r.load(std::memory_order_relaxed);
        if (w < r) {
            r = w;                      // the application went back: nothing to play
        } else if (w - r > size) {
            r = w - size;               // beyond the ring: the oldest frames are gone
        }
        const uint64_t skip = sh->skip_to.load(std::memory_order_acquire);
        if (skip > r) {
            r = (skip < w) ? skip : w;  // a flush
        }
        if (run) {
            const uint64_t fill = w - r;
            const bool draining = sh->draining.load(std::memory_order_relaxed) != 0;
            uint32_t prebuf = sh->prebuf.load(std::memory_order_relaxed);
            if (prebuf > size) {
                prebuf = size;
            }
            if (!sh->started.load(std::memory_order_relaxed) && (fill >= prebuf || draining)) {
                sh->started.store(1, std::memory_order_release);
            }
            if (sh->started.load(std::memory_order_relaxed)) {
                moved = (uint32_t)((fill < n) ? fill : n);
                const uint32_t idx = (uint32_t)(r & mask);
                const uint32_t first = (moved < size - idx) ? moved : size - idx;
                for (int c = 0; c < channels; c++) {
                    const float g = sh->gain[c].load(std::memory_order_relaxed);
                    const float* a = e->fRing + (size_t)c * size + idx;
                    const float* b = e->fRing + (size_t)c * size;
                    float* dst = port[c];
                    if (g == 1.0f) {
                        memcpy(dst, a, first * sizeof(float));
                        memcpy(dst + first, b, (moved - first) * sizeof(float));
                    } else {
                        for (uint32_t k = 0; k < first; k++) {
                            dst[k] = a[k] * g;
                        }
                        for (uint32_t k = first; k < moved; k++) {
                            dst[k] = b[k - first] * g;
                        }
                    }
                }
                if (moved < n && !draining) {
                    sh->underflows.fetch_add(1, std::memory_order_relaxed);
                    if (prebuf > 0) {
                        sh->started.store(0, std::memory_order_relaxed);
                    }
                }
            }
        }
        for (int c = 0; c < channels; c++) {
            memset(port[c] + moved, 0, (n - moved) * sizeof(float));
        }
        sh->r.store(r + moved, std::memory_order_release);
    } else {
        uint64_t r = sh->r.load(std::memory_order_acquire);
        const uint64_t w = sh->w.load(std::memory_order_relaxed);
        if (r > w) {
            r = w;
        }
        if (run) {
            const uint64_t fill = w - r;
            if (fill > size || size - fill < n) {
                sh->overflows.fetch_add(1, std::memory_order_relaxed);
            } else {
                const uint32_t idx = (uint32_t)(w & mask);
                const uint32_t first = (n < size - idx) ? n : size - idx;
                for (int c = 0; c < channels; c++) {
                    const float g = sh->gain[c].load(std::memory_order_relaxed);
                    float* a = e->fRing + (size_t)c * size + idx;
                    float* b = e->fRing + (size_t)c * size;
                    const float* src = port[c];
                    if (g == 1.0f) {
                        memcpy(a, src, first * sizeof(float));
                        memcpy(b, src + first, (n - first) * sizeof(float));
                    } else {
                        for (uint32_t k = 0; k < first; k++) {
                            a[k] = src[k] * g;
                        }
                        for (uint32_t k = first; k < n; k++) {
                            b[k - first] = src[k] * g;
                        }
                    }
                }
                moved = n;
                sh->w.store(w + n, std::memory_order_release);
            }
        }
    }

    // the cycle as it went, for the application's interpolation (a seqlock)
    const uint32_t q = sh->seq.load(std::memory_order_relaxed);
    sh->seq.store(q + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    sh->last_moved.store(before, std::memory_order_relaxed);
    sh->last_frames.store(moved, std::memory_order_relaxed);
    sh->time_ns.store(fTimeNs, std::memory_order_relaxed);
    sh->raw_ns.store(fRawNs, std::memory_order_relaxed);
    sh->next_ns.store(fNextNs, std::memory_order_relaxed);
    sh->period_ns.store(fPeriodNs, std::memory_order_relaxed);
    sh->frame.store(fFrame64, std::memory_order_relaxed);
    sh->nframes.store(n, std::memory_order_relaxed);
    sh->sample_rate.store(fSampleRate, std::memory_order_relaxed);
    sh->seq.store(q + 2, std::memory_order_release);
    sh->moved.store(before + moved, std::memory_order_release);
}

} // end of namespace
