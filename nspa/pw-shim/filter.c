/*
 * jackbus-pw: pw_filter on the application's JackBus node.
 *
 * Every pw_filter of the process is a group of ports on one JACK client (the
 * application's node, app.c); each "32 bit float mono audio" DSP port is one
 * JACK audio port, each "8 bit raw midi" or "32 bit raw UMP" port one JACK
 * MIDI port (a control sequence per cycle, converted in place: midi.c), named
 * after the port, or "<node.name>/<port>" when the filter's node.name isn't
 * the client's. pw_filter_get_data_loop() is the JACK thread's (loop.c). The node runs each connected
 * filter's process callback in the JACK realtime thread, at the server's rate
 * and period: whatever the application asked for (node.latency, node.rate,
 * PIPEWIRE_QUANTUM, ...) is logged and not obeyed, as WineASIO does with a
 * Windows app's requested buffer size. The application learns the real
 * figures the PipeWire way, from spa_io_position, filled from the node's
 * cycle record: the 64-bit frame position and the cycle's times as they are.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include <spa/control/control.h>
#include <sys/mman.h>
#include <jack/midiport.h>

#include "shim.h"

#define MIDI_SIZE 32768u		/* a cycle's MIDI as a control sequence */
#define PORT_DATA_OFFSET SPA_ROUND_UP_N(sizeof(struct filter_port), 16)

enum notify_bits {
	NOTIFY_START = 1 << 0,
	NOTIFY_SHUTDOWN = 1 << 1,
};

struct filter_port {
	struct spa_list link;
	struct pw_filter *filter;
	enum pw_direction direction;
	uint32_t flags;
	struct pw_properties *props;
	jackbus_port_t *bport;
	bool is_audio;
	bool is_midi, ump;		/* MIDI: as MIDI 1.0 bytes, or UMP */
	uint32_t number;		/* input_N / output_N when it has no name */
	uint8_t *midi;			/* MIDI: the sequence buffer, locked */

	/* per cycle, realtime thread only */
	float *buf;
	void *jmidi;			/* MIDI: the JACK buffer */
	bool used;

	/* the DSP buffer seen as a pw_buffer, for dequeue/queue users */
	struct pw_buffer pwbuf;
	struct spa_buffer sbuf;
	struct spa_data sdata;
	struct spa_chunk chunk;
};

/* the ports a cycle sees: published like the node's port table */
struct port_array {
	size_t count;
	struct filter_port *ports[];
};

struct pw_filter {
	struct shim_member member;	/* first: the application's client runs it */
	struct pw_core *core;
	struct pw_context *own_context;
	struct pw_core *own_core;
	char *name;
	struct pw_properties *props;
	struct spa_hook_list listeners;
	struct spa_hook simple_listener;
	enum pw_filter_state state;
	char *error;
	enum pw_filter_flags flags;
	uint32_t node_id;

	pthread_mutex_t ports_lock;	/* non-realtime threads */
	struct spa_list ports;
	uint32_t n_in, n_out;
	_Atomic(struct port_array *) rt_ports;

	jackbus_node_t *node;		/* the application's, while connected */
	jack_client_t *client;
	jack_nframes_t rate;
	struct spa_source *notify;
	atomic_uint pending;
	atomic_bool streaming;		/* the app has seen STREAMING */
	atomic_bool active;

	/* realtime thread only */
	bool in_process;
	jack_nframes_t nframes;
	struct spa_io_position position;

	struct spa_process_latency_info proc_latency;
};

static uint32_t next_node_id = 1000;

SHIM_EXPORT const char *pw_filter_state_as_string(enum pw_filter_state state)
{
	switch (state) {
	case PW_FILTER_STATE_ERROR: return "error";
	case PW_FILTER_STATE_UNCONNECTED: return "unconnected";
	case PW_FILTER_STATE_CONNECTING: return "connecting";
	case PW_FILTER_STATE_PAUSED: return "paused";
	case PW_FILTER_STATE_STREAMING: return "streaming";
	}
	return "invalid-state";
}

static void set_state(struct pw_filter *f, enum pw_filter_state state, int res, const char *error)
{
	enum pw_filter_state old = f->state;

	if (state == PW_FILTER_STATE_ERROR) {
		free(f->error);
		f->error = error ? strdup(error) : NULL;
	}
	if (old == state)
		return;
	f->state = state;
	shim_log("%s: %s -> %s%s%s", f->name, pw_filter_state_as_string(old),
			pw_filter_state_as_string(state), error ? ": " : "", error ? error : "");
	errno = -res;
	spa_hook_list_call(&f->listeners, struct pw_filter_events, state_changed, 0,
			old, state, error);
}

static inline void *port_user_data(struct filter_port *p)
{
	return SPA_PTROFF(p, PORT_DATA_OFFSET, void);
}

static inline struct filter_port *port_from_user_data(void *data)
{
	return SPA_PTROFF(data, -(ptrdiff_t)PORT_DATA_OFFSET, struct filter_port);
}

/* under ports_lock: publish the registered ports to the realtime thread */
static void publish_ports(struct pw_filter *f)
{
	size_t n = 0;
	struct filter_port *p;
	spa_list_for_each(p, &f->ports, link)
		if (p->bport)
			n++;
	struct port_array *a = calloc(1, sizeof(*a) + n * sizeof(a->ports[0]));
	if (a == NULL)
		return;
	spa_list_for_each(p, &f->ports, link)
		if (p->bport)
			a->ports[a->count++] = p;
	shim_app_retire(atomic_exchange(&f->rt_ports, a));
}

/* ------------------------------------------------ the realtime side */

/* called by the application's node, in the JACK realtime thread */
static void filter_process(struct shim_member *m, const jackbus_cycle_t *c)
{
	struct pw_filter *f = SPA_CONTAINER_OF(m, struct pw_filter, member);
	struct port_array *a = atomic_load_explicit(&f->rt_ports, memory_order_acquire);

	f->nframes = c->nframes;
	for (size_t i = 0; a && i < a->count; i++) {
		struct filter_port *p = a->ports[i];
		p->used = false;
		if (!p->is_midi) {
			p->buf = jackbus_port_audio(p->bport);
			continue;
		}
		p->jmidi = jackbus_port_midi(p->bport);
		p->sdata.data = p->midi;
		p->sdata.maxsize = MIDI_SIZE;
		p->chunk.offset = 0;
		p->chunk.stride = 1;
		p->chunk.flags = 0;
		if (p->direction == PW_DIRECTION_INPUT) {
			p->chunk.size = shim_midi_from_jack(p->jmidi, p->midi, MIDI_SIZE, p->ump);
		} else {
			jack_midi_clear_buffer(p->jmidi);
			p->chunk.size = 0;
		}
	}

	if (atomic_load_explicit(&f->streaming, memory_order_acquire) &&
	    atomic_load_explicit(&f->active, memory_order_relaxed)) {
		shim_fill_position(&f->position, c, f->client);
		f->in_process = true;
		/* as in PipeWire, only the first listener with process gets it */
		spa_hook_list_call_once(&f->listeners, struct pw_filter_events, process, 0,
				&f->position);
		f->in_process = false;
	}

	/* an output the app didn't ask for this cycle is silent, as in PipeWire
	 * (a JACK output buffer still holds the previous cycle) */
	for (size_t i = 0; a && i < a->count; i++) {
		struct filter_port *p = a->ports[i];
		if (p->is_midi) {
			if (p->direction == PW_DIRECTION_OUTPUT && p->used)
				shim_midi_to_jack(p->midi, MIDI_SIZE, p->chunk.offset, p->chunk.size,
						p->jmidi, c->nframes);
			p->jmidi = NULL;
			continue;
		}
		if (p->direction == PW_DIRECTION_OUTPUT && p->buf && !p->used)
			memset(p->buf, 0, c->nframes * sizeof(float));
		p->buf = NULL;
	}
}

/* the event thread of the application's node: the server went away */
static void filter_shutdown(struct shim_member *m)
{
	struct pw_filter *f = SPA_CONTAINER_OF(m, struct pw_filter, member);
	atomic_store(&f->streaming, false);
	atomic_fetch_or(&f->pending, NOTIFY_SHUTDOWN);
	pw_loop_signal_event(f->core->context->main_loop, f->notify);
}

static jack_nframes_t filter_latency(struct shim_member *m)
{
	struct pw_filter *f = SPA_CONTAINER_OF(m, struct pw_filter, member);
	struct spa_process_latency_info *l = &f->proc_latency;
	jack_nframes_t q = f->client ? jack_get_buffer_size(f->client) : 0;
	return (jack_nframes_t)(l->quantum * q) + l->rate +
		(jack_nframes_t)(l->ns * (int64_t)f->rate / SPA_NSEC_PER_SEC);
}

/* ------------------------------------------------ main-loop notifications */

static void emit_format(struct pw_filter *f, struct filter_port *p)
{
	uint8_t buffer[512];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_pod *fmt;
	if (p->is_midi)
		fmt = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
				SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
				SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_control),
				SPA_FORMAT_CONTROL_types,
				SPA_POD_Int(1u << (p->ump ? SPA_CONTROL_UMP : SPA_CONTROL_Midi)));
	else
		fmt = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
				SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio),
				SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_dsp),
				SPA_FORMAT_AUDIO_format, SPA_POD_Id(SPA_AUDIO_FORMAT_DSP_F32));
	spa_hook_list_call(&f->listeners, struct pw_filter_events, param_changed, 0,
			port_user_data(p), SPA_PARAM_Format, fmt);
}

static void on_notify(void *data, uint64_t count)
{
	struct pw_filter *f = data;
	unsigned bits = atomic_exchange(&f->pending, 0);
	struct filter_port *p;

	/* PipeWire's order: PAUSED once bound, the clock and position areas,
	 * each port's Format, then STREAMING on the Start command */
	if (bits & NOTIFY_START && f->state == PW_FILTER_STATE_CONNECTING) {
		set_state(f, PW_FILTER_STATE_PAUSED, 0, NULL);
		spa_hook_list_call(&f->listeners, struct pw_filter_events, io_changed, 0,
				NULL, SPA_IO_Clock, &f->position.clock, sizeof(f->position.clock));
		spa_hook_list_call(&f->listeners, struct pw_filter_events, io_changed, 0,
				NULL, SPA_IO_Position, &f->position, sizeof(f->position));
		spa_list_for_each(p, &f->ports, link)
			emit_format(f, p);
		if (!(f->flags & PW_FILTER_FLAG_INACTIVE)) {
			atomic_store(&f->active, true);
			set_state(f, PW_FILTER_STATE_STREAMING, 0, NULL);
		}
		atomic_store_explicit(&f->streaming, true, memory_order_release);
	}
	if (bits & NOTIFY_SHUTDOWN)
		set_state(f, PW_FILTER_STATE_ERROR, -EPIPE, "the JACK server went away");
}

/* ------------------------------------------------------------------ API */

static void apply_env(struct pw_properties *props)
{
	const char *str;
	struct spa_fraction q;

	if ((str = getenv("PIPEWIRE_PROPS")) != NULL)
		pw_properties_update_string(props, str, strlen(str));
	if ((str = getenv("PIPEWIRE_QUANTUM")) != NULL &&
	    sscanf(str, "%u/%u", &q.num, &q.denom) == 2 && q.denom != 0) {
		pw_properties_setf(props, PW_KEY_NODE_FORCE_RATE, "%u", q.denom);
		pw_properties_setf(props, PW_KEY_NODE_FORCE_QUANTUM, "%u", q.num);
	}
	if ((str = getenv("PIPEWIRE_LATENCY")) != NULL)
		pw_properties_set(props, PW_KEY_NODE_LATENCY, str);
	if ((str = getenv("PIPEWIRE_RATE")) != NULL)
		pw_properties_set(props, PW_KEY_NODE_RATE, str);
}

SHIM_EXPORT struct pw_filter *pw_filter_new(struct pw_core *core, const char *name,
		struct pw_properties *props)
{
	struct pw_filter *f = calloc(1, sizeof(*f));

	if (f == NULL) {
		pw_properties_free(props);
		return NULL;
	}
	f->member.process = filter_process;
	f->member.shutdown = filter_shutdown;
	f->member.latency = filter_latency;
	f->core = core;
	f->name = strdup(name ? name : "pw-filter");
	f->props = props ? props : pw_properties_new(NULL, NULL);
	if (pw_properties_get(f->props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_set(f->props, PW_KEY_NODE_NAME, f->name);
	apply_env(f->props);
	spa_hook_list_init(&f->listeners);
	spa_list_init(&f->ports);
	pthread_mutex_init(&f->ports_lock, NULL);
	f->node_id = next_node_id++;
	f->notify = pw_loop_add_event(core->context->main_loop, on_notify, f);
	return f;
}

SHIM_EXPORT struct pw_filter *pw_filter_new_simple(struct pw_loop *loop, const char *name,
		struct pw_properties *props, const struct pw_filter_events *events, void *data)
{
	struct pw_context *context;
	struct pw_core *core;
	struct pw_filter *f;

	if ((context = pw_context_new(loop, NULL, 0)) == NULL)
		goto error;
	if ((core = pw_context_connect(context, NULL, 0)) == NULL) {
		pw_context_destroy(context);
		goto error;
	}
	if ((f = pw_filter_new(core, name, props)) == NULL) {
		pw_core_disconnect(core);
		pw_context_destroy(context);
		return NULL;
	}
	f->own_context = context;
	f->own_core = core;
	pw_filter_add_listener(f, &f->simple_listener, events, data);
	return f;
error:
	pw_properties_free(props);
	return NULL;
}

SHIM_EXPORT void pw_filter_add_listener(struct pw_filter *f, struct spa_hook *listener,
		const struct pw_filter_events *events, void *data)
{
	spa_hook_list_append(&f->listeners, listener, events, data);
}

SHIM_EXPORT enum pw_filter_state pw_filter_get_state(struct pw_filter *f, const char **error)
{
	if (error)
		*error = f->error;
	return f->state;
}

SHIM_EXPORT const char *pw_filter_get_name(struct pw_filter *f) { return f->name; }
SHIM_EXPORT struct pw_core *pw_filter_get_core(struct pw_filter *f) { return f->core; }
SHIM_EXPORT uint32_t pw_filter_get_node_id(struct pw_filter *f) { return f->node_id; }

/* under ports_lock, connected */
static int register_port(struct pw_filter *f, struct filter_port *p)
{
	const char *pname = pw_properties_get(p->props, PW_KEY_PORT_NAME);
	bool out = p->direction == PW_DIRECTION_OUTPUT;
	char base[256], name[300];

	if (pname == NULL) {
		snprintf(base, sizeof(base), "%s_%u", out ? "output" : "input", p->number);
		pname = base;
	}
	/* the client is the application: a filter named otherwise prefixes its ports */
	shim_app_port_name(pw_properties_get(f->props, PW_KEY_NODE_NAME), pname, name, sizeof(name));

	unsigned long flags = out ? JackPortIsOutput : JackPortIsInput;
	int kind = p->is_midi ? JACKBUS_MIDI : JACKBUS_AUDIO;
	p->bport = jackbus_port_add(f->node, name, kind, flags);
	for (int i = 2; p->bport == NULL && i < 100; i++) {
		char unique[310];
		snprintf(unique, sizeof(unique), "%s-%d", name, i);
		p->bport = jackbus_port_add(f->node, unique, kind, flags);
	}
	if (p->bport == NULL)
		return -EIO;
	shim_log("%s: port %s", f->name, jack_port_name(jackbus_port_jack(p->bport)));
	return 0;
}

SHIM_EXPORT void *pw_filter_add_port(struct pw_filter *f, enum pw_direction direction,
		enum pw_filter_port_flags flags, size_t port_data_size,
		struct pw_properties *props, const struct spa_pod **params, uint32_t n_params)
{
	struct filter_port *p;
	const char *dsp;

	(void)params; (void)n_params;
	if (props == NULL)
		props = pw_properties_new(NULL, NULL);
	dsp = pw_properties_get(props, PW_KEY_FORMAT_DSP);
	bool audio = dsp && spa_streq(dsp, "32 bit float mono audio");
	bool midi = dsp && spa_streq(dsp, "8 bit raw midi");
	bool ump = dsp && spa_streq(dsp, "32 bit raw UMP");
	if (!audio && !midi && !ump) {
		/* video DSP ports: never; other control formats: not on JACK */
		shim_warn("%s: port format \"%s\" is not supported", f->name, dsp ? dsp : "(none)");
		pw_properties_free(props);
		errno = ENOTSUP;
		return NULL;
	}
	if ((p = calloc(1, PORT_DATA_OFFSET + port_data_size)) == NULL) {
		pw_properties_free(props);
		return NULL;
	}
	if (!audio) {
		void *mem = NULL;
		if (posix_memalign(&mem, 64, MIDI_SIZE) != 0) {
			pw_properties_free(props);
			free(p);
			return NULL;
		}
		memset(mem, 0, MIDI_SIZE);
		mlock(mem, MIDI_SIZE);
		p->midi = mem;
	}
	p->filter = f;
	p->direction = direction;
	p->flags = flags;
	p->props = props;
	p->is_audio = audio;
	p->is_midi = !audio;
	p->ump = ump;
	p->sbuf.n_datas = 1;
	p->sbuf.datas = &p->sdata;
	p->sdata.type = SPA_DATA_MemPtr;
	p->sdata.chunk = &p->chunk;
	p->pwbuf.buffer = &p->sbuf;

	pthread_mutex_lock(&f->ports_lock);
	p->number = (direction == PW_DIRECTION_OUTPUT) ? f->n_out++ : f->n_in++;
	if (f->node && register_port(f, p) < 0) {
		pthread_mutex_unlock(&f->ports_lock);
		pw_properties_free(props);
		free(p->midi);
		free(p);
		errno = EIO;
		return NULL;
	}
	spa_list_append(&f->ports, &p->link);
	if (f->node)
		publish_ports(f);
	pthread_mutex_unlock(&f->ports_lock);
	return port_user_data(p);
}

SHIM_EXPORT int pw_filter_remove_port(void *port_data)
{
	struct filter_port *p = port_from_user_data(port_data);
	struct pw_filter *f = p->filter;

	pthread_mutex_lock(&f->ports_lock);
	spa_list_remove(&p->link);
	if (f->node)
		publish_ports(f);	/* from here no cycle sees the port */
	pthread_mutex_unlock(&f->ports_lock);
	if (p->bport)
		jackbus_port_remove(p->bport);
	pw_properties_free(p->props);
	free(p->midi);
	free(p);
	return 0;
}

static void parse_node_params(struct pw_filter *f, const struct spa_pod **params, uint32_t n_params)
{
	for (uint32_t i = 0; i < n_params; i++) {
		struct spa_process_latency_info info;
		if (params[i] == NULL || !spa_pod_is_object_id(params[i], SPA_PARAM_ProcessLatency))
			continue;
		if (spa_process_latency_parse(params[i], &info) >= 0) {
			f->proc_latency = info;
			shim_log("%s: process latency %.2f quanta + %d samples + %" PRIi64 " ns",
					f->name, info.quantum, info.rate, info.ns);
		}
	}
}

static void log_forced_settings(struct pw_filter *f)
{
	static const char * const keys[] = {
		PW_KEY_NODE_LATENCY, PW_KEY_NODE_RATE, PW_KEY_NODE_FORCE_QUANTUM,
		PW_KEY_NODE_FORCE_RATE, PW_KEY_NODE_LOCK_QUANTUM, PW_KEY_NODE_LOCK_RATE,
		PW_KEY_NODE_MAX_LATENCY, NULL,
	};
	char asked[512] = "";
	size_t len = 0;

	for (int i = 0; keys[i]; i++) {
		const char *v = pw_properties_get(f->props, keys[i]);
		if (v && len < sizeof(asked))
			len += snprintf(asked + len, sizeof(asked) - len, " %s=%s", keys[i], v);
	}
	shim_log("%s: asked for:%s", f->name, len ? asked : " nothing");
	shim_log("%s: forced to JackBus: rate %u Hz, quantum %u frames, f32 planar%s",
			f->name, f->rate, jack_get_buffer_size(f->client),
			f->flags & PW_FILTER_FLAG_RT_PROCESS ? "" : " (process still runs in the JACK thread)");
}

/* the application's client: JACKBUS_PW_CLIENT_NAME, else the first filter's node.name */
static const char *client_name_for(struct pw_filter *f)
{
	const char *n = pw_properties_get(f->props, PW_KEY_NODE_NAME);
	if (n && n[0])
		return n;
	n = pw_get_prgname();
	return n ? n : f->name;
}

SHIM_EXPORT int pw_filter_connect(struct pw_filter *f, enum pw_filter_flags flags,
		const struct spa_pod **params, uint32_t n_params)
{
	struct filter_port *p;

	if (f->node != NULL)
		return -EBUSY;
	f->flags = flags;
	parse_node_params(f, params, n_params);
	set_state(f, PW_FILTER_STATE_CONNECTING, 0, NULL);

	if ((f->node = shim_app_ref(client_name_for(f))) == NULL) {
		set_state(f, PW_FILTER_STATE_ERROR, -EIO, "can't connect to JackBus");
		return -EIO;
	}
	f->client = jackbus_node_client(f->node);
	f->rate = jack_get_sample_rate(f->client);
	/* valid before the first cycle, for apps that read it at io_changed */
	f->position.clock.rate = SPA_FRACTION(1, f->rate);
	f->position.clock.duration = jack_get_buffer_size(f->client);
	f->position.clock.target_rate = f->position.clock.rate;
	f->position.clock.target_duration = f->position.clock.duration;
	snprintf(f->position.clock.name, sizeof(f->position.clock.name), "jackbus");
	if ((flags & PW_FILTER_FLAG_DRIVER) || (flags & PW_FILTER_FLAG_TRIGGER))
		shim_log("%s: driver/trigger flags ignored: JackBus drives the graph", f->name);
	if (!(flags & PW_FILTER_FLAG_RT_PROCESS))
		shim_log("%s: no RT_PROCESS flag: process still runs in the JACK thread", f->name);

	pthread_mutex_lock(&f->ports_lock);
	spa_list_for_each(p, &f->ports, link) {
		if (register_port(f, p) < 0) {
			pthread_mutex_unlock(&f->ports_lock);
			pw_filter_disconnect(f);
			set_state(f, PW_FILTER_STATE_ERROR, -EIO, "can't register a JACK port");
			return -EIO;
		}
	}
	publish_ports(f);
	pthread_mutex_unlock(&f->ports_lock);

	log_forced_settings(f);
	shim_app_attach(&f->member);
	shim_app_update_latency();
	atomic_fetch_or(&f->pending, NOTIFY_START);
	pw_loop_signal_event(f->core->context->main_loop, f->notify);
	return 0;
}

SHIM_EXPORT int pw_filter_disconnect(struct pw_filter *f)
{
	struct filter_port *p;

	if (f->node == NULL)
		return 0;
	atomic_store(&f->streaming, false);
	shim_app_detach(&f->member);		/* from here no cycle runs the filter */
	pthread_mutex_lock(&f->ports_lock);
	shim_app_retire(atomic_exchange(&f->rt_ports, NULL));
	spa_list_for_each(p, &f->ports, link) {
		if (p->bport) {
			jackbus_port_remove(p->bport);
			p->bport = NULL;
		}
	}
	pthread_mutex_unlock(&f->ports_lock);
	f->node = NULL;
	f->client = NULL;
	shim_app_update_latency();
	shim_app_unref();
	set_state(f, PW_FILTER_STATE_UNCONNECTED, 0, NULL);
	return 0;
}

SHIM_EXPORT void pw_filter_destroy(struct pw_filter *f)
{
	struct filter_port *p, *t;

	if (f == NULL)
		return;
	pw_filter_disconnect(f);
	spa_hook_list_call(&f->listeners, struct pw_filter_events, destroy, 0);
	spa_list_for_each_safe(p, t, &f->ports, link) {
		spa_list_remove(&p->link);
		pw_properties_free(p->props);
		free(p->midi);
		free(p);
	}
	pw_loop_destroy_source(f->core->context->main_loop, f->notify);
	spa_hook_list_clean(&f->listeners);
	if (f->own_core)
		pw_core_disconnect(f->own_core);
	if (f->own_context)
		pw_context_destroy(f->own_context);
	pthread_mutex_destroy(&f->ports_lock);
	pw_properties_free(f->props);
	free(f->error);
	free(f->name);
	free(f);
}

SHIM_EXPORT const struct pw_properties *pw_filter_get_properties(struct pw_filter *f, void *port_data)
{
	return port_data ? port_from_user_data(port_data)->props : f->props;
}

SHIM_EXPORT int pw_filter_update_properties(struct pw_filter *f, void *port_data,
		const struct spa_dict *dict)
{
	struct pw_properties *props = port_data ? port_from_user_data(port_data)->props : f->props;
	return pw_properties_update(props, dict);
}

SHIM_EXPORT int pw_filter_set_error(struct pw_filter *f, int res, const char *error, ...)
{
	char *msg = NULL;
	va_list ap;
	va_start(ap, error);
	if (vasprintf(&msg, error, ap) < 0)
		msg = NULL;
	va_end(ap);
	set_state(f, PW_FILTER_STATE_ERROR, res, msg);
	free(msg);
	return res;
}

SHIM_EXPORT int pw_filter_update_params(struct pw_filter *f, void *port_data,
		const struct spa_pod **params, uint32_t n_params)
{
	if (port_data == NULL) {
		parse_node_params(f, params, n_params);
		if (f->node)
			shim_app_update_latency();
	}
	return 0;
}

SHIM_EXPORT int pw_filter_set_active(struct pw_filter *f, bool active)
{
	atomic_store(&f->active, active);
	if (f->state == PW_FILTER_STATE_PAUSED || f->state == PW_FILTER_STATE_STREAMING)
		set_state(f, active ? PW_FILTER_STATE_STREAMING : PW_FILTER_STATE_PAUSED, 0, NULL);
	return 0;
}

SHIM_EXPORT int pw_filter_get_time(struct pw_filter *f, struct pw_time *time)
{
	memset(time, 0, sizeof(*time));
	time->now = f->position.clock.nsec;
	time->rate = f->position.clock.rate;
	time->ticks = f->position.clock.position;
	return 0;
}

SHIM_EXPORT uint64_t pw_filter_get_nsec(struct pw_filter *f)
{
	struct timespec ts;
	(void)f;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return SPA_TIMESPEC_TO_NSEC(&ts);
}

SHIM_EXPORT struct pw_loop *pw_filter_get_data_loop(struct pw_filter *f)
{
	return shim_data_loop_get(f->core->context);
}

SHIM_EXPORT struct pw_buffer *pw_filter_dequeue_buffer(void *port_data)
{
	struct filter_port *p = port_from_user_data(port_data);
	struct pw_filter *f = p->filter;

	if (p->is_midi) {
		if (!f->in_process || p->jmidi == NULL)
			return NULL;
		p->used = true;
		p->pwbuf.size = p->chunk.size;
		return &p->pwbuf;
	}
	if (!f->in_process || p->buf == NULL)
		return NULL;
	p->used = true;
	p->sdata.data = p->buf;
	p->sdata.maxsize = f->nframes * sizeof(float);
	p->chunk.offset = 0;
	p->chunk.size = f->nframes * sizeof(float);
	p->chunk.stride = sizeof(float);
	p->pwbuf.size = f->nframes;
	return &p->pwbuf;
}

SHIM_EXPORT int pw_filter_queue_buffer(void *port_data, struct pw_buffer *buffer)
{
	(void)port_data; (void)buffer;
	return 0;
}

SHIM_EXPORT void *pw_filter_get_dsp_buffer(void *port_data, uint32_t n_samples)
{
	struct filter_port *p = port_from_user_data(port_data);
	struct pw_filter *f = p->filter;

	if (p->is_midi) {
		/* as PipeWire: the sequence buffer; an output's chunk says n_samples
		 * floats, the sequence's own size bounds what is read */
		if (!f->in_process || p->jmidi == NULL)
			return NULL;
		p->used = true;
		if (p->direction == PW_DIRECTION_OUTPUT) {
			p->chunk.offset = 0;
			p->chunk.size = n_samples * sizeof(float);
			p->chunk.stride = sizeof(float);
		}
		return p->midi;
	}
	if (!f->in_process || p->buf == NULL || n_samples > f->nframes)
		return NULL;
	p->used = true;
	return p->buf;
}

SHIM_EXPORT int pw_filter_flush(struct pw_filter *f, bool drain)
{
	if (drain)
		spa_hook_list_call(&f->listeners, struct pw_filter_events, drained, 0);
	return 0;
}

SHIM_EXPORT bool pw_filter_is_driving(struct pw_filter *f) { (void)f; return false; }
SHIM_EXPORT bool pw_filter_is_lazy(struct pw_filter *f) { (void)f; return false; }
SHIM_EXPORT int pw_filter_trigger_process(struct pw_filter *f) { (void)f; return -ENOTSUP; }
SHIM_EXPORT int pw_filter_emit_event(struct pw_filter *f, const struct spa_event *event)
{
	(void)f; (void)event;
	return 0;
}
