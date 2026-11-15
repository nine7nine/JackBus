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

#ifndef __jackbus_h__
#define __jackbus_h__

#include <jack/jack.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * Open a client through the JackBus socket ($XDG_RUNTIME_DIR/jackbus/<server>).
 *
 * The arguments and the result are those of jack_client_open(), plus flags
 * (reserved: pass 0). The client gets the server's shared objects as file
 * descriptors: it needs no /dev/shm names and no shm registry. It works with
 * every jack_* function, and runs in the same graph as protocol-9 clients.
 *
 * Fails (NULL, JackFailure in *status) when the server has no JackBus socket:
 * a stock JACK server, or no XDG_RUNTIME_DIR.
 */
jack_client_t * jackbus_client_open (const char *client_name,
                                     jack_options_t options,
                                     jack_status_t *status,
                                     unsigned int flags, ...) JACK_OPTIONAL_WEAK_EXPORT;

#ifdef __cplusplus
}
#endif

#endif /* __jackbus_h__ */
