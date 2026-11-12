/* jack2-nspa stress/consistency client.
 *
 * Opens N clients in a chain (c0 -> c1 -> ... -> cN-1, 8 audio ports each),
 * runs a connect/disconnect loop on the chain from the main thread, and in
 * every process cycle checks the timing API for consistency:
 *   - jack_get_cycle_times() succeeds, next_usecs > current_usecs
 *   - current_frames == jack_last_frame_time()
 *   - current_frames advances by exactly nframes between consecutive cycles
 *     (unless an xrun was reported in between)
 *   - jack_frame_time() lies within [current_frames, current_frames + 2*nframes)
 * Exit status: 0 on success, 1 on any violation or API failure.
 *
 * Usage: stress [nclients=4] [iterations=200] [settle_ms=300]
 */
#include <jack/jack.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NPORTS 8
#define MAXCLIENTS 32

struct client {
    jack_client_t *c;
    jack_port_t *in[NPORTS], *out[NPORTS];
    _Atomic jack_nframes_t last_frames, first_frames;
    int have_last;
    atomic_int wrapped;
    atomic_ulong cycles, bad_times, bad_continuity, bad_frame_time;
    atomic_int xrun_pending;
};

static struct client cl[MAXCLIENTS];
static atomic_ulong xruns, graph_orders;

static int on_graph_order(void *arg)
{
    (void)arg;
    atomic_fetch_add(&graph_orders, 1);
    return 0;
}

static int on_xrun(void *arg)
{
    struct client *s = arg;
    atomic_fetch_add(&xruns, 1);
    atomic_store(&s->xrun_pending, 1);
    return 0;
}

static int process(jack_nframes_t n, void *arg)
{
    struct client *s = arg;
    jack_nframes_t cf;
    jack_time_t cu, nu;
    float period;

    if (jack_get_cycle_times(s->c, &cf, &cu, &nu, &period) != 0 || nu <= cu) {
        atomic_fetch_add(&s->bad_times, 1);
    } else {
        if (cf != jack_last_frame_time(s->c))
            atomic_fetch_add(&s->bad_times, 1);
        if (s->have_last && !atomic_exchange(&s->xrun_pending, 0)
            && (jack_nframes_t)(cf - s->last_frames) != n)
            atomic_fetch_add(&s->bad_continuity, 1);
        if (!s->have_last)
            s->first_frames = cf;
        else if (cf < s->last_frames)
            atomic_store(&s->wrapped, 1);
        jack_nframes_t ft = jack_frame_time(s->c);
        if ((jack_nframes_t)(ft - cf) >= 2 * n)
            atomic_fetch_add(&s->bad_frame_time, 1);
        s->last_frames = cf;
        s->have_last = 1;
    }
    for (int i = 0; i < NPORTS; i++) {
        float *src = jack_port_get_buffer(s->in[i], n);
        float *dst = jack_port_get_buffer(s->out[i], n);
        memcpy(dst, src, n * sizeof(float));
    }
    atomic_fetch_add(&s->cycles, 1);
    return 0;
}

int main(int argc, char **argv)
{
    int nclients = argc > 1 ? atoi(argv[1]) : 4;
    int iters = argc > 2 ? atoi(argv[2]) : 200;
    int settle_ms = argc > 3 ? atoi(argv[3]) : 300;
    int failures = 0;
    char name[64], a[96], b[96];

    if (nclients < 2 || nclients > MAXCLIENTS) {
        fprintf(stderr, "nclients must be 2..%d\n", MAXCLIENTS);
        return 2;
    }
    printf("libjack: %s\n", jack_get_version_string());

    for (int k = 0; k < nclients; k++) {
        jack_status_t st;
        snprintf(name, sizeof name, "stress%d", k);
        cl[k].c = jack_client_open(name, JackNoStartServer, &st);
        if (!cl[k].c) {
            fprintf(stderr, "FAIL: jack_client_open(%s) status=0x%x\n", name, st);
            return 1;
        }
        for (int i = 0; i < NPORTS; i++) {
            snprintf(a, sizeof a, "in%d", i);
            snprintf(b, sizeof b, "out%d", i);
            cl[k].in[i] = jack_port_register(cl[k].c, a, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
            cl[k].out[i] = jack_port_register(cl[k].c, b, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
            if (!cl[k].in[i] || !cl[k].out[i]) {
                fprintf(stderr, "FAIL: port_register on %s\n", name);
                return 1;
            }
        }
        jack_set_process_callback(cl[k].c, process, &cl[k]);
        jack_set_xrun_callback(cl[k].c, on_xrun, &cl[k]);
        if (k == 0)
            jack_set_graph_order_callback(cl[k].c, on_graph_order, NULL);
        if (jack_activate(cl[k].c)) {
            fprintf(stderr, "FAIL: jack_activate(%s)\n", name);
            return 1;
        }
    }

    for (int it = 0; it < iters; it++) {
        for (int k = 1; k < nclients; k++) {
            for (int i = 0; i < NPORTS; i++) {
                snprintf(a, sizeof a, "stress%d:out%d", k - 1, i);
                snprintf(b, sizeof b, "stress%d:in%d", k, i);
                int r = (it & 1) ? jack_disconnect(cl[0].c, a, b) : jack_connect(cl[0].c, a, b);
                if (r != 0) {
                    fprintf(stderr, "FAIL: %s %s -> %s = %d\n", (it & 1) ? "disconnect" : "connect", a, b, r);
                    failures++;
                }
            }
        }
    }
    usleep(settle_ms * 1000);
    if (iters > 0 && atomic_load(&graph_orders) == 0) {
        printf("FAIL: no graph order callback after %d connect/disconnect rounds\n", iters);
        failures++;
    }

    unsigned long tot_cycles = 0, tot_bad = 0;
    for (int k = 0; k < nclients; k++) {
        unsigned long bt = atomic_load(&cl[k].bad_times), bc = atomic_load(&cl[k].bad_continuity),
                      bf = atomic_load(&cl[k].bad_frame_time), cy = atomic_load(&cl[k].cycles);
        tot_cycles += cy;
        tot_bad += bt + bc + bf;
        if (bt || bc || bf)
            printf("client %d: cycles=%lu bad_times=%lu bad_continuity=%lu bad_frame_time=%lu\n", k, cy, bt, bc, bf);
        if (cy == 0) {
            printf("client %d: FAIL no cycles\n", k);
            failures++;
        }
    }
    printf("clients=%d iters=%d cycles=%lu xruns=%lu graph_orders=%lu timing_violations=%lu api_failures=%d frames=%u..%u%s\n",
           nclients, iters, tot_cycles, atomic_load(&xruns), atomic_load(&graph_orders), tot_bad, failures,
           cl[0].first_frames, cl[0].last_frames, atomic_load(&cl[0].wrapped) ? " (wrapped)" : "");

    for (int k = nclients - 1; k >= 0; k--)
        jack_client_close(cl[k].c);
    return (failures || tot_bad) ? 1 : 0;
}
