/*
 *  Copyright (C) 2004 Rui Nuno Capela, Steve Harris
 *  Copyright (C) 2008 Nedko Arnaudov
 *  Copyright (C) 2008 Grame
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "JackMessageBuffer.h"
#include "JackGlobals.h"
#include "JackError.h"
#include "JackTime.h"

namespace Jack
{

JackMessageBuffer* JackMessageBuffer::fInstance = NULL;

JackMessageBuffer::JackMessageBuffer()
    :fInit(NULL),
    fInitArg(NULL),
    fThread(this),
    fGuard(),
    fOverruns(0),
    fRunning(false)
{
    static_assert(offsetof(JackMessageBuffer, fOverruns) % sizeof(fOverruns) == 0,
                  "fOverruns must be aligned within JackMessageBuffer");
}

JackMessageBuffer::~JackMessageBuffer()
{}

bool JackMessageBuffer::Start()
{
    // Before StartSync()...
    fRunning = true;
    if (fThread.StartSync() == 0) {
        return true;
    } else {
        fRunning = false;
        return false;
    }
}

bool JackMessageBuffer::Stop()
{
    if (fOverruns > 0) {
        jack_error("WARNING: %d message buffer overruns!", fOverruns);
    } else {
        jack_log("no message buffer overruns");
    }

    if (fGuard.Lock()) {
        fRunning = false;
        fGuard.Signal();
        fGuard.Unlock();
        fThread.Stop();
    } else {
        fThread.Kill();
    }

    Flush();
    return true;
}

void JackMessageBuffer::Flush()
{
    while (fQueue.TryPop([](JackMessage& msg) { jack_log_function(msg.level, msg.message); })) {}
}

/*
 Called from realtime threads (through JackMessageBufferAdd as their log
 function): no lock and no system call. The previous version took a trylock
 and signalled a condition variable for every message (glibc condvars
 serialize on an internal lock that is not priority-inheriting), dropped the
 message on lock contention and, without a full check, overwrote unread
 messages once more than MB_BUFFERS were queued.
*/
void JackMessageBuffer::AddMessage(int level, const char *message)
{
    bool queued = fQueue.TryPush([&](JackMessage& msg) {
        msg.level = level;
        strncpy(msg.message, message, MB_BUFFERSIZE - 1);
        msg.message[MB_BUFFERSIZE - 1] = 0;
    });
    if (!queued) {
        INC_ATOMIC(&fOverruns);
    }
}

bool JackMessageBuffer::Execute()
{
    if (fGuard.Lock()) {
        while (fRunning) {
            // Producers never wake us: poll (Stop()/SetInitCallback() signal).
            fGuard.TimedWait(MB_FLUSH_INTERVAL_USEC);
            /* the client asked for all threads to run a thread
            initialization callback, which includes us.
            */
            if (fInit) {
                fInit(fInitArg);
                fInit = NULL;
                /* and we're done */
                fGuard.Signal();
            }

            /* releasing the mutex reduces contention */
            fGuard.Unlock();
            Flush();
            fGuard.Lock();
        }
        fGuard.Unlock();
    } else {
        jack_error("JackMessageBuffer::Execute lock cannot be taken");
    }

    return false;
}

/*
 jack2-nspa: the instance is never freed. Destroy() used to delete it while a thread whose log
 function is JackMessageBufferAdd (a realtime thread still finishing) could have read the pointer
 and be writing into it: a use after free, and with the next Create() at the same address, two
 threads on one cell (seen by ThreadSanitizer as a race between AddMessage and the log
 callback). Destroy() now stops the thread, flushes and unpublishes the instance; the next
 Create() starts the same one again (a message pushed after the last flush is printed then).
 fInstance is read by realtime threads: atomic accesses, no lock.
*/
static JackMessageBuffer* gKeptMessageBuffer = NULL;   // Create/Destroy: the server's control thread

bool JackMessageBuffer::Create()
{
    if (__atomic_load_n(&fInstance, __ATOMIC_ACQUIRE) == NULL) {
        if (gKeptMessageBuffer == NULL) {
            gKeptMessageBuffer = new JackMessageBuffer();
        }
        __atomic_store_n(&gKeptMessageBuffer->fOverruns, 0, __ATOMIC_RELAXED);
        if (!gKeptMessageBuffer->Start()) {
            jack_error("JackMessageBuffer::Create cannot start thread");
            return false;
        }
        __atomic_store_n(&fInstance, gKeptMessageBuffer, __ATOMIC_RELEASE);
    }

    return true;
}

bool JackMessageBuffer::Destroy()
{
    JackMessageBuffer* instance = __atomic_load_n(&fInstance, __ATOMIC_ACQUIRE);
    if (instance != NULL) {
        instance->Stop();
        __atomic_store_n(&fInstance, (JackMessageBuffer*)NULL, __ATOMIC_RELEASE);
        return true;
    } else {
        return false;
    }
}

void JackMessageBufferAdd(int level, const char *message)
{
    Jack::JackMessageBuffer* instance = __atomic_load_n(&Jack::JackMessageBuffer::fInstance, __ATOMIC_ACQUIRE);
    if (instance == NULL) {
        /* Unable to print message with realtime safety. Complain and print it anyway. */
        jack_log_function(LOG_LEVEL_ERROR, "messagebuffer not initialized, skip message");
    } else {
        instance->AddMessage(level, message);
    }
}

int JackMessageBuffer::SetInitCallback(JackThreadInitCallback callback, void *arg)
{
    if (__atomic_load_n(&fInstance, __ATOMIC_ACQUIRE) && callback && fRunning && fGuard.Lock()) {
        /* set up the callback */
        fInitArg = arg;
        fInit = callback;
        
    #ifndef WIN32
        // wake msg buffer thread 
        fGuard.Signal();
        // wait for it to be done  
        fGuard.Wait();
        // and we're done 
        fGuard.Unlock();
    #else
        /*
        The condition variable emulation code does not work reliably on Windows (lost signal).
        So use a "hackish" way to signal/wait for the result.
        Probably better in the long term : use pthread-win32 (http://sourceware.org/pthreads-win32/`
        */
        fGuard.Unlock();
        int count = 0;
        while (fInit && ++count < 1000) {
            /* wake msg buffer thread */
            fGuard.Signal();
            JackSleep(1000);
        }
        if (count == 1000) {
            jack_error("JackMessageBuffer::SetInitCallback : signal lost");
            return -1;
	}
    #endif
        return 0;
    }
    jack_error("JackMessageBuffer::SetInitCallback : callback could not be executed");
    return -1;
}

};

