/*
 * client_kill: what the rest of the graph sees when one client goes away
 * while the server is running, either by jack_client_close() or because its
 * process died without closing (killed, crashed, _exit).
 *
 *   client_kill <close|exit|kill|killfork|churn|late> <trials> [ports=40] [mem_mb=0]
 *
 * killfork: like kill, but the victim first forks a child that sleeps 5 s: the
 * child inherits libjack's sockets (not close-on-exec), so the server's request
 * socket for the victim stays open after the victim died.
 *
 * late: the victim stays, but one of its process callbacks takes
 * NSPA_LATE_PERIODS (default 1.5) periods; then it closes.
 *
 * churn: the measurement covers the victim's whole life (open, activate,
 * connect, close), not only its departure.
 *
 * Graph, per trial:   src --> victim (own process, <ports> in/out) --> sink
 *                     src --> bystander             (parallel to the victim)
 * The victim allocates and touches <mem_mb> MB (and tries to mlock it), so its
 * address space takes a while to tear down when the process dies.
 *
 * Reported per trial:
 *   xruns       xrun callbacks
 *   sink        cycles the sink (downstream of the victim) did not run
 *   bystander   cycles the bystander (not downstream) did not run
 *   reap        time from the kill/close to the victim's process being reaped
 *   gone        time to the client-unregistration notification
 *   unreg       port-unregistration notifications received (2 x ports expected)
 * Last line: PASS/FAIL against the limits given in NSPA_KILL_LIMITS
 * ("xruns sink unreg_exact", default "-1 -1 0" = report only).
 */
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <jack/jack.h>

#define MAXP 128

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ---------------------------------------------------------------- victim */

static jack_port_t *vin[MAXP], *vout[MAXP];
static int vports;
static atomic_int late_once;
static double late_us;

static void spin_us(double us)
{
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    do {
        clock_gettime(CLOCK_MONOTONIC, &b);
    } while ((b.tv_sec - a.tv_sec) * 1e6 + (b.tv_nsec - a.tv_nsec) / 1e3 < us);
}

static int victim_process(jack_nframes_t n, void *arg)
{
    (void)arg;
    if (atomic_exchange(&late_once, 0))
        spin_us(late_us);
    for (int i = 0; i < vports; i++) {
        float *in = jack_port_get_buffer(vin[i], n);
        float *out = jack_port_get_buffer(vout[i], n);
        memcpy(out, in, n * sizeof(float));
    }
    return 0;
}

static int victim_main(int ports, long mem_mb)
{
    vports = ports;
    jack_client_t *c = jack_client_open("victim", JackNoStartServer, NULL);
    if (!c) return 1;
    char name[64];
    for (int i = 0; i < ports; i++) {
        snprintf(name, sizeof name, "in_%d", i);
        vin[i] = jack_port_register(c, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        snprintf(name, sizeof name, "out_%d", i);
        vout[i] = jack_port_register(c, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (!vin[i] || !vout[i]) return 1;
    }
    if (mem_mb > 0) {
        size_t sz = (size_t)mem_mb << 20;
        char *m = malloc(sz);
        if (!m) return 1;
        memset(m, 1, sz);
        mlock(m, sz);   /* best effort, like a DAW that locks its memory */
    }
    const char *lp = getenv("NSPA_LATE_PERIODS");
    late_us = (lp ? atof(lp) : 1.5) * jack_get_buffer_size(c) * 1e6 / jack_get_sample_rate(c);
    jack_set_process_callback(c, victim_process, NULL);
    if (jack_activate(c)) return 1;
    for (int i = 0; i < ports; i++) {
        char a[128];
        snprintf(a, sizeof a, "victim:in_%d", i);
        jack_connect(c, "src:out", a);
        snprintf(a, sizeof a, "victim:out_%d", i);
        jack_connect(c, a, "sink:in");
    }
    if (getenv("NSPA_KILL_FORK") && fork() == 0) {
        sleep(5);
        _exit(0);
    }
    if (write(1, "R", 1) != 1) return 1;
    char cmd;
    if (read(0, &cmd, 1) != 1) _exit(3);
    while (cmd == 'l') {
        atomic_store(&late_once, 1);
        if (read(0, &cmd, 1) != 1) _exit(3);
    }
    if (cmd == 'c') {
        jack_client_close(c);
        exit(0);
    }
    _exit(0);   /* 'e': gone without closing */
}

/* ----------------------------------------------------------- observer side */

struct watch {
    jack_client_t *c;
    jack_port_t *in, *out;
    atomic_uint last, have_last, missed, maxgap, runs, dups;
};
static struct watch src, sink, bys;
static atomic_uint xruns, unregs, victim_gone;
static double gone_ms;
static jack_nframes_t period;

static void note_cycle(struct watch *w)
{
    jack_nframes_t f = jack_last_frame_time(w->c);
    atomic_fetch_add(&w->runs, 1);
    if (atomic_load(&w->have_last)) {
        jack_nframes_t d = f - atomic_load(&w->last);
        if (d == 0)
            atomic_fetch_add(&w->dups, 1);     /* ran twice in one cycle */
        if (d > period) {
            unsigned m = d / period - 1;
            atomic_fetch_add(&w->missed, m);
            if (m > atomic_load(&w->maxgap)) atomic_store(&w->maxgap, m);
        }
    }
    atomic_store(&w->last, f);
    atomic_store(&w->have_last, 1);
}

static int src_process(jack_nframes_t n, void *arg)
{
    (void)arg;
    float *o = jack_port_get_buffer(src.out, n);
    for (jack_nframes_t i = 0; i < n; i++) o[i] = 0.0f;
    return 0;
}
static int sink_process(jack_nframes_t n, void *arg)
{
    (void)arg;
    jack_port_get_buffer(sink.in, n);
    note_cycle(&sink);
    return 0;
}
static int bys_process(jack_nframes_t n, void *arg)
{
    (void)arg;
    jack_port_get_buffer(bys.in, n);
    note_cycle(&bys);
    return 0;
}
static int on_xrun(void *arg) { (void)arg; atomic_fetch_add(&xruns, 1); return 0; }
static void on_port_reg(jack_port_id_t id, int reg, void *arg)
{
    (void)id; (void)arg;
    if (!reg) atomic_fetch_add(&unregs, 1);
}
static void on_client_reg(const char *name, int reg, void *arg)
{
    (void)arg;
    if (!reg && strcmp(name, "victim") == 0) {
        gone_ms = now_ms();
        atomic_store(&victim_gone, 1);
    }
}

static int open_watch(struct watch *w, const char *name, int has_in, int has_out,
                      JackProcessCallback cb)
{
    w->c = jack_client_open(name, JackNoStartServer, NULL);
    if (!w->c) return -1;
    if (has_in) w->in = jack_port_register(w->c, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    if (has_out) w->out = jack_port_register(w->c, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    jack_set_process_callback(w->c, cb, NULL);
    return 0;
}

static void reset_watch(struct watch *w)
{
    atomic_store(&w->have_last, 0);
    atomic_store(&w->missed, 0);
    atomic_store(&w->maxgap, 0);
    atomic_store(&w->runs, 0);
    atomic_store(&w->dups, 0);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "victim") == 0)
        return victim_main(atoi(argv[2]), atol(argv[3]));
    if (argc < 3) {
        fprintf(stderr, "usage: %s <close|exit|kill> <trials> [ports] [mem_mb]\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1];
    int trials = atoi(argv[2]);
    int ports = argc > 3 ? atoi(argv[3]) : 40;
    long mem_mb = argc > 4 ? atol(argv[4]) : 0;
    if (ports < 1 || ports > MAXP) return 2;
    int lim_xruns = -1, lim_sink = -1, unreg_exact = 0;
    const char *lim = getenv("NSPA_KILL_LIMITS");
    if (lim) sscanf(lim, "%d %d %d", &lim_xruns, &lim_sink, &unreg_exact);

    if (open_watch(&src, "src", 0, 1, src_process) ||
        open_watch(&sink, "sink", 1, 0, sink_process) ||
        open_watch(&bys, "bystander", 1, 0, bys_process)) {
        fprintf(stderr, "cannot open clients\n");
        return 1;
    }
    period = jack_get_buffer_size(sink.c);
    jack_set_xrun_callback(sink.c, on_xrun, NULL);
    jack_set_port_registration_callback(sink.c, on_port_reg, NULL);
    jack_set_client_registration_callback(sink.c, on_client_reg, NULL);
    if (jack_activate(src.c) || jack_activate(sink.c) || jack_activate(bys.c)) return 1;
    jack_connect(src.c, "src:out", "bystander:in");

    double rate = jack_get_sample_rate(sink.c);
    double period_ms = period * 1e3 / rate;
    unsigned tot_x = 0, tot_s = 0, tot_b = 0, max_s = 0, bad_unreg = 0, tot_extra = 0, tot_short = 0, tot_dups = 0;
    double sum_reap = 0, sum_gone = 0, max_gone = 0;
    int fail = 0;
    printf("mode %s, %d ports, %ld MB, period %u (%.2f ms)\n", mode, ports, mem_mb, period, period_ms);

    for (int t = 0; t < trials; t++) {
        int churn = strcmp(mode, "churn") == 0;
        jack_nframes_t f0 = 0;
        if (churn) {
            reset_watch(&sink);
            reset_watch(&bys);
            atomic_store(&xruns, 0);
            atomic_store(&unregs, 0);
            atomic_store(&victim_gone, 0);
            f0 = jack_frame_time(sink.c);
        }
        int to_child[2], from_child[2];
        if (pipe(to_child) || pipe(from_child)) return 1;
        char sp[16], sm[16];
        snprintf(sp, sizeof sp, "%d", ports);
        snprintf(sm, sizeof sm, "%ld", mem_mb);
        pid_t pid = fork();
        if (pid == 0) {
            if (strcmp(mode, "killfork") == 0)
                setenv("NSPA_KILL_FORK", "1", 1);
            dup2(to_child[0], 0);
            dup2(from_child[1], 1);
            close(to_child[1]);
            close(from_child[0]);
            execl("/proc/self/exe", argv[0], "victim", sp, sm, (char *)NULL);
            _exit(127);
        }
        close(to_child[0]);
        close(from_child[1]);
        char r;
        if (read(from_child[0], &r, 1) != 1) {
            fprintf(stderr, "victim did not start\n");
            return 1;
        }
        if (!churn) {
            usleep(300000);
            reset_watch(&sink);
            reset_watch(&bys);
            atomic_store(&xruns, 0);
            atomic_store(&unregs, 0);
            atomic_store(&victim_gone, 0);
            f0 = jack_frame_time(sink.c);
        }

        double t0 = now_ms();
        int late = strcmp(mode, "late") == 0;
        if (late) {
            if (write(to_child[1], "l", 1) != 1) return 1;
            usleep(300000);
            jack_nframes_t f1 = jack_frame_time(sink.c);
            unsigned cyc = (f1 - f0) / period, sr = atomic_load(&sink.runs), br = atomic_load(&bys.runs);
            unsigned sd = atomic_load(&sink.dups), bd = atomic_load(&bys.dups),
                     sm = atomic_load(&sink.missed);
            printf("trial %2d: xruns %u  sink runs %u (twice in a cycle %u, skipped %u)"
                   "  bystander runs %u (twice %u) of %u cycles\n",
                   t, atomic_load(&xruns), sr, sd, sm, br, bd, cyc);
            tot_dups += sd + bd;
            tot_x += atomic_load(&xruns);
            tot_extra += (sr > cyc + 1 ? sr - cyc - 1 : 0) + (br > cyc + 1 ? br - cyc - 1 : 0);
            tot_short += (cyc > sr ? cyc - sr : 0);
            if (write(to_child[1], "c", 1) != 1) return 1;
            waitpid(pid, NULL, 0);
            while (!atomic_load(&victim_gone) && now_ms() - t0 < 10000) usleep(1000);
            close(to_child[1]);
            close(from_child[0]);
            usleep(200000);
            continue;
        }
        if (strcmp(mode, "kill") == 0 || strcmp(mode, "killfork") == 0) {
            kill(pid, SIGKILL);
        } else {
            char cmd = (strcmp(mode, "close") == 0 || churn) ? 'c' : 'e';
            if (write(to_child[1], &cmd, 1) != 1) return 1;
        }
        waitpid(pid, NULL, 0);
        double reap = now_ms() - t0;
        while (!atomic_load(&victim_gone) && now_ms() - t0 < 10000)
            usleep(1000);
        double gone = atomic_load(&victim_gone) ? gone_ms - t0 : -1;
        usleep(400000);
        close(to_child[1]);
        close(from_child[0]);

        jack_nframes_t f1 = jack_frame_time(sink.c);
        unsigned cyc = (f1 - f0) / period, sr = atomic_load(&sink.runs), br = atomic_load(&bys.runs);
        /* cycles a client did not run: counted runs against the cycles that
           elapsed (a late run lands in the next cycle and reads that cycle's
           frame time, so frame-time gaps alone undercount) */
        unsigned x = atomic_load(&xruns), u = atomic_load(&unregs);
        unsigned s = cyc > sr ? cyc - sr : 0, b = cyc > br + 1 ? cyc - br - 1 : 0;
        printf("trial %2d: xruns %u  sink missed %3u (%6.1f ms)  bystander missed %u"
               "  reap %6.1f ms  gone %6.1f ms  unreg %u/%d  runs sink %u bys %u of %u\n",
               t, x, s, s * period_ms, b, reap, gone, u, 2 * ports, sr, br, cyc);
        tot_x += x; tot_s += s; tot_b += b;
        if (s > max_s) max_s = s;
        sum_reap += reap; sum_gone += gone;
        if (gone > max_gone) max_gone = gone;
        if ((int)u != 2 * ports) bad_unreg++;
        if (gone < 0) fail = 1;
    }
    if (strcmp(mode, "late") == 0) {
        printf("total: xruns %u  double runs %u  extra runs %u  sink short %u\n", tot_x, tot_dups, tot_extra, tot_short);
        printf("%s: late, %d trials\n", tot_extra ? "FAIL" : "PASS", trials);
        return tot_extra ? 1 : 0;
    }
    printf("total: xruns %u  sink missed %u (max %u = %.1f ms)  bystander missed %u"
           "  reap avg %.1f ms  gone avg %.1f max %.1f ms  unreg wrong in %u/%d\n",
           tot_x, tot_s, max_s, max_s * period_ms, tot_b, sum_reap / trials,
           sum_gone / trials, max_gone, bad_unreg, trials);
    if (lim_xruns >= 0 && (int)tot_x > lim_xruns) fail = 1;
    if (lim_sink >= 0 && (int)max_s > lim_sink) fail = 1;
    if (unreg_exact && bad_unreg) fail = 1;
    if (tot_b) fail = 1;
    printf("%s: %s, %d trials: %u xruns, sink starved up to %u cycles, bystander %u, "
           "unregistration count wrong in %u\n", fail ? "FAIL" : "PASS", mode, trials,
           tot_x, max_s, tot_b, bad_unreg);
    jack_client_close(src.c);
    jack_client_close(sink.c);
    jack_client_close(bys.c);
    return fail;
}
