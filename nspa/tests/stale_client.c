/* JackBus: a client process that outlives its server must not disturb a
 * client of the same name on the next server.
 *
 * JACK's per-client futex is a file /dev/shm/jack_sem.<uid>_<server>_<client>.
 * A server stopped with clients connected left those files behind, and the
 * next server opened the existing file again. A hung process of the old client
 * still had it mapped and waited on it, so it took the new client's wakeups
 * ("client = stale was not finished, state = Triggered" every other cycle).
 *
 *   stale_client hang          open "stale", activate, ignore the server
 *                              going away, sleep until killed
 *   stale_client check [secs]  open "stale", count cycles and xruns for secs
 *                              seconds (default 2). Exit 0 if every cycle ran.
 */
#include <jack/jack.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static atomic_long cycles, xruns;

static int process(jack_nframes_t n, void *arg) { (void)n; (void)arg; atomic_fetch_add(&cycles, 1); return 0; }
static int on_xrun(void *arg) { (void)arg; atomic_fetch_add(&xruns, 1); return 0; }

int main(int argc, char **argv)
{
    int check = argc > 1 && !strcmp(argv[1], "check");
    jack_client_t *c = jack_client_open("stale", JackNoStartServer | JackUseExactName, NULL);
    if (!c) { fprintf(stderr, "cannot open client \"stale\"\n"); return 2; }
    jack_set_process_callback(c, process, NULL);
    jack_set_xrun_callback(c, on_xrun, NULL);
    if (jack_activate(c)) { fprintf(stderr, "activate failed\n"); return 2; }
    if (!check) {
        for (;;) pause();
    }
    int secs = argc > 2 ? atoi(argv[2]) : 2;
    usleep(200000);  /* let the activation settle */
    long c0 = atomic_load(&cycles), x0 = atomic_load(&xruns);
    sleep(secs);
    long got = atomic_load(&cycles) - c0, xr = atomic_load(&xruns) - x0;
    long want = (long)secs * jack_get_sample_rate(c) / jack_get_buffer_size(c);
    jack_client_close(c);
    int ok = xr == 0 && got >= want * 9 / 10;
    printf("%s: %ld of ~%ld cycles ran, %ld xruns\n", ok ? "PASS" : "FAIL", got, want, xr);
    return ok ? 0 : 1;
}
