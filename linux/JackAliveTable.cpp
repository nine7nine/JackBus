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

#include "JackAliveTable.h"
#include "JackTools.h"
#include "JackError.h"
#include "promiscuous.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Jack
{

static const size_t kTableSize = CLIENT_NUM * JackAliveTable::kSlotSize;

static_assert(sizeof(pthread_mutex_t) <= JackAliveTable::kSlotSize, "a slot must hold a pthread_mutex_t");

JackAliveTable::JackAliveTable(): fBase(NULL), fOwner(false)
{
    fName[0] = 0;
    const char* promiscuous = getenv("JACK_PROMISCUOUS_SERVER");
    fPromiscuous = (promiscuous != NULL);
    fPromiscuousGid = jack_group2gid(promiscuous);
}

JackAliveTable::~JackAliveTable()
{
    if (fOwner) {
        Destroy();
    } else {
        Disconnect();
    }
}

// Named like the clients' futexes (JackLinuxFutex::BuildName), per server
void JackAliveTable::BuildName(const char* server_name, char* res, int size)
{
    if (fPromiscuous) {
        snprintf(res, size, "jack_alive.%s", server_name);
    } else {
        snprintf(res, size, "jack_alive.%d_%s", JackTools::GetUID(), server_name);
    }
}

bool JackAliveTable::Allocate(const char* server_name)
{
    BuildName(server_name, fName, sizeof(fName));

    // A file left by an earlier server may still be mapped by its clients'
    // processes: start from a new one (those keep the old, unlinked one)
    shm_unlink(fName);
    int fd = shm_open(fName, O_CREAT | O_EXCL | O_RDWR, 0777);
    if (fd < 0) {
        jack_error("JackAliveTable: cannot create %s: %s", fName, strerror(errno));
        return false;
    }
    if (ftruncate(fd, kTableSize) != 0
        || (fPromiscuous && jack_promiscuous_perms(fd, fName, fPromiscuousGid) < 0)) {
        jack_error("JackAliveTable: cannot set up %s: %s", fName, strerror(errno));
        close(fd);
        shm_unlink(fName);
        return false;
    }
    void* base = mmap(NULL, kTableSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        jack_error("JackAliveTable: cannot map %s: %s", fName, strerror(errno));
        shm_unlink(fName);
        return false;
    }
    memset(base, 0, kTableSize);
    fBase = (char*)base;
    fOwner = true;
    jack_log("JackAliveTable::Allocate %s", fName);
    return true;
}

void JackAliveTable::Destroy()
{
    if (fBase) {
        munmap(fBase, kTableSize);
        fBase = NULL;
    }
    if (fOwner) {
        shm_unlink(fName);
        fOwner = false;
    }
}

void JackAliveTable::ClearSlot(int refnum)
{
    if (fBase && refnum >= 0 && refnum < CLIENT_NUM) {
        memset(fBase + refnum * kSlotSize, 0, kSlotSize);
    }
}

uint32_t* JackAliveTable::Word(int refnum) const
{
    if (!fBase || refnum < 0 || refnum >= CLIENT_NUM) {
        return NULL;
    }
    return (uint32_t*)(fBase + refnum * kSlotSize);
}

bool JackAliveTable::Connect(const char* server_name)
{
    if (fBase) {
        return true;
    }
    BuildName(server_name, fName, sizeof(fName));
    int fd = shm_open(fName, O_RDWR, 0);
    if (fd < 0) {
        // a server without it (stock JACK2, or an older JackBus): nothing to do
        jack_log("JackAliveTable::Connect %s: %s", fName, strerror(errno));
        return false;
    }
    struct stat st;
    void* base = MAP_FAILED;
    if (fstat(fd, &st) == 0 && st.st_size >= (off_t)kTableSize) {
        base = mmap(NULL, kTableSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    }
    close(fd);
    if (base == MAP_FAILED) {
        jack_log("JackAliveTable::Connect %s: cannot map", fName);
        return false;
    }
    fBase = (char*)base;
    jack_log("JackAliveTable::Connect %s", fName);
    return true;
}

int JackAliveTable::OpenFd() const
{
    if (!fOwner || !fBase) {
        return -1;
    }
    return shm_open(fName, O_RDWR | O_CLOEXEC, 0);
}

bool JackAliveTable::ConnectFd(int fd)
{
    if (fd < 0) {
        return false;
    }
    if (fBase) {
        close(fd);
        return true;
    }
    struct stat st;
    void* base = MAP_FAILED;
    if (fstat(fd, &st) == 0 && st.st_size >= (off_t)kTableSize) {
        base = mmap(NULL, kTableSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    }
    close(fd);
    if (base == MAP_FAILED) {
        jack_log("JackAliveTable::ConnectFd: cannot map");
        return false;
    }
    snprintf(fName, sizeof(fName), "(fd)");
    fBase = (char*)base;
    jack_log("JackAliveTable::ConnectFd");
    return true;
}

void JackAliveTable::Disconnect()
{
    if (fBase && !fOwner) {
        munmap(fBase, kTableSize);
        fBase = NULL;
    }
}

bool JackAliveTable::InitSlot(int refnum)
{
    if (!fBase || refnum < 0 || refnum >= CLIENT_NUM) {
        return false;
    }
    pthread_mutex_t* mutex = (pthread_mutex_t*)(fBase + refnum * kSlotSize);
    pthread_mutexattr_t attr;
    bool ok = pthread_mutexattr_init(&attr) == 0
              && pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0
              && pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0
              && pthread_mutex_init(mutex, &attr) == 0;
    pthread_mutexattr_destroy(&attr);
    if (!ok) {
        jack_log("JackAliveTable::InitSlot %d failed", refnum);
    }
    return ok;
}

bool JackAliveTable::LockSlot(int refnum)
{
    if (!fBase || refnum < 0 || refnum >= CLIENT_NUM) {
        return false;
    }
    pthread_mutex_t* mutex = (pthread_mutex_t*)(fBase + refnum * kSlotSize);
    int res = pthread_mutex_trylock(mutex);
    if (res == EOWNERDEAD) {
        // an earlier realtime thread of this client ended (deactivation, which
        // cancels it): this one takes over
        res = pthread_mutex_consistent(mutex);
    }
    if (res != 0) {
        // never wait here: without the lock, the server only learns of this
        // thread's end the way it always did
        jack_log("JackAliveTable::LockSlot %d: %s", refnum, strerror(res));
        return false;
    }
    return true;
}

} // end of namespace
