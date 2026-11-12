/* jack2-nspa hardware test: stale capture audio after (re)connecting.
 * Connects system:capture_1 -> stale:in, records the last buffer while
 * connected and the first buffer after each reconnection. With the bug, the
 * driver did not fill the capture buffer in the cycle where the connection
 * took effect, so the first buffer equals the last one before disconnecting.
 * Usage: capture_stale [rounds=20]   Exit 0 if no stale buffer was seen. */
#include <jack/jack.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXN 8192
static jack_port_t *in;
static float last[MAXN], first[MAXN];
static atomic_int was_connected, have_first, nframes_seen;

static int process(jack_nframes_t n, void *arg)
{
    (void)arg;
    int connected = jack_port_connected(in) > 0;
    float *buf = jack_port_get_buffer(in, n);
    atomic_store(&nframes_seen, n);
    if (connected) {
        if (!atomic_load(&was_connected) && !atomic_load(&have_first)) {
            memcpy(first, buf, n * sizeof(float));
            atomic_store(&have_first, 1);
        }
        memcpy(last, buf, n * sizeof(float));
    }
    atomic_store(&was_connected, connected);
    return 0;
}

static int all_zero(const float *b, int n) { for (int i = 0; i < n; i++) if (b[i] != 0.f) return 0; return 1; }

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 20, stale = 0, zero = 0, fresh = 0;
    static float prev_last[MAXN];
    jack_client_t *c = jack_client_open("stale", JackNoStartServer, NULL);
    if (!c) return 2;
    in = jack_port_register(c, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    jack_set_process_callback(c, process, NULL);
    jack_activate(c);
    for (int r = 0; r < rounds; r++) {
        atomic_store(&have_first, 0);
        if (jack_connect(c, "system:capture_1", "stale:in")) { fprintf(stderr, "connect failed\n"); return 2; }
        usleep(200000);
        while (!atomic_load(&have_first)) usleep(1000);
        int n = atomic_load(&nframes_seen);
        if (r > 0) {
            if (memcmp(first, prev_last, n * sizeof(float)) == 0) {
                if (all_zero(first, n)) zero++; else stale++;
            } else {
                fresh++;
            }
        }
        jack_disconnect(c, "system:capture_1", "stale:in");
        usleep(200000);
        memcpy(prev_last, last, n * sizeof(float));
    }
    jack_client_close(c);
    printf("%s: %d reconnections: %d stale (identical to last buffer before disconnect), %d fresh, %d inconclusive (silence)\n",
           stale ? "FAIL" : "PASS", rounds - 1, stale, fresh, zero);
    return stale ? 1 : 0;
}
