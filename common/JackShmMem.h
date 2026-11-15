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

#ifndef __JackShmMem__
#define __JackShmMem__

#include "shm.h"
#include "JackError.h"
#include "JackCompilerDeps.h"

#include <new>  // GCC 4.0
#include <errno.h>
#include <stdlib.h>
#ifdef __linux__
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "JackShmMem_os.h"

namespace Jack
{

void LockMemoryImp(void* ptr, size_t size);
void InitLockMemoryImp(void* ptr, size_t size);
void UnlockMemoryImp(void* ptr, size_t size);
void LockAllMemory();
void UnlockAllMemory();

/*!
\brief

A class which objects possibly want to be allocated in shared memory derives from this class.
*/

class JackShmMemAble
{
    protected:

        jack_shm_info_t fInfo;

    public:

        void Init();

        int GetShmIndex()
        {
            return fInfo.index;
        }

        char* GetShmAddress()
        {
            return (char*)fInfo.ptr.attached_at;
        }

        void LockMemory()
        {
            LockMemoryImp(this, fInfo.size);
        }

        void UnlockMemory()
        {
            UnlockMemoryImp(this, fInfo.size);
        }

};

/*!
\brief The base class for shared memory management.

A class which objects need to be allocated in shared memory derives from this class.
*/

class SERVER_EXPORT JackShmMem : public JackShmMemAble
{

     protected:

        JackShmMem();
        ~JackShmMem();

    public:

        void* operator new(size_t size);
        void* operator new(size_t size, void* memory);

        void operator delete(void* p, size_t size);
		void operator delete(void* p);

};

#ifdef __linux__
/*
 JackBus socket clients: map a segment received as an fd instead of opening it
 by name through the registry (no registry, no SysV lock, no /dev/shm needed).
 index is the server's registry index, kept only so that clients of the
 protocol-9 socket in the same process see the segment as attached. Takes
 ownership of fd. Returns 1 if mapped, 0 if this pointer already had that
 segment (another client of the process), -1 on error.
 */
static inline int JackShmMapFd(jack_shm_info_t* info, int fd, int index)
{
    if (fd < 0) {
        return -1;
    }
    if (info->index >= 0) {
        close(fd);
        return (info->index == index) ? 0 : -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0 || (uint64_t)st.st_size > UINT32_MAX) {
        close(fd);
        return -1;
    }
    void* ptr = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (ptr == MAP_FAILED) {
        return -1;
    }
    info->index = index;
    info->size = (uint32_t)st.st_size;
    info->ptr.attached_at = ptr;
    return 1;
}
#endif

/*!
\brief Pointer on shared memory segment in the client side.
*/

template <class T>
class JackShmReadWritePtr
{

    private:

        jack_shm_info_t fInfo;
        bool fInitDone;

        void Init(int index, const char* server_name = JACK_DEFAULT_SERVER_NAME)
        {
            if (fInfo.index < 0 && index >= 0) {
                jack_log("JackShmReadWritePtr::Init %ld %d", index, fInfo.index);
                if (jack_initialize_shm(server_name) < 0) {
                    throw std::bad_alloc();
                }
                fInfo.index = index;
                if (jack_attach_lib_shm(&fInfo)) {
                    throw std::bad_alloc();
                }
                GetShmAddress()->LockMemory();
                fInitDone = true;
            }
        }

    public:

        JackShmReadWritePtr()
        {
            fInfo.index = -1;
            fInitDone = false;
            fInfo.ptr.attached_at = (char*)NULL;
        }

        JackShmReadWritePtr(int index, const char* server_name)
        {
            Init(index, server_name);
        }

        ~JackShmReadWritePtr()
        {
            if (!fInitDone) {
               jack_error("JackShmReadWritePtr::~JackShmReadWritePtr - Init not done for %d, skipping unlock", fInfo.index);
               return;
            }
            if (fInfo.index >= 0) {
                jack_log("JackShmReadWritePtr::~JackShmReadWritePtr %d", fInfo.index);
                GetShmAddress()->UnlockMemory();
                jack_release_lib_shm(&fInfo);
                fInfo.index = -1;
             }
        }

        T* operator->() const
        {
            return (T*)fInfo.ptr.attached_at;
        }

        operator T*() const
        {
            return (T*)fInfo.ptr.attached_at;
        }

        JackShmReadWritePtr& operator=(int index)
        {
            Init(index);
            return *this;
        }

        void SetShmIndex(int index, const char* server_name)
        {
            Init(index, server_name);
        }
#ifdef __linux__
        // JackBus: the segment from an fd (see JackShmMapFd). false on error.
        bool SetShmFd(int fd, int index)
        {
            int res = JackShmMapFd(&fInfo, fd, index);
            if (res > 0) {
                GetShmAddress()->LockMemory();
                fInitDone = true;
            }
            return res >= 0;
        }
#endif

        int GetShmIndex()
        {
            return fInfo.index;
        }

        T* GetShmAddress()
        {
            return (T*)fInfo.ptr.attached_at;
        }
};

/*!
\brief Pointer on shared memory segment in the client side: destroy the segment (used client control)
*/

template <class T>
class JackShmReadWritePtr1
{

    private:

        jack_shm_info_t fInfo;
        bool fInitDone;

        void Init(int index, const char* server_name = JACK_DEFAULT_SERVER_NAME)
        {
            if (fInfo.index < 0 && index >= 0) {
                jack_log("JackShmReadWritePtr1::Init %ld %d", index, fInfo.index);
                if (jack_initialize_shm(server_name) < 0) {
                    throw std::bad_alloc();
                }
                fInfo.index = index;
                if (jack_attach_lib_shm(&fInfo)) {
                    throw std::bad_alloc();
                }
                GetShmAddress()->LockMemory();
                fInitDone = true;
                /*
                Historically the client unlinked its own client-control segment
                here. jack_destroy_shm() now only removes segments allocated by
                the calling process, so this is a no-op for clients: the server
                (the allocator) unlinks the segment when the client closes, and
                a bad index can no longer make a client delete another
                process's segment. Kept for in-process (server-side) use.
                */
                jack_destroy_shm(&fInfo);
            }
        }

    public:

        JackShmReadWritePtr1()
        {
            fInfo.index = -1;
            fInitDone = false;
            fInfo.ptr.attached_at = NULL;
        }

        JackShmReadWritePtr1(int index, const char* server_name)
        {
            Init(index, server_name);
        }

        ~JackShmReadWritePtr1()
        {
            if (!fInitDone) {
               jack_error("JackShmReadWritePtr1::~JackShmReadWritePtr1 - Init not done for %d, skipping unlock", fInfo.index);
               return;
            }
            if (fInfo.index >= 0) {
                jack_log("JackShmReadWritePtr1::~JackShmReadWritePtr1 %d", fInfo.index);
                GetShmAddress()->UnlockMemory();
                jack_release_lib_shm(&fInfo);
                fInfo.index = -1;
            }
        }

        T* operator->() const
        {
            return (T*)fInfo.ptr.attached_at;
        }

        operator T*() const
        {
            return (T*)fInfo.ptr.attached_at;
        }

        JackShmReadWritePtr1& operator=(int index)
        {
            Init(index);
            return *this;
        }

        void SetShmIndex(int index, const char* server_name)
        {
            Init(index, server_name);
        }
#ifdef __linux__
        // JackBus: the segment from an fd (see JackShmMapFd). false on error.
        bool SetShmFd(int fd, int index)
        {
            int res = JackShmMapFd(&fInfo, fd, index);
            if (res > 0) {
                GetShmAddress()->LockMemory();
                fInitDone = true;
            }
            return res >= 0;
        }
#endif

        int GetShmIndex()
        {
            return fInfo.index;
        }

        T* GetShmAddress()
        {
            return (T*)fInfo.ptr.attached_at;
        }
};

/*!
\brief Pointer on shared memory segment in the client side.
*/

template <class T>
class JackShmReadPtr
{

    private:

        jack_shm_info_t fInfo;
        bool fInitDone;

        void Init(int index, const char* server_name = JACK_DEFAULT_SERVER_NAME)
        {
            if (fInfo.index < 0 && index >= 0) {
                jack_log("JackShmPtrRead::Init %ld %d", index, fInfo.index);
                if (jack_initialize_shm(server_name) < 0) {
                    throw std::bad_alloc();
                }
                fInfo.index = index;
                if (jack_attach_lib_shm_read(&fInfo)) {
                    throw std::bad_alloc();
                }
                GetShmAddress()->LockMemory();
                fInitDone = true;
            }
        }

    public:

        JackShmReadPtr()
        {
            fInfo.index = -1;
            fInitDone = false;
            fInfo.ptr.attached_at = NULL;
        }

        JackShmReadPtr(int index, const char* server_name)
        {
            Init(index, server_name);
        }

        ~JackShmReadPtr()
        {
            if (!fInitDone) {
               jack_error("JackShmReadPtr::~JackShmReadPtr - Init not done for %ld, skipping unlock", fInfo.index);
               return;
            }
            if (fInfo.index >= 0) {
                jack_log("JackShmPtrRead::~JackShmPtrRead %ld", fInfo.index);
                GetShmAddress()->UnlockMemory();
                jack_release_lib_shm(&fInfo);
                fInfo.index = -1;
            }
        }

        T* operator->() const
        {
            return (T*)fInfo.ptr.attached_at;
        }

        operator T*() const
        {
            return (T*)fInfo.ptr.attached_at;
        }

        JackShmReadPtr& operator=(int index)
        {
            Init(index);
            return *this;
        }

        void SetShmIndex(int index, const char* server_name)
        {
            Init(index, server_name);
        }

        int GetShmIndex()
        {
            return fInfo.index;
        }

        T* GetShmAddress()
        {
            return (T*)fInfo.ptr.attached_at;
        }

};

} // end of namespace

#endif
