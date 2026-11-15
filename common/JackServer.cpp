/*
Copyright (C) 2001 Paul Davis
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

#include "JackSystemDeps.h"
#include "JackServerGlobals.h"
#include "JackTime.h"
#include "JackFreewheelDriver.h"
#include "JackThreadedDriver.h"
#include "JackGlobals.h"
#include "JackLockedEngine.h"
#include "JackAudioDriver.h"
#include "JackChannel.h"
#include "JackClientControl.h"
#include "JackEngineControl.h"
#include "JackGraphManager.h"
#include "JackInternalClient.h"
#include "JackError.h"
#include "JackMessageBuffer.h"
#include "JackInternalSessionLoader.h"
#include "JackControlNspa.h"

const char * jack_get_self_connect_mode_description(char mode);

namespace Jack
{

//----------------
// Server control 
//----------------
JackServer::JackServer(bool sync, bool temporary, int timeout, bool rt, int priority, int port_max, bool verbose, jack_timer_type_t clock, char self_connect_mode, const char* server_name)
{
    pthread_mutex_init(&fBridgeLock, NULL);
    fBridgeMonitorRunning = false;
    fBridgeMonitorStop.store(false);
    if (rt) {
        jack_info("JACK server starting in realtime mode with priority %ld", priority);
    } else {
        jack_info("JACK server starting in non-realtime mode");
    }

    jack_info("self-connect-mode is \"%s\"", jack_get_self_connect_mode_description(self_connect_mode));

    fGraphManager = JackGraphManager::Allocate(port_max);
    fMetadata = new JackMetadata(true);
    fEngineControl = new JackEngineControl(sync, temporary, timeout, rt, priority, verbose, clock, server_name);
    fEngine = new JackLockedEngine(fGraphManager, GetSynchroTable(), fEngineControl, self_connect_mode);

    // A distinction is made between the threaded freewheel driver and the
    // regular freewheel driver because the freewheel driver needs to run in
    // threaded mode when freewheel mode is active and needs to run as a slave
    // when freewheel mode isn't active.
    JackFreewheelDriver* freewheelDriver = new JackFreewheelDriver(fEngine, GetSynchroTable());
    fThreadedFreewheelDriver = new JackThreadedDriver(freewheelDriver);

    fFreewheelDriver = freewheelDriver;
    fDriverInfo = new JackDriverInfo();
    fAudioDriver = NULL;
    fFreewheel = false;
    fAudioDriverClosed = false;
    JackServerGlobals::fInstance = this;   // Unique instance
    JackServerGlobals::fUserCount = 1;     // One user
    __atomic_store_n(&JackGlobals::fVerbose, verbose, __ATOMIC_RELAXED);
}

JackServer::~JackServer()
{
    StopBridgeMonitor();    // normally stopped in Close already
    pthread_mutex_destroy(&fBridgeLock);
    JackGraphManager::Destroy(fGraphManager);
    delete fMetadata;
    delete fDriverInfo;
    delete fThreadedFreewheelDriver;
    delete fEngine;
    delete fEngineControl;
}

int JackServer::Open(jack_driver_desc_t* driver_desc, JSList* driver_params)
{
    // TODO: move that in reworked JackServerGlobals::Init()
    if (!JackMessageBuffer::Create()) {
        jack_error("Cannot create message buffer");
    }

     if ((fAudioDriver = fDriverInfo->Open(driver_desc, fEngine, GetSynchroTable(), driver_params)) == NULL) {
        jack_error("Cannot initialize driver");
        goto fail_close1;
    }

    if (fRequestChannel.Open(fEngineControl->fServerName, this) < 0) {
        jack_error("Server channel open error");
        goto fail_close2;
    }

    if (fEngine->Open() < 0) {
        jack_error("Cannot open engine");
        goto fail_close3;
    }

    if (fFreewheelDriver->Open() < 0) {
        jack_error("Cannot open freewheel driver");
        goto fail_close4;
    }

    if (fAudioDriver->Attach() < 0) {
        jack_error("Cannot attach audio driver");
        goto fail_close5;
    }

    fFreewheelDriver->SetMaster(false);
    fAudioDriver->SetMaster(true);
    fAudioDriver->AddSlave(fFreewheelDriver);
    fAudioDriver->SetBridges(&fBridges);
    InitTime();
    SetClockSource(fEngineControl->fClockSource);
    return 0;

fail_close5:
    fFreewheelDriver->Close();

fail_close4:
    fEngine->Close();

fail_close3:
    fRequestChannel.Close();

fail_close2:
    fAudioDriver->Close();

fail_close1:
    JackMessageBuffer::Destroy();
    return -1;
}

int JackServer::Close()
{
    jack_log("JackServer::Close");
    fMasterConnections.clear();
    fRequestChannel.Close();
    StopBridgeMonitor();
    while (!fBridgeInfos.empty()) {     // the master is stopped: nothing runs them now
        JackDriverInfo* info = fBridgeInfos.front();
        fBridgeInfos.pop_front();
        fBridges.Remove(dynamic_cast<JackBridgeInterface*>(info->GetBackend()), false);
        CloseBridge(info);
    }
    if (fAudioDriver && !fAudioDriverClosed) {
        fAudioDriver->Detach();
        fAudioDriver->Close();
        fAudioDriverClosed = true;
    }
    fFreewheelDriver->Close();
    fEngine->Close();
    // TODO: move that in reworked JackServerGlobals::Destroy()
    JackMessageBuffer::Destroy();
    EndTime();
    return 0;
}

int JackServer::Start()
{
    jack_log("JackServer::Start");
    if (fAudioDriverClosed) {
        jack_error("JackServer::Start: no master driver (a previous master switch failed)");
        return -1;
    }
    if (fAudioDriver->Start() < 0) {
        return -1;
    }
    return fRequestChannel.Start();
}

int JackServer::Stop()
{
    jack_log("JackServer::Stop");
    int res = -1;
    
    if (fFreewheel) {
        if (fThreadedFreewheelDriver) {
            res = fThreadedFreewheelDriver->Stop();
        }
    } else {
        if (fAudioDriver && !fAudioDriverClosed) {
            res = fAudioDriver->Stop();
        } else {
            res = 0;
        }
    }
    
    fEngine->NotifyQuit();
    fRequestChannel.Stop();
    fEngine->NotifyFailure(JackFailure | JackServerError, JACK_SERVER_FAILURE);
    
    return res;
}

bool JackServer::IsRunning()
{
    jack_log("JackServer::IsRunning");
    assert(fAudioDriver);
    return fAudioDriver->IsRunning();
}

//------------------
// Internal clients 
//------------------

int JackServer::InternalClientLoad1(const char* client_name, const char* so_name, const char* objet_data, int options, int* int_ref, jack_uuid_t uuid, int* status)
{
    JackLoadableInternalClient* client = new JackLoadableInternalClient1(JackServerGlobals::fInstance, GetSynchroTable(), objet_data);
    assert(client);
    return InternalClientLoadAux(client, so_name, client_name, options, int_ref, uuid, status);
 }

int JackServer::InternalClientLoad2(const char* client_name, const char* so_name, const JSList * parameters, int options, int* int_ref, jack_uuid_t uuid, int* status)
{
    JackLoadableInternalClient* client = new JackLoadableInternalClient2(JackServerGlobals::fInstance, GetSynchroTable(), parameters);
    assert(client);
    return InternalClientLoadAux(client, so_name, client_name, options, int_ref, uuid, status);
}

int JackServer::InternalClientLoadAux(JackLoadableInternalClient* client, const char* so_name, const char* client_name, int options, int* int_ref, jack_uuid_t uuid, int* status)
{
    // Clear status
    *status = 0;

    // Client object is internally kept in JackEngine
    if ((client->Init(so_name) < 0) || (client->Open(JackTools::DefaultServerName(), client_name,  uuid, (jack_options_t)options, (jack_status_t*)status) < 0)) {
        delete client;
        int my_status1 = *status | JackFailure;
        *status = (jack_status_t)my_status1;
        *int_ref = 0;
        return -1;
    } else {
        *int_ref = client->GetClientControl()->fRefNum;
        return 0;
    }
 }

//-----------------------
// Internal session file 
//-----------------------

int JackServer::LoadInternalSessionFile(const char* file)
{
    JackInternalSessionLoader loader(this);
    return loader.Load(file);
}

//---------------------------
// From request thread : API 
//---------------------------

int JackServer::SetBufferSize(jack_nframes_t buffer_size)
{
    jack_log("JackServer::SetBufferSize nframes = %ld", buffer_size);
    JackLock lock(&fControlLock);
    jack_nframes_t current_buffer_size = fEngineControl->fBufferSize;

    if (fAudioDriverClosed) {
        jack_error("SetBufferSize: no master driver");
        return -1;
    }

    if (current_buffer_size == buffer_size) {
        jack_log("SetBufferSize: requirement for new buffer size equals current value");
        return 0;
    }

    if (fAudioDriver->IsFixedBufferSize()) {
        jack_log("SetBufferSize: driver only supports a fixed buffer size");
        return -1;
    }

    /*
    In freewheel mode the audio driver is already stopped and the freewheel
    thread runs the graph: it must be stopped around the change, and the
    audio driver must NOT be started (it used to be, leaving two drivers
    running cycles concurrently); it restarts when freewheel ends.
    */
    JackDriverClientInterface* running = fFreewheel ? fThreadedFreewheelDriver : fAudioDriver;

    if (running->Stop() != 0) {
        jack_error("Cannot stop %s driver", fFreewheel ? "freewheel" : "audio");
        return -1;
    }

    if (fAudioDriver->SetBufferSize(buffer_size) == 0) {
        // JackBus: the bridges follow (no allocation; they place their delays again)
        pthread_mutex_lock(&fBridgeLock);
        for (std::list<JackDriverInfo*>::iterator it = fBridgeInfos.begin(); it != fBridgeInfos.end(); it++) {
            JackBridgeInterface* bridge = dynamic_cast<JackBridgeInterface*>((*it)->GetBackend());
            if (bridge) {
                bridge->BridgeBufferSize(buffer_size);
            }
        }
        pthread_mutex_unlock(&fBridgeLock);
        fEngine->NotifyBufferSize(buffer_size);
        return running->Start();
    } else { // Failure: try to restore current value
        jack_error("Cannot SetBufferSize for audio driver, restore current value %ld", current_buffer_size);
        fAudioDriver->SetBufferSize(current_buffer_size);
        running->Start();
        // SetBufferSize actually failed, so return an error...
        return -1;
    }
}

/*
Freewheel mode is implemented by switching from the (audio [slaves] + freewheel) driver to the freewheel driver only:

    - "global" connection state is saved
    - all audio driver and slaves ports are deconnected, thus there is no more dependencies with the audio driver and slaves
    - the freewheel driver will be synchronized with the end of graph execution : all clients are connected to the freewheel driver
    - the freewheel driver becomes the "master"

Normal mode is restored with the connections state valid before freewheel mode was done. Thus one consider that
no graph state change can be done during freewheel mode.
*/

int JackServer::SetFreewheel(bool onoff)
{
    jack_log("JackServer::SetFreewheel is = %ld want = %ld", fFreewheel, onoff);
    JackLock lock(&fControlLock);

    if (fAudioDriverClosed) {
        jack_error("SetFreewheel: no master driver");
        return -1;
    }

    if (fFreewheel) {
        if (onoff) {
            return -1;
        } else {
            fFreewheel = false;
            fThreadedFreewheelDriver->Stop();
            fGraphManager->Restore(&fConnectionState);   // Restore connection state
            fEngine->NotifyFreewheel(onoff);
            fFreewheelDriver->SetMaster(false);
            fAudioDriver->SetMaster(true);
            return fAudioDriver->Start();
        }
    } else {
        if (onoff) {
            fFreewheel = true;
            fAudioDriver->Stop();
            fGraphManager->Save(&fConnectionState);     // Save connection state
            // Disconnect all slaves
            std::list<JackDriverInterface*> slave_list = fAudioDriver->GetSlaves();
            std::list<JackDriverInterface*>::const_iterator it;
            for (it = slave_list.begin(); it != slave_list.end(); it++) {
                JackDriver* slave = dynamic_cast<JackDriver*>(*it);
                assert(slave);
                fGraphManager->DisconnectAllPorts(slave->GetClientControl()->fRefNum);
            }
            // Disconnect master
            fGraphManager->DisconnectAllPorts(fAudioDriver->GetClientControl()->fRefNum);
            fEngine->NotifyFreewheel(onoff);
            fAudioDriver->SetMaster(false);
            fFreewheelDriver->SetMaster(true);
            return fThreadedFreewheelDriver->Start();
        } else {
            return -1;
        }
    }
}

//---------------------------
// Coming from the RT thread
//---------------------------

void JackServer::Notify(int refnum, int notify, int value)
{
    switch (notify) {

        case kGraphOrderCallback:
            fEngine->NotifyGraphReorder();
            break;

        case kXRunCallback:
            fEngine->NotifyClientXRun(refnum);
            break;

        case kCheckStuckClients:
            fEngine->CheckStuckClients();
#ifdef __linux__
            fRequestChannel.SweepDeadClients();
#endif
            break;
    }
}

//--------------------
// Backend management
//--------------------

JackDriverInfo* JackServer::AddSlave(jack_driver_desc_t* driver_desc, JSList* driver_params)
{
    JackDriverInfo* info = new JackDriverInfo();
    JackDriverClientInterface* slave = info->Open(driver_desc, fEngine, GetSynchroTable(), driver_params);
    
    if (!slave) {
        goto error1;
    }
    if (slave->Attach() < 0) {
        goto error2;
    }
    
    slave->SetMaster(false);
    fAudioDriver->AddSlave(slave);
    return info;

error2:
    slave->Close();
    
error1:
    delete info;
    return NULL;
}

void JackServer::RemoveSlave(JackDriverInfo* info)
{
    JackDriverClientInterface* slave = info->GetBackend();
    fAudioDriver->RemoveSlave(slave);
    slave->Detach();
    slave->Close();
}

/* JackBus: the master's ports and what they are connected to, by name (for SwitchMaster) */
void JackServer::SaveMasterConnections()
{
    JackGraphManager* graph = GetGraphManager();
    const char* client = fAudioDriver->GetClientControl()->fName;
    const size_t len = strlen(client);
    const char** ports = graph->GetPorts(NULL, NULL, 0);
    if (ports == NULL) {
        return;
    }
    for (int i = 0; ports[i] != NULL; i++) {
        if (strncmp(ports[i], client, len) != 0 || ports[i][len] != ':') {
            continue;
        }
        std::set<std::string>& peers = fMasterConnections[ports[i]];
        peers.clear();
        const char** conns = graph->GetConnections(graph->GetPort(ports[i]));
        if (conns != NULL) {
            for (int j = 0; conns[j] != NULL; j++) {
                peers.insert(conns[j]);
            }
            free(conns);
        }
    }
    free(ports);
}

void JackServer::RestoreMasterConnections()
{
    JackGraphManager* graph = GetGraphManager();
    const int refnum = fAudioDriver->GetClientControl()->fRefNum;
    int restored = 0;
    for (std::map<std::string, std::set<std::string> >::iterator it = fMasterConnections.begin(); it != fMasterConnections.end();) {
        jack_port_id_t index = graph->GetPort(it->first.c_str());
        if (index == NO_PORT) {
            ++it;           /* this master has no such port: keep it for a later one */
            continue;
        }
        const bool output = (graph->GetPort(index)->GetFlags() & JackPortIsOutput) != 0;
        for (std::set<std::string>::const_iterator peer = it->second.begin(); peer != it->second.end(); ++peer) {
            if (graph->GetPort(peer->c_str()) == NO_PORT) {
                continue;   /* its client has gone meanwhile */
            }
            const char* src = output ? it->first.c_str() : peer->c_str();
            const char* dst = output ? peer->c_str() : it->first.c_str();
            if (fEngine->PortConnect(refnum, src, dst) == 0) {
                restored++;
            }
        }
        fMasterConnections.erase(it++);
    }
    if (restored > 0) {
        jack_info("Master driver switched: %d connection(s) to its ports restored", restored);
    }
}

int JackServer::SwitchMaster(jack_driver_desc_t* driver_desc, JSList* driver_params)
{
    std::list<JackDriverInterface*> slave_list;
    std::list<JackDriverInterface*>::const_iterator it;
    JackLock lock(&fControlLock);
    const jack_nframes_t old_buffer_size = fEngineControl->fBufferSize;
    const jack_nframes_t old_sample_rate = fEngineControl->fSampleRate;

    /*
    Failure handling: the old master is closed before the new one is opened
    (they may use the same device). If the new one cannot be opened or
    attached, the server is left without a running master
    (fAudioDriverClosed): Start/Stop/Close/SetBufferSize/SetFreewheel skip it
    and a later successful SwitchMaster recovers. Previously the old, closed
    driver kept being used (stopped and closed again: use-after-free / double
    free in e.g. the ALSA driver), and a failed Attach() deleted the new
    driver info that fDriverInfo already pointed to (double free on close).
    */

    // Remove current master (JackBus: its connections are kept, by port name)
    if (!fAudioDriverClosed) {
        SaveMasterConnections();
        fAudioDriver->Stop();
        fAudioDriver->Detach();
        fAudioDriver->Close();
        fAudioDriverClosed = true;
    }

    // Open new master
    JackDriverInfo* info = new JackDriverInfo();
    JackDriverClientInterface* master = info->Open(driver_desc, fEngine, GetSynchroTable(), driver_params);

    if (!master) {
        jack_error("SwitchMaster: cannot open the new master driver");
        delete info;
        return -1;
    }

    // Get slaves list
    slave_list = fAudioDriver->GetSlaves();

    // Move slaves in new master
    for (it = slave_list.begin(); it != slave_list.end(); it++) {
        JackDriverInterface* slave = *it;
        master->AddSlave(slave);
    }

    // Delete old master (closed)
    delete fDriverInfo;

    // Activate master
    fAudioDriver = master;
    fDriverInfo = info;

    if (fAudioDriver->Attach() < 0) {
        jack_error("SwitchMaster: cannot attach the new master driver");
        fAudioDriver->Close();
        return -1;  // fDriverInfo owns the (closed) new driver
    }
    fAudioDriverClosed = false;
    RestoreMasterConnections();
    BridgesFollowMaster(old_buffer_size, old_sample_rate);

    // Notify clients of new values (JackBus: only those that changed)
    if (fEngineControl->fBufferSize != old_buffer_size) {
        fEngine->NotifyBufferSize(fEngineControl->fBufferSize);
    }
    if (fEngineControl->fSampleRate != old_sample_rate) {
        fEngine->NotifySampleRate(fEngineControl->fSampleRate);
    }

    // And finally start (JackBus: the bridges go on with the new master; their delays are
    // placed again, as after a master xrun)
    fAudioDriver->SetMaster(true);
    fAudioDriver->SetBridges(&fBridges);
    return fAudioDriver->Start();
}

//------------------------------------
// JackBus: sound card bridges
//------------------------------------

void JackServer::BridgesFollowMaster(jack_nframes_t old_buffer_size, jack_nframes_t old_sample_rate)
{
    const jack_nframes_t rate = fEngineControl->fSampleRate;
    const jack_nframes_t n = fEngineControl->fBufferSize;
    if (rate == old_sample_rate && n == old_buffer_size) {
        return;
    }
    pthread_mutex_lock(&fBridgeLock);
    for (std::list<JackDriverInfo*>::iterator it = fBridgeInfos.begin(); it != fBridgeInfos.end(); it++) {
        JackBridgeInterface* bridge = dynamic_cast<JackBridgeInterface*>((*it)->GetBackend());
        if (!bridge) {
            continue;
        }
        // a new rate sets the bridge up afresh (for the new buffer size too)
        if (rate != old_sample_rate) {
            bridge->BridgeSampleRate(rate);
        } else {
            bridge->BridgeBufferSize(n);
        }
    }
    pthread_mutex_unlock(&fBridgeLock);
}

void JackServer::CloseBridge(JackDriverInfo* info)
{
    JackDriverClientInterface* driver = info->GetBackend();
    if (driver) {
        driver->Stop();
        driver->Detach();
        driver->Close();
    }
    delete info;
}

int JackServer::AddBridge(jack_driver_desc_t* driver_desc, JSList* driver_params, char* name)
{
    JackDriverInfo* info = new JackDriverInfo();
    // the bridge driver's factory opens the card and the bridge's client (OpenBridge)
    JackDriverClientInterface* driver = info->Open(driver_desc, fEngine, GetSynchroTable(), driver_params);
    if (!driver) {
        jack_error("AddBridge: cannot open the bridge");
        delete info;
        return -1;
    }
    JackBridgeInterface* bridge = dynamic_cast<JackBridgeInterface*>(driver);
    if (!bridge) {
        jack_error("AddBridge: %s is not a bridge driver", driver_desc->name);
        driver->Close();
        delete info;
        return -1;
    }
    driver->SetMaster(false);
    if (driver->Attach() < 0) {
        jack_error("AddBridge: cannot register the bridge's ports");
        driver->Close();
        delete info;
        return -1;
    }
    if (driver->Start() < 0) {
        jack_error("AddBridge: cannot start the bridge's card");
        driver->Detach();
        driver->Close();
        delete info;
        return -1;
    }
    const bool running = fAudioDriver && !fAudioDriverClosed && fAudioDriver->IsRunning();
    pthread_mutex_lock(&fBridgeLock);
    if (!fBridges.Add(bridge, running)) {
        pthread_mutex_unlock(&fBridgeLock);
        CloseBridge(info);
        return -1;
    }
    fBridgeInfos.push_back(info);
    pthread_mutex_unlock(&fBridgeLock);
    StartBridgeMonitor();
    if (name) {
        snprintf(name, JACK_CLIENT_NAME_SIZE + 1, "%s", driver->GetClientControl()->fName);
    }
    return 0;
}

void* JackServer::BridgeMonitorEntry(void* arg)
{
    JackServer* server = (JackServer*)arg;
    while (!server->fBridgeMonitorStop.load(std::memory_order_acquire)) {
        pthread_mutex_lock(&server->fBridgeLock);
        for (std::list<JackDriverInfo*>::iterator it = server->fBridgeInfos.begin(); it != server->fBridgeInfos.end(); it++) {
            JackBridgeInterface* bridge = dynamic_cast<JackBridgeInterface*>((*it)->GetBackend());
            if (bridge) {
                bridge->BridgeMonitor();
            }
        }
        pthread_mutex_unlock(&server->fBridgeLock);
        struct timespec quarter = { 0, 250000000 };
        nanosleep(&quarter, NULL);
    }
    return NULL;
}

void JackServer::StartBridgeMonitor()
{
    if (fBridgeMonitorRunning) {
        return;
    }
    // a plain thread, whatever the caller's scheduling
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    pthread_attr_setschedparam(&attr, &sp);
    fBridgeMonitorStop.store(false, std::memory_order_release);
    fBridgeMonitorRunning = (pthread_create(&fBridgeMonitor, &attr, BridgeMonitorEntry, this) == 0);
    pthread_attr_destroy(&attr);
}

void JackServer::StopBridgeMonitor()
{
    if (!fBridgeMonitorRunning) {
        return;
    }
    fBridgeMonitorStop.store(true, std::memory_order_release);
    pthread_join(fBridgeMonitor, NULL);
    fBridgeMonitorRunning = false;
}

int JackServer::RemoveBridge(const char* name)
{
    for (std::list<JackDriverInfo*>::iterator it = fBridgeInfos.begin(); it != fBridgeInfos.end(); it++) {
        JackDriverClientInterface* driver = (*it)->GetBackend();
        if (!driver || strcmp(driver->GetClientControl()->fName, name) != 0) {
            continue;
        }
        const bool running = fAudioDriver && !fAudioDriverClosed && fAudioDriver->IsRunning();
        pthread_mutex_lock(&fBridgeLock);
        if (!fBridges.Remove(dynamic_cast<JackBridgeInterface*>(driver), running)) {
            pthread_mutex_unlock(&fBridgeLock);
            // the master may still use it: leave it open (leaked, never freed under it)
            jack_error("RemoveBridge: %s is still in use by the master, left open", name);
            return -1;
        }
        JackDriverInfo* info = *it;
        fBridgeInfos.erase(it);
        pthread_mutex_unlock(&fBridgeLock);
        CloseBridge(info);
        return 0;
    }
    jack_error("RemoveBridge: no bridge %s", name);
    return -1;
}

int JackServer::GetBridgeNames(char names[][JACK_CLIENT_NAME_SIZE + 1], int max)
{
    int n = 0;
    pthread_mutex_lock(&fBridgeLock);
    for (std::list<JackDriverInfo*>::iterator it = fBridgeInfos.begin(); it != fBridgeInfos.end() && n < max; it++) {
        JackDriverClientInterface* driver = (*it)->GetBackend();
        if (driver) {
            snprintf(names[n++], JACK_CLIENT_NAME_SIZE + 1, "%s", driver->GetClientControl()->fName);
        }
    }
    pthread_mutex_unlock(&fBridgeLock);
    return n;
}

int JackServer::GetBridgeStatus(jackctl_bridge_status* status, int max)
{
    int n = 0;
    pthread_mutex_lock(&fBridgeLock);
    for (std::list<JackDriverInfo*>::iterator it = fBridgeInfos.begin(); it != fBridgeInfos.end() && n < max; it++) {
        JackBridgeInterface* bridge = dynamic_cast<JackBridgeInterface*>((*it)->GetBackend());
        if (bridge) {
            bridge->BridgeStatus(&status[n++]);
        }
    }
    pthread_mutex_unlock(&fBridgeLock);
    return n;
}

//----------------------
// Transport management
//----------------------

int JackServer::ReleaseTimebase(int refnum)
{
    return fEngineControl->fTransport.ResetTimebase(refnum);
}

int JackServer::SetTimebaseCallback(int refnum, int conditional)
{
    return fEngineControl->fTransport.SetTimebaseMaster(refnum, conditional);
}

JackLockedEngine* JackServer::GetEngine()
{
    return fEngine;
}

JackSynchro* JackServer::GetSynchroTable()
{
    return fSynchroTable;
}

JackEngineControl* JackServer::GetEngineControl()
{
    return fEngineControl;
}

JackGraphManager* JackServer::GetGraphManager()
{
    return fGraphManager;
}

JackMetadata* JackServer::GetMetadata()
{
    return fMetadata;
}

} // end of namespace

