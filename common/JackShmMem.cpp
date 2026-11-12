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

#include "JackError.h"
#include "JackShmMem.h"
#include <stdio.h>
#include <mutex>
#include <stdint.h>
#ifndef WIN32
#include <unistd.h>
#include <sys/mman.h>
#endif

namespace Jack
{

static unsigned int fSegmentNum = 0;

/*
 Process-local record of the shared memory segments this process allocated
 with JackShmMem::operator new, keyed by attach address.

 The segment info used to be handed from operator new to the constructor
 through a process-global variable, and read back by operator delete from
 the (already destroyed) object. Both are unsafe:
 - the global races when two threads allocate concurrently (jackdbus: D-Bus
   thread and server request thread);
 - stores into the object made before its constructor runs, or reads made
   after its destructor ran, are dead/undefined under C++ object lifetime
   rules, and GCC (-flifetime-dse) removes them once the code is visible
   across translation units (LTO). The server then published a garbage
   segment index for each client's JackClientControl; the client attached
   that segment (another object, possibly of another JACK server) and
   unlinked it (upstream jack2 issue #485, LTO crash at first connection).
 Looking the info up by address avoids both.
*/
namespace {

struct ShmAllocation {
    void* fAddress;
    jack_shm_info_t fInfo;
};

ShmAllocation gAllocations[MAX_SHM_ID];
std::mutex gAllocationsLock;

void RecordAllocation(void* address, const jack_shm_info_t& info)
{
    std::lock_guard<std::mutex> lock(gAllocationsLock);
    for (int i = 0; i < MAX_SHM_ID; i++) {
        if (gAllocations[i].fAddress == NULL) {
            gAllocations[i].fAddress = address;
            gAllocations[i].fInfo = info;
            return;
        }
    }
    jack_error("JackShmMem: allocation table full");
}

bool LookupAllocation(const void* address, jack_shm_info_t* info, bool remove)
{
    std::lock_guard<std::mutex> lock(gAllocationsLock);
    for (int i = 0; i < MAX_SHM_ID; i++) {
        if (gAllocations[i].fAddress == address) {
            *info = gAllocations[i].fInfo;
            if (remove) {
                gAllocations[i].fAddress = NULL;
            }
            return true;
        }
    }
    return false;
}

} // namespace

JackShmMem::JackShmMem()
{
    JackShmMemAble::Init();
    LockMemory();
}

JackShmMem::~JackShmMem()
{
    UnlockMemory();
}

void JackShmMemAble::Init()
{
    // Must be called on a live object (from or after its constructor), see above.
    jack_shm_info_t info;
    if (LookupAllocation(this, &info, false)) {
        fInfo.index = info.index;
        fInfo.ptr.attached_at = info.ptr.attached_at;
        fInfo.size = info.size;
    } else {
        jack_error("JackShmMemAble::Init: %p was not allocated by JackShmMem::operator new", this);
        fInfo.index = JACK_SHM_NULL_INDEX;
        fInfo.ptr.attached_at = NULL;
        fInfo.size = 0;
    }
}

void* JackShmMem::operator new(size_t size, void* memory)
{
    jack_log("JackShmMem::new placement size = %ld", size);
    return memory;
}

void* JackShmMem::operator new(size_t size)
{
    jack_shm_info_t info;
    JackShmMem* obj;
    char name[64];

    snprintf(name, sizeof(name), "/jack_shared%d", fSegmentNum++);

    if (jack_shmalloc(name, size, &info)) {
        jack_error("Cannot create shared memory segment of size = %d", size, strerror(errno));
        goto error;
    }

    if (jack_attach_shm(&info)) {
        jack_error("Cannot attach shared memory segment name = %s err = %s", name, strerror(errno));
        jack_destroy_shm(&info);
        goto error;
    }

    obj = (JackShmMem*)jack_shm_addr(&info);
    // Never store into the object here: its constructor has not run yet.
    // JackShmMemAble::Init() fetches this record by address once it has.
    info.size = size;
    RecordAllocation(obj, info);

    jack_log("JackShmMem::new index = %ld attached = %x size = %ld ", info.index, info.ptr.attached_at, size);
    return obj;

error:
    jack_error("JackShmMem::new bad alloc", size);
    throw std::bad_alloc();
}

void JackShmMem::operator delete(void* p, size_t size)
{
    // The object has already been destroyed: don't read it, look it up.
    jack_shm_info_t info;
    if (!LookupAllocation(p, &info, true)) {
        jack_error("JackShmMem::delete: %p was not allocated by JackShmMem::operator new", p);
        return;
    }

    jack_log("JackShmMem::delete size = %ld index = %ld", size, info.index);

    jack_release_shm(&info);
    jack_destroy_shm(&info);
}

void JackShmMem::operator delete(void* obj)
{
    if (obj) {
        JackShmMem::operator delete(obj, 0);
    }
}

#ifdef __linux__
#ifndef MADV_POPULATE_READ
#define MADV_POPULATE_READ 22
#endif
#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23
#endif

/*
 When mlock() is refused (RLIMIT_MEMLOCK, e.g. users without realtime
 privileges, sandboxed clients), at least fault the pages in now instead of
 on first access from a realtime thread (port buffers, connection tables).
 MADV_POPULATE_* (Linux 5.14) do not count against the limit; the server keeps
 the shared pages locked, so the mappings stay valid.
*/
static void PopulateMemory(void* ptr, size_t size)
{
    uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)ptr & ~(page - 1);
    size_t len = size + ((uintptr_t)ptr - start);
    if (madvise((void*)start, len, MADV_POPULATE_WRITE) != 0) {
        // read-only mapping (EFAULT) or older kernel (EINVAL)
        madvise((void*)start, len, MADV_POPULATE_READ);
    }
}
#else
static void PopulateMemory(void* ptr, size_t size)
{}
#endif

void LockMemoryImp(void* ptr, size_t size)
{
    if (CHECK_MLOCK((char*)ptr, size)) {
        jack_log("Succeeded in locking %u byte memory area", size);
    } else {
        jack_error("Cannot lock down %u byte memory area (%s), prefaulting it instead", size, strerror(errno));
        PopulateMemory(ptr, size);
    }
}

void InitLockMemoryImp(void* ptr, size_t size)
{
    if (CHECK_MLOCK((char*)ptr, size)) {
        memset(ptr, 0, size);
        jack_log("Succeeded in locking %u byte memory area", size);
    } else {
        jack_error("Cannot lock down %u byte memory area (%s), prefaulting it instead", size, strerror(errno));
        PopulateMemory(ptr, size);
    }
}

void UnlockMemoryImp(void* ptr, size_t size)
{
    if (CHECK_MUNLOCK((char*)ptr, size)) {
        jack_log("Succeeded in unlocking %u byte memory area", size);
    } else {
        jack_error("Cannot unlock down %u byte memory area (%s)", size, strerror(errno));
    }
}

void LockAllMemory()
{
    if (CHECK_MLOCKALL()) {
        jack_log("Succeeded in locking all memory");
    } else {
        jack_error("Cannot lock all memory (%s)", strerror(errno));
    }
}

void UnlockAllMemory()
{
    if (CHECK_MUNLOCKALL()) {
        jack_log("Succeeded in unlocking all memory");
    } else {
        jack_error("Cannot unlock all memory (%s)", strerror(errno));
    }
}


} // end of namespace

