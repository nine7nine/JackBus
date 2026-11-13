/*
 * notify_lag: one client's notification thread is slow or stuck; what do the
 * other clients see?
 *
 *   notify_lag slow   one client's latency callback takes 1.5 s (the first 4
 *                     times): time connect, disconnect, port registration and
 *                     client open from other clients; afterwards the slow client
 *                     must have got every port registration, in order
 *   notify_lag hung   its notification thread is stuck for 6 s while another
 *                     client registers 1500 ports (more than a socket buffer of
 *                     notifications): the others must not be held up, and the
 *                     server must not block
 *   notify_lag bufsize  the client lags (slow latency callback) when the buffer
 *                     size changes: its buffer-size callback must have run
 *                     before jack_set_buffer_size() returns
 *
 * Last line PASS/FAIL. Limit for one operation: NSPA_LAG_MAX_MS (default 700).
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <jack/jack.h>

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static atomic_int slow_left, stuck_ms, latency_calls, reg_on, reg_order_bad, bufsize_seen;
static atomic_uint last_reg_id;

static void lat_cb(jack_latency_callback_mode_t m, void *a)
{
    (void)m; (void)a;
    atomic_fetch_add(&latency_calls, 1);
    int s = atomic_exchange(&stuck_ms, 0);
    if (s) {
        usleep(s * 1000);
    } else if (atomic_load(&slow_left) > 0) {
        atomic_fetch_sub(&slow_left, 1);
        usleep(1500000);
    }
}
static void reg_cb(jack_port_id_t id, int reg, void *a)
{
    (void)a;
    if (!reg) return;
    unsigned prev = atomic_exchange(&last_reg_id, id);
    if (prev && id <= prev) atomic_fetch_add(&reg_order_bad, 1);
    atomic_fetch_add(&reg_on, 1);
}
static int bs_cb(jack_nframes_t n, void *a) { (void)a; atomic_store(&bufsize_seen, (int)n); return 0; }
static int proc(jack_nframes_t n, void *a) { (void)n; (void)a; return 0; }

static double worst;
static void timed(const char *what, double t0)
{
    double d = now_ms() - t0;
    if (d > worst) worst = d;
    printf("  %-34s %7.1f ms\n", what, d);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "slow";
    double limit = getenv("NSPA_LAG_MAX_MS") ? atof(getenv("NSPA_LAG_MAX_MS")) : 700;
    jack_client_t *slow = jack_client_open("slowui", JackNoStartServer, NULL);
    jack_client_t *other = jack_client_open("other", JackNoStartServer, NULL);
    if (!slow || !other) return 1;
    jack_port_register(slow, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    jack_port_register(other, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    jack_port_register(other, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    jack_set_latency_callback(slow, lat_cb, NULL);
    jack_set_port_registration_callback(slow, reg_cb, NULL);
    jack_set_buffer_size_callback(slow, bs_cb, NULL);
    jack_set_process_callback(slow, proc, NULL);
    jack_set_process_callback(other, proc, NULL);
    jack_activate(slow);
    jack_activate(other);
    usleep(500000);
    atomic_store(&reg_on, 0);
    atomic_store(&last_reg_id, 0);
    int fail = 0;
    double t;

    if (strcmp(mode, "slow") == 0) {
        int nports = 20;
        atomic_store(&slow_left, 4);
        for (int i = 0; i < 3; i++) {
            t = now_ms(); jack_connect(other, "other:out", "other:in"); timed("connect", t);
            usleep(100000);
            t = now_ms(); jack_disconnect(other, "other:out", "other:in"); timed("disconnect", t);
            usleep(100000);
        }
        for (int i = 0; i < nports; i++) {
            char n[32];
            snprintf(n, sizeof n, "p%d", i);
            t = now_ms(); jack_port_register(other, n, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
            if (i == 0 || i == nports - 1) timed("port register", t);
        }
        t = now_ms();
        jack_client_t *x = jack_client_open("newcomer", JackNoStartServer, NULL);
        timed("client open", t);
        if (x) jack_client_close(x);
        for (int i = 0; i < 100 && atomic_load(&reg_on) < nports; i++) usleep(100000);
        printf("slowui got %d of %d port registrations, %d out of order\n",
               atomic_load(&reg_on), nports, atomic_load(&reg_order_bad));
        if (atomic_load(&reg_on) != nports || atomic_load(&reg_order_bad)) fail = 1;
    } else if (strcmp(mode, "hung") == 0) {
        int nports = 1500;
        atomic_store(&stuck_ms, 6000);
        t = now_ms(); jack_connect(other, "other:out", "other:in"); timed("connect (starts the hang)", t);
        usleep(200000);
        double t1 = now_ms();
        for (int i = 0; i < nports; i++) {
            char n[32];
            snprintf(n, sizeof n, "h%d", i);
            t = now_ms();
            if (!jack_port_register(other, n, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0)) {
                printf("  port register %d failed\n", i);
                fail = 1;
                break;
            }
            double d = now_ms() - t;
            if (d > worst) worst = d;
        }
        printf("  %-34s %7.1f ms (slowest one %.1f ms)\n", "1500 port registrations", now_ms() - t1, worst);
        t = now_ms(); jack_disconnect(other, "other:out", "other:in"); timed("disconnect", t);
        t = now_ms();
        jack_client_t *x = jack_client_open("newcomer", JackNoStartServer, NULL);
        timed("client open", t);
        if (x) jack_client_close(x);
        for (int i = 0; i < 150 && atomic_load(&reg_on) < nports; i++) usleep(100000);
        printf("slowui got %d of %d port registrations after its hang, %d out of order\n",
               atomic_load(&reg_on), nports, atomic_load(&reg_order_bad));
        if (atomic_load(&reg_order_bad)) fail = 1;
    } else if (strcmp(mode, "bufsize") == 0) {
        atomic_store(&slow_left, 1);
        t = now_ms(); jack_connect(other, "other:out", "other:in"); timed("connect (slow latency callback)", t);
        usleep(700000);     // the server has given up waiting (500 ms): slowui lags, still in its callback
        jack_nframes_t old = jack_get_buffer_size(other), want = old == 256 ? 512 : 256;
        t = now_ms(); jack_set_buffer_size(other, want); timed("jack_set_buffer_size", t);
        int seen = atomic_load(&bufsize_seen);
        printf("slowui's buffer-size callback %s before jack_set_buffer_size returned (%d)\n",
               seen == (int)want ? "ran" : "had NOT run", seen);
        if (seen != (int)want) fail = 1;
        jack_set_buffer_size(other, old);
        limit = 2600;   // the server waits for the client here, as 1.9.22 does
    }
    if (worst > limit) fail = 1;
    printf("%s: %s, slowest operation of another client %.0f ms (limit %.0f)\n",
           fail ? "FAIL" : "PASS", mode, worst, limit);
    atomic_store(&slow_left, 0);
    jack_client_close(other);
    jack_client_close(slow);
    return fail;
}
