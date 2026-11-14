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

#include "JackRequest.h"
#include "JackSocketNotifyChannel.h"
#include "JackError.h"
#include "JackConstants.h"
#include "JackNotification.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#ifdef __linux__
#include <linux/sockios.h>
#endif

namespace Jack
{

static const size_t kMaxBacklog = 4096;             // notifications queued for one lagging client (~1.5 MB)
static const long kLatencyReplyUsecs = 500000;      // see the class comment
static const long kSlowReplyUsecs = 100000;         // answers slower than this are reported...
static const jack_time_t kSlowReportInterval = 10000000;    // ...at most every 10 s per client
static const jack_time_t kLagReportDelay = 2000000; // a full socket is reported after 2 s

static const char* NotificationName(int notify)
{
    switch (notify) {
        case kAddClient: return "client registration";
        case kActivateClient: return "activation";
        case kBufferSizeCallback: return "buffer size";
        case kSampleRateCallback: return "sample rate";
        case kStartFreewheelCallback:
        case kStopFreewheelCallback: return "freewheel";
        case kSessionCallback: return "session";
        case kLatencyCallback: return "latency";
        default: return "notification";
    }
}

JackSocketNotifyChannel::JackSocketNotifyChannel()
    : fSendBuffer(0), fLagging(false), fPendingReplies(0), fBacklog(new std::deque<JackClientNotification>()),
      fDelivered(0), fDropped(0), fLagSince(0), fReported(false), fLastSlowReport(0), fClosed(false)
{
    memset(fName, 0, sizeof(fName));
}

JackSocketNotifyChannel::~JackSocketNotifyChannel()
{
    delete fBacklog;
}

// Server to client
int JackSocketNotifyChannel::Open(const char* name)
{
    jack_log("JackSocketNotifyChannel::Open name = %s", name);

    // Connect to client listen socket
    if (fNotifySocket.Connect(jack_client_dir, name, 0) < 0) {
        jack_error("Cannot connect client socket");
        return -1;
    }

    // Use a time out for notifications
    fNotifySocket.SetReadTimeOut(SOCKET_TIME_OUT);

    strncpy(fName, name, sizeof(fName) - 1);
    socklen_t len = sizeof(fSendBuffer);
    if (getsockopt(fNotifySocket.GetFd(), SOL_SOCKET, SO_SNDBUF, &fSendBuffer, &len) < 0) {
        fSendBuffer = 0;
    }
    return 0;
}

void JackSocketNotifyChannel::Close()
{
    jack_log("JackSocketNotifyChannel::Close");
    fNotifySocket.Close();
    fBacklog->clear();
    fLagging = false;
    fPendingReplies = 0;
}

/*
 Can the socket take one more notification without blocking? A stream socket's send
 buffer holds what the client has not read yet (SIOCOUTQ: its size in the same units
 as SO_SNDBUF, with the kernel's overhead per message); a write blocks when it is
 full. A notification takes well under 8 KB of it. Elsewhere, or if unknown: as in
 1.9.22.
 */
bool JackSocketNotifyChannel::HasRoom()
{
#ifdef __linux__
    int queued = 0;
    if (fSendBuffer <= 0 || ioctl(fNotifySocket.GetFd(), SIOCOUTQ, &queued) < 0) {
        return true;
    }
    return queued + 8192 <= fSendBuffer;
#else
    return true;
#endif
}

/*
 Has the client closed its end of the socket? Then nothing more is sent to it: every
 write would fail, and be logged. It happens each time the server stops (clients
 leave on its quit notification, before the server has removed its own drivers and
 ports, which it announces to every client: 20 to 60 "Broken pipe" lines per stop
 with a few clients connected), and when a client dies, until the request thread
 removes it.
 */
bool JackSocketNotifyChannel::PeerClosed()
{
    if (!fClosed && fNotifySocket.PeerClosed()) {
        fClosed = true;
        fBacklog->clear();
        fPendingReplies = 0;
        fLagging = false;
        jack_log("JackSocketNotifyChannel: client %s closed its notification socket, nothing more is sent to it", fName);
    }
    return fClosed;
}

bool JackSocketNotifyChannel::WaitReadable(long usec)
{
    struct pollfd pfd;
    pfd.fd = fNotifySocket.GetFd();
    pfd.events = POLLIN;
    pfd.revents = 0;
    jack_time_t deadline = GetMicroSeconds() + usec;
    for (;;) {
        long left = (long)(deadline - GetMicroSeconds());
        int res = poll(&pfd, 1, (left > 0) ? (int)((left + 999) / 1000) : 0);
        if (res > 0) {
            return true;    // a reply, or the client went away (the read then fails)
        }
        if (res == 0 || errno != EINTR) {
            return false;
        }
    }
}

int JackSocketNotifyChannel::Send(JackClientNotification& event)
{
    return event.Write(&fNotifySocket);
}

// Replies owed for notifications that were not answered in time: read and drop the
// ones already there, without waiting
void JackSocketNotifyChannel::ReadLateReplies()
{
    while (fPendingReplies > 0 && WaitReadable(0)) {
        JackResult res;
        if (res.Read(&fNotifySocket) < 0) {
            fPendingReplies = 0;    // the client went away: nothing more will come
            break;
        }
        fPendingReplies--;
    }
}

void JackSocketNotifyChannel::StartLagging(const char* why)
{
    if (!fLagging) {
        fLagging = true;
        fLagSince = GetMicroSeconds();
        fDelivered = 0;
        fDropped = 0;
        fReported = false;
    }
    if (why && !fReported) {
        jack_error("client %s %s: not waiting for it until it catches up (its notifications are queued)", fName, why);
        fReported = true;
    }
}

void JackSocketNotifyChannel::Queue(JackClientNotification& event)
{
    if (fBacklog->size() >= kMaxBacklog) {
        fDropped++;
        return;
    }
    event.fSync = 0;    // nobody waits for this one: no reply
    fBacklog->push_back(event);
}

bool JackSocketNotifyChannel::Flush()
{
    if (!fLagging || PeerClosed()) {
        return false;
    }
    ReadLateReplies();
    while (!fBacklog->empty() && HasRoom()) {
        if (Send(fBacklog->front()) < 0) {
            fBacklog->clear();   // the client went away: it is being removed
            break;
        }
        fBacklog->pop_front();
        fDelivered++;
    }
    if (fPendingReplies == 0 && fBacklog->empty()) {
        fLagging = false;
        if (fDropped > 0) {
            // Notifications were lost: a graph-order callback makes clients that keep
            // their own view of the graph (patchbays) read it again
            JackClientNotification reorder(fName, -1, kGraphOrderCallback, 0, "", 0, 0);
            Send(reorder);
        }
        if (fReported) {
            if (fDropped > 0) {
                jack_info("client %s caught up: %u notification(s) sent late, %u lost (queue full)", fName, fDelivered, fDropped);
            } else {
                jack_info("client %s caught up: %u notification(s) sent late", fName, fDelivered);
            }
        }
    } else if (!fReported && GetMicroSeconds() - fLagSince > kLagReportDelay) {
        StartLagging("has not read its notifications for 2 s");
    }
    return fLagging;
}

// Wait, until deadline, for a lagging client to read what is queued and answer what
// it owes
bool JackSocketNotifyChannel::CatchUp(jack_time_t deadline)
{
    while (Flush()) {
        long left = (long)(deadline - GetMicroSeconds());
        if (left <= 0) {
            return false;
        }
        struct pollfd pfd;
        pfd.fd = fNotifySocket.GetFd();
        pfd.events = POLLIN | (fBacklog->empty() ? 0 : POLLOUT);
        pfd.revents = 0;
        long ms = (left + 999) / 1000;
        poll(&pfd, 1, (ms > 10) ? 10 : (int)ms);
    }
    return true;
}

void JackSocketNotifyChannel::ClientNotify(int refnum, const char* name, int notify, int sync, const char* message, int value1, int value2, int* result)
{
    JackClientNotification event(name, refnum, notify, sync, message, value1, value2);
    jack_time_t deadline = 0;
    *result = 0;

    if (PeerClosed()) {
        return;     // the client is gone: as for a dead client (JackExternalClient), not an error
    }

    if (fLagging && sync && notify != kLatencyCallback) {
        // The client must have handled this before the server goes on: wait for it to
        // catch up, then for its answer, together no longer than 1.9.22 waited for the
        // answer alone
        deadline = GetMicroSeconds() + SOCKET_TIME_OUT * 1000000LL;
        CatchUp(deadline);
    } else {
        Flush();
    }

    if (!fLagging && !HasRoom()) {
        StartLagging(NULL);     // reported if it lasts (Flush)
    }
    if (fLagging) {
        Queue(event);
        // A latency notification is delivered later, in order: not a failure (the lag
        // itself is reported once). Another synchronous one could not be waited for.
        *result = (sync && notify != kLatencyCallback) ? -1 : 0;
        return;
    }

    // Send notification
    if (Send(event) < 0) {
        if (PeerClosed()) {
            return;     // it closed its end just now
        }
        jack_error("Could not write notification");
        *result = -1;
        return;
    }

    // Read the result in "synchronous" mode only
    if (sync) {
        jack_time_t start = GetMicroSeconds();
        long budget = (notify == kLatencyCallback) ? kLatencyReplyUsecs : SOCKET_TIME_OUT * 1000000L;
        if (deadline != 0) {
            budget = (deadline > start) ? (long)(deadline - start) : 0;
        }
        JackResult res;
        if (WaitReadable(budget) && res.Read(&fNotifySocket) == 0) {
            *result = res.fResult;
            jack_time_t took = GetMicroSeconds() - start;
            if (took > kSlowReplyUsecs && start - fLastSlowReport > kSlowReportInterval) {
                fLastSlowReport = start;
                jack_info("client %s took %ld ms to handle a %s notification (requests from every client waited)",
                          fName, long(took / 1000), NotificationName(notify));
            }
        } else {
            fPendingReplies++;  // its reply may still come: read it then (ReadLateReplies)
            char why[96];
            snprintf(why, sizeof(why), "did not answer a %s notification within %ld ms", NotificationName(notify), budget / 1000);
            StartLagging(why);
            *result = (notify == kLatencyCallback) ? 0 : -1;    // as above
        }
    }
}

} // end of namespace
