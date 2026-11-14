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

#ifndef __JackSocketNotifyChannel__
#define __JackSocketNotifyChannel__

#include "JackChannel.h"
#include "JackSocket.h"
#include "JackTime.h"

#include <deque>

namespace Jack
{

struct JackClientNotification;

/*!
\brief JackNotifyChannel using sockets.

 Server side of a client's notification socket. The request thread (and the other
 engine-locked callers) must never be held up for long by one client: in 1.9.22 a
 client whose notification thread was busy made every synchronous notification to
 it wait up to 2 s (two latency notifications per graph change: connect, disconnect
 and client open of every other client took that long), and one that stopped reading
 altogether blocked the server for good once the socket buffer was full (writes
 have no timeout). And a reply that came after its 2 s timeout was taken as the
 reply to the next synchronous notification.

 A client "lags" when it did not answer a synchronous notification in time, or its
 socket cannot take one more notification without blocking. While it lags, its
 notifications are queued here, in order, and sent when it catches up (Flush, from
 the request thread); the replies still owed to the server are counted and read as
 they come, never mistaken for later ones. The client gets every notification, in
 order, some later; only a client that does not read for long enough to fill the
 queue (kMaxBacklog) loses notifications, counted and reported.

 Waits: a latency notification waits at most 500 ms for its reply (each client gets
 two after every graph change, in graph order); the others (buffer size, sample rate,
 freewheel, client registration to a new client, activation, session) as long as in
 1.9.22, and for a lagging client the server first waits, within that same time, for
 it to catch up: the client has handled the change before the server goes on, as in
 1.9.22.
*/
class JackSocketNotifyChannel
{

    private:

        JackClientSocket fNotifySocket;    // Socket to communicate with the server : from server to client

        char fName[JACK_CLIENT_NAME_SIZE + 1];   // the client, for messages
        int fSendBuffer;                         // SO_SNDBUF of the socket, 0 if unknown
        bool fLagging;
        int fPendingReplies;                     // synchronous notifications not answered in time
        std::deque<JackClientNotification>* fBacklog;   // (JackRequest.h cannot be included here)
        unsigned int fDelivered;                 // notifications sent late, this lagging episode
        unsigned int fDropped;                   // notifications lost (backlog full), this episode
        jack_time_t fLagSince;
        bool fReported;                          // this episode was reported in the log
        jack_time_t fLastSlowReport;
        bool fClosed;                            // the client closed its end: nothing more is sent

        bool HasRoom();
        bool PeerClosed();
        bool WaitReadable(long usec);
        int Send(JackClientNotification& event);
        void ReadLateReplies();
        void StartLagging(const char* why);
        void Queue(JackClientNotification& event);
        bool CatchUp(jack_time_t deadline);

    public:

        JackSocketNotifyChannel();
        ~JackSocketNotifyChannel();
        JackSocketNotifyChannel(const JackSocketNotifyChannel&) = delete;
        JackSocketNotifyChannel& operator=(const JackSocketNotifyChannel&) = delete;

        int Open(const char* name);		// Open the Server/Client connection
        void Close();					// Close the Server/Client connection

        void ClientNotify(int refnum, const char* name, int notify, int sync, const char* message, int value1, int value2, int* result);

        // Request thread: send what is queued for a lagging client, if it reads again.
        // Returns whether it still lags.
        bool Flush();
        bool IsLagging() const { return fLagging; }
};

} // end of namespace

#endif

