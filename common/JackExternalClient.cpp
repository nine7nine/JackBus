/*
  Copyright (C) 2001-2003 Paul Davis
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

#include "JackExternalClient.h"
#include "JackClientControl.h"
#include "JackGlobals.h"
#include "JackChannel.h"
#include "JackError.h"

namespace Jack
{

JackExternalClient::JackExternalClient(): fClientControl(NULL), fDead(false)
{}

JackExternalClient::~JackExternalClient()
{}

int JackExternalClient::ClientNotify(int refnum, const char* name, int notify, int sync, const char* message, int value1, int value2)
{
    if (fDead) {
        return 0;   // its sockets are gone: every write would fail (and be logged)
    }
    int result = -1;
    jack_log("JackExternalClient::ClientNotify ref = %ld client = %s name = %s notify = %ld", refnum, fClientControl->fName, name, notify);
    fChannel.ClientNotify(refnum, name, notify, sync, message, value1, value2, &result);
    return result;
}

int JackExternalClient::Open(const char* name, int pid, int refnum, jack_uuid_t uuid, int* shared_client)
{
    try {

        if (fChannel.Open(name) < 0) {
            jack_error("Cannot connect to client name = %s\n", name);
            return -1;
        }

        // Use "placement new" to allocate object in shared memory.
        // The shm info must be set *after* construction: stores made before the
        // object's lifetime starts may be discarded by the compiler (with LTO this
        // published a garbage segment index to the client, upstream issue #485).
        void* shared_mem = JackShmMem::operator new(sizeof(JackClientControl));
        fClientControl = new(shared_mem) JackClientControl(name, pid, refnum, uuid);
        fClientControl->JackShmMemAble::Init();

        if (fClientControl->GetShmIndex() < 0) {
            jack_error("Cannot allocate client shared memory segment");
            fClientControl->~JackClientControl();
            JackShmMem::operator delete(shared_mem);
            fClientControl = NULL;
            return -1;
        }

        *shared_client = fClientControl->GetShmIndex();
        jack_log("JackExternalClient::Open name = %s index = %ld base = %x", name, fClientControl->GetShmIndex(), fClientControl->GetShmAddress());
        return 0;

    } catch (std::exception&) {
        return -1;
    }
}

int JackExternalClient::Close()
{
    jack_log("JackExternalClient::Close");
    fChannel.Close();
    if (fClientControl) {
        fClientControl->~JackClientControl();
        JackShmMem::operator delete(fClientControl);
    }
    return 0;
}

bool JackExternalClient::FlushNotifications()
{
#ifdef WIN32
    return false;
#else
    return !fDead && fChannel.Flush();
#endif
}

JackClientControl* JackExternalClient::GetClientControl() const
{
    return fClientControl;
}

} // end of namespace
