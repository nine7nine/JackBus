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

#ifndef __JackGlobals__
#define __JackGlobals__

#include "JackPlatformPlug.h"
#include "JackSystemDeps.h"
#include "JackConstants.h"

#ifdef __CLIENTDEBUG__
#include <iostream>
#include <fstream>
#include <string>
#include <time.h>
#endif

namespace Jack
{

// Globals used for client management on server or library side.
struct JackGlobals {

    /*
     Per-thread state: plain __thread pointers instead of pthread keys. Built
     with TLS descriptors (-mtls-dialect=gnu2), a read is one indirect call to
     a two-instruction resolver when libjack sits in static TLS, which glibc
     arranges even when libjack is dlopen()ed; it never touches the DTV, so
     it cannot allocate on an RT thread. Zero-initialized, so there is no
     static-initialization-order dependency either.
     fRealTimeThread: non-NULL in the client's process thread and in RT
     threads created through jack_client_create_thread().
     fNotificationThread: non-NULL in the client's notification thread.
     fKeyLogFunction: per-thread log function (jack_log_function_t), or NULL.
    */
    static __thread void* fRealTimeThread;
    static __thread void* fNotificationThread;
    static __thread void* fKeyLogFunction;
    static JackMutex* fOpenMutex;
    static JackMutex* fSynchroMutex;
    static volatile bool fServerRunning;
    static JackClient* fClientTable[CLIENT_NUM];
    static bool fVerbose;
#ifndef WIN32
    static jack_thread_creator_t fJackThreadCreator;
#endif

#ifdef __CLIENTDEBUG__
    static std::ofstream* fStream;
#endif
#ifdef __CLIENTDEBUG__
     static void CheckContext(const char* name);
#else
     // Inline no-op: every API entry point calls it, jack_port_get_buffer included
     static void CheckContext(const char*) {}
#endif
};

// Each "side" server and client will implement this to get the shared graph manager, engine control and inter-process synchro table.
extern SERVER_EXPORT JackGraphManager* GetGraphManager();
extern SERVER_EXPORT JackEngineControl* GetEngineControl();
extern SERVER_EXPORT JackSynchro* GetSynchroTable();
extern SERVER_EXPORT JackMetadata* GetMetadata();

} // end of namespace

#endif
