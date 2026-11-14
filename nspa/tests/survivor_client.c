/*
 * survivor_client: a JACK client that should outlive its server's card. Connects its input to
 * system:capture_1 and its output to system:playback_1 (it writes zeros: silent), counts its
 * process cycles and any shutdown callback, and prints once a second:
 *   t=<s> cycles=<total> rate=<cycles/s> in=<connections> out=<connections> shutdown=<0|1>
 *   survivor_client [seconds=120]
 */
#include <jack/jack.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static jack_port_t *in, *out;
static volatile unsigned long cycles;
static volatile int shut;

static int process(jack_nframes_t n, void *arg)
{
	(void)arg;
	memset(jack_port_get_buffer(out, n), 0, n * sizeof(float));
	cycles++;
	return 0;
}

static void shutdown_cb(void *arg) { (void)arg; shut = 1; }

int main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 120;
	jack_client_t *c = jack_client_open("survivor", JackNoStartServer, NULL);
	if (!c) { printf("no server\n"); return 1; }
	in = jack_port_register(c, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
	out = jack_port_register(c, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
	jack_set_process_callback(c, process, NULL);
	jack_on_shutdown(c, shutdown_cb, NULL);
	if (jack_activate(c)) { printf("cannot activate\n"); return 1; }
	jack_connect(c, "system:capture_1", jack_port_name(in));
	jack_connect(c, jack_port_name(out), "system:playback_1");
	unsigned long last = 0;
	for (int t = 0; t < seconds; t++) {
		sleep(1);
		unsigned long now = cycles;
		printf("t=%d cycles=%lu rate=%lu in=%d out=%d shutdown=%d\n", t + 1, now, now - last,
		       jack_port_connected(in), jack_port_connected(out), shut);
		fflush(stdout);
		last = now;
		if (shut)
			break;
	}
	if (!shut)
		jack_client_close(c);
	return shut;
}
