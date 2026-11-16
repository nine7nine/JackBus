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
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "shim.h"

struct member_list {
	size_t count;
	struct shim_member *members[];
};

static struct {
	pthread_mutex_t lock;
	jackbus_node_t *node;
	int refs;
	bool active;
	atomic_bool dead;
	_Atomic(struct member_list *) list;
	atomic_ullong seq;
	pthread_t events;
	bool events_running;
	int wake[2];
	int watch[16];
	int n_watch;
} node = { .lock = PTHREAD_MUTEX_INITIALIZER, .wake = { -1, -1 } };

static int node_process(jackbus_node_t *n, const jackbus_cycle_t *c, void *arg)
{
	atomic_fetch_add(&node.seq, 1);
	struct member_list *l = atomic_load(&node.list);
	for (size_t i = 0; l && i < l->count; i++)
		l->members[i]->process(l->members[i], c);
	atomic_fetch_add(&node.seq, 1);
	return 0;
}

/* after publishing a new list: free the old one once no cycle reads it */
static void retire(void *old)
{
	if (old == NULL)
		return;
	unsigned long long seq = atomic_load(&node.seq);
	for (int i = 0; (seq & 1) && i < 20000; i++) {
		struct timespec ts = { 0, 100 * 1000 };
		nanosleep(&ts, NULL);
		if (atomic_load(&node.seq) != seq)
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
	pthread_mutex_lock(&node.lock);
	for (int i = 0; i < node.n_watch; i++) {
		ssize_t r = write(node.watch[i], &one, sizeof(one));
		(void)r;
	}
	pthread_mutex_unlock(&node.lock);
}

static void *event_thread(void *arg)
{
	struct pollfd p[2] = {
		{ jackbus_node_event_fd(node.node), POLLIN, 0 },
		{ node.wake[0], POLLIN, 0 },
	};
	for (;;) {
		if (poll(p, 2, -1) < 0 && errno != EINTR)
			break;
		if (p[1].revents)
			break;
		jackbus_event_t e;
		bool changed = false;
		while (jackbus_node_next_event(node.node, &e) == 1) {
			if (e.type == JACKBUS_EVENT_SHUTDOWN) {
				shim_log("the JACK server went away: %s", e.name);
				atomic_store(&node.dead, true);
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

jackbus_node_t *shim_node_ref(const char *name)
{
	jackbus_node_t *n = NULL;
	jack_status_t status = 0;

	pthread_mutex_lock(&node.lock);
	if (node.node == NULL) {
		const char *env = getenv("JACKBUS_PA_CLIENT_NAME");
		if (env && env[0])
			name = env;
		node.node = jackbus_node_open(name && name[0] ? name : "pulse-app", JackNoStartServer, &status);
		if (node.node == NULL) {
			shim_log("can't reach JackBus (status 0x%x)", status);
			goto out;
		}
		struct member_list *l = calloc(1, sizeof(*l));
		atomic_store(&node.list, l);
		atomic_store(&node.dead, false);
		if (jackbus_node_set_process(node.node, node_process, NULL) != 0 ||
		    pipe2(node.wake, O_CLOEXEC) != 0) {
			jackbus_node_close(node.node);
			node.node = NULL;
			free(atomic_exchange(&node.list, NULL));
			goto out;
		}
		node.events_running = pthread_create(&node.events, NULL, event_thread, NULL) == 0;
		shim_log("JACK client \"%s\" opened on the JackBus socket",
				jack_get_client_name(jackbus_node_client(node.node)));
	}
	node.refs++;
	n = node.node;
out:
	pthread_mutex_unlock(&node.lock);
	return n;
}

void shim_node_unref(void)
{
	jackbus_node_t *n = NULL;

	pthread_mutex_lock(&node.lock);
	if (node.refs > 0 && --node.refs == 0) {
		n = node.node;
		node.node = NULL;
	}
	pthread_mutex_unlock(&node.lock);
	if (n == NULL)
		return;
	if (node.events_running) {
		ssize_t r = write(node.wake[1], "x", 1);
		(void)r;
		pthread_join(node.events, NULL);
		node.events_running = false;
	}
	close(node.wake[0]);
	close(node.wake[1]);
	node.wake[0] = node.wake[1] = -1;
	node.active = false;
	jackbus_node_close(n);			/* no cycle runs after this */
	free(atomic_exchange(&node.list, NULL));
	shim_log("JACK client closed");
}

jack_client_t *shim_node_client(void)
{
	return node.node ? jackbus_node_client(node.node) : NULL;
}

bool shim_node_dead(void)
{
	return atomic_load(&node.dead);
}

void shim_node_watch(int efd, bool add)
{
	pthread_mutex_lock(&node.lock);
	if (add && node.n_watch < (int)(sizeof(node.watch) / sizeof(node.watch[0]))) {
		node.watch[node.n_watch++] = efd;
	} else if (!add) {
		for (int i = 0; i < node.n_watch; i++)
			if (node.watch[i] == efd)
				node.watch[i--] = node.watch[--node.n_watch];
	}
	pthread_mutex_unlock(&node.lock);
}

/* under node.lock: in the graph's cycle while something runs in it */
static void set_active(bool active)
{
	if (node.node == NULL || active == node.active)
		return;
	if (active) {
		if (jackbus_node_activate(node.node) != 0) {
			shim_warn("can't activate the JACK client");
			return;
		}
	} else {
		jackbus_node_deactivate(node.node);
	}
	node.active = active;
}

void shim_node_attach(struct shim_member *m)
{
	pthread_mutex_lock(&node.lock);
	struct member_list *old = atomic_load(&node.list);
	size_t n = old ? old->count : 0;
	struct member_list *l = calloc(1, sizeof(*l) + (n + 1) * sizeof(l->members[0]));
	if (l) {
		if (n)
			memcpy(l->members, old->members, n * sizeof(l->members[0]));
		l->members[n] = m;
		l->count = n + 1;
		atomic_store(&node.list, l);
		retire(old);
		set_active(true);
	}
	pthread_mutex_unlock(&node.lock);
}

void shim_node_detach(struct shim_member *m)
{
	pthread_mutex_lock(&node.lock);
	struct member_list *old = atomic_load(&node.list);
	if (old) {
		struct member_list *l = calloc(1, sizeof(*l) + old->count * sizeof(l->members[0]));
		if (l) {
			for (size_t i = 0; i < old->count; i++)
				if (old->members[i] != m)
					l->members[l->count++] = old->members[i];
			atomic_store(&node.list, l);
			retire(old);		/* from here no cycle runs m */
			if (l->count == 0)
				set_active(false);
		}
	}
	pthread_mutex_unlock(&node.lock);
}
