/* -*- Mode: C ; c-basic-offset: 4 -*- */
/*
    Copyright (C) 2007,2008,2011 Nedko Arnaudov
    Copyright (C) 2007-2008 Juuso Alasuutari
    
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#ifndef CONTROLLER_INTERNAL_H__04D54D51_3D79_49A2_A1DA_F8587E9E7F42__INCLUDED
#define CONTROLLER_INTERNAL_H__04D54D51_3D79_49A2_A1DA_F8587E9E7F42__INCLUDED

#include <stdbool.h>
#include "jslist.h"
#include "jack/control.h"
#include "JackControlNspa.h"      /* jack2-nspa additions: detach clients, client timings */
#include "jack/jack.h"
#include "jack/session.h"
#include "jackdbus.h"
#include "list.h"
#include "params.h"

struct jack_controller_slave_driver
{
    struct list_head siblings;
    char * name;
    jackctl_driver_t * handle;
    bool loaded;
};

struct jack_session_pending_command
{
    struct list_head siblings;
    DBusConnection * connection;
    DBusMessage * message;
    jack_session_event_type_t type;
    const char * target;
    const char * path;
};

#define JACK_CONTROLLER_BRIDGES_MAX 16

struct jack_controller
{
    jackctl_server_t *server;
    jack_params_handle params;

    void *patchbay_context;

    bool started;
    jack_client_t *client;
    unsigned int xruns;

    struct list_head slave_drivers;
    bool slave_drivers_set;
    struct jack_parameter slave_drivers_vparam;
    union jackctl_parameter_value slave_drivers_vparam_value;

    struct jack_dbus_object_descriptor dbus_descriptor;

    pthread_mutex_t lock;
    struct list_head session_pending_commands;

    long pending_save;          /* uptime seconds */

    /* jack2-nspa: sound cards coming and going (inotify on /dev/snd), so the ALSA driver's
       device list follows them without restarting jackdbus */
    int snd_watch_fd;           /* -1: no inotify */
    int snd_watch_wd;           /* -1: /dev/snd not watched (yet, or any more) */
    int64_t snd_first_change;   /* ms (monotonic) of the first unhandled change, 0: none */
    int64_t snd_last_change;    /* ms of the latest one */
    int64_t snd_last_rewatch;   /* ms of the last attempt to watch /dev/snd */

    /* jack2-nspa: the master driver's device went away (set from the dying driver thread) */
    int backend_lost;
    /* while the dummy driver stands in for it: the driver to go back to, when to try next */
    jackctl_driver_t * fallback_from;
    int64_t fallback_next_try;  /* ms */
    int fallback_failures;

    /* jack2-nspa: sound card bridges. The "bridges" engine parameter (kept with the settings)
       lists them; these are the ones running now (spec as listed, the bridge's client name). */
    struct jack_parameter bridges_vparam;
    union jackctl_parameter_value bridges_vparam_value;
    bool bridges_set;
    int bridges_running;
    char bridge_spec[JACK_CONTROLLER_BRIDGES_MAX][JACK_PARAM_STRING_MAX + 1];
    char bridge_name[JACK_CONTROLLER_BRIDGES_MAX][64 + 1];
};

#define DEFAULT_DRIVER "dummy"

/* jack2-nspa: sound card bridges (controller.c) */
void
jack_controller_bridges_apply(
    struct jack_controller * controller_ptr);

bool
jack_controller_bridges_add(
    struct jack_controller * controller_ptr,
    const char * spec,
    char name[64 + 1],
    char * error,
    size_t error_size);

bool
jack_controller_bridges_remove(
    struct jack_controller * controller_ptr,
    const char * what,
    char * error,
    size_t error_size);

const char *
jack_controller_bridges_running_name(
    struct jack_controller * controller_ptr,
    const char * spec);

#define JACK_CONF_HEADER_TEXT                       \
    "JACK settings, as persisted by D-Bus object.\n"        \
    "You probably don't want to edit this because\n"        \
    "it will be overwritten next time jackdbus saves.\n"

void
jack_controller_pending_save(
    struct jack_controller *controller_ptr);

bool
jack_controller_start_server(
    struct jack_controller *controller_ptr,
    void *dbus_call_context_ptr);

bool
jack_controller_stop_server(
    struct jack_controller *controller_ptr,
    void *dbus_call_context_ptr);

bool
jack_controller_switch_master(
    struct jack_controller *controller_ptr,
    void *dbus_call_context_ptr);
    
bool
jack_controller_add_slave_driver(
    struct jack_controller *controller_ptr,
    const char * driver_name);
    
bool
jack_controller_remove_slave_driver(
    struct jack_controller *controller_ptr,
    const char * driver_name);

bool
jack_controller_select_driver(
    struct jack_controller *controller_ptr,
    const char * driver_name);

bool
jack_controller_load_internal(
    struct jack_controller *controller_ptr,
    const char * internal_name);

bool
jack_controller_unload_internal(
    struct jack_controller *controller_ptr,
    const char * internal_name);

void
jack_controller_deserialize_parameter_value(
    struct jack_controller *controller_ptr,
    const char * const * address,
    const char * value);

void
jack_controller_serialize_parameter_value(
    const struct jack_parameter * param_ptr,
    char * value_buffer);

bool
jack_controller_patchbay_init(
    struct jack_controller *controller_ptr);

void
jack_controller_patchbay_uninit(
    struct jack_controller *controller_ptr);

void
jack_controller_patchbay_free(
    struct jack_controller *controller_ptr);

void *
jack_controller_patchbay_client_appeared_callback(
    void * server_context,
    uint64_t client_id,
    const char *client_name);

void
jack_controller_patchbay_client_disappeared_callback(
    void *server_context,
    uint64_t client_id,
    void *client_context);

void *
jack_controller_patchbay_port_appeared_callback(
    void *server_context,
    uint64_t client_id,
    void *client_context,
    uint64_t port_id,
    const char *port_name,
    uint32_t port_flags,
    uint32_t port_type);

void
jack_controller_patchbay_port_disappeared_callback(
    void *server_context,
    uint64_t client_id,
    void *client_context,
    uint64_t port_id,
    void *port_context);

void *
jack_controller_patchbay_ports_connected_callback(
    void *server_context,
    uint64_t client1_id,
    void *client1_context,
    uint64_t port1_id,
    void *port1_context,
    uint64_t client2_id,
    void *client2_context,
    uint64_t port2_id,
    void *port2_context,
    uint64_t connection_id);

void
jack_controller_patchbay_ports_disconnected_callback(
    void *server_context,
    uint64_t client1_id,
    void *client1_context,
    uint64_t port1_id,
    void *port1_context,
    uint64_t client2_id,
    void *client2_context,
    uint64_t port2_id,
    void *port2_context,
    uint64_t connection_id,
    void *connection_context);

extern struct jack_dbus_interface_descriptor g_jack_controller_iface_introspectable;
extern struct jack_dbus_interface_descriptor g_jack_controller_iface_control;
extern struct jack_dbus_interface_descriptor g_jack_controller_iface_configure;
extern struct jack_dbus_interface_descriptor g_jack_controller_iface_patchbay;
extern struct jack_dbus_interface_descriptor g_jack_controller_iface_session_manager;
extern struct jack_dbus_interface_descriptor g_jack_controller_iface_transport;

/* jack2-nspa: org.jackaudio.Configure.ConstraintsChanged(as container): the value lists of
   the parameters under this container changed (ALSA devices after a card came or went) */
void
jack_controller_configure_send_signal_constraints_changed(const char * const * address, int count);

#endif /* #ifndef CONTROLLER_INTERNAL_H__04D54D51_3D79_49A2_A1DA_F8587E9E7F42__INCLUDED */
