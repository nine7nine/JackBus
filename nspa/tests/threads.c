/* jack2-nspa: libjack threading behaviour.
 *
 *  api-concurrency  two application threads use one client's API at once
 *                   (port register/unregister vs connect/disconnect/get_ports):
 *                   every call must succeed with a correct result.
 *  activate-init    a client whose buffer-size callback takes 60 ms is
 *                   activated next to a running client: no xrun may result.
 *  port-churn       ports registered/unregistered while another client's
 *                   connections trigger latency recomputation (notification
 *                   thread iterating the port list): must not crash.
 *  sync-callback    jack_set_sync_callback() on an active client: invoked
 *                   within a few cycles (transport.h contract).
 *  transport-churn  timebase/sync callbacks set and released while the
 *                   transport rolls: callbacks must always get their own arg.
 *  rt-helper-thread an RT thread from jack_client_create_thread() calls
 *                   jack_port_get_latency_range() during graph changes: no
 *                   call may sleep (longest call well below a period).
 *
 * Usage: threads [subtest...]   (default: all). Exit 0 = pass.
 */
#include <jack/jack.h>
#include <jack/transport.h>
#include <jack/thread.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static int process_copy(jack_nframes_t n, void *arg) { (void)n; (void)arg; return 0; }

static jack_client_t *open_client(const char *name)
{
    jack_status_t st;
    jack_client_t *c = jack_client_open(name, JackNoStartServer, &st);
    if (!c)
        fprintf(stderr, "open %s failed 0x%x\n", name, st);
    return c;
}

/* ---------------- api-concurrency */
static atomic_int api_errors, api_stop;
static jack_client_t *api_client;

static void *api_register_thread(void *arg)
{
    (void)arg;
    char name[32];
    for (int i = 0; i < 400 && !atomic_load(&api_stop); i++) {
        snprintf(name, sizeof name, "tmp%d", i % 7);
        jack_port_t *p = jack_port_register(api_client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (!p) { atomic_fetch_add(&api_errors, 1); continue; }
        if (strcmp(jack_port_short_name(p), name) != 0) atomic_fetch_add(&api_errors, 1);
        if (jack_port_unregister(api_client, p) != 0) atomic_fetch_add(&api_errors, 1);
    }
    return NULL;
}

static void *api_connect_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 400 && !atomic_load(&api_stop); i++) {
        if (jack_connect(api_client, "api:out", "api:in") != 0) atomic_fetch_add(&api_errors, 1);
        const char **ports = jack_get_ports(api_client, "^api:", NULL, 0);
        if (!ports) atomic_fetch_add(&api_errors, 1); else jack_free(ports);
        if (jack_disconnect(api_client, "api:out", "api:in") != 0) atomic_fetch_add(&api_errors, 1);
    }
    return NULL;
}

static int test_api_concurrency(void)
{
    pthread_t a, b;
    api_client = open_client("api");
    if (!api_client) return 1;
    jack_port_register(api_client, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    jack_port_register(api_client, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    jack_set_process_callback(api_client, process_copy, NULL);
    jack_activate(api_client);
    pthread_create(&a, NULL, api_register_thread, NULL);
    pthread_create(&b, NULL, api_connect_thread, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    jack_client_close(api_client);
    printf("%s api-concurrency: %d errors in 2x400 iterations\n", atomic_load(&api_errors) ? "FAIL" : "PASS", atomic_load(&api_errors));
    return atomic_load(&api_errors) != 0;
}

/* ---------------- activate-init */
static atomic_int xruns_seen;
static int on_xrun(void *arg) { (void)arg; atomic_fetch_add(&xruns_seen, 1); return 0; }
static int slow_bufsize(jack_nframes_t n, void *arg) { (void)n; (void)arg; usleep(60000); return 0; }

static int test_activate_init(void)
{
    jack_client_t *a = open_client("act_a"), *b = open_client("act_b");
    if (!a || !b) return 1;
    jack_set_process_callback(a, process_copy, NULL);
    jack_set_xrun_callback(a, on_xrun, NULL);
    jack_activate(a);
    usleep(200000);
    atomic_store(&xruns_seen, 0);
    jack_set_process_callback(b, process_copy, NULL);
    jack_set_buffer_size_callback(b, slow_bufsize, NULL);
    jack_activate(b);
    usleep(300000);
    int x = atomic_load(&xruns_seen);
    jack_client_close(b);
    jack_client_close(a);
    printf("%s activate-init: %d xruns after activating a client with a 60 ms buffer-size callback\n", x ? "FAIL" : "PASS", x);
    return x != 0;
}

/* ---------------- port-churn */
static atomic_int churn_stop;
static jack_client_t *churn_client, *churn_other;

static void *churn_ports(void *arg)
{
    (void)arg;
    char name[32];
    for (int i = 0; !atomic_load(&churn_stop); i++) {
        snprintf(name, sizeof name, "p%d", i % 16);
        jack_port_t *p = jack_port_register(churn_client, name, JACK_DEFAULT_AUDIO_TYPE,
                                            (i & 1) ? JackPortIsInput : JackPortIsOutput, 0);
        if (p) jack_port_unregister(churn_client, p);
    }
    return NULL;
}

static int test_port_churn(void)
{
    pthread_t t;
    churn_client = open_client("churn");
    churn_other = open_client("churn_o");
    if (!churn_client || !churn_other) return 1;
    jack_port_register(churn_other, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    jack_port_register(churn_other, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    jack_set_process_callback(churn_client, process_copy, NULL);
    jack_set_process_callback(churn_other, process_copy, NULL);
    jack_activate(churn_client);
    jack_activate(churn_other);
    pthread_create(&t, NULL, churn_ports, NULL);
    for (int i = 0; i < 300; i++) {  /* each (dis)connect triggers latency callbacks in all clients */
        jack_connect(churn_other, "churn_o:out", "churn_o:in");
        jack_disconnect(churn_other, "churn_o:out", "churn_o:in");
    }
    atomic_store(&churn_stop, 1);
    pthread_join(t, NULL);
    jack_client_close(churn_other);
    jack_client_close(churn_client);
    printf("PASS port-churn: no crash with concurrent port churn and latency recomputation\n");
    return 0;
}

/* ---------------- sync-callback and transport-churn */
static atomic_int sync_calls, arg_mismatch, timebase_calls;
static int magic_sync = 0x5a5a, magic_timebase = 0xa5a5;

static int sync_cb(jack_transport_state_t s, jack_position_t *pos, void *arg)
{
    (void)s; (void)pos;
    atomic_fetch_add(&sync_calls, 1);
    if (arg != &magic_sync) atomic_fetch_add(&arg_mismatch, 1);
    return 1;
}

static void timebase_cb(jack_transport_state_t s, jack_nframes_t n, jack_position_t *pos, int new_pos, void *arg)
{
    (void)s; (void)n; (void)new_pos;
    atomic_fetch_add(&timebase_calls, 1);
    if (arg != &magic_timebase) atomic_fetch_add(&arg_mismatch, 1);
    pos->valid = JackPositionBBT;
    pos->bar = 1; pos->beat = 1; pos->tick = 0; pos->beats_per_bar = 4; pos->beat_type = 4;
    pos->ticks_per_beat = 1920; pos->beats_per_minute = 120;
}

static int test_sync_callback(void)
{
    jack_client_t *c = open_client("sync");
    if (!c) return 1;
    jack_set_process_callback(c, process_copy, NULL);
    jack_transport_stop(c);
    jack_activate(c);
    usleep(100000);
    atomic_store(&sync_calls, 0);
    jack_set_sync_callback(c, sync_cb, &magic_sync);
    usleep(100000);
    int calls = atomic_load(&sync_calls);
    jack_client_close(c);
    printf("%s sync-callback: invoked %d times within 100 ms of being set on an active client\n", calls ? "PASS" : "FAIL", calls);
    return calls == 0;
}

static int test_transport_churn(void)
{
    jack_client_t *c = open_client("tchurn");
    if (!c) return 1;
    jack_set_process_callback(c, process_copy, NULL);
    jack_activate(c);
    jack_transport_start(c);
    atomic_store(&arg_mismatch, 0);
    for (int i = 0; i < 2000; i++) {
        jack_set_timebase_callback(c, 0, timebase_cb, &magic_timebase);
        jack_set_sync_callback(c, sync_cb, &magic_sync);
        jack_release_timebase(c);
        jack_set_sync_callback(c, NULL, NULL);
    }
    jack_transport_stop(c);
    jack_client_close(c);
    int bad = atomic_load(&arg_mismatch);
    printf("%s transport-churn: 2000 set/release rounds, %d timebase calls, %d wrong-argument calls\n",
           bad ? "FAIL" : "PASS", atomic_load(&timebase_calls), bad);
    return bad != 0;
}


/* ---------------- rt-helper-thread */
static atomic_int helper_stop;
static jack_port_t *helper_port;
static double helper_worst_us;

static double now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static void *helper_thread(void *arg)
{
    (void)arg;
    jack_latency_range_t range;
    while (!atomic_load(&helper_stop)) {
        double a = now_us();
        jack_port_get_latency_range(helper_port, JackCaptureLatency, &range);
        double d = now_us() - a;
        if (d > helper_worst_us) helper_worst_us = d;
        usleep(100);
    }
    return NULL;
}

static int test_rt_helper_thread(void)
{
    jack_client_t *c = open_client("helper");
    if (!c) return 1;
    helper_port = jack_port_register(c, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    jack_port_register(c, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    jack_set_process_callback(c, process_copy, NULL);
    jack_activate(c);
    jack_native_thread_t tid;
    if (jack_client_create_thread(c, &tid, jack_client_real_time_priority(c), 1, helper_thread, NULL) != 0) {
        printf("FAIL rt-helper-thread: cannot create a realtime thread\n");
        jack_client_close(c);
        return 1;
    }
    for (int i = 0; i < 200; i++) {
        jack_connect(c, "helper:out", "helper:in");
        jack_disconnect(c, "helper:out", "helper:in");
    }
    atomic_store(&helper_stop, 1);
    pthread_join(tid, NULL);
    float period_us = 1e6f * jack_get_buffer_size(c) / jack_get_sample_rate(c);
    jack_client_close(c);
    int bad = helper_worst_us > period_us / 2;
    printf("%s rt-helper-thread: longest jack_port_get_latency_range() call %.0f us (period %.0f us)\n",
           bad ? "FAIL" : "PASS", helper_worst_us, period_us);
    return bad;
}

int main(int argc, char **argv)
{
    static const struct { const char *name; int (*fn)(void); } tests[] = {
        {"api-concurrency", test_api_concurrency}, {"activate-init", test_activate_init},
        {"port-churn", test_port_churn}, {"sync-callback", test_sync_callback},
        {"transport-churn", test_transport_churn}, {"rt-helper-thread", test_rt_helper_thread},
    };
    int fail = 0;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int run = argc < 2;
        for (int a = 1; a < argc; a++)
            if (!strcmp(argv[a], tests[i].name)) run = 1;
        if (run)
            fail |= tests[i].fn();
    }
    return fail;
}
