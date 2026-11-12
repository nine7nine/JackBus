/* jack2-nspa: the server's per-cycle loops stop at the highest refnum in use.
 *
 * Clients come and go so that live refnums are high, low, and the highest
 * one disappears:
 *   1. 12 clients in a chain (refnums ascending)
 *   2. the 6 lowest refnums close; the rest keep running
 *   3. 6 new clients (they reuse the low refnums) are appended to the chain
 *   4. the client with the highest refnum closes (the bound drops)
 * In every stage, for 100 cycles: every live client runs every cycle, the
 * value the head writes (derived from the cycle's frame time) reaches the
 * tail in the same cycle (activation order intact), and no xrun occurs.
 */
#include <jack/jack.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAXC 24
typedef struct {
    jack_client_t *c;
    jack_port_t *in, *out;
    atomic_int cycles;
    atomic_int is_head, is_tail;
    atomic_int tail_ok, tail_bad;
    int live;
} cl_t;

static cl_t cl[MAXC];
static atomic_int xruns;

static float stamp(jack_client_t *c)
{
    return (float)((jack_last_frame_time(c) / 16) % 1000000);
}

static int process(jack_nframes_t n, void *arg)
{
    cl_t *k = arg;
    float *in = jack_port_get_buffer(k->in, n), *out = jack_port_get_buffer(k->out, n);
    if (atomic_load(&k->is_head)) {
        for (jack_nframes_t i = 0; i < n; i++)
            out[i] = stamp(k->c);
    } else {
        memcpy(out, in, n * sizeof(float));
    }
    if (atomic_load(&k->is_tail)) {
        if (in[0] == stamp(k->c) && in[n - 1] == stamp(k->c))
            atomic_fetch_add(&k->tail_ok, 1);
        else
            atomic_fetch_add(&k->tail_bad, 1);
    }
    atomic_fetch_add(&k->cycles, 1);
    return 0;
}

static int on_xrun(void *arg)
{
    (void)arg;
    atomic_fetch_add(&xruns, 1);
    return 0;
}

static int open_cl(int i)
{
    char nm[32];
    snprintf(nm, sizeof nm, "rb%d", i);
    cl_t *k = &cl[i];
    memset(k, 0, sizeof *k);
    if (!(k->c = jack_client_open(nm, JackNoStartServer, NULL)))
        return -1;
    k->in = jack_port_register(k->c, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    k->out = jack_port_register(k->c, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    jack_set_process_callback(k->c, process, k);
    jack_set_xrun_callback(k->c, on_xrun, NULL);
    if (jack_activate(k->c))
        return -1;
    k->live = 1;
    return 0;
}

static void close_cl(int i)
{
    jack_client_close(cl[i].c);
    cl[i].live = 0;
}

/* chain the live clients in the given order */
static int chain(const int *order, int n)
{
    for (int j = 0; j < n; j++) {
        cl_t *k = &cl[order[j]];
        const char **conns = jack_port_get_connections(k->in);
        if (conns) {
            for (int m = 0; conns[m]; m++)
                jack_disconnect(k->c, conns[m], jack_port_name(k->in));
            jack_free(conns);
        }
        atomic_store(&k->is_head, j == 0);
        atomic_store(&k->is_tail, j == n - 1);
    }
    for (int j = 1; j < n; j++)
        if (jack_connect(cl[order[j]].c, jack_port_name(cl[order[j - 1]].out), jack_port_name(cl[order[j]].in)))
            return -1;
    return 0;
}

/* run 100 cycles after a settling period; check every live client */
static int check(const char *stage, const int *order, int n, jack_client_t *ref)
{
    usleep(300000);  /* graph change settles */
    int base[MAXC];
    for (int j = 0; j < n; j++) {
        base[j] = atomic_load(&cl[order[j]].cycles);
        atomic_store(&cl[order[j]].tail_ok, 0);
        atomic_store(&cl[order[j]].tail_bad, 0);
    }
    int x0 = atomic_load(&xruns);
    jack_nframes_t f0 = jack_frame_time(ref);
    while (jack_frame_time(ref) - f0 < 100 * jack_get_buffer_size(ref))
        usleep(2000);
    int bad = 0, minc = 1 << 30;
    for (int j = 0; j < n; j++) {
        int c = atomic_load(&cl[order[j]].cycles) - base[j];
        if (c < minc)
            minc = c;
    }
    cl_t *tail = &cl[order[n - 1]];
    int ok = atomic_load(&tail->tail_ok), tb = atomic_load(&tail->tail_bad), xr = atomic_load(&xruns) - x0;
    if (minc < 90 || tb || xr)
        bad = 1;
    printf("  %-34s clients=%2d min cycles=%3d tail same-cycle %d/%d xruns=%d %s\n", stage, n, minc, ok, ok + tb, xr, bad ? "FAIL" : "ok");
    return bad;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int order[MAXC], n = 0, fail = 0;
    for (int i = 0; i < 12; i++) {
        if (open_cl(i)) { printf("FAIL: open\n"); return 2; }
        order[n++] = i;
    }
    jack_client_t *ref = cl[11].c;
    if (chain(order, n)) { printf("FAIL: connect\n"); return 2; }
    fail |= check("1: 12 clients", order, n, ref);

    for (int i = 0; i < 6; i++)
        close_cl(i);
    n = 0;
    for (int i = 6; i < 12; i++)
        order[n++] = i;
    chain(order, n);
    fail |= check("2: lowest 6 refnums closed", order, n, ref);

    for (int i = 12; i < 18; i++) {
        if (open_cl(i)) { printf("FAIL: reopen\n"); return 2; }
        order[n++] = i;
    }
    chain(order, n);
    fail |= check("3: 6 new clients (low refnums)", order, n, ref);

    ref = cl[12].c;
    close_cl(11);  /* highest refnum */
    n = 0;
    for (int i = 6; i < 11; i++)
        order[n++] = i;
    for (int i = 12; i < 18; i++)
        order[n++] = i;
    chain(order, n);
    fail |= check("4: highest refnum closed", order, n, ref);

    for (int i = 0; i < MAXC; i++)
        if (cl[i].live)
            close_cl(i);
    printf("%s: per-cycle client loops bounded by the highest refnum\n", fail ? "FAIL" : "PASS");
    return fail;
}
