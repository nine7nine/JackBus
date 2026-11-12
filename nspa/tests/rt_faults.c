/* jack2-nspa: page faults taken by a client's process thread inside its
 * process callback when the client cannot mlock() the shared segments (run
 * it with a small RLIMIT_MEMLOCK, e.g. `ulimit -l 64`). libjack prefaults the
 * segments and the start of the RT stack in that case, so the callback should
 * take no faults even in its first cycles.
 *
 * Usage: rt_faults [cycles=300]. Exit 0 if no faults inside callbacks.
 */
#define _GNU_SOURCE
#include <jack/jack.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#define NPORTS 8
static jack_port_t *in[NPORTS], *out[NPORTS];
static atomic_long faults, cycles;

static int process(jack_nframes_t n, void *arg)
{
    (void)arg;
    struct rusage a, b;
    getrusage(RUSAGE_THREAD, &a);
    char scratch[8192];  /* some stack, like a real callback */
    memset(scratch, 0, sizeof scratch);
    for (int i = 0; i < NPORTS; i++) {
        float *s = jack_port_get_buffer(in[i], n), *d = jack_port_get_buffer(out[i], n);
        memcpy(d, s, n * sizeof(float));
        d[0] += scratch[i];
    }
    getrusage(RUSAGE_THREAD, &b);
    atomic_fetch_add(&faults, (b.ru_minflt - a.ru_minflt) + (b.ru_majflt - a.ru_majflt));
    atomic_fetch_add(&cycles, 1);
    return 0;
}

int main(int argc, char **argv)
{
    long want = argc > 1 ? atol(argv[1]) : 300;
    jack_client_t *c = jack_client_open("rtfaults", JackNoStartServer, NULL);
    if (!c) return 2;
    char name[16];
    for (int i = 0; i < NPORTS; i++) {
        snprintf(name, sizeof name, "in%d", i);
        in[i] = jack_port_register(c, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        snprintf(name, sizeof name, "out%d", i);
        out[i] = jack_port_register(c, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    }
    jack_set_process_callback(c, process, NULL);
    jack_activate(c);
    for (int i = 0; i < NPORTS; i++) {
        char a[32], b[32];
        snprintf(a, sizeof a, "rtfaults:out%d", i);
        snprintf(b, sizeof b, "rtfaults:in%d", (i + 1) % NPORTS);
        jack_connect(c, a, b);
    }
    while (atomic_load(&cycles) < want)
        usleep(10000);
    jack_client_close(c);
    long f = atomic_load(&faults);
    printf("%s: %ld page faults inside %ld process callbacks\n", f ? "FAIL" : "PASS", f, atomic_load(&cycles));
    return f ? 1 : 0;
}
