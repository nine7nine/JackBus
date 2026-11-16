/*
 * A second "sound card" for the tests: a JACK client with two physical
 * playback ports (playback_1, playback_2) and two physical capture ports,
 * as a bridged card has. Runs until killed.
 *
 *   fake-card [name]
 */
#include <stdio.h>
#include <unistd.h>

#include <jack/jack.h>

int main(int argc, char **argv)
{
	jack_client_t *c = jack_client_open(argc > 1 ? argv[1] : "fakecard", JackNoStartServer, NULL);
	if (c == NULL)
		return 1;
	for (int i = 1; i <= 2; i++) {
		char n[32];
		snprintf(n, sizeof(n), "playback_%d", i);
		jack_port_register(c, n, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput | JackPortIsPhysical | JackPortIsTerminal, 0);
		snprintf(n, sizeof(n), "capture_%d", i);
		jack_port_register(c, n, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput | JackPortIsPhysical | JackPortIsTerminal, 0);
	}
	if (jack_activate(c) != 0)
		return 1;
	printf("ready\n");
	fflush(stdout);
	for (;;)
		pause();
}
