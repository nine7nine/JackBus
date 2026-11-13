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

#include <iostream>
#include <fstream>
#include <set>
#include <vector>
#include <utility>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#include "JackSystemDeps.h"
#include "JackLockedEngine.h"
#include "JackExternalClient.h"
#include "JackInternalClient.h"
#include "JackEngineControl.h"
#include "JackClientControl.h"
#include "JackServerGlobals.h"
#include "JackGlobals.h"
#include "JackChannel.h"
#include "JackError.h"
#include "JackAtomic.h"

extern const char* JACK_METADATA_HARDWARE;
extern const char* JACK_METADATA_PRETTY_NAME;

namespace Jack
{

JackEngine::JackEngine(JackGraphManager* manager,
                       JackSynchro* table,
                       JackEngineControl* control,
                       char self_connect_mode)
                    : JackLockAble(control->fServerName),
                    fNotifier(&fChannel),
                    fMetadata(true)
{
    fGraphManager = manager;
    fSynchroTable = table;
    fEngineControl = control;
    fSelfConnectMode = self_connect_mode;
    for (int i = 0; i < CLIENT_NUM; i++) {
        fClientTable[i] = NULL;
        fPeerPid[i] = 0;
        fPeerStart[i] = 0;
        fPeerNextCheck[i] = 0;
        fThreadDead[i] = false;
        fThreadDeadSince[i] = 0;
    }
    memset(fAliveWatch, 0, sizeof(fAliveWatch));
    fRefNumBound = 0;
    fLastSwitchUsecs = 0;
    fSessionPendingReplies = 0;
    fSessionTransaction = NULL;
    fSessionResult = NULL;
}

JackEngine::~JackEngine()
{}

int JackEngine::Open()
{
    jack_log("JackEngine::Open");

    // Open audio thread => request thread communication channel
    if (fChannel.Open(fEngineControl->fServerName) < 0) {
        jack_error("Cannot connect to server");
        return -1;
    }
#ifdef __linux__
    // Without it (e.g. /dev/shm full), dead clients are found the way they were
    // before: when the graph does not finish, and when their socket closes
    if (fAliveTable.Allocate(fEngineControl->fServerName)) {
        fNotifier.SetAliveTable(&fAliveTable, fAliveWatch);
    }
#endif
    if (fNotifier.Start() < 0) {
        jack_error("Cannot start the engine notification thread");
        fChannel.Close();
        return -1;
    }
    return 0;
}

int JackEngine::Close()
{
    jack_log("JackEngine::Close");
    fNotifier.Stop();
#ifdef __linux__
    fAliveTable.Destroy();
#endif
    fChannel.Close();

    // Close remaining clients (RT is stopped)
    for (int i = fEngineControl->fDriverNum; i < CLIENT_NUM; i++) {
        if (JackLoadableInternalClient* loadable_client = dynamic_cast<JackLoadableInternalClient*>(fClientTable[i])) {
            jack_log("JackEngine::Close loadable client = %s", loadable_client->GetClientControl()->fName);
            loadable_client->Close();
            fClientTable[i] = NULL;
            delete loadable_client;
        } else if (JackExternalClient* external_client = dynamic_cast<JackExternalClient*>(fClientTable[i])) {
            jack_log("JackEngine::Close external client = %s", external_client->GetClientControl()->fName);
            external_client->Close();
            // the client process may outlive the server: remove its futex
            // name, so that it is not left in /dev/shm and found again by the
            // next server
            fSynchroTable[i].Destroy();
            fClientTable[i] = NULL;
            delete external_client;
        }
    }
    UpdateRefNumBound();

    return 0;
}

void JackEngine::NotifyQuit()
{
    fChannel.NotifyQuit();
}


//-----------------------------
// RT notifications
//-----------------------------

JackEngineNotifier::JackEngineNotifier(JackServerNotifyChannel* channel)
    : fChannel(channel), fThread(this), fDropped(0), fStuck(0), fLastWake(0)
#ifdef __linux__
    , fAliveTable(NULL), fAliveWatch(NULL), fWaitV(true)
#endif
{
#ifdef __linux__
    memset(fAliveReported, 0, sizeof(fAliveReported));
    // tests: behave as on a kernel without futex_waitv (Linux < 5.16)
    fWaitV = (getenv("JACK_NO_FUTEX_WAITV") == NULL);
#endif
}

#ifdef __linux__
// Before Start(): the notifier then also waits on the watched clients' slots
void JackEngineNotifier::SetAliveTable(JackAliveTable* table, const uint64_t* watch)
{
    fAliveTable = table;
    fAliveWatch = watch;
}

/*
 Notifier thread. For each watched client (active, external) whose realtime thread
 holds its slot: makes sure the slot's word has FUTEX_WAITERS, so that the kernel
 wakes us when the owner dies (handle_futex_death only wakes when it is set; the
 owner never unlocks: it holds the lock for its lifetime), and adds it to waiters.
 A slot marked FUTEX_OWNER_DIED is reported once (*ended), and not waited on (its
 value no longer changes: futex_waitv would return at once). Slots that are not
 locked are left alone: a libjack without slots never locks them.
 */
unsigned int JackEngineNotifier::WatchSlots(struct futex_waitv* waiters, unsigned int max, bool* ended)
{
    unsigned int count = 0;
    for (int w = 0; w < (CLIENT_NUM + 63) / 64; w++) {
        uint64_t bits = __atomic_load_n(&fAliveWatch[w], __ATOMIC_ACQUIRE);
        fAliveReported[w] &= bits;  // forget clients no longer watched
        while (bits) {
            int bit = __builtin_ctzll(bits);
            bits &= bits - 1;
            int refnum = w * 64 + bit;
            uint32_t* word = fAliveTable->Word(refnum);
            if (!word) {
                continue;
            }
            uint32_t val = __atomic_load_n(word, __ATOMIC_ACQUIRE);
            if (val & FUTEX_OWNER_DIED) {
                if (!(fAliveReported[w] & (1ULL << bit))) {
                    fAliveReported[w] |= (1ULL << bit);
                    *ended = true;
                }
                continue;
            }
            if ((val & FUTEX_TID_MASK) == 0) {
                continue;   // not locked
            }
            if (!(val & FUTEX_WAITERS)) {
                uint32_t want = val | FUTEX_WAITERS;
                if (!__atomic_compare_exchange_n(word, &val, want, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                    continue;   // changed meanwhile: looked at again next time
                }
                val = want;
            }
            if (waiters && count < max) {
                waiters[count].val = val;
                waiters[count].uaddr = (uintptr_t)word;
                waiters[count].flags = FUTEX_32;    // process-shared
                waiters[count].__reserved = 0;
                count++;
            }
        }
    }
    return count;
}
#endif

int JackEngineNotifier::Start()
{
    return fThread.Start();
}

void JackEngineNotifier::Stop()
{
    fThread.Stop();
    Drain();
    if (fDropped > 0) {
        jack_error("JackEngineNotifier: %d notifications dropped (queue full)", fDropped);
    }
}

void JackEngineNotifier::Notify(int refnum, int notify, int value)
{
    bool queued = fQueue.TryPush([&](Notification& n) {
        n.fRefNum = refnum;
        n.fNotify = notify;
        n.fValue = value;
    });
    if (!queued) {
        INC_ATOMIC(&fDropped);
    }
}

void JackEngineNotifier::Drain()
{
    while (fQueue.TryPop([this](Notification& n) { fChannel->Notify(n.fRefNum, n.fNotify, n.fValue); })) {}
}

bool JackEngineNotifier::Execute()
{
    // Poll: notifications are asynchronous (graph order, xrun callbacks), 20 ms of
    // extra delay is harmless. The realtime side wakes us only when the graph did
    // not finish in a cycle (MarkStuck), at most every 100 ms. With a JackAliveTable
    // we also wake as soon as a watched client's realtime thread ends.
    bool ended = false;
#ifdef __linux__
    bool waited = false;
  #if defined(SYS_futex_waitv) && defined(FUTEX_WAITV_MAX)
    if (fAliveTable && fWaitV) {
        struct futex_waitv waiters[FUTEX_WAITV_MAX];
        unsigned int count = WatchSlots(waiters + 1, FUTEX_WAITV_MAX - 1, &ended);
        if (ended) {
            waited = true;      // report at once
        } else if (fWake.TimedWaitV(20000, waiters, count) < 0) {
            jack_log("JackEngineNotifier: futex_waitv not available, clients' realtime threads are looked at every 20 ms");
            fWaitV = false;
        } else {
            waited = true;
        }
    }
  #endif
    if (!waited) {
        fWake.TimedWait(20000);
    }
    if (fAliveTable && !ended) {
        WatchSlots(NULL, 0, &ended);    // what woke us (or, without futex_waitv, what changed)
    }
#else
    JackSleep(20000);
#endif
    Drain();
    if (__atomic_exchange_n(&fStuck, 0, __ATOMIC_RELAXED) || ended) {
        fChannel->Notify(ALL_CLIENTS, kCheckStuckClients, 0);
    }
    return true;
}

//-----------------------------
// Client resource management
//-----------------------------

int JackEngine::AllocateRefnum()
{
    for (int i = 0; i < CLIENT_NUM; i++) {
        if (!fClientTable[i]) {
            jack_log("JackEngine::AllocateRefNum ref = %ld", i);
            return i;
        }
    }
    return -1;
}

void JackEngine::UpdateRefNumBound()
{
    int bound = 0;
    for (int i = 0; i < CLIENT_NUM; i++) {
        if (fClientTable[i]) {
            bound = i + 1;
        }
    }
    // a new client is entered before it can be activated, so the RT thread
    // sees the raised bound before the client is in the graph
    __atomic_store_n(&fRefNumBound, bound, __ATOMIC_RELEASE);
}

void JackEngine::ReleaseRefnum(int refnum)
{
    WatchClient(refnum, false);
    fThreadDead[refnum] = false;
#ifdef __linux__
    fAliveTable.ClearSlot(refnum);
#endif
    fClientTable[refnum] = NULL;
    fPeerPid[refnum] = 0;
    fPeerStart[refnum] = 0;
    fPeerNextCheck[refnum] = 0;
    UpdateRefNumBound();

    if (fEngineControl->fTemporary) {
        int i;
        for (i = fEngineControl->fDriverNum; i < CLIENT_NUM; i++) {
            if (fClientTable[i]) {
                break;
            }
        }
        if (i == CLIENT_NUM) {
            // Last client and temporary case: quit the server
            jack_log("JackEngine::ReleaseRefnum server quit");
            fEngineControl->fTemporary = false;
            throw JackTemporaryException();
        }
    }
}

//------------------
// Graph management
//------------------

void JackEngine::ProcessNext(jack_time_t cur_cycle_begin)
{
    __atomic_store_n(&fLastSwitchUsecs, cur_cycle_begin, __ATOMIC_RELAXED);
    if (fGraphManager->RunNextGraph(__atomic_load_n(&fRefNumBound, __ATOMIC_ACQUIRE)))  {   // True if the graph actually switched to a new state
        fNotifier.Notify(ALL_CLIENTS, kGraphOrderCallback, 0);
    }
    fSignal.Signal();                       // Signal for threads waiting for next cycle
}

void JackEngine::ProcessCurrent(jack_time_t cur_cycle_begin)
{
    if (cur_cycle_begin < __atomic_load_n(&fLastSwitchUsecs, __ATOMIC_RELAXED) + 2 * fEngineControl->fPeriodUsecs) { // Signal XRun only for the first failing cycle
        CheckXRun(cur_cycle_begin);
    }
    fGraphManager->RunCurrentGraph(__atomic_load_n(&fRefNumBound, __ATOMIC_ACQUIRE));
}

bool JackEngine::Process(jack_time_t cur_cycle_begin, jack_time_t prev_cycle_end)
{
    bool res = true;

    // Cycle  begin
    fEngineControl->CycleBegin(fClientTable, fGraphManager, cur_cycle_begin, prev_cycle_end,
                               __atomic_load_n(&fRefNumBound, __ATOMIC_ACQUIRE));

    // Graph
    if (fGraphManager->IsFinishedGraph()) {
        ProcessNext(cur_cycle_begin);
        res = true;
    } else {
        jack_log("Process: graph not finished!");
        fNotifier.MarkStuck(cur_cycle_begin);  // the notifier thread asks the request thread to look
        jack_time_t last_switch = __atomic_load_n(&fLastSwitchUsecs, __ATOMIC_RELAXED);  // also reset by the request thread
        if (cur_cycle_begin > last_switch + fEngineControl->fTimeOutUsecs) {
            jack_log("Process: switch to next state delta = %ld", long(cur_cycle_begin - last_switch));
            ProcessNext(cur_cycle_begin);
            res = true;
        } else {
            jack_log("Process: waiting to switch delta = %ld", long(cur_cycle_begin - last_switch));
            ProcessCurrent(cur_cycle_begin);
            res = false;
        }
    }

    // Cycle end
    fEngineControl->CycleEnd(fClientTable);
    return res;
}

/*
Client that finish *after* the callback date are considered late even if their output buffers may have been
correctly mixed in the time window: callbackUsecs <==> Read <==> Write.
*/

static const char* State2String(jack_client_state_t state)
{
    switch (state) {
        case NotTriggered:
            return "NotTriggered";
        case Triggered:
            return "Triggered";
        case Running:
            return "Running";
        case Finished:
            return "Finished";
        default:
            return "";
    }
}

/*
 Realtime thread, first failing cycle(s) after a graph switch. One failing cycle is one
 xrun: 1.9.22 sent one xrun notification per client found unfinished, so a client that
 died or hung in the middle of a chain counted as several xruns (it, and each client
 the engine had already triggered behind it), each with its own log line. Now: one
 notification, one line naming the clients (state, and how long since each was
 triggered), and jack_get_xrun_delayed_usecs() reports this xrun instead of whatever
 the driver measured for its last one: the longest time an unfinished client had been
 in its cycle, or how late a client finished after the cycle's deadline.
 */
void JackEngine::CheckXRun(jack_time_t callback_usecs)  // REVOIR les conditions de fin
{
    const int bound = __atomic_load_n(&fRefNumBound, __ATOMIC_ACQUIRE);
    char clients[256];
    int len = 0;
    int named = 0;
    int unnamed = 0;
    jack_time_t delay = 0;

    clients[0] = 0;
    for (int i = fEngineControl->fDriverNum; i < bound; i++) {
        JackClientInterface* client = fClientTable[i];
        if (client && client->GetClientControl()->fActive) {
            JackClientTiming* timing = fGraphManager->GetClientTiming(i);
            jack_client_state_t status = timing->fStatus;
            jack_time_t signaled_date = timing->fSignaledAt;
            jack_time_t finished_date = timing->fFinishedAt;
            jack_time_t late;

            if (status != NotTriggered && status != Finished) {
                late = (callback_usecs > signaled_date) ? callback_usecs - signaled_date : 0;
            } else if (status == Finished && (long)(finished_date - callback_usecs) > 0) {
                late = finished_date - callback_usecs;
            } else {
                continue;
            }

            if (late > delay) {
                delay = late;
            }
            if (named < 4 && len < (int)sizeof(clients)) {
                int n = snprintf(clients + len, sizeof(clients) - len, "%s%s (%s, %ld us)", named ? ", " : "",
                                 client->GetClientControl()->fName,
                                 (status == Finished) ? "finished late" : State2String(status), long(late));
                len += (n > 0) ? n : 0;
                named++;
            } else {
                unnamed++;
            }
        }
    }

    if (named > 0) {
        if (unnamed > 0) {
            jack_error("JackEngine::XRun: not finished in time: %s and %d more", clients, unnamed);
        } else {
            jack_error("JackEngine::XRun: not finished in time: %s", clients);
        }
        fEngineControl->fXrunDelayedUsecs = float(delay);
        fNotifier.Notify(ALL_CLIENTS, kXRunCallback, 0);  // Notify all clients
    }
}

int JackEngine::ComputeTotalLatencies()
{
    std::vector<jack_int_t> sorted;
    std::vector<jack_int_t>::iterator it;
    std::vector<jack_int_t>::reverse_iterator rit;

    fGraphManager->TopologicalSort(sorted);

    /* iterate over all clients in graph order, and emit
     * capture latency callback.
     */

    for (it = sorted.begin(); it != sorted.end(); it++) {
        NotifyClient(*it, kLatencyCallback, true, "", 0, 0);
    }

    /* now issue playback latency callbacks in reverse graph order.
     */
    for (rit = sorted.rbegin(); rit != sorted.rend(); rit++) {
        NotifyClient(*rit, kLatencyCallback, true, "", 1, 0);
    }

    return 0;
}

//--------------
// Metadata API
//--------------

int JackEngine::PropertyChangeNotify(jack_uuid_t subject, const char* key, jack_property_change_t change)
{
    jack_log("JackEngine::PropertyChangeNotify: subject = %x key = %s change = %x", subject, key, change);

    for (int i = 0; i < CLIENT_NUM; i++) {
        JackClientInterface* client = fClientTable[i];
        if (client) {
            char buf[JACK_UUID_STRING_SIZE];
            jack_uuid_unparse(subject, buf);
            client->ClientNotify(i, buf, kPropertyChangeCallback, false, key, change, 0);
        }
    }

    return 0;
}

//---------------
// Notifications
//---------------

int JackEngine::ClientNotify(JackClientInterface* client, int refnum, const char* name, int notify, int sync, const char* message, int value1, int value2)
{
    // Check if notification is needed
    if (!client->GetClientControl()->fCallback[notify]) {
        jack_log("JackEngine::ClientNotify: no callback for notification = %ld", notify);
        return 0;
    }

    int res1;

    // External client
    if (dynamic_cast<JackExternalClient*>(client)) {
       res1 = client->ClientNotify(refnum, name, notify, sync, message, value1, value2);
    // Important for internal client : unlock before calling the notification callbacks
    } else {
        bool res2 = Unlock();
        res1 = client->ClientNotify(refnum, name, notify, sync, message, value1, value2);
        if (res2) {
            Lock();
        }
    }

    if (res1 < 0) {
        jack_error("ClientNotify fails name = %s notification = %ld val1 = %ld val2 = %ld", name, notify, value1, value2);
    }
    return res1;
}

void JackEngine::NotifyClient(int refnum, int event, int sync, const char* message, int value1, int value2)
{
    JackClientInterface* client = fClientTable[refnum];
    if (client) {
        ClientNotify(client, refnum, client->GetClientControl()->fName, event, sync, message, value1, value2);
    }
}

void JackEngine::NotifyClients(int event, int sync, const char* message, int value1, int value2)
{
    for (int i = 0; i < CLIENT_NUM; i++) {
        NotifyClient(i, event, sync, message, value1, value2);
    }
}

int JackEngine::NotifyAddClient(JackClientInterface* new_client, const char* new_name, int refnum)
{
    jack_log("JackEngine::NotifyAddClient: name = %s", new_name);

    // Notify existing clients of the new client and new client of existing clients.
    for (int i = 0; i < CLIENT_NUM; i++) {
        JackClientInterface* old_client = fClientTable[i];
        if (old_client && old_client != new_client) {
            char* old_name = old_client->GetClientControl()->fName;
            if (ClientNotify(old_client, refnum, new_name, kAddClient, false, "", 0, 0) < 0) {
                jack_error("NotifyAddClient old_client fails name = %s", old_name);
                // Not considered as a failure...
            }
            if (ClientNotify(new_client, i, old_name, kAddClient, true, "", 0, 0) < 0) {
                jack_error("NotifyAddClient new_client fails name = %s", new_name);
                return -1;
            }
        }
    }

    return 0;
}

void JackEngine::NotifyRemoveClient(const char* name, int refnum)
{
    // Notify existing clients (including the one being suppressed) of the removed client
    for (int i = 0; i < CLIENT_NUM; i++) {
        JackClientInterface* client = fClientTable[i];
        if (client) {
            ClientNotify(client, refnum, name, kRemoveClient, false, "", 0, 0);
        }
    }
}

// Coming from the driver
void JackEngine::NotifyDriverXRun()
{
    // Use the audio thread => request thread communication channel
    fNotifier.Notify(ALL_CLIENTS, kXRunCallback, 0);
}

void JackEngine::NotifyClientXRun(int refnum)
{
    if (refnum == ALL_CLIENTS) {
        NotifyClients(kXRunCallback, false, "", 0, 0);
    } else {
        NotifyClient(refnum, kXRunCallback, false, "", 0, 0);
    }
}

void JackEngine::NotifyGraphReorder()
{
    ComputeTotalLatencies();
    NotifyClients(kGraphOrderCallback, false, "", 0, 0);
}

void JackEngine::NotifyBufferSize(jack_nframes_t buffer_size)
{
    NotifyClients(kBufferSizeCallback, true, "", buffer_size, 0);
}

void JackEngine::NotifySampleRate(jack_nframes_t sample_rate)
{
    NotifyClients(kSampleRateCallback, true, "", sample_rate, 0);
}

void JackEngine::NotifyFailure(int code, const char* reason)
{
    NotifyClients(kShutDownCallback, false, reason, code, 0);
}

void JackEngine::NotifyFreewheel(bool onoff)
{
    if (onoff) {
        // Save RT state
        fEngineControl->fSavedRealTime = fEngineControl->fRealTime;
        fEngineControl->fRealTime = false;
    } else {
        // Restore RT state
        fEngineControl->fRealTime = fEngineControl->fSavedRealTime;
        fEngineControl->fSavedRealTime = false;
    }
    NotifyClients((onoff ? kStartFreewheelCallback : kStopFreewheelCallback), true, "", 0, 0);
}

void JackEngine::NotifyPortRegistation(jack_port_id_t port_index, bool onoff)
{
    NotifyClients((onoff ? kPortRegistrationOnCallback : kPortRegistrationOffCallback), false, "", port_index, 0);
}

void JackEngine::NotifyPortRename(jack_port_id_t port, const char* old_name)
{
    NotifyClients(kPortRenameCallback, false, old_name, port, 0);
}

void JackEngine::NotifyPortConnect(jack_port_id_t src, jack_port_id_t dst, bool onoff)
{
    NotifyClients((onoff ? kPortConnectCallback : kPortDisconnectCallback), false, "", src, dst);
}

void JackEngine::NotifyActivate(int refnum)
{
    NotifyClient(refnum, kActivateClient, true, "", 0, 0);
}

//----------------------------
// Loadable client management
//----------------------------

int JackEngine::GetInternalClientName(int refnum, char* name_res)
{
    JackClientInterface* client = fClientTable[refnum];
    assert(client);
    strncpy(name_res, client->GetClientControl()->fName, JACK_CLIENT_NAME_SIZE);
    return 0;
}

int JackEngine::InternalClientHandle(const char* client_name, int* status, int* int_ref)
{
    // Clear status
    *status = 0;

    for (int i = 0; i < CLIENT_NUM; i++) {
        JackClientInterface* client = fClientTable[i];
        if (client && dynamic_cast<JackLoadableInternalClient*>(client) && (strcmp(client->GetClientControl()->fName, client_name) == 0)) {
            jack_log("InternalClientHandle found client name = %s ref = %ld",  client_name, i);
            *int_ref = i;
            return 0;
        }
    }

    *status |= (JackNoSuchClient | JackFailure);
    return -1;
}

int JackEngine::InternalClientUnload(int refnum, int* status)
{
    JackClientInterface* client = fClientTable[refnum];
    if (client) {
        int res = client->Close();
        delete client;
        *status = 0;
        return res;
    } else {
        *status = (JackNoSuchClient | JackFailure);
        return -1;
    }
}

//-------------------
// Client management
//-------------------

int JackEngine::ClientCheck(const char* name, jack_uuid_t uuid, char* name_res, int protocol, int options, int* status)
{
    // Clear status
    *status = 0;
    strcpy(name_res, name);

    jack_log("Check protocol client = %ld server = %ld", protocol, JACK_PROTOCOL_VERSION);

    if (protocol != JACK_PROTOCOL_VERSION) {
        *status |= (JackFailure | JackVersionError);
        jack_error("JACK protocol mismatch (%d vs %d)", protocol, JACK_PROTOCOL_VERSION);
        return -1;
    }

    std::map<int,std::string>::iterator res = fReservationMap.find(uuid);

    if (res != fReservationMap.end()) {
        strncpy(name_res, res->second.c_str(), JACK_CLIENT_NAME_SIZE);
    } else if (ClientCheckName(name)) {

        *status |= JackNameNotUnique;

        if (options & JackUseExactName) {
            jack_error("cannot create new client; %s already exists", name);
            *status |= JackFailure;
            return -1;
        }

        if (GenerateUniqueName(name_res)) {
            *status |= JackFailure;
            return -1;
        }
    }

    return 0;
}

bool JackEngine::GenerateUniqueName(char* name)
{
    int tens, ones;
    int length = strlen(name);

    if (length > JACK_CLIENT_NAME_SIZE - 4) {
        jack_error("%s exists and is too long to make unique", name);
        return true;            /* failure */
    }

    /*  generate a unique name by appending "-01".."-99" */
    name[length++] = '-';
    tens = length++;
    ones = length++;
    name[tens] = '0';
    name[ones] = '1';
    name[length] = '\0';

    while (ClientCheckName(name)) {
        if (name[ones] == '9') {
            if (name[tens] == '9') {
                jack_error("client %s has 99 extra instances already", name);
                return true; /* give up */
            }
            name[tens]++;
            name[ones] = '0';
        } else {
            name[ones]++;
        }
    }
    return false;
}

bool JackEngine::ClientCheckName(const char* name)
{
    for (int i = 0; i < CLIENT_NUM; i++) {
        JackClientInterface* client = fClientTable[i];
        if (client && (strcmp(client->GetClientControl()->fName, name) == 0)) {
            return true;
        }
    }

    for (std::map<int,std::string>::iterator i = fReservationMap.begin(); i != fReservationMap.end(); i++) {
        if (i->second == name) {
            return true;
        }
    }

    return false;
}

void JackEngine::EnsureUUID(jack_uuid_t uuid)
{
    if (jack_uuid_empty(uuid))
        return;

    for (int i = 0; i < CLIENT_NUM; i++) {
        JackClientInterface* client = fClientTable[i];
        if (client && jack_uuid_compare(client->GetClientControl()->fSessionID, uuid) == 0) {
            // FIXME? this code does nothing, but jack1 has it like this too..
            jack_uuid_clear (&uuid);
            // client->GetClientControl()->fSessionID = jack_client_uuid_generate();
        }
    }
}

int JackEngine::GetClientPID(const char* name)
{
    for (int i = 0; i < CLIENT_NUM; i++) {
        JackClientInterface* client = fClientTable[i];
        if (client && (strcmp(client->GetClientControl()->fName, name) == 0)) {
            return client->GetClientControl()->fPID;
        }
    }

    return 0;
}

int JackEngine::GetClientRefNum(const char* name)
{
    for (int i = 0; i < CLIENT_NUM; i++) {
        JackClientInterface* client = fClientTable[i];
        if (client && (strcmp(client->GetClientControl()->fName, name) == 0)) {
            return client->GetClientControl()->fRefNum;
        }
    }

    return -1;
}

// Used for external clients
int JackEngine::ClientExternalOpen(const char* name, int pid, jack_uuid_t uuid, int* ref, int* shared_engine, int* shared_client, int* shared_graph_manager)
{
    char real_name[JACK_CLIENT_NAME_SIZE + 1];

    if (jack_uuid_empty(uuid)) {
        uuid = jack_client_uuid_generate();
        strncpy(real_name, name, JACK_CLIENT_NAME_SIZE);
    } else {
        std::map<int, std::string>::iterator res = fReservationMap.find(uuid);
        if (res != fReservationMap.end()) {
            strncpy(real_name, res->second.c_str(), JACK_CLIENT_NAME_SIZE);
            fReservationMap.erase(uuid);
        } else {
            strncpy(real_name, name, JACK_CLIENT_NAME_SIZE);
        }
        EnsureUUID(uuid);
    }

    jack_log("JackEngine::ClientExternalOpen: uuid = %d, name = %s", uuid, real_name);

    int refnum = AllocateRefnum();
    if (refnum < 0) {
        jack_error("No more refnum available");
        return -1;
    }

    // A fresh slot: the client (its libjack) makes it its own, and nothing of an
    // earlier client of this refnum may be taken for this one's
    WatchClient(refnum, false);
    fThreadDead[refnum] = false;
#ifdef __linux__
    fAliveTable.ClearSlot(refnum);
#endif

    JackExternalClient* client = new JackExternalClient();

    if (!fSynchroTable[refnum].Allocate(real_name, fEngineControl->fServerName, 0)) {
        jack_error("Cannot allocate synchro");
        goto error;
    }

    if (client->Open(real_name, pid, refnum, uuid, shared_client) < 0) {
        jack_error("Cannot open client");
        goto error;
    }

    if (!fSignal.TimedWait(DRIVER_OPEN_TIMEOUT * 1000000)) {
        // Failure if RT thread is not running (problem with the driver...)
        jack_error("Driver is not running");
        goto error;
    }

    fClientTable[refnum] = client;
    UpdateRefNumBound();

    if (NotifyAddClient(client, real_name, refnum) < 0) {
        jack_error("Cannot notify add client");
        goto error;
    }

    fGraphManager->InitRefNum(refnum);
    fEngineControl->ResetRollingUsecs();
    *shared_engine = fEngineControl->GetShmIndex();
    *shared_graph_manager = fGraphManager->GetShmIndex();
    *ref = refnum;
    return 0;

error:
    // Cleanup...
    fSynchroTable[refnum].Destroy();
    fClientTable[refnum] = 0;
    UpdateRefNumBound();
    client->Close();
    delete client;
    return -1;
}

// Used for server driver clients
int JackEngine::ClientInternalOpen(const char* name, int* ref, JackEngineControl** shared_engine, JackGraphManager** shared_manager, JackClientInterface* client, bool wait)
{
    jack_log("JackEngine::ClientInternalOpen: name = %s", name);

    int refnum = AllocateRefnum();
    if (refnum < 0) {
        jack_error("No more refnum available");
        goto error;
    }

    if (!fSynchroTable[refnum].Allocate(name, fEngineControl->fServerName, 0)) {
        jack_error("Cannot allocate synchro");
        goto error;
    }

    if (wait && !fSignal.TimedWait(DRIVER_OPEN_TIMEOUT * 1000000)) {
        // Failure if RT thread is not running (problem with the driver...)
        jack_error("Driver is not running");
        goto error;
    }

    fClientTable[refnum] = client;
    UpdateRefNumBound();

    if (NotifyAddClient(client, name, refnum) < 0) {
        jack_error("Cannot notify add client");
        goto error;
    }

    fGraphManager->InitRefNum(refnum);
    fEngineControl->ResetRollingUsecs();
    *shared_engine = fEngineControl;
    *shared_manager = fGraphManager;
    *ref = refnum;
    return 0;

error:
    // Cleanup...
    fSynchroTable[refnum].Destroy();
    fClientTable[refnum] = 0;
    UpdateRefNumBound();
    return -1;
}

// Used for external clients
int JackEngine::ClientExternalClose(int refnum)
{
    jack_log("JackEngine::ClientExternalClose ref = %ld", refnum);
    JackClientInterface* client = fClientTable[refnum];
    assert(client);
    int res = ClientCloseAux(refnum, true);
    client->Close();
    delete client;
    return res;
}

// Used for server internal clients or drivers when the RT thread is stopped
int JackEngine::ClientInternalClose(int refnum, bool wait)
{
    jack_log("JackEngine::ClientInternalClose ref = %ld", refnum);
    return ClientCloseAux(refnum, wait);
}

int JackEngine::ClientCloseAux(int refnum, bool wait)
{
    jack_log("JackEngine::ClientCloseAux ref = %ld", refnum);

    JackClientInterface* client = fClientTable[refnum];
    fEngineControl->fTransport.ResetTimebase(refnum);

    jack_uuid_t uuid = JACK_UUID_EMPTY_INITIALIZER;
    jack_uuid_copy (&uuid, client->GetClientControl()->fSessionID);

    // Unregister all ports ==> notifications are sent
    jack_int_t ports[PORT_NUM_FOR_CLIENT];
    int i;

    fGraphManager->GetInputPorts(refnum, ports);
    for (i = 0; (i < PORT_NUM_FOR_CLIENT) && (ports[i] != EMPTY); i++) {
        PortUnRegister(refnum, ports[i]);
    }

    fGraphManager->GetOutputPorts(refnum, ports);
    for (i = 0; (i < PORT_NUM_FOR_CLIENT) && (ports[i] != EMPTY); i++) {
        PortUnRegister(refnum, ports[i]);
    }

    // Remove the client from the table
    ReleaseRefnum(refnum);

    // Remove all ports
    fGraphManager->RemoveAllPorts(refnum);

    // Wait until next cycle to be sure client is not used anymore
    bool graph_switched = true;
    if (wait) {
        if (!fSignal.TimedWait(fEngineControl->fTimeOutUsecs * 2)) { // Must wait at least until a switch occurs in Process, even in case of graph end failure
            graph_switched = false;
            jack_error("JackEngine::ClientCloseAux wait error ref = %ld", refnum);
        }
    }

    if (fMetadata.RemoveProperties(NULL, uuid) > 0) {
        /* have to do the notification ourselves, since the client argument
          to fMetadata->RemoveProperties() was NULL
        */
        PropertyChangeNotify(uuid, NULL, PropertyDeleted);
    }

    // Notify running clients
    NotifyRemoveClient(client->GetClientControl()->fName, refnum);

    // Cleanup...
#ifdef __linux__
    if (!graph_switched) {
        /* The graph that still activates this client may be running: the
           realtime thread can signal its futex any time. Unmapping it now
           (Destroy) could make the realtime thread write to an unmapped page
           and crash the server; this happens precisely when a client dies in
           the middle of a cycle and the graph does not finish. Remove the
           name but keep the (one page) mapping. Found with ThreadSanitizer. */
        fSynchroTable[refnum].DestroyKeepMapped();
    } else
#endif
    fSynchroTable[refnum].Destroy();
    fEngineControl->ResetRollingUsecs();
    return 0;
}

int JackEngine::ClientActivate(int refnum, bool is_real_time, bool wait)
{
    JackClientInterface* client = fClientTable[refnum];
    jack_log("JackEngine::ClientActivate ref = %ld name = %s", refnum, client->GetClientControl()->fName);

    if (is_real_time) {
        fGraphManager->Activate(refnum);
        // Its realtime thread is running and holds its slot (if its libjack has
        // one): libjack starts the thread before asking for activation
        fThreadDead[refnum] = false;
        if (dynamic_cast<JackExternalClient*>(client)) {
            WatchClient(refnum, true);
        }
    }

    // Wait for graph state change to be effective
    if (wait && !fSignal.TimedWait(fEngineControl->fTimeOutUsecs * 10)) {
        jack_error("JackEngine::ClientActivate wait error ref = %ld name = %s", refnum, client->GetClientControl()->fName);
        return -1;
    } else {
        jack_int_t input_ports[PORT_NUM_FOR_CLIENT];
        jack_int_t output_ports[PORT_NUM_FOR_CLIENT];
        fGraphManager->GetInputPorts(refnum, input_ports);
        fGraphManager->GetOutputPorts(refnum, output_ports);

        // Notify client
        NotifyActivate(refnum);

        // Then issue port registration notification
        for (int i = 0; (i < PORT_NUM_FOR_CLIENT) && (input_ports[i] != EMPTY); i++) {
            NotifyPortRegistation(input_ports[i], true);
        }
        for (int i = 0; (i < PORT_NUM_FOR_CLIENT) && (output_ports[i] != EMPTY); i++) {
            NotifyPortRegistation(output_ports[i], true);
        }

        return 0;
    }
}

// May be called without client
int JackEngine::ClientDeactivate(int refnum, bool wait)
{
    JackClientInterface* client = fClientTable[refnum];
    jack_log("JackEngine::ClientDeactivate ref = %ld name = %s", refnum, client->GetClientControl()->fName);

    // Before anything: libjack ends the client's realtime thread once this returns,
    // which is no reason for the notifier to report it
    WatchClient(refnum, false);

    if (fThreadDead[refnum]) {
        // Already out of the graph (DetachThreadEndedClient), its connections and
        // ports announced as gone then
        return 0;
    }

    jack_int_t input_ports[PORT_NUM_FOR_CLIENT];
    jack_int_t output_ports[PORT_NUM_FOR_CLIENT];
    fGraphManager->GetInputPorts(refnum, input_ports);
    fGraphManager->GetOutputPorts(refnum, output_ports);

    /*
     Take the client out of the graph in one step, then notify. 1.9.22 published
     each disconnection separately, sent its notification to every client, and
     removed the client's activation edges last: until then every cycle waited for
     the client. For a client whose process died, that is how long its successors
     stop running, and each cycle the graph does not finish is reported as an
     xrun. All edits now go into one write session (nothing is published until
     the outermost WriteNextStateStop, so the realtime thread switches to the whole
     change at once), the switch is forced at the next cycle, and the same
     notifications follow.
     */
    std::vector<std::pair<jack_port_id_t, jack_port_id_t> > removed;
    fGraphManager->WriteNextStateStart();
    for (int i = 0; (i < PORT_NUM_FOR_CLIENT) && (input_ports[i] != EMPTY); i++) {
        DisconnectAllDeferred(input_ports[i], removed);
    }
    for (int i = 0; (i < PORT_NUM_FOR_CLIENT) && (output_ports[i] != EMPTY); i++) {
        DisconnectAllDeferred(output_ports[i], removed);
    }
    fGraphManager->Deactivate(refnum);
    fGraphManager->WriteNextStateStop();
    __atomic_store_n(&fLastSwitchUsecs, (jack_time_t)0, __ATOMIC_RELAXED); // Force switch to occur next cycle, even when called with "dead" clients

    for (size_t i = 0; i < removed.size(); i++) {
        NotifyPortConnect(removed[i].first, removed[i].second, false);
    }

    // Then issue port registration notification
    for (int i = 0; (i < PORT_NUM_FOR_CLIENT) && (input_ports[i] != EMPTY); i++) {
        NotifyPortRegistation(input_ports[i], false);
    }
    for (int i = 0; (i < PORT_NUM_FOR_CLIENT) && (output_ports[i] != EMPTY); i++) {
        NotifyPortRegistation(output_ports[i], false);
    }

    // Wait for graph state change to be effective
    if (wait && !fSignal.TimedWait(fEngineControl->fTimeOutUsecs * 10)) {
        jack_error("JackEngine::ClientDeactivate wait error ref = %ld name = %s", refnum, client->GetClientControl()->fName);
        return -1;
    } else {
        return 0;
    }
}

// PortDisconnect(-1, port, ALL_PORTS) without the notifications: the pairs actually
// disconnected are appended to "removed", for the caller to notify
void JackEngine::DisconnectAllDeferred(jack_port_id_t port, std::vector<std::pair<jack_port_id_t, jack_port_id_t> >& removed)
{
    jack_int_t connections[CONNECTION_NUM_FOR_PORT];
    fGraphManager->GetConnections(port, connections);
    bool output = (fGraphManager->GetPort(port)->GetFlags() & JackPortIsOutput) != 0;

    for (int i = 0; (i < CONNECTION_NUM_FOR_PORT) && (connections[i] != EMPTY); i++) {
        jack_port_id_t src = output ? port : (jack_port_id_t)connections[i];
        jack_port_id_t dst = output ? (jack_port_id_t)connections[i] : port;
        if (fGraphManager->CheckPorts(src, dst) == 0 && fGraphManager->Disconnect(src, dst) == 0) {
            removed.push_back(std::make_pair(src, dst));
        }
    }
}

void JackEngine::ClientKill(int refnum)
{
    jack_log("JackEngine::ClientKill ref = %ld", refnum);

    /*
     The client's request socket is gone, so is its process (or it closed the
     socket): notifications to it can only fail, each one logged (four lines per
     port and per connection). And as the client did not deactivate itself, its
     fActive flag is still set, which made ClientCloseAux send every port
     unregistration a second time (patchbays then report ports they cannot find).
     */
    JackExternalClient* external = dynamic_cast<JackExternalClient*>(fClientTable[refnum]);
    bool detached = (external && external->IsDead()) || fThreadDead[refnum];
    if (external) {
        external->SetDead();
    }
    if (detached) {
        // its ports were announced as gone when it was taken out of the graph: not
        // again when they are released (ClientCloseAux)
        fClientTable[refnum]->GetClientControl()->fActive = false;
    }
    if (!detached) {
        /*
         Deactivated whatever its state, as in 1.9.22, even if it had deactivated
         itself (or was never activated): its ports are then announced as gone a
         second time (or announced at all), but a client that learned them from
         jack_get_ports() rather than from callbacks relies on that announcement.
         */
        if (ClientDeactivate(refnum, true) < 0) {
            jack_error("JackEngine::ClientKill ref = %ld cannot be removed from the graph !!", refnum);
        }
        fClientTable[refnum]->GetClientControl()->fActive = false;
    } else {
        /*
         Already taken out of the graph by CheckStuckClients (its process was found
         dying, or its realtime thread ended), which announced its connections and
         ports as gone, and does not wait for the switch. Wait for it here, as
         ClientDeactivate(refnum, true) does: releasing
         the ports below opens one write session per port, and the realtime thread
         cannot switch while one is open, so it could keep running the graph that
         still waits for this client.
         */
        if (!fSignal.TimedWait(fEngineControl->fTimeOutUsecs * 10)) {
            jack_error("JackEngine::ClientKill ref = %ld wait error", refnum);
        }
    }

    if (ClientExternalClose(refnum) < 0) {
        jack_error("JackEngine::ClientKill ref = %ld cannot be closed", refnum);
    }
}

#ifdef __linux__
/*
 Process state from /proc, for clients the graph is waiting on. Read on the request
 thread only, and only after a cycle in which the graph did not finish.

 "tid (comm) state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt
 utime stime cutime cstime priority nice num_threads itrealvalue starttime ..." (comm
 may contain spaces and parentheses: parse after the last ')').
 */
// 1: read; -1: no such task (ENOENT, or ESRCH once it is reaped); 0: anything else
static int ReadTaskStat(const char* path, char* state, unsigned long* flags, unsigned long long* start)
{
    char buf[1024];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return (errno == ENOENT || errno == ESRCH) ? -1 : 0;
    }
    ssize_t len = read(fd, buf, sizeof(buf) - 1);
    int err = errno;
    close(fd);
    if (len < 0) {
        return (err == ESRCH) ? -1 : 0;
    }
    if (len == 0) {
        return 0;
    }
    buf[len] = 0;
    const char* end = strrchr(buf, ')');
    return (end && sscanf(end + 1, " %c %*d %*d %*d %*d %*d %lu %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu",
                          state, flags, start) == 3) ? 1 : 0;
}

enum PeerState { kPeerAlive, kPeerExiting, kPeerGone };

/*
 kPeerGone: no process with this pid and start time any more. It exited and was
 reaped, yet the server still has its request socket open: another process holds a
 copy (libjack's sockets are not close-on-exec, so a child it spawned inherits them).
 kPeerExiting: every thread has PF_EXITING (set when a thread enters do_exit: SIGKILL,
 a crash, exit(); it never runs user code again) or is a zombie. A process whose main
 thread called pthread_exit() still has live threads: alive.
 Anything unreadable or unexpected: alive. Both other answers are certain.
 */
static PeerState ProcessState(int pid, unsigned long long start)
{
    const unsigned long kPFExiting = 0x00000004;    // include/linux/sched.h
    char path[96];
    char state;
    unsigned long flags;
    unsigned long long task_start;

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    int res = ReadTaskStat(path, &state, &flags, &task_start);
    if (res <= 0) {
        return (res < 0) ? kPeerGone : kPeerAlive;
    }
    if (task_start != start) {
        return kPeerGone;   // the pid now belongs to another process
    }

    snprintf(path, sizeof(path), "/proc/%d/task", pid);
    DIR* dir = opendir(path);
    if (!dir) {
        return kPeerAlive;
    }
    bool exiting = true;
    int tasks = 0;
    struct dirent* entry;
    while (exiting && (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue;
        }
        char task_path[300];
        snprintf(task_path, sizeof(task_path), "/proc/%d/task/%s/stat", pid, entry->d_name);
        if (ReadTaskStat(task_path, &state, &flags, &task_start) <= 0) {
            continue;   // this thread finished exiting since readdir (or unreadable)
        }
        tasks++;
        if (!(flags & kPFExiting) && state != 'Z' && state != 'X') {
            exiting = false;
        }
    }
    closedir(dir);
    return (exiting && tasks > 0) ? kPeerExiting : kPeerAlive;
}
#endif

bool JackEngine::IsClientDead(int refnum)
{
    if (refnum < 0 || refnum >= CLIENT_NUM) {
        return false;
    }
    JackExternalClient* client = dynamic_cast<JackExternalClient*>(fClientTable[refnum]);
    return client && client->IsDead();
}

void JackEngine::ClientSetPeerPid(int refnum, int pid, int uid)
{
#ifdef __linux__
    if (refnum < 0 || refnum >= CLIENT_NUM || !fClientTable[refnum] || pid <= 0) {
        return;
    }
    // Remember the process' start time. If /proc does not show this process (pid not
    // visible from here, hidepid, a /proc of another pid namespace: owner not the
    // socket peer's uid), leave the client unchecked.
    char path[64];
    char state;
    unsigned long flags;
    unsigned long long start;
    struct stat st;
    snprintf(path, sizeof(path), "/proc/%d", pid);
    if (stat(path, &st) < 0 || (int)st.st_uid != uid) {
        return;
    }
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if (ReadTaskStat(path, &state, &flags, &start) > 0) {
        fPeerPid[refnum] = pid;
        fPeerStart[refnum] = start;
        fPeerNextCheck[refnum] = 0;
    }
#else
    (void)refnum;
    (void)pid;
    (void)uid;
#endif
}

/*
 Request thread, when the realtime thread found the graph unfinished in a cycle.

 A client whose process died stays in the graph until the server sees its request
 socket close, and the kernel closes a dying process' files only after tearing down
 its whole address space (exit_mm comes before exit_files): 100 ms or more for a big
 process, every cycle of it with the client's successors not running and xruns
 reported. If a child process inherited the socket, it stays open until the child
 exits. The kernel marks the dying process' threads at once, though, and a reaped
 process disappears from /proc: take such a client out of the graph now; the request
 channel then removes it as if its socket had closed (KillDeadClients).

 Only clients the graph is waiting on are looked at (not finished in the cycle), and
 only certain death counts: a client that is merely late or hung is left alone. A
 client found alive is not looked at again for 100 ms.
 */
void JackEngine::CheckStuckClients()
{
#ifdef __linux__
    jack_time_t now = GetMicroSeconds();

    /*
     Clients whose realtime thread ended while they were active: their slot in the
     JackAliveTable is marked FUTEX_OWNER_DIED by the kernel (their libjack holds it
     from that thread). Certain: that thread never runs again. Usually the process
     was killed or crashed and the cycle it is due in has not started yet: out of
     the graph before it, the graph does not even miss a cycle.
     */
    for (int i = 0; i < fRefNumBound; i++) {
        JackExternalClient* client = dynamic_cast<JackExternalClient*>(fClientTable[i]);
        if (!client || client->IsDead() || fThreadDead[i] || !IsWatched(i) || !client->GetClientControl()->fActive) {
            continue;
        }
        uint32_t* word = fAliveTable.Word(i);
        if (word && (__atomic_load_n(word, __ATOMIC_ACQUIRE) & FUTEX_OWNER_DIED)) {
            jack_info("client %s: its realtime thread ended, taken out of the graph", client->GetClientControl()->fName);
            DetachThreadEndedClient(i);
            fThreadDeadSince[i] = now;
        }
    }

    /*
     Such a client is removed (ports, registration) once its process is certainly
     exiting or gone, which usually follows within a few milliseconds; looked at
     every 20 ms for 2 s. A process that lives on without that thread (its
     jack_set_process_thread function returned, say) keeps its client until it
     closes it or activates it again.
     */
    bool pending = false;
    for (int i = 0; i < fRefNumBound; i++) {
        JackExternalClient* client = dynamic_cast<JackExternalClient*>(fClientTable[i]);
        if (!client || client->IsDead() || !fThreadDead[i] || fPeerPid[i] <= 0) {
            continue;
        }
        if (ProcessState(fPeerPid[i], fPeerStart[i]) != kPeerAlive) {
            client->SetDead();  // the request channel removes it (KillDeadClients)
        } else if (now - fThreadDeadSince[i] < 2000000) {
            pending = true;
        }
    }
    if (pending) {
        fNotifier.RequestCheck();
    }

    // Clients without such a slot (another libjack): when the graph did not finish
    for (int i = 0; i < fRefNumBound; i++) {   // drivers are skipped (not external clients)
        JackExternalClient* client = dynamic_cast<JackExternalClient*>(fClientTable[i]);
        if (!client || client->IsDead() || fThreadDead[i] || fPeerPid[i] <= 0 || !client->GetClientControl()->fActive) {
            continue;
        }
        if (fGraphManager->GetClientTiming(i)->fStatus == Finished || now < fPeerNextCheck[i]) {
            continue;
        }
        PeerState state = ProcessState(fPeerPid[i], fPeerStart[i]);
        if (state == kPeerAlive) {
            fPeerNextCheck[i] = now + 100000;
        } else {
            jack_info("client %s: its process %s, taken out of the graph", client->GetClientControl()->fName,
                      (state == kPeerExiting) ? "is exiting" : "is gone");
            DetachDeadClient(i);
        }
    }
#endif
}

// Request thread: send what is queued for clients that lag, if they read again. Returns
// how many still lag (the request thread then looks again in 50 ms).
int JackEngine::FlushNotifications()
{
    int lagging = 0;
    // from 0, not fDriverNum: drivers are not external clients, and fDriverNum is
    // written without the engine lock when a driver is opened (master switch)
    for (int i = 0; i < fRefNumBound; i++) {
        JackExternalClient* client = dynamic_cast<JackExternalClient*>(fClientTable[i]);
        if (client && client->FlushNotifications()) {
            lagging++;
        }
    }
    return lagging;
}

void JackEngine::WatchClient(int refnum, bool on)
{
    if (refnum < 0 || refnum >= CLIENT_NUM) {
        return;
    }
    uint64_t bit = 1ULL << (refnum % 64);
    if (on) {
        __atomic_fetch_or(&fAliveWatch[refnum / 64], bit, __ATOMIC_RELEASE);
        fNotifier.WakeUp();     // wait on its slot from now on
    } else {
        __atomic_fetch_and(&fAliveWatch[refnum / 64], ~bit, __ATOMIC_RELEASE);
    }
}

bool JackEngine::IsWatched(int refnum) const
{
    return (__atomic_load_n(&fAliveWatch[refnum / 64], __ATOMIC_ACQUIRE) >> (refnum % 64)) & 1;
}

/*
 The client's realtime thread ended while it was active: out of the graph now, without
 waiting for the switch. It stays registered (its process may live on), and its
 fActive flag is left as it is: if the process goes on, its libjack still sees it
 active, so that jack_deactivate or jack_client_close ends and joins that thread as
 usual (ClientDeactivate has nothing left to do), and jack_activate after that brings
 it back. ClientKill clears fActive when it is gone.
 */
void JackEngine::DetachThreadEndedClient(int refnum)
{
    ClientDeactivate(refnum, false);
    fThreadDead[refnum] = true;
}

// The client's process is exiting or gone: out of the graph now, without waiting for
// the switch (other stuck clients may be found too). ClientKill, called next by the
// request channel (KillDeadClients), waits for the switch and removes the client.
void JackEngine::DetachDeadClient(int refnum)
{
    JackExternalClient* client = dynamic_cast<JackExternalClient*>(fClientTable[refnum]);
    client->SetDead();
    ClientDeactivate(refnum, false);
    client->GetClientControl()->fActive = false;
}

//-----------------
// Port management
//-----------------

int JackEngine::PortRegister(int refnum, const char* name, const char *type, unsigned int flags, unsigned int buffer_size, jack_port_id_t* port_index)
{
    jack_log("JackEngine::PortRegister ref = %ld name = %s type = %s flags = %d buffer_size = %d", refnum, name, type, flags, buffer_size);
    JackClientInterface* client = fClientTable[refnum];

    // Check if port name already exists
    if (fGraphManager->GetPort(name) != NO_PORT) {
        jack_error("port_name \"%s\" already exists", name);
        return -1;
    }

    // buffer_size is actually ignored...
    *port_index = fGraphManager->AllocatePort(refnum, name, type, (JackPortFlags)flags, fEngineControl->fBufferSize);
    if (*port_index != NO_PORT) {
        if (client->GetClientControl()->fActive) {
            NotifyPortRegistation(*port_index, true);
        }
        return 0;
    } else {
        return -1;
    }
}

int JackEngine::PortUnRegister(int refnum, jack_port_id_t port_index)
{
    jack_log("JackEngine::PortUnRegister ref = %ld port_index = %ld", refnum, port_index);
    JackClientInterface* client = fClientTable[refnum];
    assert(client);

    // Disconnect port ==> notification is sent
    PortDisconnect(-1, port_index, ALL_PORTS);

    if (fGraphManager->ReleasePort(refnum, port_index) == 0) {
        const jack_uuid_t uuid = jack_port_uuid_generate(port_index);
        if (!jack_uuid_empty(uuid))
        {
            if (fMetadata.RemoveProperties(NULL, uuid) > 0) {
                /* have to do the notification ourselves, since the client argument
                   to fMetadata->RemoveProperties() was NULL
                */
                PropertyChangeNotify(uuid, NULL, PropertyDeleted);
            }
        }

        if (client->GetClientControl()->fActive) {
            NotifyPortRegistation(port_index, false);
        }
        return 0;
    } else {
        return -1;
    }
}

// this check is to prevent apps to self connect to other apps
// TODO: make this work with multiple clients per app
int JackEngine::CheckPortsConnect(int refnum, jack_port_id_t src, jack_port_id_t dst)
{
    if (fSelfConnectMode == ' ') return 1;

    JackPort* src_port = fGraphManager->GetPort(src);
    JackPort* dst_port = fGraphManager->GetPort(dst);

    jack_log("JackEngine::CheckPortsConnect(ref = %d, src = %d, dst = %d)", refnum, src_port->GetRefNum(), dst_port->GetRefNum());

    //jack_log("%s -> %s", src_port->GetName(), dst_port->GetName());
    //jack_log("mode = '%c'", fSelfConnectMode);

    int src_self = src_port->GetRefNum() == refnum ? 1 : 0;
    int dst_self = dst_port->GetRefNum() == refnum ? 1 : 0;

    //jack_log("src_self is %s", src_self ? "true" : "false");
    //jack_log("dst_self is %s", dst_self ? "true" : "false");

    // 0 means client is connecting other client ports (control app patchbay functionality)
    // 1 means client is connecting its own port to port of other client (e.g. self connecting into "system" client)
    // 2 means client is connecting its own ports (for app internal functionality)
    int sum = src_self + dst_self;
    //jack_log("sum = %d", sum);
    if (sum == 0) return 1;
    char lmode = tolower(fSelfConnectMode);
    //jack_log("lmode = '%c'", lmode);
    if (sum == 2 && lmode == 'e') return 1;
    bool fail = lmode != fSelfConnectMode; // fail modes are upper case
    //jack_log("fail = %d", (int)fail);

    jack_info(
        "%s port self connect request%s (%s -> %s)",
        fail ? "rejecting" : "ignoring",
        sum == 1 ? " to external port" : "",
        src_port->GetName(),
        dst_port->GetName());

    return fail ? -1 : 0;
}

int JackEngine::PortConnect(int refnum, const char* src, const char* dst)
{
    jack_log("JackEngine::PortConnect ref = %d src = %s dst = %s", refnum, src, dst);
    jack_port_id_t port_src, port_dst;

    return (fGraphManager->GetTwoPorts(src, dst, &port_src, &port_dst) < 0)
           ? -1
           : PortConnect(refnum, port_src, port_dst);
}

int JackEngine::PortConnect(int refnum, jack_port_id_t src, jack_port_id_t dst)
{
    jack_log("JackEngine::PortConnect ref = %d src = %d dst = %d", refnum, src, dst);
    JackClientInterface* client;
    int ref;

    if (fGraphManager->CheckPorts(src, dst) < 0) {
        return -1;
    }

    ref = fGraphManager->GetOutputRefNum(src);
    assert(ref >= 0);
    client = fClientTable[ref];
    assert(client);
    if (!client->GetClientControl()->fActive) {
        jack_error("Cannot connect ports owned by inactive clients:"
                   " \"%s\" is not active", client->GetClientControl()->fName);
        return -1;
    }

    ref = fGraphManager->GetInputRefNum(dst);
    assert(ref >= 0);
    client = fClientTable[ref];
    assert(client);
    if (!client->GetClientControl()->fActive) {
        jack_error("Cannot connect ports owned by inactive clients:"
                   " \"%s\" is not active", client->GetClientControl()->fName);
        return -1;
    }

    int res = CheckPortsConnect(refnum, src, dst);
    if (res != 1) {
        return res;
    }

    res = fGraphManager->Connect(src, dst);
    if (res == 0) {
        NotifyPortConnect(src, dst, true);
    }
    return res;
}

int JackEngine::PortDisconnect(int refnum, const char* src, const char* dst)
{
    jack_log("JackEngine::PortDisconnect ref = %d src = %s dst = %s", refnum, src, dst);
    jack_port_id_t port_src, port_dst;

    return (fGraphManager->GetTwoPorts(src, dst, &port_src, &port_dst) < 0)
           ? -1
           : PortDisconnect(refnum, port_src, port_dst);
}

int JackEngine::PortDisconnect(int refnum, jack_port_id_t src, jack_port_id_t dst)
{
    jack_log("JackEngine::PortDisconnect ref = %d src = %d dst = %d", refnum, src, dst);

    if (dst == ALL_PORTS) {

        jack_int_t connections[CONNECTION_NUM_FOR_PORT];
        fGraphManager->GetConnections(src, connections);

        JackPort* port = fGraphManager->GetPort(src);
        int res = 0;
        if (port->GetFlags() & JackPortIsOutput) {
            for (int i = 0; (i < CONNECTION_NUM_FOR_PORT) && (connections[i] != EMPTY); i++) {
                if (PortDisconnect(refnum, src, connections[i]) != 0) {
                    res = -1;
                }
            }
        } else {
            for (int i = 0; (i < CONNECTION_NUM_FOR_PORT) && (connections[i] != EMPTY); i++) {
                if (PortDisconnect(refnum, connections[i], src) != 0) {
                    res = -1;
                }
            }
        }

        return res;
    }

    if (fGraphManager->CheckPorts(src, dst) < 0) {
        return -1;
    }

    int res = CheckPortsConnect(refnum, src, dst);
    if (res != 1) {
        return res;
    }

    res = fGraphManager->Disconnect(src, dst);
    if (res == 0)
        NotifyPortConnect(src, dst, false);
    return res;
}

int JackEngine::PortRename(int refnum, jack_port_id_t port, const char* name)
{
    char old_name[REAL_JACK_PORT_NAME_SIZE+1];
    strcpy(old_name, fGraphManager->GetPort(port)->GetName());
    fGraphManager->GetPort(port)->SetName(name);
    NotifyPortRename(port, old_name);
    return 0;
}

int JackEngine::PortSetDefaultMetadata(jack_port_id_t port, const char* pretty_name)
{
    static const char* type = "text/plain";
    jack_uuid_t uuid = jack_port_uuid_generate(port);

    int res = fMetadata.SetProperty(NULL, uuid, JACK_METADATA_HARDWARE, pretty_name, type);
    if (res == -1) {
        return -1;
    }

    char *v, *t;
    res = fMetadata.GetProperty(uuid, JACK_METADATA_PRETTY_NAME, &v, &t);
    if (res == -1) {
        res = fMetadata.SetProperty(NULL, uuid, JACK_METADATA_PRETTY_NAME, pretty_name, type);
    }

    return res;
}

//--------------------
// Session management
//--------------------

void JackEngine::SessionNotify(int refnum, const char *target, jack_session_event_type_t type, const char *path, detail::JackChannelTransactionInterface *socket, JackSessionNotifyResult** result)
{
    if (fSessionPendingReplies != 0) {
        JackSessionNotifyResult res(-1);
        res.Write(socket);
        jack_log("JackEngine::SessionNotify ... busy");
        if (result != NULL) *result = NULL;
        return;
    }

    for (int i = 0; i < CLIENT_NUM; i++) {
        JackClientInterface* client = fClientTable[i];
        if (client && jack_uuid_empty(client->GetClientControl()->fSessionID)) {
            client->GetClientControl()->fSessionID = jack_client_uuid_generate();
        }
    }
    fSessionResult = new JackSessionNotifyResult();

    for (int i = 0; i < CLIENT_NUM; i++) {
        JackClientInterface* client = fClientTable[i];
        if (client && client->GetClientControl()->fCallback[kSessionCallback]) {

            // check if this is a notification to a specific client.
            if (target != NULL && strlen(target) != 0) {
                if (strcmp(target, client->GetClientControl()->fName)) {
                    continue;
                }
            }

            char path_buf[JACK_PORT_NAME_SIZE];
            if (path[strlen(path) - 1] == DIR_SEPARATOR) {
               snprintf(path_buf, sizeof path_buf, "%s%s%c", path, client->GetClientControl()->fName, DIR_SEPARATOR);
            } else {
               snprintf(path_buf, sizeof path_buf, "%s%c%s%c", path, DIR_SEPARATOR, client->GetClientControl()->fName, DIR_SEPARATOR);
            }

            int res = JackTools::MkDir(path_buf);
            if (res) jack_error("JackEngine::SessionNotify: can not create session directory '%s'", path_buf);

            int result = client->ClientNotify(i, client->GetClientControl()->fName, kSessionCallback, true, path_buf, (int)type, 0);

            if (result == kPendingSessionReply) {
                fSessionPendingReplies += 1;
            } else if (result == kImmediateSessionReply) {
                char uuid_buf[JACK_UUID_STRING_SIZE];
                jack_uuid_unparse(client->GetClientControl()->fSessionID, uuid_buf);
                fSessionResult->fCommandList.push_back(JackSessionCommand(uuid_buf,
                                                                        client->GetClientControl()->fName,
                                                                        client->GetClientControl()->fSessionCommand,
                                                                        client->GetClientControl()->fSessionFlags));
            }
        }
    }

    if (result != NULL) *result = fSessionResult;

    if (fSessionPendingReplies == 0) {
        fSessionResult->Write(socket);
        if (result == NULL) delete fSessionResult;
        fSessionResult = NULL;
    } else {
        fSessionTransaction = socket;
    }
}

int JackEngine::SessionReply(int refnum)
{
    JackClientInterface* client = fClientTable[refnum];
    assert(client);
    char uuid_buf[JACK_UUID_STRING_SIZE];
    jack_uuid_unparse(client->GetClientControl()->fSessionID, uuid_buf);
    fSessionResult->fCommandList.push_back(JackSessionCommand(uuid_buf,
                                                            client->GetClientControl()->fName,
                                                            client->GetClientControl()->fSessionCommand,
                                                            client->GetClientControl()->fSessionFlags));
    fSessionPendingReplies -= 1;

    if (fSessionPendingReplies == 0) {
        fSessionResult->Write(fSessionTransaction);
        if (fSessionTransaction != NULL) {
            delete fSessionResult;
        }
        fSessionResult = NULL;
    }

    return 0;
}

int JackEngine::GetUUIDForClientName(const char *client_name, char *uuid_res)
{
    for (int i = 0; i < CLIENT_NUM; i++) {
        JackClientInterface* client = fClientTable[i];

        if (client && (strcmp(client_name, client->GetClientControl()->fName) == 0)) {
            jack_uuid_unparse(client->GetClientControl()->fSessionID, uuid_res);
            return 0;
        }
    }
    // Did not find name.
    return -1;
}

int JackEngine::GetClientNameForUUID(const char *uuid_buf, char *name_res)
{
    jack_uuid_t uuid;
    if (jack_uuid_parse(uuid_buf, &uuid) != 0)
        return -1;

    for (int i = 0; i < CLIENT_NUM; i++) {
        JackClientInterface* client = fClientTable[i];

        if (!client) {
            continue;
        }

        if (jack_uuid_compare(client->GetClientControl()->fSessionID, uuid) == 0) {
            strncpy(name_res, client->GetClientControl()->fName, JACK_CLIENT_NAME_SIZE);
            return 0;
        }
    }
    // Did not find uuid.
    return -1;
}

int JackEngine::ReserveClientName(const char *name, const char *uuidstr)
{
    jack_log("JackEngine::ReserveClientName ( name = %s, uuid = %s )", name, uuidstr);

    if (ClientCheckName(name)) {
        jack_log("name already taken");
        return -1;
    }

    jack_uuid_t uuid;
    if (jack_uuid_parse(uuidstr, &uuid) != 0) {
        jack_error("JackEngine::ReserveClientName invalid uuid %s", uuidstr);
        return -1;
    }

    EnsureUUID(uuid);
    fReservationMap[uuid] = name;
    return 0;
}

int JackEngine::ClientHasSessionCallback(const char *name)
{
    JackClientInterface* client = NULL;
    for (int i = 0; i < CLIENT_NUM; i++) {
        client = fClientTable[i];
        if (client && (strcmp(client->GetClientControl()->fName, name) == 0)) {
            break;
        }
    }

    if (client) {
        return client->GetClientControl()->fCallback[kSessionCallback];
     } else {
        return -1;
    }
}

} // end of namespace

