/*
 * JackBus test client: joins a server the moment it comes up and connects at once, as a
 * bridge does (PipeWire's JACK client): it retries jack_client_open every millisecond, then
 * registers in and out, activates, and connects system:capture_1 -> in and
 * out -> system:playback_1. Prints "connected" and stays until its standard input closes or
 * the server goes away.
 *
 *   eager_client <name> [seconds to keep trying=10]
 *
 * Used by jackdbus.sh (the patchbay's graph at start).
 */

#include <jack/jack.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void shutdown_cb(void *a) { (void)a; _exit(0); }

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "eager";
	int tries = (argc > 2 ? atoi(argv[2]) : 10) * 1000;
	jack_client_t *c = NULL;

	for (int i = 0; i < tries && c == NULL; i++) {
		c = jack_client_open(name, JackNoStartServer | JackUseExactName, NULL);
		if (c == NULL)
			usleep(1000);
	}
	if (c == NULL) {
		printf("no server\n");
		return 1;
	}
	jack_port_t *in = jack_port_register(c, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
	jack_port_t *out = jack_port_register(c, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
	jack_on_shutdown(c, shutdown_cb, NULL);
	if (in == NULL || out == NULL || jack_activate(c)) {
		printf("cannot activate\n");
		return 1;
	}
	int a = jack_connect(c, "system:capture_1", jack_port_name(in));
	int b = jack_connect(c, jack_port_name(out), "system:playback_1");
	printf("%s\n", a == 0 && b == 0 ? "connected" : "connect failed");
	fflush(stdout);

	char buf[64];
	while (read(0, buf, sizeof buf) > 0)
		;
	jack_client_close(c);
	return 0;
}
