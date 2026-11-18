/*
Copyright (C) 2026 JackBus

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

*/

/*
 The native JackBus API: nodes (jack/jackbus.h).

 A node is a JackBusLibClient (a libjack client of the JackBus socket) plus:
 - a process wrapper, called in the client's realtime thread, that fills the
   cycle record and resolves the node's port buffers once, then calls the
   node's process function;
 - a port table published to the realtime thread with an atomic pointer and a
   cycle sequence (odd while a cycle runs): adding or removing a port builds a
   new table, publishes it, and frees the old one only once no cycle can still
   be reading it. The realtime thread never waits, and never skips a cycle;
 - an event queue: libjack's callbacks (on its notification thread) append
   events; the application drains them from its own threads, woken by an
   eventfd written on that non-realtime thread.
*/

#include "JackClient.h"
#include "JackBusLibClient.h"
#include "JackGraphManager.h"
#include "JackEngineControl.h"
#include "JackGlobals.h"
#include "JackFrameTimer.h"
#include "JackError.h"
#include "JackConstants.h"

#include <jack/jack.h>
#include <jack/jackbus.h>

#include <atomic>
#include <mutex>
#include <new>
#include <vector>

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>

namespace Jack
{
// libjack (JackLibAPI.cpp / JackLibClient.cpp)
JackGraphManager* GetGraphManager();
JackEngineControl* GetEngineControl();
}

// JackLibAPI.cpp: jack_client_open over the JackBus socket, varargs as a va_list
jack_client_t* JackBusClientOpenV(const char* name, jack_options_t options, jack_status_t* status, va_list ap);
// JackAPI.cpp (not in the public headers, or in jack/metadata.h, whose types
// libjack's own JackMetadata.h declares too)
extern "C" float jack_get_xrun_delayed_usecs(jack_client_t* client);
extern "C" int jack_set_property_change_callback(jack_client_t* client, JackPropertyChangeCallback callback, void* arg);

using namespace Jack;

struct jackbus_port
{
    jackbus_node* node;
    jack_port_t* jport;
    jack_port_id_t index;
    int kind;
    bool input;
    std::atomic<bool> endpoint;     // an async endpoint's: JackBus moves its buffer, not the node
    void* buffer;           // this cycle's: realtime thread only
};

struct jackbus_endpoint
{
    jackbus_node* node;
    int id;
    jackbus_endpoint_shared_t* shared;
    size_t size;
    unsigned int count;
    jackbus_port* ports[JACKBUS_ENDPOINT_MAX_CHANNELS];
};

namespace
{

struct PortTable
{
    size_t count;
    jackbus_port* ports[1]; // count entries
};

PortTable* NewTable(size_t count)
{
    void* mem = calloc(1, sizeof(PortTable) + (count ? count - 1 : 0) * sizeof(jackbus_port*));
    PortTable* t = static_cast<PortTable*>(mem);
    if (t) {
        t->count = count;
    }
    return t;
}

// One zero buffer for every unconnected audio input of the process: pre-faulted,
// locked, read-only. Made by jackbus_node_open(), never by the realtime thread
// (the first call maps memory; later ones only read the initialized static).
const float* ZeroBuffer()
{
    static const float* zero = []() -> const float* {
        size_t size = (BUFFER_SIZE_MAX * sizeof(float) + 4095) & ~(size_t)4095;
        void* p = mmap(NULL, size, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
        if (p == MAP_FAILED) {
            return NULL;
        }
        mlock(p, size);     // best effort: the pages are the kernel's zero page anyway
        return static_cast<const float*>(p);
    }();
    return zero;
}

uint64_t TimespecNs(const struct timespec& ts)
{
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

} // namespace

struct jackbus_node
{
    jack_client_t* client;
    jackbus_process_t process;
    void* arg;

    // the port table: written under write_lock, read by the realtime thread
    std::mutex write_lock;
    std::atomic<PortTable*> table;
    std::atomic<uint64_t> cycle_seq;    // odd while a cycle runs

    // realtime thread only
    jackbus_cycle_t cycle;
    uint64_t frame64;
    jack_nframes_t last_frames;
    bool have_frames;
    uint64_t xruns_seen;

    // written by callbacks (notification thread), read in the cycle
    std::atomic<uint64_t> xruns;
    std::atomic<bool> freewheel;
    std::atomic<bool> first;
    std::atomic<uint32_t> process_latency;
    bool active;            // application threads (activate, set_process)
    bool realtime;          // activated with a realtime thread (a process function)
    bool priority;          // jackbus_node_set_priority

    // events
    std::mutex event_lock;
    std::vector<jackbus_event_t> events;    // ring
    size_t head, count;
    uint64_t lost;
    int event_fd;
};

/* ------------------------------------------------------------- events */

static void Push(jackbus_node* node, const jackbus_event_t& ev)
{
    {
        std::lock_guard<std::mutex> lock(node->event_lock);
        if (node->count == node->events.size()) {
            node->lost++;
        } else {
            node->events[(node->head + node->count) % node->events.size()] = ev;
            node->count++;
        }
    }
    uint64_t one = 1;
    ssize_t r = write(node->event_fd, &one, sizeof(one));
    (void)r;
}

static jackbus_event_t Event(uint32_t type)
{
    jackbus_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    return ev;
}

static void CopyName(char* dst, const char* src)
{
    if (src) {
        strncpy(dst, src, JACKBUS_NAME_SIZE - 1);
        dst[JACKBUS_NAME_SIZE - 1] = 0;
    }
}

static void OnClientRegistration(const char* name, int on, void* arg)
{
    jackbus_event_t ev = Event(JACKBUS_EVENT_CLIENT);
    ev.on = on ? 1 : 0;
    CopyName(ev.name, name);
    Push(static_cast<jackbus_node*>(arg), ev);
}

static void OnPortRegistration(jack_port_id_t id, int on, void* arg)
{
    jackbus_node* node = static_cast<jackbus_node*>(arg);
    jackbus_event_t ev = Event(JACKBUS_EVENT_PORT);
    ev.on = on ? 1 : 0;
    ev.value = id;
    jack_port_t* port = jack_port_by_id(node->client, id);
    if (port) {
        ev.id = jack_port_uuid(port);
        CopyName(ev.name, jack_port_name(port));
    }
    Push(node, ev);
}

static void OnPortConnect(jack_port_id_t a, jack_port_id_t b, int connect, void* arg)
{
    jackbus_node* node = static_cast<jackbus_node*>(arg);
    jackbus_event_t ev = Event(JACKBUS_EVENT_CONNECT);
    ev.on = connect ? 1 : 0;
    jack_port_t* pa = jack_port_by_id(node->client, a);
    jack_port_t* pb = jack_port_by_id(node->client, b);
    if (pa) {
        CopyName(ev.name, jack_port_name(pa));
    }
    if (pb) {
        CopyName(ev.name2, jack_port_name(pb));
    }
    ev.value = ((uint64_t)a << 32) | b;
    Push(node, ev);
}

static void OnPortRename(jack_port_id_t id, const char* old_name, const char* new_name, void* arg)
{
    jackbus_event_t ev = Event(JACKBUS_EVENT_RENAME);
    ev.value = id;
    CopyName(ev.name, new_name);
    CopyName(ev.name2, old_name);
    Push(static_cast<jackbus_node*>(arg), ev);
}

static int OnGraphOrder(void* arg)
{
    Push(static_cast<jackbus_node*>(arg), Event(JACKBUS_EVENT_GRAPH));
    return 0;
}

static int OnBufferSize(jack_nframes_t nframes, void* arg)
{
    jackbus_node* node = static_cast<jackbus_node*>(arg);
    // JACK also calls this once on the realtime thread as it starts, with the
    // current size: not a change, and nothing is queued from that thread
    if (pthread_equal(pthread_self(), jack_client_thread_id(node->client))) {
        return 0;
    }
    jackbus_event_t ev = Event(JACKBUS_EVENT_BUFFER_SIZE);
    ev.value = nframes;
    Push(node, ev);
    return 0;
}

static int OnSampleRate(jack_nframes_t rate, void* arg)
{
    jackbus_event_t ev = Event(JACKBUS_EVENT_SAMPLE_RATE);
    ev.value = rate;
    Push(static_cast<jackbus_node*>(arg), ev);
    return 0;
}

static int OnXRun(void* arg)
{
    jackbus_node* node = static_cast<jackbus_node*>(arg);
    node->xruns.fetch_add(1, std::memory_order_relaxed);
    jackbus_event_t ev = Event(JACKBUS_EVENT_XRUN);
    ev.value = (uint64_t)jack_get_xrun_delayed_usecs(node->client);
    Push(node, ev);
    return 0;
}

static void OnFreewheel(int starting, void* arg)
{
    jackbus_node* node = static_cast<jackbus_node*>(arg);
    node->freewheel.store(starting != 0, std::memory_order_relaxed);
    jackbus_event_t ev = Event(JACKBUS_EVENT_FREEWHEEL);
    ev.on = starting ? 1 : 0;
    Push(node, ev);
}

static void OnPropertyChange(jack_uuid_t subject, const char* key, jack_property_change_t change, void* arg)
{
    jackbus_event_t ev = Event(JACKBUS_EVENT_PROPERTY);
    ev.id = subject;
    ev.value = (uint64_t)change;
    CopyName(ev.name, key);
    Push(static_cast<jackbus_node*>(arg), ev);
}

static void OnShutdown(jack_status_t code, const char* reason, void* arg)
{
    jackbus_event_t ev = Event(JACKBUS_EVENT_SHUTDOWN);
    ev.value = (uint64_t)code;
    CopyName(ev.name, reason);
    Push(static_cast<jackbus_node*>(arg), ev);
}

/*
 JACK replaces its default latency propagation with a client's latency callback,
 so the node does what the default does (each side's maximum range taken to the
 other side) and adds its declared process latency; then tells the application.
 */
static void OnLatency(jack_latency_callback_mode_t mode, void* arg)
{
    jackbus_node* node = static_cast<jackbus_node*>(arg);
    std::lock_guard<std::mutex> lock(node->write_lock);
    PortTable* t = node->table.load(std::memory_order_acquire);
    bool from_input = (mode == JackCaptureLatency);
    jack_latency_range_t range = { UINT32_MAX, 0 };

    for (size_t i = 0; t && i < t->count; i++) {
        jackbus_port* p = t->ports[i];
        if (p->input != from_input) {
            continue;
        }
        jack_latency_range_t r;
        jack_port_get_latency_range(p->jport, mode, &r);
        if (r.min < range.min) range.min = r.min;
        if (r.max > range.max) range.max = r.max;
    }
    if (range.min == UINT32_MAX) {
        range.min = 0;
    }
    uint32_t extra = node->process_latency.load(std::memory_order_relaxed);
    range.min += extra;
    range.max += extra;
    for (size_t i = 0; t && i < t->count; i++) {
        jackbus_port* p = t->ports[i];
        if (p->input != from_input) {
            jack_port_set_latency_range(p->jport, mode, &range);
        }
    }

    jackbus_event_t ev = Event(JACKBUS_EVENT_LATENCY);
    ev.value = (mode == JackCaptureLatency) ? 0 : 1;
    Push(node, ev);
}

/* ------------------------------------------------------ realtime side */

static int Process(jack_nframes_t nframes, void* arg)
{
    jackbus_node* node = static_cast<jackbus_node*>(arg);
    node->cycle_seq.fetch_add(1, std::memory_order_seq_cst);     // odd: in a cycle
    PortTable* t = node->table.load(std::memory_order_seq_cst);

    jackbus_cycle_t& c = node->cycle;
    c.nframes = nframes;
    c.flags = 0;

    // Time: the server's frame timer (microseconds, CLOCK_MONOTONIC_RAW); the
    // MONOTONIC equivalent from two vDSO clock reads (no system call)
    JackEngineControl* control = GetEngineControl();
    JackTimer timer;
    jack_nframes_t frames = 0;
    jack_time_t usecs = 0, next_usecs = 0;
    float period_usecs = 0.0f;
    control->ReadFrameTime(&timer);
    if (timer.GetCycleTimes(&frames, &usecs, &next_usecs, &period_usecs) == 0) {
        if (!node->have_frames) {
            node->frame64 = frames;
            node->have_frames = true;
        } else {
            node->frame64 += (uint32_t)(frames - node->last_frames);
        }
        node->last_frames = frames;
        struct timespec mono, raw;
        clock_gettime(CLOCK_MONOTONIC, &mono);
        clock_gettime(CLOCK_MONOTONIC_RAW, &raw);
        int64_t offset = (int64_t)TimespecNs(mono) - (int64_t)TimespecNs(raw);
        c.frame = node->frame64;
        c.raw_ns = usecs * 1000ULL;
        c.time_ns = (uint64_t)((int64_t)c.raw_ns + offset);
        c.next_ns = (uint64_t)((int64_t)(next_usecs * 1000ULL) + offset);
        c.period_ns = (uint64_t)(period_usecs * 1000.0f);
        c.rate_diff = (period_usecs > 0.0f) ? (nframes * 1e6 / control->fSampleRate) / period_usecs : 1.0;
    }
    c.sample_rate = control->fSampleRate;
    c.cycle++;
    uint64_t xruns = node->xruns.load(std::memory_order_relaxed);
    if (xruns != node->xruns_seen) {
        c.flags |= JACKBUS_CYCLE_AFTER_XRUN;
        node->xruns_seen = xruns;
    }
    c.xruns = xruns;
    if (node->first.exchange(false, std::memory_order_relaxed)) {
        c.flags |= JACKBUS_CYCLE_FIRST;
    }
    if (node->freewheel.load(std::memory_order_relaxed)) {
        c.flags |= JACKBUS_CYCLE_FREEWHEEL;
    }
    jack_position_t pos;
    c.transport_state = jack_transport_query(node->client, &pos);
    c.transport_frame = pos.frame;
    if (c.transport_state == JackTransportRolling) {
        c.flags |= JACKBUS_CYCLE_ROLLING;
    }

    // The buffers, once: what jack_port_get_buffer() would return, except that an
    // unconnected audio input gets the shared zero buffer instead of a clearing
    JackGraphManager* graph = GetGraphManager();
    const float* zero = ZeroBuffer();
    for (size_t i = 0; t && i < t->count; i++) {
        jackbus_port* p = t->ports[i];
        if (p->endpoint.load(std::memory_order_acquire)) {
            p->buffer = NULL;           // JackBus's to fill or empty, not ours
        } else if (p->kind == JACKBUS_AUDIO && p->input && zero && graph->GetConnectionsNum(p->index) == 0) {
            p->buffer = const_cast<float*>(zero);
        } else {
            p->buffer = graph->GetBuffer(p->index, nframes);
        }
    }

    int res = node->process ? node->process(node, &c, node->arg) : 0;
    node->cycle_seq.fetch_add(1, std::memory_order_seq_cst);     // even: done
    return res;
}

/*
 After a new table was published: wait until no cycle can still read the old one
 (a cycle that started before the publication has ended). Non-realtime threads
 only. Bounded: if the realtime thread is stuck in the node's process function,
 the old table is leaked rather than freed under it.
 */
static void Retire(jackbus_node* node, PortTable* old)
{
    if (!old) {
        return;
    }
    uint64_t seq = node->cycle_seq.load(std::memory_order_seq_cst);
    for (int i = 0; (seq & 1) && i < 20000; i++) {     // 2 s
        usleep(100);
        if (node->cycle_seq.load(std::memory_order_seq_cst) != seq) {
            seq = 0;
            break;
        }
    }
    if (seq & 1) {
        jack_error("jackbus: a process cycle has run for 2 s: an old port table is kept");
        return;
    }
    free(old);
}

/* ----------------------------------------------------------------- API */

extern "C"
{

LIB_EXPORT jackbus_node_t* jackbus_node_open(const char* name, jack_options_t options, jack_status_t* status, ...)
{
    jack_status_t my_status;
    if (status == NULL) {
        status = &my_status;
    }
    jackbus_node* node = new (std::nothrow) jackbus_node();
    if (!node) {
        *status = JackFailure;
        return NULL;
    }
    if (!ZeroBuffer()) {
        jack_error("jackbus_node_open: cannot map the zero buffer");
    }
    node->table.store(NewTable(0));
    node->events.resize(1024);
    node->head = node->count = 0;
    node->lost = 0;
    memset(&node->cycle, 0, sizeof(node->cycle));
    node->cycle.size = sizeof(jackbus_cycle_t);
    node->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (node->event_fd < 0 || !node->table.load()) {
        *status = JackFailure;
        goto error;
    }

    {
        va_list ap;
        va_start(ap, status);
        node->client = JackBusClientOpenV(name, options, status, ap);
        va_end(ap);
    }
    if (!node->client) {
        goto error;
    }

    // every notification becomes an event; the process wrapper is always set
    if (jack_set_process_callback(node->client, Process, node) != 0
        || jack_set_client_registration_callback(node->client, OnClientRegistration, node) != 0
        || jack_set_port_registration_callback(node->client, OnPortRegistration, node) != 0
        || jack_set_port_connect_callback(node->client, OnPortConnect, node) != 0
        || jack_set_port_rename_callback(node->client, OnPortRename, node) != 0
        || jack_set_graph_order_callback(node->client, OnGraphOrder, node) != 0
        || jack_set_buffer_size_callback(node->client, OnBufferSize, node) != 0
        || jack_set_sample_rate_callback(node->client, OnSampleRate, node) != 0
        || jack_set_xrun_callback(node->client, OnXRun, node) != 0
        || jack_set_freewheel_callback(node->client, OnFreewheel, node) != 0
        || jack_set_latency_callback(node->client, OnLatency, node) != 0
        || jack_set_property_change_callback(node->client, OnPropertyChange, node) != 0) {
        jack_error("jackbus_node_open: cannot set the node's callbacks");
        jack_client_close(node->client);
        *status = JackFailure;
        goto error;
    }
    jack_on_info_shutdown(node->client, OnShutdown, node);
    return node;

error:
    if (node->event_fd >= 0) {
        close(node->event_fd);
    }
    free(node->table.load());
    delete node;
    return NULL;
}

LIB_EXPORT int jackbus_node_close(jackbus_node_t* node)
{
    if (!node) {
        return -EINVAL;
    }
    int res = jack_client_close(node->client);     // the realtime thread is gone after this
    PortTable* t = node->table.load();
    for (size_t i = 0; t && i < t->count; i++) {
        delete t->ports[i];
    }
    free(t);
    close(node->event_fd);
    delete node;
    return res;
}

LIB_EXPORT jack_client_t* jackbus_node_client(jackbus_node_t* node)
{
    return node ? node->client : NULL;
}

LIB_EXPORT int jackbus_node_set_process(jackbus_node_t* node, jackbus_process_t process, void* arg)
{
    if (!node) {
        return -EINVAL;
    }
    if (node->active && !node->realtime && process) {
        // active without a realtime thread: it gets one, the way JACK gives one to a client
        // activated without (ActivateAux); the wrapper calls process from the first cycle on
        node->arg = arg;
        node->process = process;
        node->first.store(true, std::memory_order_relaxed);
        if (reinterpret_cast<JackClient*>(node->client)->SetProcessCallbackActive(Process, node) != 0) {
            node->process = NULL;
            return -EIO;
        }
        node->realtime = true;
        return 0;
    }
    if (node->active) {
        jack_error("jackbus_node_set_process: the node is active");
        return -EBUSY;
    }
    node->arg = arg;
    node->process = process;
    return 0;
}

LIB_EXPORT int jackbus_node_set_process_latency(jackbus_node_t* node, jack_nframes_t frames)
{
    if (!node) {
        return -EINVAL;
    }
    node->process_latency.store(frames, std::memory_order_relaxed);
    return jack_recompute_total_latencies(node->client);
}

LIB_EXPORT int jackbus_node_activate(jackbus_node_t* node)
{
    if (!node) {
        return -EINVAL;
    }
    node->first.store(true, std::memory_order_relaxed);
    // no process function: no realtime thread (JACK's client without a process callback); its
    // ports carry audio only through async endpoints
    const bool realtime = node->process != NULL;
    if (jack_set_process_callback(node->client, realtime ? Process : NULL, node) != 0) {
        return -EIO;
    }
    int res = jack_activate(node->client);
    if (res == 0) {
        node->active = true;
        node->realtime = realtime;
    }
    return res;
}

LIB_EXPORT int jackbus_node_deactivate(jackbus_node_t* node)
{
    if (!node) {
        return -EINVAL;
    }
    int res = jack_deactivate(node->client);
    if (res == 0) {
        node->active = false;
    }
    return res;
}

LIB_EXPORT jackbus_port_t* jackbus_port_add(jackbus_node_t* node, const char* name, int kind, unsigned long flags)
{
    if (!node || !name || (kind != JACKBUS_AUDIO && kind != JACKBUS_MIDI)
        || !(flags & (JackPortIsInput | JackPortIsOutput)) || ((flags & JackPortIsInput) && (flags & JackPortIsOutput))) {
        return NULL;
    }
    jackbus_port* p = new (std::nothrow) jackbus_port();
    if (!p) {
        return NULL;
    }
    p->node = node;
    p->kind = kind;
    p->input = (flags & JackPortIsInput) != 0;
    p->buffer = NULL;
    p->jport = jack_port_register(node->client, name,
                                  kind == JACKBUS_AUDIO ? JACK_DEFAULT_AUDIO_TYPE : JACK_DEFAULT_MIDI_TYPE, flags, 0);
    if (!p->jport) {
        delete p;
        return NULL;
    }
    p->index = (jack_port_id_t)(uintptr_t)p->jport;

    std::lock_guard<std::mutex> lock(node->write_lock);
    PortTable* old = node->table.load(std::memory_order_seq_cst);
    PortTable* t = NewTable(old->count + 1);
    if (!t) {
        jack_port_unregister(node->client, p->jport);
        delete p;
        return NULL;
    }
    memcpy(t->ports, old->ports, old->count * sizeof(jackbus_port*));
    t->ports[old->count] = p;
    node->table.store(t, std::memory_order_seq_cst);
    Retire(node, old);
    return p;
}

LIB_EXPORT int jackbus_port_remove(jackbus_port_t* port)
{
    if (!port) {
        return -EINVAL;
    }
    jackbus_node* node = port->node;
    {
        std::lock_guard<std::mutex> lock(node->write_lock);
        PortTable* old = node->table.load(std::memory_order_seq_cst);
        PortTable* t = NewTable(old->count ? old->count - 1 : 0);
        if (!t) {
            return -ENOMEM;
        }
        size_t n = 0;
        for (size_t i = 0; i < old->count; i++) {
            if (old->ports[i] != port) {
                t->ports[n++] = old->ports[i];
            }
        }
        t->count = n;
        node->table.store(t, std::memory_order_seq_cst);
        Retire(node, old);      // from here no cycle sees the port
    }
    int res = jack_port_unregister(node->client, port->jport);
    delete port;
    return res;
}

LIB_EXPORT jack_port_t* jackbus_port_jack(jackbus_port_t* port)
{
    return port ? port->jport : NULL;
}

LIB_EXPORT float* jackbus_port_audio(jackbus_port_t* port)
{
    return (port && port->kind == JACKBUS_AUDIO) ? static_cast<float*>(port->buffer) : NULL;
}

LIB_EXPORT void* jackbus_port_midi(jackbus_port_t* port)
{
    return (port && port->kind == JACKBUS_MIDI) ? port->buffer : NULL;
}

LIB_EXPORT int jackbus_node_set_priority(jackbus_node_t* node, int on)
{
    if (!node) {
        return -EINVAL;
    }
    if (node->active) {
        jack_error("jackbus_node_set_priority: the node is active");
        return -EBUSY;
    }
    JackBusLibClient* client = dynamic_cast<JackBusLibClient*>(reinterpret_cast<JackClient*>(node->client));
    if (!client || client->SetPriority(on != 0) != 0) {
        return -EIO;
    }
    node->priority = (on != 0);
    return 0;
}

LIB_EXPORT jackbus_endpoint_t* jackbus_endpoint_open(jackbus_node_t* node, jackbus_port_t* const* ports,
                                                     unsigned int count, unsigned int ring_frames)
{
    if (!node || !ports || count < 1 || count > JACKBUS_ENDPOINT_MAX_CHANNELS || ring_frames < 1 || ring_frames > (1u << 20)) {
        errno = EINVAL;
        return NULL;
    }
    JackBusLibClient* client = dynamic_cast<JackBusLibClient*>(reinterpret_cast<JackClient*>(node->client));
    if (!client) {
        errno = ENOTSUP;
        return NULL;
    }
    jack_port_id_t ids[JACKBUS_ENDPOINT_MAX_CHANNELS];
    for (unsigned int i = 0; i < count; i++) {
        if (!ports[i] || ports[i]->node != node || ports[i]->kind != JACKBUS_AUDIO
            || ports[i]->input != ports[0]->input || ports[i]->endpoint.load()) {
            errno = EINVAL;
            return NULL;
        }
        ids[i] = ports[i]->index;
    }
    jackbus_endpoint* e = new (std::nothrow) jackbus_endpoint();
    if (!e) {
        errno = ENOMEM;
        return NULL;
    }
    int id = -1, fd = -1;
    int res = client->EndpointOpen((int)count, ids, (int)ring_frames, &id, &fd);
    if (res != 0 || fd < 0) {
        delete e;
        errno = (res < 0 && res > -4096) ? -res : EIO;
        return NULL;
    }
    struct stat st;
    void* mem = MAP_FAILED;
    if (fstat(fd, &st) == 0 && (size_t)st.st_size >= sizeof(jackbus_endpoint_shared_t)) {
        mem = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
    }
    close(fd);
    jackbus_endpoint_shared_t* sh = static_cast<jackbus_endpoint_shared_t*>(mem);
    if (mem == MAP_FAILED || sh->size != sizeof(jackbus_endpoint_shared_t) || sh->channels != count
        || sh->ring_offset + (uint64_t)count * sh->ring_frames * sizeof(float) > (uint64_t)st.st_size) {
        if (mem != MAP_FAILED) {
            munmap(mem, (size_t)st.st_size);
        }
        client->EndpointClose(id);
        delete e;
        errno = EPROTO;
        return NULL;
    }
    mlock(mem, (size_t)st.st_size);     // best effort: the application's side touches it
    e->node = node;
    e->id = id;
    e->shared = sh;
    e->size = (size_t)st.st_size;
    e->count = count;
    for (unsigned int i = 0; i < count; i++) {
        e->ports[i] = ports[i];
        ports[i]->endpoint.store(true, std::memory_order_release);
    }
    return e;
}

LIB_EXPORT int jackbus_endpoint_close(jackbus_endpoint_t* e)
{
    if (!e) {
        return -EINVAL;
    }
    JackBusLibClient* client = dynamic_cast<JackBusLibClient*>(reinterpret_cast<JackClient*>(e->node->client));
    int res = client ? client->EndpointClose(e->id) : -EIO;
    // once JackBus no longer serves it (the reply), the ports are the node's again
    for (unsigned int i = 0; i < e->count; i++) {
        e->ports[i]->endpoint.store(false, std::memory_order_release);
    }
    munmap(e->shared, e->size);
    delete e;
    return res;
}

LIB_EXPORT jackbus_endpoint_shared_t* jackbus_endpoint_shared(jackbus_endpoint_t* e)
{
    return e ? e->shared : NULL;
}

LIB_EXPORT float* jackbus_endpoint_ring(jackbus_endpoint_t* e, unsigned int channel)
{
    if (!e || channel >= e->shared->channels) {
        return NULL;
    }
    return reinterpret_cast<float*>(reinterpret_cast<char*>(e->shared) + e->shared->ring_offset)
        + (size_t)channel * e->shared->ring_frames;
}

LIB_EXPORT int jackbus_node_event_fd(jackbus_node_t* node)
{
    return node ? node->event_fd : -EINVAL;
}

LIB_EXPORT int jackbus_node_next_event(jackbus_node_t* node, jackbus_event_t* event)
{
    if (!node || !event) {
        return -EINVAL;
    }
    for (int pass = 0; pass < 2; pass++) {
        {
            std::lock_guard<std::mutex> lock(node->event_lock);
            if (node->count > 0) {
                *event = node->events[node->head];
                node->head = (node->head + 1) % node->events.size();
                node->count--;
                return 1;
            }
            if (node->lost > 0) {
                *event = Event(JACKBUS_EVENT_LOST);
                event->value = node->lost;
                node->lost = 0;
                return 1;
            }
        }
        // empty: reset the fd, then look once more (an event appended before the
        // reset is found now; one appended after it makes the fd readable again)
        uint64_t n;
        ssize_t r = read(node->event_fd, &n, sizeof(n));
        (void)r;
    }
    return 0;
}

} // extern "C"
