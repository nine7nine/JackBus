/*
Copyright (C) 2004-2008 Grame
Copyright (C) 2016 Filipe Coelho

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

#include "JackLinuxFutex.h"
#include "JackTools.h"
#include "JackConstants.h"
#include "JackError.h"
#include "promiscuous.h"
#include <climits>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <syscall.h>
#include <unistd.h>
#include <linux/futex.h>

#if !defined(SYS_futex) && defined(SYS_futex_time64)
#define SYS_futex SYS_futex_time64
#endif

/*
 ThreadSanitizer does not model raw futex system calls as synchronization:
 tell it that Signal()s happen-before later Wait() returns, so the graph's
 ordering of clients in one process (e.g. a client reading the output buffer
 of the client it follows) is not reported as a race. No-op in normal builds.
*/
#if defined(__SANITIZE_THREAD__)
extern "C" void __tsan_acquire(void* addr);
extern "C" void __tsan_release(void* addr);
/* One address for all futexes: a cycle's order is client -> server -> next
   client, and the server is another process. Pairing every Signal() with every
   later Wait() return in this process captures that order (over-approximates
   synchronization slightly; TSan builds only). */
static char gJackTsanCycleOrder;
#define JACK_TSAN_RELEASE(addr) __tsan_release(&gJackTsanCycleOrder)
#define JACK_TSAN_ACQUIRE(addr) __tsan_acquire(&gJackTsanCycleOrder)
#else
#define JACK_TSAN_RELEASE(addr)
#define JACK_TSAN_ACQUIRE(addr)
#endif

namespace Jack
{

#if defined(__SANITIZE_THREAD__)
// JackActivationCount: a counter decremented without a Signal()
void JackTsanCycleRelease()
{
    JACK_TSAN_RELEASE(NULL);
}
#endif

JackLinuxFutex::JackLinuxFutex() : JackSynchro(), fSharedMem(-1), fFutex(NULL), fPrivate(false)
{
    const char* promiscuous = getenv("JACK_PROMISCUOUS_SERVER");
    fPromiscuous = (promiscuous != NULL);
    fPromiscuousGid = jack_group2gid(promiscuous);
}

void JackLinuxFutex::BuildName(const char* client_name, const char* server_name, char* res, int size)
{
    char ext_client_name[SYNC_MAX_NAME_SIZE + 1];
    JackTools::RewriteName(client_name, ext_client_name);
    if (fPromiscuous) {
        snprintf(res, size, "jack_sem.%s_%s", server_name, ext_client_name);
    } else {
        snprintf(res, size, "jack_sem.%d_%s_%s", JackTools::GetUID(), server_name, ext_client_name);
    }
}

bool JackLinuxFutex::Signal()
{
    if (!fFutex) {
        jack_error("JackLinuxFutex::Signal name = %s already deallocated!!", fName);
        return false;
    }

    // before the flush check: a flushed signal (to the driver, end of graph)
    // still orders this client's buffer accesses before the next cycle
    JACK_TSAN_RELEASE(fFutex);

    if (__atomic_load_n(&fFlush, __ATOMIC_RELAXED)) {
        return true;
    }

    if (! __sync_bool_compare_and_swap(&fFutex->futex, 0, 1))
    {
        // already unlocked, do not wake futex
        if (! fFutex->internal) return true;
    }

    ::syscall(SYS_futex, fFutex, fFutex->internal ? FUTEX_WAKE_PRIVATE : FUTEX_WAKE, 1, NULL, NULL, 0);
    return true;
}

bool JackLinuxFutex::SignalAll()
{
    return Signal();
}

bool JackLinuxFutex::Wait()
{
    if (!fFutex) {
        jack_error("JackLinuxFutex::Wait name = %s already deallocated!!", fName);
        return false;
    }

    if (fFutex->needsChange)
    {
        fFutex->needsChange = false;
        fFutex->internal = !fFutex->internal;
    }

    const int wait_mode = fFutex->internal ? FUTEX_WAIT_PRIVATE : FUTEX_WAIT;

    for (;;)
    {
        if (__sync_bool_compare_and_swap(&fFutex->futex, 1, 0)) {
            JACK_TSAN_ACQUIRE(fFutex);
            return true;
        }

        if (::syscall(SYS_futex, fFutex, wait_mode, 0, NULL, NULL, 0) != 0)
            if (errno != EAGAIN && errno != EINTR)
                return false;
    }
}

bool JackLinuxFutex::TimedWait(long usec)
{
    if (usec == LONG_MAX)
        return Wait();

    if (!fFutex) {
        jack_error("JackLinuxFutex::TimedWait name = %s already deallocated!!", fName);
        return false;
     }

    if (fFutex->needsChange)
    {
        fFutex->needsChange = false;
        fFutex->internal = !fFutex->internal;
    }

    const uint secs  =  usec / 1000000;
    const int  nsecs = (usec % 1000000) * 1000;

    const timespec timeout = { static_cast<time_t>(secs), nsecs };
    const int wait_mode = fFutex->internal ? FUTEX_WAIT_PRIVATE : FUTEX_WAIT;

    for (;;)
    {
        if (__sync_bool_compare_and_swap(&fFutex->futex, 1, 0)) {
            JACK_TSAN_ACQUIRE(fFutex);
            return true;
        }

        if (::syscall(SYS_futex, fFutex, wait_mode, 0, &timeout, NULL, 0) != 0)
            if (errno != EAGAIN && errno != EINTR)
                return false;
    }
}

// Server side : publish the futex in the global namespace
bool JackLinuxFutex::Allocate(const char* name, const char* server_name, int value, bool internal)
{
    BuildName(name, server_name, fName, sizeof(fName));
    jack_log("JackLinuxFutex::Allocate name = %s val = %ld", fName, value);

    /* A file of the same name can be left over from an earlier server that
       stopped (or crashed) while this client name was connected, and a hung
       process of that client may still have it mapped and be waiting on it.
       Reusing it would let that process take this client's wakeups (the new
       client then misses cycles: "was not finished, state = Triggered").
       Start from a new file; only reuse one that cannot be removed. */
    shm_unlink(fName);
    if ((fSharedMem = shm_open(fName, O_CREAT | O_EXCL | O_RDWR, 0777)) < 0) {
        if (errno != EEXIST || (fSharedMem = shm_open(fName, O_CREAT | O_RDWR, 0777)) < 0) {
            jack_error("Allocate: can't check in named futex name = %s err = %s", fName, strerror(errno));
            return false;
        }
        jack_error("Allocate: reusing named futex %s left by an earlier server", fName);
    }

    if (ftruncate(fSharedMem, sizeof(FutexData)) != 0) {
        jack_error("Allocate: can't set shared memory size in named futex name = %s err = %s", fName, strerror(errno));
        return false;
    }

    if (fPromiscuous && (jack_promiscuous_perms(fSharedMem, fName, fPromiscuousGid) < 0)) {
        close(fSharedMem);
        fSharedMem = -1;
        shm_unlink(fName);
        return false;
    }

    FutexData* futex = (FutexData*)mmap(NULL, sizeof(FutexData), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_LOCKED, fSharedMem, 0);

    if (futex == NULL || futex == MAP_FAILED) {
        jack_error("Allocate: can't check in named futex name = %s err = %s", fName, strerror(errno));
        close(fSharedMem);
        fSharedMem = -1;
        shm_unlink(fName);
        return false;
    }

    fPrivate = internal;

    futex->futex = value;
    futex->internal = internal;
    futex->wasInternal = internal;
    futex->needsChange = false;
    futex->externalCount = 0;
    fFutex = futex;
    return true;
}

// Client side : get the published futex from server
bool JackLinuxFutex::Connect(const char* name, const char* server_name)
{
    BuildName(name, server_name, fName, sizeof(fName));
    jack_log("JackLinuxFutex::Connect name = %s", fName);

    // Temporary...
    if (fFutex) {
        jack_log("Already connected name = %s", name);
        return true;
    }

    if ((fSharedMem = shm_open(fName, O_RDWR, 0)) < 0) {
        jack_error("Connect: can't connect named futex name = %s err = %s", fName, strerror(errno));
        return false;
    }

    FutexData* futex = (FutexData*)mmap(NULL, sizeof(FutexData), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_LOCKED, fSharedMem, 0);

    if (futex == NULL || futex == MAP_FAILED) {
        jack_error("Connect: can't connect named futex name = %s err = %s", fName, strerror(errno));
        close(fSharedMem);
        fSharedMem = -1;
        return false;
    }

    if (! fPrivate && futex->wasInternal)
    {
        const char* externalSync = getenv("JACK_INTERNAL_CLIENT_SYNC");

        if (externalSync != NULL && strstr(fName, externalSync) != NULL && ++futex->externalCount == 1)
        {
            jack_error("Note: client %s running as external client temporarily", fName);
            futex->needsChange = true;
        }
    }

    fFutex = futex;
    return true;
}

bool JackLinuxFutex::ConnectFd(int fd)
{
    if (fFutex) {
        // another client of this process connected it already (shared table):
        // as Connect() by name, true whether or not an fd came
        if (fd >= 0) {
            close(fd);
        }
        return true;
    }
    if (fd < 0) {
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(FutexData)) {
        jack_error("ConnectFd: not a futex file (fd %d)", fd);
        close(fd);
        return false;
    }

    FutexData* futex = (FutexData*)mmap(NULL, sizeof(FutexData), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_LOCKED, fd, 0);
    if (futex == NULL || futex == MAP_FAILED) {
        jack_error("ConnectFd: can't map futex fd %d err = %s", fd, strerror(errno));
        close(fd);
        return false;
    }

    snprintf(fName, sizeof(fName), "(fd %d)", fd);
    fSharedMem = fd;
    fFutex = futex;
    return true;
}

int JackLinuxFutex::DupFd() const
{
    return (fSharedMem >= 0) ? fcntl(fSharedMem, F_DUPFD_CLOEXEC, 0) : -1;
}

int JackLinuxFutex::OpenNamedFd(const char* name, const char* server_name)
{
    char path[SYNC_MAX_NAME_SIZE];
    BuildName(name, server_name, path, sizeof(path));
    int fd = shm_open(path, O_RDWR | O_CLOEXEC, 0);
    if (fd < 0) {
        jack_log("JackLinuxFutex::OpenNamedFd %s: %s", path, strerror(errno));
    }
    return fd;
}

bool JackLinuxFutex::ConnectInput(const char* name, const char* server_name)
{
    return Connect(name, server_name);
}

bool JackLinuxFutex::ConnectOutput(const char* name, const char* server_name)
{
    return Connect(name, server_name);
}

bool JackLinuxFutex::Disconnect()
{
    if (!fFutex) {
        return true;
    }

    if (! fPrivate && fFutex->wasInternal)
    {
        const char* externalSync = getenv("JACK_INTERNAL_CLIENT_SYNC");

        if (externalSync != NULL && strstr(fName, externalSync) != NULL && --fFutex->externalCount == 0)
        {
            jack_error("Note: client %s now running as internal client again", fName);
            fFutex->needsChange = true;
        }
    }

    munmap(fFutex, sizeof(FutexData));
    fFutex = NULL;

    close(fSharedMem);
    fSharedMem = -1;
    return true;
}

// Server side: remove the futex name, but keep the mapping valid for a
// realtime thread that may still signal it. The mapping (one page) is
// dropped, not unmapped: this only happens on an error path.
void JackLinuxFutex::DestroyKeepMapped()
{
    if (!fFutex) {
        return;
    }
    close(fSharedMem);
    fSharedMem = -1;
    shm_unlink(fName);
    // fFutex stays valid; a later Allocate() maps a new futex for the refnum.
}

// Server side : destroy the futex
void JackLinuxFutex::Destroy()
{
    if (!fFutex) {
        return;
    }

    munmap(fFutex, sizeof(FutexData));
    fFutex = NULL;

    close(fSharedMem);
    fSharedMem = -1;

    shm_unlink(fName);
}

} // end of namespace

