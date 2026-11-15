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

#ifndef __JackFdPassing__
#define __JackFdPassing__

/*
 File descriptors over the JackBus socket (a SOCK_STREAM unix socket, like the
 protocol-9 one, because requests are read field by field).

 A message that carries fds ends with a trailer: one byte holding the number
 of fds, sent with sendmsg() and SCM_RIGHTS. The fields before it are written
 and read as usual; reads of exact field sizes stop before the trailer, which
 only JackRecvFds() reads (with recvmsg(): a plain read() of that byte would
 make the kernel close the fds it carries).
 */

#define JACK_MAX_PASSED_FDS 8

#ifdef __cplusplus
extern "C" {
#endif

/* Send n (0..JACK_MAX_PASSED_FDS) fds as a trailer. 0 on success, -1 on error. */
int JackSendFds(int sock, const int* fds, int n);

/* Receive a trailer: its fds are stored in fds[] (close-on-exec), at most max.
   Returns their number, or -1 on error (no trailer, a count that doesn't match,
   or more fds than max: any fds received are closed then). */
int JackRecvFds(int sock, int* fds, int max);

#ifdef __cplusplus
}
#endif

#endif
