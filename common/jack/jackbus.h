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

#ifndef __jackbus_h__
#define __jackbus_h__

#include <stdint.h>
#include <jack/jack.h>
#include <jack/transport.h>

/* the endpoints' shared memory (below): atomics both C and C++ lay out alike */
#ifdef __cplusplus
#include <atomic>
#define JACKBUS_ATOMIC(type) std::atomic<type>
#else
#include <stdatomic.h>
#define JACKBUS_ATOMIC(type) _Atomic type
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * Open a client through the JackBus socket ($XDG_RUNTIME_DIR/jackbus/<server>).
 *
 * The arguments and the result are those of jack_client_open(), plus flags
 * (reserved: pass 0). The client gets the server's shared objects as file
 * descriptors: it needs no /dev/shm names and no shm registry. It works with
 * every jack_* function, and runs in the same graph as protocol-9 clients.
 *
 * Fails (NULL, JackFailure in *status) when the server has no JackBus socket:
 * a stock JACK server, or no XDG_RUNTIME_DIR.
 */
jack_client_t * jackbus_client_open (const char *client_name,
                                     jack_options_t options,
                                     jack_status_t *status,
                                     unsigned int flags, ...) JACK_OPTIONAL_WEAK_EXPORT;

/* ------------------------------------------------------------------------
 * The native API: nodes. EXPERIMENTAL, it may still change.
 *
 * A node is a client of the JackBus socket (above) with what JACK's API makes
 * applications work around:
 * - its process callback gets a cycle record: a 64-bit frame position, the
 *   cycle's times in nanoseconds on both clocks, the measured period, the
 *   rate difference, xruns, transport;
 * - its ports' buffers are resolved once per cycle, before the callback (an
 *   unconnected audio input is a shared zero buffer, never cleared again);
 * - notifications are events in a queue the application drains from any of
 *   its threads, with a file descriptor to poll, instead of callbacks on a
 *   library thread that must not call the server;
 * - its latency is data: a process latency it declares is added to the
 *   ranges JACK computes for its ports.
 * jackbus_node_client() gives the same client to the jack_* API for the rest
 * (connections, metadata, transport): set no callbacks on it.
 * ------------------------------------------------------------------------ */

typedef struct jackbus_node jackbus_node_t;
typedef struct jackbus_port jackbus_port_t;

/* One cycle as the node's process callback sees it. time_ns and next_ns are
   CLOCK_MONOTONIC, raw_ns CLOCK_MONOTONIC_RAW (JACK's own clock); the server
   publishes them in microsecond steps for now.
   frame is JACK's frame time (jack_last_frame_time()) in 64 bits: it advances
   by nframes each cycle, jumps at an xrun (JACKBUS_CYCLE_AFTER_XRUN), and at a
   buffer-size change advances by the new size once. While freewheeling, the
   times are those of the fast cycles: only frame and cycle mean anything. */
typedef struct jackbus_cycle {
    uint32_t size;              /* sizeof(jackbus_cycle_t) in the library */
    uint32_t nframes;           /* frames in this cycle */
    uint32_t sample_rate;
    uint32_t flags;             /* JACKBUS_CYCLE_* */
    uint64_t frame;             /* position of the cycle's first frame (64 bits: no wrap) */
    uint64_t time_ns;           /* the cycle's start (the driver's wakeup) */
    uint64_t raw_ns;            /* the same on CLOCK_MONOTONIC_RAW */
    uint64_t next_ns;           /* when the next cycle is due */
    uint64_t period_ns;         /* the measured period (the server's DLL) */
    double rate_diff;           /* nominal period / measured period */
    uint64_t cycle;             /* cycles this node has run, this one included */
    uint64_t xruns;             /* xruns the node has been told of */
    jack_transport_state_t transport_state;
    jack_nframes_t transport_frame;
} jackbus_cycle_t;

#define JACKBUS_CYCLE_FIRST         (1u << 0)   /* the first cycle since activation */
#define JACKBUS_CYCLE_AFTER_XRUN    (1u << 1)   /* the first cycle after an xrun */
#define JACKBUS_CYCLE_FREEWHEEL     (1u << 2)   /* freewheeling: as fast as possible */
#define JACKBUS_CYCLE_ROLLING       (1u << 3)   /* transport rolling */

/* Called in the node's realtime thread. Returning non-zero takes the node out
   of the graph, as with jack_set_process_callback(). */
typedef int (*jackbus_process_t)(jackbus_node_t *node, const jackbus_cycle_t *cycle, void *arg);

/* Open a node over the JackBus socket (varargs as jack_client_open(): the
   server name with JackServerName). NULL if the server has no JackBus socket. */
jackbus_node_t * jackbus_node_open (const char *name, jack_options_t options,
                                    jack_status_t *status, ...) JACK_OPTIONAL_WEAK_EXPORT;
int jackbus_node_close (jackbus_node_t *node) JACK_OPTIONAL_WEAK_EXPORT;
jack_client_t * jackbus_node_client (jackbus_node_t *node) JACK_OPTIONAL_WEAK_EXPORT;

/* Before activation */
int jackbus_node_set_process (jackbus_node_t *node, jackbus_process_t process, void *arg) JACK_OPTIONAL_WEAK_EXPORT;
/* Frames between the node's inputs and outputs, added to its latency ranges
   (any time; 0 at first) */
int jackbus_node_set_process_latency (jackbus_node_t *node, jack_nframes_t frames) JACK_OPTIONAL_WEAK_EXPORT;

int jackbus_node_activate (jackbus_node_t *node) JACK_OPTIONAL_WEAK_EXPORT;
int jackbus_node_deactivate (jackbus_node_t *node) JACK_OPTIONAL_WEAK_EXPORT;

/* Ports, any time (also while the node runs: the realtime thread never waits
   for it, and never misses a cycle) */
#define JACKBUS_AUDIO 0
#define JACKBUS_MIDI  1
/* flags: JackPortIsInput or JackPortIsOutput, and JackPortIsTerminal etc. */
jackbus_port_t * jackbus_port_add (jackbus_node_t *node, const char *name, int kind, unsigned long flags) JACK_OPTIONAL_WEAK_EXPORT;
int jackbus_port_remove (jackbus_port_t *port) JACK_OPTIONAL_WEAK_EXPORT;
jack_port_t * jackbus_port_jack (jackbus_port_t *port) JACK_OPTIONAL_WEAK_EXPORT;

/* In the process callback only. No system call, no lock. An audio input's
   buffer is read-only: an unconnected one is a zero buffer shared by all, and
   mapped read-only (writing it is a crash, not a silent corruption). */
float * jackbus_port_audio (jackbus_port_t *port) JACK_OPTIONAL_WEAK_EXPORT;
/* A JACK MIDI buffer, for the jack_midi_* functions */
void * jackbus_port_midi (jackbus_port_t *port) JACK_OPTIONAL_WEAK_EXPORT;

/* Events */
typedef enum {
    JACKBUS_EVENT_CLIENT = 1,       /* on: registered or gone; name */
    JACKBUS_EVENT_PORT,             /* on; id: the port's uuid; value: its jack_port_id_t; name (if known) */
    JACKBUS_EVENT_CONNECT,          /* on: connected or disconnected; name, name2: the ports */
    JACKBUS_EVENT_RENAME,           /* value: jack_port_id_t; name: new; name2: old */
    JACKBUS_EVENT_GRAPH,            /* the processing order changed */
    JACKBUS_EVENT_BUFFER_SIZE,      /* value: frames */
    JACKBUS_EVENT_SAMPLE_RATE,      /* value: Hz */
    JACKBUS_EVENT_XRUN,             /* value: how late, in microseconds */
    JACKBUS_EVENT_FREEWHEEL,        /* on: started or stopped */
    JACKBUS_EVENT_LATENCY,          /* value: 0 capture, 1 playback; the ranges were updated */
    JACKBUS_EVENT_PROPERTY,         /* id: the subject; value: jack_property_change_t; name: the key */
    JACKBUS_EVENT_SHUTDOWN,         /* the server went away; name: why */
    JACKBUS_EVENT_LOST              /* value: events lost (queue full): read the graph again */
} jackbus_event_type_t;

#define JACKBUS_NAME_SIZE 384       /* a full port name, and its terminating 0 */

typedef struct jackbus_event {
    uint32_t type;                  /* jackbus_event_type_t */
    uint32_t on;
    uint64_t value;
    uint64_t id;
    char name[JACKBUS_NAME_SIZE];
    char name2[JACKBUS_NAME_SIZE];
} jackbus_event_t;

/* Readable (poll) while events are queued. Owned by the node: don't close. */
int jackbus_node_event_fd (jackbus_node_t *node) JACK_OPTIONAL_WEAK_EXPORT;
/* From any thread but the process callback. 1: an event in *event; 0: none. */
int jackbus_node_next_event (jackbus_node_t *node, jackbus_event_t *event) JACK_OPTIONAL_WEAK_EXPORT;

/* ------------------------------------------------------------------------
 * Priority. EXPERIMENTAL.
 *
 * A node with priority starts first: at each cycle's start the drivers wake
 * it before other clients, and its realtime thread runs one step above
 * theirs (still below the server's own threads). Neither changes what JACK
 * computes or in which order data flows. Before activation.
 * ------------------------------------------------------------------------ */
int jackbus_node_set_priority (jackbus_node_t *node, int on) JACK_OPTIONAL_WEAK_EXPORT;

/* ------------------------------------------------------------------------
 * Async endpoints. EXPERIMENTAL.
 *
 * An endpoint is a group of a node's audio ports, all outputs or all
 * inputs, whose buffers JackBus itself fills (outputs) or empties (inputs)
 * once per cycle, from or into a ring the application shares with it. The
 * application's side is never part of the graph's cycle: nothing waits for
 * it, and if it falls behind, only its own stream runs short (silence, or
 * frames dropped), counted here. The price: what a graph client sends to an
 * endpoint arrives a cycle later than it would at a client's port.
 *
 * Connections to and from an endpoint's ports carry audio as any others do
 * but never make a client wait for another.
 *
 * A node with no process function (jackbus_node_set_process() not called,
 * or called with NULL) activates without a realtime thread: with only
 * endpoints, it is outside the cycle entirely. Setting a process function on
 * such a node while it is active gives it one (JACK's own path for a client
 * activated without a realtime thread).
 *
 * Ports: added with jackbus_port_add(), not connected yet; the endpoint is
 * opened before they are, and closed before they are removed. A process
 * function, if the node has one, sees no buffer for them (NULL).
 * ------------------------------------------------------------------------ */

#define JACKBUS_ENDPOINT_MAX_CHANNELS 64

/* The memory an endpoint shares with JackBus. Positions are frames since the
 * endpoint was opened (64 bits: they never wrap); channel c's ring is
 * ring_frames floats at (char *)shared + ring_offset + c * ring_frames *
 * sizeof(float) (jackbus_endpoint_ring()), at JackBus's rate. Fields marked
 * "app" are the application's to write, the others JackBus's. JackBus never
 * trusts the application's: a position beyond the ring is clamped. */
typedef struct jackbus_endpoint_shared {
    uint32_t size;                  /* sizeof(jackbus_endpoint_shared_t) */
    uint32_t channels;
    uint32_t ring_frames;           /* per channel, a power of two */
    uint32_t playback;              /* 1: the application's audio to the graph (output ports) */
    uint64_t ring_offset;
    uint8_t pad0[40];

    /* playback: the application's (app) / capture: JackBus's */
    JACKBUS_ATOMIC(uint64_t) w;
    uint8_t pad1[56];
    /* playback: JackBus's / capture: the application's (app) */
    JACKBUS_ATOMIC(uint64_t) r;
    uint8_t pad2[56];

    /* app: read by JackBus each cycle */
    JACKBUS_ATOMIC(uint32_t) run;       /* 0: paused: nothing moves; playback ports silent */
    JACKBUS_ATOMIC(uint32_t) prebuf;    /* playback: frames queued before it starts, and
                                           after an underflow restarts (0: at once) */
    JACKBUS_ATOMIC(uint32_t) draining;  /* playback: running out is the end, not an underflow */
    uint32_t pad3;
    JACKBUS_ATOMIC(uint64_t) skip_to;   /* playback: what is queued before it is dropped */
    uint8_t pad4[40];
    JACKBUS_ATOMIC(float) gain[JACKBUS_ENDPOINT_MAX_CHANNELS];  /* per channel, 1 at first */

    /* JackBus's */
    JACKBUS_ATOMIC(uint32_t) started;   /* playback: prebuffering done */
    JACKBUS_ATOMIC(uint32_t) underflows;
    JACKBUS_ATOMIC(uint32_t) overflows;
    uint32_t pad5;
    JACKBUS_ATOMIC(uint64_t) moved;     /* frames moved in all */
    /* the last cycle, a seqlock: seq is odd while it is written */
    JACKBUS_ATOMIC(uint32_t) seq;
    JACKBUS_ATOMIC(uint32_t) last_frames;   /* frames moved in it */
    JACKBUS_ATOMIC(uint64_t) last_moved;    /* moved before it */
    JACKBUS_ATOMIC(uint64_t) time_ns;       /* its start (CLOCK_MONOTONIC) */
    JACKBUS_ATOMIC(uint64_t) raw_ns;        /* the same on CLOCK_MONOTONIC_RAW */
    JACKBUS_ATOMIC(uint64_t) next_ns;       /* the next cycle's (CLOCK_MONOTONIC) */
    JACKBUS_ATOMIC(uint64_t) period_ns;     /* the measured period */
    JACKBUS_ATOMIC(uint64_t) frame;         /* its first frame (64 bits) */
    JACKBUS_ATOMIC(uint32_t) nframes;
    JACKBUS_ATOMIC(uint32_t) sample_rate;
} jackbus_endpoint_shared_t;

typedef struct jackbus_endpoint jackbus_endpoint_t;

/* count ports of the node (1 to JACKBUS_ENDPOINT_MAX_CHANNELS, audio, all
 * outputs or all inputs, unconnected); ring_frames: rounded up to a power of
 * two (at most 1 << 20). NULL on failure (errno). */
jackbus_endpoint_t * jackbus_endpoint_open (jackbus_node_t *node, jackbus_port_t *const *ports,
                                            unsigned int count, unsigned int ring_frames) JACK_OPTIONAL_WEAK_EXPORT;
/* JackBus stops using it, then it is unmapped */
int jackbus_endpoint_close (jackbus_endpoint_t *endpoint) JACK_OPTIONAL_WEAK_EXPORT;
jackbus_endpoint_shared_t * jackbus_endpoint_shared (jackbus_endpoint_t *endpoint) JACK_OPTIONAL_WEAK_EXPORT;
float * jackbus_endpoint_ring (jackbus_endpoint_t *endpoint, unsigned int channel) JACK_OPTIONAL_WEAK_EXPORT;

#ifdef __cplusplus
}
#endif

#endif /* __jackbus_h__ */
