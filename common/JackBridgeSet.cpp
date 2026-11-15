/*
 jack2-nspa: sound card bridges as the master driver sees them (see JackBridgeSet.h).
*/

#include "JackBridgeSet.h"
#include "JackError.h"

#include <stdlib.h>
#include <time.h>

namespace Jack
{

JackBridgeSet::~JackBridgeSet()
{
    free(fCurrent.load(std::memory_order_relaxed));
}

int JackBridgeSet::Count() const
{
    JackBridgeList* list = fCurrent.load(std::memory_order_acquire);
    return list ? list->fCount : 0;
}

bool JackBridgeSet::Publish(JackBridgeList* next, bool running)
{
    JackBridgeList* old = fCurrent.exchange(next, std::memory_order_acq_rel);
    if (!old) {
        return true;
    }
    if (running) {
        // The master may be in a cycle that took the old list: wait until it has finished two
        // cycles (the one under way, and one that started after the switch). A master that
        // stops meanwhile reads nothing more; one that runs but stalls for seconds keeps the
        // old list (leaked rather than freed under it).
        const uint64_t start = fCycles.load(std::memory_order_acquire);
        for (int i = 0; i < 2000; i++) {
            if (fCycles.load(std::memory_order_acquire) >= start + 2) {
                free(old);
                return true;
            }
            struct timespec ms = { 0, 1000000 };
            nanosleep(&ms, NULL);
        }
        jack_error("JackBridgeSet: the master did not finish a cycle in 2 s: the old bridge list is kept");
        return false;
    }
    free(old);
    return true;
}

bool JackBridgeSet::Add(JackBridgeInterface* bridge, bool running)
{
    JackBridgeList* cur = fCurrent.load(std::memory_order_acquire);
    if (cur && cur->fCount >= BRIDGE_MAX) {
        jack_error("JackBridgeSet: no more than %d bridges", BRIDGE_MAX);
        return false;
    }
    JackBridgeList* next = (JackBridgeList*)calloc(1, sizeof(JackBridgeList));
    if (!next) {
        return false;
    }
    if (cur) {
        memcpy(next, cur, sizeof(JackBridgeList));
    }
    next->fBridges[next->fCount++] = bridge;
    Publish(next, running);
    return true;
}

bool JackBridgeSet::Remove(JackBridgeInterface* bridge, bool running)
{
    JackBridgeList* cur = fCurrent.load(std::memory_order_acquire);
    if (!cur) {
        return false;
    }
    JackBridgeList* next = (JackBridgeList*)calloc(1, sizeof(JackBridgeList));
    if (!next) {
        return false;
    }
    bool found = false;
    for (int i = 0; i < cur->fCount; i++) {
        if (cur->fBridges[i] == bridge) {
            found = true;
        } else {
            next->fBridges[next->fCount++] = cur->fBridges[i];
        }
    }
    if (!found) {
        free(next);
        return false;
    }
    // once this returns true, the master no longer calls the bridge: it can be closed
    return Publish(next, running);
}

} // end of namespace
