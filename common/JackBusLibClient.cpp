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

#include "JackBusLibClient.h"
#include "JackBusClientChannel.h"
#include "JackLibGlobals.h"
#include "JackGlobals.h"
#include "JackRequest.h"
#include "JackNotification.h"
#include "JackTools.h"
#include "JackError.h"

#include <unistd.h>

namespace Jack
{

JackBusLibClient::JackBusLibClient(JackSynchro* table): JackLibClient(table)
{
    jack_log("JackBusLibClient::JackBusLibClient table = %x", table);
    delete fChannel;    // JackLibClient's protocol-9 channel
    fBusChannel = new JackBusClientChannel();
    fChannel = fBusChannel;
}

JackBusLibClient::~JackBusLibClient()
{
    jack_log("JackBusLibClient::~JackBusLibClient");
    // fChannel (fBusChannel) is deleted by ~JackLibClient
}

static void CloseFds(int* fds, int n)
{
    for (int i = 0; i < n; i++) {
        if (fds[i] >= 0) {
            close(fds[i]);
            fds[i] = -1;
        }
    }
}

int JackBusLibClient::Open(const char* server_name, const char* name, jack_uuid_t uuid, jack_options_t options, jack_status_t* status)
{
    int shared_engine = -1, shared_client = -1, shared_graph = -1, refnum = -1, result = -1;
    int fds[kBusOpenFds];
    char name_res[JACK_CLIENT_NAME_SIZE + 1];
    bool res;

    for (int i = 0; i < kBusOpenFds; i++) {
        fds[i] = -1;
    }
    jack_log("JackBusLibClient::Open name = %s", name);

    if (strlen(name) >= JACK_CLIENT_NAME_SIZE) {
        jack_error("\"%s\" is too long to be used as a JACK client name.\n"
                   "Please use %lu characters or less",
                   name,
                   JACK_CLIENT_NAME_SIZE - 1);
        return -1;
    }

    strncpy(fServerName, server_name, sizeof(fServerName));
    fServerName[sizeof(fServerName) - 1] = 0;

    // Connect, version, name check
    if (fBusChannel->Open(server_name, name, uuid, name_res, this, options, status) < 0) {
        jack_error("Cannot connect to the server's JackBus socket");
        goto error;
    }

    // Start receiving notifications: the open below gets one per existing client
    if (fBusChannel->Start() < 0) {
        jack_error("Cannot start channel");
        goto error;
    }

    fBusChannel->BusClientOpen(name_res, JackTools::GetPID(), uuid, &shared_engine, &shared_client, &shared_graph, &refnum, fds, &result);
    if (result < 0) {
        jack_error("Cannot open %s client", name_res);
        goto error;
    }

    // Map the segments from their fds (each SetShmFd owns its fd). A process
    // whose protocol-9 clients mapped them already keeps that mapping.
    res = JackLibGlobals::fGlobals->fEngineControl.SetShmFd(fds[kBusFdEngine], shared_engine);
    fds[kBusFdEngine] = -1;
    if (!res) {
        jack_error("JackBus socket: cannot map the engine segment");
        goto error;
    }
    res = JackLibGlobals::fGlobals->fGraphManager.SetShmFd(fds[kBusFdGraph], shared_graph);
    fds[kBusFdGraph] = -1;
    if (!res) {
        jack_error("JackBus socket: cannot map the graph segment");
        goto error;
    }
    res = fClientControl.SetShmFd(fds[kBusFdClient], shared_client);
    fds[kBusFdClient] = -1;
    if (!res || GetClientControl()->fRefNum != refnum) {
        jack_error("JackBus socket: cannot map the client segment");
        goto error;
    }
    __atomic_store_n(&JackGlobals::fVerbose, GetEngineControl()->fVerbose, __ATOMIC_RELAXED);

    SetupDriverSync(false);

    // This client's futex: the synchro table is the process' own, shared with its
    // other clients (protocol 9 or not)
    assert(JackGlobals::fSynchroMutex);
    JackGlobals::fSynchroMutex->Lock();
    res = fSynchroTable[refnum].ConnectFd(fds[kBusFdFutex]);
    JackGlobals::fSynchroMutex->Unlock();
    fds[kBusFdFutex] = -1;
    if (!res) {
        jack_error("Cannot ConnectSemaphore %s client", name_res);
        goto error;
    }

    // The slot its realtime thread holds: required on this socket
    fAlive = JackLibGlobals::fGlobals->AliveTableFd(fServerName, fds[kBusFdAlive]);
    fds[kBusFdAlive] = -1;
    if (!fAlive || !fAlive->InitSlot(refnum)) {
        jack_error("JackBus socket: no slot in the server's alive table for %s", name_res);
        fAlive = NULL;
        goto error;
    }

    JackGlobals::fClientTable[refnum] = this;
    SetClockSource(GetEngineControl()->fClockSource);
    jack_log("JackBusLibClient::Open name = %s refnum = %ld", name_res, refnum);
    return 0;

error:
    CloseFds(fds, kBusOpenFds);
    fChannel->Stop();
    fChannel->Close();
    return -1;
}

int JackBusLibClient::ClientNotifyImp(int refnum, const char* name, int notify, int sync, const char* message, int value1, int value2)
{
    if (notify != kAddClient) {
        return JackLibClient::ClientNotifyImp(refnum, name, notify, sync, message, value1, value2);
    }

    // As JackLibClient's, with the futex file that came with the notification
    int fd = fBusChannel->TakeNotifyFd();
    JackNoCancelScope no_cancel;
    assert(JackGlobals::fSynchroMutex);
    JackGlobals::fSynchroMutex->Lock();
    jack_log("JackBusLibClient::AddClient name = %s, ref = %ld fd = %d", name, refnum, fd);
    int res = fSynchroTable[refnum].ConnectFd(fd) ? 0 : -1;
    JackGlobals::fSynchroMutex->Unlock();
    if (res < 0) {
        jack_error("JackBus socket: cannot connect the futex of client %s", name);
    }
    return res;
}

} // end of namespace
