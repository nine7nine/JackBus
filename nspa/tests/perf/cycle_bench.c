/* JackBus cycle bench: graphs of clients whose process callbacks note, in ns on
 * CLOCK_MONOTONIC_RAW (the clock JACK's µs come from, and the server's cycle trace's), when
 * they start and end, with the cycle's frame time. With the server's trace
 * (JACK_CYCLE_TRACE, joined on the frame time by analyze.py) that gives each cycle's
 * timeline: the driver's parts, each client's wake-up and its callback.
 *
 *   cycle_bench [-d secs] [-w warmup] [-s spin_us] [-c channels] [-t] -o file group...
 *   cycle_bench -q     exit status 0 if a server answers
 *
 * group: kind:n[@src][>dst]
 *   chain:n   n clients in series
 *   fan:n     n clients side by side
 *   grid:mxk  m chains of k clients side by side
 * src: the first clients' inputs come from <src>1, <src>2, ... (default system:capture_;
 *      a bridge: Loopback:capture_); "-": unconnected. dst: the last clients' outputs go to
 *      <dst>N (default system:playback_; "-": nowhere).
 * Each client copies its inputs to its outputs and spins for spin_us (default 0) from its start.
 * One process per client (as applications are), or with -t threads of one process.
 * -e rate: while recording, the controller connects and disconnects the first client's out1
 * and system:playback_2 at that rate (graph edits, as a patchbay makes), each call's time noted.
 *
 * After warmup (default 2 s) it records for secs (default 5), writes <file>, and stays
 * connected until the server goes away (the harness stops the server then: its trace is
 * written with these clients still in the graph) or 20 s pass.
 * Exit status: 0, or 1 if anything failed.
 */
#define _GNU_SOURCE
#include <jack/jack.h>
#include <dirent.h>
#include <execinfo.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAXCLIENTS 64
#define MAXCH 8
#define CAP 65536   /* records per client (a ring) */

struct rec {
    uint32_t frames;
    uint32_t pad;
    int64_t entry, exit;
};

struct client {
    char name[32];
    char src[64], dst[64];      /* port prefixes ("" : none) */
    int pred;                   /* the client before it in a chain, or -1 */
    int group;
    _Atomic int ready;          /* 1: activated; -1: failed */
    atomic_ulong count;         /* records written */
    atomic_ulong xruns;
    _Atomic int recording;
    struct rec ring[CAP];
};

struct shared {
    int nclients, channels;
    int64_t spin_ns;
    _Atomic int go;             /* 1: record; 2: stop recording */
    _Atomic int quit;           /* the server went away or time is up */
    struct client cl[MAXCLIENTS];
};

static struct shared *sh;

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

struct local {
    struct client *c;
    jack_client_t *jc;
    jack_port_t *in[MAXCH], *out[MAXCH];
};

static int process(jack_nframes_t n, void *arg)
{
    struct local *l = arg;
    const int64_t t0 = now_ns();
    const jack_nframes_t f = jack_last_frame_time(l->jc);
    for (int i = 0; i < sh->channels; i++) {
        const float *src = jack_port_get_buffer(l->in[i], n);
        float *dst = jack_port_get_buffer(l->out[i], n);
        memcpy(dst, src, n * sizeof(float));
    }
    if (sh->spin_ns > 0) {
        while (now_ns() - t0 < sh->spin_ns)
            ;
    }
    if (atomic_load_explicit(&sh->go, memory_order_relaxed) == 1) {
        unsigned long k = atomic_load_explicit(&l->c->count, memory_order_relaxed);
        struct rec *r = &l->c->ring[k % CAP];
        r->frames = f;
        r->entry = t0;
        r->exit = now_ns();
        atomic_store_explicit(&l->c->count, k + 1, memory_order_release);
    }
    return 0;
}

static int on_xrun(void *arg)
{
    struct local *l = arg;
    if (atomic_load_explicit(&sh->go, memory_order_relaxed) == 1)
        atomic_fetch_add(&l->c->xruns, 1);
    return 0;
}

static void on_shutdown(void *arg)
{
    (void)arg;
    atomic_store(&sh->quit, 1);
}

static struct local *open_client(struct client *c)
{
    /* the ring's pages present (and locked if allowed) before the callback writes them */
    memset(c->ring, 0, sizeof c->ring);
    (void)mlock(c->ring, sizeof c->ring);
    struct local *l = calloc(1, sizeof *l);
    jack_status_t st;
    char p[64];
    l->c = c;
    l->jc = jack_client_open(c->name, JackNoStartServer | JackUseExactName, &st);
    if (!l->jc) {
        fprintf(stderr, "cannot open %s (status 0x%x)\n", c->name, st);
        return NULL;
    }
    for (int i = 0; i < sh->channels; i++) {
        snprintf(p, sizeof p, "in%d", i + 1);
        l->in[i] = jack_port_register(l->jc, p, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        snprintf(p, sizeof p, "out%d", i + 1);
        l->out[i] = jack_port_register(l->jc, p, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (!l->in[i] || !l->out[i]) {
            fprintf(stderr, "cannot register the ports of %s\n", c->name);
            return NULL;
        }
    }
    jack_set_process_callback(l->jc, process, l);
    jack_set_xrun_callback(l->jc, on_xrun, l);
    jack_on_shutdown(l->jc, on_shutdown, NULL);
    if (jack_activate(l->jc)) {
        fprintf(stderr, "cannot activate %s\n", c->name);
        return NULL;
    }
    return l;
}

/* A close that does not return (seen once after the server stopped): every thread's stack to
 * stderr after 5 s, then exit after 15 s (no process left behind). A debugging aid only. */
static void stack_here(int sig)
{
    (void)sig;
    void *frames[64];
    char head[64];
    int n = snprintf(head, sizeof head, "--- thread %ld:\n", (long)syscall(SYS_gettid));
    write(2, head, n);
    backtrace_symbols_fd(frames, backtrace(frames, 64), 2);
}

static void close_hung(int sig)
{
    (void)sig;
    static int fired;
    if (fired++) {
        write(2, "close still hung: exiting\n", 26);
        _exit(3);
    }
    write(2, "jack_client_close did not return within 5 s\n", 45);
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        long tid = atol(e->d_name);
        if (tid > 0)
            syscall(SYS_tgkill, getpid(), tid, SIGUSR2);
    }
    if (d)
        closedir(d);
    alarm(10);
}

static void child(struct client *c)
{
    struct local *l = open_client(c);
    atomic_store(&c->ready, l ? 1 : -1);
    if (!l)
        _exit(1);
    while (!atomic_load(&sh->quit))
        usleep(20000);
    void *warm[1];
    backtrace(warm, 1);     /* loads what backtrace needs now, not in a signal handler */
    signal(SIGUSR2, stack_here);
    signal(SIGALRM, close_hung);
    alarm(5);
    jack_client_close(l->jc);
    alarm(0);
    _exit(0);
}

/* the ports <prefix>1, <prefix>2, ... that exist (a card bridged with fewer channels than
 * the clients have: its ports are used round), waiting up to 5 s for the first */
static int count_ports(jack_client_t *ctl, const char *prefix)
{
    char name[128];
    int n = 0;
    for (int i = 0; i < 100 && n == 0; i++) {
        while (n < 64) {
            snprintf(name, sizeof name, "%s%d", prefix, n + 1);
            if (!jack_port_by_name(ctl, name))
                break;
            n++;
        }
        if (n == 0)
            usleep(50000);
    }
    return n;
}

static int connect_retry(jack_client_t *ctl, const char *a, const char *b)
{
    for (int i = 0; i < 100; i++) {
        if (jack_connect(ctl, a, b) == 0)
            return 0;
        usleep(50000);
    }
    fprintf(stderr, "cannot connect %s to %s\n", a, b);
    return -1;
}

static int parse_group(const char *g, int group)
{
    char kind[16] = "", src[64] = "system:capture_", dst[64] = "system:playback_";
    const char *colon = strchr(g, ':');
    if (!colon || colon - g >= (int)sizeof kind)
        return -1;
    memcpy(kind, g, colon - g);
    const char *spec = colon + 1;
    int m = 1, k = 0;
    char *end;
    k = (int)strtol(spec, &end, 10);
    if (strcmp(kind, "grid") == 0) {
        if (*end != 'x')
            return -1;
        m = k;
        k = (int)strtol(end + 1, &end, 10);
    } else if (strcmp(kind, "fan") == 0) {
        m = k;
        k = 1;
    } else if (strcmp(kind, "chain") != 0) {
        return -1;
    }
    const char *at = strchr(end, '@'), *gt = strchr(end, '>');
    if (at) {
        size_t len = gt && gt > at ? (size_t)(gt - at - 1) : strlen(at + 1);
        if (len >= sizeof src)
            return -1;
        memcpy(src, at + 1, len);
        src[len] = 0;
    }
    if (gt) {
        size_t len = at && at > gt ? (size_t)(at - gt - 1) : strlen(gt + 1);
        if (len >= sizeof dst)
            return -1;
        memcpy(dst, gt + 1, len);
        dst[len] = 0;
    }
    if (strcmp(src, "-") == 0)
        src[0] = 0;
    if (strcmp(dst, "-") == 0)
        dst[0] = 0;
    if (m < 1 || k < 1 || sh->nclients + m * k > MAXCLIENTS)
        return -1;
    for (int a = 0; a < m; a++) {
        for (int b = 0; b < k; b++) {
            struct client *c = &sh->cl[sh->nclients];
            snprintf(c->name, sizeof c->name, "b%d_%d_%d", group, a, b);
            c->group = group;
            c->pred = b > 0 ? sh->nclients - 1 : -1;
            if (b == 0)
                snprintf(c->src, sizeof c->src, "%s", src);
            if (b == k - 1)
                snprintf(c->dst, sizeof c->dst, "%s", dst);
            sh->nclients++;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    double secs = 5, warmup = 2, spin_us = 0, edit_rate = 0;
    int threads = 0, channels = 2, opt, fail = 0;
    const char *outfile = NULL;

    if (argc == 2 && strcmp(argv[1], "-q") == 0) {     /* is a server there? */
        jack_client_t *probe = jack_client_open("bench_probe", JackNoStartServer, NULL);
        if (probe)
            jack_client_close(probe);
        return probe ? 0 : 1;
    }
    sh = mmap(NULL, sizeof *sh, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sh == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    while ((opt = getopt(argc, argv, "d:w:s:c:to:e:")) != -1) {
        switch (opt) {
        case 'd': secs = atof(optarg); break;
        case 'w': warmup = atof(optarg); break;
        case 's': spin_us = atof(optarg); break;
        case 'c': channels = atoi(optarg); break;
        case 't': threads = 1; break;
        case 'o': outfile = optarg; break;
        case 'e': edit_rate = atof(optarg); break;
        default: return 2;
        }
    }
    if (!outfile || optind >= argc || channels < 1 || channels > MAXCH) {
        fprintf(stderr, "usage: cycle_bench [-d secs] [-w warmup] [-s spin_us] [-c channels] [-t] -o file group...\n");
        return 2;
    }
    sh->channels = channels;
    sh->spin_ns = (int64_t)(spin_us * 1000);
    for (int i = optind; i < argc; i++) {
        if (parse_group(argv[i], i - optind) < 0) {
            fprintf(stderr, "bad group %s\n", argv[i]);
            return 2;
        }
    }

    /* children first, before this process has any libjack state to inherit */
    pid_t pids[MAXCLIENTS] = {0};
    struct local *locals[MAXCLIENTS] = {0};
    for (int i = 0; i < sh->nclients && !threads; i++) {
        pids[i] = fork();
        if (pids[i] == 0)
            child(&sh->cl[i]);
        if (pids[i] < 0) {
            perror("fork");
            atomic_store(&sh->quit, 1);
            return 1;
        }
    }

    /* the controller: connects, watches for the server going away; no process callback */
    jack_status_t st;
    jack_client_t *ctl = jack_client_open("bench_ctl", JackNoStartServer, &st);
    if (!ctl) {
        fprintf(stderr, "no server (status 0x%x)\n", st);
        atomic_store(&sh->quit, 1);
        return 1;
    }
    jack_on_shutdown(ctl, on_shutdown, NULL);
    for (int i = 0; i < sh->nclients && threads; i++) {
        locals[i] = open_client(&sh->cl[i]);
        atomic_store(&sh->cl[i].ready, locals[i] ? 1 : -1);
    }
    for (int i = 0; i < sh->nclients; i++) {
        int waited = 0;
        while (atomic_load(&sh->cl[i].ready) == 0 && waited++ < 1000)
            usleep(10000);
        if (atomic_load(&sh->cl[i].ready) != 1) {
            fprintf(stderr, "client %s did not start\n", sh->cl[i].name);
            fail = 1;
        }
    }

    char a[128], b[128];
    for (int i = 0; i < sh->nclients && !fail; i++) {
        struct client *c = &sh->cl[i];
        for (int ch = 0; ch < channels; ch++) {
            if (c->pred >= 0) {
                snprintf(a, sizeof a, "%s:out%d", sh->cl[c->pred].name, ch + 1);
                snprintf(b, sizeof b, "%s:in%d", c->name, ch + 1);
                if (connect_retry(ctl, a, b))
                    fail = 1;
            }
            if (c->src[0]) {
                const int n = count_ports(ctl, c->src);
                snprintf(a, sizeof a, "%s%d", c->src, n > 0 ? ch % n + 1 : ch + 1);
                snprintf(b, sizeof b, "%s:in%d", c->name, ch + 1);
                if (connect_retry(ctl, a, b))
                    fail = 1;
            }
            if (c->dst[0]) {
                const int n = count_ports(ctl, c->dst);
                if (n > 0 && ch >= n)
                    continue;   /* fewer playback ports than channels: the first ones */
                snprintf(a, sizeof a, "%s:out%d", c->name, ch + 1);
                snprintf(b, sizeof b, "%s%d", c->dst, ch + 1);
                if (connect_retry(ctl, a, b))
                    fail = 1;
            }
        }
    }

    static int64_t edits[100000];
    int nedits = 0;
    char edit_src[64];
    snprintf(edit_src, sizeof edit_src, "%s:out1", sh->cl[0].name);
    if (!fail) {
        usleep((useconds_t)(warmup * 1e6));
        atomic_store(&sh->go, 1);
        if (edit_rate > 0) {
            const int64_t start = now_ns(), step = (int64_t)(1e9 / edit_rate);
            for (int64_t k = 0; now_ns() - start < (int64_t)(secs * 1e9) && !atomic_load(&sh->quit); k++) {
                const int64_t t0 = now_ns();
                int r = (k & 1) ? jack_disconnect(ctl, edit_src, "system:playback_2")
                                : jack_connect(ctl, edit_src, "system:playback_2");
                if (r == 0 && nedits < 100000)
                    edits[nedits++] = now_ns() - t0;
                const int64_t next = start + (k + 1) * step, left = next - now_ns();
                if (left > 0)
                    usleep((useconds_t)(left / 1000));
            }
            if (nedits & 1)
                jack_disconnect(ctl, edit_src, "system:playback_2");
        } else {
            for (int64_t t = 0; t < (int64_t)(secs * 100) && !atomic_load(&sh->quit); t++)
                usleep(10000);
        }
        atomic_store(&sh->go, 2);
        usleep(50000);  /* the callbacks in flight */
        if (atomic_load(&sh->quit)) {
            fprintf(stderr, "the server went away while recording\n");
            fail = 1;
        }
    }

    /* the records, then the file under its name (the harness waits for it) */
    if (!fail) {
        char tmp[512];
        snprintf(tmp, sizeof tmp, "%s.tmp", outfile);
        FILE *f = fopen(tmp, "w");
        if (!f) {
            perror(tmp);
            fail = 1;
        } else {
            fprintf(f, "# cycle_bench 1\nrate %u period %u channels %d spin_ns %lld mode %s\n",
                    jack_get_sample_rate(ctl), jack_get_buffer_size(ctl), channels,
                    (long long)sh->spin_ns, threads ? "threads" : "processes");
            for (int i = 0; i < sh->nclients; i++) {
                struct client *c = &sh->cl[i];
                fprintf(f, "client %d %s group %d pred %s src %s dst %s xruns %lu count %lu\n", i, c->name, c->group,
                        c->pred >= 0 ? sh->cl[c->pred].name : "-", c->src[0] ? c->src : "-",
                        c->dst[0] ? c->dst : "-", atomic_load(&c->xruns), atomic_load(&c->count));
            }
            for (int k = 0; k < nedits; k++)
                fprintf(f, "e %lld\n", (long long)edits[k]);
            for (int i = 0; i < sh->nclients; i++) {
                struct client *c = &sh->cl[i];
                unsigned long n = atomic_load(&c->count);
                unsigned long first = n > CAP ? n - CAP : 0;
                for (unsigned long k = first; k < n; k++) {
                    const struct rec *r = &c->ring[k % CAP];
                    fprintf(f, "r %d %u %lld %lld\n", i, r->frames, (long long)r->entry, (long long)r->exit);
                }
            }
            if (fclose(f) != 0 || rename(tmp, outfile) != 0) {
                perror(outfile);
                fail = 1;
            }
        }
    }
    printf("%s: %d clients, %s\n", fail ? "FAIL" : "done", sh->nclients, outfile);
    fflush(stdout);

    /* stay in the graph until the server stops (or 20 s) */
    for (int t = 0; t < 2000 && !atomic_load(&sh->quit); t++)
        usleep(10000);
    atomic_store(&sh->quit, 1);
    for (int i = 0; i < sh->nclients; i++) {
        if (threads && locals[i])
            jack_client_close(locals[i]->jc);
        if (pids[i] > 0) {
            int status;
            if (waitpid(pids[i], &status, 0) == pids[i] && (!WIFEXITED(status) || WEXITSTATUS(status) != 0))
                fail = 1;
        }
    }
    jack_client_close(ctl);
    return fail;
}
