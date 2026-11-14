/*
Copyright (C) 2005 Grame

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

#ifndef __JackLibGlobals__
#define __JackLibGlobals__

#include "JackShmMem.h"
#include "JackEngineControl.h"
#include "JackGlobals.h"
#include "JackPlatformPlug.h"
#include "JackGraphManager.h"
#include "JackMessageBuffer.h"
#include "JackTime.h"
#include "JackClient.h"
#include "JackError.h"
#ifdef __linux__
#include "JackAliveTable.h"
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifdef WIN32
#ifdef __MINGW32__
#include <sys/types.h>
typedef _sigset_t sigset_t;
#else
typedef HANDLE sigset_t;
#endif
#endif

namespace Jack
{

class JackClient;

/*!
\brief Global library static structure: singleton kind of pattern.
*/

struct JackLibGlobals
{
    JackShmReadWritePtr<JackGraphManager> fGraphManager;	/*! Shared memory Port manager */
    JackShmReadWritePtr<JackEngineControl> fEngineControl;	/*! Shared engine control */  // transport engine has to be writable
    JackSynchro fSynchroTable[CLIENT_NUM];                  /*! Shared synchro table */
    JackMetadata *fMetadata;                                /*! Shared metadata base */
    sigset_t fProcessSignals;
#ifdef __linux__
    /*! Slots telling the server at once when one of this process' clients' realtime
        threads ends (JackAliveTable), if the server provides them. Mapped when the
        first client opens; unmapped with the globals, after the last client closed:
        every realtime thread has been ended and joined by then. */
    JackAliveTable fAliveTable;
    char fAliveServer[JACK_SERVER_NAME_SIZE + 1];
    bool fAliveTried;
#endif

    static int fClientCount;
    static JackLibGlobals* fGlobals;

    JackLibGlobals()
    {
        jack_log("JackLibGlobals");
        if (!JackMessageBuffer::Create()) {
            jack_error("Cannot create message buffer");
        }
        fGraphManager = -1;
        fEngineControl = -1;
#ifdef __linux__
        fAliveServer[0] = 0;
        fAliveTried = false;
#endif

        fMetadata = new JackMetadata(false);

        // Filter SIGPIPE to avoid having client get a SIGPIPE when trying to access a died server.
    #ifdef WIN32
        // TODO
    #else
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGPIPE);
        sigprocmask(SIG_BLOCK, &signals, &fProcessSignals);
    #endif
    }

    ~JackLibGlobals()
    {
        jack_log("~JackLibGlobals");
        for (int i = 0; i < CLIENT_NUM; i++) {
            fSynchroTable[i].Disconnect();
        }
        JackMessageBuffer::Destroy();

        delete fMetadata;
        fMetadata = NULL;

       // Restore old signal mask
    #ifdef WIN32
       // TODO
    #else
       sigprocmask(SIG_BLOCK, &fProcessSignals, 0);
    #endif
    }

#ifdef __linux__
    // Under JackGlobals::fOpenMutex (jack_client_open). NULL if the server has no
    // such slots, or for a client of a second server in the same process.
    JackAliveTable* AliveTable(const char* server_name)
    {
        if (!fAliveTried) {
            fAliveTried = true;
            strncpy(fAliveServer, server_name, JACK_SERVER_NAME_SIZE);
            fAliveServer[JACK_SERVER_NAME_SIZE] = 0;
            // tests: behave as a libjack without slots
            if (getenv("JACK_NO_ALIVE_SLOT") == NULL) {
                fAliveTable.Connect(server_name);
            }
        }
        if (!fAliveTable.IsMapped() || strcmp(fAliveServer, server_name) != 0) {
            return NULL;
        }
        return &fAliveTable;
    }
#endif

    static void Init()
    {
        if (!__atomic_load_n(&JackGlobals::fServerRunning, __ATOMIC_RELAXED) && fClientCount > 0) {

            // Cleanup remaining clients
            jack_error("Jack server was closed but clients are still allocated, cleanup...");
            for (int i = 0; i < CLIENT_NUM; i++) {
                JackClient* client = JackGlobals::fClientTable[i];
                if (client) {
                    jack_error("Cleanup client ref = %d", i);
                    client->Close();
                    delete client;
                }
            }

            // Cleanup global context
            fClientCount = 0;
            delete fGlobals;
            fGlobals = NULL;
        }

        if (fClientCount++ == 0 && !fGlobals) {
            jack_log("JackLibGlobals Init %x", fGlobals);
            InitTime();
            fGlobals = new JackLibGlobals();
        }
    }

    static void Destroy()
    {
        if (--fClientCount == 0 && fGlobals) {
            jack_log("JackLibGlobals Destroy %x", fGlobals);
            EndTime();
            delete fGlobals;
            fGlobals = NULL;
        }
    }

};

} // end of namespace

#endif

