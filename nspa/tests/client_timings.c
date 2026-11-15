/*
 * JackBus test: the server's per-client cycle statistics
 * (org.jackaudio.JackControl.GetClientTimings), driven by nspa/tests/jackdbus.sh.
 *
 *   client_timings <late> <periods> [reactivate]
 *
 * Three clients in one process:
 *   nspa_late:   <late> of its process callbacks, 50 cycles apart, take <periods>
 *                periods (busy, as an overloaded plugin would be);
 *   nspa_after:  its input connected from nspa_late's output, so when nspa_late is
 *                late it waits for it: it must not be counted late itself;
 *   nspa_steady: does nothing.
 * Prints "ready" once the late callbacks are done (and, with "reactivate",
 * nspa_late has been deactivated and activated again), then stays until its
 * standard input closes.
 */

#include <jack/jack.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static jack_port_t *late_out, *after_in;
static atomic_uint late_runs, late_done, after_runs;
static unsigned late_wanted;
static double late_us;

static void spin_us(double us)
{
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    do {
        clock_gettime(CLOCK_MONOTONIC, &b);
    } while ((b.tv_sec - a.tv_sec) * 1e6 + (b.tv_nsec - a.tv_nsec) / 1e3 < us);
}

static int late_process(jack_nframes_t n, void *arg)
{
    (void)arg;
    unsigned run = atomic_fetch_add(&late_runs, 1) + 1;
    if (run % 50 == 0 && atomic_load(&late_done) < late_wanted) {
        spin_us(late_us);
        atomic_fetch_add(&late_done, 1);
    }
    memset(jack_port_get_buffer(late_out, n), 0, n * sizeof(float));
    return 0;
}

static int after_process(jack_nframes_t n, void *arg)
{
    (void)arg;
    (void)jack_port_get_buffer(after_in, n);
    atomic_fetch_add(&after_runs, 1);
    return 0;
}

static int steady_process(jack_nframes_t n, void *arg)
{
    (void)n;
    (void)arg;
    return 0;
}

static jack_client_t *open_client(const char *name, JackProcessCallback cb)
{
    jack_client_t *c = jack_client_open(name, JackNoStartServer, NULL);
    if (!c) {
        fprintf(stderr, "cannot open %s\n", name);
        exit(1);
    }
    jack_set_process_callback(c, cb, NULL);
    return c;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: client_timings <late> <periods> [reactivate]\n");
        return 2;
    }
    late_wanted = (unsigned)atoi(argv[1]);
    int reactivate = (argc > 3 && strcmp(argv[3], "reactivate") == 0);

    jack_client_t *late = open_client("nspa_late", late_process);
    jack_client_t *after = open_client("nspa_after", after_process);
    jack_client_t *steady = open_client("nspa_steady", steady_process);
    late_us = atof(argv[2]) * jack_get_buffer_size(late) * 1e6 / jack_get_sample_rate(late);

    late_out = jack_port_register(late, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    after_in = jack_port_register(after, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    if (!late_out || !after_in
        || jack_activate(late) || jack_activate(after) || jack_activate(steady)
        || jack_connect(late, "nspa_late:out", "nspa_after:in")) {
        fprintf(stderr, "cannot set up the clients\n");
        return 1;
    }

    for (int i = 0; i < 1000 && atomic_load(&late_done) < late_wanted; i++)
        usleep(10000);
    if (atomic_load(&late_done) < late_wanted) {
        fprintf(stderr, "the late callbacks did not happen\n");
        return 1;
    }
    usleep(200000);
    if (reactivate) {
        if (jack_deactivate(late) || jack_activate(late)
            || jack_connect(late, "nspa_late:out", "nspa_after:in")) {
            fprintf(stderr, "cannot reactivate nspa_late\n");
            return 1;
        }
        usleep(200000);
    }
    printf("ready: nspa_late ran %u times (%u late), nspa_after %u\n",
           atomic_load(&late_runs), atomic_load(&late_done), atomic_load(&after_runs));
    fflush(stdout);

    char b;
    while (read(0, &b, 1) > 0)
        ;
    jack_client_close(steady);
    jack_client_close(after);
    jack_client_close(late);
    return 0;
}
