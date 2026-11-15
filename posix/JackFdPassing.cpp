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

#include "JackFdPassing.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>

int JackSendFds(int sock, const int* fds, int n)
{
    if (n < 0 || n > JACK_MAX_PASSED_FDS) {
        errno = EINVAL;
        return -1;
    }
    unsigned char count = (unsigned char)n;
    struct iovec iov;
    iov.iov_base = &count;
    iov.iov_len = 1;

    union {
        char buf[CMSG_SPACE(sizeof(int) * JACK_MAX_PASSED_FDS)];
        struct cmsghdr align;
    } control;
    memset(&control, 0, sizeof(control));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (n > 0) {
        msg.msg_control = control.buf;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * n);
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * n);
        memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * n);
    }

    ssize_t res;
    do {
        res = sendmsg(sock, &msg, MSG_NOSIGNAL);
    } while (res < 0 && errno == EINTR);
    return (res == 1) ? 0 : -1;
}

static void CloseAll(const int* fds, int n)
{
    for (int i = 0; i < n; i++) {
        close(fds[i]);
    }
}

int JackRecvFds(int sock, int* fds, int max)
{
    unsigned char count = 0;
    struct iovec iov;
    iov.iov_base = &count;
    iov.iov_len = 1;

    union {
        char buf[CMSG_SPACE(sizeof(int) * JACK_MAX_PASSED_FDS)];
        struct cmsghdr align;
    } control;
    memset(&control, 0, sizeof(control));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.buf;
    msg.msg_controllen = sizeof(control.buf);

    ssize_t res;
    do {
        res = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
    } while (res < 0 && errno == EINTR);
    if (res != 1) {
        if (res == 0) {
            errno = ECONNRESET;
        }
        return -1;
    }

    int got[JACK_MAX_PASSED_FDS];
    int n = 0;
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            continue;
        }
        int k = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        for (int i = 0; i < k; i++) {
            int fd;
            memcpy(&fd, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
            if (n < JACK_MAX_PASSED_FDS) {
                got[n++] = fd;
            } else {
                close(fd);
            }
        }
    }
    if ((msg.msg_flags & MSG_CTRUNC) || n != (int)count || n > max) {
        CloseAll(got, n);
        errno = EPROTO;
        return -1;
    }
    memcpy(fds, got, sizeof(int) * n);
    return n;
}
