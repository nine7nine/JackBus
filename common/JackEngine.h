/*
Copyright (C) 2004-2008 Grame

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#ifndef __JackEngine__
#define __JackEngine__

#include "JackConstants.h"
#include "JackGraphManager.h"
#include "JackSynchro.h"
#include "JackMutex.h"
#include "JackTransportEngine.h"
#include "JackPlatformPlug.h"
#include "JackRequest.h"
#include "JackChannel.h"
#include "JackBoundedQueue.h"
#include "JackCycleSignal.h"
#ifdef __linux__
#include "JackAliveTable.h"
#endif
#include <utility>
#include <vector>
#include <map>

namespace Jack
{

class JackClientInterface;
struct JackEngineControl;
class JackExternalClient;

/*
 Server-internal notification, from the notifier thread to the request thread over
 the server's own channel (JackServer::Notify): the graph did not finish in some
 cycle, look for clients whose process is exiting or gone. Never sent to clients:
 the value is above every client notification and below kMaxNotification, so it
 fits the request encoding unchanged.
 */
#define kCheckStuckClients (kMaxNotification - 1)

/*!
\brief Carries notifications from the realtime thread to the server request thread.

 The realtime thread used to write each notification (graph reorder, xrun)
 to the server's request socket itself: a system call taking socket locks
 and allocating kernel memory in the realtime thread, on a non-blocking
 socket where a partial write would desynchronize the request stream. Now
 the realtime side only pushes onto a lock-free queue (no lock, no system
 call; dropped and counted if full) and this thread forwards the
 notifications through the same channel, so the request thread receives and
 handles them exactly as before (ordering and threading unchanged).
*/
class JackEngineNotifier : public JackRunnableInterface
{
    private:

        struct Notification {
            int fRefNum;
            int fNotify;
            int fValue;
        };

        JackBoundedQueue<Notification, 256> fQueue;
        JackServerNotifyChannel* fChannel;
        JackThread fThread;
        alignas(SInt32) SInt32 fDropped;
        alignas(SInt32) SInt32 fStuck;    // set by the realtime thread: a cycle found the graph unfinished
        JackCycleSignal fWake;            // the realtime thread cuts the 20 ms sleep short (see MarkStuck)
        jack_time_t fLastWake;            // realtime thread only

    #ifdef __linux__
        // Clients' realtime threads (JackAliveTable): the notifier also waits on the
        // slots of the clients in fAliveWatch (set by the request thread), and has the
        // request thread check a client whose thread ended (FUTEX_OWNER_DIED)
        JackAliveTable* fAliveTable;
        const uint64_t* fAliveWatch;
        uint64_t fAliveReported[(CLIENT_NUM + 63) / 64];  // notifier thread only
        bool fWaitV;                                      // futex_waitv available
        unsigned int WatchSlots(struct futex_waitv* waiters, unsigned int max, bool* ended);
    #endif

        void Drain();

    public:

        JackEngineNotifier(JackServerNotifyChannel* channel);

        int Start();
        void Stop();

        // Realtime thread.
        void Notify(int refnum, int notify, int value);
        /*
         A cycle found the graph unfinished: the request thread should look for clients
         whose process died (JackEngine::CheckStuckClients). On Linux, the first such
         cycle also wakes the notifier thread instead of letting it finish its 20 ms
         sleep, so a dead client is found within a cycle or two: one FUTEX_WAKE at most
         every 100 ms, only in cycles that already failed.
         */
        // Request thread: look for dead clients again at the next tick (20 ms)
        void RequestCheck() { __atomic_store_n(&fStuck, 1, __ATOMIC_RELAXED); }
        // Request thread: the set of watched clients changed
        void WakeUp() { fWake.Signal(); }
    #ifdef __linux__
        void SetAliveTable(JackAliveTable* table, const uint64_t* watch);
    #endif

        void MarkStuck(jack_time_t now)
        {
            __atomic_store_n(&fStuck, 1, __ATOMIC_RELAXED);
        #ifdef __linux__
            if (now - fLastWake > 100000) {
                fLastWake = now;
                fWake.Signal();
            }
        #else
            (void)now;
        #endif
        }

        // JackRunnableInterface (notifier thread).
        bool Execute();
};

/*!
\brief Engine description.
*/

class SERVER_EXPORT JackEngine : public JackLockAble
{
    friend class JackLockedEngine;

    private:

        JackGraphManager* fGraphManager;
        JackEngineControl* fEngineControl;
        char fSelfConnectMode;
        JackClientInterface* fClientTable[CLIENT_NUM];
        JackSynchro* fSynchroTable;
        JackServerNotifyChannel fChannel;              /*! Engine -> server request thread */
        JackEngineNotifier fNotifier;                  /*! RT thread -> fChannel, without system calls */
        JackCycleSignal fSignal;                       /*! "a cycle has passed", signalled by the RT thread */
        jack_time_t fLastSwitchUsecs;
        // 1 + highest refnum in fClientTable (written by the server thread,
        // read by the RT thread): per-cycle loops over clients stop there
        int fRefNumBound;
        void UpdateRefNumBound();
        // Each external client's process as seen from the server: pid in the server's
        // pid namespace (SO_PEERCRED on its request socket; 0 = unknown, no checks) and
        // the process start time, which tells it from a later process reusing the pid.
        // fPeerNextCheck: earliest time to look again at a client found alive.
        int fPeerPid[CLIENT_NUM];
        unsigned long long fPeerStart[CLIENT_NUM];
        jack_time_t fPeerNextCheck[CLIENT_NUM];
    #ifdef __linux__
        // Slots of the clients' realtime threads (see JackAliveTable)
        JackAliveTable fAliveTable;
    #endif
        // Active external clients whose slot the notifier watches (request thread
        // writes, notifier thread reads)
        alignas(8) uint64_t fAliveWatch[(CLIENT_NUM + 63) / 64];
        // Taken out of the graph while still registered: their realtime thread ended
        // (DetachThreadEndedClient), or a session manager is about to stop their
        // process (DetachClients). Removed when their process is found exiting or
        // gone, or their socket closes; back in the graph if they activate again.
        bool fDetached[CLIENT_NUM];
        jack_time_t fDetachedSince[CLIENT_NUM];
        void WatchClient(int refnum, bool on);
        bool IsWatched(int refnum) const;
        void DetachThreadEndedClient(int refnum);
        JackMetadata fMetadata;

        int fSessionPendingReplies;
        detail::JackChannelTransactionInterface* fSessionTransaction;
        JackSessionNotifyResult* fSessionResult;
        std::map<int,std::string> fReservationMap;

        int ClientCloseAux(int refnum, bool wait);
        void DetachDeadClient(int refnum);
        void DisconnectAllDeferred(jack_port_id_t port, std::vector<std::pair<jack_port_id_t, jack_port_id_t> >& removed);
        void CheckXRun(jack_time_t callback_usecs);

        int NotifyAddClient(JackClientInterface* new_client, const char* new_name, int refnum);
        void NotifyRemoveClient(const char* name, int refnum);

        void ProcessNext(jack_time_t callback_usecs);
        void ProcessCurrent(jack_time_t callback_usecs);

        bool ClientCheckName(const char* name);
        bool GenerateUniqueName(char* name);

        int AllocateRefnum();
        void ReleaseRefnum(int refnum);

        int ClientNotify(JackClientInterface* client, int refnum, const char* name, int notify, int sync, const char* message, int value1, int value2);

        void NotifyClient(int refnum, int event, int sync, const char*  message, int value1, int value2);
        void NotifyClients(int event, int sync, const char*  message,  int value1, int value2);

        void NotifyPortRegistation(jack_port_id_t port_index, bool onoff);
        void NotifyPortConnect(jack_port_id_t src, jack_port_id_t dst, bool onoff);
        void NotifyPortRename(jack_port_id_t src, const char* old_name);
        void NotifyActivate(int refnum);

        void EnsureUUID(jack_uuid_t uuid);

        bool CheckClient(int refnum)
        {
            return (refnum >= 0 && refnum < CLIENT_NUM && fClientTable[refnum] != NULL);
        }

        int CheckPortsConnect(int refnum, jack_port_id_t src, jack_port_id_t dst);

    public:

        JackEngine(JackGraphManager* manager, JackSynchro* table, JackEngineControl* controler, char self_connect_mode);
        ~JackEngine();

        int Open();
        int Close();

        // Client management
        int ClientCheck(const char* name, jack_uuid_t uuid, char* name_res, int protocol, int options, int* status);

        int ClientExternalOpen(const char* name, int pid, jack_uuid_t uuid, int* ref, int* shared_engine, int* shared_client, int* shared_graph_manager);
        int ClientInternalOpen(const char* name, int* ref, JackEngineControl** shared_engine, JackGraphManager** shared_manager, JackClientInterface* client, bool wait);

        int ClientExternalClose(int refnum);
        int ClientInternalClose(int refnum, bool wait);

        int ClientActivate(int refnum, bool is_real_time, bool wait);
        int ClientDeactivate(int refnum, bool wait);

        void ClientKill(int refnum);
        bool IsClientDead(int refnum);
        void ClientSetPeerPid(int refnum, int pid, int uid);
        void CheckStuckClients();
        int FlushNotifications();
        int DetachClients(const char* client_name, int pid);

        int GetClientPID(const char* name);
        int GetClientRefNum(const char* name);

        // Internal client management
        int GetInternalClientName(int int_ref, char* name_res);
        int InternalClientHandle(const char* client_name, int* status, int* int_ref);
        int InternalClientUnload(int refnum, int* status);

        // Port management
        int PortRegister(int refnum, const char* name, const char *type, unsigned int flags, unsigned int buffer_size, jack_port_id_t* port);
        int PortUnRegister(int refnum, jack_port_id_t port);

        int PortConnect(int refnum, const char* src, const char* dst);
        int PortDisconnect(int refnum, const char* src, const char* dst);

        int PortConnect(int refnum, jack_port_id_t src, jack_port_id_t dst);
        int PortDisconnect(int refnum, jack_port_id_t src, jack_port_id_t dst);

        int PortRename(int refnum, jack_port_id_t port, const char* name);

        int PortSetDefaultMetadata(jack_port_id_t port, const char* pretty_name);

        int ComputeTotalLatencies();

        int PropertyChangeNotify(jack_uuid_t subject, const char* key,jack_property_change_t change);

        // Graph
        bool Process(jack_time_t cur_cycle_begin, jack_time_t prev_cycle_end);

        // Notifications
        void NotifyDriverXRun();
        void NotifyClientXRun(int refnum);
        void NotifyFailure(int code, const char* reason);
        void NotifyGraphReorder();
        void NotifyBufferSize(jack_nframes_t buffer_size);
        void NotifySampleRate(jack_nframes_t sample_rate);
        void NotifyFreewheel(bool onoff);
        void NotifyQuit();

        // Session management
        void SessionNotify(int refnum, const char *target, jack_session_event_type_t type, const char *path, detail::JackChannelTransactionInterface *socket, JackSessionNotifyResult** result);
        int SessionReply(int refnum);

        int GetUUIDForClientName(const char *client_name, char *uuid_res);
        int GetClientNameForUUID(const char *uuid, char *name_res);
        int ReserveClientName(const char *name, const char *uuid);
        int ClientHasSessionCallback(const char *name);
};


} // end of namespace

#endif

