/*
 fd_trailer: the framing of the JackBus socket (posix/JackFdPassing).

 Requests and notifications are read field by field from a SOCK_STREAM socket;
 a message carrying fds ends with a 1-byte SCM_RIGHTS trailer. Checks that:
 - exact-size field reads never consume the trailer, however the fields were
   written (one write each, all in one write, or dribbled in small pieces while
   the reader already waits);
 - the trailer delivers the fds, intact, and nothing leaks;
 - a 0-fd trailer, a count/fd mismatch and too many fds behave as documented;
 - reading the trailer byte with plain read() loses its fds (why only
   JackRecvFds may read it) without leaking them.
*/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/mman.h>

#include "JackFdPassing.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static int open_fds(void)
{
    int n = 0;
    DIR* d = opendir("/proc/self/fd");
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') {
            n++;
        }
    }
    closedir(d);
    return n - 1;   // the DIR's own fd
}

static int make_memfd(char tag)
{
    int fd = memfd_create("fd_trailer", MFD_CLOEXEC);
    if (fd >= 0 && write(fd, &tag, 1) != 1) {
        close(fd);
        return -1;
    }
    return fd;
}

static char memfd_tag(int fd)
{
    char c = 0;
    return pread(fd, &c, 1, 0) == 1 ? c : 0;
}

static int read_exact(int fd, void* buf, int len)
{
    char* p = (char*)buf;
    while (len > 0) {
        ssize_t r = read(fd, p, len);
        if (r <= 0) {
            return -1;
        }
        p += r;
        len -= r;
    }
    return 0;
}

static int write_exact(int fd, const void* buf, int len)
{
    return write(fd, buf, len) == len ? 0 : -1;
}

struct header { int type; int refnum; };
struct body { char name[60]; int value; };

/* one message: header + body (+ trailer with tags[0..n-1] memfds) */
static void send_message(int sock, int mode, int i, int n)
{
    struct header h = { 100 + (i % 3), i };
    struct body b;
    memset(&b, 0, sizeof(b));
    snprintf(b.name, sizeof(b.name), "client-%d", i);
    b.value = i * 7;

    if (mode == 0) {            // a write per field, as JackRequest::Write does
        write_exact(sock, &h.type, sizeof(int));
        write_exact(sock, &h.refnum, sizeof(int));
        write_exact(sock, b.name, sizeof(b.name));
        write_exact(sock, &b.value, sizeof(int));
    } else if (mode == 1) {     // everything in one write
        char buf[sizeof(h) + sizeof(b)];
        memcpy(buf, &h, sizeof(h));
        memcpy(buf + sizeof(h), &b, sizeof(b));
        write_exact(sock, buf, sizeof(buf));
    } else {                    // dribbled: 3 bytes at a time
        char buf[sizeof(h) + sizeof(b)];
        memcpy(buf, &h, sizeof(h));
        memcpy(buf + sizeof(h), &b, sizeof(b));
        for (size_t off = 0; off < sizeof(buf); off += 3) {
            size_t len = sizeof(buf) - off < 3 ? sizeof(buf) - off : 3;
            write_exact(sock, buf + off, (int)len);
            if (i % 50 == 0) {
                usleep(50);
            }
        }
    }
    if (n >= 0) {
        int fds[JACK_MAX_PASSED_FDS];
        for (int k = 0; k < n; k++) {
            fds[k] = make_memfd((char)('a' + (i + k) % 26));
        }
        CHECK(JackSendFds(sock, fds, n) == 0, "send %d fds", n);
        for (int k = 0; k < n; k++) {
            close(fds[k]);      // the receiver has its own now
        }
    }
}

static void receive_message(int sock, int i, int n)
{
    struct header h;
    struct body b;
    CHECK(read_exact(sock, &h.type, sizeof(int)) == 0, "read type %d", i);
    CHECK(read_exact(sock, &h.refnum, sizeof(int)) == 0, "read refnum %d", i);
    CHECK(read_exact(sock, b.name, sizeof(b.name)) == 0, "read name %d", i);
    CHECK(read_exact(sock, &b.value, sizeof(int)) == 0, "read value %d", i);
    char expect[60];
    snprintf(expect, sizeof(expect), "client-%d", i);
    CHECK(h.type == 100 + (i % 3) && h.refnum == i && b.value == i * 7 && strcmp(b.name, expect) == 0,
          "message %d garbled", i);
    if (n >= 0) {
        int fds[JACK_MAX_PASSED_FDS];
        int got = JackRecvFds(sock, fds, JACK_MAX_PASSED_FDS);
        CHECK(got == n, "message %d: %d fds, expected %d", i, got, n);
        for (int k = 0; k < got; k++) {
            CHECK(memfd_tag(fds[k]) == (char)('a' + (i + k) % 26), "message %d fd %d wrong", i, k);
            close(fds[k]);
        }
    }
}

struct writer_args { int sock; int count; };

/* message i carries (i % 4) - 1 fds: -1 = no trailer, 0..2 = a trailer */
static void* writer(void* arg)
{
    struct writer_args* a = (struct writer_args*)arg;
    for (int i = 0; i < a->count; i++) {
        send_message(a->sock, i % 3, i, (i % 4) - 1);
    }
    return NULL;
}

int main(void)
{
    int sv[2];
    int base = open_fds();

    /* 1. a stream of mixed messages, the reader already waiting */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    struct writer_args wa = { sv[0], 20000 };
    pthread_t t;
    pthread_create(&t, NULL, writer, &wa);
    for (int i = 0; i < wa.count; i++) {
        receive_message(sv[1], i, (i % 4) - 1);
    }
    pthread_join(t, NULL);

    /* 2. JACK_MAX_PASSED_FDS at once */
    send_message(sv[0], 0, 1, JACK_MAX_PASSED_FDS);
    receive_message(sv[1], 1, JACK_MAX_PASSED_FDS);

    /* 3. more fds than the receiver takes: refused, nothing leaks */
    int before = open_fds();
    send_message(sv[0], 1, 2, 3);
    struct header h; struct body b; int fds[JACK_MAX_PASSED_FDS];
    read_exact(sv[1], &h, sizeof(h)); read_exact(sv[1], &b, sizeof(b));
    CHECK(JackRecvFds(sv[1], fds, 2) == -1, "3 fds into room for 2 must fail");
    CHECK(open_fds() == before, "refused fds leaked (%d -> %d)", before, open_fds());

    /* 4. a plain read() of the trailer byte: the fds are lost, and not leaked */
    send_message(sv[0], 0, 3, 2);
    read_exact(sv[1], &h, sizeof(h)); read_exact(sv[1], &b, sizeof(b));
    unsigned char byte = 0xff;
    CHECK(read(sv[1], &byte, 1) == 1 && byte == 2, "trailer byte");
    CHECK(open_fds() == before, "fds of a plainly read trailer leaked");

    /* 5. a trailer expected where none was sent: an error, not a hang
          (the next message's first byte is taken for a count with no fds) */
    send_message(sv[0], 0, 4, -1);
    send_message(sv[0], 0, 5, -1);
    read_exact(sv[1], &h, sizeof(h)); read_exact(sv[1], &b, sizeof(b));
    CHECK(JackRecvFds(sv[1], fds, JACK_MAX_PASSED_FDS) == -1, "missing trailer must fail");

    close(sv[0]);
    close(sv[1]);
    CHECK(open_fds() == base, "fds leaked overall (%d -> %d)", base, open_fds());

    printf("fd_trailer: %s (%d messages)\n", failures ? "FAIL" : "PASS", wa.count + 4);
    return failures ? 1 : 0;
}
