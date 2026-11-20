/*
 * A level meter, as pavucontrol's and KDE's: a record stream with
 * PA_STREAM_PEAK_DETECT, float32 mono at 25 Hz, one float a fragment, on a
 * source by index. Prints "ready" once connected, then each second the
 * number of values received in it and their largest: "peaks <n> <max>", for
 * <seconds> (argument 1, default 4). With "glib" (argument 2) on PulseAudio's
 * GLib main loop (libpulse-mainloop-glib, as pavucontrol and Plasma), and at
 * the end the process's CPU time over the seconds: "cpu <percent>".
 */
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>
#include <glib.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static pa_mainloop *ml;
static pa_glib_mainloop *gml;
static int values;
static float top;

static void on_read(pa_stream *s, size_t n, void *u)
{
	const void *data;
	size_t len;
	while (pa_stream_readable_size(s) > 0) {
		if (pa_stream_peek(s, &data, &len) < 0)
			return;
		if (data == NULL) {
			if (len)
				pa_stream_drop(s);
			return;
		}
		for (size_t i = 0; i < len / sizeof(float); i++) {
			float v = ((const float *)data)[i];
			values++;
			if (v > top)
				top = v;
		}
		pa_stream_drop(s);
	}
}

static uint32_t source = PA_INVALID_INDEX;

static void on_source(pa_context *c, const pa_source_info *i, int eol, void *u)
{
	if (!eol && i && strstr(i->name, "system.capture"))
		source = i->index;
}

static double now(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

static void iterate(void)
{
	if (gml)
		g_main_context_iteration(NULL, TRUE);
	else
		pa_mainloop_iterate(ml, 1, NULL);
}

static gboolean tick(gpointer u)
{
	return G_SOURCE_CONTINUE;
}

static void run_until(double t)
{
	while (now() < t) {
		if (gml) {
			g_main_context_iteration(NULL, TRUE);
		} else if (pa_mainloop_prepare(ml, 20) < 0 || pa_mainloop_poll(ml) < 0 || pa_mainloop_dispatch(ml) < 0) {
			break;
		}
	}
}

static double cpu_seconds(void)
{
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

int main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 4;
	pa_mainloop_api *api;
	if (argc > 2 && strcmp(argv[2], "glib") == 0) {
		gml = pa_glib_mainloop_new(NULL);
		api = pa_glib_mainloop_get_api(gml);
		g_timeout_add(100, tick, NULL);		/* the waits below wake */
	} else {
		ml = pa_mainloop_new();
		api = pa_mainloop_get_api(ml);
	}
	pa_context *c = pa_context_new(api, "meter");
	pa_context_connect(c, NULL, PA_CONTEXT_NOFLAGS, NULL);
	double t0 = now();
	while (pa_context_get_state(c) != PA_CONTEXT_READY && now() - t0 < 5)
		iterate();
	pa_operation *o = pa_context_get_source_info_list(c, on_source, NULL);
	while (o && pa_operation_get_state(o) == PA_OPERATION_RUNNING)
		iterate();
	if (source == PA_INVALID_INDEX) {
		printf("no source\n");
		return 1;
	}
	pa_sample_spec ss = { .format = PA_SAMPLE_FLOAT32, .rate = 25, .channels = 1 };
	pa_buffer_attr attr;
	memset(&attr, 0, sizeof(attr));
	attr.fragsize = sizeof(float);
	attr.maxlength = (uint32_t)-1;
	pa_stream *s = pa_stream_new(c, "Peak detect", &ss, NULL);
	pa_stream_set_read_callback(s, on_read, NULL);
	char dev[16];
	snprintf(dev, sizeof(dev), "%u", source);
	if (pa_stream_connect_record(s, dev, &attr,
			PA_STREAM_DONT_MOVE | PA_STREAM_PEAK_DETECT | PA_STREAM_ADJUST_LATENCY) < 0) {
		printf("refused: %s\n", pa_strerror(pa_context_errno(c)));
		return 1;
	}
	t0 = now();
	while (pa_stream_get_state(s) == PA_STREAM_CREATING && now() - t0 < 5)
		iterate();
	if (pa_stream_get_state(s) != PA_STREAM_READY) {
		printf("refused: %s\n", pa_strerror(pa_context_errno(c)));
		return 1;
	}
	printf("ready\n");
	fflush(stdout);
	double cpu0 = cpu_seconds(), wall0 = now();
	for (int i = 0; i < seconds; i++) {
		values = 0;
		top = 0.0f;
		run_until(now() + 1.0);
		printf("peaks %d %.4f\n", values, top);
		fflush(stdout);
	}
	printf("cpu %.0f\n", 100.0 * (cpu_seconds() - cpu0) / (now() - wall0));
	return 0;
}
