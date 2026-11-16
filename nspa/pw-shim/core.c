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
 * The registry shows the JACK graph as PipeWire's objects:
 *  - Nodes: an Audio/Sink per JACK client with physical playback ports (the
 *    system card, each bridged card), an Audio/Source per client with
 *    physical capture ports ("jackbus.<client>.playback|capture"), a
 *    Midi/Bridge for physical MIDI ports; one node per other JACK client
 *    (named after it); and each of this application's own pw_filters and
 *    pw_streams, under the id pw_filter_get_node_id() and
 *    pw_stream_get_node_id() give;
 *  - Ports: every JACK port (port.alias is its JACK name, node.id its node);
 *  - Links: every JACK connection;
 *  - Metadata: "default" (default.audio.sink and .source: the first device of
 *    each kind), "settings" (JackBus's clock.rate and clock.quantum, read
 *    only: a request to force them is logged, JackBus decides).
 * The objects follow JACK's graph (global, global_remove). Nodes, ports,
 * links and metadata can be bound. link-factory makes JACK connections
 * (link.output.port/link.input.port, or link.output.node/link.input.node,
 * port by port), which go with their proxy unless object.linger is set;
 * pw_registry_destroy() of a link disconnects it. A stream's target.object
 * (a node's name or serial) picks the device its ports connect to.
 *
 * The registry holds the application's JACK client open (not active, so not
 * in the graph's cycle) to read JACK's graph.
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
#define PW_API_PORT_IMPL SHIM_EXPORT
#define PW_API_LINK_IMPL SHIM_EXPORT
#define PW_API_METADATA_IMPL SHIM_EXPORT
#include <pipewire/core.h>
#include <pipewire/node.h>
#include <pipewire/port.h>
#include <pipewire/link.h>
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
#define ID_FIRST		100
#define CLIENT_NAME_MAX		128	/* JACK's client name size limit */
#define PORT_NAME_MAX		320	/* JACK's full port name size limit */

/* ---------------------------------------------------------------- types */

enum proxy_kind { PROXY_REGISTRY, PROXY_NODE, PROXY_PORT, PROXY_LINK, PROXY_METADATA, PROXY_MADE_LINK };

struct shim_proxy {
	/* must stay first: pw_registry_*, pw_node_*, pw_port_*, pw_link_*,
	 * pw_metadata_* are inline spa_interface calls on the proxy */
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
	/* a link made through link-factory: its JACK connections */
	char (*pairs)[2][PORT_NAME_MAX];
	uint32_t n_pairs;
	bool linger;
	struct spa_hook_list object_listeners;	/* the interface's events */
	struct spa_hook_list proxy_listeners;	/* pw_proxy_events */
	void *user_data;
};

enum gtype { G_NODE, G_PORT, G_LINK };
enum nkind { N_CLIENT, N_OWN, N_PLAYBACK, N_CAPTURE, N_MIDI };

/* one object of the JACK graph as the registry shows it */
struct gobj {
	struct spa_list link;			/* core->graph */
	enum gtype type;
	uint32_t id;
	bool seen, fresh;
	char key[2 * PORT_NAME_MAX + 2];	/* node name, port name, "out>in" */
	/* nodes */
	enum nkind kind;
	char name[PORT_NAME_MAX], desc[PORT_NAME_MAX + 32], media_class[32];
	char client[CLIENT_NAME_MAX];
	uint32_t n_in, n_out;
	/* ports */
	uint32_t node_id, index;
	bool out, physical, midi;
	const char *short_name;			/* in key */
	/* links */
	uint32_t out_port, in_port, out_node, in_node;
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

/* ------------------------------------------------------------------ ids */

/* every object id of the process, from one counter: filters, streams and
 * the registry's objects never share one */
static atomic_uint next_id = ID_FIRST;

uint32_t shim_new_id(void)
{
	return atomic_fetch_add(&next_id, 1);
}

/* an object's id, the same in every core of the process for as long as it
 * runs (a device, a port that comes back, keep theirs) */
static pthread_mutex_t ids_lock = PTHREAD_MUTEX_INITIALIZER;
static struct id_entry { char *key; uint32_t id; } *ids;
static uint32_t n_ids, cap_ids;

static uint32_t stable_id(const char *key)
{
	uint32_t id = 0;
	pthread_mutex_lock(&ids_lock);
	for (uint32_t i = 0; i < n_ids && id == 0; i++)
		if (strcmp(ids[i].key, key) == 0)
			id = ids[i].id;
	if (id == 0) {
		id = shim_new_id();
		if (n_ids == cap_ids) {
			uint32_t cap = cap_ids ? cap_ids * 2 : 256;
			struct id_entry *n = realloc(ids, cap * sizeof(*n));
			if (n) {
				ids = n;
				cap_ids = cap;
			}
		}
		if (n_ids < cap_ids && (ids[n_ids].key = strdup(key)) != NULL)
			ids[n_ids++].id = id;
	}
	pthread_mutex_unlock(&ids_lock);
	return id;
}

static bool key_of_id(uint32_t id, char *buf, size_t size)
{
	bool found = false;
	pthread_mutex_lock(&ids_lock);
	for (uint32_t i = 0; i < n_ids && !found; i++)
		if (ids[i].id == id) {
			snprintf(buf, size, "%s", ids[i].key);
			found = true;
		}
	pthread_mutex_unlock(&ids_lock);
	return found;
}

/* ------------------------------------------- the application's own nodes */

static pthread_mutex_t own_lock = PTHREAD_MUTEX_INITIALIZER;
static struct own_node { uint32_t id; char name[256]; char media_class[32]; } own_nodes[64];
static uint32_t n_own_nodes;
static struct own_port { const jack_port_t *port; uint32_t node; } *own_ports;
static uint32_t n_own_ports, cap_own_ports;

void shim_own_node(uint32_t id, const char *name, const char *media_class)
{
	pthread_mutex_lock(&own_lock);
	uint32_t i;
	for (i = 0; i < n_own_nodes && own_nodes[i].id != id; i++)
		;
	if (i == n_own_nodes && n_own_nodes < SPA_N_ELEMENTS(own_nodes))
		n_own_nodes++;
	if (i < n_own_nodes) {
		own_nodes[i].id = id;
		snprintf(own_nodes[i].name, sizeof(own_nodes[i].name), "%s", name ? name : "");
		snprintf(own_nodes[i].media_class, sizeof(own_nodes[i].media_class), "%s",
				media_class ? media_class : "");
	}
	pthread_mutex_unlock(&own_lock);
	shim_registry_changed();
}

void shim_own_node_gone(uint32_t id)
{
	pthread_mutex_lock(&own_lock);
	for (uint32_t i = 0; i < n_own_nodes; i++)
		if (own_nodes[i].id == id)
			own_nodes[i--] = own_nodes[--n_own_nodes];
	pthread_mutex_unlock(&own_lock);
	shim_registry_changed();
}

void shim_own_port(const jack_port_t *port, uint32_t node_id)
{
	pthread_mutex_lock(&own_lock);
	if (n_own_ports == cap_own_ports) {
		uint32_t cap = cap_own_ports ? cap_own_ports * 2 : 64;
		struct own_port *n = realloc(own_ports, cap * sizeof(*n));
		if (n) {
			own_ports = n;
			cap_own_ports = cap;
		}
	}
	if (n_own_ports < cap_own_ports) {
		own_ports[n_own_ports].port = port;
		own_ports[n_own_ports++].node = node_id;
	}
	pthread_mutex_unlock(&own_lock);
	shim_registry_changed();
}

void shim_own_port_gone(const jack_port_t *port)
{
	pthread_mutex_lock(&own_lock);
	for (uint32_t i = 0; i < n_own_ports; i++)
		if (own_ports[i].port == port)
			own_ports[i--] = own_ports[--n_own_ports];
	pthread_mutex_unlock(&own_lock);
	shim_registry_changed();
}

static uint32_t own_port_node(const jack_port_t *port)
{
	uint32_t node = 0;
	pthread_mutex_lock(&own_lock);
	for (uint32_t i = 0; i < n_own_ports && node == 0; i++)
		if (own_ports[i].port == port)
			node = own_ports[i].node;
	pthread_mutex_unlock(&own_lock);
	return node;
}

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

/* as in PipeWire, a context takes its cores with it */
static void disconnect_cores_of(struct pw_context *context)
{
	for (;;) {
		struct pw_core *c, *found = NULL;
		pthread_mutex_lock(&cores_lock);
		spa_list_for_each(c, &cores, link) {
			if (c->context == context) {
				found = c;
				break;
			}
		}
		pthread_mutex_unlock(&cores_lock);
		if (found == NULL)
			return;
		pw_core_disconnect(found);
	}
}

SHIM_EXPORT void pw_context_destroy(struct pw_context *context)
{
	if (context == NULL)
		return;
	spa_hook_list_call(&context->listeners, struct pw_context_events, destroy, 0);
	disconnect_cores_of(context);
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

/* ---------------------------------------------------------------- graph */

static struct gobj *gfind_id(struct pw_core *core, enum gtype type, uint32_t id)
{
	struct gobj *g;
	spa_list_for_each(g, &core->graph, link)
		if (g->type == type && g->id == id)
			return g;
	return NULL;
}

static struct gobj *gfind(struct pw_core *core, enum gtype type, const char *key)
{
	struct gobj *g;
	spa_list_for_each(g, &core->graph, link)
		if (g->type == type && strcmp(g->key, key) == 0)
			return g;
	return NULL;
}

/* the object, marked seen in this scan; made (fresh) if it is new */
static struct gobj *gget(struct pw_core *core, enum gtype type, const char *key, uint32_t id)
{
	struct gobj *g = gfind(core, type, key);
	if (g == NULL) {
		if ((g = calloc(1, sizeof(*g))) == NULL)
			return NULL;
		g->type = type;
		snprintf(g->key, sizeof(g->key), "%s", key);
		if (id == 0) {
			char k[sizeof(g->key) + 3];
			snprintf(k, sizeof(k), "%c:%s", "npl"[type], key);
			id = stable_id(k);
		}
		g->id = id;
		g->fresh = true;
		spa_list_append(&core->graph, &g->link);
	}
	g->seen = true;
	return g;
}

static struct gobj *default_device(struct pw_core *core, bool playback)
{
	struct gobj *g;
	spa_list_for_each(g, &core->graph, link)
		if (g->type == G_NODE && g->kind == (playback ? N_PLAYBACK : N_CAPTURE))
			return g;
	return NULL;
}

static void emit_global(struct shim_proxy *r, struct pw_core *core, struct gobj *g);
static void emit_metadata(struct pw_core *core, struct work *w);

/* JACK's graph as objects; tell the registries what changed */
static void rescan(struct pw_core *core)
{
	jack_client_t *c = shim_app_client();
	struct gobj *g, *t;
	struct shim_proxy *p;
	struct gobj *old_sink = default_device(core, true), *old_source = default_device(core, false);
	uint32_t old_sink_id = old_sink ? old_sink->id : SPA_ID_INVALID;
	uint32_t old_source_id = old_source ? old_source->id : SPA_ID_INVALID;
	char key[2 * PORT_NAME_MAX + 2];

	if (c == NULL)
		return;
	spa_list_for_each(g, &core->graph, link) {
		g->seen = false;
		g->n_in = g->n_out = 0;
	}

	/* the application's own filters and streams, with or without ports */
	struct own_node own[64];
	pthread_mutex_lock(&own_lock);
	uint32_t n_own = n_own_nodes;
	memcpy(own, own_nodes, n_own * sizeof(own[0]));
	pthread_mutex_unlock(&own_lock);
	for (uint32_t i = 0; i < n_own; i++) {
		snprintf(key, sizeof(key), "own:%u", own[i].id);
		if ((g = gget(core, G_NODE, key, own[i].id)) == NULL)
			continue;
		g->kind = N_OWN;
		snprintf(g->name, sizeof(g->name), "%s", own[i].name);
		snprintf(g->desc, sizeof(g->desc), "%s", own[i].name);
		snprintf(g->media_class, sizeof(g->media_class), "%s", own[i].media_class);
		snprintf(g->client, sizeof(g->client), "%s", jack_get_client_name(c));
	}

	/* every port, and its node */
	const char **ports = jack_get_ports(c, NULL, NULL, 0);
	for (size_t i = 0; ports && ports[i]; i++) {
		jack_port_t *jp = jack_port_by_name(c, ports[i]);
		const char *colon = strchr(ports[i], ':');
		if (jp == NULL || colon == NULL)
			continue;
		const char *ptype = jack_port_type(jp);
		bool midi = ptype && strcmp(ptype, JACK_DEFAULT_MIDI_TYPE) == 0;
		bool audio = ptype && strcmp(ptype, JACK_DEFAULT_AUDIO_TYPE) == 0;
		if (!midi && !audio)
			continue;
		int flags = jack_port_flags(jp);
		bool out = flags & JackPortIsOutput;
		bool physical = flags & JackPortIsPhysical;
		char client[CLIENT_NAME_MAX];
		snprintf(client, sizeof(client), "%.*s", (int)(colon - ports[i]), ports[i]);

		uint32_t own_node = jack_port_is_mine(c, jp) ? own_port_node(jp) : 0;
		enum nkind kind = own_node ? N_OWN : !physical ? N_CLIENT :
			midi ? N_MIDI : out ? N_CAPTURE : N_PLAYBACK;
		struct gobj *n;
		if (kind == N_OWN) {
			snprintf(key, sizeof(key), "own:%u", own_node);
			n = gfind(core, G_NODE, key);
		} else {
			if (kind == N_CLIENT)
				snprintf(key, sizeof(key), "%s", client);
			else
				snprintf(key, sizeof(key), "jackbus.%s.%s", client,
						kind == N_MIDI ? "midi" : kind == N_CAPTURE ? "capture" : "playback");
			if ((n = gget(core, G_NODE, key, 0)) != NULL) {
				n->kind = kind;
				snprintf(n->name, sizeof(n->name), "%.*s", (int)sizeof(n->name) - 1, key);
				snprintf(n->client, sizeof(n->client), "%s", client);
				if (kind == N_CLIENT)
					snprintf(n->desc, sizeof(n->desc), "%s", client);
				else
					snprintf(n->desc, sizeof(n->desc), "%s %s (JackBus)", client,
							kind == N_MIDI ? "MIDI" : kind == N_CAPTURE ? "capture" : "playback");
			}
		}
		if (n == NULL)
			continue;
		struct gobj *pt = gget(core, G_PORT, ports[i], 0);
		if (pt == NULL)
			continue;
		pt->node_id = n->id;
		pt->out = out;
		pt->physical = physical;
		pt->midi = midi;
		pt->short_name = strchr(pt->key, ':') + 1;
		pt->index = out ? n->n_out++ : n->n_in++;
	}

	/* every connection, from its output */
	for (size_t i = 0; ports && ports[i]; i++) {
		struct gobj *po = gfind(core, G_PORT, ports[i]);
		if (po == NULL || !po->seen || !po->out)
			continue;
		jack_port_t *jp = jack_port_by_name(c, ports[i]);
		const char **conns = jp ? jack_port_get_all_connections(c, jp) : NULL;
		for (size_t k = 0; conns && conns[k]; k++) {
			struct gobj *pi = gfind(core, G_PORT, conns[k]);
			if (pi == NULL || !pi->seen)
				continue;
			snprintf(key, sizeof(key), "%s>%s", ports[i], conns[k]);
			struct gobj *l = gget(core, G_LINK, key, 0);
			if (l == NULL)
				continue;
			l->out_port = po->id;
			l->in_port = pi->id;
			l->out_node = po->node_id;
			l->in_node = pi->node_id;
		}
		jack_free(conns);
	}
	jack_free(ports);

	/* media classes, from the ports */
	spa_list_for_each(g, &core->graph, link) {
		if (g->type != G_NODE)
			continue;
		switch (g->kind) {
		case N_PLAYBACK: snprintf(g->media_class, sizeof(g->media_class), "Audio/Sink"); break;
		case N_CAPTURE: snprintf(g->media_class, sizeof(g->media_class), "Audio/Source"); break;
		case N_MIDI: snprintf(g->media_class, sizeof(g->media_class), "Midi/Bridge"); break;
		case N_CLIENT:
			snprintf(g->media_class, sizeof(g->media_class), "%s",
					g->n_in && g->n_out ? "Audio/Duplex" :
					g->n_out ? "Stream/Output/Audio" : "Stream/Input/Audio");
			break;
		case N_OWN:
			break;
		}
	}

	/* what went: links, then ports, then nodes */
	for (int type = G_LINK; type >= G_NODE; type--) {
		spa_list_for_each_safe(g, t, &core->graph, link) {
			if (g->seen || (int)g->type != type)
				continue;
			spa_list_for_each(p, &core->proxies, link) {
				if (p->kind == PROXY_REGISTRY)
					spa_hook_list_call(&p->object_listeners, struct pw_registry_events,
							global_remove, 0, g->id);
				else if (p->id == g->id && !p->removed && p->kind != PROXY_MADE_LINK) {
					p->removed = true;
					spa_hook_list_call(&p->proxy_listeners, struct pw_proxy_events, removed, 0);
				}
			}
			if (g->type == G_NODE)
				shim_log("registry: node %s is gone", g->name);
			spa_list_remove(&g->link);
			free(g);
		}
	}
	/* what came: nodes, then ports, then links */
	for (int type = G_NODE; type <= G_LINK; type++) {
		spa_list_for_each(g, &core->graph, link) {
			if (!g->fresh || (int)g->type != type)
				continue;
			g->fresh = false;
			if (g->type == G_NODE)
				shim_log("registry: node %s (%s, id %u)", g->name, g->media_class, g->id);
			spa_list_for_each(p, &core->proxies, link)
				if (p->kind == PROXY_REGISTRY)
					emit_global(p, core, g);
		}
	}
	/* a new default device: tell the bound "default" metadata */
	struct gobj *sink = default_device(core, true), *source = default_device(core, false);
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

/* a device by its node name, serial or id; a JACK client by its name */
const char **shim_target_ports(jack_client_t *c, const char *target, uint32_t target_id, bool playback)
{
	char who[CLIENT_NAME_MAX] = "";
	char key[2 * PORT_NAME_MAX + 4] = "";
	uint32_t id = target_id;
	bool physical = true;

	if (target && target[0]) {
		char *end;
		unsigned long v = strtoul(target, &end, 10);
		if (*end == 0)
			id = (uint32_t)v;
		else
			snprintf(key, sizeof(key), "n:%s", target);
	}
	if (key[0] == 0 && id != SPA_ID_INVALID && id != PW_ID_ANY && !key_of_id(id, key, sizeof(key))) {
		shim_log("target %u is not a JackBus node: the default", id);
		key[0] = 0;
	}
	if (key[0]) {
		const char *name = key + 2;
		const char *dir = playback ? ".playback" : ".capture";
		size_t len = strlen(name), dl = strlen(dir);
		if (spa_strstartswith(name, "jackbus.") && len > 8 + dl && strcmp(name + len - dl, dir) == 0) {
			snprintf(who, sizeof(who), "%.*s", (int)(len - 8 - dl), name + 8);
		} else {
			snprintf(who, sizeof(who), "%s", name);		/* a JACK client */
			physical = false;
		}
	}

	const char **ports = jack_get_ports(c, NULL, JACK_DEFAULT_AUDIO_TYPE,
			(physical ? JackPortIsPhysical : 0) | (playback ? JackPortIsInput : JackPortIsOutput));
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
	/* the default: the first physical client's ports */
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

/* --------------------------------------------------------- object props */

struct props {
	struct spa_dict_item items[24];
	char buf[12][64];
	uint32_t n, nb;
};

static const char *pnum(struct props *p, uint32_t v)
{
	char *b = p->buf[p->nb++ % SPA_N_ELEMENTS(p->buf)];
	snprintf(b, 64, "%u", v);
	return b;
}

static void padd(struct props *p, const char *key, const char *value)
{
	if (p->n < SPA_N_ELEMENTS(p->items) && value)
		p->items[p->n++] = SPA_DICT_ITEM_INIT(key, value);
}

static void props_of(struct pw_core *core, struct gobj *g, struct props *p)
{
	jack_client_t *c = shim_app_client();
	p->n = p->nb = 0;
	padd(p, PW_KEY_OBJECT_ID, pnum(p, g->id));
	padd(p, PW_KEY_OBJECT_SERIAL, pnum(p, g->id));
	switch (g->type) {
	case G_NODE:
		padd(p, PW_KEY_NODE_NAME, g->name);
		padd(p, PW_KEY_NODE_DESCRIPTION, g->desc);
		padd(p, PW_KEY_NODE_NICK, g->client);
		if (g->media_class[0])
			padd(p, PW_KEY_MEDIA_CLASS, g->media_class);
		padd(p, "jack.client-name", g->client);
		if (g->kind == N_PLAYBACK || g->kind == N_CAPTURE) {
			padd(p, PW_KEY_AUDIO_CHANNELS, pnum(p, g->kind == N_PLAYBACK ? g->n_in : g->n_out));
			padd(p, PW_KEY_AUDIO_RATE, pnum(p, c ? jack_get_sample_rate(c) : 48000));
			padd(p, PW_KEY_PRIORITY_SESSION,
					g == default_device(core, g->kind == N_PLAYBACK) ? "1000" : "900");
		}
		if (g->kind == N_PLAYBACK || g->kind == N_CAPTURE || g->kind == N_MIDI)
			padd(p, PW_KEY_DEVICE_API, "jackbus");
		break;
	case G_PORT:
		padd(p, PW_KEY_PORT_ID, pnum(p, g->index));
		padd(p, PW_KEY_PORT_NAME, g->short_name);
		padd(p, PW_KEY_PORT_ALIAS, g->key);
		padd(p, PW_KEY_OBJECT_PATH, g->key);
		padd(p, PW_KEY_PORT_DIRECTION, g->out ? "out" : "in");
		padd(p, PW_KEY_NODE_ID, pnum(p, g->node_id));
		padd(p, PW_KEY_FORMAT_DSP, g->midi ? "8 bit raw midi" : "32 bit float mono audio");
		if (g->physical) {
			padd(p, PW_KEY_PORT_PHYSICAL, "true");
			padd(p, PW_KEY_PORT_TERMINAL, "true");
		}
		break;
	case G_LINK:
		padd(p, PW_KEY_LINK_OUTPUT_PORT, pnum(p, g->out_port));
		padd(p, PW_KEY_LINK_INPUT_PORT, pnum(p, g->in_port));
		padd(p, PW_KEY_LINK_OUTPUT_NODE, pnum(p, g->out_node));
		padd(p, PW_KEY_LINK_INPUT_NODE, pnum(p, g->in_node));
		break;
	}
}

static void emit_global(struct shim_proxy *r, struct pw_core *core, struct gobj *g)
{
	static const char *const types[] = {
		PW_TYPE_INTERFACE_Node, PW_TYPE_INTERFACE_Port, PW_TYPE_INTERFACE_Link,
	};
	static const uint32_t versions[] = { PW_VERSION_NODE, PW_VERSION_PORT, PW_VERSION_LINK };
	struct props p;
	props_of(core, g, &p);
	struct spa_dict dict = SPA_DICT_INIT(p.items, p.n);
	spa_hook_list_call(&r->object_listeners, struct pw_registry_events, global, 0,
			g->id, PW_PERM_R | PW_PERM_X, types[g->type], versions[g->type], &dict);
}

static void emit_metadata_global(struct shim_proxy *r, uint32_t id)
{
	struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT(PW_KEY_METADATA_NAME, id == ID_METADATA_DEFAULT ? "default" : "settings"),
	};
	struct spa_dict dict = SPA_DICT_INIT_ARRAY(items);
	spa_hook_list_call(&r->object_listeners, struct pw_registry_events, global, 0,
			id, PW_PERM_R | PW_PERM_X, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, &dict);
}

static void emit_globals(struct pw_core *core, struct work *w)
{
	struct gobj *g;
	emit_metadata_global(w->proxy, ID_METADATA_DEFAULT);
	emit_metadata_global(w->proxy, ID_METADATA_SETTINGS);
	for (int type = G_NODE; type <= G_LINK; type++)
		spa_list_for_each(g, &core->graph, link)
			if ((int)g->type == type)
				emit_global(w->proxy, core, g);
}

/* -------------------------------------------------------------- proxies */

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
	if (p->kind == PROXY_MADE_LINK) {
		jack_client_t *c = shim_app_client();
		for (uint32_t i = 0; c && !p->linger && i < p->n_pairs; i++)
			jack_disconnect(c, p->pairs[i][0], p->pairs[i][1]);
		free(p->pairs);
	}
	if (p->app_ref)
		shim_app_unref();
	free(p);
}

static void emit_bound(struct pw_core *core, struct shim_proxy *p, struct props *pr)
{
	if (p->bound_sent)
		return;
	p->bound_sent = true;
	if (pr) {
		struct spa_dict dict = SPA_DICT_INIT(pr->items, pr->n);
		spa_hook_list_call(&p->proxy_listeners, struct pw_proxy_events, bound_props, 0, p->id, &dict);
	}
	spa_hook_list_call(&p->proxy_listeners, struct pw_proxy_events, bound, 0, p->id);
}

static int obj_add_listener(void *object, struct spa_hook *listener, const void *events, void *data)
{
	struct shim_proxy *p = object;
	spa_hook_list_append(&p->object_listeners, listener, events, data);
	return 0;
}

/* nodes */

static int node_add_listener(void *object, struct spa_hook *listener,
		const struct pw_node_events *events, void *data)
{
	return obj_add_listener(object, listener, events, data);
}

/* a device's formats: JackBus's rate as a range (default, min, max), as
 * PipeWire's nodes publish it (SDL reads only that form) */
static void emit_param(struct pw_core *core, struct work *w)
{
	struct shim_proxy *p = w->proxy;
	struct gobj *g = gfind_id(core, G_NODE, p->id);
	jack_client_t *c = shim_app_client();
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	uint32_t pos[SPA_AUDIO_MAX_CHANNELS];

	if (g == NULL || (g->kind != N_PLAYBACK && g->kind != N_CAPTURE) ||
	    (w->param_id != SPA_PARAM_EnumFormat && w->param_id != SPA_PARAM_Format))
		return;
	int rate = c ? (int)jack_get_sample_rate(c) : 48000;
	uint32_t ch = SPA_MIN(g->kind == N_PLAYBACK ? g->n_in : g->n_out, (uint32_t)SPA_AUDIO_MAX_CHANNELS);
	for (uint32_t i = 0; i < ch; i++)
		pos[i] = (ch == 2) ? (i ? SPA_AUDIO_CHANNEL_FR : SPA_AUDIO_CHANNEL_FL) :
			(ch == 1) ? SPA_AUDIO_CHANNEL_MONO : SPA_AUDIO_CHANNEL_AUX0 + i;
	struct spa_pod_frame f;
	spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, w->param_id);
	spa_pod_builder_add(&b,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_AUDIO_format, SPA_POD_Id(SPA_AUDIO_FORMAT_F32P),
			SPA_FORMAT_AUDIO_rate, SPA_POD_CHOICE_RANGE_Int(rate, rate, rate),
			SPA_FORMAT_AUDIO_channels, SPA_POD_Int((int)ch),
			0);
	spa_pod_builder_prop(&b, SPA_FORMAT_AUDIO_position, 0);
	spa_pod_builder_array(&b, sizeof(uint32_t), SPA_TYPE_Id, ch, pos);
	struct spa_pod *param = spa_pod_builder_pop(&b, &f);
	spa_hook_list_call(&p->object_listeners, struct pw_node_events, param, 0,
			w->seq, w->param_id, 0, 1, param);
}

static int node_subscribe_params(void *object, uint32_t *ids_, uint32_t n_ids)
{
	struct shim_proxy *p = object;
	for (uint32_t i = 0; i < n_ids; i++)
		defer(p->core, emit_param, p, 0, 0, ids_[i]);
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
	return 0;	/* JackBus's settings are its own */
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
	struct gobj *g = gfind_id(core, G_NODE, p->id);
	struct props pr;

	if (g == NULL)
		return;
	props_of(core, g, &pr);
	emit_bound(core, p, &pr);
	struct spa_dict dict = SPA_DICT_INIT(pr.items, pr.n);
	bool device = g->kind == N_PLAYBACK || g->kind == N_CAPTURE;
	struct spa_param_info params[2] = {
		SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ),
		SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READ),
	};
	struct pw_node_info info = {
		.id = g->id,
		.max_input_ports = g->n_in,
		.max_output_ports = g->n_out,
		.change_mask = PW_NODE_CHANGE_MASK_ALL,
		.n_input_ports = g->n_in,
		.n_output_ports = g->n_out,
		.state = PW_NODE_STATE_RUNNING,
		.error = NULL,
		.props = &dict,
		.params = device ? params : NULL,
		.n_params = device ? 2 : 0,
	};
	spa_hook_list_call(&p->object_listeners, struct pw_node_events, info, 0, &info);
}

/* ports */

static int port_add_listener(void *object, struct spa_hook *listener,
		const struct pw_port_events *events, void *data)
{
	return obj_add_listener(object, listener, events, data);
}

static int port_subscribe_params(void *object, uint32_t *ids_, uint32_t n_ids) { return 0; }

static int port_enum_params(void *object, int seq, uint32_t id, uint32_t start, uint32_t num,
		const struct spa_pod *filter)
{
	return 0;
}

static const struct pw_port_methods port_methods = {
	PW_VERSION_PORT_METHODS,
	.add_listener = port_add_listener,
	.subscribe_params = port_subscribe_params,
	.enum_params = port_enum_params,
};

static void emit_port_info(struct pw_core *core, struct work *w)
{
	struct shim_proxy *p = w->proxy;
	struct gobj *g = gfind_id(core, G_PORT, p->id);
	struct props pr;

	if (g == NULL)
		return;
	props_of(core, g, &pr);
	emit_bound(core, p, &pr);
	struct spa_dict dict = SPA_DICT_INIT(pr.items, pr.n);
	struct pw_port_info info = {
		.id = g->id,
		.direction = g->out ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT,
		.change_mask = PW_PORT_CHANGE_MASK_ALL,
		.props = &dict,
		.params = NULL,
		.n_params = 0,
	};
	spa_hook_list_call(&p->object_listeners, struct pw_port_events, info, 0, &info);
}

/* links: bound globals, and those made through link-factory */

static int link_add_listener(void *object, struct spa_hook *listener,
		const struct pw_link_events *events, void *data)
{
	return obj_add_listener(object, listener, events, data);
}

static const struct pw_link_methods link_methods = {
	PW_VERSION_LINK_METHODS,
	.add_listener = link_add_listener,
};

static void emit_link_info(struct pw_core *core, struct work *w)
{
	struct shim_proxy *p = w->proxy;
	struct gobj *g = gfind_id(core, G_LINK, p->id);
	struct props pr;

	if (g == NULL) {
		if (p->kind == PROXY_MADE_LINK)
			emit_bound(core, p, NULL);
		return;
	}
	props_of(core, g, &pr);
	emit_bound(core, p, &pr);
	struct spa_dict dict = SPA_DICT_INIT(pr.items, pr.n);
	struct pw_link_info info = {
		.id = g->id,
		.output_node_id = g->out_node,
		.output_port_id = g->out_port,
		.input_node_id = g->in_node,
		.input_port_id = g->in_port,
		.change_mask = PW_LINK_CHANGE_MASK_ALL,
		.state = PW_LINK_STATE_ACTIVE,
		.error = NULL,
		.format = NULL,
		.props = &dict,
	};
	spa_hook_list_call(&p->object_listeners, struct pw_link_events, info, 0, &info);
}

/* metadata: "default" and "settings" */

static int metadata_add_listener(void *object, struct spa_hook *listener,
		const struct pw_metadata_events *events, void *data)
{
	return obj_add_listener(object, listener, events, data);
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
	char v[PORT_NAME_MAX + 32];

	emit_bound(core, p, NULL);
	if (p->id == ID_METADATA_DEFAULT) {
		for (int playback = 1; playback >= 0; playback--) {
			struct gobj *d = default_device(core, playback);
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
	return obj_add_listener(object, listener, events, data);
}

static void *registry_bind(void *object, uint32_t id, const char *type,
		uint32_t version, size_t user_data_size)
{
	struct shim_proxy *r = object;
	struct pw_core *core = r->core;
	struct shim_proxy *p = NULL;

	if (id == ID_METADATA_DEFAULT || id == ID_METADATA_SETTINGS) {
		if (!spa_streq(type, PW_TYPE_INTERFACE_Metadata))
			goto wrong;
		p = proxy_new(core, PROXY_METADATA, id, PW_TYPE_INTERFACE_Metadata,
				PW_VERSION_METADATA, &metadata_methods, user_data_size);
		if (p)
			defer(core, emit_metadata, p, 0, 0, 0);
		return p;
	}
	if (spa_streq(type, PW_TYPE_INTERFACE_Node) && gfind_id(core, G_NODE, id)) {
		if ((p = proxy_new(core, PROXY_NODE, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE,
				&node_methods, user_data_size)))
			defer(core, emit_node_info, p, 0, 0, 0);
	} else if (spa_streq(type, PW_TYPE_INTERFACE_Port) && gfind_id(core, G_PORT, id)) {
		if ((p = proxy_new(core, PROXY_PORT, id, PW_TYPE_INTERFACE_Port, PW_VERSION_PORT,
				&port_methods, user_data_size)))
			defer(core, emit_port_info, p, 0, 0, 0);
	} else if (spa_streq(type, PW_TYPE_INTERFACE_Link) && gfind_id(core, G_LINK, id)) {
		if ((p = proxy_new(core, PROXY_LINK, id, PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
				&link_methods, user_data_size)))
			defer(core, emit_link_info, p, 0, 0, 0);
	} else {
		goto wrong;
	}
	return p;
wrong:
	shim_log("registry: bind %u as %s: no such object", id, type);
	errno = ENOENT;
	return NULL;
}

/* destroying a link global disconnects it in JACK */
static int registry_destroy(void *object, uint32_t id)
{
	struct shim_proxy *r = object;
	struct gobj *l = gfind_id(r->core, G_LINK, id);
	jack_client_t *c = shim_app_client();
	char out[PORT_NAME_MAX];
	const char *sep;

	if (l == NULL || c == NULL)
		return -EPERM;
	if ((sep = strchr(l->key, '>')) == NULL)
		return -EINVAL;
	snprintf(out, sizeof(out), "%.*s", (int)(sep - l->key), l->key);
	return jack_disconnect(c, out, sep + 1) == 0 ? 0 : -EIO;
}

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

/* --------------------------------------------------------- link-factory */

static uint32_t dict_u32(const struct spa_dict *props, const char *key)
{
	const char *v = props ? spa_dict_lookup(props, key) : NULL;
	char *end;
	if (v == NULL || v[0] == 0)
		return SPA_ID_INVALID;
	unsigned long n = strtoul(v, &end, 10);
	return *end == 0 ? (uint32_t)n : SPA_ID_INVALID;
}

static int add_pair(struct shim_proxy *p, const char *out, const char *in)
{
	void *n = realloc(p->pairs, (p->n_pairs + 1) * sizeof(p->pairs[0]));
	if (n == NULL)
		return -ENOMEM;
	p->pairs = n;
	snprintf(p->pairs[p->n_pairs][0], PORT_NAME_MAX, "%s", out);
	snprintf(p->pairs[p->n_pairs][1], PORT_NAME_MAX, "%s", in);
	p->n_pairs++;
	return 0;
}

/* ports of node `node` in direction `out`, in their order */
static uint32_t node_ports(struct pw_core *core, uint32_t node, bool out, struct gobj **res, uint32_t max)
{
	uint32_t n = 0;
	struct gobj *g;
	for (uint32_t idx = 0; n < max; idx++) {
		bool found = false;
		spa_list_for_each(g, &core->graph, link) {
			if (g->type == G_PORT && g->node_id == node && g->out == out && g->index == idx) {
				res[n++] = g;
				found = true;
				break;
			}
		}
		if (!found)
			break;
	}
	return n;
}

static void *make_link(struct pw_core *core, const struct spa_dict *props, size_t user_data_size)
{
	jack_client_t *c = shim_app_client();
	if (c == NULL) {
		errno = EIO;
		return NULL;
	}
	rescan(core);		/* ids made since the last look, by the application itself */
	uint32_t op = dict_u32(props, PW_KEY_LINK_OUTPUT_PORT), ip = dict_u32(props, PW_KEY_LINK_INPUT_PORT);
	uint32_t on = dict_u32(props, PW_KEY_LINK_OUTPUT_NODE), in = dict_u32(props, PW_KEY_LINK_INPUT_NODE);
	const char *linger = props ? spa_dict_lookup(props, PW_KEY_OBJECT_LINGER) : NULL;
	struct shim_proxy *p = proxy_new(core, PROXY_MADE_LINK, SPA_ID_INVALID, PW_TYPE_INTERFACE_Link,
			PW_VERSION_LINK, &link_methods, user_data_size);
	if (p == NULL)
		return NULL;
	p->linger = linger && spa_atob(linger);

	if (op != SPA_ID_INVALID && ip != SPA_ID_INVALID) {
		struct gobj *po = gfind_id(core, G_PORT, op), *pi = gfind_id(core, G_PORT, ip);
		if (po && pi && po->out && !pi->out)
			add_pair(p, po->key, pi->key);
	} else if (on != SPA_ID_INVALID && in != SPA_ID_INVALID) {
		struct gobj *outs[64], *ins[64];
		uint32_t no = node_ports(core, on, true, outs, 64), ni = node_ports(core, in, false, ins, 64);
		for (uint32_t i = 0; i < no && i < ni; i++)
			add_pair(p, outs[i]->key, ins[i]->key);
	}
	if (p->n_pairs == 0) {
		shim_log("link-factory: no such ports (output node %u port %u, input node %u port %u)",
				on, op, in, ip);
		proxy_free(p);
		errno = ENOENT;
		return NULL;
	}
	for (uint32_t i = 0; i < p->n_pairs; i++) {
		int res = jack_connect(c, p->pairs[i][0], p->pairs[i][1]);
		if (res != 0 && res != EEXIST)
			shim_log("link-factory: %s -> %s failed (%d)", p->pairs[i][0], p->pairs[i][1], res);
	}
	char key[2 * PORT_NAME_MAX + 4];
	snprintf(key, sizeof(key), "l:%s>%s", p->pairs[0][0], p->pairs[0][1]);
	p->id = stable_id(key);			/* the link global's, once it shows */
	shim_log("link-factory: %s -> %s%s", p->pairs[0][0], p->pairs[0][1],
			p->n_pairs > 1 ? " (and more)" : "");
	defer(core, emit_link_info, p, 0, 0, 0);
	return p;
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
	/* the application's client, to read JACK's graph (not in the cycle) */
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
	struct pw_core *core = object;
	if (spa_streq(factory_name, "link-factory") && spa_streq(type, PW_TYPE_INTERFACE_Link))
		return make_link(core, props, user_data_size);
	shim_log("create_object(%s, %s) is not supported on JackBus", factory_name, type);
	errno = ENOTSUP;
	return NULL;
}

static int core_destroy_proxy(void *object, void *proxy)
{
	if (proxy == NULL)
		return -EINVAL;
	proxy_free(proxy);
	return 0;
}

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
	spa_list_init(&core->graph);
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
	struct gobj *g;
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
	spa_list_consume(g, &core->graph, link) {
		spa_list_remove(&g->link);
		free(g);
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
