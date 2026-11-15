/*
 * jbpw-check: feeds a JACK input port from one client and checks a JACK
 * output port in another, sample for sample, in the same cycle. Used to
 * show that a PipeWire filter run through jackbus-pw processes audio in
 * the JACK graph:  src:out -> <filter in> ... <filter out> -> sink:in
 *
 *   jbpw-check <filter-in-port> <filter-out-port> [seconds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>

#include <jack/jack.h>

static jack_client_t *src, *sink;
static jack_port_t *src_out, *sink_in;
static atomic_ulong compared, mismatched, silent_before, nframes_seen;

static inline float sig(jack_nframes_t t)
{
	return (float)((t % 4801u) / 4801.0 - 0.5);
}

static int src_process(jack_nframes_t n, void *arg)
{
	float *buf = jack_port_get_buffer(src_out, n);
	jack_nframes_t t0 = jack_last_frame_time(src);
	for (jack_nframes_t i = 0; i < n; i++)
		buf[i] = sig(t0 + i);
	return 0;
}

static int sink_process(jack_nframes_t n, void *arg)
{
	const float *buf = jack_port_get_buffer(sink_in, n);
	jack_nframes_t t0 = jack_last_frame_time(sink);
	int zero = 1, bad = 0;

	for (jack_nframes_t i = 0; i < n; i++) {
		if (buf[i] != 0.0f)
			zero = 0;
		if (buf[i] != sig(t0 + i))
			bad = 1;
	}
	atomic_store(&nframes_seen, n);
	if (atomic_load(&compared) == 0 && zero) {
		atomic_fetch_add(&silent_before, 1);	/* not linked or not streaming yet */
		return 0;
	}
	atomic_fetch_add(&compared, 1);
	if (bad)
		atomic_fetch_add(&mismatched, 1);
	return 0;
}

int main(int argc, char *argv[])
{
	const char *fin, *fout;
	int seconds, i;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <filter-in-port> <filter-out-port> [seconds]\n", argv[0]);
		return 2;
	}
	fin = argv[1];
	fout = argv[2];
	seconds = argc > 3 ? atoi(argv[3]) : 2;

	src = jack_client_open("jbpw-src", JackNoStartServer, NULL);
	sink = jack_client_open("jbpw-sink", JackNoStartServer, NULL);
	if (!src || !sink) {
		fprintf(stderr, "check: no JACK server\n");
		return 2;
	}
	src_out = jack_port_register(src, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
	sink_in = jack_port_register(sink, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
	jack_set_process_callback(src, src_process, NULL);
	jack_set_process_callback(sink, sink_process, NULL);
	jack_activate(src);
	jack_activate(sink);

	for (i = 0; i < 100 && (!jack_port_by_name(src, fin) || !jack_port_by_name(src, fout)); i++)
		usleep(100000);
	if (i == 100) {
		fprintf(stderr, "check: ports %s / %s never appeared\n", fin, fout);
		return 2;
	}
	if (jack_connect(src, jack_port_name(src_out), fin) ||
	    jack_connect(src, fout, jack_port_name(sink_in))) {
		fprintf(stderr, "check: can't connect\n");
		return 2;
	}
	sleep(seconds);
	jack_deactivate(sink);
	jack_deactivate(src);

	printf("check: server %u Hz, %lu frames per cycle; %lu cycles compared, %lu mismatched"
			" (%lu silent cycles before the first signal)\n",
			jack_get_sample_rate(src), atomic_load(&nframes_seen),
			atomic_load(&compared), atomic_load(&mismatched), atomic_load(&silent_before));
	jack_client_close(sink);
	jack_client_close(src);
	return atomic_load(&compared) > 0 && atomic_load(&mismatched) == 0 ? 0 : 1;
}
