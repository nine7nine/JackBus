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
 * tell the registries (core.c) to look again. The server going away kills the
 * node, not the process: what the application connects next gets a new node.
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
#include "../compat/compat_conf.h"

struct filter_list {
	size_t count;
	struct shim_member *members[];
};

/* one node: the live one, or a dead one (its server went away) that the
 * filters, streams and registries made on it still hold. A dead node is never
 * handed out again: the next reference opens a new one, and the dead one is
 * closed once its last holder lets go (each releases the node it got). */
struct gen {
	jackbus_node_t *node;
	int refs;
	_Atomic(struct filter_list *) list;
	atomic_ullong seq;		/* odd while a cycle runs */
	pthread_t events;
	int wake[2];			/* stops the event thread */
	bool events_running;
	bool active;
	atomic_bool dead;
	bool async;			/* the settings' pw.async, when the node opened */
	bool has_process;		/* app_process set: a realtime thread when active */
	struct gen *next;		/* the dead ones */
};

static struct {
	pthread_mutex_t lock;		/* non-realtime threads */
	struct gen *live;
	struct gen *dead;
	char name[JACK_CLIENT_NAME_SIZE_MAX];	/* asked for */
	char primary[256];			/* the first ports' node.name: no prefix */
	bool have_primary;
} app = { .lock = PTHREAD_MUTEX_INITIALIZER };

static int app_process(jackbus_node_t *node, const jackbus_cycle_t *cycle, void *arg)
{
	struct gen *g = arg;
	(void)node;
	atomic_fetch_add_explicit(&g->seq, 1, memory_order_seq_cst);
	shim_data_loop_run();
	struct filter_list *l = atomic_load_explicit(&g->list, memory_order_seq_cst);
	for (size_t i = 0; l && i < l->count; i++)
		l->members[i]->process(l->members[i], cycle);
	atomic_fetch_add_explicit(&g->seq, 1, memory_order_seq_cst);
	return 0;
}

/* under app.lock: the node n's (the live one or a dead one) */
static struct gen *find(jackbus_node_t *n)
{
	if (n == NULL)
		return NULL;
	if (app.live && app.live->node == n)
		return app.live;
	for (struct gen *g = app.dead; g; g = g->next)
		if (g->node == n)
			return g;
	return NULL;
}

static void retire_gen(struct gen *g, void *old)
{
	if (old == NULL)
		return;
	if (g == NULL) {
		free(old);		/* no node: no cycle reads it */
		return;
	}
	unsigned long long seq = atomic_load(&g->seq);
	for (int i = 0; (seq & 1) && i < 20000; i++) {		/* 2 s */
		usleep(100);
		if (atomic_load(&g->seq) != seq) {
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

/* after publishing a new array (filters, a filter's ports): free the old one
 * once no cycle of node n can still read it */
void shim_app_retire(jackbus_node_t *n, void *old)
{
	pthread_mutex_lock(&app.lock);
	struct gen *g = find(n);
	pthread_mutex_unlock(&app.lock);
	retire_gen(g, old);	/* a holder keeps g while it retires its own */
}

static void *event_thread(void *arg)
{
	struct gen *g = arg;
	struct pollfd p[2] = {
		{ jackbus_node_event_fd(g->node), POLLIN, 0 },
		{ g->wake[0], POLLIN, 0 },
	};
	for (;;) {
		if (poll(p, 2, -1) < 0 && errno != EINTR)
			break;
		if (p[1].revents)
			break;
		jackbus_event_t e;
		while (jackbus_node_next_event(g->node, &e) == 1) {
			if (e.type == JACKBUS_EVENT_CLIENT || e.type == JACKBUS_EVENT_PORT ||
			    e.type == JACKBUS_EVENT_CONNECT || e.type == JACKBUS_EVENT_RENAME ||
			    e.type == JACKBUS_EVENT_LOST) {
				shim_registry_changed();
				continue;
			}
			if (e.type != JACKBUS_EVENT_SHUTDOWN)
				continue;
			shim_log("the JACK server went away: %s", e.name);
			atomic_store(&g->dead, true);
			pthread_mutex_lock(&app.lock);
			if (g == app.live)
				shim_data_loop_set_running(false);
			struct filter_list *l = atomic_load(&g->list);
			for (size_t i = 0; l && i < l->count; i++)
				l->members[i]->shutdown(l->members[i]);
			pthread_mutex_unlock(&app.lock);
			shim_registry_changed();	/* its devices are gone */
		}
	}
	return NULL;
}

/* under app.lock */
static struct gen *open_gen(const char *name)
{
	jack_status_t status = 0;
	struct gen *g = calloc(1, sizeof(*g));
	if (g == NULL)
		return NULL;
	g->wake[0] = g->wake[1] = -1;
	g->node = jackbus_node_open(name, JackNoStartServer, &status);
	if (g->node == NULL) {
		shim_warn("can't open a JACK client \"%s\" on the JackBus socket (status 0x%x)", name, status);
		free(g);
		return NULL;
	}
	atomic_store(&g->list, calloc(1, sizeof(struct filter_list)));
	/* async audio streams (pw.async) need no realtime thread: the node gets one only
	 * for what runs in the cycle (shim_app_need_rt) */
	g->async = jackbus_compat_bool("async", "JACKBUS_PW_ASYNC", false);
	g->has_process = !g->async;
	if ((g->has_process && jackbus_node_set_process(g->node, app_process, g) != 0)
	    || pipe2(g->wake, O_CLOEXEC) != 0) {
		shim_warn("can't start the JACK client \"%s\"", name);
		jackbus_node_close(g->node);
		free(atomic_load(&g->list));
		free(g);
		return NULL;
	}
	g->events_running = pthread_create(&g->events, NULL, event_thread, g) == 0;
	shim_log("JACK client \"%s\" opened on the JackBus socket",
			jack_get_client_name(jackbus_node_client(g->node)));
	return g;
}

/* not under app.lock: no reference left */
static void close_gen(struct gen *g, bool was_live)
{
	if (g->events_running) {
		ssize_t r = write(g->wake[1], "x", 1);
		(void)r;
		pthread_join(g->events, NULL);
	}
	close(g->wake[0]);
	close(g->wake[1]);
	jackbus_node_close(g->node);		/* the realtime thread is gone after this */
	if (was_live)
		shim_data_loop_set_running(false);
	free(atomic_load(&g->list));
	free(g);
	shim_log("JACK client closed");
}

jackbus_node_t *shim_app_ref(const char *name)
{
	jackbus_node_t *node = NULL;

	pthread_mutex_lock(&app.lock);
	if (app.live && atomic_load(&app.live->dead)) {
		/* the server went away: set it aside for its last holders, open anew */
		app.live->next = app.dead;
		app.dead = app.live;
		app.live = NULL;
		app.have_primary = false;
	}
	if (app.live == NULL) {
		const char *env = getenv("JACKBUS_PW_CLIENT_NAME");
		if (env && env[0])
			name = env;
		snprintf(app.name, sizeof(app.name), "%s", name);
		app.live = open_gen(name);
	}
	if (app.live) {
		app.live->refs++;
		node = app.live->node;
	}
	pthread_mutex_unlock(&app.lock);
	return node;
}

void shim_app_unref(jackbus_node_t *n)
{
	struct gen *gone = NULL;
	bool was_live = false;

	pthread_mutex_lock(&app.lock);
	struct gen *g = find(n);
	if (g && g->refs > 0 && --g->refs == 0) {
		gone = g;
		if (g == app.live) {
			app.live = NULL;
			app.have_primary = false;
			was_live = true;
		} else {
			for (struct gen **p = &app.dead; *p; p = &(*p)->next) {
				if (*p == g) {
					*p = g->next;
					break;
				}
			}
		}
	}
	pthread_mutex_unlock(&app.lock);
	if (gone)
		close_gen(gone, was_live);
}

const char *shim_app_client_name(void)
{
	return app.live ? app.name : NULL;
}

jack_client_t *shim_app_client(void)
{
	pthread_mutex_lock(&app.lock);
	jack_client_t *c = app.live && !atomic_load(&app.live->dead) ? jackbus_node_client(app.live->node) : NULL;
	pthread_mutex_unlock(&app.lock);
	return c;
}

bool shim_app_async(jackbus_node_t *n)
{
	pthread_mutex_lock(&app.lock);
	struct gen *g = find(n);
	bool async = g && g->async;
	pthread_mutex_unlock(&app.lock);
	return async;
}

/* something that runs in the cycle (a filter, an RT_PROCESS or MIDI stream, or a stream
 * without an endpoint): the node gets its process function, and a realtime thread if it is
 * active already (JACK's path for a client activated without one) */
void shim_app_need_rt(jackbus_node_t *n)
{
	pthread_mutex_lock(&app.lock);
	struct gen *g = find(n);
	if (g && !g->has_process) {
		if (jackbus_node_set_process(g->node, app_process, g) == 0)
			g->has_process = true;
		else
			shim_warn("can't give the JACK client \"%s\" a realtime thread", app.name);
	}
	pthread_mutex_unlock(&app.lock);
}

/* under app.lock: in the graph's cycle while something runs in it */
static void set_active(struct gen *g, bool active)
{
	if (active == g->active)
		return;
	if (active) {
		if (jackbus_node_activate(g->node) != 0) {
			shim_warn("can't activate the JACK client \"%s\"", app.name);
			return;
		}
		if (g == app.live)
			shim_data_loop_set_running(true);
	} else {
		jackbus_node_deactivate(g->node);
		if (g == app.live)
			shim_data_loop_set_running(false);	/* no cycle runs from here */
	}
	g->active = active;
}

void shim_app_attach(jackbus_node_t *n, struct shim_member *f)
{
	pthread_mutex_lock(&app.lock);
	struct gen *g = find(n);
	struct filter_list *old = g ? atomic_load(&g->list) : NULL;
	size_t count = old ? old->count : 0;
	struct filter_list *l = g ? calloc(1, sizeof(*l) + (count + 1) * sizeof(l->members[0])) : NULL;
	if (l) {
		if (count)
			memcpy(l->members, old->members, count * sizeof(l->members[0]));
		l->members[count] = f;
		l->count = count + 1;
		atomic_store(&g->list, l);
		retire_gen(g, old);
		set_active(g, true);
	}
	pthread_mutex_unlock(&app.lock);
}

void shim_app_detach(jackbus_node_t *n, struct shim_member *f)
{
	pthread_mutex_lock(&app.lock);
	struct gen *g = find(n);
	struct filter_list *old = g ? atomic_load(&g->list) : NULL;
	if (old) {
		struct filter_list *l = calloc(1, sizeof(*l) + old->count * sizeof(l->members[0]));
		if (l) {
			for (size_t i = 0; i < old->count; i++)
				if (old->members[i] != f)
					l->members[l->count++] = old->members[i];
			atomic_store(&g->list, l);
			retire_gen(g, old);	/* from here no cycle runs f */
			if (l->count == 0) {
				set_active(g, false);
				if (g == app.live)
					app.have_primary = false;
			}
		}
	}
	pthread_mutex_unlock(&app.lock);
}

/* the process latency of the client: the largest any filter declared */
void shim_app_update_latency(jackbus_node_t *n)
{
	pthread_mutex_lock(&app.lock);
	struct gen *g = find(n);
	jack_nframes_t frames = 0;
	struct filter_list *l = g ? atomic_load(&g->list) : NULL;
	for (size_t i = 0; l && i < l->count; i++) {
		jack_nframes_t f = l->members[i]->latency(l->members[i]);
		if (f > frames)
			frames = f;
	}
	if (g)
		jackbus_node_set_process_latency(g->node, frames);
	pthread_mutex_unlock(&app.lock);
}

/* the first filter or stream to name ports names them plainly (whatever the
 * client is called: an application that listed devices first has its
 * program's name); a later one of another node.name prefixes them */
void shim_app_port_name(const char *node_name, const char *port, char *buf, size_t size)
{
	const char *n = node_name ? node_name : "";
	pthread_mutex_lock(&app.lock);
	if (!app.have_primary) {
		snprintf(app.primary, sizeof(app.primary), "%s", n);
		app.have_primary = true;
	}
	bool plain = strcmp(n, app.primary) == 0 || strcmp(n, app.name) == 0 || n[0] == 0;
	pthread_mutex_unlock(&app.lock);
	if (plain)
		snprintf(buf, size, "%s", port);
	else
		snprintf(buf, size, "%s/%s", n, port);
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
