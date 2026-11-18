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

/*
 JackBus: async endpoints, the server's side (jack/jackbus.h).

 An endpoint is a group of a client's audio ports, all outputs or all inputs, that the master
 driver itself serves once per cycle from or into a ring in memory it shares with the client
 (a sealed memfd: the client can neither shrink nor grow it). The client's side runs on its own
 threads at its own pace; nothing in the cycle waits for it.

 Where, in the master's cycle (both modes): after the engine's Process, before any client is
 woken. Outputs: the next frames of the ring into the ports, for the clients woken after it.
 Inputs: the ports' mixes into the ring; the clients that feed them wrote them in the previous
 cycle (or the driver's capture, in this one), so they are whole. The ports' connections make no
 activation edge (JackGraphManager::SetNoEdge): no client waits for an endpoint, and an endpoint
 waits for none.

 The list the master reads is published as the bridges' is (JackBridgeSet): a change publishes a
 new list, and an endpoint closed is freed only once no cycle can still hold it. Without the
 master running (stopped, freewheeling), nothing reads the list.
*/

#ifndef __JackBusEndpoint__
#define __JackBusEndpoint__

#include "JackCompilerDeps.h"
#include "types.h"
#include <jack/jackbus.h>
#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace Jack
{

class JackGraphManager;
class JackEngineControl;

#define ENDPOINT_MAX 256

struct JackBusEndpoint
{
    int fId;
    int fRefNum;
    bool fPlayback;
    int fChannels;
    uint32_t fRingFrames;
    jack_port_id_t fPorts[JACKBUS_ENDPOINT_MAX_CHANNELS];
    jackbus_endpoint_shared_t* fShared;
    float* fRing;               // channel c's ring at fRing + c * fRingFrames
    size_t fSize;               // of the mapping
};

struct JackBusEndpointList
{
    int fCount;
    JackBusEndpoint* fEndpoints[ENDPOINT_MAX];
};

class SERVER_EXPORT JackBusEndpointSet
{
    public:

        JackBusEndpointSet();
        ~JackBusEndpointSet();

        // non-RT, the engine (one at a time, under its lock). Open: the ports are checked by the
        // caller (the client's, audio, one direction, unconnected or already the endpoints');
        // returns the endpoint's id and its memory's fd in *fd (the caller sends then closes it),
        // or -errno.
        int Open(int refnum, const jack_port_id_t* ports, int count, bool playback, int ring_frames,
                 JackGraphManager* graph, int* fd);
        // the endpoint is no longer used when they return (false: a cycle held it for 2 s: kept)
        int Close(int refnum, int id, JackGraphManager* graph);
        void CloseClient(int refnum, JackGraphManager* graph);
        void ClosePort(jack_port_id_t port, JackGraphManager* graph);
        bool HasPort(jack_port_id_t port) const;
        // non-RT, the master stopped (freewheel): the outputs silent until it runs again
        void Silence(JackGraphManager* graph, jack_nframes_t frames);

        // RT, the master's thread, once per cycle
        void Run(JackGraphManager* graph, JackEngineControl* control, jack_nframes_t frames);

    private:

        bool Publish(JackBusEndpointList* next);
        void Free(JackBusEndpoint* endpoint);
        void RunOne(JackBusEndpoint* e, JackGraphManager* graph, jack_nframes_t n);

        std::atomic<JackBusEndpointList*> fCurrent;
        std::atomic<uint64_t> fCycles;
        std::atomic<int> fInCycle;
        int fNextId;

        // RT only
        uint64_t fFrame64;
        jack_nframes_t fLastFrames;
        bool fHaveFrames;
        uint64_t fTimeNs, fRawNs, fNextNs, fPeriodNs;
        jack_nframes_t fSampleRate;
};

} // end of namespace

#endif
