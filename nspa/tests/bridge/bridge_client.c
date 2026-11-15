/*
 jack2-nspa test client for sound card bridges: reads one port, writes silence to another,
 counts cycles and xruns, measures the level of what it reads, and prints once a second:
   t=<s> cycles=<n> xruns=<n> peak=<dBFS of the input this second>
 At the end: the two ports' latency ranges and flags.
   bridge_client <capture port> <playback port> [seconds=30]
*/
#include <jack/jack.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static jack_port_t *in, *out;
static unsigned long cycles;
static int xruns;
static float peak;

static int xrun_cb(void *arg) { (void)arg; __atomic_add_fetch(&xruns, 1, __ATOMIC_RELAXED); return 0; }

static int process(jack_nframes_t n, void *arg)
{
	(void)arg;
	const float *x = jack_port_get_buffer(in, n);
	float p = peak;   /* written here, read and reset by main once a second (a test: no lock) */
	for (jack_nframes_t i = 0; i < n; i++)
		if (fabsf(x[i]) > p)
			p = fabsf(x[i]);
	peak = p;
	memset(jack_port_get_buffer(out, n), 0, n * sizeof(float));
	__atomic_add_fetch(&cycles, 1, __ATOMIC_RELAXED);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s capture_port playback_port [seconds]\n", argv[0]);
		return 2;
	}
	int seconds = argc > 3 ? atoi(argv[3]) : 30;
	jack_client_t *c = jack_client_open("bridgetest", JackNoStartServer, NULL);
	if (!c) { printf("no server\n"); return 1; }
	in = jack_port_register(c, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
	out = jack_port_register(c, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
	jack_set_process_callback(c, process, NULL);
	jack_set_xrun_callback(c, xrun_cb, NULL);
	if (jack_activate(c)) { printf("cannot activate\n"); return 1; }
	if (jack_connect(c, argv[1], jack_port_name(in)) || jack_connect(c, jack_port_name(out), argv[2])) {
		printf("cannot connect %s / %s\n", argv[1], argv[2]);
		return 1;
	}
	unsigned long last = 0;
	for (int t = 0; t < seconds; t++) {
		sleep(1);
		unsigned long now = __atomic_load_n(&cycles, __ATOMIC_RELAXED);
		float p = peak;
		peak = 0.0f;
		printf("t=%d cycles=%lu rate=%lu xruns=%d peak=%.1f\n", t + 1, now, now - last,
		       __atomic_load_n(&xruns, __ATOMIC_RELAXED), p > 0 ? 20.0 * log10(p) : -200.0);
		fflush(stdout);
		last = now;
	}
	jack_port_t *cp = jack_port_by_name(c, argv[1]), *pp = jack_port_by_name(c, argv[2]);
	jack_latency_range_t r;
	if (cp) {
		jack_port_get_latency_range(cp, JackCaptureLatency, &r);
		printf("%s: capture latency %u..%u, flags physical=%d terminal=%d\n", argv[1], r.min, r.max,
		       !!(jack_port_flags(cp) & JackPortIsPhysical), !!(jack_port_flags(cp) & JackPortIsTerminal));
	}
	if (pp) {
		jack_port_get_latency_range(pp, JackPlaybackLatency, &r);
		printf("%s: playback latency %u..%u, flags physical=%d terminal=%d\n", argv[2], r.min, r.max,
		       !!(jack_port_flags(pp) & JackPortIsPhysical), !!(jack_port_flags(pp) & JackPortIsTerminal));
	}
	jack_client_close(c);
	return 0;
}
