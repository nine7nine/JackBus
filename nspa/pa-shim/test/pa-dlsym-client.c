/*
 * A client that gets pa_context_new with dlsym, as slimcore does: under
 * dlsym-hook.so (teams-sdl-cef's audio probe), the wrapper it is handed calls
 * "the real one" through libpulse.so.0. Prints "ready: <server>" once the
 * context is ready.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

#include <pulse/pulseaudio.h>

int main(void)
{
	pa_context *(*cnew)(pa_mainloop_api *, const char *) =
		(pa_context *(*)(pa_mainloop_api *, const char *))dlsym(RTLD_DEFAULT, "pa_context_new");
	pa_mainloop *m = pa_mainloop_new();
	pa_context *c = cnew ? cnew(pa_mainloop_get_api(m), "dlsym-client") : NULL;
	if (c == NULL || pa_context_connect(c, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
		printf("no context\n");
		return 1;
	}
	for (int i = 0; i < 500; i++) {
		pa_context_state_t s = pa_context_get_state(c);
		if (s == PA_CONTEXT_READY) {
			printf("ready: %s\n", pa_context_get_server(c));
			pa_context_disconnect(c);
			return 0;
		}
		if (!PA_CONTEXT_IS_GOOD(s)) {
			printf("failed: %s\n", pa_strerror(pa_context_errno(c)));
			return 1;
		}
		pa_mainloop_iterate(m, 0, NULL);
		usleep(10000);
	}
	printf("timed out\n");
	return 1;
}
