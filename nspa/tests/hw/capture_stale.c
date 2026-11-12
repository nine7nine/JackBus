/* jack2-nspa hardware test: capture buffer in the cycle a capture port gets
 * connected (C24).
 *
 * The ALSA driver fills only the capture ports that have connections, and it
 * decides that before the engine switches to the new graph. In the cycle
 * where a new connection takes effect the port was skipped, and the client
 * receives whatever the buffer held from the last time it was filled (old
 * samples from when it was last connected). The fork fills every channel
 * while a graph change is pending, and zeroes a skipped buffer once.
 *
 * The test reads system:capture_1's own buffer in every cycle (a client may
 * read any output port's buffer without being connected to it). In the first
 * cycle after connecting, the buffer it receives must differ from what that
 * port held in the previous cycle; identical means the driver did not fill
 * it. A silent input (the buffer after that also all zero) is inconclusive.
 *
 * Usage: capture_stale [rounds=20] [port=system:capture_1]
 * Exit 0 if no stale buffer was seen.
 */
#include <jack/jack.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXN 8192
static jack_port_t *in, *cap;
static float prev[MAXN];
static int have_prev;
/* 1: waiting for the first connected cycle, 2: for the one after, 3: done */
static atomic_int state;
static atomic_int first_same, first_zero, second_zero;

static int all_zero(const float *b, jack_nframes_t n)
{
    for (jack_nframes_t i = 0; i < n; i++)
        if (b[i] != 0.f)
            return 0;
    return 1;
}

static int process(jack_nframes_t n, void *arg)
{
    (void)arg;
    const float *peek = jack_port_get_buffer(cap, n);
    int connected = jack_port_connected(in) > 0;
    int st = atomic_load(&state);
    if (st == 1 && connected && have_prev) {
        const float *buf = jack_port_get_buffer(in, n);
        atomic_store(&first_same, memcmp(buf, prev, n * sizeof(float)) == 0);
        atomic_store(&first_zero, all_zero(buf, n));
        atomic_store(&state, 2);
    } else if (st == 2) {
        atomic_store(&second_zero, all_zero(jack_port_get_buffer(in, n), n));
        atomic_store(&state, 3);
    }
    memcpy(prev, peek, n * sizeof(float));
    have_prev = 1;
    return 0;
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 20;
    const char *src = argc > 2 ? argv[2] : "system:capture_1";
    int stale_old = 0, stale_zero = 0, fresh = 0, silent = 0;
    jack_client_t *c = jack_client_open("stale", JackNoStartServer, NULL);
    if (!c)
        return 2;
    if (jack_get_buffer_size(c) > MAXN || !(cap = jack_port_by_name(c, src))) {
        fprintf(stderr, "no port %s\n", src);
        return 2;
    }
    in = jack_port_register(c, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    jack_set_process_callback(c, process, NULL);
    jack_activate(c);
    char dst[64];
    snprintf(dst, sizeof dst, "%s:in", jack_get_client_name(c));
    for (int r = 0; r < rounds; r++) {
        usleep(200000);  /* several cycles disconnected */
        atomic_store(&state, 1);
        if (jack_connect(c, src, dst)) {
            fprintf(stderr, "connect failed\n");
            return 2;
        }
        while (atomic_load(&state) != 3)
            usleep(1000);
        if (atomic_load(&second_zero))
            silent++;
        else if (!atomic_load(&first_same))
            fresh++;
        else if (atomic_load(&first_zero))
            stale_zero++;
        else
            stale_old++;
        jack_disconnect(c, src, dst);
    }
    jack_client_close(c);
    int stale = stale_old + stale_zero;
    printf("%s: %d connections to %s: %d fresh, %d stale (%d old samples, %d zeroed), %d inconclusive (silent input)\n",
           stale ? "FAIL" : (fresh ? "PASS" : "INCONCLUSIVE"), rounds, src, fresh, stale, stale_old, stale_zero, silent);
    return stale ? 1 : (fresh ? 0 : 3);
}
