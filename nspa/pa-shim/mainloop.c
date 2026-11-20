/*
 * jackbus-pa: libpulse's main loops (pa_mainloop, pa_threaded_mainloop) and
 * signals, on poll(): applications run their own I/O and timers on them,
 * and the context and streams theirs (deferred events, a timer per stream).
 * A time given with PulseAudio's realtime-clock bit (pa_context_rttime_new)
 * is CLOCK_MONOTONIC; otherwise it is wall-clock time.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "shim.h"

#define RTCLOCK_BIT (1U << 30)		/* PulseAudio's PA_TIMEVAL_RTCLOCK */

/* ---------------------------------------------------------------- misc */

int shim_verbose = -1;

static bool verbose(void)
{
	if (shim_verbose < 0) {
		const char *v = getenv("JACKBUS_PA_VERBOSE");
		shim_verbose = v && v[0] && strcmp(v, "0") != 0;
	}
	return shim_verbose;
}

void shim_log(const char *fmt, ...)
{
	va_list ap;
	if (!verbose())
		return;
	va_start(ap, fmt);
	fputs("jackbus-pa: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

void shim_warn(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("jackbus-pa: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

uint64_t shim_now_usec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

static pa_io_event *io_new(pa_mainloop_api *a, int fd, pa_io_event_flags_t events,
		pa_io_event_cb_t cb, void *userdata);

/* As libpulse's pa_context_rttime_new: this library's own main loops take the
 * monotonic time, marked PA_TIMEVAL_RTCLOCK; any other (libpulse-mainloop-glib's,
 * an application's own) compares a time with the wall clock and ignores the mark,
 * so it gets the wall clock's time of the moment (a monotonic one would be long
 * past to it: every timer due at once, the loop never sleeping) */
struct timeval *shim_rttime(pa_mainloop_api *a, struct timeval *tv, uint64_t usec)
{
	if (a && a->io_new != io_new) {
		struct timeval now;
		gettimeofday(&now, NULL);
		int64_t wall = (int64_t)now.tv_sec * 1000000 + now.tv_usec + ((int64_t)usec - (int64_t)shim_now_usec());
		if (wall < 0)
			wall = 0;
		tv->tv_sec = (time_t)(wall / 1000000);
		tv->tv_usec = (suseconds_t)(wall % 1000000);
		return tv;
	}
	tv->tv_sec = (time_t)(usec / 1000000ull);
	tv->tv_usec = (suseconds_t)((usec % 1000000ull) | RTCLOCK_BIT);
	return tv;
}

static uint64_t monotonic_of(const struct timeval *tv)
{
	if (tv->tv_usec & RTCLOCK_BIT)
		return (uint64_t)tv->tv_sec * 1000000ull + (uint64_t)(tv->tv_usec & ~RTCLOCK_BIT);
	struct timeval now;
	gettimeofday(&now, NULL);
	int64_t wall = (int64_t)tv->tv_sec * 1000000 + tv->tv_usec;
	int64_t wall_now = (int64_t)now.tv_sec * 1000000 + now.tv_usec;
	int64_t t = (int64_t)shim_now_usec() + (wall - wall_now);
	return t > 0 ? (uint64_t)t : 0;
}

SHIM_EXPORT const char *pa_get_library_version(void)
{
	return pa_get_headers_version();
}

/* ------------------------------------------------------------ pa_mainloop */

struct pa_io_event {
	pa_mainloop *m;
	int fd;
	pa_io_event_flags_t events;
	pa_io_event_cb_t cb;
	void *userdata;
	pa_io_event_destroy_cb_t destroy;
	bool dead;
	struct pa_io_event *next;
};

struct pa_time_event {
	pa_mainloop *m;
	bool enabled;
	uint64_t when;			/* monotonic usec */
	struct timeval tv;		/* as given, for the callback */
	pa_time_event_cb_t cb;
	void *userdata;
	pa_time_event_destroy_cb_t destroy;
	bool dead;
	struct pa_time_event *next;
};

struct pa_defer_event {
	pa_mainloop *m;
	bool enabled;
	pa_defer_event_cb_t cb;
	void *userdata;
	pa_defer_event_destroy_cb_t destroy;
	bool dead;
	struct pa_defer_event *next;
};

struct pa_mainloop {
	pa_mainloop_api api;
	struct pa_io_event *ios;
	struct pa_time_event *times;
	struct pa_defer_event *defers;
	int n_dead;
	int wake_fd;
	bool quit;
	int retval;
	pa_poll_func poll_func;
	void *poll_userdata;
	struct pollfd *pfds;
	unsigned int n_pfds, cap_pfds;
	int timeout;
};

static void wake(pa_mainloop *m)
{
	uint64_t one = 1;
	ssize_t r = write(m->wake_fd, &one, sizeof(one));
	(void)r;
}

static pa_io_event *io_new(pa_mainloop_api *a, int fd, pa_io_event_flags_t events,
		pa_io_event_cb_t cb, void *userdata)
{
	pa_mainloop *m = a->userdata;
	pa_io_event *e = calloc(1, sizeof(*e));
	if (e == NULL)
		return NULL;
	e->m = m;
	e->fd = fd;
	e->events = events;
	e->cb = cb;
	e->userdata = userdata;
	e->next = m->ios;
	m->ios = e;
	wake(m);
	return e;
}

static void io_enable(pa_io_event *e, pa_io_event_flags_t events)
{
	e->events = events;
	wake(e->m);
}

static void io_free(pa_io_event *e)
{
	if (!e->dead) {
		e->dead = true;
		e->m->n_dead++;
		wake(e->m);
	}
}

static void io_set_destroy(pa_io_event *e, pa_io_event_destroy_cb_t cb)
{
	e->destroy = cb;
}

static pa_time_event *time_new(pa_mainloop_api *a, const struct timeval *tv,
		pa_time_event_cb_t cb, void *userdata)
{
	pa_mainloop *m = a->userdata;
	pa_time_event *e = calloc(1, sizeof(*e));
	if (e == NULL)
		return NULL;
	e->m = m;
	e->cb = cb;
	e->userdata = userdata;
	if (tv) {
		e->enabled = true;
		e->tv = *tv;
		e->when = monotonic_of(tv);
	}
	e->next = m->times;
	m->times = e;
	wake(m);
	return e;
}

static void time_restart(pa_time_event *e, const struct timeval *tv)
{
	if (tv) {
		e->enabled = true;
		e->tv = *tv;
		e->when = monotonic_of(tv);
	} else {
		e->enabled = false;
	}
	wake(e->m);
}

static void time_free(pa_time_event *e)
{
	if (!e->dead) {
		e->dead = true;
		e->enabled = false;
		e->m->n_dead++;
		wake(e->m);
	}
}

static void time_set_destroy(pa_time_event *e, pa_time_event_destroy_cb_t cb)
{
	e->destroy = cb;
}

static pa_defer_event *defer_new(pa_mainloop_api *a, pa_defer_event_cb_t cb, void *userdata)
{
	pa_mainloop *m = a->userdata;
	pa_defer_event *e = calloc(1, sizeof(*e));
	if (e == NULL)
		return NULL;
	e->m = m;
	e->enabled = true;
	e->cb = cb;
	e->userdata = userdata;
	e->next = m->defers;
	m->defers = e;
	wake(m);
	return e;
}

static void defer_enable(pa_defer_event *e, int b)
{
	e->enabled = b;
	if (b)
		wake(e->m);
}

static void defer_free(pa_defer_event *e)
{
	if (!e->dead) {
		e->dead = true;
		e->enabled = false;
		e->m->n_dead++;
		wake(e->m);
	}
}

static void defer_set_destroy(pa_defer_event *e, pa_defer_event_destroy_cb_t cb)
{
	e->destroy = cb;
}

static void quit(pa_mainloop_api *a, int retval)
{
	pa_mainloop_quit(a->userdata, retval);
}

static const pa_mainloop_api api_template = {
	.io_new = io_new,
	.io_enable = io_enable,
	.io_free = io_free,
	.io_set_destroy = io_set_destroy,
	.time_new = time_new,
	.time_restart = time_restart,
	.time_free = time_free,
	.time_set_destroy = time_set_destroy,
	.defer_new = defer_new,
	.defer_enable = defer_enable,
	.defer_free = defer_free,
	.defer_set_destroy = defer_set_destroy,
	.quit = quit,
};

SHIM_EXPORT pa_mainloop *pa_mainloop_new(void)
{
	pa_mainloop *m = calloc(1, sizeof(*m));
	if (m == NULL)
		return NULL;
	m->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (m->wake_fd < 0) {
		free(m);
		return NULL;
	}
	m->api = api_template;
	m->api.userdata = m;
	m->timeout = -1;
	return m;
}

/* free what was freed (destroy callbacks now); all of it with `all` */
static void cleanup(pa_mainloop *m, bool all)
{
	for (pa_io_event **p = &m->ios; *p;) {
		pa_io_event *e = *p;
		if (all || e->dead) {
			*p = e->next;
			if (e->destroy)
				e->destroy(&m->api, e, e->userdata);
			free(e);
		} else {
			p = &e->next;
		}
	}
	for (pa_time_event **p = &m->times; *p;) {
		pa_time_event *e = *p;
		if (all || e->dead) {
			*p = e->next;
			if (e->destroy)
				e->destroy(&m->api, e, e->userdata);
			free(e);
		} else {
			p = &e->next;
		}
	}
	for (pa_defer_event **p = &m->defers; *p;) {
		pa_defer_event *e = *p;
		if (all || e->dead) {
			*p = e->next;
			if (e->destroy)
				e->destroy(&m->api, e, e->userdata);
			free(e);
		} else {
			p = &e->next;
		}
	}
	m->n_dead = 0;
}

SHIM_EXPORT void pa_mainloop_free(pa_mainloop *m)
{
	if (m == NULL)
		return;
	cleanup(m, true);
	close(m->wake_fd);
	free(m->pfds);
	free(m);
}

SHIM_EXPORT pa_mainloop_api *pa_mainloop_get_api(pa_mainloop *m)
{
	return &m->api;
}

SHIM_EXPORT void pa_mainloop_quit(pa_mainloop *m, int retval)
{
	m->quit = true;
	m->retval = retval;
	wake(m);
}

SHIM_EXPORT int pa_mainloop_get_retval(const pa_mainloop *m)
{
	return m->retval;
}

SHIM_EXPORT void pa_mainloop_wakeup(pa_mainloop *m)
{
	wake(m);
}

SHIM_EXPORT void pa_mainloop_set_poll_func(pa_mainloop *m, pa_poll_func poll_func, void *userdata)
{
	m->poll_func = poll_func;
	m->poll_userdata = userdata;
}

SHIM_EXPORT int pa_mainloop_prepare(pa_mainloop *m, int timeout)
{
	if (m->quit)
		return -2;
	if (m->n_dead)
		cleanup(m, false);
	/* an enabled deferred event: no waiting */
	for (pa_defer_event *e = m->defers; e; e = e->next)
		if (e->enabled && !e->dead)
			timeout = 0;
	/* the next timer */
	if (timeout != 0) {
		uint64_t now = shim_now_usec();
		for (pa_time_event *e = m->times; e; e = e->next) {
			if (!e->enabled || e->dead)
				continue;
			int t = e->when <= now ? 0 : (int)SHIM_MIN((e->when - now + 999) / 1000, (uint64_t)INT32_MAX);
			if (timeout < 0 || t < timeout)
				timeout = t;
		}
	}
	/* the descriptors: the wakeup eventfd first */
	unsigned int n = 1;
	for (pa_io_event *e = m->ios; e; e = e->next)
		if (!e->dead)
			n++;
	if (n > m->cap_pfds) {
		struct pollfd *p = realloc(m->pfds, n * sizeof(*p));
		if (p == NULL)
			return -1;
		m->pfds = p;
		m->cap_pfds = n;
	}
	m->pfds[0] = (struct pollfd){ .fd = m->wake_fd, .events = POLLIN };
	n = 1;
	for (pa_io_event *e = m->ios; e; e = e->next) {
		if (e->dead)
			continue;
		short ev = 0;
		if (e->events & PA_IO_EVENT_INPUT)
			ev |= POLLIN;
		if (e->events & PA_IO_EVENT_OUTPUT)
			ev |= POLLOUT;
		m->pfds[n++] = (struct pollfd){ .fd = e->fd, .events = ev };
	}
	m->n_pfds = n;
	m->timeout = timeout;
	return 0;
}

SHIM_EXPORT int pa_mainloop_poll(pa_mainloop *m)
{
	int r;
	if (m->quit)
		return -2;
	if (m->poll_func)
		r = m->poll_func(m->pfds, m->n_pfds, m->timeout, m->poll_userdata);
	else
		r = poll(m->pfds, m->n_pfds, m->timeout);
	if (r < 0 && errno == EINTR)
		r = 0;
	return r;
}

SHIM_EXPORT int pa_mainloop_dispatch(pa_mainloop *m)
{
	int n = 0;
	if (m->quit)
		return -2;
	if (m->pfds[0].revents) {
		uint64_t v;
		ssize_t r = read(m->wake_fd, &v, sizeof(v));
		(void)r;
	}
	for (pa_defer_event *e = m->defers; e && !m->quit; e = e->next) {
		if (!e->enabled || e->dead)
			continue;
		e->cb(&m->api, e, e->userdata);
		n++;
	}
	uint64_t now = shim_now_usec();
	for (pa_time_event *e = m->times; e && !m->quit; e = e->next) {
		if (!e->enabled || e->dead || e->when > now)
			continue;
		e->enabled = false;
		e->cb(&m->api, e, &e->tv, e->userdata);
		n++;
	}
	/* the descriptors, as polled (events added meanwhile wait a turn) */
	unsigned int i = 1;
	for (pa_io_event *e = m->ios; e && i < m->n_pfds && !m->quit; e = e->next) {
		if (e->dead || e->fd != m->pfds[i].fd)
			continue;
		short re = m->pfds[i++].revents;
		if (!re)
			continue;
		pa_io_event_flags_t f = 0;
		if (re & POLLIN)
			f |= PA_IO_EVENT_INPUT;
		if (re & POLLOUT)
			f |= PA_IO_EVENT_OUTPUT;
		if (re & POLLHUP)
			f |= PA_IO_EVENT_HANGUP;
		if (re & (POLLERR | POLLNVAL))
			f |= PA_IO_EVENT_ERROR;
		e->cb(&m->api, e, e->fd, f, e->userdata);
		n++;
	}
	return m->quit ? -2 : n;
}

SHIM_EXPORT int pa_mainloop_iterate(pa_mainloop *m, int block, int *retval)
{
	int r;
	if ((r = pa_mainloop_prepare(m, block ? -1 : 0)) < 0 ||
	    (r = pa_mainloop_poll(m)) < 0 ||
	    (r = pa_mainloop_dispatch(m)) < 0) {
		if (r == -2 && retval)
			*retval = m->retval;
		return r;
	}
	return r;
}

SHIM_EXPORT int pa_mainloop_run(pa_mainloop *m, int *retval)
{
	int r;
	while ((r = pa_mainloop_iterate(m, 1, retval)) >= 0)
		;
	if (r == -2)
		return 1;
	return -1;
}

/* --------------------------------------------------- pa_threaded_mainloop */

struct pa_threaded_mainloop {
	pa_mainloop *real;
	pthread_t thread;
	bool running;
	pthread_mutex_t mutex;
	pthread_cond_t cond, accept_cond;
	int n_waiting_for_accept;
	char name[16];
};

/* the loop thread holds the lock, except while it polls */
static int unlocked_poll(struct pollfd *ufds, unsigned long nfds, int timeout, void *userdata)
{
	pa_threaded_mainloop *t = userdata;
	pthread_mutex_unlock(&t->mutex);
	int r = poll(ufds, nfds, timeout);
	pthread_mutex_lock(&t->mutex);
	return r;
}

SHIM_EXPORT pa_threaded_mainloop *pa_threaded_mainloop_new(void)
{
	pa_threaded_mainloop *t = calloc(1, sizeof(*t));
	pthread_mutexattr_t attr;
	if (t == NULL)
		return NULL;
	if ((t->real = pa_mainloop_new()) == NULL) {
		free(t);
		return NULL;
	}
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&t->mutex, &attr);
	pthread_mutexattr_destroy(&attr);
	pthread_cond_init(&t->cond, NULL);
	pthread_cond_init(&t->accept_cond, NULL);
	pa_mainloop_set_poll_func(t->real, unlocked_poll, t);
	snprintf(t->name, sizeof(t->name), "pa-mainloop");
	return t;
}

static void *thread_main(void *userdata)
{
	pa_threaded_mainloop *t = userdata;
	pthread_setname_np(pthread_self(), t->name);
	pthread_mutex_lock(&t->mutex);
	pa_mainloop_run(t->real, NULL);
	pthread_mutex_unlock(&t->mutex);
	return NULL;
}

SHIM_EXPORT int pa_threaded_mainloop_start(pa_threaded_mainloop *t)
{
	if (t->running)
		return -1;
	t->real->quit = false;
	if (pthread_create(&t->thread, NULL, thread_main, t) != 0)
		return -1;
	t->running = true;
	return 0;
}

SHIM_EXPORT void pa_threaded_mainloop_stop(pa_threaded_mainloop *t)
{
	if (!t->running)
		return;
	pthread_mutex_lock(&t->mutex);
	pa_mainloop_quit(t->real, 0);
	pthread_mutex_unlock(&t->mutex);
	pthread_join(t->thread, NULL);
	t->running = false;
}

SHIM_EXPORT void pa_threaded_mainloop_free(pa_threaded_mainloop *t)
{
	if (t == NULL)
		return;
	pa_threaded_mainloop_stop(t);
	pa_mainloop_free(t->real);
	pthread_mutex_destroy(&t->mutex);
	pthread_cond_destroy(&t->cond);
	pthread_cond_destroy(&t->accept_cond);
	free(t);
}

SHIM_EXPORT void pa_threaded_mainloop_lock(pa_threaded_mainloop *t)
{
	pthread_mutex_lock(&t->mutex);
}

SHIM_EXPORT void pa_threaded_mainloop_unlock(pa_threaded_mainloop *t)
{
	pthread_mutex_unlock(&t->mutex);
}

SHIM_EXPORT void pa_threaded_mainloop_wait(pa_threaded_mainloop *t)
{
	pthread_cond_wait(&t->cond, &t->mutex);
}

SHIM_EXPORT void pa_threaded_mainloop_signal(pa_threaded_mainloop *t, int wait_for_accept)
{
	pthread_cond_broadcast(&t->cond);
	if (wait_for_accept) {
		t->n_waiting_for_accept++;
		while (t->n_waiting_for_accept > 0)
			pthread_cond_wait(&t->accept_cond, &t->mutex);
	}
}

SHIM_EXPORT void pa_threaded_mainloop_accept(pa_threaded_mainloop *t)
{
	t->n_waiting_for_accept--;
	pthread_cond_signal(&t->accept_cond);
}

SHIM_EXPORT int pa_threaded_mainloop_get_retval(const pa_threaded_mainloop *t)
{
	return pa_mainloop_get_retval(t->real);
}

SHIM_EXPORT pa_mainloop_api *pa_threaded_mainloop_get_api(pa_threaded_mainloop *t)
{
	return pa_mainloop_get_api(t->real);
}

SHIM_EXPORT int pa_threaded_mainloop_in_thread(pa_threaded_mainloop *t)
{
	return t->running && pthread_equal(pthread_self(), t->thread);
}

SHIM_EXPORT void pa_threaded_mainloop_set_name(pa_threaded_mainloop *t, const char *name)
{
	snprintf(t->name, sizeof(t->name), "%s", name ? name : "pa-mainloop");
	if (t->running)
		pthread_setname_np(t->thread, t->name);
}

struct once_unlocked {
	pa_threaded_mainloop *t;
	void (*cb)(pa_threaded_mainloop *t, void *userdata);
	void *userdata;
	bool done;
};

static void once_unlocked_cb(pa_mainloop_api *a, void *userdata)
{
	struct once_unlocked *o = userdata;
	pthread_mutex_unlock(&o->t->mutex);
	o->cb(o->t, o->userdata);
	pthread_mutex_lock(&o->t->mutex);
	o->done = true;
	pthread_cond_broadcast(&o->t->cond);
}

SHIM_EXPORT void pa_threaded_mainloop_once_unlocked(pa_threaded_mainloop *t,
		void (*callback)(pa_threaded_mainloop *t, void *userdata), void *userdata)
{
	struct once_unlocked o = { t, callback, userdata, false };
	pa_mainloop_api_once(&t->real->api, once_unlocked_cb, &o);
	while (!o.done)
		pthread_cond_wait(&t->cond, &t->mutex);
}

/* --------------------------------------------------------------- signals */

struct pa_signal_event {
	int sig;
	struct sigaction saved;
	pa_signal_cb_t cb;
	void *userdata;
	pa_signal_destroy_cb_t destroy;
	struct pa_signal_event *next;
};

static struct {
	pa_mainloop_api *api;
	int pipe[2];
	pa_io_event *io;
	struct pa_signal_event *events;
} sig = { .pipe = { -1, -1 } };

static void signal_handler(int s)
{
	int saved = errno;
	ssize_t r = write(sig.pipe[1], &s, sizeof(s));
	(void)r;
	errno = saved;
}

static void signal_io(pa_mainloop_api *a, pa_io_event *e, int fd, pa_io_event_flags_t f, void *userdata)
{
	int s;
	while (read(fd, &s, sizeof(s)) == sizeof(s)) {
		for (struct pa_signal_event *ev = sig.events; ev; ev = ev->next)
			if (ev->sig == s)
				ev->cb(a, ev, s, ev->userdata);
	}
}

SHIM_EXPORT int pa_signal_init(pa_mainloop_api *api)
{
	if (sig.api)
		return -1;
	if (pipe2(sig.pipe, O_NONBLOCK | O_CLOEXEC) < 0)
		return -1;
	sig.api = api;
	sig.io = api->io_new(api, sig.pipe[0], PA_IO_EVENT_INPUT, signal_io, NULL);
	return 0;
}

SHIM_EXPORT pa_signal_event *pa_signal_new(int s, pa_signal_cb_t callback, void *userdata)
{
	struct pa_signal_event *e = calloc(1, sizeof(*e));
	struct sigaction sa;
	if (e == NULL || sig.api == NULL) {
		free(e);
		return NULL;
	}
	e->sig = s;
	e->cb = callback;
	e->userdata = userdata;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sigaction(s, &sa, &e->saved);
	e->next = sig.events;
	sig.events = e;
	return e;
}

SHIM_EXPORT void pa_signal_free(pa_signal_event *e)
{
	for (struct pa_signal_event **p = &sig.events; *p; p = &(*p)->next) {
		if (*p == e) {
			*p = e->next;
			break;
		}
	}
	sigaction(e->sig, &e->saved, NULL);
	if (e->destroy)
		e->destroy(sig.api, e, e->userdata);
	free(e);
}

SHIM_EXPORT void pa_signal_set_destroy(pa_signal_event *e, pa_signal_destroy_cb_t callback)
{
	e->destroy = callback;
}

SHIM_EXPORT void pa_signal_done(void)
{
	while (sig.events)
		pa_signal_free(sig.events);
	if (sig.io)
		sig.api->io_free(sig.io);
	if (sig.pipe[0] >= 0) {
		close(sig.pipe[0]);
		close(sig.pipe[1]);
	}
	sig.pipe[0] = sig.pipe[1] = -1;
	sig.io = NULL;
	sig.api = NULL;
}
