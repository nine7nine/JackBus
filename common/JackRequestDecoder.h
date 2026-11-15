/*
Copyright (C) 2012 Grame

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

#ifndef __JackRequestDecoder__
#define __JackRequestDecoder__

#include "JackChannel.h"

namespace Jack
{

class JackServer;
struct JackClientOpenRequest;
struct JackClientOpenResult;

struct JackClientHandlerInterface {

    virtual void ClientAdd(detail::JackChannelTransactionInterface* socket, JackClientOpenRequest* req, JackClientOpenResult* res) = 0;
    virtual void ClientRemove(detail::JackChannelTransactionInterface* socket, int refnum) = 0;
    
    virtual ~JackClientHandlerInterface()
    {}

};

/*!
\brief Request decoder
*/

class JackRequestDecoder
{
    private:

        JackServer* fServer;
        JackClientHandlerInterface* fHandler;

    public:

        JackRequestDecoder(JackServer* server, JackClientHandlerInterface* handler);
        virtual ~JackRequestDecoder();

        /*
         owner: -1 for the protocol-9 socket (nothing checked, as always). For a
         JackBus socket client, its refnum: a request on behalf of another client is
         refused (answered with -1, so the stream stays in step), and one that can't
         be answered (no result, or the server's own kNotification) returns -2: the
         caller drops the client.
         */
        int HandleRequest(detail::JackChannelTransactionInterface* socket, int type, int owner = -1);
};

} // end of namespace

#endif
