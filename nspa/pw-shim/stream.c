/*
 * jackbus-pw: pw_stream on the application's JackBus node.
 *
 * An audio pw_stream is a group of JACK ports on the application's client
 * (app.c), one per channel, named by channel position (FL, FR, ...), or
 * "<node.name>/<position>" when the stream's node.name isn't the client's.
 *
 * Format: negotiated in pw_stream_connect() from the EnumFormat params the
 * application offers, the WineASIO way: the rate is JackBus's (a stream that
 * offers only other rates is refused, for now), the sample format is the
 * first of the application's that the shim converts (F32, F32P, S16, S16P,
 * S32, S32P, S24_32, F64), the channel count is the application's. The
 * conversion to and from JACK's float ports happens at the boundary, with
 * the stream's volume, mute and channel volumes (pw_stream_set_control).
 *
 * Connections: with PW_STREAM_FLAG_AUTOCONNECT (where PipeWire would link the
 * stream to the default device) the channels go to the physical playback or
 * capture ports in order, unless JACKBUS_PW_AUTOCONNECT=0 (a session manager,
 * PatchBox, routes instead). Without the flag, none are made.
 *
 * Process: in the JACK realtime thread, once per cycle, with one buffer of the
 * cycle's frames: a playback stream dequeues it, fills it and queues it; a
 * capture stream dequeues it filled. Without PW_STREAM_FLAG_RT_PROCESS the
 * process callback still runs there (logged); PipeWire would run it on the
 * main loop, a cycle late.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <sys/mman.h>

#include <spa/param/format-utils.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-types.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/utils/result.h>

#include "shim.h"

#define MAX_CHANNELS SPA_AUDIO_MAX_CHANNELS
#define MAX_FRAMES 8192u			/* JACK's BUFFER_SIZE_MAX */

enum notify_bits {
	NOTIFY_START = 1 << 0,
	NOTIFY_SHUTDOWN = 1 << 1,
	NOTIFY_DRAINED = 1 << 2,
};

struct pw_stream {
	struct shim_member member;	/* first: the application's client runs it */
	struct pw_core *core;
	struct pw_context *own_context;
	struct pw_core *own_core;
	char *name;
	struct pw_properties *props;
	struct spa_hook_list listeners;
	struct spa_hook simple_listener;
	enum pw_stream_state state;
	char *error;
	enum pw_stream_flags flags;
	enum spa_direction direction;
	uint32_t node_id;

	/* the negotiated format */
	struct spa_audio_info_raw info;
	bool planar;
	uint32_t sample_size, stride;	/* stride: bytes per frame in one data block */

	/* the stream's ports and its one buffer */
	jackbus_node_t *node;
	jack_client_t *client;
	jackbus_port_t *ports[MAX_CHANNELS];
	void *mem;
	size_t mem_size;
	struct pw_buffer pwbuf;
	struct spa_buffer sbuf;
	struct spa_data datas[MAX_CHANNELS];
	struct spa_chunk chunks[MAX_CHANNELS];
	struct spa_meta metas[1];

	struct spa_source *notify;
	atomic_uint pending;
	atomic_bool streaming;
	atomic_bool active;

	/* volume, mute, channel volumes: written by the application, read in the
	 * cycle (aligned floats: torn values aren't possible on x86-64) */
	volatile float volume;
	volatile bool mute;
	volatile float channel_volumes[MAX_CHANNELS];

	/* realtime thread only */
	bool in_process, available, queued;
	jack_nframes_t nframes;
	struct spa_io_position position;
};

static uint32_t next_node_id = 2000;

SHIM_EXPORT const char *pw_stream_state_as_string(enum pw_stream_state state)
{
	switch (state) {
	case PW_STREAM_STATE_ERROR: return "error";
	case PW_STREAM_STATE_UNCONNECTED: return "unconnected";
	case PW_STREAM_STATE_CONNECTING: return "connecting";
	case PW_STREAM_STATE_PAUSED: return "paused";
	case PW_STREAM_STATE_STREAMING: return "streaming";
	}
	return "invalid-state";
}

static void set_state(struct pw_stream *s, enum pw_stream_state state, int res, const char *error)
{
	enum pw_stream_state old = s->state;

	if (state == PW_STREAM_STATE_ERROR) {
		free(s->error);
		s->error = error ? strdup(error) : NULL;
	}
	if (old == state)
		return;
	s->state = state;
	shim_log("%s: %s -> %s%s%s", s->name, pw_stream_state_as_string(old),
			pw_stream_state_as_string(state), error ? ": " : "", error ? error : "");
	errno = -res;
	spa_hook_list_call(&s->listeners, struct pw_stream_events, state_changed, 0,
			old, state, error);
}

/* ------------------------------------------------------------ formats */

static bool format_known(uint32_t f, bool *planar, uint32_t *size)
{
	switch (f) {
	case SPA_AUDIO_FORMAT_F32: *planar = false; *size = 4; return true;
	case SPA_AUDIO_FORMAT_F32P: *planar = true; *size = 4; return true;
	case SPA_AUDIO_FORMAT_S16: *planar = false; *size = 2; return true;
	case SPA_AUDIO_FORMAT_S16P: *planar = true; *size = 2; return true;
	case SPA_AUDIO_FORMAT_S32: *planar = false; *size = 4; return true;
	case SPA_AUDIO_FORMAT_S32P: *planar = true; *size = 4; return true;
	case SPA_AUDIO_FORMAT_S24_32: *planar = false; *size = 4; return true;
	case SPA_AUDIO_FORMAT_F64: *planar = false; *size = 8; return true;
	default: return false;
	}
}

/* the base format, for the conversion loops */
static uint32_t base_format(uint32_t f)
{
	switch (f) {
	case SPA_AUDIO_FORMAT_F32P: return SPA_AUDIO_FORMAT_F32;
	case SPA_AUDIO_FORMAT_S16P: return SPA_AUDIO_FORMAT_S16;
	case SPA_AUDIO_FORMAT_S32P: return SPA_AUDIO_FORMAT_S32;
	default: return f;
	}
}

static void default_positions(uint32_t channels, uint32_t *pos)
{
	static const uint32_t layouts[][8] = {
		{ SPA_AUDIO_CHANNEL_MONO },
		{ SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR },
		{ SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_LFE },
		{ SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR },
		{ SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_RL,
		  SPA_AUDIO_CHANNEL_RR },
		{ SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE,
		  SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR },
		{ SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE,
		  SPA_AUDIO_CHANNEL_RC, SPA_AUDIO_CHANNEL_SL, SPA_AUDIO_CHANNEL_SR },
		{ SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE,
		  SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR, SPA_AUDIO_CHANNEL_SL, SPA_AUDIO_CHANNEL_SR },
	};
	for (uint32_t i = 0; i < channels; i++)
		pos[i] = (channels <= 8) ? layouts[channels - 1][i] : SPA_AUDIO_CHANNEL_AUX0 + i;
}

/* does the value pod (any choice) allow v? *def gets its default */
static bool int_allows(const struct spa_pod *value, int32_t v, int32_t *def)
{
	uint32_t n, choice;
	const struct spa_pod *p = spa_pod_get_values(value, &n, &choice);
	if (n == 0 || !spa_pod_is_int(p))
		return false;
	const int32_t *vals = SPA_POD_BODY_CONST(p);
	*def = vals[0];
	switch (choice) {
	case SPA_CHOICE_None:
		return vals[0] == v;
	case SPA_CHOICE_Range:
	case SPA_CHOICE_Step:
		return n >= 3 && v >= vals[1] && v <= vals[2];
	case SPA_CHOICE_Enum:
		for (uint32_t i = 0; i < n; i++)
			if (vals[i] == v)
				return true;
		return false;
	default:
		return false;
	}
}

/* one EnumFormat pod: what JackBus can take of it */
static bool try_format(struct pw_stream *s, const struct spa_pod *param, uint32_t rate, char *why, size_t len)
{
	uint32_t media_type, media_subtype;
	if (!spa_pod_is_object_type(param, SPA_TYPE_OBJECT_Format) ||
	    spa_format_parse(param, &media_type, &media_subtype) < 0)
		return false;
	if (media_type != SPA_MEDIA_TYPE_audio || media_subtype != SPA_MEDIA_SUBTYPE_raw) {
		snprintf(why, len, "only raw audio");
		return false;
	}
	const struct spa_pod_object *obj = (const struct spa_pod_object *)param;
	const struct spa_pod_prop *prop;
	struct spa_audio_info_raw info;
	spa_zero(info);

	/* the sample format: the first offered that is converted here */
	if ((prop = spa_pod_object_find_prop(obj, NULL, SPA_FORMAT_AUDIO_format)) == NULL)
		return false;
	uint32_t n, choice;
	const struct spa_pod *p = spa_pod_get_values(&prop->value, &n, &choice);
	if (!spa_pod_is_id(p))
		return false;
	const uint32_t *ids = SPA_POD_BODY_CONST(p);
	bool found = false;
	for (uint32_t i = 0; i < n && !found; i++) {
		bool planar;
		uint32_t size;
		if (format_known(ids[i], &planar, &size)) {
			info.format = ids[i];
			found = true;
		}
	}
	if (!found) {
		snprintf(why, len, "no sample format converted here (F32, S16, S32, S24_32, F64, planar or not)");
		return false;
	}

	/* the rate: JackBus's, or nothing */
	int32_t def = 0;
	if ((prop = spa_pod_object_find_prop(obj, NULL, SPA_FORMAT_AUDIO_rate)) != NULL &&
	    !int_allows(&prop->value, (int32_t)rate, &def)) {
		snprintf(why, len, "rate %d Hz asked, JackBus runs at %u Hz (no resampling yet)", def, rate);
		return false;
	}
	info.rate = rate;

	/* the channels: the application's */
	info.channels = 2;
	if ((prop = spa_pod_object_find_prop(obj, NULL, SPA_FORMAT_AUDIO_channels)) != NULL) {
		int_allows(&prop->value, 0, &def);
		if (def > 0)
			info.channels = ((uint32_t)def < MAX_CHANNELS) ? (uint32_t)def : MAX_CHANNELS;
	}
	default_positions(info.channels, info.position);
	if ((prop = spa_pod_object_find_prop(obj, NULL, SPA_FORMAT_AUDIO_position)) != NULL &&
	    spa_pod_is_array(&prop->value)) {
		uint32_t np = 0;
		const uint32_t *pos = spa_pod_get_array(&prop->value, &np);
		/* an unpositioned channel (UNKNOWN: a format built without positions)
		 * keeps the default layout's position */
		for (uint32_t i = 0; pos && i < np && i < info.channels; i++)
			if (pos[i] != SPA_AUDIO_CHANNEL_UNKNOWN)
				info.position[i] = pos[i];
	}
	s->info = info;
	return true;
}

/* ------------------------------------------------- the realtime side */

static inline float gain_of(struct pw_stream *s, uint32_t ch)
{
	return s->mute ? 0.0f : s->volume * s->channel_volumes[ch];
}

/* app samples -> JACK floats, frames of channel ch */
static void to_jack(struct pw_stream *s, uint32_t ch, float *dst, uint32_t frames)
{
	float g = gain_of(s, ch);
	const uint8_t *base = s->planar ? s->datas[ch].data : s->datas[0].data;
	uint32_t step = s->planar ? 1 : s->info.channels;
	uint32_t off = s->planar ? 0 : ch;

	switch (base_format(s->info.format)) {
	case SPA_AUDIO_FORMAT_F32: {
		const float *src = (const float *)base;
		for (uint32_t i = 0; i < frames; i++)
			dst[i] = src[i * step + off] * g;
		break;
	}
	case SPA_AUDIO_FORMAT_S16: {
		const int16_t *src = (const int16_t *)base;
		for (uint32_t i = 0; i < frames; i++)
			dst[i] = src[i * step + off] * (g / 32768.0f);
		break;
	}
	case SPA_AUDIO_FORMAT_S32: {
		const int32_t *src = (const int32_t *)base;
		for (uint32_t i = 0; i < frames; i++)
			dst[i] = (float)(src[i * step + off] * (g / 2147483648.0));
		break;
	}
	case SPA_AUDIO_FORMAT_S24_32: {
		const int32_t *src = (const int32_t *)base;
		for (uint32_t i = 0; i < frames; i++)
			dst[i] = ((int32_t)((uint32_t)src[i * step + off] << 8) >> 8) * (g / 8388608.0f);
		break;
	}
	case SPA_AUDIO_FORMAT_F64: {
		const double *src = (const double *)base;
		for (uint32_t i = 0; i < frames; i++)
			dst[i] = (float)(src[i * step + off] * g);
		break;
	}
	}
}

static inline float clampf(float v)
{
	return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
}

/* JACK floats -> app samples, frames of channel ch */
static void from_jack(struct pw_stream *s, uint32_t ch, const float *src, uint32_t frames)
{
	float g = gain_of(s, ch);
	uint8_t *base = s->planar ? s->datas[ch].data : s->datas[0].data;
	uint32_t step = s->planar ? 1 : s->info.channels;
	uint32_t off = s->planar ? 0 : ch;

	switch (base_format(s->info.format)) {
	case SPA_AUDIO_FORMAT_F32: {
		float *dst = (float *)base;
		for (uint32_t i = 0; i < frames; i++)
			dst[i * step + off] = src[i] * g;
		break;
	}
	case SPA_AUDIO_FORMAT_S16: {
		int16_t *dst = (int16_t *)base;
		for (uint32_t i = 0; i < frames; i++)
			dst[i * step + off] = (int16_t)lrintf(clampf(src[i] * g) * 32767.0f);
		break;
	}
	case SPA_AUDIO_FORMAT_S32: {
		int32_t *dst = (int32_t *)base;
		for (uint32_t i = 0; i < frames; i++)
			dst[i * step + off] = (int32_t)lrint(clampf(src[i] * g) * 2147483647.0);
		break;
	}
	case SPA_AUDIO_FORMAT_S24_32: {
		int32_t *dst = (int32_t *)base;
		for (uint32_t i = 0; i < frames; i++)
			dst[i * step + off] = (int32_t)lrintf(clampf(src[i] * g) * 8388607.0f);
		break;
	}
	case SPA_AUDIO_FORMAT_F64: {
		double *dst = (double *)base;
		for (uint32_t i = 0; i < frames; i++)
			dst[i * step + off] = src[i] * g;
		break;
	}
	}
}

static void set_chunks(struct pw_stream *s, uint32_t frames)
{
	uint32_t blocks = s->planar ? s->info.channels : 1;
	for (uint32_t b = 0; b < blocks; b++) {
		s->chunks[b].offset = 0;
		s->chunks[b].stride = s->stride;
		s->chunks[b].size = frames * s->stride;
		s->chunks[b].flags = 0;
	}
}

static void stream_process(struct shim_member *m, const jackbus_cycle_t *c)
{
	struct pw_stream *s = SPA_CONTAINER_OF(m, struct pw_stream, member);
	bool playback = s->direction == SPA_DIRECTION_OUTPUT;
	uint32_t frames = SPA_MIN(c->nframes, MAX_FRAMES);
	bool run = atomic_load_explicit(&s->streaming, memory_order_acquire) &&
		atomic_load_explicit(&s->active, memory_order_relaxed);

	s->nframes = frames;
	if (run) {
		shim_fill_position(&s->position, c, s->client);
		if (!playback) {
			for (uint32_t ch = 0; ch < s->info.channels; ch++)
				from_jack(s, ch, jackbus_port_audio(s->ports[ch]), frames);
			set_chunks(s, frames);
		} else {
			set_chunks(s, 0);
		}
		s->pwbuf.requested = frames;
		s->pwbuf.size = playback ? 0 : frames;
		s->available = true;
		s->queued = false;
		s->in_process = true;
		spa_hook_list_call_once(&s->listeners, struct pw_stream_events, process, 0);
		s->in_process = false;
		s->available = false;
	}
	if (playback) {
		uint32_t got = 0;
		if (run && s->queued && s->stride)
			got = SPA_MIN(s->chunks[0].size / s->stride, frames);
		for (uint32_t ch = 0; ch < s->info.channels; ch++) {
			float *out = jackbus_port_audio(s->ports[ch]);
			if (got)
				to_jack(s, ch, out, got);
			if (got < frames)
				memset(out + got, 0, (frames - got) * sizeof(float));
		}
	}
}

static void stream_shutdown(struct shim_member *m)
{
	struct pw_stream *s = SPA_CONTAINER_OF(m, struct pw_stream, member);
	atomic_store(&s->streaming, false);
	atomic_fetch_or(&s->pending, NOTIFY_SHUTDOWN);
	pw_loop_signal_event(s->core->context->main_loop, s->notify);
}

static jack_nframes_t stream_latency(struct shim_member *m)
{
	(void)m;
	return 0;
}

/* ------------------------------------------------ main-loop notifications */

static void autoconnect(struct pw_stream *s)
{
	const char *env = getenv("JACKBUS_PW_AUTOCONNECT");
	if (!(s->flags & PW_STREAM_FLAG_AUTOCONNECT) || (env && strcmp(env, "0") == 0))
		return;
	bool playback = s->direction == SPA_DIRECTION_OUTPUT;
	const char **phys = jack_get_ports(s->client, NULL, JACK_DEFAULT_AUDIO_TYPE,
			JackPortIsPhysical | (playback ? JackPortIsInput : JackPortIsOutput));
	for (uint32_t ch = 0; phys && phys[ch] && ch < s->info.channels; ch++) {
		const char *mine = jack_port_name(jackbus_port_jack(s->ports[ch]));
		if (playback)
			jack_connect(s->client, mine, phys[ch]);
		else
			jack_connect(s->client, phys[ch], mine);
	}
	jack_free(phys);
}

static void on_notify(void *data, uint64_t count)
{
	struct pw_stream *s = data;
	unsigned bits = atomic_exchange(&s->pending, 0);

	if (bits & NOTIFY_START && s->state == PW_STREAM_STATE_CONNECTING) {
		uint8_t buffer[1024];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		struct spa_pod *fmt = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &s->info);

		set_state(s, PW_STREAM_STATE_PAUSED, 0, NULL);
		spa_hook_list_call(&s->listeners, struct pw_stream_events, io_changed, 0,
				SPA_IO_Position, &s->position, sizeof(s->position));
		spa_hook_list_call(&s->listeners, struct pw_stream_events, param_changed, 0,
				SPA_PARAM_Format, fmt);
		spa_hook_list_call(&s->listeners, struct pw_stream_events, add_buffer, 0, &s->pwbuf);
		if (!(s->flags & PW_STREAM_FLAG_INACTIVE)) {
			atomic_store(&s->active, true);
			set_state(s, PW_STREAM_STATE_STREAMING, 0, NULL);
		}
		atomic_store_explicit(&s->streaming, true, memory_order_release);
		autoconnect(s);
	}
	if (bits & NOTIFY_DRAINED)
		spa_hook_list_call(&s->listeners, struct pw_stream_events, drained, 0);
	if (bits & NOTIFY_SHUTDOWN)
		set_state(s, PW_STREAM_STATE_ERROR, -EPIPE, "the JACK server went away");
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

SHIM_EXPORT struct pw_stream *pw_stream_new(struct pw_core *core, const char *name,
		struct pw_properties *props)
{
	struct pw_stream *s = calloc(1, sizeof(*s));

	if (s == NULL) {
		pw_properties_free(props);
		return NULL;
	}
	s->member.process = stream_process;
	s->member.shutdown = stream_shutdown;
	s->member.latency = stream_latency;
	s->core = core;
	s->name = strdup(name ? name : "pw-stream");
	s->props = props ? props : pw_properties_new(NULL, NULL);
	if (pw_properties_get(s->props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_set(s->props, PW_KEY_NODE_NAME, s->name);
	apply_env(s->props);
	spa_hook_list_init(&s->listeners);
	s->node_id = next_node_id++;
	s->volume = 1.0f;
	for (uint32_t i = 0; i < MAX_CHANNELS; i++)
		s->channel_volumes[i] = 1.0f;
	s->notify = pw_loop_add_event(core->context->main_loop, on_notify, s);
	return s;
}

SHIM_EXPORT struct pw_stream *pw_stream_new_simple(struct pw_loop *loop, const char *name,
		struct pw_properties *props, const struct pw_stream_events *events, void *data)
{
	struct pw_context *context;
	struct pw_core *core;
	struct pw_stream *s;

	if ((context = pw_context_new(loop, NULL, 0)) == NULL)
		goto error;
	if ((core = pw_context_connect(context, NULL, 0)) == NULL) {
		pw_context_destroy(context);
		goto error;
	}
	if ((s = pw_stream_new(core, name, props)) == NULL) {
		pw_core_disconnect(core);
		pw_context_destroy(context);
		return NULL;
	}
	s->own_context = context;
	s->own_core = core;
	pw_stream_add_listener(s, &s->simple_listener, events, data);
	return s;
error:
	pw_properties_free(props);
	return NULL;
}

SHIM_EXPORT void pw_stream_add_listener(struct pw_stream *s, struct spa_hook *listener,
		const struct pw_stream_events *events, void *data)
{
	spa_hook_list_append(&s->listeners, listener, events, data);
}

SHIM_EXPORT enum pw_stream_state pw_stream_get_state(struct pw_stream *s, const char **error)
{
	if (error)
		*error = s->error;
	return s->state;
}

SHIM_EXPORT const char *pw_stream_get_name(struct pw_stream *s) { return s->name; }
SHIM_EXPORT struct pw_core *pw_stream_get_core(struct pw_stream *s) { return s->core; }
SHIM_EXPORT const struct pw_properties *pw_stream_get_properties(struct pw_stream *s) { return s->props; }
SHIM_EXPORT uint32_t pw_stream_get_node_id(struct pw_stream *s) { return s->node_id; }

SHIM_EXPORT int pw_stream_update_properties(struct pw_stream *s, const struct spa_dict *dict)
{
	return pw_properties_update(s->props, dict);
}

static void free_buffer(struct pw_stream *s)
{
	if (s->mem) {
		munlock(s->mem, s->mem_size);
		free(s->mem);
	}
	s->mem = NULL;
	s->mem_size = 0;
}

/* the one buffer: MAX_FRAMES frames, locked (it is written in the realtime thread) */
static int alloc_buffer(struct pw_stream *s)
{
	uint32_t blocks = s->planar ? s->info.channels : 1;
	size_t block = (size_t)MAX_FRAMES * s->stride;
	s->mem_size = blocks * block;
	if (posix_memalign(&s->mem, 64, s->mem_size) != 0) {
		s->mem = NULL;
		return -ENOMEM;
	}
	memset(s->mem, 0, s->mem_size);
	mlock(s->mem, s->mem_size);
	spa_zero(s->sbuf);
	s->sbuf.n_datas = blocks;
	s->sbuf.datas = s->datas;
	s->sbuf.n_metas = 0;
	s->sbuf.metas = s->metas;
	for (uint32_t i = 0; i < blocks; i++) {
		spa_zero(s->datas[i]);
		s->datas[i].type = SPA_DATA_MemPtr;
		s->datas[i].flags = SPA_DATA_FLAG_READWRITE;
		s->datas[i].maxsize = block;
		s->datas[i].data = SPA_PTROFF(s->mem, i * block, void);
		s->datas[i].chunk = &s->chunks[i];
	}
	s->pwbuf.buffer = &s->sbuf;
	return 0;
}

static void log_forced_settings(struct pw_stream *s)
{
	char pos[256] = "";
	size_t len = 0;
	for (uint32_t i = 0; i < s->info.channels && len < sizeof(pos); i++)
		len += snprintf(pos + len, sizeof(pos) - len, "%s%s", i ? "," : "",
				spa_type_audio_channel_to_short_name(s->info.position[i]));
	const char *lat = pw_properties_get(s->props, PW_KEY_NODE_LATENCY);
	shim_log("%s: %s, %s %u Hz %u ch (%s); quantum %u frames%s%s%s", s->name,
			s->direction == SPA_DIRECTION_OUTPUT ? "playback" : "capture",
			spa_type_audio_format_to_short_name(s->info.format), s->info.rate, s->info.channels, pos,
			jack_get_buffer_size(s->client),
			lat ? " (asked node.latency=" : "", lat ? lat : "", lat ? ")" : "");
}

static const char *client_name_for(struct pw_stream *s)
{
	const char *n = pw_properties_get(s->props, PW_KEY_APP_NAME);
	if (n && n[0])
		return n;
	n = pw_properties_get(s->props, PW_KEY_NODE_NAME);
	if (n && n[0])
		return n;
	n = pw_get_prgname();
	return n ? n : s->name;
}

SHIM_EXPORT int pw_stream_connect(struct pw_stream *s, enum pw_direction direction,
		uint32_t target_id, enum pw_stream_flags flags,
		const struct spa_pod **params, uint32_t n_params)
{
	char why[256] = "no audio format offered";

	(void)target_id;
	if (s->node != NULL)
		return -EBUSY;
	s->flags = flags;
	s->direction = (enum spa_direction)direction;
	set_state(s, PW_STREAM_STATE_CONNECTING, 0, NULL);

	const char *media = pw_properties_get(s->props, PW_KEY_MEDIA_TYPE);
	if (media && strcmp(media, "Audio") != 0) {
		set_state(s, PW_STREAM_STATE_ERROR, -ENOTSUP, "JackBus carries audio streams only");
		return -ENOTSUP;
	}
	if ((s->node = shim_app_ref(client_name_for(s))) == NULL) {
		set_state(s, PW_STREAM_STATE_ERROR, -EIO, "can't connect to JackBus");
		return -EIO;
	}
	s->client = jackbus_node_client(s->node);
	uint32_t rate = jack_get_sample_rate(s->client);

	bool ok = false;
	for (uint32_t i = 0; i < n_params && !ok; i++)
		if (params[i])
			ok = try_format(s, params[i], rate, why, sizeof(why));
	if (!ok) {
		shim_warn("%s: %s", s->name, why);
		s->node = NULL;
		shim_app_unref();
		set_state(s, PW_STREAM_STATE_ERROR, -EINVAL, why);
		return -EINVAL;
	}
	format_known(s->info.format, &s->planar, &s->sample_size);
	s->stride = s->planar ? s->sample_size : s->sample_size * s->info.channels;
	if (alloc_buffer(s) < 0) {
		s->node = NULL;
		shim_app_unref();
		set_state(s, PW_STREAM_STATE_ERROR, -ENOMEM, "out of memory");
		return -ENOMEM;
	}

	/* the ports, one per channel */
	unsigned long jflags = (s->direction == SPA_DIRECTION_OUTPUT) ? JackPortIsOutput : JackPortIsInput;
	for (uint32_t ch = 0; ch < s->info.channels; ch++) {
		char name[300];
		shim_app_port_name(pw_properties_get(s->props, PW_KEY_NODE_NAME),
				spa_type_audio_channel_to_short_name(s->info.position[ch]), name, sizeof(name));
		s->ports[ch] = jackbus_port_add(s->node, name, JACKBUS_AUDIO, jflags);
		for (int k = 2; s->ports[ch] == NULL && k < 100; k++) {
			char unique[310];
			snprintf(unique, sizeof(unique), "%s-%d", name, k);
			s->ports[ch] = jackbus_port_add(s->node, unique, JACKBUS_AUDIO, jflags);
		}
		if (s->ports[ch] == NULL) {
			pw_stream_disconnect(s);
			set_state(s, PW_STREAM_STATE_ERROR, -EIO, "can't register a JACK port");
			return -EIO;
		}
	}

	/* valid before the first cycle */
	s->position.clock.rate = SPA_FRACTION(1, rate);
	s->position.clock.duration = jack_get_buffer_size(s->client);
	snprintf(s->position.clock.name, sizeof(s->position.clock.name), "jackbus");
	if (!(flags & PW_STREAM_FLAG_RT_PROCESS))
		shim_log("%s: no RT_PROCESS flag: process still runs in the JACK thread", s->name);
	log_forced_settings(s);

	shim_app_attach(&s->member);
	atomic_fetch_or(&s->pending, NOTIFY_START);
	pw_loop_signal_event(s->core->context->main_loop, s->notify);
	return 0;
}

SHIM_EXPORT int pw_stream_disconnect(struct pw_stream *s)
{
	if (s->node == NULL)
		return 0;
	atomic_store(&s->streaming, false);
	shim_app_detach(&s->member);		/* from here no cycle runs the stream */
	for (uint32_t ch = 0; ch < MAX_CHANNELS; ch++) {
		if (s->ports[ch]) {
			jackbus_port_remove(s->ports[ch]);
			s->ports[ch] = NULL;
		}
	}
	if (s->state == PW_STREAM_STATE_STREAMING || s->state == PW_STREAM_STATE_PAUSED)
		spa_hook_list_call(&s->listeners, struct pw_stream_events, remove_buffer, 0, &s->pwbuf);
	free_buffer(s);
	s->node = NULL;
	s->client = NULL;
	shim_app_unref();
	set_state(s, PW_STREAM_STATE_UNCONNECTED, 0, NULL);
	return 0;
}

SHIM_EXPORT void pw_stream_destroy(struct pw_stream *s)
{
	if (s == NULL)
		return;
	pw_stream_disconnect(s);
	spa_hook_list_call(&s->listeners, struct pw_stream_events, destroy, 0);
	pw_loop_destroy_source(s->core->context->main_loop, s->notify);
	spa_hook_list_clean(&s->listeners);
	if (s->own_core)
		pw_core_disconnect(s->own_core);
	if (s->own_context)
		pw_context_destroy(s->own_context);
	pw_properties_free(s->props);
	free(s->error);
	free(s->name);
	free(s);
}

SHIM_EXPORT int pw_stream_set_error(struct pw_stream *s, int res, const char *error, ...)
{
	char *msg = NULL;
	va_list ap;
	va_start(ap, error);
	if (vasprintf(&msg, error, ap) < 0)
		msg = NULL;
	va_end(ap);
	set_state(s, PW_STREAM_STATE_ERROR, res, msg);
	free(msg);
	return res;
}

SHIM_EXPORT int pw_stream_update_params(struct pw_stream *s, const struct spa_pod **params, uint32_t n_params)
{
	(void)s; (void)params; (void)n_params;
	return 0;	/* Buffers, Latency, ...: the shim's buffer is fixed */
}

SHIM_EXPORT int pw_stream_set_param(struct pw_stream *s, uint32_t id, const struct spa_pod *param)
{
	(void)s; (void)id; (void)param;
	return 0;
}

SHIM_EXPORT const struct pw_stream_control *pw_stream_get_control(struct pw_stream *s, uint32_t id)
{
	(void)s; (void)id;
	return NULL;
}

SHIM_EXPORT int pw_stream_set_control(struct pw_stream *s, uint32_t id, uint32_t n_values, float *values, ...)
{
	va_list ap;
	va_start(ap, values);
	for (;;) {
		switch (id) {
		case SPA_PROP_volume:
			if (n_values > 0)
				s->volume = values[0];
			break;
		case SPA_PROP_mute:
			if (n_values > 0)
				s->mute = values[0] >= 0.5f;
			break;
		case SPA_PROP_channelVolumes:
			for (uint32_t i = 0; i < n_values && i < MAX_CHANNELS; i++)
				s->channel_volumes[i] = values[i];
			break;
		default:
			break;
		}
		id = va_arg(ap, uint32_t);
		if (id == 0)
			break;
		n_values = va_arg(ap, uint32_t);
		values = va_arg(ap, float *);
	}
	va_end(ap);
	return 0;
}

SHIM_EXPORT int pw_stream_get_time_n(struct pw_stream *s, struct pw_time *time, size_t size)
{
	struct pw_time t;
	spa_zero(t);
	t.now = (int64_t)s->position.clock.nsec;
	t.rate = s->position.clock.rate;
	t.ticks = s->position.clock.position;
	if (s->ports[0]) {
		/* the delay to the device: the JACK latency of the stream's ports */
		jack_latency_range_t r;
		jack_port_get_latency_range(jackbus_port_jack(s->ports[0]),
				s->direction == SPA_DIRECTION_OUTPUT ? JackPlaybackLatency : JackCaptureLatency, &r);
		t.delay = r.max;
	}
	t.queued = 0;
	t.buffered = 0;
	t.queued_buffers = 0;
	t.avail_buffers = 1;
	t.size = s->nframes;
	memcpy(time, &t, SPA_MIN(size, sizeof(t)));
	return 0;
}

SHIM_EXPORT int pw_stream_get_time(struct pw_stream *s, struct pw_time *time)
{
	return pw_stream_get_time_n(s, time, sizeof(struct pw_time));
}

SHIM_EXPORT uint64_t pw_stream_get_nsec(struct pw_stream *s)
{
	struct timespec ts;
	(void)s;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return SPA_TIMESPEC_TO_NSEC(&ts);
}

SHIM_EXPORT struct pw_loop *pw_stream_get_data_loop(struct pw_stream *s)
{
	return s->core->context->main_loop;
}

SHIM_EXPORT struct pw_buffer *pw_stream_dequeue_buffer(struct pw_stream *s)
{
	if (!s->in_process || !s->available) {
		errno = EPIPE;
		return NULL;
	}
	s->available = false;
	return &s->pwbuf;
}

SHIM_EXPORT int pw_stream_queue_buffer(struct pw_stream *s, struct pw_buffer *buffer)
{
	if (buffer != &s->pwbuf)
		return -EINVAL;
	s->queued = true;
	return 0;
}

SHIM_EXPORT int pw_stream_return_buffer(struct pw_stream *s, struct pw_buffer *buffer)
{
	(void)s; (void)buffer;
	return 0;
}

SHIM_EXPORT int pw_stream_set_active(struct pw_stream *s, bool active)
{
	atomic_store(&s->active, active);
	if (s->state == PW_STREAM_STATE_PAUSED || s->state == PW_STREAM_STATE_STREAMING)
		set_state(s, active ? PW_STREAM_STATE_STREAMING : PW_STREAM_STATE_PAUSED, 0, NULL);
	return 0;
}

SHIM_EXPORT int pw_stream_flush(struct pw_stream *s, bool drain)
{
	/* nothing is held back: the last queued buffer played in its cycle */
	if (drain) {
		atomic_fetch_or(&s->pending, NOTIFY_DRAINED);
		pw_loop_signal_event(s->core->context->main_loop, s->notify);
	}
	return 0;
}

SHIM_EXPORT bool pw_stream_is_driving(struct pw_stream *s) { (void)s; return false; }
SHIM_EXPORT bool pw_stream_is_lazy(struct pw_stream *s) { (void)s; return false; }
SHIM_EXPORT int pw_stream_trigger_process(struct pw_stream *s) { (void)s; return -ENOTSUP; }
SHIM_EXPORT int pw_stream_emit_event(struct pw_stream *s, const struct spa_event *event)
{
	(void)s; (void)event;
	return 0;
}
SHIM_EXPORT int pw_stream_set_rate(struct pw_stream *s, double rate)
{
	(void)s;
	return rate == 1.0 ? 0 : -ENOTSUP;
}
