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

#ifndef __JackBusLibClient__
#define __JackBusLibClient__

#include "JackLibClient.h"

namespace Jack
{

class JackBusClientChannel;

/*!
\brief A libjack client of the JackBus socket.

 The same client as JackLibClient, on the same shared memory, futex words and
 alive slot, so it runs JackClient's realtime loop unchanged and works with
 every jack_* function. Only the way it gets hold of those objects differs:
 fds over the JackBus socket instead of names in /dev/shm and the shm registry.
 Its realtime thread must hold its alive slot: the open fails without one.
*/

class JackBusLibClient : public JackLibClient
{

    private:

        JackBusClientChannel* fBusChannel;  // = fChannel

    public:

        JackBusLibClient(JackSynchro* table);
        virtual ~JackBusLibClient();

        int Open(const char* server_name, const char* name, jack_uuid_t uuid, jack_options_t options, jack_status_t* status);
        int ClientNotifyImp(int refnum, const char* name, int notify, int sync, const char* message, int value1, int value2);

        // JackBus requests (jack/jackbus.h): async endpoints, priority
        int EndpointOpen(int count, const jack_port_id_t* ports, int ring_frames, int* id, int* fd);
        int EndpointClose(int id);
        int SetPriority(bool on);
        int RealTimePriority();

    private:
        bool fPriority;
};

} // end of namespace

#endif
