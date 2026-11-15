/*
Copyright (C) 2026 jack2-nspa

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
 jack2-nspa additions to the server control API (libjackserver, common/JackControlAPI.cpp),
 for jackdbus. Not part of libjack: applications are not concerned.
 */

#ifndef __JackControlNspa__
#define __JackControlNspa__

#include <stdint.h>
#include <stdbool.h>
#include "JackConstants.h"

/* jack/control.h (C) and JackControlAPI.h (libjackserver's C++ copy) both name the
   server jackctl_server_t: struct jackctl_server here suits either */
struct jackctl_server;
struct jackctl_driver;

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define JACK_NSPA_CONTROL_EXPORT __attribute__((visibility("default")))
#else
#define JACK_NSPA_CONTROL_EXPORT
#endif

/* Take the client client_name, or (client_name NULL) every client of process pid, out of the
   graph before a session manager stops its process (JackEngine::DetachClients). Returns how
   many were taken out, -1 for an unknown client name or without a server. */
JACK_NSPA_CONTROL_EXPORT int
jackctl_server_detach_clients(struct jackctl_server * server, const char * client_name, int pid);

/* How a client in the graph keeps up (JackEngine::UpdateClientCycles), since it was
   activated or the last reset. A client still waiting for one of its inputs at the
   start of a cycle is not late itself: the one it waits for is. */
typedef struct {
    char name[JACK_CLIENT_NAME_SIZE + 1];
    uint32_t late_cycles;           /* cycles it had not finished when the next one began */
    uint32_t max_callback_usecs;    /* its longest cycle, woken to finished: measured only
                                       while timings are asked for (in the last minute),
                                       which costs the server some work every cycle */
} jackctl_client_timing_t;

/* Fills timings (up to max) for the clients in the graph: active external clients with a
   process callback (internal clients and drivers are not counted). Returns how many, -1
   without a server. Measures the clients' cycle lengths for the next minute. */
JACK_NSPA_CONTROL_EXPORT int
jackctl_server_get_client_timings(struct jackctl_server * server, jackctl_client_timing_t * timings, int max);

/* Starts every client's counts afresh (from the next cycle). */
JACK_NSPA_CONTROL_EXPORT bool
jackctl_server_reset_client_timings(struct jackctl_server * server);

/* Build the driver's descriptor again from its module and take the value lists (enumeration
   and range constraints) that changed: the ALSA driver's device list after a card was plugged
   in or removed. Parameter values are untouched. Returns how many parameters changed, -1 if
   the module could not be asked. Call it from the thread that uses the control API (jackdbus's
   main loop): a list taken from a parameter before the call must not be used after it. */
JACK_NSPA_CONTROL_EXPORT int
jackctl_driver_refresh_constraints(struct jackctl_driver * driver);

/* Called once when the running master driver loses its device (a USB interface unplugged),
   from the driver thread as it ends: set a flag and return, nothing else (no locks, no
   control API calls). With a callback set, clients are not told the server stopped: the
   controller is expected to switch the master driver (jackctl_server_switch_master) to keep
   the server running. NULL restores the default (clients get a shutdown). */
JACK_NSPA_CONTROL_EXPORT void
jackctl_server_set_backend_lost_callback(struct jackctl_server * server, void (*callback)(void * arg), void * arg);

/*
 * Sound card bridges: a card on its own clock, joined to the master's graph with adaptive
 * resampling (the "bridge" driver). Added and removed while the server runs (after start).
 * add: `driver` is the bridge driver (jackctl_server_get_drivers_list, named "bridge") with its
 * parameters set (device, and optionally name, mode, period, nperiods, wake, quality): each call
 * adds a bridge with the parameters as they are then. remove: by the bridge's client name (the
 * card's ALSA id unless "name" was set). get: the bridges' client names; returns how many.
 */
JACK_NSPA_CONTROL_EXPORT bool
jackctl_server_add_bridge(struct jackctl_server * server, struct jackctl_driver * driver);

JACK_NSPA_CONTROL_EXPORT bool
jackctl_server_remove_bridge(struct jackctl_server * server, const char * name);

JACK_NSPA_CONTROL_EXPORT int
jackctl_server_get_bridges(struct jackctl_server * server, char names[][64 + 1], int max);

#ifdef __cplusplus
}
#endif

#endif
