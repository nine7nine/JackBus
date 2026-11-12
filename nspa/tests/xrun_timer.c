/* jack2-nspa: frame timer behaviour across a server-side xrun.
 *
 * Run against a sync-mode server (jackd -S ...): the process callback sleeps
 * for several periods at cycle XRUN_AT, which makes the driver cycle overrun
 * and the server reset its frame timer (JackEngineControl::NotifyXRun ->
 * JackFrameTimer::ResetFrameTime).
 *
 * Checks:
 *  - a monitor thread polls jack_get_cycle_times() continuously:
 *    next_usecs must always be > current_usecs;
 *  - in each callback, err = now - current_usecs (how long after the DLL's
 *    idea of the cycle start the callback runs). After the xrun, err must
 *    return to its pre-xrun level within SETTLE cycles (a DLL chasing a stale
 *    prediction stays off by about the stall length for hundreds of cycles).
 *
 * Exit status 0 = pass, 1 = fail, 2 = setup problem (e.g. no xrun happened).
 */
#include <jack/jack.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define XRUN_AT 300
#define SETTLE 8
#define TOTAL (XRUN_AT + 400)

static jack_client_t *client;
static int64_t err_us[TOTAL];
static atomic_int cycle;
static atomic_int xruns, running = 1;
static atomic_ulong monitor_bad, monitor_reads;
static float nominal_period;

static int on_xrun(void *arg)
{
    (void)arg;
    atomic_fetch_add(&xruns, 1);
    return 0;
}

static int process(jack_nframes_t n, void *arg)
{
    (void)n; (void)arg;
    jack_time_t now = jack_get_time();
    jack_nframes_t cf;
    jack_time_t cu, nu;
    float per;
    int c = atomic_load(&cycle);

    if (c >= TOTAL)
        return 0;
    if (jack_get_cycle_times(client, &cf, &cu, &nu, &per) == 0)
        err_us[c] = (int64_t)now - (int64_t)cu;
    if (c == XRUN_AT)
        usleep((useconds_t)(4 * nominal_period));
    atomic_store(&cycle, c + 1);
    return 0;
}

static void *monitor(void *arg)
{
    (void)arg;
    while (atomic_load(&running)) {
        jack_nframes_t cf;
        jack_time_t cu, nu;
        float per;
        if (jack_get_cycle_times(client, &cf, &cu, &nu, &per) == 0) {
            atomic_fetch_add(&monitor_reads, 1);
            if (nu <= cu)
                atomic_fetch_add(&monitor_bad, 1);
        }
        usleep(20);
    }
    return NULL;
}

static int64_t median(int64_t *v, int from, int to)
{
    int n = to - from;
    int64_t *t = malloc(n * sizeof *t);
    for (int i = 0; i < n; i++) t[i] = v[from + i];
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && t[j - 1] > t[j]; j--) { int64_t x = t[j]; t[j] = t[j - 1]; t[j - 1] = x; }
    int64_t m = t[n / 2];
    free(t);
    return m;
}

int main(void)
{
    jack_status_t st;
    pthread_t mon;

    client = jack_client_open("xrun_timer", JackNoStartServer, &st);
    if (!client) { fprintf(stderr, "open failed 0x%x\n", st); return 2; }
    nominal_period = 1e6f * jack_get_buffer_size(client) / jack_get_sample_rate(client);
    jack_set_process_callback(client, process, NULL);
    jack_set_xrun_callback(client, on_xrun, NULL);
    if (jack_activate(client)) { fprintf(stderr, "activate failed\n"); return 2; }
    pthread_create(&mon, NULL, monitor, NULL);

    for (int i = 0; i < 2000 && atomic_load(&cycle) < TOTAL; i++)
        usleep(10000);
    atomic_store(&running, 0);
    pthread_join(mon, NULL);
    jack_deactivate(client);
    jack_client_close(client);

    if (atomic_load(&cycle) < TOTAL) { fprintf(stderr, "only %d cycles\n", atomic_load(&cycle)); return 2; }
    if (atomic_load(&xruns) == 0) { fprintf(stderr, "no xrun was reported; is the server in sync mode (-S)?\n"); return 2; }

    int64_t base = median(err_us, 50, XRUN_AT);
    int64_t worst = 0;
    int worst_at = 0;
    for (int c = XRUN_AT + SETTLE; c < TOTAL; c++) {
        int64_t d = err_us[c] - base;
        if (d < 0) d = -d;
        if (d > worst) { worst = d; worst_at = c - XRUN_AT; }
    }
    int64_t limit = (int64_t)(nominal_period / 4);
    printf("period=%.0fus baseline_err=%lldus worst_dev_after_settle=%lldus (cycle +%d, limit %lld) "
           "monitor_reads=%lu next<=current=%lu xruns=%d\n",
           nominal_period, (long long)base, (long long)worst, worst_at, (long long)limit,
           atomic_load(&monitor_reads), atomic_load(&monitor_bad), atomic_load(&xruns));
    return (worst > limit || atomic_load(&monitor_bad)) ? 1 : 0;
}
