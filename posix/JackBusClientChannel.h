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

#ifndef __JackBusClientChannel__
#define __JackBusClientChannel__

#include "JackGenericClientChannel.h"
#include "JackSocket.h"
#include "JackPlatformPlug.h"
#include "JackThread.h"

namespace Jack
{

class JackClient;

/*!
\brief The client side of the JackBus socket ($XDG_RUNTIME_DIR/jackbus/<server>).

 Requests after the open are protocol 9's, unchanged (JackGenericClientChannel).
 What differs is how the client gets hold of the server's objects: instead of
 opening them by name in /dev/shm, it receives them as fds.
 - The notification socket is one end of a socket pair created here; the other
   end goes to the server with the open (kBusClientOpen), so the server never
   connects to a name of the client's.
 - The open's result carries the fds of the engine and graph segments, the
   client's control segment, its futex file and the alive table.
 - Each kAddClient notification carries the new peer's futex file.
*/

class JackBusClientChannel : public JackGenericClientChannel, public JackRunnableInterface
{

    private:

        JackClientSocket* fBusRequest;          // = fRequest
        JackClientSocket* fNotificationSocket;  // this end of the pair
        int fPeerEnd;                           // the server's end, until the open sends it
        JackThread fThread;                     // notifications
        JackClient* fClient;
        int fNotifyFd;                          // notification thread: the fd of the kAddClient being handled

    public:

        JackBusClientChannel();
        virtual ~JackBusClientChannel();

        // connect, kBusHello, the name check
        int Open(const char* server_name, const char* name, jack_uuid_t uuid, char* name_res, JackClient* client, jack_options_t options, jack_status_t* status);
        void Close();

        int Start();
        void Stop();

        // the open: fds[] receives kBusOpenFds fds (kBusFd* order) if *result is 0
        void BusClientOpen(const char* name, int pid, jack_uuid_t uuid, int* shared_engine, int* shared_client,
                           int* shared_graph, int* refnum, int* fds, int* result);

        // notification thread, while handling kAddClient: the peer's futex file
        // (the caller owns it), -1 if none came
        int TakeNotifyFd();

        // JackRunnableInterface interface
        bool Init();
        bool Execute();

        bool IsChannelThread() { return fThread.IsThread(); }
};

} // end of namespace

#endif
