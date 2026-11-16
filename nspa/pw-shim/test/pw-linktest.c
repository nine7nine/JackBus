/*
 * jackbus-pw test: the registry and links, the way libremidi (ossia score)
 * uses them. A pw_filter with two inputs and two outputs, linked against the
 * system's libpipewire:
 *  1. waits until the registry shows its own node (pw_filter_get_node_id())
 *     with its four ports, and the default sink and source (the "default"
 *     metadata);
 *  2. links the default source's outputs to its inputs and its outputs to
 *     the default sink's inputs through link-factory, by port id, and waits
 *     for the four Link globals;
 *  3. destroys the two output links' proxies and waits for those globals to
 *     go;
 *  4. links its node to the sink by node id and waits for the two links.
 * Prints PASS or FAIL for each step.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <pipewire/extensions/metadata.h>
#include <spa/utils/json.h>

#define MAX_OBJS 512

struct obj {
	uint32_t id;
	int type;			/* 0 node, 1 port, 2 link */
	uint32_t node_id;
	bool out;
	char name[256];
	uint32_t lout, lin;		/* link ports */
};

struct data {
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_registry *registry;
	struct spa_hook registry_listener, metadata_listener;
	struct pw_proxy *metadata;
	struct pw_filter *filter;
	void *in[2], *out[2];
	struct obj objs[MAX_OBJS];
	int n_objs;
	char sink[256], source[256];
	struct pw_proxy *links[4];
	struct pw_proxy *node_link;
	int step, ticks, fails;
	struct spa_source *timer;
};

static struct obj *find(struct data *d, uint32_t id)
{
	for (int i = 0; i < d->n_objs; i++)
		if (d->objs[i].id == id)
			return &d->objs[i];
	return NULL;
}

static struct obj *node_named(struct data *d, const char *name)
{
	for (int i = 0; i < d->n_objs; i++)
		if (d->objs[i].type == 0 && strcmp(d->objs[i].name, name) == 0)
			return &d->objs[i];
	return NULL;
}

/* the ports of a node in one direction, in registry order */
static int ports_of(struct data *d, uint32_t node, bool out, uint32_t *ids, int max)
{
	int n = 0;
	for (int i = 0; i < d->n_objs && n < max; i++)
		if (d->objs[i].type == 1 && d->objs[i].node_id == node && d->objs[i].out == out)
			ids[n++] = d->objs[i].id;
	return n;
}

static bool has_link(struct data *d, uint32_t out, uint32_t in)
{
	for (int i = 0; i < d->n_objs; i++)
		if (d->objs[i].type == 2 && d->objs[i].lout == out && d->objs[i].lin == in)
			return true;
	return false;
}

static int on_property(void *data, uint32_t subject, const char *key, const char *type, const char *value)
{
	struct data *d = data;
	char name[256];
	struct spa_json it[2];
	if (key == NULL || value == NULL)
		return 0;
	char *dst = strcmp(key, "default.audio.sink") == 0 ? d->sink :
		strcmp(key, "default.audio.source") == 0 ? d->source : NULL;
	if (dst == NULL)
		return 0;
	spa_json_init(&it[0], value, strlen(value));
	if (spa_json_enter_object(&it[0], &it[1]) > 0) {
		char k[64];
		while (spa_json_get_string(&it[1], k, sizeof(k)) > 0) {
			if (strcmp(k, "name") == 0 && spa_json_get_string(&it[1], name, sizeof(name)) > 0) {
				snprintf(dst, 256, "%s", name);
				break;
			}
			spa_json_next(&it[1], &(const char *){NULL});
		}
	}
	return 0;
}

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = on_property,
};

static void on_global(void *data, uint32_t id, uint32_t permissions, const char *type,
		uint32_t version, const struct spa_dict *props)
{
	struct data *d = data;
	const char *v;
	if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
		v = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
		if (v && strcmp(v, "default") == 0 && d->metadata == NULL) {
			d->metadata = pw_registry_bind(d->registry, id, type, PW_VERSION_METADATA, 0);
			pw_metadata_add_listener((struct pw_metadata *)d->metadata, &d->metadata_listener,
					&metadata_events, d);
		}
		return;
	}
	if (d->n_objs == MAX_OBJS)
		return;
	struct obj o = { .id = id, .type = -1 };
	if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
		o.type = 0;
		if ((v = spa_dict_lookup(props, PW_KEY_NODE_NAME)))
			snprintf(o.name, sizeof(o.name), "%s", v);
	} else if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
		o.type = 1;
		if ((v = spa_dict_lookup(props, PW_KEY_NODE_ID)))
			o.node_id = atoi(v);
		o.out = (v = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION)) && strcmp(v, "out") == 0;
		if ((v = spa_dict_lookup(props, PW_KEY_PORT_ALIAS)))
			snprintf(o.name, sizeof(o.name), "%s", v);
	} else if (strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
		o.type = 2;
		if ((v = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_PORT)))
			o.lout = atoi(v);
		if ((v = spa_dict_lookup(props, PW_KEY_LINK_INPUT_PORT)))
			o.lin = atoi(v);
	}
	if (o.type >= 0)
		d->objs[d->n_objs++] = o;
}

static void on_global_remove(void *data, uint32_t id)
{
	struct data *d = data;
	for (int i = 0; i < d->n_objs; i++)
		if (d->objs[i].id == id)
			d->objs[i--] = d->objs[--d->n_objs];
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_global,
	.global_remove = on_global_remove,
};

static struct pw_proxy *link_ports(struct data *d, uint32_t out, uint32_t in, const char *okey, const char *ikey)
{
	char o[16], i[16];
	snprintf(o, sizeof(o), "%u", out);
	snprintf(i, sizeof(i), "%u", in);
	struct pw_properties *props = pw_properties_new(okey, o, ikey, i, NULL);
	struct pw_proxy *p = pw_core_create_object(d->core, "link-factory", PW_TYPE_INTERFACE_Link,
			PW_VERSION_LINK, &props->dict, 0);
	pw_properties_free(props);
	return p;
}

static void result(struct data *d, bool ok, const char *what)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	fflush(stdout);
	if (!ok)
		d->fails++;
}

static void on_tick(void *data, uint64_t expirations)
{
	struct data *d = data;
	uint32_t self = pw_filter_get_node_id(d->filter);
	uint32_t fin[4], fout[4], sin[4], sout[4];
	struct obj *sink = node_named(d, d->sink), *source = node_named(d, d->source);
	int nfi = ports_of(d, self, false, fin, 4), nfo = ports_of(d, self, true, fout, 4);
	int nsi = sink ? ports_of(d, sink->id, false, sin, 4) : 0;
	int nso = source ? ports_of(d, source->id, true, sout, 4) : 0;
	char what[512];

	if (++d->ticks > 50) {
		result(d, false, "timed out");
		pw_main_loop_quit(d->loop);
		return;
	}
	switch (d->step) {
	case 0:
		if (!find(d, self) || nfi < 2 || nfo < 2 || nsi < 2 || nso < 2)
			return;
		snprintf(what, sizeof(what), "registry: own node %u with 2+2 ports; default sink %s, source %s",
				self, d->sink, d->source);
		result(d, true, what);
		d->links[0] = link_ports(d, sout[0], fin[0], PW_KEY_LINK_OUTPUT_PORT, PW_KEY_LINK_INPUT_PORT);
		d->links[1] = link_ports(d, sout[1], fin[1], PW_KEY_LINK_OUTPUT_PORT, PW_KEY_LINK_INPUT_PORT);
		d->links[2] = link_ports(d, fout[0], sin[0], PW_KEY_LINK_OUTPUT_PORT, PW_KEY_LINK_INPUT_PORT);
		d->links[3] = link_ports(d, fout[1], sin[1], PW_KEY_LINK_OUTPUT_PORT, PW_KEY_LINK_INPUT_PORT);
		d->step = 1;
		d->ticks = 0;
		return;
	case 1:
		if (!(has_link(d, sout[0], fin[0]) && has_link(d, sout[1], fin[1]) &&
		      has_link(d, fout[0], sin[0]) && has_link(d, fout[1], sin[1])))
			return;
		result(d, d->links[0] && d->links[3], "link-factory by port id: 4 links, all in the registry");
		pw_proxy_destroy(d->links[2]);
		pw_proxy_destroy(d->links[3]);
		d->step = 2;
		d->ticks = 0;
		return;
	case 2:
		if (has_link(d, fout[0], sin[0]) || has_link(d, fout[1], sin[1]))
			return;
		result(d, has_link(d, sout[0], fin[0]) && has_link(d, sout[1], fin[1]),
				"destroying 2 link proxies removed those links, the others stay");
		d->node_link = link_ports(d, self, sink->id, PW_KEY_LINK_OUTPUT_NODE, PW_KEY_LINK_INPUT_NODE);
		d->step = 3;
		d->ticks = 0;
		return;
	case 3:
		if (!(has_link(d, fout[0], sin[0]) && has_link(d, fout[1], sin[1])))
			return;
		result(d, d->node_link != NULL, "link-factory by node id: output i to input i");
		pw_main_loop_quit(d->loop);
		return;
	}
}

static void on_process(void *data, struct spa_io_position *position)
{
	struct data *d = data;
	uint32_t n = position->clock.duration;
	for (int i = 0; i < 2; i++) {
		float *in = pw_filter_get_dsp_buffer(d->in[i], n);
		float *out = pw_filter_get_dsp_buffer(d->out[i], n);
		if (out)
			memcpy(out, in ? in : out, in ? n * sizeof(float) : 0);
	}
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.process = on_process,
};

int main(int argc, char *argv[])
{
	struct data d = { 0 };
	pw_init(&argc, &argv);
	d.loop = pw_main_loop_new(NULL);
	d.context = pw_context_new(pw_main_loop_get_loop(d.loop), NULL, 0);
	d.core = pw_context_connect(d.context, NULL, 0);
	d.registry = pw_core_get_registry(d.core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(d.registry, &d.registry_listener, &registry_events, &d);
	d.filter = pw_filter_new(d.core, "linktest",
			pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Duplex",
				PW_KEY_MEDIA_ROLE, "DSP", NULL));
	static struct spa_hook filter_listener;
	pw_filter_add_listener(d.filter, &filter_listener, &filter_events, &d);
	const char *names[] = { "in_0", "in_1", "out_0", "out_1" };
	for (int i = 0; i < 4; i++) {
		void *p = pw_filter_add_port(d.filter, i < 2 ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
				PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
				pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
					PW_KEY_PORT_NAME, names[i], NULL), NULL, 0);
		if (i < 2)
			d.in[i] = p;
		else
			d.out[i - 2] = p;
	}
	if (pw_filter_connect(d.filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0) {
		fprintf(stderr, "can't connect the filter\n");
		return 1;
	}
	d.timer = pw_loop_add_timer(pw_main_loop_get_loop(d.loop), on_tick, &d);
	struct timespec iv = { 0, 100 * 1000 * 1000 };
	pw_loop_update_timer(pw_main_loop_get_loop(d.loop), d.timer, &iv, &iv, false);
	pw_main_loop_run(d.loop);

	for (int i = 0; i < 2; i++)
		if (d.links[i])
			pw_proxy_destroy(d.links[i]);
	if (d.node_link)
		pw_proxy_destroy(d.node_link);
	if (d.metadata)
		pw_proxy_destroy(d.metadata);
	pw_filter_destroy(d.filter);
	pw_proxy_destroy((struct pw_proxy *)d.registry);
	pw_core_disconnect(d.core);
	pw_context_destroy(d.context);
	pw_main_loop_destroy(d.loop);
	pw_deinit();
	return d.fails ? 1 : 0;
}
