/*
 bus_client: a client for the JackBus socket tests (nspa/tests/bus.sh).

   bus_client [-b] [-s seconds] <role> <name> [port]

 -b opens with jackbus_client_open() (the JackBus socket), else jack_client_open()
 (protocol 9; with a stock libjack when run against /usr/lib). Roles:
   src  <name>          an output "out" carrying sig(frame time)
   thru <name> <src>    "in" (connected from <src>) copied to "out"
   sink <name> <src>    "in" connected from <src>, checked sample for sample
                        against sig(frame time) of the same cycle
   pair <name> <src>    one process, two clients: <name>-p9 (protocol 9) and
                        <name>-bus (-b implied): src -> p9:in, p9:out -> bus:in,
                        bus:out is "<name>-bus:out" (a chain inside one process)
   probe <name>         opens, reports what it mapped (/proc/self/maps), closes
 Prints one result line; exit 0 on success.
*/
#define _GNU_SOURCE
#include <stdio.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdatomic.h>

#include <jack/jack.h>
#include <jack/jackbus.h>

static inline float sig(jack_nframes_t t)
{
    return (float)((t % 4801u) / 4801.0 - 0.5);
}

struct node {
    jack_client_t* client;
    jack_port_t* in;
    jack_port_t* out;
    int role;           // 0 src, 1 thru, 2 sink
    atomic_ulong cycles, compared, mismatched, silent;
};

static volatile sig_atomic_t stop;
static void on_signal(int s) { (void)s; stop = 1; }

static int process(jack_nframes_t n, void* arg)
{
    struct node* nd = arg;
    jack_nframes_t t0 = jack_last_frame_time(nd->client);
    atomic_fetch_add(&nd->cycles, 1);
    if (nd->role == 0) {
        float* out = jack_port_get_buffer(nd->out, n);
        for (jack_nframes_t i = 0; i < n; i++) {
            out[i] = sig(t0 + i);
        }
    } else if (nd->role == 1) {
        const float* in = jack_port_get_buffer(nd->in, n);
        float* out = jack_port_get_buffer(nd->out, n);
        memcpy(out, in, n * sizeof(float));
    } else {
        const float* in = jack_port_get_buffer(nd->in, n);
        int zero = 1, bad = 0;
        for (jack_nframes_t i = 0; i < n; i++) {
            if (in[i] != 0.0f) zero = 0;
            if (in[i] != sig(t0 + i)) bad = 1;
        }
        if (atomic_load(&nd->compared) == 0 && zero) {
            atomic_fetch_add(&nd->silent, 1);
            return 0;
        }
        atomic_fetch_add(&nd->compared, 1);
        if (bad) atomic_fetch_add(&nd->mismatched, 1);
    }
    return 0;
}

/* looked up at run time, as an application that uses it when present would:
   the role runs with any libjack, a stock one too */
typedef jack_client_t* (*bus_open_t)(const char*, jack_options_t, jack_status_t*, unsigned int, ...);

static jack_client_t* open_client(const char* name, int bus)
{
    jack_status_t status = 0;
    jack_client_t* c;
    if (bus) {
        bus_open_t bus_open = (bus_open_t)dlsym(RTLD_DEFAULT, "jackbus_client_open");
        if (!bus_open) {
            fprintf(stderr, "%s: this libjack has no jackbus_client_open\n", name);
            return NULL;
        }
        c = bus_open(name, JackNoStartServer, &status, 0);
    } else {
        c = jack_client_open(name, JackNoStartServer, &status);
    }
    if (!c) {
        fprintf(stderr, "%s: cannot open (%s), status 0x%x\n", name, bus ? "JackBus socket" : "protocol 9", status);
    }
    return c;
}

static int setup(struct node* nd, const char* name, int bus, int role, const char* src)
{
    nd->role = role;
    if (!(nd->client = open_client(name, bus))) return -1;
    if (role != 0) nd->in = jack_port_register(nd->client, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    if (role != 2) nd->out = jack_port_register(nd->client, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    jack_set_process_callback(nd->client, process, nd);
    if (jack_activate(nd->client) != 0) {
        fprintf(stderr, "%s: cannot activate\n", name);
        return -1;
    }
    if (src) {
        int i;
        for (i = 0; i < 100 && !jack_port_by_name(nd->client, src); i++) usleep(50000);
        if (jack_connect(nd->client, src, jack_port_name(nd->in)) != 0) {
            fprintf(stderr, "%s: cannot connect %s\n", name, src);
            return -1;
        }
    }
    return 0;
}

static void maps_report(const char* name)
{
    FILE* f = fopen("/proc/self/maps", "r");
    char line[512];
    int registry = 0, segments = 0, futexes = 0, alive = 0;
    while (f && fgets(line, sizeof(line), f)) {
        if (strstr(line, "jack-shm-registry")) registry++;
        else if (strstr(line, "/jack-")) segments++;
        else if (strstr(line, "jack_sem.")) futexes++;
        else if (strstr(line, "jack_alive.")) alive++;
    }
    if (f) fclose(f);
    printf("%s: maps: registry %d, segments %d, futex files %d, alive table %d\n", name, registry, segments, futexes, alive);
}

int main(int argc, char* argv[])
{
    int bus = 0, seconds = 2, opt;
    while ((opt = getopt(argc, argv, "bs:")) != -1) {
        if (opt == 'b') bus = 1;
        else if (opt == 's') seconds = atoi(optarg);
        else return 2;
    }
    if (argc - optind < 2) {
        fprintf(stderr, "usage: bus_client [-b] [-s seconds] <src|thru|sink|pair|probe> <name> [src-port]\n");
        return 2;
    }
    const char* role = argv[optind];
    const char* name = argv[optind + 1];
    const char* src = (argc - optind > 2) ? argv[optind + 2] : NULL;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (strcmp(role, "probe") == 0) {
        jack_client_t* c = open_client(name, bus);
        if (!c) return 1;
        maps_report(name);
        printf("%s: opened as %s, rate %u, period %u\n", name, jack_get_client_name(c),
               jack_get_sample_rate(c), jack_get_buffer_size(c));
        jack_client_close(c);
        return 0;
    }

    if (strcmp(role, "pair") == 0) {
        static struct node a, b;
        char n1[64], n2[64], a_out[80];
        snprintf(n1, sizeof(n1), "%s-p9", name);
        snprintf(n2, sizeof(n2), "%s-bus", name);
        if (setup(&a, n1, 0, 1, src) < 0) return 1;
        snprintf(a_out, sizeof(a_out), "%s:out", jack_get_client_name(a.client));
        if (setup(&b, n2, 1, 1, a_out) < 0) return 1;
        for (int i = 0; i < seconds * 10 && !stop; i++) usleep(100000);
        printf("%s: pair: %lu and %lu cycles\n", name, atomic_load(&a.cycles), atomic_load(&b.cycles));
        jack_deactivate(b.client);
        jack_deactivate(a.client);
        jack_client_close(b.client);
        jack_client_close(a.client);
        return (atomic_load(&a.cycles) > 0 && atomic_load(&b.cycles) > 0) ? 0 : 1;
    }

    static struct node nd;
    int r = strcmp(role, "src") == 0 ? 0 : strcmp(role, "thru") == 0 ? 1 : strcmp(role, "sink") == 0 ? 2 : -1;
    if (r < 0 || (r > 0 && !src)) {
        fprintf(stderr, "bad role or missing source port\n");
        return 2;
    }
    if (setup(&nd, name, bus, r, src) < 0) return 1;
    for (int i = 0; i < seconds * 10 && !stop; i++) usleep(100000);
    jack_deactivate(nd.client);
    int ok = atomic_load(&nd.cycles) > 0;
    if (r == 2) {
        printf("%s: %lu cycles compared, %lu mismatched (%lu silent before the first signal)\n", name,
               atomic_load(&nd.compared), atomic_load(&nd.mismatched), atomic_load(&nd.silent));
        ok = ok && atomic_load(&nd.compared) > 0 && atomic_load(&nd.mismatched) == 0;
    } else {
        printf("%s: %lu cycles\n", name, atomic_load(&nd.cycles));
    }
    jack_client_close(nd.client);
    return ok ? 0 : 1;
}
