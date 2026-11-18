/*
Copyright (C) 2004-2008 Grame

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

#include "JackSocketServerChannel.h"
#include "JackRequest.h"
#include "JackServer.h"
#include "JackLockedEngine.h"
#include "JackGlobals.h"
#include "JackClient.h"
#include "JackTools.h"
#include "JackNotification.h"
#include "JackException.h"
#include "JackConstants.h"
#ifdef __linux__
#include "JackFdPassing.h"
#endif

#include <assert.h>
#include <vector>
#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>
#include <limits.h>

using namespace std;

namespace Jack
{

JackSocketServerChannel::JackSocketServerChannel():
    fThread(this), fDecoder(NULL)
{
    fPollTable = NULL;
    fRebuild = true;
    fSweepDead = false;
    fLagging = false;
}

JackSocketServerChannel::~JackSocketServerChannel()
{
    delete[] fPollTable;
}

int JackSocketServerChannel::Open(const char* server_name, JackServer* server)
{
    jack_log("JackSocketServerChannel::Open");
  
    // Prepare request socket
    if (fRequestListenSocket.Bind(jack_server_dir, server_name, 0) < 0) {
        jack_log("JackSocketServerChannel::Open : cannot create result listen socket");
        return -1;
    }

#ifdef __linux__
    // The JackBus socket, beside the protocol-9 one: without it (no XDG_RUNTIME_DIR,
    // or a directory that isn't safe), JackBus socket clients can't connect; nothing
    // else changes
    char bus_path[PATH_MAX];
    if (getenv("JACK_NO_BUS_SOCKET") == NULL
        && JackBusSocketPath(server_name, bus_path, sizeof(bus_path), true) == 0
        && fBusListenSocket.BindPath(bus_path) == 0) {
        jack_log("JackSocketServerChannel::Open: JackBus socket %s", bus_path);
    }
#endif

    // Prepare for poll
    BuildPoolTable();
    
    fDecoder = new JackRequestDecoder(server, this);
    fServer = server;
    return 0;
}

void JackSocketServerChannel::Close()
{
   fRequestListenSocket.Close();
   fBusListenSocket.Close();    // removes its path
   fBusSockets.clear();

    // Close remaining client sockets
    std::map<int, std::pair<int, JackClientSocket*> >::iterator it;

    for (it = fSocketTable.begin(); it != fSocketTable.end(); it++) {
        pair<int, JackClientSocket*> elem = (*it).second;
        JackClientSocket* socket = elem.second;
        assert(socket);
        socket->Close();
        delete socket;
    }

    delete fDecoder;
    fDecoder = NULL;
}

int JackSocketServerChannel::Start()
{
    if (fThread.Start() != 0) {
        jack_error("Cannot start Jack server listener");
        return -1;
    } else {
        return 0;
    }
}

void JackSocketServerChannel::Stop()
{
    fThread.Stop();
}

void JackSocketServerChannel::ClientCreate()
{
    jack_log("JackSocketServerChannel::ClientCreate socket");
    JackClientSocket* socket = fRequestListenSocket.Accept();
    if (socket) {
        fSocketTable[socket->GetFd()] = make_pair(-1, socket);
        fRebuild = true;
    } else {
        jack_error("Client socket cannot be created");
    }
}

/*
 A client of the JackBus socket: only this user's processes (the directory is
 0700 too); it must send kBusHello first.
 */
void JackSocketServerChannel::BusClientCreate()
{
    JackClientSocket* socket = fBusListenSocket.Accept();
    if (!socket) {
        jack_error("JackBus socket: client socket cannot be created");
        return;
    }
#ifdef __linux__
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(socket->GetFd(), SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0 || cred.uid != getuid()) {
        jack_error("JackBus socket: refused a client of another user");
        socket->Close();
        delete socket;
        return;
    }
#endif
    // A request that stops half-way must not hold the request thread (and every
    // other client) for good: reads give up after SOCKET_TIME_OUT, and the client
    // is dropped
    socket->SetReadTimeOut(SOCKET_TIME_OUT);
    fSocketTable[socket->GetFd()] = make_pair(-1, socket);
    fBusSockets[socket->GetFd()] = JackRequest::kBusHello;  // what it must send next
    fRebuild = true;
}

/*
 Requests only JackBus socket clients send. Returns -1 to drop the client (a
 protocol error: the stream can't be trusted after it).
 */
int JackSocketServerChannel::HandleBusRequest(JackClientSocket* socket, int fd, int type)
{
    int& state = fBusSockets[fd];
    switch (type) {
        case JackRequest::kBusHello: {
            JackBusHelloRequest req;
            JackBusHelloResult res;
            if (state != JackRequest::kBusHello || req.Read(socket) < 0) {
                return -1;
            }
            res.fVersion = JACKBUS_API_VERSION;
#ifdef __linux__
            res.fFeatures = JACKBUS_FEATURE_ENDPOINTS;
#else
            res.fFeatures = 0;
#endif
            res.fResult = (req.fVersion == JACKBUS_API_VERSION) ? 0 : -1;
            if (res.Write(socket) < 0) {
                return -1;
            }
            state = (res.fResult == 0) ? JackRequest::kBusClientOpen : JackRequest::kBusHello;
            return 0;
        }
        case JackRequest::kClientCheck: {
            // the name check before the open, as on the protocol-9 socket, but without
            // fOpen: the open that follows must be kBusClientOpen (the decoder would
            // take a protocol-9 kClientOpen after it)
            JackClientCheckRequest req;
            JackClientCheckResult res;
            if (state != JackRequest::kBusClientOpen || req.Read(socket) < 0 || req.fOpen) {
                return -1;
            }
            res.fResult = fServer->GetEngine()->ClientCheck(req.fName, req.fUUID, res.fName, req.fProtocol, req.fOptions, &res.fStatus);
            return (res.Write(socket) < 0) ? -1 : 0;
        }
        case JackRequest::kBusClientOpen:
            if (state != JackRequest::kBusClientOpen) {
                return -1;
            }
            return BusClientOpen(socket, fd);
#ifdef __linux__
        case JackRequest::kBusEndpointOpen: {
            // after the open, for the client's own refnum only
            JackBusEndpointOpenRequest req;
            JackBusEndpointOpenResult res;
            if (state != -1 || req.Read(socket) < 0 || req.fRefNum != fSocketTable[fd].first) {
                return -1;
            }
            static_assert(JACKBUS_BUS_ENDPOINT_PORTS == JACKBUS_ENDPOINT_MAX_CHANNELS, "endpoint ports");
            int mem = -1;
            jack_port_id_t ports[JACKBUS_BUS_ENDPOINT_PORTS];
            int count = (req.fCount >= 1 && req.fCount <= JACKBUS_BUS_ENDPOINT_PORTS) ? req.fCount : 0;
            for (int i = 0; i < count; i++) {
                ports[i] = (jack_port_id_t)req.fPorts[i];
            }
            int id = count ? fServer->GetEngine()->EndpointOpen(req.fRefNum, ports, count, req.fRingFrames, &mem) : -EINVAL;
            res.fResult = (id >= 0) ? 0 : id;
            res.fId = id;
            int err = res.Write(socket);
            if (err == 0 && mem >= 0) {
                err = JackSendFds(fd, &mem, 1);
            }
            if (mem >= 0) {
                close(mem);
            }
            if (err < 0 && id >= 0) {
                fServer->GetEngine()->EndpointClose(req.fRefNum, id);
            }
            return (err < 0) ? -1 : 0;
        }
        case JackRequest::kBusEndpointClose:
        case JackRequest::kBusClientPriority: {
            JackBusRefNumRequest req((JackRequest::RequestType)type);
            JackResult res;
            if (state != -1 || req.Read(socket) < 0 || req.fRefNum != fSocketTable[fd].first) {
                return -1;
            }
            res.fResult = (type == JackRequest::kBusEndpointClose)
                ? fServer->GetEngine()->EndpointClose(req.fRefNum, req.fValue)
                : fServer->GetEngine()->ClientPriority(req.fRefNum, req.fValue != 0);
            return (res.Write(socket) < 0) ? -1 : 0;
        }
#endif
        default:
            jack_error("JackBus socket: unknown request %d", type);
            return -1;
    }
}

int JackSocketServerChannel::BusClientOpen(JackClientSocket* socket, int fd)
{
#ifdef __linux__
    JackBusClientOpenRequest req;
    JackBusClientOpenResult res;
    if (req.Read(socket) < 0) {
        return -1;
    }
    int notify_fd = -1;
    if (JackRecvFds(fd, &notify_fd, 1) != 1) {
        jack_error("JackBus socket: open without a notification socket");
        return -1;
    }

    // The process is the socket's peer, as the kernel sees it (the pid a sandboxed
    // client reports is one of its own namespace)
    struct ucred cred;
    socklen_t len = sizeof(cred);
    memset(&cred, 0, sizeof(cred));
    int pid = req.fPID;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.pid > 0) {
        pid = cred.pid;
    }

    int refnum = -1;
    res.fResult = fServer->GetEngine()->ClientExternalOpen(req.fName, pid, req.fUUID, &refnum,
                    &res.fSharedEngine, &res.fSharedClient, &res.fSharedGraph, notify_fd);
    int fds[kBusOpenFds];
    if (res.fResult == 0) {
        if (fServer->GetEngine()->ClientBusFds(refnum, fds) != kBusOpenFds) {
            fServer->GetEngine()->ClientExternalClose(refnum);
            res.fResult = -1;
        } else {
            res.fRefNum = refnum;
            fSocketTable[fd].first = refnum;
            fRebuild = true;
            if (cred.pid > 0) {
                fServer->GetEngine()->ClientSetPeerPid(refnum, cred.pid, cred.uid);
            }
            jack_log("JackSocketServerChannel::BusClientOpen ref = %d fd = %d", refnum, fd);
        }
    } else {
        jack_error("Cannot create new client");
    }

    int err = res.Write(socket);
    if (res.fResult == 0) {
        if (err == 0) {
            err = JackSendFds(fd, fds, kBusOpenFds);
        }
        for (int i = 0; i < kBusOpenFds; i++) {
            close(fds[i]);
        }
        fBusSockets[fd] = -1;   // opened: from now on, the requests of protocol 9
    }
    return (err < 0) ? -1 : 0;
#else
    return -1;
#endif
}

int JackSocketServerChannel::GetFd(JackClientSocket* socket_aux)
{
    std::map<int, std::pair<int, JackClientSocket*> >::iterator it;

    for (it = fSocketTable.begin(); it != fSocketTable.end(); it++) {
        pair<int, JackClientSocket*> elem = (*it).second;
        JackClientSocket* socket = elem.second;
        if (socket_aux == socket) {
            return (*it).first;
        }
    }
    return -1;
}

void JackSocketServerChannel::ClientAdd(detail::JackChannelTransactionInterface* socket_aux, JackClientOpenRequest* req, JackClientOpenResult *res)
{
    int refnum = -1;
    res->fResult = fServer->GetEngine()->ClientExternalOpen(req->fName, req->fPID, req->fUUID, &refnum, &res->fSharedEngine, &res->fSharedClient, &res->fSharedGraph);
    if (res->fResult == 0) {
        JackClientSocket* socket = dynamic_cast<JackClientSocket*>(socket_aux);
        assert(socket);
        int fd = GetFd(socket);
        assert(fd >= 0);
        fSocketTable[fd].first = refnum;
        fRebuild = true;
        jack_log("JackSocketServerChannel::ClientAdd ref = %d fd = %d", refnum, fd);
    #ifdef __linux__
        // The client's process as the kernel sees it from here (req->fPID is the
        // client's own getpid(), wrong across pid namespaces, e.g. Flatpak)
        struct ucred cred;
        socklen_t len = sizeof(cred);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.pid > 0) {
            fServer->GetEngine()->ClientSetPeerPid(refnum, cred.pid, cred.uid);
        }
    #endif
    #ifdef __APPLE__
        int on = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&on, sizeof(on)) < 0) {
            jack_log("JackSocketServerChannel::ClientAdd : setsockopt SO_NOSIGPIPE fd = %ld err = %s", fd, strerror(errno));
        }
    #endif
    } else {
        jack_error("Cannot create new client");
    }
}

void JackSocketServerChannel::ClientRemove(detail::JackChannelTransactionInterface* socket_aux, int refnum)
{
    JackClientSocket* socket = dynamic_cast<JackClientSocket*>(socket_aux);
    assert(socket);
    int fd = GetFd(socket);
    assert(fd >= 0);

    jack_log("JackSocketServerChannel::ClientRemove ref = %d fd = %d", refnum, fd);
    fSocketTable.erase(fd);
    fBusSockets.erase(fd);
    socket->Close();
    delete socket;
    fRebuild = true;
}

void JackSocketServerChannel::ClientKill(int fd)
{
    pair<int, JackClientSocket*> elem = fSocketTable[fd];
    JackClientSocket* socket = elem.second;
    int refnum = elem.first;
    assert(socket);
    
    jack_log("JackSocketServerChannel::ClientKill ref = %d fd = %d", refnum, fd);
    if (refnum == -1) {  // Should never happen... correspond to a client that started the socket but never opened...
        jack_log("Client was not opened : probably correspond to server_check");
    } else {
        fServer->GetEngine()->ClientKill(refnum);
    }
   
    fSocketTable.erase(fd);
    fBusSockets.erase(fd);
    socket->Close();
    delete socket;
    fRebuild = true;
}

/*
 Clients whose process the engine found exiting or gone are already out of the graph
 (JackEngine::DetachDeadClient). Their request socket may stay open for a long time:
 the kernel closes a dying process' files only after tearing down its memory, and a
 child it spawned may hold a copy of the socket for as long as it runs. Remove them now
 as if the socket had closed (ports, client registration, notifications to the others).
 Called after the pass over the poll table, never during it.
 */
void JackSocketServerChannel::KillDeadClients()
{
    fSweepDead = false;
    std::vector<int> dead;
    std::map<int, std::pair<int, JackClientSocket*> >::iterator it;
    for (it = fSocketTable.begin(); it != fSocketTable.end(); it++) {
        if (it->second.first >= 0 && fServer->GetEngine()->IsClientDead(it->second.first)) {
            dead.push_back(it->first);
        }
    }
    for (size_t i = 0; i < dead.size(); i++) {
        jack_log("JackSocketServerChannel::KillDeadClients fd = %d", dead[i]);
        ClientKill(dead[i]);
    }
}

void JackSocketServerChannel::BuildPoolTable()
{
    if (fRebuild) {
        fRebuild = false;
        delete[] fPollTable;
        fPollTable = new pollfd[fSocketTable.size() + 2];

        jack_log("JackSocketServerChannel::BuildPoolTable size = %d", fSocketTable.size() + 2);

        // First fd is the server request socket
        fPollTable[0].fd = fRequestListenSocket.GetFd();
        fPollTable[0].events = POLLIN | POLLERR;
        fPollTable[1].fd = fBusListenSocket.GetFd();    // -1 (ignored by poll) without it
        fPollTable[1].events = POLLIN | POLLERR;
        fPollTable[1].revents = 0;

        // Next fd for clients
        map<int, pair<int, JackClientSocket*> >::iterator it;
        int i;

        for (i = 2, it = fSocketTable.begin(); it != fSocketTable.end(); it++, i++) {
            jack_log("JackSocketServerChannel::BuildPoolTable fSocketTable i = %ld fd = %ld", i, it->first);
            fPollTable[i].fd = it->first;
            fPollTable[i].events = POLLIN | POLLPRI | POLLERR | POLLHUP | POLLNVAL;
        }
    }
}

bool JackSocketServerChannel::Init()
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, 0);
    return true;
}

bool JackSocketServerChannel::Execute()
{
    try {

        // Global poll
        // While a client lags, look every 50 ms whether it reads again
        if ((poll(fPollTable, fSocketTable.size() + 2, fLagging ? 50 : 10000) < 0) && (errno != EINTR)) {
            jack_error("JackSocketServerChannel::Execute : engine poll failed err = %s request thread quits...", strerror(errno));
            return false;
        } else {

            // Poll all clients
            for (unsigned int i = 2; i < fSocketTable.size() + 2; i++) {
                int fd = fPollTable[i].fd;
                if (fPollTable[i].revents & ~POLLIN) {
                    jack_log("JackSocketServerChannel::Execute : poll client error err = %s", strerror(errno));
                    ClientKill(fd);
                } else if (fPollTable[i].revents & POLLIN) {
                    JackClientSocket* socket = fSocketTable[fd].second;
                    // Decode header
                    JackRequestHeader header;
                    if (header.Read(socket) < 0) {
                        jack_log("JackSocketServerChannel::Execute : cannot decode header");
                        ClientKill(fd);
                    // Decode request
                    } else if (fBusSockets.count(fd) == 0) {
                        // Result is not needed here
                        fDecoder->HandleRequest(socket, header.fType);
                    } else if (header.fType >= JackRequest::kBusHello || fBusSockets[fd] >= 0) {
                        // JackBus socket: its own requests (and the name check), which
                        // must come before any other
                        if (HandleBusRequest(socket, fd, header.fType) < 0) {
                            ClientKill(fd);
                        }
                    } else if (header.fType == JackRequest::kClientOpen || header.fType == JackRequest::kClientCheck) {
                        // a JackBus socket client opens once, with kBusClientOpen
                        jack_error("JackBus socket: request %d refused after the open", header.fType);
                        ClientKill(fd);
                    } else if (fDecoder->HandleRequest(socket, header.fType, fSocketTable[fd].first) == -2) {
                        ClientKill(fd);     // a request it may not send
                    }
                }
            }

            if (fSweepDead) {
                KillDeadClients();
            }

            fLagging = (fServer->GetEngine()->FlushNotifications() > 0);

            // Check the server request socket */
            if (fPollTable[0].revents & POLLERR) {
                jack_error("Error on server request socket err = %s", strerror(errno));
            }

            if (fPollTable[0].revents & POLLIN) {
                ClientCreate();
            }

            if (fPollTable[1].revents & POLLIN) {
                BusClientCreate();
            }
        }

        BuildPoolTable();
        return true;

    } catch (JackQuitException& e) {
        jack_log("JackSocketServerChannel::Execute : JackQuitException");
        return false;
    }
}

} // end of namespace


