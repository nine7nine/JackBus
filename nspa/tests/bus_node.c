/*
 bus_node: the native JackBus API (jackbus_node_*) under test.

   bus_node full          cycle record, buffer table, events, ports added and
                          removed while running, process latency
   bus_node rtwrite       writes into an unconnected audio input (must crash:
                          the shared zero buffer is read-only)
   bus_node steady <s>    a node at work for <s> seconds (for the system-call
                          trace in bus.sh)
   bus_node shutdown      runs until the server goes away: prints "ready", then
                          whether a SHUTDOWN event came and the close returned

 Prints PASS/FAIL lines; exit 0 if all passed.
*/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/syscall.h>

#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/metadata.h>
#include <jack/jackbus.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) { printf("PASS "); } else { printf("FAIL "); failures++; } printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static inline float sig(jack_nframes_t t) { return (float)((t % 4801u) / 4801.0 - 0.5); }

/* ---------------------------------------------------------------- node */

static jackbus_node_t* node;
static jackbus_port_t *ain, *aout, *afree1, *afree2, *min, *mout;
static atomic_int rt_tid;
static atomic_ulong cycles, frame_errors, time_errors, period_errors, field_errors, zero_errors, midi_in, first_flags;
static atomic_ulong copied, extra_ports_seen;
static uint64_t prev_frame, prev_time, prev_cycle;
static uint32_t prev_nframes;
static atomic_int rtwrite;
static atomic_int no_timing;     // around freewheel: its fast cycles have no meaningful times
static int rt_syscall;           // BUS_NODE_RT_SYSCALL=1: one system call in cycle 10 (the trace's negative control)

static int process(jackbus_node_t* n, const jackbus_cycle_t* c, void* arg)
{
    (void)arg;
    atomic_store_explicit(&rt_tid, 1, memory_order_relaxed);   // running (no gettid: the trace must see none)
    if (rt_syscall && c->cycle == 10) syscall(SYS_getppid);
    atomic_fetch_add(&cycles, 1);

    int timing = !atomic_load(&no_timing) && !(c->flags & JACKBUS_CYCLE_FREEWHEEL);
    if (c->size != sizeof(jackbus_cycle_t) || c->sample_rate != 48000 || c->time_ns == 0 || c->raw_ns == 0
        || (timing && (c->rate_diff < 0.8 || c->rate_diff > 1.25 || c->next_ns <= c->time_ns))) {
        atomic_fetch_add(&field_errors, 1);
    }
    if (c->flags & JACKBUS_CYCLE_FIRST) {
        atomic_fetch_add(&first_flags, 1);
    } else if (prev_cycle) {
        if (c->cycle != prev_cycle + 1) atomic_fetch_add(&field_errors, 1);
        // JACK's frame time: one period per cycle, except at an xrun and at a
        // buffer-size change (the server advances by the new size then)
        if (!(c->flags & JACKBUS_CYCLE_AFTER_XRUN) && c->nframes == prev_nframes && c->frame != prev_frame + prev_nframes) {
            atomic_fetch_add(&frame_errors, 1);
        }
        if (timing && c->time_ns <= prev_time) atomic_fetch_add(&time_errors, 1);
    }
    double expect = c->nframes * 1e9 / c->sample_rate;
    if (timing && (c->period_ns < expect * 0.8 || c->period_ns > expect * 1.25)) {
        atomic_fetch_add(&period_errors, 1);
    }
    prev_frame = c->frame; prev_time = c->time_ns; prev_cycle = c->cycle; prev_nframes = c->nframes;

    // the table: zero buffers, a copy, MIDI
    const float* z1 = jackbus_port_audio(afree1);
    const float* z2 = jackbus_port_audio(afree2);
    if (z1 != z2) atomic_fetch_add(&zero_errors, 1);
    for (uint32_t i = 0; z1 && i < c->nframes; i++) if (z1[i] != 0.0f) { atomic_fetch_add(&zero_errors, 1); break; }
    if (atomic_load(&rtwrite)) ((float*)z1)[0] = 1.0f;      // must fault

    const float* in = jackbus_port_audio(ain);
    float* out = jackbus_port_audio(aout);
    if (in && out) {
        memcpy(out, in, c->nframes * sizeof(float));
        atomic_fetch_add(&copied, 1);
    }
    void* mo = jackbus_port_midi(mout);
    void* mi = jackbus_port_midi(min);
    if (mo) {
        jack_midi_clear_buffer(mo);
        jack_midi_data_t note[3] = { 0x90, 60, 100 };
        jack_midi_event_write(mo, 0, note, 3);
    }
    if (mi) atomic_fetch_add(&midi_in, jack_midi_get_event_count(mi));
    (void)n;
    return 0;
}

/* a protocol-9 source of sig() and a sink checking it, in this process */
static jack_client_t *src, *sink;
static jack_port_t *src_out, *sink_in;
static atomic_ulong compared, mismatched;
static int src_process(jack_nframes_t n, void* a) {
    (void)a;
    float* o = jack_port_get_buffer(src_out, n);
    jack_nframes_t t0 = jack_last_frame_time(src);
    for (jack_nframes_t i = 0; i < n; i++) o[i] = sig(t0 + i);
    return 0;
}
static int sink_process(jack_nframes_t n, void* a) {
    (void)a;
    const float* in = jack_port_get_buffer(sink_in, n);
    jack_nframes_t t0 = jack_last_frame_time(sink);
    int zero = 1, bad = 0;
    for (jack_nframes_t i = 0; i < n; i++) { if (in[i] != 0.0f) zero = 0; if (in[i] != sig(t0 + i)) bad = 1; }
    if (atomic_load(&compared) == 0 && zero) return 0;
    atomic_fetch_add(&compared, 1);
    if (bad) atomic_fetch_add(&mismatched, 1);
    return 0;
}

/* ---------------------------------------------------------------- events */

#define MAXEV 8192
static jackbus_event_t evs[MAXEV];
static int nev;

static void drain(int ms)
{
    struct pollfd p = { jackbus_node_event_fd(node), POLLIN, 0 };
    for (;;) {
        jackbus_event_t e;
        while (jackbus_node_next_event(node, &e) == 1) {
            if (nev < MAXEV) evs[nev++] = e;
        }
        if (poll(&p, 1, ms) <= 0) break;
    }
}

static int find(uint32_t type, int on, const char* name, const char* name2)
{
    for (int i = 0; i < nev; i++) {
        if (evs[i].type == type && (on < 0 || (int)evs[i].on == on)
            && (!name || strcmp(evs[i].name, name) == 0) && (!name2 || strcmp(evs[i].name2, name2) == 0)) {
            return i;
        }
    }
    return -1;
}

/* ports added and removed while the node runs */
static atomic_int churn_stop;
static void* churn(void* arg)
{
    (void)arg;
    int n = 0;
    while (!atomic_load(&churn_stop) && n < 300) {
        char name[32];
        snprintf(name, sizeof(name), "churn%d", n % 7);
        jackbus_port_t* p = jackbus_port_add(node, name, (n & 1) ? JACKBUS_MIDI : JACKBUS_AUDIO, JackPortIsInput);
        if (p) jackbus_port_remove(p);
        n++;
    }
    atomic_store(&extra_ports_seen, n);
    return NULL;
}

static jackbus_node_t* open_node(const char* name)
{
    jack_status_t st = 0;
    jackbus_node_t* n = jackbus_node_open(name, JackNoStartServer, &st);
    if (!n) fprintf(stderr, "cannot open node %s (0x%x)\n", name, st);
    return n;
}

static int full(void)
{
    if (!(node = open_node("nodetest"))) return 1;
    jack_client_t* c = jackbus_node_client(node);
    ain = jackbus_port_add(node, "in", JACKBUS_AUDIO, JackPortIsInput);
    aout = jackbus_port_add(node, "out", JACKBUS_AUDIO, JackPortIsOutput);
    afree1 = jackbus_port_add(node, "free1", JACKBUS_AUDIO, JackPortIsInput);
    afree2 = jackbus_port_add(node, "free2", JACKBUS_AUDIO, JackPortIsInput);
    min = jackbus_port_add(node, "midi_in", JACKBUS_MIDI, JackPortIsInput);
    mout = jackbus_port_add(node, "midi_out", JACKBUS_MIDI, JackPortIsOutput);
    CHECK(ain && aout && afree1 && afree2 && min && mout, "6 ports added before activation");
    CHECK(jackbus_node_set_process(node, process, NULL) == 0, "process set");
    CHECK(jackbus_node_activate(node) == 0, "activated");
    CHECK(jackbus_node_set_process(node, process, NULL) < 0, "set_process refused while active");

    // protocol-9 peers in this process: src -> node:in, node:out -> sink; node MIDI loop
    src = jack_client_open("evpeer", JackNoStartServer, NULL);
    sink = jack_client_open("sinkpeer", JackNoStartServer, NULL);
    src_out = jack_port_register(src, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    sink_in = jack_port_register(sink, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    jack_set_process_callback(src, src_process, NULL);
    jack_set_process_callback(sink, sink_process, NULL);
    jack_activate(src);
    jack_activate(sink);
    jack_connect(c, "evpeer:out", "nodetest:in");
    jack_connect(c, "nodetest:out", "sinkpeer:in");
    jack_connect(c, "nodetest:midi_out", "nodetest:midi_in");
    sleep(2);
    drain(100);

    CHECK(atomic_load(&cycles) > 300, "the process ran: %lu cycles", atomic_load(&cycles));
    CHECK(atomic_load(&first_flags) == 1, "JACKBUS_CYCLE_FIRST once (%lu)", atomic_load(&first_flags));
    CHECK(atomic_load(&field_errors) == 0, "cycle fields consistent (%lu errors)", atomic_load(&field_errors));
    CHECK(atomic_load(&frame_errors) == 0, "frame advances by nframes each cycle (%lu errors)", atomic_load(&frame_errors));
    CHECK(atomic_load(&time_errors) == 0, "time_ns increases (%lu errors)", atomic_load(&time_errors));
    CHECK(atomic_load(&period_errors) == 0, "period_ns matches the period (%lu errors)", atomic_load(&period_errors));
    CHECK(atomic_load(&zero_errors) == 0, "unconnected inputs: one shared zero buffer, all zeros (%lu errors)", atomic_load(&zero_errors));
    CHECK(atomic_load(&compared) > 300 && atomic_load(&mismatched) == 0,
          "audio through the node, sample-exact: %lu cycles, %lu mismatched", atomic_load(&compared), atomic_load(&mismatched));
    CHECK(atomic_load(&midi_in) > 300, "MIDI out -> in through the node: %lu events", atomic_load(&midi_in));

    // events of what happened, from this thread (not a library thread)
    CHECK(find(JACKBUS_EVENT_CLIENT, 1, "evpeer", NULL) >= 0, "event: client evpeer registered");
    CHECK(find(JACKBUS_EVENT_PORT, 1, "evpeer:out", NULL) >= 0, "event: port evpeer:out registered");
    CHECK(find(JACKBUS_EVENT_CONNECT, 1, "evpeer:out", "nodetest:in") >= 0, "event: evpeer:out -> nodetest:in");
    CHECK(find(JACKBUS_EVENT_CLIENT, 1, "evpeer", NULL) < find(JACKBUS_EVENT_CONNECT, 1, "evpeer:out", "nodetest:in"),
          "events in order: the client before its connection");
    CHECK(find(JACKBUS_EVENT_GRAPH, -1, NULL, NULL) >= 0, "event: graph order");

    nev = 0;
    jack_port_rename(c, jackbus_port_jack(afree2), "spare");     // the short name
    jack_nframes_t size0 = jack_get_buffer_size(c);
    jack_nframes_t size1 = (size0 == 128) ? 256 : 128;     // a change, whatever the server runs at
    jack_set_buffer_size(c, size1);
    sleep(1);
    atomic_store(&no_timing, 1);
    jack_set_freewheel(c, 1);
    usleep(300000);
    jack_set_freewheel(c, 0);
    usleep(1500000);        // the DLL settles again
    atomic_store(&no_timing, 0);
    jack_set_property(c, jack_port_uuid(jackbus_port_jack(ain)), "http://jackaudio.org/metadata/pretty-name", "Input", NULL);
    // a short-lived client: its arrival and departure, with a connection
    jack_client_t* gone = jack_client_open("gonepeer", JackNoStartServer, NULL);
    if (gone) {
        jack_port_register(gone, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        jack_activate(gone);
        jack_connect(c, "gonepeer:out", "nodetest:spare");
        usleep(300000);
        jack_client_close(gone);
    }
    sleep(1);
    drain(200);
    CHECK(find(JACKBUS_EVENT_RENAME, -1, "nodetest:spare", "nodetest:free2") >= 0, "event: rename free2 -> spare");
    int bs = find(JACKBUS_EVENT_BUFFER_SIZE, -1, NULL, NULL);
    CHECK(bs >= 0 && evs[bs].value == size1, "event: buffer size %u", size1);
    CHECK(find(JACKBUS_EVENT_LATENCY, -1, NULL, NULL) >= 0, "event: latency");
    CHECK(find(JACKBUS_EVENT_FREEWHEEL, 1, NULL, NULL) >= 0 && find(JACKBUS_EVENT_FREEWHEEL, 0, NULL, NULL) >= 0, "events: freewheel on and off");
    int pr = find(JACKBUS_EVENT_PROPERTY, -1, "http://jackaudio.org/metadata/pretty-name", NULL);
    CHECK(pr >= 0 && evs[pr].id == jack_port_uuid(jackbus_port_jack(ain)), "event: property, its subject and key");
    CHECK(find(JACKBUS_EVENT_CLIENT, 1, "gonepeer", NULL) >= 0 && find(JACKBUS_EVENT_CLIENT, 0, "gonepeer", NULL) >= 0,
          "events: client gonepeer registered, then gone");
    CHECK(find(JACKBUS_EVENT_CONNECT, 1, "gonepeer:out", "nodetest:spare") >= 0
          && find(JACKBUS_EVENT_CONNECT, 0, "gonepeer:out", "nodetest:spare") >= 0,
          "events: its connection made, then undone when it left");
    CHECK(atomic_load(&frame_errors) == 0 && atomic_load(&field_errors) == 0,
          "cycle record consistent through buffer size, freewheel (%lu %lu)", atomic_load(&frame_errors), atomic_load(&field_errors));

    // process latency: the node's ranges carry it (without the node's own MIDI
    // loop, which would feed its output latency back into its input)
    jack_disconnect(c, "nodetest:midi_out", "nodetest:midi_in");
    jack_connect(c, "system:capture_1", "nodetest:spare");
    jack_connect(c, "nodetest:out", "system:playback_1");
    jackbus_node_set_process_latency(node, 64);
    usleep(500000);
    jack_latency_range_t cap_src, cap_out, play_dst, play_in;
    jack_port_get_latency_range(jack_port_by_name(c, "system:capture_1"), JackCaptureLatency, &cap_src);
    jack_port_get_latency_range(jackbus_port_jack(aout), JackCaptureLatency, &cap_out);
    jack_port_get_latency_range(jack_port_by_name(c, "system:playback_1"), JackPlaybackLatency, &play_dst);
    jack_port_get_latency_range(jackbus_port_jack(ain), JackPlaybackLatency, &play_in);
    CHECK(cap_out.max == cap_src.max + 64 && play_in.max == play_dst.max + 64,
          "process latency 64: capture %u -> %u at node:out, playback %u -> %u at node:in",
          cap_src.max, cap_out.max, play_dst.max, play_in.max);
    jack_disconnect(c, "system:capture_1", "nodetest:spare");    // an unconnected input again
    jack_disconnect(c, "nodetest:out", "system:playback_1");
    usleep(200000);
    unsigned long mm0 = atomic_load(&mismatched), ze0 = atomic_load(&zero_errors);

    // queue overflow: many events not drained, then a LOST event (on a port of
    // its own: the checked chain stays as it is)
    jackbus_port_t* ovf = jackbus_port_add(node, "ovf", JACKBUS_AUDIO, JackPortIsInput);
    nev = 0;
    for (int i = 0; ovf && i < 700; i++) {
        jack_connect(c, "system:capture_2", "nodetest:ovf");
        jack_disconnect(c, "system:capture_2", "nodetest:ovf");
    }
    sleep(1);
    drain(200);
    int lost = find(JACKBUS_EVENT_LOST, -1, NULL, NULL);
    CHECK(lost >= 0 && evs[lost].value > 0, "queue full: a LOST event (%lu lost, %d delivered before it)",
          lost >= 0 ? (unsigned long)evs[lost].value : 0UL, lost);
    if (ovf) jackbus_port_remove(ovf);

    // ports added and removed while the node runs: no cycle lost, nothing torn
    unsigned long before = atomic_load(&cycles), fe = atomic_load(&frame_errors);
    pthread_t t;
    pthread_create(&t, NULL, churn, NULL);
    pthread_join(t, NULL);
    usleep(200000);
    CHECK(atomic_load(&extra_ports_seen) == 300 && atomic_load(&cycles) > before && atomic_load(&frame_errors) == fe
          && atomic_load(&zero_errors) == ze0,
          "300 ports added and removed while running: %lu cycles meanwhile (ports %lu, frame errors %lu -> %lu, zero errors %lu)",
          atomic_load(&cycles) - before, atomic_load(&extra_ports_seen), fe, atomic_load(&frame_errors), atomic_load(&zero_errors) - ze0);
    CHECK(atomic_load(&mismatched) == mm0 && atomic_load(&zero_errors) == ze0,
          "audio sample-exact through the overflow and the port churn (%lu mismatched cycles, %lu earlier, at the buffer-size change and freewheel)",
          atomic_load(&mismatched) - mm0, mm0);

    jack_set_buffer_size(c, size0);
    jackbus_node_deactivate(node);
    CHECK(jackbus_node_close(node) == 0, "closed");
    jack_client_close(sink);
    jack_client_close(src);
    printf("%s (%d failed)\n", failures ? "SOME FAILED" : "ALL PASS", failures);
    return failures ? 1 : 0;
}

static int steady(int seconds)
{
    if (!(node = open_node("steady"))) return 1;
    jack_client_t* c = jackbus_node_client(node);
    ain = jackbus_port_add(node, "in", JACKBUS_AUDIO, JackPortIsInput);
    aout = jackbus_port_add(node, "out", JACKBUS_AUDIO, JackPortIsOutput);
    afree1 = jackbus_port_add(node, "free1", JACKBUS_AUDIO, JackPortIsInput);
    afree2 = jackbus_port_add(node, "free2", JACKBUS_AUDIO, JackPortIsInput);
    min = jackbus_port_add(node, "midi_in", JACKBUS_MIDI, JackPortIsInput);
    mout = jackbus_port_add(node, "midi_out", JACKBUS_MIDI, JackPortIsOutput);
    jackbus_node_set_process(node, process, NULL);
    jackbus_node_activate(node);
    jack_connect(c, "system:capture_1", "steady:in");
    jack_connect(c, "steady:out", "system:playback_1");
    jack_connect(c, "steady:midi_out", "steady:midi_in");
    for (int i = 0; i < 50 && atomic_load(&rt_tid) == 0; i++) usleep(20000);
    sleep(seconds);
    printf("steady: %lu cycles\n", atomic_load(&cycles));
    jackbus_node_close(node);
    return 0;
}

static int shutdown_case(void)
{
    if (!(node = open_node("goner"))) return 1;
    ain = jackbus_port_add(node, "in", JACKBUS_AUDIO, JackPortIsInput);
    aout = jackbus_port_add(node, "out", JACKBUS_AUDIO, JackPortIsOutput);
    jackbus_node_set_process(node, process, NULL);
    jackbus_node_activate(node);
    printf("ready\n");
    fflush(stdout);
    // the server is killed now: the event must come, within 10 s
    struct pollfd p = { jackbus_node_event_fd(node), POLLIN, 0 };
    int got = 0;
    for (int i = 0; i < 100 && !got; i++) {
        jackbus_event_t e;
        while (jackbus_node_next_event(node, &e) == 1) {
            if (e.type == JACKBUS_EVENT_SHUTDOWN) {
                got = 1;
                printf("shutdown event: %s\n", e.name);
            }
        }
        if (!got) poll(&p, 1, 100);
    }
    unsigned long before = atomic_load(&cycles);
    usleep(200000);
    int stopped = atomic_load(&cycles) == before;
    int res = jackbus_node_close(node);
    printf("%s: SHUTDOWN event %s, process stopped %s, close returned %d\n",
           got && stopped ? "PASS" : "FAIL", got ? "yes" : "no", stopped ? "yes" : "no", res);
    return got && stopped ? 0 : 1;
}

static int write_zero(void)
{
    if (!(node = open_node("rtwrite"))) return 1;
    afree1 = jackbus_port_add(node, "free1", JACKBUS_AUDIO, JackPortIsInput);
    afree2 = afree1;
    jackbus_node_set_process(node, process, NULL);
    atomic_store(&rtwrite, 1);
    jackbus_node_activate(node);
    sleep(3);
    printf("rtwrite: still alive (the zero buffer was writable)\n");
    jackbus_node_close(node);
    return 1;
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: bus_node full | rtwrite | steady <seconds> | shutdown\n");
        return 2;
    }
    rt_syscall = getenv("BUS_NODE_RT_SYSCALL") != NULL;
    if (strcmp(argv[1], "full") == 0) return full();
    if (strcmp(argv[1], "rtwrite") == 0) return write_zero();
    if (strcmp(argv[1], "shutdown") == 0) return shutdown_case();
    if (strcmp(argv[1], "steady") == 0) return steady(argc > 2 ? atoi(argv[2]) : 3);
    return 2;
}
