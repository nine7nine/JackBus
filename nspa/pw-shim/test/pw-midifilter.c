/*
 * jackbus-pw test: a pw_filter with a MIDI input and a MIDI output ("8 bit raw
 * midi" DSP ports), linked against the system's libpipewire. Every event in
 * goes out at its offset, notes a semitone up (so the output is provably the
 * filter's). Also checks the data loop: after a second, a blocking
 * pw_loop_invoke() on pw_filter_get_data_loop() from the main thread must run
 * in the thread that runs process.
 *
 *   pw-midifilter <seconds>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/control/control.h>

struct data {
	struct pw_main_loop *loop;
	struct pw_filter *filter;
	void *in, *out;
	pthread_t process_thread, invoke_thread;
	bool invoked;
	int events, cycles;
	int seconds;
	struct spa_source *timer;
	int ticks;
};

static void on_process(void *userdata, struct spa_io_position *position)
{
	struct data *d = userdata;
	struct pw_buffer *bi = pw_filter_dequeue_buffer(d->in);
	struct pw_buffer *bo = pw_filter_dequeue_buffer(d->out);

	d->process_thread = pthread_self();	/* no system call in process */
	if (d->cycles == 10 && getenv("MIDIFILTER_RT_SYSCALL") && getenv("MIDIFILTER_RT_SYSCALL")[0])
		getppid();		/* the negative control of the RT trace */
	d->cycles++;
	if (bo == NULL) {
		if (bi)
			pw_filter_queue_buffer(d->in, bi);
		return;
	}
	struct spa_data *od = &bo->buffer->datas[0];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(od->data, od->maxsize);
	struct spa_pod_frame f;
	spa_pod_builder_push_sequence(&b, &f, 0);
	if (bi) {
		struct spa_data *id = &bi->buffer->datas[0];
		struct spa_pod *pod = spa_pod_from_data(id->data, id->maxsize, id->chunk->offset, id->chunk->size);
		if (pod && spa_pod_is_sequence(pod)) {
			struct spa_pod_control *c;
			SPA_POD_SEQUENCE_FOREACH((struct spa_pod_sequence *)pod, c) {
				if (c->type != SPA_CONTROL_Midi)
					continue;
				uint8_t m[3];
				uint32_t sz = SPA_MIN(SPA_POD_BODY_SIZE(&c->value), 3u);
				memcpy(m, SPA_POD_BODY(&c->value), sz);
				if (sz == 3 && (m[0] & 0xe0) == 0x80)
					m[1]++;
				spa_pod_builder_control(&b, c->offset, SPA_CONTROL_Midi);
				spa_pod_builder_bytes(&b, m, sz);
				d->events++;
			}
		}
		pw_filter_queue_buffer(d->in, bi);
	}
	spa_pod_builder_pop(&b, &f);
	od->chunk->offset = 0;
	od->chunk->size = b.state.offset;
	od->chunk->stride = 1;
	pw_filter_queue_buffer(d->out, bo);
}

static int do_invoke(struct spa_loop *loop, bool async, uint32_t seq, const void *data,
		size_t size, void *user_data)
{
	struct data *d = user_data;
	d->invoke_thread = pthread_self();
	d->invoked = true;
	return 42;
}

static void on_timer(void *userdata, uint64_t expirations)
{
	struct data *d = userdata;
	if (++d->ticks == 1) {
		int res = pw_loop_invoke(pw_filter_get_data_loop(d->filter), do_invoke, 1, NULL, 0, true, d);
		printf("invoke: returned %d, ran in the process thread: %s\n", res,
				d->invoked && pthread_equal(d->invoke_thread, d->process_thread) ? "yes" : "no");
		fflush(stdout);
	}
	if (d->ticks >= d->seconds)
		pw_main_loop_quit(d->loop);
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.process = on_process,
};

int main(int argc, char *argv[])
{
	struct data d = { 0 };
	d.seconds = argc > 1 ? atoi(argv[1]) : 3;

	pw_init(&argc, &argv);
	d.loop = pw_main_loop_new(NULL);
	d.filter = pw_filter_new_simple(pw_main_loop_get_loop(d.loop), "midifilter",
			pw_properties_new(PW_KEY_MEDIA_TYPE, "Midi", PW_KEY_MEDIA_CATEGORY, "Filter",
				PW_KEY_MEDIA_ROLE, "DSP", NULL),
			&filter_events, &d);
	d.in = pw_filter_add_port(d.filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
			pw_properties_new(PW_KEY_FORMAT_DSP, "8 bit raw midi", PW_KEY_PORT_NAME, "midi_in", NULL),
			NULL, 0);
	d.out = pw_filter_add_port(d.filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
			pw_properties_new(PW_KEY_FORMAT_DSP, "8 bit raw midi", PW_KEY_PORT_NAME, "midi_out", NULL),
			NULL, 0);
	if (d.in == NULL || d.out == NULL || pw_filter_connect(d.filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0) {
		fprintf(stderr, "can't connect the filter\n");
		return 1;
	}
	d.timer = pw_loop_add_timer(pw_main_loop_get_loop(d.loop), on_timer, &d);
	struct timespec one = { 1, 0 };
	pw_loop_update_timer(pw_main_loop_get_loop(d.loop), d.timer, &one, &one, false);
	pw_main_loop_run(d.loop);
	printf("midifilter: %d cycles, %d events passed\n", d.cycles, d.events);
	pw_filter_destroy(d.filter);
	pw_main_loop_destroy(d.loop);
	pw_deinit();
	return 0;
}
