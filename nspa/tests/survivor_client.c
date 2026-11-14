/*
 * survivor_client: a JACK client that should outlive its server's card. Connects its input to
 * system:capture_1 and its output to system:playback_1 (it writes zeros: silent), counts its
 * process cycles and any shutdown callback, and prints once a second:
 *   t=<s> cycles=<total> rate=<cycles/s> in=<connections> out=<connections> shutdown=<0|1> xruns=<total> load=<%>
 *   survivor_client [seconds=120]
 */
#include <jack/jack.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static jack_port_t *in, *out;
static unsigned long cycles;   /* written by the process thread, read by main: atomics */
static int shut;
static int xruns;

static int xrun_cb(void *arg) { (void)arg; __atomic_add_fetch(&xruns, 1, __ATOMIC_RELAXED); return 0; }

static int process(jack_nframes_t n, void *arg)
{
	(void)arg;
	memset(jack_port_get_buffer(out, n), 0, n * sizeof(float));
	__atomic_add_fetch(&cycles, 1, __ATOMIC_RELAXED);
	return 0;
}

static void shutdown_cb(void *arg) { (void)arg; __atomic_store_n(&shut, 1, __ATOMIC_RELAXED); }

int main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 120;
	jack_client_t *c = jack_client_open("survivor", JackNoStartServer, NULL);
	if (!c) { printf("no server\n"); return 1; }
	in = jack_port_register(c, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
	out = jack_port_register(c, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
	jack_set_process_callback(c, process, NULL);
	jack_on_shutdown(c, shutdown_cb, NULL);
	jack_set_xrun_callback(c, xrun_cb, NULL);
	if (jack_activate(c)) { printf("cannot activate\n"); return 1; }
	jack_connect(c, "system:capture_1", jack_port_name(in));
	jack_connect(c, jack_port_name(out), "system:playback_1");
	unsigned long last = 0;
	for (int t = 0; t < seconds; t++) {
		sleep(1);
		unsigned long now = __atomic_load_n(&cycles, __ATOMIC_RELAXED);
		int sh = __atomic_load_n(&shut, __ATOMIC_RELAXED);
		printf("t=%d cycles=%lu rate=%lu in=%d out=%d shutdown=%d xruns=%d load=%.1f\n", t + 1, now, now - last,
		       jack_port_connected(in), jack_port_connected(out), sh, __atomic_load_n(&xruns, __ATOMIC_RELAXED), jack_cpu_load(c));
		fflush(stdout);
		last = now;
		if (sh)
			break;
	}
	int sh = __atomic_load_n(&shut, __ATOMIC_RELAXED);
	if (!sh)
		jack_client_close(c);
	return sh;
}
