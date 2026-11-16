/* -*- Mode: C ; c-basic-offset: 4 -*- */
/*
    Copyright (C) 2007,2008,2010,2011 Nedko Arnaudov
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

#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <dbus/dbus.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <time.h>
#include <sys/inotify.h>

#include "controller.h"
#include "controller_internal.h"
#include "xml.h"
#include "reserve.h"
#include <pthread.h>
#include "uptime.h"

struct jack_dbus_interface_descriptor * g_jackcontroller_interfaces[] =
{
    &g_jack_controller_iface_introspectable,
    &g_jack_controller_iface_control,
    &g_jack_controller_iface_configure,
    &g_jack_controller_iface_patchbay,
    &g_jack_controller_iface_session_manager,
    &g_jack_controller_iface_transport,
    NULL
};

static
jackctl_driver_t *
jack_controller_find_driver(
    jackctl_server_t *server,
    const char *driver_name)
{
    const JSList * node_ptr;

    node_ptr = jackctl_server_get_drivers_list(server);

    while (node_ptr)
    {
        if (strcmp(jackctl_driver_get_name((jackctl_driver_t *)node_ptr->data), driver_name) == 0)
        {
            return node_ptr->data;
        }

        node_ptr = jack_slist_next(node_ptr);
    }

    return NULL;
}

static bool jack_controller_check_slave_driver(struct jack_controller * controller_ptr, const char * name)
{
    struct list_head * node_ptr;
    struct jack_controller_slave_driver * driver_ptr;

    list_for_each(node_ptr, &controller_ptr->slave_drivers)
    {
        driver_ptr = list_entry(node_ptr, struct jack_controller_slave_driver, siblings);
        if (strcmp(name, driver_ptr->name) == 0)
        {
            return true;
        }
    }

    return false;
}

static bool jack_controller_load_slave_drivers(struct jack_controller * controller_ptr)
{
    struct list_head * node_ptr;
    struct jack_controller_slave_driver * driver_ptr;

    list_for_each(node_ptr, &controller_ptr->slave_drivers)
    {
        driver_ptr = list_entry(node_ptr, struct jack_controller_slave_driver, siblings);

        assert(driver_ptr->handle != NULL);
        assert(!driver_ptr->loaded);

        if (!jackctl_server_add_slave(controller_ptr->server, driver_ptr->handle))
        {
            jack_error("Driver \"%s\" cannot be loaded", driver_ptr->name);
            return false;
        }

        driver_ptr->loaded = true;
    }

    return true;
}

static void jack_controller_unload_slave_drivers(struct jack_controller * controller_ptr)
{
    struct list_head * node_ptr;
    struct jack_controller_slave_driver * driver_ptr;

    list_for_each(node_ptr, &controller_ptr->slave_drivers)
    {
        driver_ptr = list_entry(node_ptr, struct jack_controller_slave_driver, siblings);
        if (driver_ptr->loaded)
        {
            jackctl_server_remove_slave(controller_ptr->server, driver_ptr->handle);
            driver_ptr->loaded = false;
        }
    }
}

static void jack_controller_remove_slave_drivers(struct jack_controller * controller_ptr)
{
    struct jack_controller_slave_driver * driver_ptr;

    while (!list_empty(&controller_ptr->slave_drivers))
    {
        driver_ptr = list_entry(controller_ptr->slave_drivers.next, struct jack_controller_slave_driver, siblings);
        assert(!driver_ptr->loaded);
        list_del(&driver_ptr->siblings);
        free(driver_ptr->name);
        free(driver_ptr);
    }

    controller_ptr->slave_drivers_vparam_value.str[0] = 0;
}

static
jackctl_internal_t *
jack_controller_find_internal(
    jackctl_server_t *server,
    const char *internal_name)
{
    const JSList * node_ptr;

    node_ptr = jackctl_server_get_internals_list(server);

    while (node_ptr)
    {
        if (strcmp(jackctl_internal_get_name((jackctl_internal_t *)node_ptr->data), internal_name) == 0)
        {
            return node_ptr->data;
        }

        node_ptr = jack_slist_next(node_ptr);
    }

    return NULL;
}

bool
jack_controller_select_driver(
    struct jack_controller * controller_ptr,
    const char * driver_name)
{
    if (!jack_params_set_driver(controller_ptr->params, driver_name))
    {
        return false;
    }

    jack_info("driver \"%s\" selected", driver_name);

    return true;
}

static
int
jack_controller_xrun(void * arg)
{
	((struct jack_controller *)arg)->xruns++;

	return 0;
}

bool
jack_controller_start_server(
    struct jack_controller * controller_ptr,
    void *dbus_call_context_ptr)
{
    int ret;

    jack_info("Starting jack server...");

    assert(!controller_ptr->started); /* should be ensured by caller */

    controller_ptr->xruns = 0;

    if (!jackctl_server_open(
            controller_ptr->server,
            jack_params_get_driver(controller_ptr->params)))
    {
        jack_dbus_error(dbus_call_context_ptr, JACK_DBUS_ERROR_GENERIC, "Failed to open server");
        goto fail;
    }

    jack_controller_load_slave_drivers(controller_ptr);

    if (!jackctl_server_start(
            controller_ptr->server))
    {
        jack_dbus_error(dbus_call_context_ptr, JACK_DBUS_ERROR_GENERIC, "Failed to start server");
        goto fail_close_server;
    }

    controller_ptr->client = jack_client_open(
        "dbusapi",
        JackNoStartServer,
        NULL);
    if (controller_ptr->client == NULL)
    {
        jack_dbus_error(dbus_call_context_ptr, JACK_DBUS_ERROR_GENERIC, "failed to create dbusapi jack client");
        goto fail_stop_server;
    }

    ret = jack_set_xrun_callback(controller_ptr->client, jack_controller_xrun, controller_ptr);
    if (ret != 0)
    {
        jack_dbus_error(dbus_call_context_ptr, JACK_DBUS_ERROR_GENERIC, "failed to set xrun callback. error is %d", ret);
        goto fail_close_client;
    }

    if (!jack_controller_patchbay_init(controller_ptr))
    {
        jack_dbus_error(dbus_call_context_ptr, JACK_DBUS_ERROR_GENERIC, "Failed to initialize patchbay district");
        goto fail_close_client;
    }

    ret = jack_activate(controller_ptr->client);
    if (ret != 0)
    {
        jack_dbus_error(dbus_call_context_ptr, JACK_DBUS_ERROR_GENERIC, "failed to activate dbusapi jack client. error is %d", ret);
        goto fail_patchbay_uninit;
    }

    controller_ptr->started = true;

    /* JackBus: the sound card bridges listed */
    controller_ptr->bridges_running = 0;
    jack_controller_bridges_apply(controller_ptr);

    return TRUE;

fail_patchbay_uninit:
    jack_controller_patchbay_uninit(controller_ptr);

fail_close_client:
    ret = jack_client_close(controller_ptr->client);
    if (ret != 0)
    {
        jack_error("jack_client_close() failed with error %d", ret);
    }

    controller_ptr->client = NULL;
    jack_controller_patchbay_free(controller_ptr);

fail_stop_server:
    if (!jackctl_server_stop(controller_ptr->server))
    {
        jack_error("failed to stop jack server");
    }

fail_close_server:
    jack_controller_unload_slave_drivers(controller_ptr);

    if (!jackctl_server_close(controller_ptr->server))
    {
        jack_error("failed to close jack server");
    }

fail:
    return FALSE;
}

bool
jack_controller_stop_server(
    struct jack_controller * controller_ptr,
    void *dbus_call_context_ptr)
{
    int ret;

    pthread_mutex_lock(&controller_ptr->lock);
    if (!list_empty(&controller_ptr->session_pending_commands))
    {
        pthread_mutex_unlock(&controller_ptr->lock);
        jack_dbus_error(dbus_call_context_ptr, JACK_DBUS_ERROR_GENERIC, "Refusing to stop JACK server because of pending session commands");
        return false;
    }
    pthread_mutex_unlock(&controller_ptr->lock);

    jack_info("Stopping jack server...");

    assert(controller_ptr->started); /* should be ensured by caller */

    ret = jack_deactivate(controller_ptr->client);
    if (ret != 0)
    {
        jack_error("failed to deactivate dbusapi jack client. error is %d", ret);
    }

    jack_controller_patchbay_uninit(controller_ptr);

    ret = jack_client_close(controller_ptr->client);
    if (ret != 0)
    {
        jack_error("jack_client_close() failed with error %d", ret);
    }

    controller_ptr->client = NULL;
    jack_controller_patchbay_free(controller_ptr);

    if (!jackctl_server_stop(controller_ptr->server))
    {
        jack_dbus_error(dbus_call_context_ptr, JACK_DBUS_ERROR_GENERIC, "Failed to stop server");
        return FALSE;
    }

    jack_controller_unload_slave_drivers(controller_ptr);

    if (!jackctl_server_close(controller_ptr->server))
    {
        jack_dbus_error(dbus_call_context_ptr, JACK_DBUS_ERROR_GENERIC, "Failed to close server");
        return FALSE;
    }

    controller_ptr->started = false;
    controller_ptr->bridges_running = 0;    /* closed with the server */

    return TRUE;
}

bool
jack_controller_switch_master(
    struct jack_controller * controller_ptr,
    void *dbus_call_context_ptr)
{
    assert(controller_ptr->started); /* should be ensured by caller */

    if (!jackctl_server_switch_master(
            controller_ptr->server,
            jack_params_get_driver(controller_ptr->params)))
    {
        /* The server has no running master now. Only clearing 'started' left
         * the dbusapi client, patchbay and server open: StopServer was refused
         * and StartServer failed ("server already active"). Run the normal
         * stop path so that a new StartServer works. */
        jack_error("Failed to switch master, stopping the server");
        jack_controller_stop_server(controller_ptr, NULL);
        controller_ptr->started = false;
        jack_dbus_error(dbus_call_context_ptr, JACK_DBUS_ERROR_GENERIC, "Failed to switch master (server stopped)");
        return FALSE;
    }

    return TRUE;
}

#define DEVICE_MAX 20

typedef struct reserved_audio_device {

     char device_name[64];
     rd_device * reserved_device;
     int count;

} reserved_audio_device;


int g_device_count = 0;
static reserved_audio_device g_reserved_device[DEVICE_MAX];
static pthread_mutex_t g_reserved_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 Slot bookkeeping: acquire used to write slot [g_device_count] even when that
 slot was still in use, release looked a device up by name but never freed
 its slot, and the count was decremented even for unknown devices. With
 several cards (or SwitchMaster while a slave holds one) this wrote out of
 bounds ([2] or [-1]), leaked ReserveDevice1 names (locking PulseAudio /
 PipeWire out of the card) and released the same reservation twice. Use free
 slots (reserved_device == NULL) and clear them on release.
 Sound card bridges reserve their cards too, from the server's bridge monitor thread as well as
 this one (hence the lock), and a bridge may use another device of the master's card: a name
 asked for twice is held once and counted, released when the last one lets go.
*/
static
bool
on_device_acquire(const char * device_name)
{
    int ret;
    int i;
    int slot = -1;
    DBusError error;

    pthread_mutex_lock(&g_reserved_lock);
    for (i = 0; i < DEVICE_MAX; i++) {
        if (g_reserved_device[i].reserved_device != NULL &&
            strcmp(g_reserved_device[i].device_name, device_name) == 0) {
            g_reserved_device[i].count++;
            pthread_mutex_unlock(&g_reserved_lock);
            return true;
        }
        if (slot < 0 && g_reserved_device[i].reserved_device == NULL)
            slot = i;
    }
    if (slot < 0)
    {
        jack_error("Cannot reserve audio card %s: already holding %d cards", device_name, DEVICE_MAX);
        pthread_mutex_unlock(&g_reserved_lock);
        return false;
    }

    dbus_error_init(&error);

    ret = rd_acquire(
        &g_reserved_device[slot].reserved_device,
        g_connection,
        device_name,
        "Jack audio server",
        INT32_MAX,
        NULL,
        &error);
    if (ret  < 0)
    {
        jack_error("Failed to acquire device name : %s error : %s", device_name, (error.message ? error.message : strerror(-ret)));
        dbus_error_free(&error);
        g_reserved_device[slot].reserved_device = NULL;
        pthread_mutex_unlock(&g_reserved_lock);
        return false;
    }

    snprintf(g_reserved_device[slot].device_name, sizeof(g_reserved_device[slot].device_name), "%s", device_name);
    g_reserved_device[slot].count = 1;
    g_device_count++;
    jack_info("Acquired audio card %s", device_name);
    pthread_mutex_unlock(&g_reserved_lock);
    return true;
}

static
void
on_device_release(const char * device_name)
{
    int i;

    pthread_mutex_lock(&g_reserved_lock);
    // Look for corresponding reserved device
    for (i = 0; i < DEVICE_MAX; i++) {
        if (g_reserved_device[i].reserved_device != NULL &&
            strcmp(g_reserved_device[i].device_name, device_name) == 0)
            break;
    }

    if (i == DEVICE_MAX) {
        jack_error("Audio card %s not found!!", device_name);
    } else if (--g_reserved_device[i].count == 0) {
        jack_info("Released audio card %s", device_name);
        rd_release(g_reserved_device[i].reserved_device);
        g_reserved_device[i].reserved_device = NULL;
        g_reserved_device[i].device_name[0] = 0;
        g_device_count--;
    }
    pthread_mutex_unlock(&g_reserved_lock);
}

#define controller_ptr ((struct jack_controller *)obj)

static bool slave_drivers_parameter_is_set(void * obj)
{
    return controller_ptr->slave_drivers_set;
}

static bool slave_drivers_parameter_reset(void * obj)
{
    if (controller_ptr->started)
    {
        jack_error("Cannot modify slave-drivers when server is started");
        return false;
    }

    jack_controller_remove_slave_drivers(controller_ptr);
    controller_ptr->slave_drivers_set = false;
    return true;
}

static union jackctl_parameter_value slave_drivers_parameter_get_value(void * obj)
{
    return controller_ptr->slave_drivers_vparam_value;
}

static bool slave_drivers_parameter_set_value(void * obj, const union jackctl_parameter_value * value_ptr)
{
    char * buffer;
    char * save;
    const char * token;
    struct list_head old_list;
    struct list_head new_list;
    union jackctl_parameter_value old_value;
    union jackctl_parameter_value new_value;
    bool old_set;

    if (controller_ptr->started)
    {
        jack_error("Cannot modify slave-drivers when server is started");
        return false;
    }

    old_set = controller_ptr->slave_drivers_set;
    old_value = controller_ptr->slave_drivers_vparam_value;
    controller_ptr->slave_drivers_vparam_value.str[0] = 0;
    old_list = controller_ptr->slave_drivers;
    INIT_LIST_HEAD(&controller_ptr->slave_drivers);

    buffer = strdup(value_ptr->str);
    if (buffer == NULL)
    {
        jack_error("strdup() failed.");
        return false;
    }

    token = strtok_r(buffer, ",", &save);
    while (token)
    {
        //jack_info("slave driver '%s'", token);
        if (!jack_controller_add_slave_driver(controller_ptr, token))
        {
            jack_controller_remove_slave_drivers(controller_ptr);
            controller_ptr->slave_drivers = old_list;
            controller_ptr->slave_drivers_vparam_value = old_value;
            controller_ptr->slave_drivers_set = old_set;

            free(buffer);

            return false;
        }

        token = strtok_r(NULL, ",", &save);
    }

    new_value = controller_ptr->slave_drivers_vparam_value;
    new_list = controller_ptr->slave_drivers;
    controller_ptr->slave_drivers = old_list;
    jack_controller_remove_slave_drivers(controller_ptr);
    controller_ptr->slave_drivers_vparam_value = new_value;
    controller_ptr->slave_drivers = new_list;
    controller_ptr->slave_drivers_set = true;

    free(buffer);

    return true;
}

static union jackctl_parameter_value slave_drivers_parameter_get_default_value(void * obj)
{
    union jackctl_parameter_value value;
    value.str[0] = 0;
    return value;
}

#undef controller_ptr

/*
 JackBus: sound card bridges. The "bridges" engine parameter lists them as jackd's --bridge
 takes them, device[/period[/nperiods]][@wake], separated by spaces (device names have commas:
 "hw:U44,0"). They are added once the server runs, and while it runs a change to the list adds
 and removes bridges at once. A listed card that is not there, or cannot be opened, stays listed
 but not running: it is tried again whenever sound cards change (a card plugged in).
*/

#define BRIDGE_SEPARATORS " \t;"
#define BRIDGE_DEVICE_SIZE (JACK_PARAM_STRING_MAX + 8)     /* a spec's device, "hw:" and ",0" added */

static int
jack_controller_bridges_split(
    const char * list,
    char specs[][JACK_PARAM_STRING_MAX + 1],
    int max)
{
    char buffer[JACK_PARAM_STRING_MAX + 1];
    char * save;
    char * token;
    int n = 0;

    snprintf(buffer, sizeof(buffer), "%s", list);
    for (token = strtok_r(buffer, BRIDGE_SEPARATORS, &save);
         token != NULL && n < max;
         token = strtok_r(NULL, BRIDGE_SEPARATORS, &save))
    {
        snprintf(specs[n++], JACK_PARAM_STRING_MAX + 1, "%s", token);
    }
    return n;
}

/* the device a spec names, as one name for one device: "U44", "hw:U44" and "hw:U44,0" are all
   "hw:U44,0" */
static void
jack_controller_bridge_device(
    const char * spec,
    char * device,
    size_t size)
{
    char buffer[JACK_PARAM_STRING_MAX + 1];
    const size_t len = strcspn(spec, "/@");

    snprintf(buffer, sizeof(buffer), "%.*s", (int)(len < sizeof(buffer) ? len : sizeof(buffer) - 1), spec);
    if (strchr(buffer, ':') == NULL)
    {
        snprintf(device, size, "hw:%s,0", buffer);
    }
    else if ((strncmp(buffer, "hw:", 3) == 0 || strncmp(buffer, "plughw:", 7) == 0) && strchr(buffer, ',') == NULL)
    {
        snprintf(device, size, "%s,0", buffer);
    }
    else
    {
        snprintf(device, size, "%s", buffer);
    }
}

/* the /proc/asound directory of the card a spec names (<id> or card<n>); false for device
   names other than hw: and plughw: ones */
static bool
jack_controller_bridge_card_path(
    const char * spec,
    char * path,
    size_t size)
{
    char device[BRIDGE_DEVICE_SIZE];
    char card[64];
    const char * p;

    jack_controller_bridge_device(spec, device, sizeof(device));
    if (strncmp(device, "hw:", 3) == 0)
    {
        p = device + 3;
    }
    else if (strncmp(device, "plughw:", 7) == 0)
    {
        p = device + 7;
    }
    else
    {
        return false;
    }
    if (strncmp(p, "CARD=", 5) == 0)
    {
        p += 5;
    }
    snprintf(card, sizeof(card), "%.*s", (int)strcspn(p, ","), p);
    if (card[0] == 0)
    {
        return false;
    }
    if (card[strspn(card, "0123456789")] == 0)
    {
        snprintf(path, size, "/proc/asound/card%s", card);
    }
    else
    {
        snprintf(path, size, "/proc/asound/%s", card);
    }
    return true;
}

/* whether the card a spec names is there now (other device names than hw: and plughw: ones:
   assumed) */
static bool
jack_controller_bridge_card_present(
    const char * spec)
{
    char path[128];

    if (!jack_controller_bridge_card_path(spec, path, sizeof(path)))
    {
        return true;
    }
    return access(path, F_OK) == 0;
}

/* whether the card a spec names has audio: a PCM device (/proc/asound/cardN/pcmXp or pcmXc).
   A card with MIDI only (a USB keyboard, a controller) has none: there is nothing to bridge,
   its MIDI ports come from the MIDI slave drivers. Not there, or another device name: assumed */
static bool
jack_controller_bridge_card_has_pcm(
    const char * spec)
{
    char path[128];
    struct dirent * entry;
    DIR * dir;
    bool pcm = false;

    if (!jack_controller_bridge_card_path(spec, path, sizeof(path))
        || (dir = opendir(path)) == NULL)
    {
        return true;
    }
    while (!pcm && (entry = readdir(dir)) != NULL)
    {
        size_t len = strlen(entry->d_name);
        pcm = strncmp(entry->d_name, "pcm", 3) == 0 && len > 4
            && strspn(entry->d_name + 3, "0123456789") == len - 4
            && (entry->d_name[len - 1] == 'p' || entry->d_name[len - 1] == 'c');
    }
    closedir(dir);
    return pcm;
}

static int
jack_controller_bridges_running_index(
    struct jack_controller * controller_ptr,
    const char * spec)
{
    int i;

    for (i = 0; i < controller_ptr->bridges_running; i++)
    {
        if (strcmp(controller_ptr->bridge_spec[i], spec) == 0)
        {
            return i;
        }
    }
    return -1;
}

const char *
jack_controller_bridges_running_name(
    struct jack_controller * controller_ptr,
    const char * spec)
{
    const int i = jack_controller_bridges_running_index(controller_ptr, spec);

    return i < 0 ? NULL : controller_ptr->bridge_name[i];
}

static void
jack_controller_bridges_forget(
    struct jack_controller * controller_ptr,
    int i)
{
    for (; i + 1 < controller_ptr->bridges_running; i++)
    {
        memcpy(controller_ptr->bridge_spec[i], controller_ptr->bridge_spec[i + 1], sizeof(controller_ptr->bridge_spec[i]));
        memcpy(controller_ptr->bridge_name[i], controller_ptr->bridge_name[i + 1], sizeof(controller_ptr->bridge_name[i]));
    }
    controller_ptr->bridges_running--;
}

static void
jack_controller_bridges_remember(
    struct jack_controller * controller_ptr,
    const char * spec,
    const char * name)
{
    const int i = controller_ptr->bridges_running;

    if (i >= JACK_CONTROLLER_BRIDGES_MAX)
    {
        return;
    }
    snprintf(controller_ptr->bridge_spec[i], sizeof(controller_ptr->bridge_spec[i]), "%s", spec);
    snprintf(controller_ptr->bridge_name[i], sizeof(controller_ptr->bridge_name[i]), "%s", name);
    controller_ptr->bridges_running++;
}

/* the server running: the bridges it runs follow the list */
void
jack_controller_bridges_apply(
    struct jack_controller * controller_ptr)
{
    char specs[JACK_CONTROLLER_BRIDGES_MAX][JACK_PARAM_STRING_MAX + 1];
    char name[64 + 1];
    int n;
    int i;
    int j;
    bool listed;

    if (!controller_ptr->started)
    {
        return;
    }
    n = jack_controller_bridges_split(controller_ptr->bridges_vparam_value.str, specs, JACK_CONTROLLER_BRIDGES_MAX);

    /* no longer listed: removed */
    for (i = 0; i < controller_ptr->bridges_running; )
    {
        listed = false;
        for (j = 0; j < n; j++)
        {
            if (strcmp(specs[j], controller_ptr->bridge_spec[i]) == 0)
            {
                listed = true;
            }
        }
        if (listed)
        {
            i++;
        }
        else if (jackctl_server_remove_bridge(controller_ptr->server, controller_ptr->bridge_name[i]))
        {
            jack_info("bridge %s removed", controller_ptr->bridge_name[i]);
            jack_controller_bridges_forget(controller_ptr, i);
        }
        else
        {
            i++;    /* the server still has it */
        }
    }

    /* listed and not running: added, if the card is there (else when sound cards change) */
    for (j = 0; j < n; j++)
    {
        if (jack_controller_bridges_running_index(controller_ptr, specs[j]) >= 0 ||
            !jack_controller_bridge_card_present(specs[j]))
        {
            continue;
        }
        if (jackctl_server_add_bridge_spec(controller_ptr->server, specs[j], name))
        {
            jack_controller_bridges_remember(controller_ptr, specs[j], name);
        }
        else
        {
            jack_error("bridge %s not added: tried again when sound cards change", specs[j]);
        }
    }
}

/* the list, checked (each spec, each device once) and with single spaces; false: not stored */
static bool
jack_controller_bridges_store(
    struct jack_controller * controller_ptr,
    const char * list,
    char * error,
    size_t error_size)
{
    char specs[JACK_CONTROLLER_BRIDGES_MAX + 1][JACK_PARAM_STRING_MAX + 1];
    char device_i[BRIDGE_DEVICE_SIZE];
    char device_j[BRIDGE_DEVICE_SIZE];
    char joined[JACK_PARAM_STRING_MAX + 1];
    size_t len = 0;
    int n;
    int i;
    int j;

    if (strlen(list) > JACK_PARAM_STRING_MAX)
    {
        snprintf(error, error_size, "the bridges list is longer than %d characters", JACK_PARAM_STRING_MAX);
        return false;
    }
    n = jack_controller_bridges_split(list, specs, JACK_CONTROLLER_BRIDGES_MAX + 1);
    if (n > JACK_CONTROLLER_BRIDGES_MAX)
    {
        snprintf(error, error_size, "no more than %d bridges", JACK_CONTROLLER_BRIDGES_MAX);
        return false;
    }
    joined[0] = 0;
    for (i = 0; i < n; i++)
    {
        if (!jackctl_bridge_spec_check(specs[i]))
        {
            snprintf(error, error_size, "not a bridge: \"%s\" (device[/period[/nperiods]][@auto|timer|irq])", specs[i]);
            return false;
        }
        jack_controller_bridge_device(specs[i], device_i, sizeof(device_i));
        for (j = 0; j < i; j++)
        {
            jack_controller_bridge_device(specs[j], device_j, sizeof(device_j));
            if (strcmp(device_i, device_j) == 0)
            {
                snprintf(error, error_size, "%s is bridged already (%s)", device_i, specs[j]);
                return false;
            }
        }
        len += snprintf(joined + len, sizeof(joined) - len, "%s%s", i ? " " : "", specs[i]);
    }
    snprintf(controller_ptr->bridges_vparam_value.str, sizeof(controller_ptr->bridges_vparam_value.str), "%s", joined);
    controller_ptr->bridges_set = (n > 0);
    return true;
}

#define controller_ptr ((struct jack_controller *)obj)

static bool bridges_parameter_is_set(void * obj)
{
    return controller_ptr->bridges_set;
}

static bool bridges_parameter_set_value(void * obj, const union jackctl_parameter_value * value_ptr)
{
    char error[256];

    if (!jack_controller_bridges_store(controller_ptr, value_ptr->str, error, sizeof(error)))
    {
        jack_error("bridges: %s", error);
        return false;
    }
    jack_controller_bridges_apply(controller_ptr);
    return true;
}

static bool bridges_parameter_reset(void * obj)
{
    union jackctl_parameter_value value;

    value.str[0] = 0;
    return bridges_parameter_set_value(obj, &value);
}

static union jackctl_parameter_value bridges_parameter_get_value(void * obj)
{
    return controller_ptr->bridges_vparam_value;
}

static union jackctl_parameter_value bridges_parameter_get_default_value(void * obj)
{
    union jackctl_parameter_value value;
    value.str[0] = 0;
    return value;
}

#undef controller_ptr

bool
jack_controller_bridges_add(
    struct jack_controller * controller_ptr,
    const char * spec,
    char name[64 + 1],
    char * error,
    size_t error_size)
{
    char list[2 * JACK_PARAM_STRING_MAX + 2];
    char saved[JACK_PARAM_STRING_MAX + 1];

    name[0] = 0;
    if (strpbrk(spec, BRIDGE_SEPARATORS) != NULL || spec[0] == 0)
    {
        snprintf(error, error_size, "not a bridge: \"%s\"", spec);
        return false;
    }
    if (!jack_controller_bridge_card_has_pcm(spec))
    {
        snprintf(error, error_size, "%s has no audio device, only MIDI: nothing to bridge "
            "(its MIDI ports come from a MIDI slave driver, alsarawmidi)", spec);
        return false;
    }
    snprintf(saved, sizeof(saved), "%s", controller_ptr->bridges_vparam_value.str);
    snprintf(list, sizeof(list), "%s%s%s", saved, saved[0] ? " " : "", spec);
    if (!jack_controller_bridges_store(controller_ptr, list, error, error_size))
    {
        return false;
    }
    if (!controller_ptr->started)
    {
        return true;    /* added when the server starts */
    }
    /* now, or not at all */
    if (!jack_controller_bridge_card_present(spec))
    {
        snprintf(error, error_size, "the card of %s is not there", spec);
    }
    else if (jackctl_server_add_bridge_spec(controller_ptr->server, spec, name))
    {
        jack_controller_bridges_remember(controller_ptr, spec, name);
        return true;
    }
    else
    {
        snprintf(error, error_size, "%s could not be bridged (the server's log says why)", spec);
    }
    jack_controller_bridges_store(controller_ptr, saved, error + strlen(error), 0);
    return false;
}

bool
jack_controller_bridges_remove(
    struct jack_controller * controller_ptr,
    const char * what,
    char * error,
    size_t error_size)
{
    char specs[JACK_CONTROLLER_BRIDGES_MAX][JACK_PARAM_STRING_MAX + 1];
    char device[BRIDGE_DEVICE_SIZE];
    char wanted[BRIDGE_DEVICE_SIZE];
    char list[JACK_PARAM_STRING_MAX + 1];
    const char * name;
    size_t len = 0;
    int found = -1;
    int running;
    int n;
    int i;

    /* by the bridge's client name, its spec, or its device */
    n = jack_controller_bridges_split(controller_ptr->bridges_vparam_value.str, specs, JACK_CONTROLLER_BRIDGES_MAX);
    jack_controller_bridge_device(what, wanted, sizeof(wanted));
    for (i = 0; i < n && found < 0; i++)
    {
        name = jack_controller_bridges_running_name(controller_ptr, specs[i]);
        jack_controller_bridge_device(specs[i], device, sizeof(device));
        if (strcmp(specs[i], what) == 0 || strcmp(device, wanted) == 0 || (name != NULL && strcmp(name, what) == 0))
        {
            found = i;
        }
    }
    if (found < 0)
    {
        snprintf(error, error_size, "no bridge %s", what);
        return false;
    }
    running = jack_controller_bridges_running_index(controller_ptr, specs[found]);
    if (running >= 0)
    {
        if (!jackctl_server_remove_bridge(controller_ptr->server, controller_ptr->bridge_name[running]))
        {
            snprintf(error, error_size, "the bridge %s could not be removed", controller_ptr->bridge_name[running]);
            return false;
        }
        jack_info("bridge %s removed", controller_ptr->bridge_name[running]);
        jack_controller_bridges_forget(controller_ptr, running);
    }
    list[0] = 0;
    for (i = 0; i < n; i++)
    {
        if (i != found)
        {
            len += snprintf(list + len, sizeof(list) - len, "%s%s", len ? " " : "", specs[i]);
        }
    }
    return jack_controller_bridges_store(controller_ptr, list, error, error_size);
}

#define controller_ptr ((struct jack_controller *)obj)

#undef controller_ptr

/* JackBus: sound card hotplug. The ALSA driver's device list was built once, when jackdbus
   started; a card plugged in later never appeared. jackdbus watches /dev/snd (inotify, read
   without blocking from the main loop, which wakes every 200 ms anyway) and, once the changes
   have settled, rebuilds the list (jackctl_driver_refresh_constraints), copies it again into
   the D-Bus parameter tree and tells clients (Configure.ConstraintsChanged). Nothing here
   touches the server or its real-time thread. */

#define SND_DIR "/dev/snd"
#define SND_SETTLE_MS 500       /* quiet time after the last change */
#define SND_READY_MAX_MS 5000   /* longest wait for a card's nodes to become usable */
#define SND_REWATCH_MS 1000     /* how often to retry watching a missing /dev/snd */

static int64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void jack_controller_snd_watch_add(struct jack_controller * controller_ptr)
{
    controller_ptr->snd_last_rewatch = monotonic_ms();
    controller_ptr->snd_watch_wd = inotify_add_watch(
        controller_ptr->snd_watch_fd,
        SND_DIR,
        IN_CREATE | IN_DELETE | IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF);
}

static void jack_controller_snd_watch_init(struct jack_controller * controller_ptr)
{
    controller_ptr->snd_watch_wd = -1;
    controller_ptr->snd_first_change = 0;
    controller_ptr->snd_last_change = 0;
    controller_ptr->snd_last_rewatch = 0;
    controller_ptr->snd_watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (controller_ptr->snd_watch_fd < 0)
    {
        jack_info("Sound cards plugged in later won't be listed (inotify: %s)", strerror(errno));
        return;
    }
    jack_controller_snd_watch_add(controller_ptr);
}

/* Every card's nodes are present and usable: a PCM node's card has its control node (the
   kernel registers that one last), and each node can be opened for reading and writing (udev
   and logind set permissions after the node appears). */
static bool jack_controller_snd_nodes_ready(void)
{
    DIR * dir = opendir(SND_DIR);
    struct dirent * entry;
    char path[PATH_MAX];
    unsigned int card, device;
    char direction;
    bool ready = true;

    if (dir == NULL)
    {
        return true;
    }
    while (ready && (entry = readdir(dir)) != NULL)
    {
        if (sscanf(entry->d_name, "controlC%u", &card) == 1)
        {
            snprintf(path, sizeof(path), SND_DIR "/%s", entry->d_name);
            ready = access(path, R_OK | W_OK) == 0;
        }
        else if (sscanf(entry->d_name, "pcmC%uD%u%c", &card, &device, &direction) == 3)
        {
            snprintf(path, sizeof(path), SND_DIR "/%s", entry->d_name);
            ready = access(path, R_OK | W_OK) == 0;
            snprintf(path, sizeof(path), SND_DIR "/controlC%u", card);
            ready = ready && access(path, F_OK) == 0;
        }
    }
    closedir(dir);
    return ready;
}

static void jack_controller_snd_refresh(struct jack_controller * controller_ptr)
{
    const JSList * node;
    jackctl_driver_t * driver = NULL;
    int changed;
    const char * address[2] = { "drivers", "alsa" };

    for (node = jackctl_server_get_drivers_list(controller_ptr->server); node != NULL; node = jack_slist_next(node))
    {
        if (strcmp(jackctl_driver_get_name(node->data), "alsa") == 0)
        {
            driver = node->data;
        }
    }
    if (driver == NULL)
    {
        return;
    }

    changed = jackctl_driver_refresh_constraints(driver);
    if (changed < 0)
    {
        jack_error("Could not read the ALSA devices again");
        return;
    }
    if (changed == 0)
    {
        return;
    }
    /* the tree's copies point into the lists just replaced: copy them again before any D-Bus
       message is handled */
    if (!jack_params_reload_driver_constraints(controller_ptr->params, driver))
    {
        jack_error("Could not copy the new ALSA device list");
    }
    jack_info("Sound cards changed: the ALSA device list was read again");
    jack_controller_configure_send_signal_constraints_changed(address, 2);
}

static void jack_controller_snd_watch_run(struct jack_controller * controller_ptr)
{
    char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    const struct inotify_event * event;
    ssize_t len;
    char * ptr;
    bool changed = false;
    int64_t now;

    if (controller_ptr->snd_watch_fd < 0)
    {
        return;
    }
    now = monotonic_ms();

    if (controller_ptr->snd_watch_wd < 0 && now - controller_ptr->snd_last_rewatch >= SND_REWATCH_MS)
    {
        jack_controller_snd_watch_add(controller_ptr);
        if (controller_ptr->snd_watch_wd >= 0)
        {
            changed = true;     /* /dev/snd (re)appeared: whatever is in it is new */
        }
    }

    while ((len = read(controller_ptr->snd_watch_fd, buffer, sizeof(buffer))) > 0)
    {
        for (ptr = buffer; ptr < buffer + len; ptr += sizeof(struct inotify_event) + event->len)
        {
            event = (const struct inotify_event *)ptr;
            if (event->mask & IN_Q_OVERFLOW)
            {
                changed = true;
            }
            else if (event->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF))
            {
                controller_ptr->snd_watch_wd = -1;      /* gone: watch it again when it is back */
                changed = true;
            }
            else if (event->len > 0 &&
                     (strncmp(event->name, "controlC", 8) == 0 || strncmp(event->name, "pcmC", 4) == 0))
            {
                changed = true;
            }
        }
    }

    if (changed)
    {
        if (controller_ptr->snd_first_change == 0)
        {
            controller_ptr->snd_first_change = now;
        }
        controller_ptr->snd_last_change = now;
    }

    if (controller_ptr->snd_first_change == 0 ||
        now - controller_ptr->snd_last_change < SND_SETTLE_MS)
    {
        return;
    }
    if (!jack_controller_snd_nodes_ready() &&
        now - controller_ptr->snd_first_change < SND_READY_MAX_MS)
    {
        return;
    }

    controller_ptr->snd_first_change = 0;
    controller_ptr->snd_last_change = 0;
    jack_controller_snd_refresh(controller_ptr);
    /* a listed bridge's card plugged in: bridged now */
    jack_controller_bridges_apply(controller_ptr);
}

/* JackBus: surviving an unplugged card. When the master driver's device goes away (a USB
   interface pulled out), the driver thread calls back (jackctl_server_set_backend_lost_callback)
   instead of telling clients the server stopped. On the next tick, jackdbus puts the dummy
   driver in its place, at the same rate and buffer size and with as many ports, so clients
   keep running (silently) and keep their connections (the server restores them by port name).
   When the card is back and free, jackdbus switches back to it. jackdbus's saved settings
   (the selected driver, the dummy's parameters) are left as they were. */

#define FALLBACK_RETRY_MS 1000
#define FALLBACK_MAX_FAILURES 30

static void jack_controller_on_backend_lost(void * arg)
{
    __atomic_store_n(&((struct jack_controller *)arg)->backend_lost, 1, __ATOMIC_RELEASE);
}

static jackctl_driver_t * jack_controller_nspa_driver(struct jack_controller * controller_ptr, const char * name)
{
    const JSList * node;
    for (node = jackctl_server_get_drivers_list(controller_ptr->server); node != NULL; node = jack_slist_next(node))
    {
        if (strcmp(jackctl_driver_get_name(node->data), name) == 0)
        {
            return node->data;
        }
    }
    return NULL;
}

static jackctl_parameter_t * jack_controller_nspa_param(jackctl_driver_t * driver, const char * name)
{
    const JSList * node;
    for (node = jackctl_driver_get_parameters(driver); node != NULL; node = jack_slist_next(node))
    {
        if (strcmp(jackctl_parameter_get_name(node->data), name) == 0)
        {
            return node->data;
        }
    }
    return NULL;
}

static int jack_controller_count_ports(jack_client_t * client, const char * pattern)
{
    const char ** ports = client ? jack_get_ports(client, pattern, JACK_DEFAULT_AUDIO_TYPE, 0) : NULL;
    int n = 0;
    if (ports != NULL)
    {
        while (ports[n] != NULL)
        {
            n++;
        }
        jack_free(ports);
    }
    return n;
}

/* Switch the running master to the dummy driver, shaped like the one that went away. The dummy's
   own settings are set only for the switch (the driver copies them when it opens) and put back
   right after, so nothing changes in conf.xml. */
static bool jack_controller_switch_to_dummy(struct jack_controller * controller_ptr)
{
    static const char * names[4] = { "rate", "period", "capture", "playback" };
    jackctl_driver_t * dummy = jack_controller_nspa_driver(controller_ptr, "dummy");
    jackctl_parameter_t * params[4];
    union jackctl_parameter_value saved[4], value;
    bool was_set[4];
    bool ok;
    int i;

    if (dummy == NULL || controller_ptr->client == NULL)
    {
        return false;
    }
    for (i = 0; i < 4; i++)
    {
        params[i] = jack_controller_nspa_param(dummy, names[i]);
        if (params[i] == NULL)
        {
            return false;
        }
        was_set[i] = jackctl_parameter_is_set(params[i]);
        saved[i] = jackctl_parameter_get_value(params[i]);
    }
    value.ui = jack_get_sample_rate(controller_ptr->client);
    jackctl_parameter_set_value(params[0], &value);
    value.ui = jack_get_buffer_size(controller_ptr->client);
    jackctl_parameter_set_value(params[1], &value);
    value.ui = jack_controller_count_ports(controller_ptr->client, "^system:capture_");
    jackctl_parameter_set_value(params[2], &value);
    value.ui = jack_controller_count_ports(controller_ptr->client, "^system:playback_");
    jackctl_parameter_set_value(params[3], &value);

    ok = jackctl_server_switch_master(controller_ptr->server, dummy);

    for (i = 0; i < 4; i++)
    {
        if (was_set[i])
        {
            jackctl_parameter_set_value(params[i], &saved[i]);
        }
        else
        {
            jackctl_parameter_reset(params[i]);
        }
    }
    return ok;
}

/* The card a driver's "device" names ("hw:U44", "hw:U44,0", "hw:1") is present, its nodes usable
   and the PCM not open by anyone (PipeWire probes a new card for a moment). */
static bool jack_controller_card_ready(jackctl_driver_t * driver)
{
    jackctl_parameter_t * param = jack_controller_nspa_param(driver, "device");
    union jackctl_parameter_value value;
    char card[64], path[PATH_MAX], line[64];
    const char * name;
    int device = 0, n;
    FILE * f;
    const char * dirs[2] = { "p", "c" };

    if (param == NULL)
    {
        return false;
    }
    value = jackctl_parameter_get_value(param);
    name = value.str;
    if (strncmp(name, "hw:", 3) != 0)
    {
        return true;        /* not a hardware card name: just try */
    }
    name += 3;
    n = strcspn(name, ",");
    if (n == 0 || n >= (int)sizeof(card))
    {
        return true;
    }
    memcpy(card, name, n);
    card[n] = 0;
    if (name[n] == ',')
    {
        device = atoi(name + n + 1);
    }
    if (card[0] >= '0' && card[0] <= '9')
    {
        snprintf(path, sizeof(path), "/proc/asound/card%s", card);
    }
    else
    {
        snprintf(path, sizeof(path), "/proc/asound/%s", card);
    }
    if (access(path, F_OK) != 0 || !jack_controller_snd_nodes_ready())
    {
        return false;
    }
    for (n = 0; n < 2; n++)
    {
        char status[PATH_MAX + 32];
        snprintf(status, sizeof(status), "%s/pcm%d%s/sub0/status", path, device, dirs[n]);
        f = fopen(status, "r");
        if (f == NULL)
        {
            continue;       /* no such direction, or no procfs detail: don't block on it */
        }
        line[0] = 0;
        if (fgets(line, sizeof(line), f) == NULL)
        {
            line[0] = 0;
        }
        fclose(f);
        if (line[0] != 0 && strncmp(line, "closed", 6) != 0)
        {
            return false;   /* open by someone: try again later */
        }
    }
    return true;
}

static void jack_controller_fallback_run(struct jack_controller * controller_ptr)
{
    int64_t now;

    if (__atomic_exchange_n(&controller_ptr->backend_lost, 0, __ATOMIC_ACQ_REL))
    {
        jackctl_driver_t * lost = jack_params_get_driver(controller_ptr->params);
        if (!controller_ptr->started)
        {
            return;
        }
        if (jack_controller_switch_to_dummy(controller_ptr))
        {
            /* keep the first driver if it was already a fallback (the dummy cannot lose a device) */
            if (controller_ptr->fallback_from == NULL)
            {
                controller_ptr->fallback_from = lost;
            }
            controller_ptr->fallback_next_try = monotonic_ms() + FALLBACK_RETRY_MS;
            controller_ptr->fallback_failures = 0;
            jack_error("The sound card of the %s driver is gone: the server keeps running on the dummy driver until it is back",
                       jackctl_driver_get_name(controller_ptr->fallback_from));
        }
        else
        {
            jack_error("The sound card is gone and the dummy driver could not take over: the server is stopped");
            jack_controller_stop_server(controller_ptr, NULL);
        }
        return;
    }

    if (controller_ptr->fallback_from == NULL)
    {
        return;
    }
    if (!controller_ptr->started)
    {
        controller_ptr->fallback_from = NULL;   /* stopped meanwhile: the next start is a new one */
        return;
    }
    now = monotonic_ms();
    if (now < controller_ptr->fallback_next_try)
    {
        return;
    }
    controller_ptr->fallback_next_try = now + FALLBACK_RETRY_MS;
    if (!jack_controller_card_ready(controller_ptr->fallback_from))
    {
        return;
    }
    if (jackctl_server_switch_master(controller_ptr->server, controller_ptr->fallback_from))
    {
        jack_info("The sound card is back: the server runs on the %s driver again",
                  jackctl_driver_get_name(controller_ptr->fallback_from));
        controller_ptr->fallback_from = NULL;
        return;
    }
    /* the card is there but would not open (busy, or not the same card): the dummy again */
    controller_ptr->fallback_failures++;
    if (!jack_controller_switch_to_dummy(controller_ptr))
    {
        jack_error("The sound card is back but could not be used, and the dummy driver could not take over again: the server is stopped");
        controller_ptr->fallback_from = NULL;
        jack_controller_stop_server(controller_ptr, NULL);
        return;
    }
    if (controller_ptr->fallback_failures >= FALLBACK_MAX_FAILURES)
    {
        jack_error("The sound card is back but could not be used (%d tries): the server stays on the dummy driver; switch the master driver to use it",
                   controller_ptr->fallback_failures);
        controller_ptr->fallback_from = NULL;
    }
}

void *
jack_controller_create(
        DBusConnection *connection)
{
    int error;
    struct jack_controller *controller_ptr;
    const char * address[PARAM_ADDRESS_SIZE];
    DBusObjectPathVTable vtable =
    {
        jack_dbus_message_handler_unregister,
        jack_dbus_message_handler,
        NULL
    };

    controller_ptr = malloc(sizeof(struct jack_controller));
    if (!controller_ptr)
    {
        jack_error("Ran out of memory trying to allocate struct jack_controller");
        goto fail;
    }

    error = pthread_mutex_init(&controller_ptr->lock, NULL);
    if (error != 0)
    {
        jack_error("Failed to initialize mutex. error %d", error);
        goto fail_free;
    }

    INIT_LIST_HEAD(&controller_ptr->session_pending_commands);

    controller_ptr->server = jackctl_server_create2(on_device_acquire, on_device_release, NULL);
    if (controller_ptr->server == NULL)
    {
        jack_error("Failed to create server object");
        goto fail_uninit_mutex;
    }
    controller_ptr->backend_lost = 0;
    jackctl_server_set_backend_lost_callback(controller_ptr->server, jack_controller_on_backend_lost, controller_ptr);

    controller_ptr->params = jack_params_create(controller_ptr->server);
    if (controller_ptr->params == NULL)
    {
        jack_error("Failed to initialize parameter tree");
        goto fail_destroy_server;
    }

    controller_ptr->client = NULL;
    controller_ptr->started = false;

    controller_ptr->pending_save = 0;

    jack_controller_snd_watch_init(controller_ptr);
    controller_ptr->backend_lost = 0;
    controller_ptr->fallback_from = NULL;
    controller_ptr->fallback_next_try = 0;
    controller_ptr->fallback_failures = 0;

    INIT_LIST_HEAD(&controller_ptr->slave_drivers);
    controller_ptr->slave_drivers_set = false;
    controller_ptr->slave_drivers_vparam_value.str[0] = 0;

    controller_ptr->slave_drivers_vparam.obj = controller_ptr;

    controller_ptr->slave_drivers_vparam.vtable.is_set = slave_drivers_parameter_is_set;
    controller_ptr->slave_drivers_vparam.vtable.reset = slave_drivers_parameter_reset;
    controller_ptr->slave_drivers_vparam.vtable.get_value = slave_drivers_parameter_get_value;
    controller_ptr->slave_drivers_vparam.vtable.set_value = slave_drivers_parameter_set_value;
    controller_ptr->slave_drivers_vparam.vtable.get_default_value = slave_drivers_parameter_get_default_value;

    controller_ptr->slave_drivers_vparam.type = JackParamString;
    controller_ptr->slave_drivers_vparam.name = "slave-drivers";
    controller_ptr->slave_drivers_vparam.short_decr = "Slave drivers to use";
    controller_ptr->slave_drivers_vparam.long_descr = "A comma separated list of slave drivers";
    controller_ptr->slave_drivers_vparam.constraint_flags = 0;

    address[0] = PTNODE_ENGINE;
    address[1] = NULL;
    jack_params_add_parameter(controller_ptr->params, address, true, &controller_ptr->slave_drivers_vparam);

    controller_ptr->bridges_set = false;
    controller_ptr->bridges_running = 0;
    controller_ptr->bridges_vparam_value.str[0] = 0;
    controller_ptr->bridges_vparam.obj = controller_ptr;
    controller_ptr->bridges_vparam.vtable.is_set = bridges_parameter_is_set;
    controller_ptr->bridges_vparam.vtable.reset = bridges_parameter_reset;
    controller_ptr->bridges_vparam.vtable.get_value = bridges_parameter_get_value;
    controller_ptr->bridges_vparam.vtable.set_value = bridges_parameter_set_value;
    controller_ptr->bridges_vparam.vtable.get_default_value = bridges_parameter_get_default_value;
    controller_ptr->bridges_vparam.type = JackParamString;
    controller_ptr->bridges_vparam.name = "bridges";
    controller_ptr->bridges_vparam.short_decr = "Sound card bridges";
    controller_ptr->bridges_vparam.long_descr =
        "Sound cards on their own clocks, resampled into the graph: device[/period[/nperiods]][@auto|timer|irq], "
        "separated by spaces (\"U44 Headset/64\"). Changes apply at once while the server runs.";
    controller_ptr->bridges_vparam.constraint_flags = 0;
    jack_params_add_parameter(controller_ptr->params, address, true, &controller_ptr->bridges_vparam);

    controller_ptr->dbus_descriptor.context = controller_ptr;
    controller_ptr->dbus_descriptor.interfaces = g_jackcontroller_interfaces;

    if (!dbus_connection_register_object_path(
            connection,
            JACK_CONTROLLER_OBJECT_PATH,
            &vtable,
            &controller_ptr->dbus_descriptor))
    {
        jack_error("Ran out of memory trying to register D-Bus object path");
        goto fail_destroy_params;
    }

    jack_controller_settings_load(controller_ptr);

    return controller_ptr;

fail_destroy_params:
    if (controller_ptr->snd_watch_fd >= 0)
    {
        close(controller_ptr->snd_watch_fd);
    }
    jack_params_destroy(controller_ptr->params);

fail_destroy_server:
    jackctl_server_set_backend_lost_callback(controller_ptr->server, NULL, NULL);
    jackctl_server_destroy(controller_ptr->server);

fail_uninit_mutex:
    pthread_mutex_destroy(&controller_ptr->lock);

fail_free:
    free(controller_ptr);

fail:
    return NULL;
}

bool
jack_controller_add_slave_driver(
    struct jack_controller * controller_ptr,
    const char * driver_name)
{
    jackctl_driver_t * driver;
    struct jack_controller_slave_driver * driver_ptr;
    size_t len_old;
    size_t len_new;

    len_old = strlen(controller_ptr->slave_drivers_vparam_value.str);
    len_new = strlen(driver_name);
    if (len_old + len_new + 2 > sizeof(controller_ptr->slave_drivers_vparam_value.str))
    {
        jack_error("No more space for slave drivers.");
        return false;
    }

    driver = jack_controller_find_driver(controller_ptr->server, driver_name);
    if (driver == NULL)
    {
        jack_error("Unknown driver \"%s\"", driver_name);
        return false;
    }

    if (jack_controller_check_slave_driver(controller_ptr, driver_name))
    {
        jack_info("Driver \"%s\" is already slave", driver_name);
        return true;
    }

    driver_ptr = malloc(sizeof(struct jack_controller_slave_driver));
    if (driver_ptr == NULL)
    {
        jack_error("malloc() failed to allocate jack_controller_slave_driver struct");
        return false;
    }

    driver_ptr->name = strdup(driver_name);
    if (driver_ptr->name == NULL)
    {
        jack_error("strdup() failed for slave driver name \"%s\"", driver_name);
        free(driver_ptr);
        return false;
    }

    driver_ptr->handle = driver;
    driver_ptr->loaded = false;

    jack_info("driver \"%s\" set as slave", driver_name);

    list_add_tail(&driver_ptr->siblings, &controller_ptr->slave_drivers);

    if (len_old != 0)
    {
        controller_ptr->slave_drivers_vparam_value.str[len_old++] = ',';
    }

    memcpy(controller_ptr->slave_drivers_vparam_value.str + len_old, driver_name, len_new + 1);
    controller_ptr->slave_drivers_set = true;

    return true;
}

bool
jack_controller_remove_slave_driver(
    struct jack_controller * controller_ptr,
    const char * driver_name)
{
    struct list_head * node_ptr;
    struct jack_controller_slave_driver * driver_ptr;

    list_for_each(node_ptr, &controller_ptr->slave_drivers)
    {
        driver_ptr = list_entry(node_ptr, struct jack_controller_slave_driver, siblings);
        if (strcmp(driver_ptr->name, driver_name) == 0)
        {
            list_del(&driver_ptr->siblings);
            free(driver_ptr->name);
            free(driver_ptr);

            /* update the slave-drivers param value */
            controller_ptr->slave_drivers_vparam_value.str[0] = 0;
            list_for_each(node_ptr, &controller_ptr->slave_drivers)
            {
                driver_ptr = list_entry(node_ptr, struct jack_controller_slave_driver, siblings);
                if (controller_ptr->slave_drivers_vparam_value.str[0] != 0)
                {
                    strcat(controller_ptr->slave_drivers_vparam_value.str, ",");
                }

                strcat(controller_ptr->slave_drivers_vparam_value.str, driver_ptr->name);
            }

            jack_info("driver \"%s\" is not slave anymore", driver_name);

            return true;
        }
    }

    return false;
}

bool
jack_controller_load_internal(
    struct jack_controller *controller_ptr,
    const char * internal_name)
{
    jackctl_internal_t *internal;

    internal = jack_controller_find_internal(controller_ptr->server, internal_name);
    if (internal == NULL)
    {
        return false;
    }

    jack_info("internal \"%s\" selected", internal_name);

    return jackctl_server_load_internal(controller_ptr->server, internal);
}

bool
jack_controller_unload_internal(
    struct jack_controller *controller_ptr,
    const char * internal_name)
{
    jackctl_internal_t *internal;

    internal = jack_controller_find_internal(controller_ptr->server, internal_name);
    if (internal == NULL)
    {
        return false;
    }

    jack_info("internal \"%s\" selected", internal_name);

    return jackctl_server_unload_internal(controller_ptr->server, internal);
}

#define controller_ptr ((struct jack_controller *)context)

void
jack_controller_destroy(
        void * context)
{
    if (controller_ptr->pending_save != 0)
    {
        /* settings changed less than 2 s ago (save is delayed): don't lose them on exit */
        controller_ptr->pending_save = 0;
        jack_controller_settings_save_auto(controller_ptr);
    }

    if (controller_ptr->started)
    {
        while (!jack_controller_stop_server(controller_ptr, NULL))
        {
            jack_info("jack server failed to stop, retrying in 3 seconds...");
            usleep(3000000);
        }
    }

    jack_controller_remove_slave_drivers(controller_ptr);
    jackctl_server_set_backend_lost_callback(controller_ptr->server, NULL, NULL);
    if (controller_ptr->snd_watch_fd >= 0)
    {
        close(controller_ptr->snd_watch_fd);
    }
    jack_params_destroy(controller_ptr->params);
    jackctl_server_destroy(controller_ptr->server);
    pthread_mutex_destroy(&controller_ptr->lock);
    free(controller_ptr);
}

void
jack_controller_run(
    void * context)
{
    long ut;

    jack_controller_snd_watch_run(controller_ptr);
    jack_controller_fallback_run(controller_ptr);

    if (controller_ptr->pending_save == 0)
    {
        return;
    }

    if ((ut = uptime()) < 0)
    {
        jack_error(UPTIME_FUNCTION_NAME "() failed with %d", errno);
    }
    else if (ut < controller_ptr->pending_save + 2) /* delay save by two seconds */
    {
        return;
    }

    controller_ptr->pending_save = 0;
    jack_controller_settings_save_auto(controller_ptr);
}

#undef controller_ptr

void
jack_controller_pending_save(
    struct jack_controller * controller_ptr)
{
    long ut;

    if ((ut = uptime()) < 0)
    {
        jack_error(UPTIME_FUNCTION_NAME "() failed with %d.", errno);
        controller_ptr->pending_save = 0;
        jack_controller_settings_save_auto(controller_ptr);
        return;
    }

    controller_ptr->pending_save = ut;
}
