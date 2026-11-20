/*
 * Chromium's way: each sound stream on a context of its own, in one process
 * (one JackBus node), all named alike ("Playback"). Opens <n> contexts
 * (argument 1, default 3), a float32 stereo playback stream on each, and
 * prints "ready <n>/<n>" with how many reached PA_STREAM_READY, then holds
 * them for a second; exit 0 if all did.
 */
#include <pulse/pulseaudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static double now(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 3;
	pa_mainloop *ml = pa_mainloop_new();
	pa_mainloop_api *api = pa_mainloop_get_api(ml);
	pa_context *c[16];
	pa_stream *s[16];
	pa_sample_spec ss = { .format = PA_SAMPLE_FLOAT32LE, .rate = 48000, .channels = 2 };
	for (int i = 0; i < n && i < 16; i++) {
		c[i] = pa_context_new(api, "Chromium");
		pa_context_connect(c[i], NULL, PA_CONTEXT_NOFLAGS, NULL);
		double t = now();
		while (pa_context_get_state(c[i]) != PA_CONTEXT_READY && now() - t < 5)
			pa_mainloop_iterate(ml, 1, NULL);
		s[i] = pa_stream_new(c[i], "Playback", &ss, NULL);
		pa_stream_connect_playback(s[i], NULL, NULL, PA_STREAM_NOFLAGS, NULL, NULL);
		t = now();
		while (pa_stream_get_state(s[i]) == PA_STREAM_CREATING && now() - t < 5)
			pa_mainloop_iterate(ml, 1, NULL);
	}
	int ready = 0;
	for (int i = 0; i < n && i < 16; i++)
		ready += pa_stream_get_state(s[i]) == PA_STREAM_READY;
	printf("ready %d/%d\n", ready, n);
	fflush(stdout);
	double t = now();
	while (now() - t < 1.0)
		pa_mainloop_iterate(ml, 0, NULL), pa_mainloop_prepare(ml, 20), pa_mainloop_poll(ml), pa_mainloop_dispatch(ml);
	return ready == n ? 0 : 1;
}
