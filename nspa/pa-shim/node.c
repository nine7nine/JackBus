/*
 * jackbus-pa: the application's JackBus node, one per process, as a JACK
 * application has one client: opened by the first context to connect (to
 * read JACK's devices), closed with the last; in the graph's cycle only while
 * a stream is connected. Its process function runs each connected stream in
 * turn in the JACK realtime thread. The list of streams is published to that
 * thread as a new array behind an atomic pointer; the old one is freed once no
 * cycle can still read it (a cycle sequence, odd while a cycle runs). The
 * realtime thread never waits and makes no system call.
 *
 * The node's events are read by a small thread: client and port changes, and
 * the server's shutdown, wake the eventfds the contexts watch.
 *
 * The server going away kills the node, not the process: its contexts and
 * streams fail, and the application connects again (a volume applet retries
 * every second). A dead node is never handed out again: the next reference
 * opens a new one, and the dead one is closed once the last context or stream
 * that held it lets go (each holder releases the node it got). One process can
 * hold several contexts (a desktop shell: its volume applet, libcanberra's
 * event sounds), so the dead node may outlive the server for a while.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "shim.h"
#include "../compat/server.h"
#include "../compat/compat_conf.h"

struct member_list {
	size_t count;
	struct shim_member *members[];
};

/* one node: the live one, or a dead one its last holders still hold */
struct gen {
	jackbus_node_t *node;
	int refs;
	bool active;
	atomic_bool dead;
	_Atomic(struct member_list *) list;
	atomic_ullong seq;
	pthread_t events;
	bool events_running;
	int wake[2];
	bool async;			/* the settings' pa.async, when the node opened */
	bool has_process;		/* node_process set: a realtime thread when active */
	int holds;			/* async streams */
	struct gen *next;		/* the dead ones */
};

static struct {
	pthread_mutex_t lock;
	struct gen *live;
	struct gen *dead;
	int watch[16];
	int n_watch;
} nodes = { .lock = PTHREAD_MUTEX_INITIALIZER };

static int node_process(jackbus_node_t *n, const jackbus_cycle_t *c, void *arg)
{
	struct gen *g = arg;
	atomic_fetch_add(&g->seq, 1);
	struct member_list *l = atomic_load(&g->list);
	for (size_t i = 0; l && i < l->count; i++)
		l->members[i]->process(l->members[i], c);
	atomic_fetch_add(&g->seq, 1);
	return 0;
}

/* after publishing a new list: free the old one once no cycle reads it */
static void retire(struct gen *g, void *old)
{
	if (old == NULL)
		return;
	unsigned long long seq = atomic_load(&g->seq);
	for (int i = 0; (seq & 1) && i < 20000; i++) {
		struct timespec ts = { 0, 100 * 1000 };
		nanosleep(&ts, NULL);
		if (atomic_load(&g->seq) != seq)
			seq = 0;
	}
	if (seq & 1) {
		shim_warn("a process cycle has run for 2 s: an old list is kept");
		return;
	}
	free(old);
}

static void notify_watchers(void)
{
	uint64_t one = 1;
	pthread_mutex_lock(&nodes.lock);
	for (int i = 0; i < nodes.n_watch; i++) {
		ssize_t r = write(nodes.watch[i], &one, sizeof(one));
		(void)r;
	}
	pthread_mutex_unlock(&nodes.lock);
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
		bool changed = false;
		while (jackbus_node_next_event(g->node, &e) == 1) {
			if (e.type == JACKBUS_EVENT_SHUTDOWN) {
				shim_log("the JACK server went away: %s", e.name);
				atomic_store(&g->dead, true);
				changed = true;
			} else if (e.type == JACKBUS_EVENT_CLIENT || e.type == JACKBUS_EVENT_PORT ||
					e.type == JACKBUS_EVENT_RENAME || e.type == JACKBUS_EVENT_LOST ||
					e.type == JACKBUS_EVENT_BUFFER_SIZE) {
				changed = true;
			}
		}
		if (changed)
			notify_watchers();
	}
	return NULL;
}

/* under nodes.lock: the node n is (the live one or a dead one) */
static struct gen *find(jackbus_node_t *n)
{
	if (n == NULL)
		return NULL;
	if (nodes.live && nodes.live->node == n)
		return nodes.live;
	for (struct gen *g = nodes.dead; g; g = g->next)
		if (g->node == n)
			return g;
	return NULL;
}

static struct gen *open_gen(const char *name)
{
	jack_status_t status = 0;
	const char *env = getenv("JACKBUS_PA_CLIENT_NAME");
	if (env && env[0])
		name = env;
	if (!jackbus_server_present()) {
		shim_log("JackBus is not running (no server socket)");
		return NULL;
	}
	struct gen *g = calloc(1, sizeof(*g));
	if (g == NULL)
		return NULL;
	g->wake[0] = g->wake[1] = -1;
	g->node = jackbus_node_open(name && name[0] ? name : "pulse-app", JackNoStartServer, &status);
	if (g->node == NULL) {
		shim_log("can't reach JackBus (status 0x%x)", status);
		free(g);
		return NULL;
	}
	atomic_store(&g->list, calloc(1, sizeof(struct member_list)));
	g->async = jackbus_compat_bool("async", "JACKBUS_PA_ASYNC", false);
	g->has_process = !g->async;
	if ((g->has_process && jackbus_node_set_process(g->node, node_process, g) != 0) ||
	    pipe2(g->wake, O_CLOEXEC) != 0) {
		jackbus_node_close(g->node);
		free(atomic_load(&g->list));
		free(g);
		return NULL;
	}
	g->events_running = pthread_create(&g->events, NULL, event_thread, g) == 0;
	shim_log("JACK client \"%s\" opened on the JackBus socket%s",
			jack_get_client_name(jackbus_node_client(g->node)), g->async ? " (async streams)" : "");
	return g;
}

/* not under nodes.lock: no reference left */
static void close_gen(struct gen *g)
{
	if (g->events_running) {
		ssize_t r = write(g->wake[1], "x", 1);
		(void)r;
		pthread_join(g->events, NULL);
	}
	close(g->wake[0]);
	close(g->wake[1]);
	jackbus_node_close(g->node);		/* no cycle runs after this */
	free(atomic_load(&g->list));
	free(g);
	shim_log("JACK client closed");
}

jackbus_node_t *shim_node_ref(const char *name)
{
	jackbus_node_t *n = NULL;
	pthread_mutex_lock(&nodes.lock);
	if (nodes.live && atomic_load(&nodes.live->dead)) {
		/* the server went away: set it aside for its last holders, open anew */
		nodes.live->next = nodes.dead;
		nodes.dead = nodes.live;
		nodes.live = NULL;
	}
	if (nodes.live == NULL)
		nodes.live = open_gen(name);
	if (nodes.live) {
		nodes.live->refs++;
		n = nodes.live->node;
	}
	pthread_mutex_unlock(&nodes.lock);
	return n;
}

void shim_node_unref(jackbus_node_t *n)
{
	struct gen *gone = NULL;
	pthread_mutex_lock(&nodes.lock);
	struct gen *g = find(n);
	if (g && g->refs > 0 && --g->refs == 0) {
		gone = g;
		if (g == nodes.live) {
			nodes.live = NULL;
		} else {
			for (struct gen **p = &nodes.dead; *p; p = &(*p)->next) {
				if (*p == g) {
					*p = g->next;
					break;
				}
			}
		}
	}
	pthread_mutex_unlock(&nodes.lock);
	if (gone)
		close_gen(gone);
}

jack_client_t *shim_node_client(void)
{
	pthread_mutex_lock(&nodes.lock);
	jack_client_t *c = nodes.live ? jackbus_node_client(nodes.live->node) : NULL;
	pthread_mutex_unlock(&nodes.lock);
	return c;
}

bool shim_node_dead(jackbus_node_t *n)
{
	pthread_mutex_lock(&nodes.lock);
	struct gen *g = find(n);
	bool dead = g == NULL || atomic_load(&g->dead);
	pthread_mutex_unlock(&nodes.lock);
	return dead;
}

void shim_node_watch(int efd, bool add)
{
	pthread_mutex_lock(&nodes.lock);
	if (add && nodes.n_watch < (int)(sizeof(nodes.watch) / sizeof(nodes.watch[0]))) {
		nodes.watch[nodes.n_watch++] = efd;
	} else if (!add) {
		for (int i = 0; i < nodes.n_watch; i++)
			if (nodes.watch[i] == efd)
				nodes.watch[i--] = nodes.watch[--nodes.n_watch];
	}
	pthread_mutex_unlock(&nodes.lock);
}

bool shim_node_async(jackbus_node_t *n)
{
	pthread_mutex_lock(&nodes.lock);
	struct gen *g = find(n);
	bool async = g && g->async;
	pthread_mutex_unlock(&nodes.lock);
	return async;
}

/* under nodes.lock: in the graph's cycle while something runs in it */
static void set_active(struct gen *g, bool active)
{
	if (active == g->active)
		return;
	if (active) {
		if (jackbus_node_activate(g->node) != 0) {
			shim_warn("can't activate the JACK client");
			return;
		}
	} else {
		jackbus_node_deactivate(g->node);
	}
	g->active = active;
}

static size_t members(struct gen *g)
{
	struct member_list *l = atomic_load(&g->list);
	return l ? l->count : 0;
}

/* a stream in the cycle: the node gets its process function (and, active, a realtime
 * thread: JACK's path for a client activated without one) */
void shim_node_need_rt(jackbus_node_t *n)
{
	pthread_mutex_lock(&nodes.lock);
	struct gen *g = find(n);
	if (g && !g->has_process) {
		if (jackbus_node_set_process(g->node, node_process, g) == 0)
			g->has_process = true;
		else
			shim_warn("can't give the JACK client a realtime thread");
	}
	pthread_mutex_unlock(&nodes.lock);
}

void shim_node_hold(jackbus_node_t *n, bool on)
{
	pthread_mutex_lock(&nodes.lock);
	struct gen *g = find(n);
	if (g) {
		g->holds += on ? 1 : -1;
		if (g->holds < 0)
			g->holds = 0;
		set_active(g, g->holds > 0 || members(g) > 0);
	}
	pthread_mutex_unlock(&nodes.lock);
}

void shim_node_attach(jackbus_node_t *n, struct shim_member *m)
{
	pthread_mutex_lock(&nodes.lock);
	struct gen *g = find(n);
	struct member_list *old = g ? atomic_load(&g->list) : NULL;
	size_t count = old ? old->count : 0;
	struct member_list *l = g ? calloc(1, sizeof(*l) + (count + 1) * sizeof(l->members[0])) : NULL;
	if (l) {
		if (count)
			memcpy(l->members, old->members, count * sizeof(l->members[0]));
		l->members[count] = m;
		l->count = count + 1;
		atomic_store(&g->list, l);
		retire(g, old);
		set_active(g, true);
	}
	pthread_mutex_unlock(&nodes.lock);
}

void shim_node_detach(jackbus_node_t *n, struct shim_member *m)
{
	pthread_mutex_lock(&nodes.lock);
	struct gen *g = find(n);
	struct member_list *old = g ? atomic_load(&g->list) : NULL;
	if (old) {
		struct member_list *l = calloc(1, sizeof(*l) + old->count * sizeof(l->members[0]));
		if (l) {
			for (size_t i = 0; i < old->count; i++)
				if (old->members[i] != m)
					l->members[l->count++] = old->members[i];
			atomic_store(&g->list, l);
			retire(g, old);		/* from here no cycle runs m */
			if (l->count == 0 && g->holds == 0)
				set_active(g, false);
		}
	}
	pthread_mutex_unlock(&nodes.lock);
}
