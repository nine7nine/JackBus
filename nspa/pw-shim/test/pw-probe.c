/*
 * pw-probe: a PipeWire filter application, built against the system's
 * libpipewire-0.3, that asks for 128 frames at 48 kHz and reports what the
 * graph actually gives it. It uses a pw_thread_loop, an explicit
 * pw_context_connect() with a core round trip, and pw_filter_new() on that
 * core: the paths audio-dsp-filter doesn't take.
 *
 *   pw-probe [seconds]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>

#include <spa/param/format-utils.h>
#include <spa/param/audio/format.h>

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>

struct data {
	struct pw_thread_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct spa_hook core_listener;
	struct pw_filter *filter;
	struct spa_hook filter_listener;
	void *in, *out;
	int pending;
	int synced;
	int streaming;
	int formats;
	int saw_position_io;

	/* written by process (JACK thread), read after the run */
	uint64_t cycles;
	uint32_t rate;
	uint64_t dur_min, dur_max;
	uint64_t first_nsec, last_nsec;
	uint64_t first_pos, last_pos;
	double rate_diff;
	uint64_t null_buffers;
	int policy, priority;
	char thread[16];
};

static void on_core_done(void *userdata, uint32_t id, int seq)
{
	struct data *d = userdata;
	if (id == PW_ID_CORE && seq == d->pending) {
		d->synced = 1;
		pw_thread_loop_signal(d->loop, false);
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done,
};

static void on_state_changed(void *userdata, enum pw_filter_state old,
		enum pw_filter_state state, const char *error)
{
	struct data *d = userdata;
	printf("probe: state %s -> %s%s%s\n", pw_filter_state_as_string(old),
			pw_filter_state_as_string(state), error ? ": " : "", error ? error : "");
	if (state == PW_FILTER_STATE_STREAMING)
		d->streaming = 1;
}

static void on_io_changed(void *userdata, void *port_data, uint32_t id, void *area, uint32_t size)
{
	struct data *d = userdata;
	if (port_data == NULL && id == SPA_IO_Position && area != NULL) {
		struct spa_io_position *pos = area;
		d->saw_position_io = 1;
		printf("probe: io_changed Position: rate 1/%u, duration %lu\n",
				pos->clock.rate.denom, (unsigned long)pos->clock.duration);
	}
}

static void on_param_changed(void *userdata, void *port_data, uint32_t id, const struct spa_pod *param)
{
	struct data *d = userdata;
	uint32_t media_type, media_subtype;

	if (id != SPA_PARAM_Format || param == NULL || port_data == NULL)
		return;
	if (spa_format_parse(param, &media_type, &media_subtype) >= 0 &&
	    media_type == SPA_MEDIA_TYPE_audio && media_subtype == SPA_MEDIA_SUBTYPE_dsp) {
		d->formats++;
		printf("probe: %s port format: audio/dsp (f32 planar)\n",
				port_data == d->in ? "input" : "output");
	}
}

static void on_process(void *userdata, struct spa_io_position *position)
{
	struct data *d = userdata;
	uint32_t n = position->clock.duration;
	float *in = pw_filter_get_dsp_buffer(d->in, n);
	float *out = pw_filter_get_dsp_buffer(d->out, n);

	if (in == NULL || out == NULL) {
		d->null_buffers++;
		return;
	}
	memcpy(out, in, n * sizeof(float));

	if (d->cycles == 0) {
		struct sched_param sp;
		pthread_getschedparam(pthread_self(), &d->policy, &sp);
		d->priority = sp.sched_priority;
		pthread_getname_np(pthread_self(), d->thread, sizeof(d->thread));
		d->first_nsec = position->clock.nsec;
		d->first_pos = position->clock.position;
		d->dur_min = d->dur_max = n;
	}
	d->rate = position->clock.rate.denom;
	d->dur_min = SPA_MIN(d->dur_min, (uint64_t)n);
	d->dur_max = SPA_MAX(d->dur_max, (uint64_t)n);
	d->last_nsec = position->clock.nsec;
	d->last_pos = position->clock.position;
	d->rate_diff = position->clock.rate_diff;
	d->cycles++;
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.state_changed = on_state_changed,
	.io_changed = on_io_changed,
	.param_changed = on_param_changed,
	.process = on_process,
};

int main(int argc, char *argv[])
{
	struct data d = { 0 };
	int seconds = argc > 1 ? atoi(argv[1]) : 2;
	int ok;

	pw_init(&argc, &argv);
	printf("probe: libpipewire %s (built against %s)\n",
			pw_get_library_version(), pw_get_headers_version());

	d.loop = pw_thread_loop_new("probe-loop", NULL);
	pw_thread_loop_lock(d.loop);
	pw_thread_loop_start(d.loop);

	d.context = pw_context_new(pw_thread_loop_get_loop(d.loop), NULL, 0);
	d.core = pw_context_connect(d.context, NULL, 0);
	if (d.core == NULL) {
		fprintf(stderr, "probe: can't connect\n");
		return 1;
	}
	pw_core_add_listener(d.core, &d.core_listener, &core_events, &d);
	d.pending = pw_core_sync(d.core, PW_ID_CORE, 0);
	while (!d.synced)
		pw_thread_loop_wait(d.loop);
	printf("probe: core round trip done\n");

	d.filter = pw_filter_new(d.core, "pw-probe",
			pw_properties_new(
				PW_KEY_MEDIA_TYPE, "Audio",
				PW_KEY_MEDIA_CATEGORY, "Filter",
				PW_KEY_MEDIA_ROLE, "DSP",
				PW_KEY_NODE_LATENCY, "128/48000",
				PW_KEY_NODE_RATE, "1/48000",
				NULL));
	pw_filter_add_listener(d.filter, &d.filter_listener, &filter_events, &d);
	d.in = pw_filter_add_port(d.filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
			pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
				PW_KEY_PORT_NAME, "input", NULL), NULL, 0);
	d.out = pw_filter_add_port(d.filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
			pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
				PW_KEY_PORT_NAME, "output", NULL), NULL, 0);
	if (pw_filter_connect(d.filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0) {
		fprintf(stderr, "probe: can't connect the filter\n");
		return 1;
	}
	pw_thread_loop_unlock(d.loop);

	sleep(seconds);

	pw_thread_loop_lock(d.loop);
	pw_filter_destroy(d.filter);
	pw_thread_loop_unlock(d.loop);
	pw_thread_loop_stop(d.loop);
	pw_core_disconnect(d.core);
	pw_context_destroy(d.context);
	pw_thread_loop_destroy(d.loop);

	printf("probe: asked for 128 frames at 48000 Hz (node.latency=128/48000, node.rate=1/48000)\n");
	if (d.cycles > 1) {
		double period_us = (d.last_nsec - d.first_nsec) / 1e3 / (d.cycles - 1);
		double frames_per_cycle = (double)(d.last_pos - d.first_pos) / (d.cycles - 1);
		printf("probe: got %u Hz, %lu..%lu frames per cycle, %lu cycles\n", d.rate,
				(unsigned long)d.dur_min, (unsigned long)d.dur_max, (unsigned long)d.cycles);
		printf("probe: process runs in thread \"%s\", %s priority %d\n", d.thread,
				d.policy == SCHED_FIFO ? "SCHED_FIFO" : d.policy == SCHED_RR ? "SCHED_RR" : "SCHED_OTHER",
				d.priority);
		printf("probe: clock: %.1f us and %.1f frames per cycle (expected %.1f us), rate_diff %.6f\n",
				period_us, frames_per_cycle, d.dur_max * 1e6 / d.rate, d.rate_diff);
	}
	ok = d.streaming && d.formats == 2 && d.saw_position_io && d.cycles > 1 && d.null_buffers == 0;
	printf("probe: %s\n", ok ? "PASS" : "FAIL");
	pw_deinit();
	return ok ? 0 : 1;
}
