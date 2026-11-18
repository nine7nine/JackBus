/*
 * A desktop shell's audio, as jackbus-pa sees it: two contexts in one
 * process. One is kept whatever happens to it (libcanberra's, for event
 * sounds); the other is a volume applet's, which, when its context fails,
 * drops it and connects anew every 500 ms (plasma-pa does every second).
 * Prints "ready" once both are connected, then waits for the server to go
 * away and come back: "reconnected: <n> sinks" once a new context lists the
 * server's sinks again (exit 0), or "stuck: ..." after 20 s (exit 1).
 */
#include <pulse/pulseaudio.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static pa_mainloop *ml;
static pa_mainloop_api *api;
static int sinks = -1;          /* the applet context's last listing */
static int failures;

static void count_sink(pa_context *c, const pa_sink_info *i, int eol, void *u)
{
	static int n;
	if (eol) {
		sinks = n;
		n = 0;
		return;
	}
	n++;
}

static pa_context *applet;

static void applet_state(pa_context *c, void *u)
{
	pa_context_state_t st = pa_context_get_state(c);
	if (st == PA_CONTEXT_READY) {
		pa_operation *o = pa_context_get_sink_info_list(c, count_sink, NULL);
		if (o)
			pa_operation_unref(o);
	} else if (st == PA_CONTEXT_FAILED) {
		failures++;
	}
}

static pa_context *connect_ctx(const char *name, pa_context_notify_cb_t cb)
{
	pa_context *c = pa_context_new(api, name);
	if (cb)
		pa_context_set_state_callback(c, cb, NULL);
	pa_context_connect(c, NULL, PA_CONTEXT_NOFLAGS, NULL);
	return c;
}

static double now(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

static void run_for(double s)
{
	double end = now() + s;
	while (now() < end) {
		if (pa_mainloop_prepare(ml, 20) < 0 || pa_mainloop_poll(ml) < 0 || pa_mainloop_dispatch(ml) < 0)
			break;
	}
}

int main(void)
{
	ml = pa_mainloop_new();
	api = pa_mainloop_get_api(ml);
	pa_context *kept = connect_ctx("event sounds", NULL);
	applet = connect_ctx("volume applet", applet_state);
	double t = now();
	while (now() - t < 10 && (pa_context_get_state(kept) != PA_CONTEXT_READY || sinks < 1))
		run_for(0.05);
	if (sinks < 1) {
		printf("not ready: %d sinks\n", sinks);
		return 1;
	}
	printf("ready: %d sinks\n", sinks);
	fflush(stdout);

	/* the server goes away (the script stops it, then starts it again) */
	t = now();
	while (now() - t < 20) {
		run_for(0.5);
		if (pa_context_get_state(applet) == PA_CONTEXT_FAILED ||
		    pa_context_get_state(applet) == PA_CONTEXT_TERMINATED) {
			pa_context_unref(applet);
			sinks = -1;
			applet = connect_ctx("volume applet", applet_state);
			continue;
		}
		if (failures > 0 && pa_context_get_state(applet) == PA_CONTEXT_READY && sinks >= 1) {
			printf("reconnected: %d sinks (the kept context: %s)\n", sinks,
					pa_context_get_state(kept) == PA_CONTEXT_FAILED ? "failed, still held" : "other");
			return 0;
		}
		if (failures > 0 && pa_context_get_state(applet) == PA_CONTEXT_READY) {
			/* connected, no sinks yet: list again */
			pa_operation *o = pa_context_get_sink_info_list(applet, count_sink, NULL);
			if (o)
				pa_operation_unref(o);
		}
	}
	printf("stuck: applet %d, %d sinks, %d failures\n", pa_context_get_state(applet), sinks, failures);
	return 1;
}
