/*
 bus_proto: protocol errors on the JackBus socket. Each case connects to
 $XDG_RUNTIME_DIR/jackbus/<server> and misbehaves; the server must drop the
 connection (or refuse the request) and go on serving everyone else.

   bus_proto <server> <p9open|garbage|badver|notrailer|partial|silent>

   p9open     a protocol-9 kClientOpen as the first request: dropped
   garbage    random bytes: dropped
   badver     kBusHello version 99: refused (result -1, the server's version),
              then version 1 on the same connection: accepted
   notrailer  kBusHello, then kBusClientOpen without its fd trailer: dropped
   partial    half a request, then nothing: dropped after the read timeout
   silent     connects and says nothing for 3 s: the server isn't held up
   spoof <victim>  a full open by hand, then kRegisterPort on behalf of the
              client named <victim> (refused: result -1), then for itself (0)
              (other clients are served meanwhile; checked by the caller)
   samename   two clients check the name "dup" (both told it is free), then
              both open with it: the second is refused (the name was taken
              since its check); it goes away, and a third client opens (the
              first's futex is still there for the server to hand it)
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/uio.h>

enum { kClientOpen = 23, kBusHello = 100, kBusClientOpen = 101 };

static int connect_bus(const char* server)
{
    const char* rt = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/jackbus/%s", rt ? rt : "", server);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return -1;
    }
    return fd;
}

// 1 if the server closed the connection within ms, 0 if not
static int dropped(int fd, int ms)
{
    struct pollfd p = { fd, POLLIN, 0 };
    if (poll(&p, 1, ms) <= 0) {
        return 0;
    }
    // EOF, or a reset: closing a unix socket with unread data resets the peer
    char c;
    ssize_t r = recv(fd, &c, 1, MSG_DONTWAIT);
    return r == 0 || (r < 0 && errno == ECONNRESET);
}

static int send_hello(int fd, int version)
{
    unsigned char pkt[4 + 4 + 16];
    int type = kBusHello, size = 16, reserved = 0;
    uint64_t features = 0;
    memcpy(pkt, &type, 4);
    memcpy(pkt + 4, &size, 4);
    memcpy(pkt + 8, &version, 4);
    memcpy(pkt + 12, &reserved, 4);
    memcpy(pkt + 16, &features, 8);
    return write(fd, pkt, sizeof(pkt)) == (ssize_t)sizeof(pkt) ? 0 : -1;
}

static int read_hello(int fd, int* result, int* version)
{
    unsigned char buf[16];
    size_t got = 0;
    while (got < sizeof(buf)) {
        ssize_t r = read(fd, buf + got, sizeof(buf) - got);
        if (r <= 0) return -1;
        got += r;
    }
    memcpy(result, buf, 4);
    memcpy(version, buf + 4, 4);
    return 0;
}

/* ---- a complete JackBus open by hand, for spoof ---- */

enum { kRegisterPort = 1, kClientCheck = 22 };
#define NAME1 65                 /* JACK_CLIENT_NAME_SIZE + 1 */
#define NOTIFY_SIZE (4 + NAME1 + 5 * 4 + 257)

static int write_all(int fd, const void* buf, size_t len) { return write(fd, buf, len) == (ssize_t)len ? 0 : -1; }
static int read_all(int fd, void* buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t r = read(fd, (char*)buf + got, len - got);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

static int send_trailer(int sock, int fd)
{
    unsigned char count = 1;
    struct iovec iov = { &count, 1 };
    union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } u;
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    memset(&u, 0, sizeof(u));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof(u.buf);
    struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    return sendmsg(sock, &msg, 0) == 1 ? 0 : -1;
}

static int recv_trailer(int sock)   /* closes what came; the fd count, -1 on error */
{
    unsigned char count = 0;
    struct iovec iov = { &count, 1 };
    union { char buf[CMSG_SPACE(sizeof(int) * 8)]; struct cmsghdr align; } u;
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof(u.buf);
    if (recvmsg(sock, &msg, 0) != 1) return -1;
    int n = 0;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        int k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (int i = 0; i < k; i++) {
            int fd;
            memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
            close(fd);
            n++;
        }
    }
    return n;
}

static const char* victim_name;
static atomic_int victim_ref = -1;      /* written by the notification thread */

static void* notify_thread(void* arg)
{
    int fd = *(int*)arg;
    unsigned char ev[NOTIFY_SIZE];
    while (read_all(fd, ev, sizeof(ev)) == 0) {
        int refnum, notify, sync;
        char name[NAME1];
        memcpy(name, ev + 4, NAME1);
        name[NAME1 - 1] = 0;
        memcpy(&refnum, ev + 4 + NAME1, 4);
        memcpy(&notify, ev + 4 + NAME1 + 4, 4);
        memcpy(&sync, ev + 4 + NAME1 + 16, 4);
        int ok = 0;
        if (notify == 0) {          /* kAddClient: its futex trailer */
            int n = recv_trailer(fd);
            if (n < 0) break;
            ok = n == 1 ? 0 : -1;   /* no futex file: refused, as a JackBus client does */
            if (victim_name && strcmp(name, victim_name) == 0) atomic_store(&victim_ref, refnum);
        }
        if (sync) {
            if (write_all(fd, &ok, 4) < 0) break;
        }
    }
    return NULL;
}

static int register_port(int fd, int refnum, const char* pname, int* result)
{
    unsigned char pkt[8 + 4 + 257 + 33 + 4 + 4];
    int type = kRegisterPort, size = sizeof(pkt) - 8;
    unsigned int flags = 2 /* JackPortIsOutput */, bufsize = 0;
    memset(pkt, 0, sizeof(pkt));
    memcpy(pkt, &type, 4);
    memcpy(pkt + 4, &size, 4);
    memcpy(pkt + 8, &refnum, 4);
    strncpy((char*)pkt + 12, pname, 256);
    strcpy((char*)pkt + 12 + 257, "32 bit float mono audio");
    memcpy(pkt + 12 + 257 + 33, &flags, 4);
    memcpy(pkt + 12 + 257 + 33 + 4, &bufsize, 4);
    unsigned char res[8];
    if (write_all(fd, pkt, sizeof(pkt)) < 0 || read_all(fd, res, sizeof(res)) < 0) return -1;
    memcpy(result, res, 4);
    return 0;
}

static int spoof(int fd, const char* victim)
{
    int result = -1, version = 0;
    victim_name = victim;
    if (send_hello(fd, 1) < 0 || read_hello(fd, &result, &version) < 0 || result != 0) return 0;

    /* kClientCheck: name[65] protocol options uuid open */
    unsigned char chk[8 + NAME1 + 4 + 4 + 8 + 4];
    int type = kClientCheck, size = sizeof(chk) - 8, protocol = 9, options = 0x01 /* JackNoStartServer */, open = 0;
    uint64_t uuid = 0;
    memset(chk, 0, sizeof(chk));
    memcpy(chk, &type, 4);
    memcpy(chk + 4, &size, 4);
    strcpy((char*)chk + 8, "spoofer");
    memcpy(chk + 8 + NAME1, &protocol, 4);
    memcpy(chk + 8 + NAME1 + 4, &options, 4);
    memcpy(chk + 8 + NAME1 + 8, &uuid, 8);
    memcpy(chk + 8 + NAME1 + 16, &open, 4);
    unsigned char chkres[4 + NAME1 + 4];
    if (write_all(fd, chk, sizeof(chk)) < 0 || read_all(fd, chkres, sizeof(chkres)) < 0) return 0;
    memcpy(&result, chkres, 4);
    if (result != 0) return 0;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return 0;
    pthread_t t;
    pthread_create(&t, NULL, notify_thread, &sv[0]);

    /* kBusClientOpen: pid uuid name[65], then the notification socket */
    unsigned char op[8 + 4 + 8 + NAME1];
    int pid = getpid();
    type = kBusClientOpen;
    size = sizeof(op) - 8;
    memset(op, 0, sizeof(op));
    memcpy(op, &type, 4);
    memcpy(op + 4, &size, 4);
    memcpy(op + 8, &pid, 4);
    memcpy(op + 12, &uuid, 8);
    memcpy(op + 20, chkres + 4, NAME1);
    int opres[5];
    if (write_all(fd, op, sizeof(op)) < 0 || send_trailer(fd, sv[1]) < 0) return 0;
    close(sv[1]);
    if (read_all(fd, opres, sizeof(opres)) < 0 || opres[0] != 0 || recv_trailer(fd) != 5) return 0;
    int own = opres[4];
    int vref = atomic_load(&victim_ref);
    if (vref < 0) {
        fprintf(stderr, "spoof: no client named %s\n", victim);
        return 0;
    }
    int foreign = 0, mine = -1;
    if (register_port(fd, vref, "spoofed", &foreign) < 0) return 0;
    if (register_port(fd, own, "mine", &mine) < 0) return 0;
    printf("spoof: own refnum %d, %s is %d: for it %d, for itself %d\n", own, victim, vref, foreign, mine);
    shutdown(sv[0], SHUT_RDWR);     /* the notification thread's read ends */
    pthread_join(t, NULL);
    close(sv[0]);
    return foreign == -1 && mine == 0;
}

/* a hello on a new connection: the connection, -1 */
static int hello(const char* server)
{
    int fd = connect_bus(server), result = -1, version = 0;
    if (fd < 0) return -1;
    if (send_hello(fd, 1) < 0 || read_hello(fd, &result, &version) < 0 || result != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* kClientCheck of name: the server's result, the name it gives in out */
static int check_name(int fd, const char* name, char* out)
{
    unsigned char chk[8 + NAME1 + 4 + 4 + 8 + 4];
    int type = kClientCheck, size = sizeof(chk) - 8, protocol = 9, options = 0x01, open = 0, result = -1;
    uint64_t uuid = 0;
    memset(chk, 0, sizeof(chk));
    memcpy(chk, &type, 4);
    memcpy(chk + 4, &size, 4);
    strncpy((char*)chk + 8, name, NAME1 - 1);
    memcpy(chk + 8 + NAME1, &protocol, 4);
    memcpy(chk + 8 + NAME1 + 4, &options, 4);
    memcpy(chk + 8 + NAME1 + 8, &uuid, 8);
    memcpy(chk + 8 + NAME1 + 16, &open, 4);
    unsigned char res[4 + NAME1 + 4];
    if (write_all(fd, chk, sizeof(chk)) < 0 || read_all(fd, res, sizeof(res)) < 0) return -1;
    memcpy(&result, res, 4);
    memcpy(out, res + 4, NAME1);
    out[NAME1 - 1] = 0;
    return result;
}

/* kBusClientOpen as name, its notifications answered by a thread: the server's
 * result (0 opened); *notify the client's end of the notification socket */
static int open_as(int fd, const char* name, int* notify, pthread_t* t)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return -1;
    int* mine = malloc(sizeof(int));
    *mine = sv[0];
    pthread_create(t, NULL, notify_thread, mine);
    unsigned char op[8 + 4 + 8 + NAME1];
    int type = kBusClientOpen, size = sizeof(op) - 8, pid = getpid();
    uint64_t uuid = 0;
    memset(op, 0, sizeof(op));
    memcpy(op, &type, 4);
    memcpy(op + 4, &size, 4);
    memcpy(op + 8, &pid, 4);
    memcpy(op + 12, &uuid, 8);
    strncpy((char*)op + 20, name, NAME1 - 1);
    int opres[5];
    if (write_all(fd, op, sizeof(op)) < 0 || send_trailer(fd, sv[1]) < 0) return -1;
    close(sv[1]);
    if (read_all(fd, opres, sizeof(opres)) < 0) return -1;
    if (opres[0] == 0 && recv_trailer(fd) != 5) return -1;
    *notify = sv[0];
    return opres[0];
}

static int samename(const char* server)
{
    char a_name[NAME1], b_name[NAME1], c_name[NAME1];
    int a = hello(server), b = hello(server);
    if (a < 0 || b < 0) return 0;
    int ca = check_name(a, "dup", a_name), cb = check_name(b, "dup", b_name);
    printf("samename: both checked: %d \"%s\", %d \"%s\"\n", ca, a_name, cb, b_name);
    if (ca != 0 || cb != 0 || strcmp(a_name, "dup") || strcmp(b_name, "dup")) return 0;
    int na = -1, nb = -1, nc = -1;
    pthread_t ta, tb, tc;
    int ra = open_as(a, a_name, &na, &ta);
    int rb = open_as(b, b_name, &nb, &tb);
    printf("samename: the first opens: %d; the second, as the same name: %d\n", ra, rb);
    /* the second goes away (refused, or opened: its process killed) */
    close(b);
    if (nb >= 0) {
        shutdown(nb, SHUT_RDWR);
        pthread_join(tb, NULL);
        close(nb);
    }
    usleep(500 * 1000);
    int c = hello(server);
    int cc = c < 0 ? -1 : check_name(c, "probe", c_name);
    int rc = cc == 0 ? open_as(c, c_name, &nc, &tc) : -1;
    printf("samename: then a third client opens: %d\n", rc);
    if (c >= 0) close(c);
    if (nc >= 0) {
        shutdown(nc, SHUT_RDWR);
        pthread_join(tc, NULL);
        close(nc);
    }
    close(a);
    if (na >= 0) {
        shutdown(na, SHUT_RDWR);
        pthread_join(ta, NULL);
        close(na);
    }
    return ra == 0 && rb != 0 && rc == 0;
}

int main(int argc, char* argv[])
{
    if (argc < 3) {
        fprintf(stderr, "usage: bus_proto <server> <case>\n");
        return 2;
    }
    const char* what = argv[2];
    int fd = connect_bus(argv[1]);
    if (fd < 0) return 1;
    int ok = 0;

    if (strcmp(what, "p9open") == 0) {
        // kClientOpen: [type][size][pid][uuid 8][name 65]
        unsigned char pkt[8 + 4 + 8 + 65];
        int type = kClientOpen, size = 4 + 8 + 65;
        memset(pkt, 0, sizeof(pkt));
        memcpy(pkt, &type, 4);
        memcpy(pkt + 4, &size, 4);
        strcpy((char*)pkt + 20, "intruder");
        ok = write(fd, pkt, sizeof(pkt)) == (ssize_t)sizeof(pkt) && dropped(fd, 3000);
    } else if (strcmp(what, "garbage") == 0) {
        unsigned char junk[256];
        for (size_t i = 0; i < sizeof(junk); i++) junk[i] = (unsigned char)(i * 37 + 11);
        ok = write(fd, junk, sizeof(junk)) == (ssize_t)sizeof(junk) && dropped(fd, 3000);
    } else if (strcmp(what, "badver") == 0) {
        int result = 0, version = 0;
        ok = send_hello(fd, 99) == 0 && read_hello(fd, &result, &version) == 0 && result == -1 && version == 1;
        ok = ok && send_hello(fd, 1) == 0 && read_hello(fd, &result, &version) == 0 && result == 0;
    } else if (strcmp(what, "notrailer") == 0) {
        int result = -1, version = 0;
        ok = send_hello(fd, 1) == 0 && read_hello(fd, &result, &version) == 0 && result == 0;
        // kBusClientOpen: [type][size][pid][uuid 8][name 65], and no trailer
        unsigned char pkt[8 + 4 + 8 + 65];
        int type = kBusClientOpen, size = 4 + 8 + 65;
        memset(pkt, 0, sizeof(pkt));
        memcpy(pkt, &type, 4);
        memcpy(pkt + 4, &size, 4);
        strcpy((char*)pkt + 20, "notrailer");
        ok = ok && write(fd, pkt, sizeof(pkt)) == (ssize_t)sizeof(pkt);
        // the server waits for the trailer's byte: send a plain one (no fds)
        unsigned char zero = 0;
        ok = ok && write(fd, &zero, 1) == 1 && dropped(fd, 3000);
    } else if (strcmp(what, "partial") == 0) {
        // a hello's header and half its body, then nothing: dropped after the
        // server's read timeout (seconds), not held forever
        unsigned char pkt[12];
        int type = kBusHello, size = 16, version = 1;
        memcpy(pkt, &type, 4);
        memcpy(pkt + 4, &size, 4);
        memcpy(pkt + 8, &version, 4);
        ok = write(fd, pkt, sizeof(pkt)) == (ssize_t)sizeof(pkt) && dropped(fd, 8000);
    } else if (strcmp(what, "spoof") == 0) {
        ok = argc > 3 && spoof(fd, argv[3]);
    } else if (strcmp(what, "samename") == 0) {
        ok = samename(argv[1]);
    } else if (strcmp(what, "silent") == 0) {
        sleep(3);
        ok = !dropped(fd, 0);
    } else {
        fprintf(stderr, "unknown case %s\n", what);
        return 2;
    }
    close(fd);
    printf("bus_proto %s: %s\n", what, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
