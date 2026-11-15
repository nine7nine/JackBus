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

#ifndef __JackAliveTable__
#define __JackAliveTable__

#include "JackConstants.h"
#include "JackCompilerDeps.h"
#include <stdint.h>

namespace Jack
{

/*!
\brief Tells the server at once that a client's realtime thread is gone.

 The server learns that a client died from its request socket closing, and the
 kernel closes a dying process' files only after tearing down its memory (do_exit:
 exit_mm before exit_files), 100 ms or more for a big process. The kernel deals
 with a thread's robust futexes first, though: at the start of exit_mm, each robust
 futex the dying thread holds is marked FUTEX_OWNER_DIED and one waiter is woken,
 whichever way the thread ends (SIGKILL, a crash, exit, its last function returning).

 So: one slot per client refnum in a small shared memory file the server provides
 (jack_alive.<uid>_<server>). libjack makes its client's slot a process-shared
 robust mutex, which the client's realtime thread locks when it starts and holds
 for as long as it lives; the server waits on the slots of its active clients
 (futex_waitv, JackEngineNotifier) and knows within microseconds when one of those
 threads is gone.

 The two sides share only the kernel's robust futex protocol on the first 32-bit
 word of a slot (owner TID, FUTEX_WAITERS, FUTEX_OWNER_DIED): libjack initializes
 the mutex in its own ABI's layout (64- or 32-bit glibc, both start with that word),
 the server never calls pthread functions on it. A libjack without this, or a
 server without the file, simply do without (an old libjack never locks its slot,
 and the server leaves unlocked slots alone).
*/

class SERVER_EXPORT JackAliveTable
{

    public:

        static const int kSlotSize = 64;    // one cache line, holds a pthread_mutex_t

    private:

        char fName[SYNC_MAX_NAME_SIZE + 1];
        char* fBase;
        bool fOwner;                        // server side: removes the file
        bool fPromiscuous;
        int fPromiscuousGid;

        void BuildName(const char* server_name, char* res, int size);

    public:

        JackAliveTable();
        ~JackAliveTable();

        // Server: create the file (all slots cleared), or remove it
        bool Allocate(const char* server_name);
        void Destroy();
        // Server: clear a slot no thread holds any more (client opened or gone)
        void ClearSlot(int refnum);
        // Server: the robust futex word of a slot, NULL if no table
        uint32_t* Word(int refnum) const;

        // Server: a new fd for the file (JackBus socket clients), -1 if none
        int OpenFd() const;

        // Client (libjack): map the server's file, if there is one, or unmap it
        bool Connect(const char* server_name);
        // Client: the same, from an fd received over the JackBus socket (takes
        // ownership of fd); true if mapped, or already mapped (fd closed then)
        bool ConnectFd(int fd);
        void Disconnect();
        // Client: make a slot a process-shared robust mutex, unlocked
        bool InitSlot(int refnum);
        // Client, from the realtime thread: lock the slot for the thread's lifetime
        bool LockSlot(int refnum);

        bool IsMapped() const { return fBase != 0; }
        const char* GetName() const { return fName; }
};

} // end of namespace

#endif
