/*
 * jackbus-pa: pa_context and pa_operation on JackBus.
 *
 * No PulseAudio server is behind a context. Connecting opens the
 * application's JackBus node (not yet in the graph's cycle) and goes through
 * PulseAudio's states to READY on the application's main loop, as from a
 * socket. What a server would answer later (server and device information,
 * an operation's success) is delivered by deferred events, in order.
 *
 * The server JackBus presents: its rate as the server's and every device's
 * (so applications that open at the server's rate, as browsers do, take the
 * direct route); a sink per JACK client with physical playback ports (the
 * system card, each bridged card) and a source per client with physical
 * capture ports, named "jackbus.<client>.playback|capture"; the first of
 * each the default. Devices have no volume of their own (a request is
 * accepted and logged); a stream's own volume and mute (sink input, source
 * output) are applied to its samples. Device changes reach subscribers as
 * sink and source events; a server going away fails the context.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "shim.h"

#define MAX_DEVICES 32

struct pa_operation {
	int refs;
	pa_context *context;
	pa_stream *stream;
	pa_operation_state_t state;
	pa_operation_notify_cb_t state_cb;
	void *state_userdata;
	pa_defer_event *defer;
	bool owned;			/* our reference, held until done or cancelled */
	void (*run)(pa_operation *o);
	void *cb;
	void *userdata;
	struct shim_op_data data;
	pa_operation *next;
};

struct pa_context {
	int refs;
	pa_mainloop_api *api;
	char *name;
	pa_proplist *props;
	pa_context_state_t state;
	int error;
	pa_context_flags_t flags;
	pa_context_notify_cb_t state_cb;
	void *state_userdata;
	pa_context_subscribe_cb_t sub_cb;
	void *sub_userdata;
	pa_subscription_mask_t sub_mask;
	pa_defer_event *connect_defer;
	pa_time_event *retry;
	bool node_ref;
	int efd;
	pa_io_event *efd_io;
	struct shim_device devices[MAX_DEVICES];
	int n_devices;
	pa_operation *ops;
	pa_stream **streams;
	int n_streams, cap_streams;
	uint32_t next_stream_index;
};

/* a device's index, the same for as long as the process runs */
static struct { char name[256]; bool playback; uint32_t index; } device_index[64];
static int n_device_index;
static uint32_t next_sink = 0, next_source = 0;

static uint32_t index_of(const char *name, bool playback)
{
	for (int i = 0; i < n_device_index; i++)
		if (device_index[i].playback == playback && strcmp(device_index[i].name, name) == 0)
			return device_index[i].index;
	uint32_t idx = playback ? next_sink++ : next_source++;
	if (n_device_index < (int)(sizeof(device_index) / sizeof(device_index[0]))) {
		snprintf(device_index[n_device_index].name, sizeof(device_index[0].name), "%s", name);
		device_index[n_device_index].playback = playback;
		device_index[n_device_index++].index = idx;
	}
	return idx;
}

/* ------------------------------------------------------------ operations */

static void op_unlink(pa_operation *o)
{
	if (o->context == NULL)
		return;
	for (pa_operation **p = &o->context->ops; *p; p = &(*p)->next) {
		if (*p == o) {
			*p = o->next;
			break;
		}
	}
}

static void op_set_state(pa_operation *o, pa_operation_state_t st)
{
	if (o->state == st)
		return;
	o->state = st;
	if (o->state_cb)
		o->state_cb(o, o->state_userdata);
}

static void op_free_defer(pa_operation *o)
{
	if (o->defer && o->context) {
		o->context->api->defer_free(o->defer);
		o->defer = NULL;
	}
}

static void drop_owned(pa_operation *o)
{
	if (o->owned) {
		o->owned = false;
		pa_operation_unref(o);
	}
}

static void op_defer_cb(pa_mainloop_api *a, pa_defer_event *e, void *userdata)
{
	pa_operation *o = userdata;
	op_free_defer(o);
	if (o->state != PA_OPERATION_RUNNING)
		return;
	/* off the context's list first: what the callback does (disconnect a
	 * stream, cancel operations) no longer reaches this one */
	op_unlink(o);
	pa_operation_ref(o);
	if (o->run)
		o->run(o);
	if (o->state == PA_OPERATION_RUNNING)
		op_set_state(o, PA_OPERATION_DONE);
	drop_owned(o);
	pa_operation_unref(o);
}

pa_operation *shim_operation_new(pa_context *c, pa_stream *s, void (*run)(pa_operation *o),
		void *cb, void *userdata)
{
	pa_operation *o = calloc(1, sizeof(*o));
	if (o == NULL)
		return NULL;
	o->refs = 2;			/* the application's, and ours until done */
	o->owned = true;
	o->context = c;
	o->stream = s;
	o->state = PA_OPERATION_RUNNING;
	o->run = run;
	o->cb = cb;
	o->userdata = userdata;
	o->data.success = 1;
	o->next = c->ops;
	c->ops = o;
	if (run)
		o->defer = c->api->defer_new(c->api, op_defer_cb, o);
	return o;
}

struct shim_op_data *shim_operation_data(pa_operation *o)
{
	return &o->data;
}

pa_stream *shim_operation_stream(pa_operation *o)
{
	return o->stream;
}

pa_context *shim_operation_context(pa_operation *o)
{
	return o->context;
}

void *shim_operation_callback(pa_operation *o, void **userdata)
{
	*userdata = o->userdata;
	return o->cb;
}

static void run_stream_success(pa_operation *o)
{
	if (o->cb)
		((pa_stream_success_cb_t)o->cb)(o->stream, o->data.success, o->userdata);
}

void shim_operation_finish(pa_operation *o, int success)
{
	if (o->state != PA_OPERATION_RUNNING || o->defer)
		return;
	o->data.success = success;
	if (o->run == NULL)
		o->run = run_stream_success;
	o->defer = o->context->api->defer_new(o->context->api, op_defer_cb, o);
}

static void op_cancel(pa_operation *o)
{
	if (o->state != PA_OPERATION_RUNNING)
		return;
	op_free_defer(o);
	op_unlink(o);
	op_set_state(o, PA_OPERATION_CANCELLED);
	drop_owned(o);
}

void shim_operation_cancel_all(pa_context *c, pa_stream *s)
{
	for (;;) {
		pa_operation *o;
		for (o = c->ops; o; o = o->next)
			if (s == NULL || o->stream == s)
				break;
		if (o == NULL)
			return;
		op_cancel(o);
	}
}

SHIM_EXPORT pa_operation *pa_operation_ref(pa_operation *o)
{
	o->refs++;
	return o;
}

SHIM_EXPORT void pa_operation_unref(pa_operation *o)
{
	if (o == NULL || --o->refs > 0)
		return;
	free(o->data.name);
	free(o);
}

SHIM_EXPORT void pa_operation_cancel(pa_operation *o)
{
	op_cancel(o);
}

SHIM_EXPORT pa_operation_state_t pa_operation_get_state(const pa_operation *o)
{
	return o->state;
}

SHIM_EXPORT void pa_operation_set_state_callback(pa_operation *o, pa_operation_notify_cb_t cb, void *userdata)
{
	o->state_cb = cb;
	o->state_userdata = userdata;
}

/* ---------------------------------------------------------------- state */

static void set_state(pa_context *c, pa_context_state_t st)
{
	if (c->state == st)
		return;
	pa_context_ref(c);
	c->state = st;
	if (c->state_cb)
		c->state_cb(c, c->state_userdata);
	pa_context_unref(c);
}

void shim_context_set_errno(pa_context *c, int error)
{
	c->error = error;
}

int shim_context_fail(pa_context *c, int error)
{
	c->error = error;
	set_state(c, PA_CONTEXT_FAILED);
	return -error;
}

pa_mainloop_api *shim_context_api(pa_context *c)
{
	return c->api;
}

const char *shim_context_app_name(pa_context *c)
{
	const char *n = c->props ? pa_proplist_gets(c->props, PA_PROP_APPLICATION_NAME) : NULL;
	return n && n[0] ? n : c->name;
}

uint32_t shim_context_new_stream_index(pa_context *c)
{
	return c->next_stream_index++;
}

/* ---------------------------------------------------------------- devices */

static void scan_devices(pa_context *c, struct shim_device *out, int *n_out)
{
	jack_client_t *client = shim_node_client();
	int n = 0;
	if (client == NULL) {
		*n_out = 0;
		return;
	}
	for (int playback = 1; playback >= 0; playback--) {
		const char **ports = jack_get_ports(client, NULL, JACK_DEFAULT_AUDIO_TYPE,
				JackPortIsPhysical | (playback ? JackPortIsInput : JackPortIsOutput));
		for (size_t i = 0; ports && ports[i]; i++) {
			const char *colon = strchr(ports[i], ':');
			if (colon == NULL)
				continue;
			char who[128];
			snprintf(who, sizeof(who), "%.*s", (int)(colon - ports[i]), ports[i]);
			int k;
			for (k = 0; k < n; k++)
				if (out[k].playback == playback && strcmp(out[k].client, who) == 0)
					break;
			if (k == n) {
				if (n == MAX_DEVICES)
					continue;
				struct shim_device *d = &out[n++];
				memset(d, 0, sizeof(*d));
				d->playback = playback;
				snprintf(d->client, sizeof(d->client), "%s", who);
				snprintf(d->name, sizeof(d->name), "jackbus.%s.%s", who, playback ? "playback" : "capture");
				snprintf(d->desc, sizeof(d->desc), "%s %s (JackBus)", who, playback ? "playback" : "capture");
				d->index = index_of(d->name, playback);
			}
			out[k].channels++;
		}
		jack_free(ports);
	}
	*n_out = n;
}

static void notify_sub(pa_context *c, pa_subscription_event_type_t t, uint32_t idx)
{
	static const pa_subscription_mask_t masks[] = {
		[PA_SUBSCRIPTION_EVENT_SINK] = PA_SUBSCRIPTION_MASK_SINK,
		[PA_SUBSCRIPTION_EVENT_SOURCE] = PA_SUBSCRIPTION_MASK_SOURCE,
		[PA_SUBSCRIPTION_EVENT_SINK_INPUT] = PA_SUBSCRIPTION_MASK_SINK_INPUT,
		[PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT] = PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT,
	};
	unsigned f = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
	pa_subscription_mask_t m = f == PA_SUBSCRIPTION_EVENT_SERVER ? PA_SUBSCRIPTION_MASK_SERVER :
		f < sizeof(masks) / sizeof(masks[0]) ? masks[f] : 0;
	if (c->sub_cb && (c->sub_mask & m))
		c->sub_cb(c, t, idx, c->sub_userdata);
}

static void rescan(pa_context *c, bool notify)
{
	struct shim_device now[MAX_DEVICES];
	int n = 0;
	scan_devices(c, now, &n);
	const struct shim_device *old_sink = shim_context_default_device(c, true);
	const struct shim_device *old_source = shim_context_default_device(c, false);
	uint32_t old_sink_idx = old_sink ? old_sink->index : PA_INVALID_INDEX;
	uint32_t old_source_idx = old_source ? old_source->index : PA_INVALID_INDEX;
	if (notify) {
		for (int i = 0; i < c->n_devices; i++) {
			bool gone = true;
			for (int k = 0; k < n; k++)
				if (now[k].playback == c->devices[i].playback && now[k].index == c->devices[i].index)
					gone = false;
			if (gone)
				notify_sub(c, (c->devices[i].playback ? PA_SUBSCRIPTION_EVENT_SINK :
						PA_SUBSCRIPTION_EVENT_SOURCE) | PA_SUBSCRIPTION_EVENT_REMOVE,
						c->devices[i].index);
		}
	}
	int old_n = c->n_devices;
	struct shim_device old[MAX_DEVICES];
	memcpy(old, c->devices, sizeof(old));
	memcpy(c->devices, now, sizeof(now));
	c->n_devices = n;
	if (!notify)
		return;
	for (int k = 0; k < n; k++) {
		bool fresh = true;
		for (int i = 0; i < old_n; i++)
			if (old[i].playback == now[k].playback && old[i].index == now[k].index)
				fresh = false;
		if (fresh)
			notify_sub(c, (now[k].playback ? PA_SUBSCRIPTION_EVENT_SINK : PA_SUBSCRIPTION_EVENT_SOURCE) |
					PA_SUBSCRIPTION_EVENT_NEW, now[k].index);
	}
	const struct shim_device *sink = shim_context_default_device(c, true);
	const struct shim_device *source = shim_context_default_device(c, false);
	if ((sink ? sink->index : PA_INVALID_INDEX) != old_sink_idx ||
	    (source ? source->index : PA_INVALID_INDEX) != old_source_idx)
		notify_sub(c, PA_SUBSCRIPTION_EVENT_SERVER | PA_SUBSCRIPTION_EVENT_CHANGE, PA_INVALID_INDEX);
}

const struct shim_device *shim_context_default_device(pa_context *c, bool playback)
{
	for (int i = 0; i < c->n_devices; i++)
		if (c->devices[i].playback == playback)
			return &c->devices[i];
	return NULL;
}

const struct shim_device *shim_context_device(pa_context *c, bool playback, const char *name, uint32_t index)
{
	if (name && (strcmp(name, "@DEFAULT_SINK@") == 0 || strcmp(name, "@DEFAULT_SOURCE@") == 0 ||
	    strcmp(name, "@DEFAULT_MONITOR@") == 0))
		return shim_context_default_device(c, playback);
	for (int i = 0; i < c->n_devices; i++) {
		const struct shim_device *d = &c->devices[i];
		if (d->playback != playback)
			continue;
		if (name ? (strcmp(d->name, name) == 0 || strcmp(d->client, name) == 0) : d->index == index)
			return d;
	}
	return NULL;
}

/* the node's events: devices changed, or the server went away */
static void on_node_event(pa_mainloop_api *a, pa_io_event *e, int fd, pa_io_event_flags_t f, void *userdata)
{
	pa_context *c = userdata;
	uint64_t v;
	ssize_t r = read(fd, &v, sizeof(v));
	(void)r;
	if (c->state != PA_CONTEXT_READY)
		return;
	if (shim_node_dead()) {
		pa_context_ref(c);
		for (int i = c->n_streams - 1; i >= 0; i--)
			shim_stream_fail(c->streams[i], PA_ERR_CONNECTIONTERMINATED);
		shim_context_fail(c, PA_ERR_CONNECTIONTERMINATED);
		pa_context_unref(c);
		return;
	}
	rescan(c, true);
}

/* ----------------------------------------------------------------- streams */

void shim_context_stream_add(pa_context *c, pa_stream *s)
{
	if (c->n_streams == c->cap_streams) {
		int cap = c->cap_streams ? c->cap_streams * 2 : 8;
		pa_stream **p = realloc(c->streams, cap * sizeof(*p));
		if (p == NULL)
			return;
		c->streams = p;
		c->cap_streams = cap;
	}
	c->streams[c->n_streams++] = s;
	notify_sub(c, (shim_stream_playback(s) ? PA_SUBSCRIPTION_EVENT_SINK_INPUT :
			PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT) | PA_SUBSCRIPTION_EVENT_NEW, shim_stream_index(s));
}

void shim_context_stream_remove(pa_context *c, pa_stream *s)
{
	for (int i = 0; i < c->n_streams; i++) {
		if (c->streams[i] == s) {
			c->streams[i] = c->streams[--c->n_streams];
			notify_sub(c, (shim_stream_playback(s) ? PA_SUBSCRIPTION_EVENT_SINK_INPUT :
					PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT) | PA_SUBSCRIPTION_EVENT_REMOVE,
					shim_stream_index(s));
			return;
		}
	}
}

pa_stream *shim_context_find_stream(pa_context *c, bool playback, uint32_t index)
{
	for (int i = 0; i < c->n_streams; i++)
		if (shim_stream_playback(c->streams[i]) == playback && shim_stream_index(c->streams[i]) == index)
			return c->streams[i];
	return NULL;
}

/* ------------------------------------------------------------------- API */

SHIM_EXPORT pa_context *pa_context_new_with_proplist(pa_mainloop_api *api, const char *name,
		const pa_proplist *proplist)
{
	pa_context *c = calloc(1, sizeof(*c));
	if (c == NULL)
		return NULL;
	c->refs = 1;
	c->api = api;
	c->name = strdup(name ? name : "pulse-app");
	c->props = proplist ? pa_proplist_copy(proplist) : pa_proplist_new();
	if (name && !pa_proplist_contains(c->props, PA_PROP_APPLICATION_NAME))
		pa_proplist_sets(c->props, PA_PROP_APPLICATION_NAME, name);
	c->state = PA_CONTEXT_UNCONNECTED;
	c->efd = -1;
	return c;
}

SHIM_EXPORT pa_context *pa_context_new(pa_mainloop_api *api, const char *name)
{
	return pa_context_new_with_proplist(api, name, NULL);
}

static void teardown(pa_context *c)
{
	shim_operation_cancel_all(c, NULL);
	if (c->connect_defer) {
		c->api->defer_free(c->connect_defer);
		c->connect_defer = NULL;
	}
	if (c->retry) {
		c->api->time_free(c->retry);
		c->retry = NULL;
	}
	if (c->efd_io) {
		c->api->io_free(c->efd_io);
		c->efd_io = NULL;
	}
	if (c->efd >= 0) {
		shim_node_watch(c->efd, false);
		close(c->efd);
		c->efd = -1;
	}
	if (c->node_ref) {
		c->node_ref = false;
		shim_node_unref();
	}
}

SHIM_EXPORT pa_context *pa_context_ref(pa_context *c)
{
	c->refs++;
	return c;
}

SHIM_EXPORT void pa_context_unref(pa_context *c)
{
	if (c == NULL || --c->refs > 0)
		return;
	teardown(c);
	free(c->streams);
	if (c->props)
		pa_proplist_free(c->props);
	free(c->name);
	free(c);
}

SHIM_EXPORT void pa_context_set_state_callback(pa_context *c, pa_context_notify_cb_t cb, void *userdata)
{
	c->state_cb = cb;
	c->state_userdata = userdata;
}

SHIM_EXPORT void pa_context_set_event_callback(pa_context *c, pa_context_event_cb_t cb, void *userdata)
{
	(void)c; (void)cb; (void)userdata;
}

SHIM_EXPORT int pa_context_errno(const pa_context *c)
{
	return c ? c->error : PA_ERR_INVALID;
}

SHIM_EXPORT int pa_context_is_pending(const pa_context *c)
{
	return c->ops != NULL;
}

SHIM_EXPORT pa_context_state_t pa_context_get_state(const pa_context *c)
{
	return c->state;
}

static void try_connect(pa_context *c);

static void on_retry(pa_mainloop_api *a, pa_time_event *e, const struct timeval *tv, void *userdata)
{
	try_connect(userdata);
}

static void try_connect(pa_context *c)
{
	if (shim_node_ref(shim_context_app_name(c)) == NULL) {
		if (c->flags & PA_CONTEXT_NOFAIL) {
			struct timeval tv;
			if (c->retry == NULL)
				c->retry = c->api->time_new(c->api, shim_rttime(&tv, shim_now_usec() + 1000000), on_retry, c);
			else
				c->api->time_restart(c->retry, shim_rttime(&tv, shim_now_usec() + 1000000));
			return;
		}
		static bool warned;		/* once: an application may retry for ever */
		if (!warned)
			shim_warn("JackBus is not running");
		warned = true;
		shim_context_fail(c, PA_ERR_CONNECTIONREFUSED);
		return;
	}
	c->node_ref = true;
	c->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (c->efd >= 0) {
		c->efd_io = c->api->io_new(c->api, c->efd, PA_IO_EVENT_INPUT, on_node_event, c);
		shim_node_watch(c->efd, true);
	}
	rescan(c, false);
	pa_context_ref(c);
	set_state(c, PA_CONTEXT_AUTHORIZING);
	if (c->state == PA_CONTEXT_AUTHORIZING)
		set_state(c, PA_CONTEXT_SETTING_NAME);
	if (c->state == PA_CONTEXT_SETTING_NAME)
		set_state(c, PA_CONTEXT_READY);
	pa_context_unref(c);
}

static void on_connect(pa_mainloop_api *a, pa_defer_event *e, void *userdata)
{
	pa_context *c = userdata;
	a->defer_free(e);
	c->connect_defer = NULL;
	try_connect(c);
}

SHIM_EXPORT int pa_context_connect(pa_context *c, const char *server, pa_context_flags_t flags,
		const pa_spawn_api *api)
{
	(void)server; (void)api;
	if (c->state != PA_CONTEXT_UNCONNECTED) {
		c->error = PA_ERR_BADSTATE;
		return -PA_ERR_BADSTATE;
	}
	c->flags = flags;
	c->connect_defer = c->api->defer_new(c->api, on_connect, c);
	set_state(c, PA_CONTEXT_CONNECTING);
	return 0;
}

SHIM_EXPORT void pa_context_disconnect(pa_context *c)
{
	if (c->state == PA_CONTEXT_TERMINATED || c->state == PA_CONTEXT_UNCONNECTED)
		return;
	pa_context_ref(c);
	for (int i = c->n_streams - 1; i >= 0; i--)
		pa_stream_disconnect(c->streams[i]);
	teardown(c);
	set_state(c, PA_CONTEXT_TERMINATED);
	pa_context_unref(c);
}

SHIM_EXPORT pa_operation *pa_context_drain(pa_context *c, pa_context_notify_cb_t cb, void *userdata)
{
	(void)cb; (void)userdata;
	c->error = PA_ERR_BADSTATE;
	return NULL;		/* nothing is ever pending towards a server */
}

static void run_context_success(pa_operation *o)
{
	if (o->cb)
		((pa_context_success_cb_t)o->cb)(o->context, o->data.success, o->userdata);
}

static pa_operation *success_op(pa_context *c, pa_context_success_cb_t cb, void *userdata, int success)
{
	pa_operation *o = shim_operation_new(c, NULL, run_context_success, (void *)cb, userdata);
	if (o)
		o->data.success = success;
	return o;
}

pa_operation *shim_context_success_op(pa_context *c, pa_context_success_cb_t cb, void *userdata,
		int success, int error)
{
	if (!success)
		c->error = error;
	return success_op(c, cb, userdata, success);
}

SHIM_EXPORT pa_operation *pa_context_exit_daemon(pa_context *c, pa_context_success_cb_t cb, void *userdata)
{
	return success_op(c, cb, userdata, 0);
}

SHIM_EXPORT pa_operation *pa_context_set_default_sink(pa_context *c, const char *name,
		pa_context_success_cb_t cb, void *userdata)
{
	return success_op(c, cb, userdata, 1);
}

SHIM_EXPORT pa_operation *pa_context_set_default_source(pa_context *c, const char *name,
		pa_context_success_cb_t cb, void *userdata)
{
	return success_op(c, cb, userdata, 1);
}

SHIM_EXPORT int pa_context_is_local(const pa_context *c)
{
	return 1;
}

SHIM_EXPORT pa_operation *pa_context_set_name(pa_context *c, const char *name,
		pa_context_success_cb_t cb, void *userdata)
{
	return success_op(c, cb, userdata, 1);
}

SHIM_EXPORT const char *pa_context_get_server(const pa_context *c)
{
	return "jackbus";
}

SHIM_EXPORT uint32_t pa_context_get_protocol_version(const pa_context *c)
{
	return PA_PROTOCOL_VERSION;
}

SHIM_EXPORT uint32_t pa_context_get_server_protocol_version(const pa_context *c)
{
	return PA_PROTOCOL_VERSION;
}

SHIM_EXPORT pa_operation *pa_context_proplist_update(pa_context *c, pa_update_mode_t mode,
		const pa_proplist *p, pa_context_success_cb_t cb, void *userdata)
{
	if (p)
		pa_proplist_update(c->props, mode, p);
	return success_op(c, cb, userdata, 1);
}

SHIM_EXPORT pa_operation *pa_context_proplist_remove(pa_context *c, const char *const keys[],
		pa_context_success_cb_t cb, void *userdata)
{
	return success_op(c, cb, userdata, 1);
}

SHIM_EXPORT uint32_t pa_context_get_index(const pa_context *c)
{
	return 1;
}

SHIM_EXPORT pa_time_event *pa_context_rttime_new(const pa_context *c, pa_usec_t usec,
		pa_time_event_cb_t cb, void *userdata)
{
	struct timeval tv;
	return c->api->time_new(c->api, usec == PA_USEC_INVALID ? NULL : shim_rttime(&tv, usec), cb, userdata);
}

SHIM_EXPORT void pa_context_rttime_restart(const pa_context *c, pa_time_event *e, pa_usec_t usec)
{
	struct timeval tv;
	c->api->time_restart(e, usec == PA_USEC_INVALID ? NULL : shim_rttime(&tv, usec));
}

SHIM_EXPORT size_t pa_context_get_tile_size(const pa_context *c, const pa_sample_spec *ss)
{
	size_t fs = ss ? pa_frame_size(ss) : 1;
	size_t t = 65536;
	return fs ? t - (t % fs) : t;
}

SHIM_EXPORT int pa_context_load_cookie_from_file(pa_context *c, const char *cookie_file_path)
{
	return 0;
}

/* ---------------------------------------------------------- introspection */

static void run_server_info(pa_operation *o)
{
	pa_context *c = o->context;
	jack_client_t *client = shim_node_client();
	struct passwd *pw = getpwuid(getuid());
	char host[256] = "";
	gethostname(host, sizeof(host) - 1);
	const struct shim_device *sink = shim_context_default_device(c, true);
	const struct shim_device *source = shim_context_default_device(c, false);
	pa_server_info i;
	memset(&i, 0, sizeof(i));
	i.user_name = pw ? pw->pw_name : "";
	i.host_name = host;
	i.server_version = "17.0.0";
	i.server_name = "PulseAudio (on JackBus)";
	i.sample_spec.format = PA_SAMPLE_FLOAT32LE;
	i.sample_spec.rate = client ? jack_get_sample_rate(client) : 48000;
	i.sample_spec.channels = 2;
	i.default_sink_name = sink ? sink->name : NULL;
	i.default_source_name = source ? source->name : NULL;
	i.cookie = 0x4a42;
	pa_channel_map_init_stereo(&i.channel_map);
	if (o->cb)
		((pa_server_info_cb_t)o->cb)(c, &i, o->userdata);
}

SHIM_EXPORT pa_operation *pa_context_get_server_info(pa_context *c, pa_server_info_cb_t cb, void *userdata)
{
	return shim_operation_new(c, NULL, run_server_info, (void *)cb, userdata);
}

static pa_usec_t device_latency(const struct shim_device *d)
{
	jack_client_t *client = shim_node_client();
	char name[300];
	if (client == NULL)
		return 0;
	/* the first physical port of the device */
	const char **ports = jack_get_ports(client, NULL, JACK_DEFAULT_AUDIO_TYPE,
			JackPortIsPhysical | (d->playback ? JackPortIsInput : JackPortIsOutput));
	pa_usec_t usec = 0;
	snprintf(name, sizeof(name), "%s:", d->client);
	for (size_t i = 0; ports && ports[i]; i++) {
		if (strncmp(ports[i], name, strlen(name)) != 0)
			continue;
		jack_port_t *p = jack_port_by_name(client, ports[i]);
		jack_latency_range_t r = { 0, 0 };
		if (p)
			jack_port_get_latency_range(p, d->playback ? JackPlaybackLatency : JackCaptureLatency, &r);
		usec = (pa_usec_t)r.max * 1000000ull / SHIM_MAX(jack_get_sample_rate(client), 1u);
		break;
	}
	jack_free(ports);
	return usec;
}

static void device_spec(const struct shim_device *d, pa_sample_spec *ss, pa_channel_map *map)
{
	jack_client_t *client = shim_node_client();
	ss->format = PA_SAMPLE_FLOAT32LE;
	ss->rate = client ? jack_get_sample_rate(client) : 48000;
	ss->channels = (uint8_t)SHIM_MIN(SHIM_MAX(d->channels, 1u), (uint32_t)PA_CHANNELS_MAX);
	if (!pa_channel_map_init_auto(map, ss->channels, PA_CHANNEL_MAP_AUX))
		pa_channel_map_init_extend(map, ss->channels, PA_CHANNEL_MAP_AUX);
	if (ss->channels == 2)
		pa_channel_map_init_stereo(map);
	else if (ss->channels == 1)
		pa_channel_map_init_mono(map);
}

/* one sink (or source) to the callback */
static void emit_device(pa_operation *o, const struct shim_device *d)
{
	pa_context *c = o->context;
	pa_sample_spec ss;
	pa_channel_map map;
	pa_cvolume vol;
	device_spec(d, &ss, &map);
	pa_cvolume_set(&vol, ss.channels, PA_VOLUME_NORM);
	pa_proplist *p = pa_proplist_new();
	pa_proplist_sets(p, PA_PROP_DEVICE_DESCRIPTION, d->desc);
	pa_proplist_sets(p, PA_PROP_DEVICE_CLASS, "sound");
	pa_proplist_sets(p, PA_PROP_DEVICE_API, "jackbus");
	pa_proplist_sets(p, "jack.client-name", d->client);
	pa_format_info *f = pa_format_info_from_sample_spec(&ss, &map);
	pa_format_info *formats[1] = { f };
	pa_usec_t lat = device_latency(d);

	if (d->playback) {
		pa_sink_info i;
		memset(&i, 0, sizeof(i));
		i.name = d->name;
		i.index = d->index;
		i.description = d->desc;
		i.sample_spec = ss;
		i.channel_map = map;
		i.owner_module = PA_INVALID_INDEX;
		i.volume = vol;
		i.monitor_source = PA_INVALID_INDEX;
		i.latency = lat;
		i.driver = "jackbus";
		i.flags = PA_SINK_HARDWARE | PA_SINK_LATENCY;
		i.proplist = p;
		i.configured_latency = lat;
		i.base_volume = PA_VOLUME_NORM;
		i.state = PA_SINK_RUNNING;
		i.n_volume_steps = PA_VOLUME_NORM + 1;
		i.card = PA_INVALID_INDEX;
		i.n_formats = f ? 1 : 0;
		i.formats = f ? formats : NULL;
		((pa_sink_info_cb_t)o->cb)(c, &i, 0, o->userdata);
	} else {
		pa_source_info i;
		memset(&i, 0, sizeof(i));
		i.name = d->name;
		i.index = d->index;
		i.description = d->desc;
		i.sample_spec = ss;
		i.channel_map = map;
		i.owner_module = PA_INVALID_INDEX;
		i.volume = vol;
		i.monitor_of_sink = PA_INVALID_INDEX;
		i.latency = lat;
		i.driver = "jackbus";
		i.flags = PA_SOURCE_HARDWARE | PA_SOURCE_LATENCY;
		i.proplist = p;
		i.configured_latency = lat;
		i.base_volume = PA_VOLUME_NORM;
		i.state = PA_SOURCE_RUNNING;
		i.n_volume_steps = PA_VOLUME_NORM + 1;
		i.card = PA_INVALID_INDEX;
		i.n_formats = f ? 1 : 0;
		i.formats = f ? formats : NULL;
		((pa_source_info_cb_t)o->cb)(c, &i, 0, o->userdata);
	}
	if (f)
		pa_format_info_free(f);
	pa_proplist_free(p);
}

static void end_list(pa_operation *o, bool playback)
{
	if (playback)
		((pa_sink_info_cb_t)o->cb)(o->context, NULL, 1, o->userdata);
	else
		((pa_source_info_cb_t)o->cb)(o->context, NULL, 1, o->userdata);
}

/* data.index: 0 sinks, 1 sources; data.name / data.success (by index) selects one */
enum { SEL_ALL, SEL_NAME, SEL_INDEX };

static void run_devices(pa_operation *o)
{
	pa_context *c = o->context;
	bool playback = (o->data.index & 1) == 0;
	int sel = (int)(o->data.index >> 1);
	uint32_t want = (uint32_t)o->data.success;
	if (o->cb == NULL)
		return;
	if (sel == SEL_ALL) {
		for (int i = 0; i < c->n_devices; i++)
			if (c->devices[i].playback == playback)
				emit_device(o, &c->devices[i]);
		end_list(o, playback);
		return;
	}
	const struct shim_device *d = shim_context_device(c, playback, sel == SEL_NAME ? o->data.name : NULL, want);
	if (d) {
		emit_device(o, d);
		end_list(o, playback);
	} else {
		c->error = PA_ERR_NOENTITY;
		/* PulseAudio: eol < 0 for no such entity */
		if (playback)
			((pa_sink_info_cb_t)o->cb)(c, NULL, -1, o->userdata);
		else
			((pa_source_info_cb_t)o->cb)(c, NULL, -1, o->userdata);
	}
}

static pa_operation *devices_op(pa_context *c, bool playback, int sel, const char *name, uint32_t idx,
		void *cb, void *userdata)
{
	pa_operation *o = shim_operation_new(c, NULL, run_devices, cb, userdata);
	if (o == NULL)
		return NULL;
	o->data.index = (uint32_t)(sel << 1) | (playback ? 0 : 1);
	o->data.name = name ? strdup(name) : NULL;
	o->data.success = (int)idx;
	return o;
}

SHIM_EXPORT pa_operation *pa_context_get_sink_info_list(pa_context *c, pa_sink_info_cb_t cb, void *userdata)
{
	return devices_op(c, true, SEL_ALL, NULL, 0, (void *)cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_get_sink_info_by_name(pa_context *c, const char *name,
		pa_sink_info_cb_t cb, void *userdata)
{
	return devices_op(c, true, SEL_NAME, name, 0, (void *)cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_get_sink_info_by_index(pa_context *c, uint32_t idx,
		pa_sink_info_cb_t cb, void *userdata)
{
	return devices_op(c, true, SEL_INDEX, NULL, idx, (void *)cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_get_source_info_list(pa_context *c, pa_source_info_cb_t cb, void *userdata)
{
	return devices_op(c, false, SEL_ALL, NULL, 0, (void *)cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_get_source_info_by_name(pa_context *c, const char *name,
		pa_source_info_cb_t cb, void *userdata)
{
	return devices_op(c, false, SEL_NAME, name, 0, (void *)cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_get_source_info_by_index(pa_context *c, uint32_t idx,
		pa_source_info_cb_t cb, void *userdata)
{
	return devices_op(c, false, SEL_INDEX, NULL, idx, (void *)cb, userdata);
}

/* device volumes: JackBus's devices have none of their own */
static pa_operation *device_volume_op(pa_context *c, pa_context_success_cb_t cb, void *userdata, const char *what)
{
	static bool logged;
	if (!logged)
		shim_log("%s: JackBus's devices have no volume of their own (accepted, not applied)", what);
	logged = true;
	return success_op(c, cb, userdata, 1);
}

SHIM_EXPORT pa_operation *pa_context_set_sink_volume_by_index(pa_context *c, uint32_t idx,
		const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	return device_volume_op(c, cb, userdata, "sink volume");
}

SHIM_EXPORT pa_operation *pa_context_set_sink_volume_by_name(pa_context *c, const char *name,
		const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	return device_volume_op(c, cb, userdata, "sink volume");
}

SHIM_EXPORT pa_operation *pa_context_set_sink_mute_by_index(pa_context *c, uint32_t idx, int mute,
		pa_context_success_cb_t cb, void *userdata)
{
	return device_volume_op(c, cb, userdata, "sink mute");
}

SHIM_EXPORT pa_operation *pa_context_set_sink_mute_by_name(pa_context *c, const char *name, int mute,
		pa_context_success_cb_t cb, void *userdata)
{
	return device_volume_op(c, cb, userdata, "sink mute");
}

SHIM_EXPORT pa_operation *pa_context_set_source_volume_by_index(pa_context *c, uint32_t idx,
		const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	return device_volume_op(c, cb, userdata, "source volume");
}

SHIM_EXPORT pa_operation *pa_context_set_source_volume_by_name(pa_context *c, const char *name,
		const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	return device_volume_op(c, cb, userdata, "source volume");
}

SHIM_EXPORT pa_operation *pa_context_set_source_mute_by_index(pa_context *c, uint32_t idx, int mute,
		pa_context_success_cb_t cb, void *userdata)
{
	return device_volume_op(c, cb, userdata, "source mute");
}

SHIM_EXPORT pa_operation *pa_context_set_source_mute_by_name(pa_context *c, const char *name, int mute,
		pa_context_success_cb_t cb, void *userdata)
{
	return device_volume_op(c, cb, userdata, "source mute");
}

/* this application's own streams: sink inputs and source outputs */

/* one stream's info to the operation's callback (eol 0) */
static void emit_stream_info(pa_operation *o, pa_stream *s, bool playback)
{
	pa_context *c = o->context;
	pa_cvolume vol;
	int mute = 0;
	shim_stream_get_volume(s, &vol, &mute);
	pa_proplist *p = pa_proplist_new();
	pa_proplist_sets(p, PA_PROP_MEDIA_NAME, shim_stream_name(s));
	pa_proplist_sets(p, PA_PROP_APPLICATION_NAME, shim_context_app_name(c));
	pa_format_info *f = pa_format_info_from_sample_spec(shim_stream_spec(s), shim_stream_map(s));
	if (playback) {
		pa_sink_input_info i;
		memset(&i, 0, sizeof(i));
		i.index = shim_stream_index(s);
		i.name = shim_stream_name(s);
		i.owner_module = PA_INVALID_INDEX;
		i.client = 1;
		i.sink = shim_stream_device(s);
		i.sample_spec = *shim_stream_spec(s);
		i.channel_map = *shim_stream_map(s);
		i.volume = vol;
		i.buffer_usec = shim_stream_latency_usec(s);
		i.resample_method = "none";
		i.driver = "jackbus";
		i.mute = mute;
		i.proplist = p;
		i.has_volume = 1;
		i.volume_writable = 1;
		i.format = f;
		((pa_sink_input_info_cb_t)o->cb)(c, &i, 0, o->userdata);
	} else {
		pa_source_output_info i;
		memset(&i, 0, sizeof(i));
		i.index = shim_stream_index(s);
		i.name = shim_stream_name(s);
		i.owner_module = PA_INVALID_INDEX;
		i.client = 1;
		i.source = shim_stream_device(s);
		i.sample_spec = *shim_stream_spec(s);
		i.channel_map = *shim_stream_map(s);
		i.buffer_usec = shim_stream_latency_usec(s);
		i.resample_method = "none";
		i.driver = "jackbus";
		i.proplist = p;
		i.volume = vol;
		i.mute = mute;
		i.has_volume = 1;
		i.volume_writable = 1;
		i.format = f;
		((pa_source_output_info_cb_t)o->cb)(c, &i, 0, o->userdata);
	}
	if (f)
		pa_format_info_free(f);
	pa_proplist_free(p);
}

static void end_stream_list(pa_operation *o, bool playback, int eol)
{
	if (playback)
		((pa_sink_input_info_cb_t)o->cb)(o->context, NULL, eol, o->userdata);
	else
		((pa_source_output_info_cb_t)o->cb)(o->context, NULL, eol, o->userdata);
}

/* data.name: NULL for sink inputs, "" for source outputs; data.success 2: the list */
static void run_stream_info(pa_operation *o)
{
	pa_context *c = o->context;
	bool playback = o->data.name == NULL;
	if (o->cb == NULL)
		return;
	if (o->data.success == 2) {
		/* by index: a callback may disconnect a stream (the array shrinks) */
		for (int i = 0; i < c->n_streams; i++) {
			pa_stream *s = c->streams[i];
			if (shim_stream_playback(s) == playback && pa_stream_get_state(s) == PA_STREAM_READY)
				emit_stream_info(o, s, playback);
		}
		end_stream_list(o, playback, 1);
		return;
	}
	pa_stream *s = shim_context_find_stream(c, playback, o->data.index);
	if (s == NULL) {
		c->error = PA_ERR_NOENTITY;
		end_stream_list(o, playback, -1);
		return;
	}
	emit_stream_info(o, s, playback);
	end_stream_list(o, playback, 1);
}

SHIM_EXPORT pa_operation *pa_context_get_sink_input_info(pa_context *c, uint32_t idx,
		pa_sink_input_info_cb_t cb, void *userdata)
{
	pa_operation *o = shim_operation_new(c, NULL, run_stream_info, (void *)cb, userdata);
	if (o)
		o->data.index = idx;
	return o;
}

SHIM_EXPORT pa_operation *pa_context_get_source_output_info(pa_context *c, uint32_t idx,
		pa_source_output_info_cb_t cb, void *userdata)
{
	pa_operation *o = shim_operation_new(c, NULL, run_stream_info, (void *)cb, userdata);
	if (o) {
		o->data.index = idx;
		o->data.name = strdup("");
	}
	return o;
}

SHIM_EXPORT pa_operation *pa_context_get_sink_input_info_list(pa_context *c, pa_sink_input_info_cb_t cb,
		void *userdata)
{
	pa_operation *o = shim_operation_new(c, NULL, run_stream_info, (void *)cb, userdata);
	if (o)
		o->data.success = 2;
	return o;
}

SHIM_EXPORT pa_operation *pa_context_get_source_output_info_list(pa_context *c, pa_source_output_info_cb_t cb,
		void *userdata)
{
	pa_operation *o = shim_operation_new(c, NULL, run_stream_info, (void *)cb, userdata);
	if (o) {
		o->data.success = 2;
		o->data.name = strdup("");
	}
	return o;
}

/* killing one of this application's streams: it fails, PA_ERR_KILLED (as a
 * server kills a sink input), when the operation runs */
static void run_kill(pa_operation *o)
{
	pa_context *c = o->context;
	bool playback = o->data.name == NULL;
	pa_stream *s = shim_context_find_stream(c, playback, o->data.index);
	if (s)
		shim_stream_fail(s, PA_ERR_KILLED);
	else
		c->error = PA_ERR_NOENTITY;
	o->data.success = s != NULL;
	run_context_success(o);
}

static pa_operation *kill_op(pa_context *c, bool playback, uint32_t idx, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o = shim_operation_new(c, NULL, run_kill, (void *)cb, userdata);
	if (o) {
		o->data.index = idx;
		if (!playback)
			o->data.name = strdup("");
	}
	return o;
}

SHIM_EXPORT pa_operation *pa_context_kill_sink_input(pa_context *c, uint32_t idx,
		pa_context_success_cb_t cb, void *userdata)
{
	return kill_op(c, true, idx, cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_kill_source_output(pa_context *c, uint32_t idx,
		pa_context_success_cb_t cb, void *userdata)
{
	return kill_op(c, false, idx, cb, userdata);
}

/* moving a stream to another device: its ports reconnected to that device's */
static pa_operation *move_op(pa_context *c, bool playback, uint32_t idx, const char *name, uint32_t dev_idx,
		pa_context_success_cb_t cb, void *userdata)
{
	pa_stream *s = shim_context_find_stream(c, playback, idx);
	const struct shim_device *d = shim_context_device(c, playback, name, dev_idx);
	int err = s == NULL || d == NULL ? -PA_ERR_NOENTITY : shim_stream_move(s, d);
	if (err == 0)
		notify_sub(c, (playback ? PA_SUBSCRIPTION_EVENT_SINK_INPUT : PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT) |
				PA_SUBSCRIPTION_EVENT_CHANGE, idx);
	return shim_context_success_op(c, cb, userdata, err == 0, -err);
}

SHIM_EXPORT pa_operation *pa_context_move_sink_input_by_name(pa_context *c, uint32_t idx, const char *sink_name,
		pa_context_success_cb_t cb, void *userdata)
{
	return move_op(c, true, idx, sink_name, PA_INVALID_INDEX, cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_move_sink_input_by_index(pa_context *c, uint32_t idx, uint32_t sink_idx,
		pa_context_success_cb_t cb, void *userdata)
{
	return move_op(c, true, idx, NULL, sink_idx, cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_move_source_output_by_name(pa_context *c, uint32_t idx,
		const char *source_name, pa_context_success_cb_t cb, void *userdata)
{
	return move_op(c, false, idx, source_name, PA_INVALID_INDEX, cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_move_source_output_by_index(pa_context *c, uint32_t idx,
		uint32_t source_idx, pa_context_success_cb_t cb, void *userdata)
{
	return move_op(c, false, idx, NULL, source_idx, cb, userdata);
}

/* this application: the one client there is (index 1, as its streams say) */
static void run_client_info(pa_operation *o)
{
	pa_context *c = o->context;
	if (o->cb == NULL)
		return;
	if (o->data.index != PA_INVALID_INDEX && o->data.index != 1) {
		c->error = PA_ERR_NOENTITY;
		((pa_client_info_cb_t)o->cb)(c, NULL, -1, o->userdata);
		return;
	}
	pa_client_info i;
	memset(&i, 0, sizeof(i));
	i.index = 1;
	i.name = shim_context_app_name(c);
	i.owner_module = PA_INVALID_INDEX;
	i.driver = "jackbus";
	i.proplist = c->props;
	pa_proplist *empty = NULL;
	if (i.proplist == NULL)
		i.proplist = empty = pa_proplist_new();
	((pa_client_info_cb_t)o->cb)(c, &i, 0, o->userdata);
	((pa_client_info_cb_t)o->cb)(c, NULL, 1, o->userdata);
	if (empty)
		pa_proplist_free(empty);
}

SHIM_EXPORT pa_operation *pa_context_get_client_info(pa_context *c, uint32_t idx, pa_client_info_cb_t cb,
		void *userdata)
{
	pa_operation *o = shim_operation_new(c, NULL, run_client_info, (void *)cb, userdata);
	if (o)
		o->data.index = idx == PA_INVALID_INDEX ? 0 : idx;	/* INVALID: the list's marker */
	return o;
}

SHIM_EXPORT pa_operation *pa_context_get_client_info_list(pa_context *c, pa_client_info_cb_t cb, void *userdata)
{
	pa_operation *o = shim_operation_new(c, NULL, run_client_info, (void *)cb, userdata);
	if (o)
		o->data.index = PA_INVALID_INDEX;
	return o;
}

static void run_kill_client(pa_operation *o)
{
	pa_context *c = o->context;
	o->data.success = o->data.index == 1;
	if (!o->data.success)
		c->error = PA_ERR_NOENTITY;
	run_context_success(o);
	if (o->data.success && c->state == PA_CONTEXT_READY)
		shim_context_fail(c, PA_ERR_KILLED);	/* this application was the client */
}

SHIM_EXPORT pa_operation *pa_context_kill_client(pa_context *c, uint32_t idx, pa_context_success_cb_t cb,
		void *userdata)
{
	pa_operation *o = shim_operation_new(c, NULL, run_kill_client, (void *)cb, userdata);
	if (o)
		o->data.index = idx;
	return o;
}

static pa_operation *stream_volume_op(pa_context *c, bool playback, uint32_t idx, const pa_cvolume *v,
		int mute, pa_context_success_cb_t cb, void *userdata)
{
	pa_stream *s = shim_context_find_stream(c, playback, idx);
	if (s) {
		shim_stream_set_gain(s, v, mute);
		notify_sub(c, (playback ? PA_SUBSCRIPTION_EVENT_SINK_INPUT : PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT) |
				PA_SUBSCRIPTION_EVENT_CHANGE, idx);
	}
	return success_op(c, cb, userdata, s != NULL);
}

SHIM_EXPORT pa_operation *pa_context_set_sink_input_volume(pa_context *c, uint32_t idx,
		const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	return stream_volume_op(c, true, idx, volume, -1, cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_set_sink_input_mute(pa_context *c, uint32_t idx, int mute,
		pa_context_success_cb_t cb, void *userdata)
{
	return stream_volume_op(c, true, idx, NULL, mute, cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_set_source_output_volume(pa_context *c, uint32_t idx,
		const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	return stream_volume_op(c, false, idx, volume, -1, cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_set_source_output_mute(pa_context *c, uint32_t idx, int mute,
		pa_context_success_cb_t cb, void *userdata)
{
	return stream_volume_op(c, false, idx, NULL, mute, cb, userdata);
}

/* --------------------------------------------------------------- subscribe */

SHIM_EXPORT pa_operation *pa_context_subscribe(pa_context *c, pa_subscription_mask_t m,
		pa_context_success_cb_t cb, void *userdata)
{
	c->sub_mask = m;
	return success_op(c, cb, userdata, 1);
}

SHIM_EXPORT void pa_context_set_subscribe_callback(pa_context *c, pa_context_subscribe_cb_t cb, void *userdata)
{
	c->sub_cb = cb;
	c->sub_userdata = userdata;
}
