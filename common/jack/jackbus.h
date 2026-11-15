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

#ifdef __cplusplus
}
#endif

#endif /* __jackbus_h__ */
