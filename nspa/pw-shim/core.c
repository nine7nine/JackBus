/*
 * jackbus-pw: pw_context, pw_core, the registry and proxies.
 *
 * There is no PipeWire daemon behind these objects. pw_context_connect()
 * returns a local pw_core. What a daemon would send later (done for a sync,
 * the core's info, registry globals, a bound object's info and params) is
 * queued and delivered in order on the application's loop, as it would come
 * from the socket: an application that waits for a sync's done has seen
 * every global by then.
 *
 * The registry shows JackBus the way applications look for devices:
 *  - an Audio/Sink node per JACK client with physical playback ports (the
 *    system card and each bridged card), an Audio/Source node per client with
 *    physical capture ports, named "jackbus.<client>.playback|capture";
 *  - the "default" metadata: default.audio.sink and .source are the first
 *    of each kind;
 *  - the "settings" metadata: JackBus's clock.rate and clock.quantum, read
 *    only (a request to force them is logged: JackBus decides, the WineASIO
 *    way).
 * The list follows JACK's ports (global, global_remove). A node can be bound
 * for its info and its EnumFormat/Format params, a metadata for its
 * properties. A stream's target.object (such a node's name or serial) picks
 * the device its ports connect to. Not listed: streams, clients, ports,
 * links; link-factory is refused.
 *
 * The registry holds the application's JACK client open (not active, so not
 * in the graph's cycle) to read JACK's ports.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

#define SHIM_EXPORT __attribute__((visibility("default")))
#define PW_API_CORE_IMPL SHIM_EXPORT
#define PW_API_REGISTRY_IMPL SHIM_EXPORT
#define PW_API_NODE_IMPL SHIM_EXPORT
#define PW_API_METADATA_IMPL SHIM_EXPORT
#include <pipewire/core.h>
#include <pipewire/node.h>
#include <pipewire/proxy.h>
#include <pipewire/permission.h>
#include <pipewire/extensions/metadata.h>

#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include "shim.h"

#define ID_METADATA_DEFAULT	30
#define ID_METADATA_SETTINGS	31
#define ID_FIRST_DEVICE		100
#define CLIENT_NAME_MAX		128	/* JACK's client name size limit */

/* ---------------------------------------------------------------- types */

enum proxy_kind { PROXY_REGISTRY, PROXY_NODE, PROXY_METADATA };

struct shim_proxy {
	/* must stay first: pw_registry_*, pw_node_*, pw_metadata_* are inline
	 * spa_interface calls on the proxy */
	struct spa_interface iface;
	struct spa_list link;			/* core->proxies */
	struct pw_core *core;
	enum proxy_kind kind;
	uint32_t id;				/* the global's, for bound objects */
	const char *type;
	uint32_t version;
	int refs;
	bool removed;
	bool bound_sent;
	bool app_ref;				/* a registry holds the application's client */
	struct spa_hook_list object_listeners;	/* the interface's events */
	struct spa_hook_list proxy_listeners;	/* pw_proxy_events */
	void *user_data;
};

struct device {
	struct spa_list link;			/* core->devices */
	uint32_t id;
	bool playback;				/* Audio/Sink: physical playback ports */
	bool seen;
	uint32_t channels;
	char client[CLIENT_NAME_MAX];
	char name[CLIENT_NAME_MAX + 32];
	char desc[CLIENT_NAME_MAX + 32];
};

struct work {
	struct spa_list link;
	void (*fn)(struct pw_core *core, struct work *w);
	struct shim_proxy *proxy;
	uint32_t id;
	int seq;
	uint32_t param_id;
};

static pthread_mutex_t cores_lock = PTHREAD_MUTEX_INITIALIZER;
static struct spa_list cores = SPA_LIST_INIT(&cores);
static uint32_t next_device_id = ID_FIRST_DEVICE;	/* under cores_lock */

/* -------------------------------------------------------------- context */

SHIM_EXPORT struct pw_context *pw_context_new(struct pw_loop *main_loop,
		struct pw_properties *props, size_t user_data_size)
{
	struct pw_context *this = calloc(1, sizeof(*this) + user_data_size);
	if (this == NULL) {
		pw_properties_free(props);
		return NULL;
	}
	this->main_loop = main_loop;
	this->props = props ? props : pw_properties_new(NULL, NULL);
	if (user_data_size > 0)
		this->user_data = SPA_PTROFF(this, sizeof(*this), void);
	spa_hook_list_init(&this->listeners);
	return this;
}

SHIM_EXPORT void pw_context_destroy(struct pw_context *context)
{
	if (context == NULL)
		return;
	spa_hook_list_call(&context->listeners, struct pw_context_events, destroy, 0);
	shim_data_loop_free(context);
	spa_hook_list_call(&context->listeners, struct pw_context_events, free, 0);
	spa_hook_list_clean(&context->listeners);
	pw_properties_free(context->props);
	free(context);
}

SHIM_EXPORT void *pw_context_get_user_data(struct pw_context *context) { return context->user_data; }

SHIM_EXPORT void pw_context_add_listener(struct pw_context *context, struct spa_hook *listener,
		const struct pw_context_events *events, void *data)
{
	spa_hook_list_append(&context->listeners, listener, events, data);
}

SHIM_EXPORT const struct pw_properties *pw_context_get_properties(struct pw_context *context)
{
	return context->props;
}

SHIM_EXPORT int pw_context_update_properties(struct pw_context *context, const struct spa_dict *dict)
{
	return pw_properties_update(context->props, dict);
}

SHIM_EXPORT struct pw_loop *pw_context_get_main_loop(struct pw_context *context)
{
	return context->main_loop;
}

SHIM_EXPORT const char *pw_context_get_conf_section(struct pw_context *context, const char *section)
{
	(void)context; (void)section;
	return NULL;
}

SHIM_EXPORT int pw_context_conf_update_props(struct pw_context *context,
		const char *section, struct pw_properties *props)
{
	(void)context; (void)section; (void)props;
	return 0;
}

SHIM_EXPORT int pw_context_conf_section_match_rules(struct pw_context *context,
		const char *section, const struct spa_dict *props,
		int (*callback) (void *data, const char *location, const char *action,
			const char *str, size_t len), void *data)
{
	(void)context; (void)section; (void)props; (void)callback; (void)data;
	return 0;
}

SHIM_EXPORT int pw_context_conf_section_for_each(struct pw_context *context,
		const char *section,
		int (*callback) (void *data, const char *location, const char *section,
			const char *str, size_t len), void *data)
{
	(void)context; (void)section; (void)callback; (void)data;
	return 0;
}

/* -------------------------------------------------------- deferred work */

static void defer(struct pw_core *core, void (*fn)(struct pw_core *, struct work *),
		struct shim_proxy *proxy, uint32_t id, int seq, uint32_t param_id)
{
	struct work *w = calloc(1, sizeof(*w));
	if (w == NULL)
		return;
	w->fn = fn;
	w->proxy = proxy;
	w->id = id;
	w->seq = seq;
	w->param_id = param_id;
	spa_list_append(&core->work, &w->link);
	pw_loop_signal_event(core->context->main_loop, core->work_source);
}

static void drop_work_of(struct pw_core *core, struct shim_proxy *proxy)
{
	struct work *w, *t;
	spa_list_for_each_safe(w, t, &core->work, link) {
		if (w->proxy == proxy) {
			spa_list_remove(&w->link);
			free(w);
		}
	}
}

static void rescan(struct pw_core *core);

static void on_work(void *data, uint64_t count)
{
	struct pw_core *core = data;

	if (atomic_exchange(&core->changed, false))
		rescan(core);
	while (!spa_list_is_empty(&core->work)) {
		struct work *w = spa_list_first(&core->work, struct work, link);
		spa_list_remove(&w->link);
		w->fn(core, w);
		free(w);
	}
}

/* -------------------------------------------------------------- devices */

static struct device *find_device(struct pw_core *core, uint32_t id)
{
	struct device *d;
	spa_list_for_each(d, &core->devices, link)
		if (d->id == id)
			return d;
	return NULL;
}

static struct device *default_device(struct pw_core *core, bool playback)
{
	struct device *d;
	spa_list_for_each(d, &core->devices, link)
		if (d->playback == playback)
			return d;
	return NULL;
}

/* the physical ports of one direction, grouped by client, in JACK's order */
static void scan_direction(struct pw_core *core, jack_client_t *c, bool playback)
{
	const char **ports = jack_get_ports(c, NULL, JACK_DEFAULT_AUDIO_TYPE,
			JackPortIsPhysical | (playback ? JackPortIsInput : JackPortIsOutput));
	for (size_t i = 0; ports && ports[i]; i++) {
		const char *colon = strchr(ports[i], ':');
		if (colon == NULL)
			continue;
		char client[CLIENT_NAME_MAX];
		snprintf(client, sizeof(client), "%.*s", (int)(colon - ports[i]), ports[i]);
		struct device *d, *found = NULL;
		spa_list_for_each(d, &core->devices, link)
			if (d->playback == playback && strcmp(d->client, client) == 0)
				found = d;
		if (found == NULL) {
			if ((found = calloc(1, sizeof(*found))) == NULL)
				continue;
			pthread_mutex_lock(&cores_lock);
			found->id = next_device_id++;
			pthread_mutex_unlock(&cores_lock);
			found->playback = playback;
			snprintf(found->client, sizeof(found->client), "%s", client);
			snprintf(found->name, sizeof(found->name), "jackbus.%s.%s", client,
					playback ? "playback" : "capture");
			snprintf(found->desc, sizeof(found->desc), "%s %s (JackBus)", client,
					playback ? "playback" : "capture");
			found->channels = 0;
			found->seen = false;
			spa_list_append(&core->devices, &found->link);
		}
		if (!found->seen) {
			found->seen = true;
			found->channels = 0;
		}
		found->channels++;
	}
	jack_free(ports);
}

static void emit_global(struct shim_proxy *r, struct pw_core *core, uint32_t id);
static void emit_metadata(struct pw_core *core, struct work *w);

/* JACK's physical ports as devices; tell the registries what changed */
static void rescan(struct pw_core *core)
{
	jack_client_t *c = shim_app_client();
	struct device *d, *t;
	struct shim_proxy *p;
	struct device *old_sink = default_device(core, true), *old_source = default_device(core, false);
	uint32_t old_sink_id = old_sink ? old_sink->id : SPA_ID_INVALID;
	uint32_t old_source_id = old_source ? old_source->id : SPA_ID_INVALID;

	if (c == NULL)
		return;
	spa_list_for_each(d, &core->devices, link)
		d->seen = false;
	/* the ids known before this scan */
	uint32_t known[256];
	uint32_t n_known = 0;
	spa_list_for_each(d, &core->devices, link)
		if (n_known < SPA_N_ELEMENTS(known))
			known[n_known++] = d->id;

	scan_direction(core, c, true);
	scan_direction(core, c, false);

	spa_list_for_each_safe(d, t, &core->devices, link) {
		if (d->seen)
			continue;
		spa_list_for_each(p, &core->proxies, link) {
			if (p->kind == PROXY_REGISTRY)
				spa_hook_list_call(&p->object_listeners, struct pw_registry_events,
						global_remove, 0, d->id);
			else if (p->kind == PROXY_NODE && p->id == d->id && !p->removed) {
				p->removed = true;
				spa_hook_list_call(&p->proxy_listeners, struct pw_proxy_events, removed, 0);
			}
		}
		shim_log("registry: %s is gone", d->name);
		spa_list_remove(&d->link);
		free(d);
	}
	spa_list_for_each(d, &core->devices, link) {
		bool is_new = true;
		for (uint32_t i = 0; i < n_known; i++)
			if (known[i] == d->id)
				is_new = false;
		if (!is_new)
			continue;
		shim_log("registry: %s, %u channels (id %u)", d->name, d->channels, d->id);
		spa_list_for_each(p, &core->proxies, link)
			if (p->kind == PROXY_REGISTRY)
				emit_global(p, core, d->id);
	}
	/* a new default device: tell the bound "default" metadata */
	struct device *sink = default_device(core, true), *source = default_device(core, false);
	if ((sink ? sink->id : SPA_ID_INVALID) != old_sink_id ||
	    (source ? source->id : SPA_ID_INVALID) != old_source_id) {
		spa_list_for_each(p, &core->proxies, link)
			if (p->kind == PROXY_METADATA && p->id == ID_METADATA_DEFAULT)
				defer(core, emit_metadata, p, 0, 0, 0);
	}
}

void shim_registry_changed(void)
{
	struct pw_core *core;
	pthread_mutex_lock(&cores_lock);
	spa_list_for_each(core, &cores, link) {
		atomic_store(&core->changed, true);
		pw_loop_signal_event(core->context->main_loop, core->work_source);
	}
	pthread_mutex_unlock(&cores_lock);
}

/* a device by its node name, serial or id; the client name for a plain one */
const char **shim_target_ports(jack_client_t *c, const char *target, uint32_t target_id, bool playback)
{
	char who[CLIENT_NAME_MAX] = "";
	uint32_t id = target_id;
	struct pw_core *core;
	struct device *d;

	if (target && target[0]) {
		char *end;
		unsigned long v = strtoul(target, &end, 10);
		if (*end == 0)
			id = (uint32_t)v;
		else {
			const char *dir = playback ? ".playback" : ".capture";
			size_t len = strlen(target), dl = strlen(dir);
			if (spa_strstartswith(target, "jackbus.") && len > 8 + dl &&
			    strcmp(target + len - dl, dir) == 0)
				snprintf(who, sizeof(who), "%.*s", (int)(len - 8 - dl), target + 8);
			else
				snprintf(who, sizeof(who), "%s", target);	/* a JACK client */
		}
	}
	if (who[0] == 0 && id != SPA_ID_INVALID && id != PW_ID_ANY) {
		pthread_mutex_lock(&cores_lock);
		spa_list_for_each(core, &cores, link)
			spa_list_for_each(d, &core->devices, link)
				if (d->id == id && d->playback == playback)
					snprintf(who, sizeof(who), "%s", d->client);
		pthread_mutex_unlock(&cores_lock);
		if (who[0] == 0)
			shim_log("target %u is not a JackBus %s device: the default", id,
					playback ? "playback" : "capture");
	}

	const char **ports = jack_get_ports(c, NULL, JACK_DEFAULT_AUDIO_TYPE,
			JackPortIsPhysical | (playback ? JackPortIsInput : JackPortIsOutput));
	if (ports == NULL)
		return NULL;
	size_t n = 0;
	while (ports[n])
		n++;
	const char **res = calloc(n + 1, sizeof(*res));
	if (res == NULL) {
		jack_free(ports);
		return NULL;
	}
	/* the default: the first client's ports */
	if (who[0] == 0) {
		const char *colon = strchr(ports[0], ':');
		snprintf(who, sizeof(who), "%.*s", colon ? (int)(colon - ports[0]) : 0, ports[0]);
	}
	size_t k = 0, wl = strlen(who);
	for (size_t i = 0; i < n; i++)
		if (strncmp(ports[i], who, wl) == 0 && ports[i][wl] == ':')
			res[k++] = ports[i];	/* names live in the graph's shared memory */
	jack_free(ports);
	if (k == 0) {
		shim_log("target \"%s\" has no %s ports", who, playback ? "playback" : "capture");
		free(res);
		return NULL;
	}
	return res;
}

/* ------------------------------------------------------- the registry */

static void props_of_device(struct pw_core *core, struct device *d, struct spa_dict_item *items,
		uint32_t *n, char (*buf)[64])
{
	jack_client_t *c = shim_app_client();
	uint32_t i = 0;
	snprintf(buf[0], 64, "%u", d->id);
	snprintf(buf[1], 64, "%u", d->channels);
	snprintf(buf[2], 64, "%u", c ? jack_get_sample_rate(c) : 48000);
	items[i++] = SPA_DICT_ITEM_INIT(PW_KEY_OBJECT_ID, buf[0]);
	items[i++] = SPA_DICT_ITEM_INIT(PW_KEY_OBJECT_SERIAL, buf[0]);
	items[i++] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_NAME, d->name);
	items[i++] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_DESCRIPTION, d->desc);
	items[i++] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_NICK, d->client);
	items[i++] = SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_CLASS, d->playback ? "Audio/Sink" : "Audio/Source");
	items[i++] = SPA_DICT_ITEM_INIT(PW_KEY_AUDIO_CHANNELS, buf[1]);
	items[i++] = SPA_DICT_ITEM_INIT(PW_KEY_AUDIO_RATE, buf[2]);
	items[i++] = SPA_DICT_ITEM_INIT(PW_KEY_DEVICE_API, "jackbus");
	items[i++] = SPA_DICT_ITEM_INIT(PW_KEY_PRIORITY_SESSION,
			d == default_device(core, d->playback) ? "1000" : "900");
	*n = i;
}

static void emit_global(struct shim_proxy *r, struct pw_core *core, uint32_t id)
{
	struct spa_dict_item items[16];
	char buf[3][64];
	uint32_t n = 0;

	if (id == ID_METADATA_DEFAULT || id == ID_METADATA_SETTINGS) {
		items[n++] = SPA_DICT_ITEM_INIT(PW_KEY_METADATA_NAME,
				id == ID_METADATA_DEFAULT ? "default" : "settings");
		struct spa_dict dict = SPA_DICT_INIT(items, n);
		spa_hook_list_call(&r->object_listeners, struct pw_registry_events, global, 0,
				id, PW_PERM_R | PW_PERM_X, PW_TYPE_INTERFACE_Metadata,
				PW_VERSION_METADATA, &dict);
		return;
	}
	struct device *d = find_device(core, id);
	if (d == NULL)
		return;
	props_of_device(core, d, items, &n, buf);
	struct spa_dict dict = SPA_DICT_INIT(items, n);
	spa_hook_list_call(&r->object_listeners, struct pw_registry_events, global, 0,
			id, PW_PERM_R | PW_PERM_X, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &dict);
}

static void emit_globals(struct pw_core *core, struct work *w)
{
	struct device *d;
	emit_global(w->proxy, core, ID_METADATA_DEFAULT);
	emit_global(w->proxy, core, ID_METADATA_SETTINGS);
	spa_list_for_each(d, &core->devices, link)
		emit_global(w->proxy, core, d->id);
}

static struct shim_proxy *proxy_new(struct pw_core *core, enum proxy_kind kind, uint32_t id,
		const char *type, uint32_t version, const void *methods, size_t user_data_size)
{
	struct shim_proxy *p = calloc(1, sizeof(*p) + user_data_size);
	if (p == NULL)
		return NULL;
	p->iface = SPA_INTERFACE_INIT(type, version, methods, p);
	p->core = core;
	p->kind = kind;
	p->id = id;
	p->type = type;
	p->version = version;
	p->refs = 1;
	spa_hook_list_init(&p->object_listeners);
	spa_hook_list_init(&p->proxy_listeners);
	if (user_data_size > 0)
		p->user_data = SPA_PTROFF(p, sizeof(*p), void);
	spa_list_append(&core->proxies, &p->link);
	return p;
}

static void proxy_free(struct shim_proxy *p)
{
	struct pw_core *core = p->core;
	spa_hook_list_call(&p->proxy_listeners, struct pw_proxy_events, destroy, 0);
	drop_work_of(core, p);
	spa_list_remove(&p->link);
	spa_hook_list_clean(&p->object_listeners);
	spa_hook_list_clean(&p->proxy_listeners);
	if (p->app_ref)
		shim_app_unref();
	free(p);
}

/* node proxies: a JackBus device */

static int node_add_listener(void *object, struct spa_hook *listener,
		const struct pw_node_events *events, void *data)
{
	struct shim_proxy *p = object;
	spa_hook_list_append(&p->object_listeners, listener, events, data);
	return 0;
}

static void emit_param(struct pw_core *core, struct work *w)
{
	struct shim_proxy *p = w->proxy;
	struct device *d = find_device(core, p->id);
	jack_client_t *c = shim_app_client();
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_audio_info_raw info;

	if (d == NULL || (w->param_id != SPA_PARAM_EnumFormat && w->param_id != SPA_PARAM_Format))
		return;
	spa_zero(info);
	info.format = SPA_AUDIO_FORMAT_F32P;
	info.rate = c ? jack_get_sample_rate(c) : 48000;
	info.channels = SPA_MIN(d->channels, (uint32_t)SPA_AUDIO_MAX_CHANNELS);
	for (uint32_t i = 0; i < info.channels; i++)
		info.position[i] = (info.channels == 2) ? (i ? SPA_AUDIO_CHANNEL_FR : SPA_AUDIO_CHANNEL_FL) :
			(info.channels == 1) ? SPA_AUDIO_CHANNEL_MONO : SPA_AUDIO_CHANNEL_AUX0 + i;
	struct spa_pod *param = spa_format_audio_raw_build(&b, w->param_id, &info);
	spa_hook_list_call(&p->object_listeners, struct pw_node_events, param, 0,
			w->seq, w->param_id, 0, 1, param);
}

static int node_subscribe_params(void *object, uint32_t *ids, uint32_t n_ids)
{
	struct shim_proxy *p = object;
	for (uint32_t i = 0; i < n_ids; i++)
		defer(p->core, emit_param, p, 0, 0, ids[i]);
	return 0;
}

static int node_enum_params(void *object, int seq, uint32_t id, uint32_t start, uint32_t num,
		const struct spa_pod *filter)
{
	struct shim_proxy *p = object;
	if (start == 0)
		defer(p->core, emit_param, p, 0, seq, id);
	return 0;
}

static int node_set_param(void *object, uint32_t id, uint32_t flags, const struct spa_pod *param)
{
	return 0;	/* a JackBus device's settings are JackBus's */
}

static int node_send_command(void *object, const struct spa_command *command)
{
	return 0;
}

static const struct pw_node_methods node_methods = {
	PW_VERSION_NODE_METHODS,
	.add_listener = node_add_listener,
	.subscribe_params = node_subscribe_params,
	.enum_params = node_enum_params,
	.set_param = node_set_param,
	.send_command = node_send_command,
};

static void emit_node_info(struct pw_core *core, struct work *w)
{
	struct shim_proxy *p = w->proxy;
	struct device *d = find_device(core, p->id);
	struct spa_dict_item items[16];
	char buf[3][64];
	uint32_t n = 0;

	if (d == NULL)
		return;
	props_of_device(core, d, items, &n, buf);
	struct spa_dict dict = SPA_DICT_INIT(items, n);
	spa_hook_list_call(&p->proxy_listeners, struct pw_proxy_events, bound_props, 0, d->id, &dict);
	spa_hook_list_call(&p->proxy_listeners, struct pw_proxy_events, bound, 0, d->id);

	struct spa_param_info params[2] = {
		SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ),
		SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READ),
	};
	struct pw_node_info info = {
		.id = d->id,
		.max_input_ports = d->playback ? d->channels : 0,
		.max_output_ports = d->playback ? 0 : d->channels,
		.change_mask = PW_NODE_CHANGE_MASK_ALL,
		.n_input_ports = d->playback ? d->channels : 0,
		.n_output_ports = d->playback ? 0 : d->channels,
		.state = PW_NODE_STATE_RUNNING,
		.error = NULL,
		.props = &dict,
		.params = params,
		.n_params = 2,
	};
	spa_hook_list_call(&p->object_listeners, struct pw_node_events, info, 0, &info);
}

/* metadata proxies: "default" and "settings" */

static int metadata_add_listener(void *object, struct spa_hook *listener,
		const struct pw_metadata_events *events, void *data)
{
	struct shim_proxy *p = object;
	spa_hook_list_append(&p->object_listeners, listener, events, data);
	return 0;
}

static int metadata_set_property(void *object, uint32_t subject, const char *key,
		const char *type, const char *value)
{
	shim_log("metadata: %s=%s not applied: JackBus's settings are its own",
			key ? key : "(null)", value ? value : "(null)");
	return 0;
}

static int metadata_clear(void *object)
{
	return 0;
}

static const struct pw_metadata_methods metadata_methods = {
	PW_VERSION_METADATA_METHODS,
	.add_listener = metadata_add_listener,
	.set_property = metadata_set_property,
	.clear = metadata_clear,
};

static void metadata_prop(struct shim_proxy *p, const char *key, const char *type, const char *value)
{
	spa_hook_list_call(&p->object_listeners, struct pw_metadata_events, property, 0,
			PW_ID_CORE, key, type, value);
}

static void emit_metadata(struct pw_core *core, struct work *w)
{
	struct shim_proxy *p = w->proxy;
	jack_client_t *c = shim_app_client();
	char v[256];

	if (!p->bound_sent)
		spa_hook_list_call(&p->proxy_listeners, struct pw_proxy_events, bound, 0, p->id);
	p->bound_sent = true;
	if (p->id == ID_METADATA_DEFAULT) {
		for (int playback = 1; playback >= 0; playback--) {
			struct device *d = default_device(core, playback);
			const char *k = playback ? "default.audio.sink" : "default.audio.source";
			const char *kc = playback ? "default.configured.audio.sink" : "default.configured.audio.source";
			if (d == NULL) {
				metadata_prop(p, k, NULL, NULL);
				continue;
			}
			snprintf(v, sizeof(v), "{ \"name\": \"%s\" }", d->name);
			metadata_prop(p, k, "Spa:String:JSON", v);
			metadata_prop(p, kc, "Spa:String:JSON", v);
		}
	} else if (c) {
		unsigned rate = jack_get_sample_rate(c), q = jack_get_buffer_size(c);
		snprintf(v, sizeof(v), "%u", rate);
		metadata_prop(p, "clock.rate", NULL, v);
		snprintf(v, sizeof(v), "[ %u ]", rate);
		metadata_prop(p, "clock.allowed-rates", NULL, v);
		snprintf(v, sizeof(v), "%u", q);
		metadata_prop(p, "clock.quantum", NULL, v);
		metadata_prop(p, "clock.min-quantum", NULL, v);
		metadata_prop(p, "clock.max-quantum", NULL, v);
		metadata_prop(p, "clock.force-quantum", NULL, "0");
		metadata_prop(p, "clock.force-rate", NULL, "0");
	}
}

/* the registry proxy */

static int registry_add_listener(void *object, struct spa_hook *listener,
		const struct pw_registry_events *events, void *data)
{
	struct shim_proxy *r = object;
	spa_hook_list_append(&r->object_listeners, listener, events, data);
	return 0;
}

static void *registry_bind(void *object, uint32_t id, const char *type,
		uint32_t version, size_t user_data_size)
{
	struct shim_proxy *r = object;
	struct pw_core *core = r->core;
	struct shim_proxy *p;

	if (id == ID_METADATA_DEFAULT || id == ID_METADATA_SETTINGS) {
		if (!spa_streq(type, PW_TYPE_INTERFACE_Metadata))
			goto wrong;
		p = proxy_new(core, PROXY_METADATA, id, PW_TYPE_INTERFACE_Metadata,
				PW_VERSION_METADATA, &metadata_methods, user_data_size);
		if (p)
			defer(core, emit_metadata, p, 0, 0, 0);
		return p;
	}
	if (find_device(core, id) == NULL) {
		errno = ENOENT;
		return NULL;
	}
	if (!spa_streq(type, PW_TYPE_INTERFACE_Node))
		goto wrong;
	p = proxy_new(core, PROXY_NODE, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE,
			&node_methods, user_data_size);
	if (p)
		defer(core, emit_node_info, p, 0, 0, 0);
	return p;
wrong:
	shim_log("registry: bind %u as %s: not that type", id, type);
	errno = EINVAL;
	return NULL;
}

static int registry_destroy(void *object, uint32_t id) { return -EPERM; }

static const struct pw_registry_methods registry_methods = {
	PW_VERSION_REGISTRY_METHODS,
	.add_listener = registry_add_listener,
	.bind = registry_bind,
	.destroy = registry_destroy,
};

static const char *app_name_of(struct pw_core *core)
{
	const char *n = pw_properties_get(core->props, PW_KEY_APP_NAME);
	if (n == NULL || n[0] == 0)
		n = pw_properties_get(core->context->props, PW_KEY_APP_NAME);
	if (n == NULL || n[0] == 0)
		n = pw_get_prgname();
	return n && n[0] ? n : "pipewire-app";
}

/* ----------------------------------------------------------------- core */

static void emit_done(struct pw_core *core, struct work *w)
{
	spa_hook_list_call(&core->listeners, struct pw_core_events, done, 0, w->id, w->seq);
}

static void emit_core_info(struct pw_core *core, struct work *w)
{
	char *user = getlogin();
	char host[256] = "";
	gethostname(host, sizeof(host) - 1);
	struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT(PW_KEY_CORE_NAME, "jackbus"),
		SPA_DICT_ITEM_INIT("jackbus.shim", "1"),
	};
	struct spa_dict dict = SPA_DICT_INIT_ARRAY(items);
	struct pw_core_info info = {
		.id = PW_ID_CORE,
		.cookie = 0x4a42,
		.user_name = user ? user : "",
		.host_name = host,
		.version = pw_get_library_version(),
		.name = "jackbus",
		.change_mask = PW_CORE_CHANGE_MASK_ALL,
		.props = &dict,
	};
	spa_hook_list_call(&core->listeners, struct pw_core_events, info, 0, &info);
}

static int core_add_listener(void *object, struct spa_hook *listener,
		const struct pw_core_events *events, void *data)
{
	struct pw_core *core = object;
	spa_hook_list_append(&core->listeners, listener, events, data);
	defer(core, emit_core_info, NULL, 0, 0, 0);
	return 0;
}

static int core_hello(void *object, uint32_t version) { return 0; }

static int core_sync(void *object, uint32_t id, int seq)
{
	struct pw_core *core = object;
	int res = SPA_RESULT_RETURN_ASYNC(++core->sync_seq & SPA_ASYNC_SEQ_MASK);
	defer(core, emit_done, NULL, id, res, 0);
	return res;
}

static int core_pong(void *object, uint32_t id, int seq) { return 0; }

static int core_error(void *object, uint32_t id, int seq, int res, const char *message)
{
	shim_log("core error from the application: id %u: %s", id, message);
	return 0;
}

static struct pw_registry *core_get_registry(void *object, uint32_t version,
		size_t user_data_size)
{
	struct pw_core *core = object;
	struct shim_proxy *r = proxy_new(core, PROXY_REGISTRY, 0, PW_TYPE_INTERFACE_Registry,
			PW_VERSION_REGISTRY, &registry_methods, user_data_size);
	if (r == NULL)
		return NULL;
	/* the application's client, to read JACK's ports (not in the cycle) */
	if (shim_app_ref(app_name_of(core)) != NULL)
		r->app_ref = true;
	rescan(core);
	defer(core, emit_globals, r, 0, 0, 0);
	return (struct pw_registry *)r;
}

static void *core_create_object(void *object, const char *factory_name,
		const char *type, uint32_t version, const struct spa_dict *props,
		size_t user_data_size)
{
	shim_log("create_object(%s, %s) is not supported on JackBus", factory_name, type);
	errno = ENOTSUP;
	return NULL;
}

static int core_destroy_proxy(void *object, void *proxy) { return -ENOTSUP; }

static const struct pw_core_methods core_methods = {
	PW_VERSION_CORE_METHODS,
	.add_listener = core_add_listener,
	.hello = core_hello,
	.sync = core_sync,
	.pong = core_pong,
	.error = core_error,
	.get_registry = core_get_registry,
	.create_object = core_create_object,
	.destroy = core_destroy_proxy,
};

SHIM_EXPORT struct pw_core *pw_context_connect(struct pw_context *context,
		struct pw_properties *properties, size_t user_data_size)
{
	struct pw_core *core = calloc(1, sizeof(*core) + user_data_size);

	if (core == NULL) {
		pw_properties_free(properties);
		return NULL;
	}
	core->iface = SPA_INTERFACE_INIT(PW_TYPE_INTERFACE_Core, PW_VERSION_CORE,
			&core_methods, core);
	spa_hook_list_init(&core->listeners);
	spa_list_init(&core->work);
	spa_list_init(&core->proxies);
	spa_list_init(&core->devices);
	core->context = context;
	core->props = properties ? properties : pw_properties_new(NULL, NULL);
	if (user_data_size > 0)
		core->user_data = SPA_PTROFF(core, sizeof(*core), void);
	core->work_source = pw_loop_add_event(context->main_loop, on_work, core);
	if (core->work_source == NULL) {
		pw_properties_free(core->props);
		free(core);
		return NULL;
	}
	pthread_mutex_lock(&cores_lock);
	spa_list_append(&cores, &core->link);
	pthread_mutex_unlock(&cores_lock);
	shim_log("pw_context_connect: local core, no PipeWire daemon involved");
	return core;
}

SHIM_EXPORT struct pw_core *pw_context_connect_fd(struct pw_context *context, int fd,
		struct pw_properties *properties, size_t user_data_size)
{
	(void)fd;
	return pw_context_connect(context, properties, user_data_size);
}

SHIM_EXPORT struct pw_core *pw_context_connect_self(struct pw_context *context,
		struct pw_properties *properties, size_t user_data_size)
{
	return pw_context_connect(context, properties, user_data_size);
}

SHIM_EXPORT int pw_core_disconnect(struct pw_core *core)
{
	struct shim_proxy *p;
	struct device *d;
	struct work *w;

	if (core == NULL)
		return 0;
	pthread_mutex_lock(&cores_lock);
	spa_list_remove(&core->link);
	pthread_mutex_unlock(&cores_lock);
	spa_list_consume(p, &core->proxies, link)
		proxy_free(p);
	spa_list_consume(w, &core->work, link) {
		spa_list_remove(&w->link);
		free(w);
	}
	spa_list_consume(d, &core->devices, link) {
		spa_list_remove(&d->link);
		free(d);
	}
	spa_hook_list_clean(&core->listeners);
	pw_loop_destroy_source(core->context->main_loop, core->work_source);
	pw_properties_free(core->props);
	free(core);
	return 0;
}

SHIM_EXPORT struct pw_context *pw_core_get_context(struct pw_core *core) { return core->context; }
SHIM_EXPORT void *pw_core_get_user_data(struct pw_core *core) { return core->user_data; }
SHIM_EXPORT const struct pw_properties *pw_core_get_properties(struct pw_core *core) { return core->props; }
SHIM_EXPORT int pw_core_update_properties(struct pw_core *core, const struct spa_dict *dict)
{
	return pw_properties_update(core->props, dict);
}
SHIM_EXPORT int pw_core_set_paused(struct pw_core *core, bool paused) { return 0; }

/* ---------------------------------------------------------------- proxy */

SHIM_EXPORT void pw_proxy_add_listener(struct pw_proxy *proxy, struct spa_hook *listener,
		const struct pw_proxy_events *events, void *data)
{
	struct shim_proxy *p = (struct shim_proxy *)proxy;
	spa_hook_list_append(&p->proxy_listeners, listener, events, data);
}

SHIM_EXPORT void pw_proxy_add_object_listener(struct pw_proxy *proxy, struct spa_hook *listener,
		const void *funcs, void *data)
{
	struct shim_proxy *p = (struct shim_proxy *)proxy;
	spa_hook_list_append(&p->object_listeners, listener, funcs, data);
}

SHIM_EXPORT void *pw_proxy_get_user_data(struct pw_proxy *proxy)
{
	return ((struct shim_proxy *)proxy)->user_data;
}

SHIM_EXPORT uint32_t pw_proxy_get_id(struct pw_proxy *proxy)
{
	return ((struct shim_proxy *)proxy)->id;
}

SHIM_EXPORT uint32_t pw_proxy_get_bound_id(struct pw_proxy *proxy)
{
	struct shim_proxy *p = (struct shim_proxy *)proxy;
	return p->kind == PROXY_REGISTRY ? SPA_ID_INVALID : p->id;
}

SHIM_EXPORT const char *pw_proxy_get_type(struct pw_proxy *proxy, uint32_t *version)
{
	struct shim_proxy *p = (struct shim_proxy *)proxy;
	if (version)
		*version = p->version;
	return p->type;
}

SHIM_EXPORT struct spa_hook_list *pw_proxy_get_object_listeners(struct pw_proxy *proxy)
{
	return &((struct shim_proxy *)proxy)->object_listeners;
}

static void emit_proxy_done(struct pw_core *core, struct work *w)
{
	spa_hook_list_call(&w->proxy->proxy_listeners, struct pw_proxy_events, done, 0, w->seq);
}

SHIM_EXPORT int pw_proxy_sync(struct pw_proxy *proxy, int seq)
{
	struct shim_proxy *p = (struct shim_proxy *)proxy;
	int res = SPA_RESULT_RETURN_ASYNC(++p->core->sync_seq & SPA_ASYNC_SEQ_MASK);
	defer(p->core, emit_proxy_done, p, 0, res, 0);
	return res;
}

SHIM_EXPORT int pw_proxy_error(struct pw_proxy *proxy, int res, const char *error)
{
	shim_log("proxy error from the application: %s", error ? error : "");
	return 0;
}

SHIM_EXPORT int pw_proxy_errorf(struct pw_proxy *proxy, int res, const char *error, ...)
{
	return 0;
}

SHIM_EXPORT void pw_proxy_ref(struct pw_proxy *proxy)
{
	((struct shim_proxy *)proxy)->refs++;
}

SHIM_EXPORT void pw_proxy_destroy(struct pw_proxy *proxy)
{
	if (proxy)
		proxy_free((struct shim_proxy *)proxy);
}

SHIM_EXPORT void pw_proxy_unref(struct pw_proxy *proxy)
{
	struct shim_proxy *p = (struct shim_proxy *)proxy;
	if (p && --p->refs <= 0)
		proxy_free(p);
}

/* ------------------------------------------------------------ node info */

static struct spa_dict *dict_copy(const struct spa_dict *src)
{
	if (src == NULL)
		return NULL;
	struct spa_dict *d = calloc(1, sizeof(*d) + src->n_items * sizeof(struct spa_dict_item));
	if (d == NULL)
		return NULL;
	struct spa_dict_item *items = SPA_PTROFF(d, sizeof(*d), struct spa_dict_item);
	for (uint32_t i = 0; i < src->n_items; i++) {
		items[i].key = strdup(src->items[i].key);
		items[i].value = src->items[i].value ? strdup(src->items[i].value) : NULL;
	}
	d->items = items;
	d->n_items = src->n_items;
	return d;
}

static void dict_free(struct spa_dict *d)
{
	if (d == NULL)
		return;
	for (uint32_t i = 0; i < d->n_items; i++) {
		free((char *)d->items[i].key);
		free((char *)d->items[i].value);
	}
	free(d);
}

SHIM_EXPORT struct pw_node_info *pw_node_info_merge(struct pw_node_info *info,
		const struct pw_node_info *update, bool reset)
{
	if (update == NULL)
		return info;
	if (info == NULL) {
		if ((info = calloc(1, sizeof(*info))) == NULL)
			return NULL;
		info->id = update->id;
		info->max_input_ports = update->max_input_ports;
		info->max_output_ports = update->max_output_ports;
	}
	if (reset)
		info->change_mask = 0;
	info->change_mask |= update->change_mask;
	if (update->change_mask & PW_NODE_CHANGE_MASK_INPUT_PORTS) {
		info->max_input_ports = update->max_input_ports;
		info->n_input_ports = update->n_input_ports;
	}
	if (update->change_mask & PW_NODE_CHANGE_MASK_OUTPUT_PORTS) {
		info->max_output_ports = update->max_output_ports;
		info->n_output_ports = update->n_output_ports;
	}
	if (update->change_mask & PW_NODE_CHANGE_MASK_STATE) {
		info->state = update->state;
		free((char *)info->error);
		info->error = update->error ? strdup(update->error) : NULL;
	}
	if (update->change_mask & PW_NODE_CHANGE_MASK_PROPS) {
		dict_free(info->props);
		info->props = dict_copy(update->props);
	}
	if (update->change_mask & PW_NODE_CHANGE_MASK_PARAMS) {
		free(info->params);
		info->params = NULL;
		info->n_params = 0;
		if (update->n_params &&
		    (info->params = calloc(update->n_params, sizeof(struct spa_param_info))) != NULL) {
			memcpy(info->params, update->params, update->n_params * sizeof(struct spa_param_info));
			info->n_params = update->n_params;
		}
	}
	return info;
}

SHIM_EXPORT struct pw_node_info *pw_node_info_update(struct pw_node_info *info,
		const struct pw_node_info *update)
{
	return pw_node_info_merge(info, update, true);
}

SHIM_EXPORT void pw_node_info_free(struct pw_node_info *info)
{
	if (info == NULL)
		return;
	free((char *)info->error);
	dict_free(info->props);
	free(info->params);
	free(info);
}

/* ----------------------------------------------------------------- i18n */

SHIM_EXPORT const char *pw_gettext(const char *msgid)
{
	return msgid;
}

SHIM_EXPORT const char *pw_ngettext(const char *msgid, const char *msgid_plural, unsigned long int n)
{
	return n == 1 ? msgid : msgid_plural;
}
