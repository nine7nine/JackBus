/*
 * pw-twofilter: two pw_filters in one PipeWire application, "alpha" and
 * "beta", each a pass-through, on one pw_main_loop. Under jackbus-pw they
 * must be one JACK client ("alpha", the first one's node.name) with four
 * ports: alpha's own (input, output) and beta's prefixed (beta/input,
 * beta/output); both process callbacks must run.
 *
 *   pw-twofilter [seconds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>

struct one {
	struct pw_filter *filter;
	void *in, *out;
	unsigned long cycles;
};

static struct pw_main_loop *loop;

static void on_process(void *userdata, struct spa_io_position *position)
{
	struct one *o = userdata;
	uint32_t n = position->clock.duration;
	float *in = pw_filter_get_dsp_buffer(o->in, n);
	float *out = pw_filter_get_dsp_buffer(o->out, n);
	if (in && out)
		memcpy(out, in, n * sizeof(float));
	o->cycles++;
}

static const struct pw_filter_events events = {
	PW_VERSION_FILTER_EVENTS,
	.process = on_process,
};

static void make(struct one *o, const char *name)
{
	o->filter = pw_filter_new_simple(pw_main_loop_get_loop(loop), name,
			pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Filter",
				PW_KEY_NODE_NAME, name, NULL), &events, o);
	o->in = pw_filter_add_port(o->filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
			pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME, "input", NULL), NULL, 0);
	o->out = pw_filter_add_port(o->filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
			pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME, "output", NULL), NULL, 0);
	pw_filter_connect(o->filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0);
}

static void on_quit(void *data, int sig) { (void)data; (void)sig; pw_main_loop_quit(loop); }

int main(int argc, char *argv[])
{
	struct one a = { 0 }, b = { 0 };
	pw_init(&argc, &argv);
	loop = pw_main_loop_new(NULL);
	pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGINT, on_quit, NULL);
	pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGTERM, on_quit, NULL);
	make(&a, "alpha");
	make(&b, "beta");
	pw_main_loop_run(loop);
	printf("twofilter: alpha %lu cycles, beta %lu cycles\n", a.cycles, b.cycles);
	pw_filter_destroy(b.filter);
	pw_filter_destroy(a.filter);
	pw_main_loop_destroy(loop);
	pw_deinit();
	return (a.cycles > 0 && b.cycles > 0) ? 0 : 1;
}
