/*
 * jackbus-pw: the application's JACK client.
 *
 * One JACK client per process, as a JACK application has: a node of the
 * native JackBus API (jack/jackbus.h), opened at the first reference (a
 * pw_filter or pw_stream connecting, or a registry listing JackBus's devices)
 * and closed with the last. It is active (in the graph's cycle) only while a
 * filter or stream is connected: an application that only enumerates devices
 * costs the graph nothing. Every filter and stream is a group of ports on
 * it, and the node's process function runs each in turn, in the JACK
 * realtime thread, after the data loop's queued invokes (loop.c).
 *
 * The list of connected filters and streams is published to that thread the way the node
 * publishes its port table: a new array, an atomic pointer, and the old array
 * freed once no cycle can still be reading it (a cycle sequence, odd while a
 * cycle runs). The realtime thread never waits.
 *
 * The node's events are read by a small non-realtime thread: a SHUTDOWN puts
 * every filter in the error state, on its own loop; client and port changes
 * tell the registries (core.c) to look again.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>

#define JACK_CLIENT_NAME_SIZE_MAX 128

#include "shim.h"

struct filter_list {
	size_t count;
	struct shim_member *members[];
};

static struct {
	pthread_mutex_t lock;		/* non-realtime threads */
	jackbus_node_t *node;
	int refs;
	_Atomic(struct filter_list *) list;
	atomic_ullong seq;		/* odd while a cycle runs */
	pthread_t events;
	int wake[2];			/* stops the event thread */
	bool events_running;
	bool active;
	char name[JACK_CLIENT_NAME_SIZE_MAX];	/* asked for */
} app = { .lock = PTHREAD_MUTEX_INITIALIZER, .wake = { -1, -1 } };

static int app_process(jackbus_node_t *node, const jackbus_cycle_t *cycle, void *arg)
{
	(void)node; (void)arg;
	atomic_fetch_add_explicit(&app.seq, 1, memory_order_seq_cst);
	shim_data_loop_run();
	struct filter_list *l = atomic_load_explicit(&app.list, memory_order_seq_cst);
	for (size_t i = 0; l && i < l->count; i++)
		l->members[i]->process(l->members[i], cycle);
	atomic_fetch_add_explicit(&app.seq, 1, memory_order_seq_cst);
	return 0;
}

/* after publishing a new array (filters, a filter's ports): free the old one
 * once no cycle can still read it */
void shim_app_retire(void *old)
{
	if (old == NULL)
		return;
	unsigned long long seq = atomic_load(&app.seq);
	for (int i = 0; (seq & 1) && i < 20000; i++) {		/* 2 s */
		usleep(100);
		if (atomic_load(&app.seq) != seq) {
			seq = 0;
			break;
		}
	}
	if (seq & 1) {
		shim_warn("a process cycle has run for 2 s: an old table is kept");
		return;
	}
	free(old);
}

static void *event_thread(void *arg)
{
	(void)arg;
	struct pollfd p[2] = {
		{ jackbus_node_event_fd(app.node), POLLIN, 0 },
		{ app.wake[0], POLLIN, 0 },
	};
	for (;;) {
		if (poll(p, 2, -1) < 0 && errno != EINTR)
			break;
		if (p[1].revents)
			break;
		jackbus_event_t e;
		while (jackbus_node_next_event(app.node, &e) == 1) {
			if (e.type == JACKBUS_EVENT_CLIENT || e.type == JACKBUS_EVENT_PORT ||
			    e.type == JACKBUS_EVENT_RENAME || e.type == JACKBUS_EVENT_LOST) {
				shim_registry_changed();
				continue;
			}
			if (e.type != JACKBUS_EVENT_SHUTDOWN)
				continue;
			shim_log("the JACK server went away: %s", e.name);
			shim_data_loop_set_running(false);
			pthread_mutex_lock(&app.lock);
			struct filter_list *l = atomic_load(&app.list);
			for (size_t i = 0; l && i < l->count; i++)
				l->members[i]->shutdown(l->members[i]);
			pthread_mutex_unlock(&app.lock);
		}
	}
	return NULL;
}

jackbus_node_t *shim_app_ref(const char *name)
{
	jackbus_node_t *node = NULL;
	jack_status_t status = 0;

	pthread_mutex_lock(&app.lock);
	if (app.node == NULL) {
		const char *env = getenv("JACKBUS_PW_CLIENT_NAME");
		if (env && env[0])
			name = env;
		snprintf(app.name, sizeof(app.name), "%s", name);
		app.node = jackbus_node_open(name, JackNoStartServer, &status);
		if (app.node == NULL) {
			shim_warn("can't open a JACK client \"%s\" on the JackBus socket (status 0x%x)", name, status);
			goto out;
		}
		struct filter_list *l = calloc(1, sizeof(*l));
		atomic_store(&app.list, l);
		if (jackbus_node_set_process(app.node, app_process, NULL) != 0
		    || pipe2(app.wake, O_CLOEXEC) != 0) {
			shim_warn("can't start the JACK client \"%s\"", name);
			jackbus_node_close(app.node);
			app.node = NULL;
			free(atomic_exchange(&app.list, NULL));
			goto out;
		}
		app.events_running = pthread_create(&app.events, NULL, event_thread, NULL) == 0;
		shim_log("JACK client \"%s\" opened on the JackBus socket",
				jack_get_client_name(jackbus_node_client(app.node)));
	}
	app.refs++;
	node = app.node;
out:
	pthread_mutex_unlock(&app.lock);
	return node;
}

void shim_app_unref(void)
{
	jackbus_node_t *node = NULL;

	pthread_mutex_lock(&app.lock);
	if (app.refs > 0 && --app.refs == 0) {
		node = app.node;
		app.node = NULL;
	}
	pthread_mutex_unlock(&app.lock);
	if (node == NULL)
		return;
	if (app.events_running) {
		ssize_t r = write(app.wake[1], "x", 1);
		(void)r;
		pthread_join(app.events, NULL);
		app.events_running = false;
	}
	close(app.wake[0]);
	close(app.wake[1]);
	app.wake[0] = app.wake[1] = -1;
	app.active = false;
	jackbus_node_close(node);		/* the realtime thread is gone after this */
	shim_data_loop_set_running(false);
	free(atomic_exchange(&app.list, NULL));
	shim_log("JACK client closed");
}

const char *shim_app_client_name(void)
{
	return app.node ? app.name : NULL;
}

jack_client_t *shim_app_client(void)
{
	return app.node ? jackbus_node_client(app.node) : NULL;
}

/* under app.lock: in the graph's cycle while something runs in it */
static void set_active(bool active)
{
	if (app.node == NULL || active == app.active)
		return;
	if (active) {
		if (jackbus_node_activate(app.node) != 0) {
			shim_warn("can't activate the JACK client \"%s\"", app.name);
			return;
		}
		shim_data_loop_set_running(true);
	} else {
		jackbus_node_deactivate(app.node);
		shim_data_loop_set_running(false);	/* no cycle runs from here */
	}
	app.active = active;
}

void shim_app_attach(struct shim_member *f)
{
	pthread_mutex_lock(&app.lock);
	struct filter_list *old = atomic_load(&app.list);
	size_t n = old ? old->count : 0;
	struct filter_list *l = calloc(1, sizeof(*l) + (n + 1) * sizeof(l->members[0]));
	if (l) {
		if (n)
			memcpy(l->members, old->members, n * sizeof(l->members[0]));
		l->members[n] = f;
		l->count = n + 1;
		atomic_store(&app.list, l);
		shim_app_retire(old);
		set_active(true);
	}
	pthread_mutex_unlock(&app.lock);
}

void shim_app_detach(struct shim_member *f)
{
	pthread_mutex_lock(&app.lock);
	struct filter_list *old = atomic_load(&app.list);
	if (old) {
		struct filter_list *l = calloc(1, sizeof(*l) + old->count * sizeof(l->members[0]));
		if (l) {
			for (size_t i = 0; i < old->count; i++)
				if (old->members[i] != f)
					l->members[l->count++] = old->members[i];
			atomic_store(&app.list, l);
			shim_app_retire(old);	/* from here no cycle runs f */
			if (l->count == 0)
				set_active(false);
		}
	}
	pthread_mutex_unlock(&app.lock);
}

/* the process latency of the client: the largest any filter declared */
void shim_app_update_latency(void)
{
	pthread_mutex_lock(&app.lock);
	jack_nframes_t frames = 0;
	struct filter_list *l = atomic_load(&app.list);
	for (size_t i = 0; l && i < l->count; i++) {
		jack_nframes_t f = l->members[i]->latency(l->members[i]);
		if (f > frames)
			frames = f;
	}
	if (app.node)
		jackbus_node_set_process_latency(app.node, frames);
	pthread_mutex_unlock(&app.lock);
}

void shim_app_port_name(const char *node_name, const char *port, char *buf, size_t size)
{
	const char *client = shim_app_client_name();
	if (node_name && client && strcmp(node_name, client) != 0)
		snprintf(buf, size, "%s/%s", node_name, port);
	else
		snprintf(buf, size, "%s", port);
}

/* spa_io_position from the node's cycle record (realtime thread) */
void shim_fill_position(struct spa_io_position *pos, const jackbus_cycle_t *c, jack_client_t *client)
{
	struct spa_io_clock *clk = &pos->clock;
	struct spa_io_segment *seg = &pos->segments[0];

	clk->flags = (c->flags & JACKBUS_CYCLE_FREEWHEEL) ? SPA_IO_CLOCK_FLAG_FREEWHEEL : 0;
	clk->nsec = c->time_ns;
	clk->next_nsec = c->next_ns;
	clk->rate = SPA_FRACTION(1, c->sample_rate);
	clk->position = c->frame;
	clk->duration = c->nframes;
	clk->delay = 0;
	clk->rate_diff = c->rate_diff;
	clk->target_rate = clk->rate;
	clk->target_duration = c->nframes;
	clk->cycle++;
	clk->xrun = c->xruns;

	pos->offset = 0;
	pos->state = c->transport_state == JackTransportRolling ? SPA_IO_POSITION_STATE_RUNNING :
		c->transport_state == JackTransportStarting ? SPA_IO_POSITION_STATE_STARTING :
		SPA_IO_POSITION_STATE_STOPPED;
	pos->n_segments = 1;
	seg->flags = 0;
	seg->start = c->frame;
	seg->duration = 0;
	seg->rate = c->transport_state == JackTransportRolling ? 1.0 : 0.0;
	seg->position = c->transport_frame;
	spa_zero(seg->bar);
	jack_position_t tp;
	jack_transport_query(client, &tp);		/* realtime-safe: shared memory */
	if (tp.valid & JackPositionBBT) {
		seg->bar.flags = SPA_IO_SEGMENT_BAR_FLAG_VALID;
		seg->bar.signature_num = tp.beats_per_bar;
		seg->bar.signature_denom = tp.beat_type;
		seg->bar.bpm = tp.beats_per_minute;
		seg->bar.beat = (tp.bar - 1) * tp.beats_per_bar + (tp.beat - 1) +
			(tp.ticks_per_beat > 0 ? tp.tick / tp.ticks_per_beat : 0);
		seg->bar.bar_start_tick = tp.bar_start_tick;
		seg->bar.ticks_per_beat = tp.ticks_per_beat;
	}
}
