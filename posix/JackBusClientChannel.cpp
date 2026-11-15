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

#include "JackBusClientChannel.h"
#include "JackRequest.h"
#include "JackClient.h"
#include "JackGlobals.h"
#include "JackError.h"
#include "JackConstants.h"
#include "JackNotification.h"
#include "JackFdPassing.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

namespace Jack
{

JackBusClientChannel::JackBusClientChannel()
    : JackGenericClientChannel(), fNotificationSocket(NULL), fPeerEnd(-1), fThread(this), fClient(NULL), fNotifyFd(-1)
{
    fBusRequest = new JackClientSocket();
    fRequest = fBusRequest;
}

JackBusClientChannel::~JackBusClientChannel()
{
    if (fPeerEnd >= 0) {
        close(fPeerEnd);
    }
    delete fBusRequest;
    delete fNotificationSocket;
}

int JackBusClientChannel::Open(const char* server_name, const char* name, jack_uuid_t uuid, char* name_res, JackClient* client, jack_options_t options, jack_status_t* status)
{
    int result = 0;
    char path[PATH_MAX];
    int sv[2];
    jack_log("JackBusClientChannel::Open name = %s", name);

    // Before any server/client call
    fClient = client;

    if (JackBusSocketPath(server_name, path, sizeof(path), false) < 0) {
        jack_error("JackBus socket: no usable $XDG_RUNTIME_DIR/jackbus");
        return -1;
    }
    if (fBusRequest->ConnectPath(path) < 0) {
        jack_error("Cannot connect to the JackBus socket %s", path);
        return -1;
    }

    // OK so server is there...
    __atomic_store_n(&JackGlobals::fServerRunning, true, __ATOMIC_RELAXED);

    {
        JackBusHelloRequest req(JACKBUS_API_VERSION, 0);
        JackBusHelloResult res;
        ServerSyncCall(&req, &res, &result);
        if (result < 0) {
            jack_error("JackBus socket: the server speaks version %d, this libjack %d", res.fVersion, JACKBUS_API_VERSION);
            goto error;
        }
    }

    // The name check, as on the protocol-9 socket (the open is kBusClientOpen)
    ClientCheck(name, uuid, name_res, JACK_PROTOCOL_VERSION, (int)options, (int*)status, &result, false);
    if (result < 0) {
        int status1 = *status;
        if (status1 & JackVersionError) {
            jack_error("JACK protocol mismatch %d", JACK_PROTOCOL_VERSION);
        } else {
            jack_error("Client name = %s conflits with another running client", name);
        }
        goto error;
    }

    // The notification socket: a pair, no name
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        jack_error("JackBus socket: socketpair: %s", strerror(errno));
        goto error;
    }
    fNotificationSocket = new JackClientSocket(sv[0]);
    fPeerEnd = sv[1];
    return 0;

error:
    fBusRequest->Close();
    return -1;
}

void JackBusClientChannel::Close()
{
    fBusRequest->Close();
    if (fNotificationSocket) {
        fNotificationSocket->Close();
    }
    if (fPeerEnd >= 0) {
        close(fPeerEnd);
        fPeerEnd = -1;
    }
}

int JackBusClientChannel::Start()
{
    jack_log("JackBusClientChannel::Start");
    // The notification thread runs before the open: the server sends the new
    // client one synchronous kAddClient per existing client during it
    if (fThread.StartSync() != 0) {
        jack_error("Cannot start Jack client listener");
        return -1;
    }
    return 0;
}

void JackBusClientChannel::Stop()
{
    jack_log("JackBusClientChannel::Stop");
    fThread.Kill();
}

void JackBusClientChannel::BusClientOpen(const char* name, int pid, jack_uuid_t uuid, int* shared_engine, int* shared_client,
                                         int* shared_graph, int* refnum, int* fds, int* result)
{
    *result = -1;
    for (int i = 0; i < kBusOpenFds; i++) {
        fds[i] = -1;
    }
    if (JackGlobals::fNotificationThread) {
        jack_error("Cannot callback the server in notification thread!");
        return;
    }
    if (fPeerEnd < 0 || !fNotificationSocket) {
        return;
    }

    JackNoCancelScope no_cancel;
    fRequestLock.Lock();

    JackBusClientOpenRequest req(name, pid, uuid);
    JackBusClientOpenResult res;
    if (req.Write(fRequest) < 0 || JackSendFds(fBusRequest->GetFd(), &fPeerEnd, 1) < 0) {
        jack_error("Could not write request type = %ld", req.fType);
    } else {
        close(fPeerEnd);    // the server holds its own copy now
        fPeerEnd = -1;
        if (res.Read(fRequest) < 0) {
            jack_error("Could not read result type = %ld", req.fType);
        } else if (res.fResult < 0) {
            *result = res.fResult;
        } else if (JackRecvFds(fBusRequest->GetFd(), fds, kBusOpenFds) != kBusOpenFds) {
            jack_error("JackBus socket: the open's fds did not come");
        } else {
            *shared_engine = res.fSharedEngine;
            *shared_client = res.fSharedClient;
            *shared_graph = res.fSharedGraph;
            *refnum = res.fRefNum;
            *result = 0;
        }
    }

    fRequestLock.Unlock();
}

int JackBusClientChannel::TakeNotifyFd()
{
    int fd = fNotifyFd;
    fNotifyFd = -1;
    return fd;
}

bool JackBusClientChannel::Init()
{
    jack_log("JackBusClientChannel::Init");
    // Setup context
    JackGlobals::fNotificationThread = this;
    return true;
}

bool JackBusClientChannel::Execute()
{
    JackClientNotification event;
    JackResult res;

    int err = event.Read(fNotificationSocket);
    if (err < 0) {
        // aborted reading due to shutdown
        if (err != JACK_REQUEST_ERR_ABORTED) {
            jack_error("JackBusClientChannel read fail");
        }
        goto error;
    }

    if (event.fNotify == kAddClient) {
        int fd = -1;
        int n = JackRecvFds(fNotificationSocket->GetFd(), &fd, 1);
        if (n < 0) {
            jack_error("JackBus socket: a client registration without its futex trailer");
            goto error;
        }
        fNotifyFd = (n == 1) ? fd : -1;
    }

    res.fResult = fClient->ClientNotify(event.fRefNum, event.fName, event.fNotify, event.fSync, event.fMessage, event.fValue1, event.fValue2);

    if (fNotifyFd >= 0) {
        close(fNotifyFd);   // not taken
        fNotifyFd = -1;
    }

    if (event.fSync) {
        if (res.Write(fNotificationSocket) < 0) {
            jack_error("JackBusClientChannel write fail");
            goto error;
        }
    }
    return true;

error:
    fNotificationSocket->Close();
    fClient->ShutDown(jack_status_t(JackFailure | JackServerError), JACK_SERVER_FAILURE);
    return false;
}

} // end of namespace
