/* jack2-nspa hardware test helper.
 *   hwutil wait [secs=10]   wait until a server accepts clients (exit 0) or time out (exit 1)
 *   hwutil midi-ports       print the number of MIDI ports the server has
 *   hwutil xruns [secs]     count xrun callbacks and the peak DSP load for secs
 *                           seconds, or until SIGTERM/SIGINT if secs is 0 or omitted
 */
#include <jack/jack.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static atomic_int xruns;
static volatile sig_atomic_t stop;

static int on_xrun(void *arg) { (void)arg; atomic_fetch_add(&xruns, 1); return 0; }
static int on_process(jack_nframes_t n, void *arg) { (void)n; (void)arg; return 0; }
static void on_signal(int s) { (void)s; stop = 1; }
static void on_shutdown(void *arg) { (void)arg; stop = 1; }

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "wait")) {
        int secs = argc > 2 ? atoi(argv[2]) : 10;
        for (int i = 0; i < secs * 10; i++) {
            jack_client_t *c = jack_client_open("hwwait", JackNoStartServer, NULL);
            if (c) { jack_client_close(c); return 0; }
            usleep(100000);
        }
        return 1;
    }
    jack_client_t *c = jack_client_open("hwutil", JackNoStartServer, NULL);
    if (!c) { fprintf(stderr, "hwutil: no server\n"); return 2; }
    if (!strcmp(mode, "midi-ports")) {
        const char **p = jack_get_ports(c, NULL, JACK_DEFAULT_MIDI_TYPE, 0);
        int n = 0;
        while (p && p[n]) n++;
        jack_free(p);
        printf("%d\n", n);
    } else if (!strcmp(mode, "xruns")) {
        int secs = argc > 2 ? atoi(argv[2]) : 0;
        signal(SIGTERM, on_signal);
        signal(SIGINT, on_signal);
        jack_on_shutdown(c, on_shutdown, NULL);
        jack_set_xrun_callback(c, on_xrun, NULL);
        jack_set_process_callback(c, on_process, NULL);
        jack_activate(c);
        float peak = 0;
        struct timespec t0, t;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        while (!stop) {
            usleep(100000);
            float l = jack_cpu_load(c);
            if (l > peak) peak = l;
            clock_gettime(CLOCK_MONOTONIC, &t);
            if (secs > 0 && t.tv_sec - t0.tv_sec >= secs) break;
        }
        clock_gettime(CLOCK_MONOTONIC, &t);
        printf("xruns=%d peak_dsp_load=%.1f%% seconds=%ld period=%u\n", atomic_load(&xruns), peak,
               (long)(t.tv_sec - t0.tv_sec), jack_get_buffer_size(c));
        jack_deactivate(c);
    } else {
        fprintf(stderr, "usage: hwutil wait [secs] | midi-ports | xruns [secs]\n");
        jack_client_close(c);
        return 2;
    }
    jack_client_close(c);
    return 0;
}
