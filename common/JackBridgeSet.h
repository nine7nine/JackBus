/*
 jack2-nspa: sound card bridges as the master driver sees them.

 A bridge (the "bridge" driver, linux/bridge) joins a card on its own clock to the master's graph.
 The master runs every bridge in its own cycle: each bridge's playback side right after the
 master's own playback (from the previous cycle's buffers, as the master's own ports in async
 mode), each bridge's capture side right after the graph has started (it fills its ports and
 signals the clients that read them). Neither ever waits.

 Bridges come and go while the server runs, so the master reads a published list: a change
 publishes a new list, and the old one is freed once the master has gone through two cycles
 (or straight away if it is not running).
*/

#ifndef __JackBridgeSet__
#define __JackBridgeSet__

#include "JackCompilerDeps.h"
#include "types.h"

#include <atomic>
#include <stdint.h>
#include <string.h>

namespace Jack
{

/*!
\brief What the master calls on a bridge (implemented by the bridge driver).
*/
class SERVER_EXPORT JackBridgeInterface
{
    public:

        virtual ~JackBridgeInterface() {}

        // RT, the master's thread. t_ns: the cycle's start (the master's frame timer, on
        // CLOCK_MONOTONIC_RAW); ns_per_frame: the master's measured frame period; n: frames.
        virtual void BridgePlayback(int64_t t_ns, double ns_per_frame, jack_nframes_t n) = 0;
        virtual void BridgeCapture(int64_t t_ns, double ns_per_frame, jack_nframes_t n) = 0;

        // non-RT, the master stopped: its buffer size changed
        virtual int BridgeBufferSize(jack_nframes_t buffer_size) = 0;

        // non-RT, about four times a second (the server's bridge monitor): the ports' latencies
        // follow the delays as placed; state changes are logged
        virtual void BridgeMonitor() = 0;
};

#define BRIDGE_MAX 16

struct JackBridgeList
{
    int fCount;
    JackBridgeInterface* fBridges[BRIDGE_MAX];
};

class SERVER_EXPORT JackBridgeSet
{
    public:

        JackBridgeSet(): fCurrent(NULL), fCycles(0) {}
        ~JackBridgeSet();

        // RT: the list for this cycle (NULL: none), and the end of the cycle
        JackBridgeList* Acquire() { return fCurrent.load(std::memory_order_acquire); }
        void CycleDone() { fCycles.fetch_add(1, std::memory_order_release); }

        // non-RT (the server's control thread, one at a time). running: whether the master
        // runs cycles now (if not, nothing reads the old list)
        bool Add(JackBridgeInterface* bridge, bool running);
        bool Remove(JackBridgeInterface* bridge, bool running);
        int Count() const;

    private:

        bool Publish(JackBridgeList* next, bool running);

        std::atomic<JackBridgeList*> fCurrent;
        std::atomic<uint64_t> fCycles;
};

} // end of namespace

#endif
