/* jack2-nspa: input-port mixdown correctness.
 *
 * One client connects k = 0..6 of its own outputs to one of its inputs and
 * checks, every cycle, that the input buffer is exactly the sum of the
 * connected outputs. The sources write multiples of 1/64 in [-0.5, 0.5), so
 * every partial sum is exact in single precision and the check does not
 * depend on the summation order: a missing, doubled or stale source, or a
 * wrong tail (period not a multiple of the vector width), shows up as a
 * mismatch. k = 0 checks the zero-filled buffer, k = 1 the single-source
 * copy (same-client connection), k >= 2 the mixdown.
 *
 * Usage: mixdown [cycles per k = 50]
 */
#include <jack/jack.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define SRC 6
static jack_port_t *out[SRC], *in;
static atomic_int checked[SRC + 1], bad[SRC + 1];
static atomic_uint cycle;

static float val(int s, jack_nframes_t i, unsigned c)
{
    return (float)((int)((s * 31u + i * 7u + c * 13u) % 64u) - 32) / 64.f;
}

static int process(jack_nframes_t n, void *arg)
{
    (void)arg;
    unsigned c = atomic_fetch_add(&cycle, 1);
    for (int s = 0; s < SRC; s++) {
        float *b = jack_port_get_buffer(out[s], n);
        for (jack_nframes_t i = 0; i < n; i++)
            b[i] = val(s, i, c);
    }
    /* outputs are connected in order, so the first k are the sources */
    int k = jack_port_connected(in);
    if (k < 0 || k > SRC)
        return 0;
    const float *b = jack_port_get_buffer(in, n);
    int wrong = 0;
    for (jack_nframes_t i = 0; i < n; i++) {
        float e = 0.f;
        for (int s = 0; s < k; s++)
            e += val(s, i, c);
        wrong |= b[i] != e;
    }
    atomic_fetch_add(&checked[k], 1);
    if (wrong)
        atomic_fetch_add(&bad[k], 1);
    return 0;
}

static void wait_cycles(unsigned n)
{
    unsigned start = atomic_load(&cycle);
    while (atomic_load(&cycle) - start < n)
        usleep(1000);
}

int main(int argc, char **argv)
{
    unsigned per_k = argc > 1 ? (unsigned)atoi(argv[1]) : 50;
    jack_client_t *c = jack_client_open("mixdown", JackNoStartServer, NULL);
    if (!c) {
        printf("FAIL: cannot open client\n");
        return 2;
    }
    char name[32], dst[64];
    for (int s = 0; s < SRC; s++) {
        snprintf(name, sizeof name, "out%d", s);
        out[s] = jack_port_register(c, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    }
    in = jack_port_register(c, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    jack_set_process_callback(c, process, NULL);
    if (jack_activate(c)) {
        printf("FAIL: cannot activate\n");
        return 2;
    }
    snprintf(dst, sizeof dst, "%s:in", jack_get_client_name(c));
    wait_cycles(per_k);
    for (int s = 0; s < SRC; s++) {
        if (jack_connect(c, jack_port_name(out[s]), dst)) {
            printf("FAIL: cannot connect %s\n", jack_port_name(out[s]));
            return 2;
        }
        wait_cycles(per_k);
    }
    jack_deactivate(c);
    int fail = 0;
    char detail[256] = "", *p = detail;
    for (int k = 0; k <= SRC; k++) {
        int ck = atomic_load(&checked[k]), bk = atomic_load(&bad[k]);
        p += snprintf(p, detail + sizeof detail - p, " k=%d:%d/%d", k, ck - bk, ck);
        if (bk || ck == 0)
            fail = 1;
    }
    printf("%s: %u frames, exact sums in%s (correct/checked cycles)\n", fail ? "FAIL" : "PASS",
           jack_get_buffer_size(c), detail);
    jack_client_close(c);
    return fail;
}
