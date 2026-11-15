/*
    Copyright (C) 2009 Grame
    
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

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <pthread.h>

#include "reserve.h"
#include "audio_reserve.h"
#include "jack/control.h"

/*
 The cards jackd holds (org.freedesktop.ReserveDevice1.Audio<n>): the master's, its slaves',
 and the sound card bridges' (added and removed while the server runs, from the server's bridge
 monitor thread as well as its control thread, hence the lock). A name asked for twice (a bridge
 on another device of the master's card) is held once and counted: released when the last one
 lets go. Free slots are reused (the old table wrote slot [count] even when it was in use and
 never freed one).
*/
#define DEVICE_MAX 20

typedef struct reserved_audio_device {

     char device_name[64];
     rd_device * reserved_device;
     int count;

} reserved_audio_device;

static DBusConnection* gConnection = NULL;
static reserved_audio_device gReservedDevice[DEVICE_MAX];
static int gReserveCount = 0;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;

SERVER_EXPORT int audio_reservation_init()
{
    DBusError error;
    dbus_error_init(&error);

    if (!(gConnection = dbus_bus_get(DBUS_BUS_SESSION, &error))) {
        jack_error("Failed to connect to session bus for device reservation: %s\n", error.message);
        jack_info("To bypass device reservation via session bus, set JACK_NO_AUDIO_RESERVATION=1 prior to starting jackd.\n");
        dbus_error_free(&error);
        return -1;
    }

    jack_info("audio_reservation_init");
    return 0;
}

SERVER_EXPORT int audio_reservation_finish()
{
    if (gConnection) {
        dbus_connection_unref(gConnection);
        gConnection = NULL;
        jack_info("audio_reservation_finish");
    }
    return 0;
}

SERVER_EXPORT bool audio_acquire(const char * device_name)
{
    DBusError error;
    int ret, i, slot = -1;

    pthread_mutex_lock(&gLock);
    for (i = 0; i < DEVICE_MAX; i++) {
        if (gReservedDevice[i].reserved_device != NULL && strcmp(gReservedDevice[i].device_name, device_name) == 0) {
            gReservedDevice[i].count++;
            pthread_mutex_unlock(&gLock);
            return true;
        }
        if (slot < 0 && gReservedDevice[i].reserved_device == NULL) {
            slot = i;
        }
    }
    if (slot < 0) {
        jack_error("Cannot reserve audio card %s: already holding %d cards", device_name, DEVICE_MAX);
        pthread_mutex_unlock(&gLock);
        return false;
    }

    // Open DBus connection first time
    if (gReserveCount == 0 && gConnection == NULL) {
        if (audio_reservation_init() != 0) {
            pthread_mutex_unlock(&gLock);
            return false;
        }
    }

    dbus_error_init(&error);

    if ((ret= rd_acquire(
                 &gReservedDevice[slot].reserved_device,
                 gConnection,
                 device_name,
                 "Jack audio server",
                 INT32_MAX,
                 NULL,
                 &error)) < 0) {

        jack_error("Failed to acquire device name : %s error : %s", device_name, (error.message ? error.message : strerror(-ret)));
        dbus_error_free(&error);
        gReservedDevice[slot].reserved_device = NULL;
        if (gReserveCount == 0)
            audio_reservation_finish();
        pthread_mutex_unlock(&gLock);
        return false;
    }

    snprintf(gReservedDevice[slot].device_name, sizeof(gReservedDevice[slot].device_name), "%s", device_name);
    gReservedDevice[slot].count = 1;
    gReserveCount++;
    jack_info("Acquire audio card %s", device_name);
    pthread_mutex_unlock(&gLock);
    return true;
}

SERVER_EXPORT void audio_release(const char * device_name)
{
    int i;

    pthread_mutex_lock(&gLock);
    // Look for corresponding reserved device
    for (i = 0; i < DEVICE_MAX; i++) {
        if (gReservedDevice[i].reserved_device != NULL && strcmp(gReservedDevice[i].device_name, device_name) == 0)
            break;
    }

    if (i == DEVICE_MAX) {
        jack_error("Audio card %s not found!!", device_name);
        pthread_mutex_unlock(&gLock);
        return;
    }
    if (--gReservedDevice[i].count > 0) {
        pthread_mutex_unlock(&gLock);
        return;
    }
    jack_info("Released audio card %s", device_name);
    rd_release(gReservedDevice[i].reserved_device);
    gReservedDevice[i].reserved_device = NULL;
    gReservedDevice[i].device_name[0] = 0;

    // Close DBus connection last time
    gReserveCount--;
    if (gReserveCount == 0)
        audio_reservation_finish();
    pthread_mutex_unlock(&gLock);
}

SERVER_EXPORT void audio_reserve_loop()
{
    DBusConnection* connection;

    // a reference, so that a release meanwhile (the last one closes the connection) cannot free it
    pthread_mutex_lock(&gLock);
    connection = gConnection ? dbus_connection_ref(gConnection) : NULL;
    pthread_mutex_unlock(&gLock);
    if (connection != NULL) {
       dbus_connection_read_write_dispatch (connection, 200);
       dbus_connection_unref(connection);
    } else {
       usleep(200000);   // nothing held: not a busy loop
    }
}
