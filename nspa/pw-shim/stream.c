/*
 * jackbus-pw: pw_stream on the application's JackBus node.
 *
 * An audio pw_stream is a group of JACK ports on the application's client
 * (app.c), one per channel, named by channel position (FL, FR, ...), or
 * "<node.name>/<position>" when the stream's node.name isn't the client's.
 * A MIDI stream (application/control) is one JACK MIDI port, midi_out or
 * midi_in, carrying MIDI 1.0 bytes or UMP as the application asked
 * (format.dsp "8 bit raw midi", the default, or "32 bit raw UMP").
 *
 * Format: negotiated in pw_stream_connect() from the EnumFormat params the
 * application offers, the WineASIO way: the sample format is the first of the
 * application's that the shim converts (F32, F32P, S16, S16P, S32, S32P,
 * S24_32, F64), the channel count is the application's, and the rate is
 * JackBus's whenever any format the application offers allows it (the
 * direct route: no conversion of rate, no added latency). An application
 * that offers only another rate is resampled by JackBus's bridge resampler, a
 * fallback, at a fixed ratio (it runs on JackBus's clock, so nothing drifts),
 * unless the settings say no (~/.config/jackbus/compat.conf "resample = no",
 * JACKBUS_PW_RESAMPLE=0, jackbus-pw -R): then it is refused with the reason.
 * Conversion
 * happens at the boundary, with the stream's volume, mute and channel volumes
 * (pw_stream_set_control).
 *
 * Process, as PipeWire runs it:
 *  - with PW_STREAM_FLAG_RT_PROCESS, in the JACK realtime thread, once per
 *    cycle (more often only when resampling needs more input than one buffer
 *    holds);
 *  - without it (PipeWire's async node), on the application's loop: a
 *    playback stream fills a ring at JackBus's rate up to two quanta ahead
 *    (or 3 timer periods' worth), a capture stream is handed what the cycles
 *    put in its ring; a timer on that loop, at half a period, drives it. The
 *    realtime side only copies between the ports and the ring: no lock, no
 *    system call, no wakeup. A late application is an underrun (silence),
 *    never a stall of the graph. MIDI streams do the same with one sequence
 *    per cycle.
 *
 * Connections: with PW_STREAM_FLAG_AUTOCONNECT (where PipeWire would link the
 * stream to a device) the channels go to the physical ports of the device
 * that target.object (or the target id) names, from the registry, else of the
 * default (the first) device; a mono stream goes to the first two. Unless
 * JACKBUS_PW_AUTOCONNECT=0 (a session manager, PatchBox, routes instead).
 * MIDI streams are not connected.
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
#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/control/control.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include <jack/midiport.h>

#include "shim.h"
#include "resample.h"
#include "../compat/compat_conf.h"

#define MAX_CHANNELS SPA_AUDIO_MAX_CHANNELS
#define MAX_FRAMES 8192u			/* JACK's BUFFER_SIZE_MAX */
#define RING_FRAMES 32768u			/* async audio, per channel: a power of two */
#define MIDI_SIZE 32768u			/* a cycle's MIDI as a control sequence */
#define MIDI_SLOTS 8u				/* async MIDI: cycles queued */

enum notify_bits {
	NOTIFY_START = 1 << 0,
	NOTIFY_SHUTDOWN = 1 << 1,
	NOTIFY_DRAINED = 1 << 2,
};

struct midi_slot {
	uint32_t size;
	uint8_t data[MIDI_SIZE];
};

/* the last cycle record, for other threads (get_time_n, the async side) */
struct time_snap {
	atomic_uint seq;
	jackbus_cycle_t c;
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
	uint32_t target_id;

	/* the negotiated format */
	bool midi, ump;
	struct spa_audio_info_raw info;	/* audio: the application's */
	bool planar;
	uint32_t sample_size, stride;	/* stride: bytes per frame in one data block */
	uint32_t n_ports;

	/* how it runs */
	bool async;			/* no RT_PROCESS: process on the application's loop */
	uint32_t jack_rate;
	double ratio;			/* resampler input frames per output frame */
	struct shim_resampler *rs;	/* the application's rate isn't JackBus's */
	uint32_t quantum, target;	/* async: JackBus's period, the frames kept queued */
	struct spa_source *timer;

	/* ports, buffers (one locked block) */
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
	float *tmp[MAX_CHANNELS];	/* the application's rate (resampling) */
	float *stage[MAX_CHANNELS];	/* async: JackBus's rate, to and from the ring */
	float *left[MAX_CHANNELS];	/* playback at JackBus's rate: given beyond the request */
	uint32_t n_left;
	float *ring[MAX_CHANNELS];
	atomic_ullong ring_w, ring_r;
	struct midi_slot *slots;
	atomic_uint slot_w, slot_r;
	atomic_bool flush_rt;		/* playback: the realtime side drops what is queued */
	atomic_bool counting;		/* playback: data has come, underruns count */

	struct spa_source *notify;
	atomic_uint pending;
	atomic_bool streaming;
	atomic_bool active;
	bool draining;			/* async: the application's loop */

	/* volume, mute, channel volumes: written by the application, read by the
	 * side that converts (aligned floats: torn values aren't possible on x86-64) */
	volatile float volume;
	volatile bool mute;
	volatile float channel_volumes[MAX_CHANNELS];

	struct time_snap snap;
	atomic_uint buffered;		/* resampler or held frames, the application's rate */

	/* the thread that runs the application's process callback */
	bool in_process, available, queued;
	struct spa_io_position position;
	/* as PipeWire's adapter gives a stream: the frames per cycle at the
	 * stream's rate (OpenAL sizes its mixing from it) */
	struct spa_io_rate_match rate_match;

	/* counted where they happen, reported at disconnect */
	uint64_t underruns, overruns, dropped;
};

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
		for (uint32_t i = 1; i < n; i++)
			if (vals[i] == v)
				return true;
		return vals[0] == v;
	default:
		return false;
	}
}

/* resampling is a fallback, only when asked for */
/* after the direct route was looked for: resampling, unless the settings say no */
static bool resampling_allowed(void)
{
	return jackbus_compat_bool("resample", "JACKBUS_PW_RESAMPLE", true);
}

/* one EnumFormat pod: what JackBus can take of it; resample: may it take
 * the application's own rate (the second pass, when no format allows
 * JackBus's) */
static bool try_format(struct pw_stream *s, const struct spa_pod *param, uint32_t rate,
		bool resample, char *why, size_t len)
{
	uint32_t media_type, media_subtype;
	if (!spa_pod_is_object_type(param, SPA_TYPE_OBJECT_Format) ||
	    spa_format_parse(param, &media_type, &media_subtype) < 0)
		return false;
	if (media_type == SPA_MEDIA_TYPE_application && media_subtype == SPA_MEDIA_SUBTYPE_control) {
		const char *dsp = pw_properties_get(s->props, PW_KEY_FORMAT_DSP);
		s->midi = true;
		s->ump = dsp && spa_streq(dsp, "32 bit raw UMP");
		return true;
	}
	if (media_type != SPA_MEDIA_TYPE_audio || media_subtype != SPA_MEDIA_SUBTYPE_raw) {
		snprintf(why, len, "only raw audio and MIDI");
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

	/* the rate: JackBus's if allowed, else the application's, resampled */
	int32_t def = 0;
	info.rate = rate;
	if ((prop = spa_pod_object_find_prop(obj, NULL, SPA_FORMAT_AUDIO_rate)) != NULL &&
	    !int_allows(&prop->value, (int32_t)rate, &def)) {
		if (!resample || !resampling_allowed()) {
			snprintf(why, len, "rate %d Hz asked, JackBus runs at %u Hz (resampling is off: "
					"resample = no in compat.conf, or JACKBUS_PW_RESAMPLE=0)", def, rate);
			return false;
		}
		if (def <= 0 || (double)def / rate < 0.2 || (double)def / rate > 5.0) {
			snprintf(why, len, "rate %d Hz asked, JackBus runs at %u Hz: too far apart to resample", def, rate);
			return false;
		}
		info.rate = (uint32_t)def;
	}

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
	s->midi = false;
	s->info = info;
	return true;
}

/* ------------------------------------------------------- conversions */

static inline float gain_of(struct pw_stream *s, uint32_t ch)
{
	return s->mute ? 0.0f : s->volume * s->channel_volumes[ch];
}

/* frames of channel ch from the application's buffer (from its frame first,
 * past the chunk's offset) to floats */
static void to_float(struct pw_stream *s, uint32_t ch, float *dst, uint32_t first, uint32_t frames)
{
	float g = gain_of(s, ch);
	uint32_t blk = s->planar ? ch : 0;
	const uint8_t *base = SPA_PTROFF(s->datas[blk].data, s->chunks[blk].offset, const uint8_t);
	uint32_t step = s->planar ? 1 : s->info.channels;
	size_t off = (size_t)first * step + (s->planar ? 0 : ch);

	switch (base_format(s->info.format)) {
	case SPA_AUDIO_FORMAT_F32: {
		const float *src = (const float *)base + off;
		for (uint32_t i = 0; i < frames; i++)
			dst[i] = src[i * step] * g;
		break;
	}
	case SPA_AUDIO_FORMAT_S16: {
		const int16_t *src = (const int16_t *)base + off;
		for (uint32_t i = 0; i < frames; i++)
			dst[i] = src[i * step] * (g / 32768.0f);
		break;
	}
	case SPA_AUDIO_FORMAT_S32: {
		const int32_t *src = (const int32_t *)base + off;
		for (uint32_t i = 0; i < frames; i++)
			dst[i] = (float)(src[i * step] * (g / 2147483648.0));
		break;
	}
	case SPA_AUDIO_FORMAT_S24_32: {
		const int32_t *src = (const int32_t *)base + off;
		for (uint32_t i = 0; i < frames; i++)
			dst[i] = ((int32_t)((uint32_t)src[i * step] << 8) >> 8) * (g / 8388608.0f);
		break;
	}
	case SPA_AUDIO_FORMAT_F64: {
		const double *src = (const double *)base + off;
		for (uint32_t i = 0; i < frames; i++)
			dst[i] = (float)(src[i * step] * g);
		break;
	}
	}
}

static inline float clampf(float v)
{
	return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
}

/* floats to frames of channel ch of the application's buffer, from frame 0 */
static void from_float(struct pw_stream *s, uint32_t ch, const float *src, uint32_t frames)
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

/* ----------------------------------------- the application's process */

/* the process callback with the one buffer, in the thread that runs it */
static void call_process(struct pw_stream *s, uint64_t requested, uint32_t size)
{
	s->pwbuf.requested = requested;
	s->pwbuf.size = size;
	s->available = true;
	s->queued = false;
	s->in_process = true;
	spa_hook_list_call_once(&s->listeners, struct pw_stream_events, process, 0);
	s->in_process = false;
	s->available = false;
}

/* playback: the frames the application queued */
static uint32_t queued_frames(struct pw_stream *s)
{
	if (!s->queued || s->stride == 0)
		return 0;
	uint32_t off = s->chunks[0].offset, max = s->datas[0].maxsize;
	if (off >= max)
		return 0;
	return SPA_MIN(s->chunks[0].size, max - off) / s->stride;
}

/* playback: up to n frames at JackBus's rate into out, from the application
 * (and what it gave beyond earlier requests); exact: all n or none, when
 * resampling. Returns the frames written. */
static uint32_t produce(struct pw_stream *s, float *const *out, uint32_t n, bool exact)
{
	uint32_t ch, nc = s->info.channels;

	if (s->rs) {
		for (int calls = 0; calls < 8; calls++) {
			int need = shim_resampler_needed(s->rs, n);
			if (need <= 0)
				break;
			set_chunks(s, 0);
			call_process(s, SPA_MIN((uint32_t)need, MAX_FRAMES), 0);
			uint32_t got = queued_frames(s);
			if (got == 0)
				break;
			for (ch = 0; ch < nc; ch++)
				to_float(s, ch, s->tmp[ch], 0, got);
			int took = shim_resampler_push(s->rs, (const float *const *)s->tmp, (int)got);
			if (took < (int)got)
				s->dropped += got - took;
		}
		int done = exact ? shim_resampler_pull(s->rs, out, (int)n) :
			shim_resampler_pull_available(s->rs, out, (int)n);
		double held = shim_resampler_delay(s->rs);
		atomic_store_explicit(&s->buffered, held > 0 ? (uint32_t)held : 0, memory_order_relaxed);
		return done > 0 ? (uint32_t)done : 0;
	}

	uint32_t done = 0;
	if (s->n_left) {
		uint32_t take = SPA_MIN(s->n_left, n);
		for (ch = 0; ch < nc; ch++) {
			memcpy(out[ch], s->left[ch], take * sizeof(float));
			memmove(s->left[ch], s->left[ch] + take, (s->n_left - take) * sizeof(float));
		}
		s->n_left -= take;
		done = take;
	}
	if (done < n) {
		set_chunks(s, 0);
		call_process(s, n - done, 0);
		uint32_t got = queued_frames(s);
		uint32_t use = SPA_MIN(got, n - done);
		for (ch = 0; ch < nc; ch++)
			to_float(s, ch, out[ch] + done, 0, use);
		if (got > use) {
			uint32_t extra = SPA_MIN(got - use, MAX_FRAMES - s->n_left);
			for (ch = 0; ch < nc; ch++)
				to_float(s, ch, s->left[ch] + s->n_left, use, extra);
			s->n_left += extra;
			s->dropped += got - use - extra;
		}
		done += use;
	}
	atomic_store_explicit(&s->buffered, s->n_left, memory_order_relaxed);
	return done;
}

/* capture: n frames at JackBus's rate to the application */
static void consume(struct pw_stream *s, const float *const *in, uint32_t n)
{
	uint32_t ch, nc = s->info.channels;

	if (s->rs) {
		shim_resampler_push(s->rs, in, (int)n);
		for (int calls = 0; calls < 8; calls++) {
			int got = shim_resampler_pull_available(s->rs, s->tmp, MAX_FRAMES);
			if (got <= 0)
				break;
			for (ch = 0; ch < nc; ch++)
				from_float(s, ch, s->tmp[ch], (uint32_t)got);
			set_chunks(s, (uint32_t)got);
			call_process(s, 0, (uint32_t)got);
		}
		return;
	}
	for (ch = 0; ch < nc; ch++)
		from_float(s, ch, in[ch], n);
	set_chunks(s, n);
	call_process(s, 0, n);
}

/* --------------------------------------------------- the async ring */

static void ring_out(struct pw_stream *s, uint64_t pos, float *const *dst, uint32_t n)
{
	uint32_t idx = pos & (RING_FRAMES - 1), first = SPA_MIN(n, RING_FRAMES - idx);
	for (uint32_t ch = 0; ch < s->info.channels; ch++) {
		memcpy(dst[ch], s->ring[ch] + idx, first * sizeof(float));
		memcpy(dst[ch] + first, s->ring[ch], (n - first) * sizeof(float));
	}
}

static void ring_in(struct pw_stream *s, uint64_t pos, const float *const *src, uint32_t n)
{
	uint32_t idx = pos & (RING_FRAMES - 1), first = SPA_MIN(n, RING_FRAMES - idx);
	for (uint32_t ch = 0; ch < s->info.channels; ch++) {
		memcpy(s->ring[ch] + idx, src[ch], first * sizeof(float));
		memcpy(s->ring[ch], src[ch] + first, (n - first) * sizeof(float));
	}
}

/* realtime: ports <-> ring */
static void ring_rt(struct pw_stream *s, float *const *port, uint32_t n, bool playback, bool run)
{
	if (playback) {
		uint64_t w = atomic_load_explicit(&s->ring_w, memory_order_acquire);
		uint64_t r = atomic_load_explicit(&s->ring_r, memory_order_relaxed);
		if (atomic_load_explicit(&s->flush_rt, memory_order_relaxed)) {
			r = w;
			atomic_store_explicit(&s->flush_rt, false, memory_order_relaxed);
		}
		uint32_t take = run ? (uint32_t)SPA_MIN(w - r, (uint64_t)n) : 0;
		if (take)
			ring_out(s, r, port, take);
		if (take < n) {
			for (uint32_t ch = 0; ch < s->info.channels; ch++)
				memset(port[ch] + take, 0, (n - take) * sizeof(float));
			if (run && atomic_load_explicit(&s->counting, memory_order_relaxed))
				s->underruns++;
		}
		atomic_store_explicit(&s->ring_r, r + take, memory_order_release);
	} else if (run) {
		uint64_t r = atomic_load_explicit(&s->ring_r, memory_order_acquire);
		uint64_t w = atomic_load_explicit(&s->ring_w, memory_order_relaxed);
		if (RING_FRAMES - (w - r) < n) {
			s->overruns++;
			return;
		}
		ring_in(s, w, (const float *const *)port, n);
		atomic_store_explicit(&s->ring_w, w + n, memory_order_release);
	}
}

/* ------------------------------------------------------------- MIDI */

static void midi_prepare_out(struct pw_stream *s)
{
	s->chunks[0].offset = 0;
	s->chunks[0].size = 0;
	s->chunks[0].stride = 1;
	s->chunks[0].flags = 0;
}

static void midi_rt(struct pw_stream *s, const jackbus_cycle_t *c, uint32_t n, bool playback, bool run)
{
	void *jbuf = jackbus_port_midi(s->ports[0]);

	if (playback) {
		jack_midi_clear_buffer(jbuf);
		if (s->async) {
			uint32_t r = atomic_load_explicit(&s->slot_r, memory_order_relaxed);
			uint32_t w = atomic_load_explicit(&s->slot_w, memory_order_acquire);
			if (atomic_load_explicit(&s->flush_rt, memory_order_relaxed)) {
				r = w;
				atomic_store_explicit(&s->flush_rt, false, memory_order_relaxed);
			}
			if (run && r != w) {
				struct midi_slot *sl = &s->slots[r % MIDI_SLOTS];
				shim_midi_to_jack(sl->data, MIDI_SIZE, 0, sl->size, jbuf, n);
				r++;
			} else if (run && atomic_load_explicit(&s->counting, memory_order_relaxed)) {
				s->underruns++;
			}
			atomic_store_explicit(&s->slot_r, r, memory_order_release);
			return;
		}
		if (!run)
			return;
		shim_fill_position(&s->position, c, s->client);
		midi_prepare_out(s);
		call_process(s, n, 0);
		if (s->queued)
			shim_midi_to_jack(s->datas[0].data, s->datas[0].maxsize, s->chunks[0].offset,
					s->chunks[0].size, jbuf, n);
		return;
	}
	if (!run)
		return;
	if (s->async) {
		uint32_t r = atomic_load_explicit(&s->slot_r, memory_order_acquire);
		uint32_t w = atomic_load_explicit(&s->slot_w, memory_order_relaxed);
		if (w - r >= MIDI_SLOTS) {
			s->overruns++;
			return;
		}
		struct midi_slot *sl = &s->slots[w % MIDI_SLOTS];
		sl->size = shim_midi_from_jack(jbuf, sl->data, MIDI_SIZE, s->ump);
		atomic_store_explicit(&s->slot_w, w + 1, memory_order_release);
		return;
	}
	shim_fill_position(&s->position, c, s->client);
	uint32_t size = shim_midi_from_jack(jbuf, s->datas[0].data, MIDI_SIZE, s->ump);
	s->chunks[0].offset = 0;
	s->chunks[0].size = size;
	s->chunks[0].stride = 1;
	call_process(s, 0, size);
}

/* ------------------------------------------------ the realtime side */

static void publish_time(struct pw_stream *s, const jackbus_cycle_t *c)
{
	unsigned seq = atomic_load_explicit(&s->snap.seq, memory_order_relaxed);
	atomic_store_explicit(&s->snap.seq, seq + 1, memory_order_relaxed);
	atomic_thread_fence(memory_order_release);
	s->snap.c = *c;
	atomic_store_explicit(&s->snap.seq, seq + 2, memory_order_release);
}

static bool read_time(struct pw_stream *s, jackbus_cycle_t *c)
{
	for (int i = 0; i < 1000; i++) {
		unsigned a = atomic_load_explicit(&s->snap.seq, memory_order_acquire);
		if (a & 1)
			continue;
		*c = s->snap.c;
		atomic_thread_fence(memory_order_acquire);
		if (atomic_load_explicit(&s->snap.seq, memory_order_relaxed) == a)
			return a != 0;
	}
	return false;
}

static uint32_t frames_at_stream_rate(struct pw_stream *s, uint32_t n)
{
	if (s->midi || s->jack_rate == 0 || s->info.rate == s->jack_rate)
		return n;
	return (uint32_t)(((uint64_t)n * s->info.rate + s->jack_rate - 1) / s->jack_rate);
}

static void stream_process(struct shim_member *m, const jackbus_cycle_t *c)
{
	struct pw_stream *s = SPA_CONTAINER_OF(m, struct pw_stream, member);
	bool playback = s->direction == SPA_DIRECTION_OUTPUT;
	uint32_t n = SPA_MIN(c->nframes, MAX_FRAMES);
	bool run = atomic_load_explicit(&s->streaming, memory_order_acquire) &&
		atomic_load_explicit(&s->active, memory_order_relaxed);
	float *port[MAX_CHANNELS];

	publish_time(s, c);
	if (!s->async)
		s->rate_match.size = frames_at_stream_rate(s, n);
	if (s->midi) {
		midi_rt(s, c, n, playback, run);
		return;
	}
	for (uint32_t ch = 0; ch < s->info.channels; ch++)
		port[ch] = jackbus_port_audio(s->ports[ch]);
	if (s->async) {
		ring_rt(s, port, n, playback, run);
		return;
	}
	if (playback) {
		uint32_t got = 0;
		if (run) {
			shim_fill_position(&s->position, c, s->client);
			got = produce(s, port, n, true);
		}
		if (got < n) {
			for (uint32_t ch = 0; ch < s->info.channels; ch++)
				memset(port[ch] + got, 0, (n - got) * sizeof(float));
			if (run)
				s->underruns++;
		}
	} else if (run) {
		shim_fill_position(&s->position, c, s->client);
		consume(s, (const float *const *)port, n);
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

/* --------------------------------------- the async side (application's loop) */

static uint32_t async_quanta(void)
{
	const char *env = getenv("JACKBUS_PW_ASYNC_QUANTA");
	int v = env ? atoi(env) : 2;
	return v >= 1 && v <= 3 ? (uint32_t)v : 2;
}

/* JackBus's period as now: the timer at half of it (0.5 to 5 ms), and the
 * frames kept queued: two quanta, at least three timer periods */
static void update_quantum(struct pw_stream *s)
{
	uint32_t q = jack_get_buffer_size(s->client);
	if (q == 0 || q > MAX_FRAMES || q == s->quantum)
		return;
	s->quantum = q;
	uint64_t period = (uint64_t)q * SPA_NSEC_PER_SEC / s->jack_rate;
	uint64_t interval = SPA_CLAMP(period / 2, (uint64_t)500 * SPA_NSEC_PER_USEC, (uint64_t)5 * SPA_NSEC_PER_MSEC);
	uint64_t floor_frames = (3 * interval * s->jack_rate + SPA_NSEC_PER_SEC - 1) / SPA_NSEC_PER_SEC;
	uint32_t target = async_quanta() * q;
	while (target < floor_frames)
		target += q;
	s->target = SPA_MIN(target, RING_FRAMES / 2);
	s->rate_match.size = frames_at_stream_rate(s, q);
	struct timespec value = { 0, 1 };
	struct timespec iv = { (time_t)(interval / SPA_NSEC_PER_SEC), (long)(interval % SPA_NSEC_PER_SEC) };
	pw_loop_update_timer(s->core->context->main_loop, s->timer, &value, &iv, false);
	shim_log("%s: async, %u frames queued (quantum %u), timer %.2f ms", s->name, s->target, q,
			interval / 1e6);
}

static void emit_drained(struct pw_stream *s)
{
	s->draining = false;
	atomic_store(&s->counting, false);
	spa_hook_list_call(&s->listeners, struct pw_stream_events, drained, 0);
}

static void async_midi(struct pw_stream *s, bool playback)
{
	if (playback) {
		for (int i = 0; i < 32; i++) {
			uint32_t r = atomic_load_explicit(&s->slot_r, memory_order_acquire);
			uint32_t w = atomic_load_explicit(&s->slot_w, memory_order_relaxed);
			if (s->draining) {
				if (r == w)
					emit_drained(s);
				return;
			}
			if (!atomic_load(&s->active) || (uint64_t)(w - r) * s->quantum >= s->target ||
			    w - r >= MIDI_SLOTS)
				return;
			midi_prepare_out(s);
			call_process(s, s->quantum, 0);
			struct midi_slot *sl = &s->slots[w % MIDI_SLOTS];
			sl->size = 0;
			if (s->queued && s->chunks[0].size > 0 && s->chunks[0].offset < MIDI_SIZE) {
				const struct spa_pod *pod = SPA_PTROFF(s->datas[0].data, s->chunks[0].offset,
						const struct spa_pod);
				uint32_t size = SPA_POD_SIZE(pod);
				if (size <= MIDI_SIZE - s->chunks[0].offset) {
					memcpy(sl->data, pod, size);
					sl->size = size;
				}
			}
			atomic_store_explicit(&s->slot_w, w + 1, memory_order_release);
			atomic_store(&s->counting, true);
		}
		return;
	}
	for (int i = 0; i < 64; i++) {
		uint32_t w = atomic_load_explicit(&s->slot_w, memory_order_acquire);
		uint32_t r = atomic_load_explicit(&s->slot_r, memory_order_relaxed);
		if (r == w)
			return;
		struct midi_slot *sl = &s->slots[r % MIDI_SLOTS];
		uint32_t size = sl->size;
		memcpy(s->datas[0].data, sl->data, size);
		atomic_store_explicit(&s->slot_r, r + 1, memory_order_release);
		if (!atomic_load(&s->active))
			continue;
		s->chunks[0].offset = 0;
		s->chunks[0].size = size;
		s->chunks[0].stride = 1;
		call_process(s, 0, size);
	}
}

static void on_timer(void *data, uint64_t expirations)
{
	struct pw_stream *s = data;
	bool playback = s->direction == SPA_DIRECTION_OUTPUT;
	jackbus_cycle_t c;

	if (!atomic_load(&s->streaming) || s->client == NULL)
		return;
	update_quantum(s);
	if (read_time(s, &c))
		shim_fill_position(&s->position, &c, s->client);
	s->position.clock.duration = s->quantum;
	if (s->midi) {
		async_midi(s, playback);
		return;
	}
	if (playback) {
		for (int i = 0; i < 32; i++) {
			uint64_t r = atomic_load_explicit(&s->ring_r, memory_order_acquire);
			uint64_t w = atomic_load_explicit(&s->ring_w, memory_order_relaxed);
			if (s->draining) {
				if (w == r)
					emit_drained(s);	/* the application may destroy s */
				return;
			}
			if (!atomic_load(&s->active) || w - r >= s->target)
				return;
			uint32_t got = produce(s, s->stage, s->quantum, false);
			if (got == 0)
				return;
			ring_in(s, w, (const float *const *)s->stage, got);
			atomic_store_explicit(&s->ring_w, w + got, memory_order_release);
			atomic_store(&s->counting, true);
		}
		return;
	}
	for (int i = 0; i < 32; i++) {
		uint64_t w = atomic_load_explicit(&s->ring_w, memory_order_acquire);
		uint64_t r = atomic_load_explicit(&s->ring_r, memory_order_relaxed);
		if (w == r)
			return;
		uint32_t n = (uint32_t)SPA_MIN(w - r, (uint64_t)s->quantum);
		ring_out(s, r, s->stage, n);
		atomic_store_explicit(&s->ring_r, r + n, memory_order_release);
		if (atomic_load(&s->active))
			consume(s, (const float *const *)s->stage, n);
	}
}

/* ------------------------------------------------ main-loop notifications */

static void autoconnect(struct pw_stream *s)
{
	if (!(s->flags & PW_STREAM_FLAG_AUTOCONNECT) ||
	    !jackbus_compat_bool("autoconnect", "JACKBUS_PW_AUTOCONNECT", true))
		return;
	if (s->midi) {
		shim_log("%s: a MIDI stream is not connected (no default MIDI device in JACK)", s->name);
		return;
	}
	bool playback = s->direction == SPA_DIRECTION_OUTPUT;
	const char *target = pw_properties_get(s->props, PW_KEY_TARGET_OBJECT);
	if (target == NULL)
		target = pw_properties_get(s->props, "node.target");
	const char **phys = shim_target_ports(s->client, target, s->target_id, playback);
	if (phys == NULL)
		return;
	uint32_t n_phys = 0;
	while (phys[n_phys])
		n_phys++;
	for (uint32_t i = 0; i < n_phys && i < SPA_MAX(s->info.channels, 2u); i++) {
		uint32_t ch = i;
		if (ch >= s->info.channels) {
			if (s->info.channels != 1)
				break;
			ch = 0;			/* mono: to the first two */
		}
		const char *mine = jack_port_name(jackbus_port_jack(s->ports[ch]));
		if (playback)
			jack_connect(s->client, mine, phys[i]);
		else if (i == ch)
			jack_connect(s->client, phys[i], mine);
	}
	free(phys);
}

static void emit_format(struct pw_stream *s)
{
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_pod *fmt;

	if (s->midi)
		fmt = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
				SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
				SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_control),
				SPA_FORMAT_CONTROL_types, SPA_POD_Int(1u << (s->ump ? SPA_CONTROL_UMP : SPA_CONTROL_Midi)));
	else
		fmt = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &s->info);
	spa_hook_list_call(&s->listeners, struct pw_stream_events, param_changed, 0,
			SPA_PARAM_Format, fmt);
}

/* the Latency param, once connected: from the ports' JACK latency (mpv,
 * for one, takes it as the sign that the stream is linked) */
static void emit_latency(struct pw_stream *s)
{
	uint8_t buffer[512];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_latency_info info;
	jack_latency_range_t r = { 0, 0 };
	bool playback = s->direction == SPA_DIRECTION_OUTPUT;

	if (s->ports[0])
		jack_port_get_latency_range(jackbus_port_jack(s->ports[0]),
				playback ? JackPlaybackLatency : JackCaptureLatency, &r);
	info = SPA_LATENCY_INFO(playback ? SPA_DIRECTION_INPUT : SPA_DIRECTION_OUTPUT);
	info.min_rate = r.min;
	info.max_rate = r.max;
	spa_hook_list_call(&s->listeners, struct pw_stream_events, param_changed, 0,
			SPA_PARAM_Latency, spa_latency_build(&b, SPA_PARAM_Latency, &info));
}

static void on_notify(void *data, uint64_t count)
{
	struct pw_stream *s = data;
	unsigned bits = atomic_exchange(&s->pending, 0);

	if (bits & NOTIFY_START && s->state == PW_STREAM_STATE_CONNECTING) {
		set_state(s, PW_STREAM_STATE_PAUSED, 0, NULL);
		spa_hook_list_call(&s->listeners, struct pw_stream_events, io_changed, 0,
				SPA_IO_Position, &s->position, sizeof(s->position));
		s->rate_match.rate = 1.0;
		s->rate_match.size = frames_at_stream_rate(s, jack_get_buffer_size(s->client));
		s->rate_match.flags = s->rs ? SPA_IO_RATE_MATCH_FLAG_ACTIVE : 0;
		spa_hook_list_call(&s->listeners, struct pw_stream_events, io_changed, 0,
				SPA_IO_RateMatch, &s->rate_match, sizeof(s->rate_match));
		emit_format(s);
		spa_hook_list_call(&s->listeners, struct pw_stream_events, add_buffer, 0, &s->pwbuf);
		if (!(s->flags & PW_STREAM_FLAG_INACTIVE)) {
			atomic_store(&s->active, true);
			set_state(s, PW_STREAM_STATE_STREAMING, 0, NULL);
		}
		atomic_store_explicit(&s->streaming, true, memory_order_release);
		if (s->async)
			update_quantum(s);
		autoconnect(s);
		emit_latency(s);
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
	if ((str = getenv("PIPEWIRE_NODE")) != NULL)
		pw_properties_set(props, PW_KEY_TARGET_OBJECT, str);
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
	s->node_id = shim_new_id();
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

static void free_buffers(struct pw_stream *s)
{
	if (s->mem) {
		munlock(s->mem, s->mem_size);
		free(s->mem);
	}
	s->mem = NULL;
	s->mem_size = 0;
	s->slots = NULL;
	for (uint32_t i = 0; i < MAX_CHANNELS; i++)
		s->tmp[i] = s->stage[i] = s->left[i] = s->ring[i] = NULL;
	shim_resampler_free(s->rs);
	s->rs = NULL;
}

/* all of a stream's buffers in one locked block: they are touched in the
 * realtime thread */
static int alloc_buffers(struct pw_stream *s)
{
	uint32_t nc = s->midi ? 0 : s->info.channels;
	uint32_t blocks = s->midi ? 1 : (s->planar ? nc : 1);
	size_t block = s->midi ? MIDI_SIZE : SPA_ROUND_UP_N((size_t)MAX_FRAMES * s->stride, 64);
	size_t floats = SPA_ROUND_UP_N((size_t)MAX_FRAMES * sizeof(float), 64);
	size_t ring = s->async && !s->midi ? (size_t)RING_FRAMES * sizeof(float) : 0;
	size_t slots = s->async && s->midi ? MIDI_SLOTS * SPA_ROUND_UP_N(sizeof(struct midi_slot), 64) : 0;
	size_t off = 0;

	s->mem_size = blocks * block + (size_t)nc * (3 * floats + ring) + slots;
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
		s->datas[i].maxsize = s->midi ? MIDI_SIZE : MAX_FRAMES * s->stride;
		s->datas[i].data = SPA_PTROFF(s->mem, off, void);
		s->datas[i].chunk = &s->chunks[i];
		off += block;
	}
	for (uint32_t ch = 0; ch < nc; ch++) {
		s->tmp[ch] = SPA_PTROFF(s->mem, off, float);
		off += floats;
		s->stage[ch] = SPA_PTROFF(s->mem, off, float);
		off += floats;
		s->left[ch] = SPA_PTROFF(s->mem, off, float);
		off += floats;
		if (ring) {
			s->ring[ch] = SPA_PTROFF(s->mem, off, float);
			off += ring;
		}
	}
	if (slots)
		s->slots = SPA_PTROFF(s->mem, off, struct midi_slot);
	s->pwbuf.buffer = &s->sbuf;
	return 0;
}

static void log_forced_settings(struct pw_stream *s)
{
	const char *lat = pw_properties_get(s->props, PW_KEY_NODE_LATENCY);
	const char *how = s->async ? "on its loop (async)" : "in the JACK thread";
	if (s->midi) {
		shim_log("%s: MIDI %s (%s), process %s", s->name,
				s->direction == SPA_DIRECTION_OUTPUT ? "playback" : "capture",
				s->ump ? "UMP" : "MIDI 1.0", how);
		return;
	}
	char pos[256] = "";
	size_t len = 0;
	for (uint32_t i = 0; i < s->info.channels && len < sizeof(pos); i++)
		len += snprintf(pos + len, sizeof(pos) - len, "%s%s", i ? "," : "",
				spa_type_audio_channel_to_short_name(s->info.position[i]));
	shim_log("%s: %s, %s %u Hz %u ch (%s); quantum %u frames%s%s%s; process %s", s->name,
			s->direction == SPA_DIRECTION_OUTPUT ? "playback" : "capture",
			spa_type_audio_format_to_short_name(s->info.format), s->info.rate, s->info.channels, pos,
			jack_get_buffer_size(s->client),
			lat ? " (asked node.latency=" : "", lat ? lat : "", lat ? ")" : "", how);
	if (s->rs)
		shim_log("%s: resampled %u Hz -> %u Hz (JackBus's)", s->name,
				s->direction == SPA_DIRECTION_OUTPUT ? s->info.rate : s->jack_rate,
				s->direction == SPA_DIRECTION_OUTPUT ? s->jack_rate : s->info.rate);
}

static const char *client_name_for(struct pw_stream *s)
{
	const char *n = pw_properties_get(s->props, PW_KEY_APP_NAME);
	if (n && n[0])
		return n;
	n = pw_properties_get(s->core->props, PW_KEY_APP_NAME);
	if (n && n[0])
		return n;
	n = pw_properties_get(s->props, PW_KEY_NODE_NAME);
	if (n && n[0])
		return n;
	n = pw_get_prgname();
	return n ? n : s->name;
}

static void connect_failed(struct pw_stream *s, int res, const char *why)
{
	free_buffers(s);
	if (s->node) {
		s->node = NULL;
		s->client = NULL;
		shim_app_unref();
	}
	set_state(s, PW_STREAM_STATE_ERROR, res, why);
}

SHIM_EXPORT int pw_stream_connect(struct pw_stream *s, enum pw_direction direction,
		uint32_t target_id, enum pw_stream_flags flags,
		const struct spa_pod **params, uint32_t n_params)
{
	char why[256] = "no audio or MIDI format offered";

	if (s->node != NULL)
		return -EBUSY;
	s->flags = flags;
	s->direction = (enum spa_direction)direction;
	s->target_id = target_id;
	s->async = !(flags & PW_STREAM_FLAG_RT_PROCESS);
	set_state(s, PW_STREAM_STATE_CONNECTING, 0, NULL);

	const char *media = pw_properties_get(s->props, PW_KEY_MEDIA_TYPE);
	if (media && strcmp(media, "Audio") != 0 && strcmp(media, "Midi") != 0) {
		set_state(s, PW_STREAM_STATE_ERROR, -ENOTSUP, "JackBus carries audio and MIDI streams only");
		return -ENOTSUP;
	}
	if ((s->node = shim_app_ref(client_name_for(s))) == NULL) {
		set_state(s, PW_STREAM_STATE_ERROR, -EIO, "can't connect to JackBus");
		return -EIO;
	}
	s->client = jackbus_node_client(s->node);
	s->jack_rate = jack_get_sample_rate(s->client);

	/* the direct route first: any offered format at JackBus's rate; only
	 * then one to resample */
	bool ok = false;
	for (int pass = 0; pass < 2 && !ok; pass++)
		for (uint32_t i = 0; i < n_params && !ok; i++)
			if (params[i])
				ok = try_format(s, params[i], s->jack_rate, pass == 1, why, sizeof(why));
	if (!ok) {
		shim_warn("%s: %s", s->name, why);
		connect_failed(s, -EINVAL, why);
		return -EINVAL;
	}
	if (s->midi) {
		s->planar = false;
		s->sample_size = s->stride = 1;
		s->n_ports = 1;
	} else {
		format_known(s->info.format, &s->planar, &s->sample_size);
		s->stride = s->planar ? s->sample_size : s->sample_size * s->info.channels;
		s->n_ports = s->info.channels;
	}
	if (alloc_buffers(s) < 0) {
		connect_failed(s, -ENOMEM, "out of memory");
		return -ENOMEM;
	}
	if (!s->midi && s->info.rate != s->jack_rate) {
		bool playback = s->direction == SPA_DIRECTION_OUTPUT;
		s->ratio = playback ? (double)s->info.rate / s->jack_rate : (double)s->jack_rate / s->info.rate;
		s->rs = shim_resampler_new((int)s->info.channels, s->ratio, MAX_FRAMES, MAX_FRAMES);
		if (s->rs == NULL) {
			connect_failed(s, -ENOMEM, "can't make a resampler");
			return -ENOMEM;
		}
	}

	/* the ports: one per channel, or one MIDI port */
	unsigned long jflags = (s->direction == SPA_DIRECTION_OUTPUT) ? JackPortIsOutput : JackPortIsInput;
	for (uint32_t ch = 0; ch < s->n_ports; ch++) {
		char name[300];
		const char *base = s->midi ? (s->direction == SPA_DIRECTION_OUTPUT ? "midi_out" : "midi_in") :
			spa_type_audio_channel_to_short_name(s->info.position[ch]);
		shim_app_port_name(pw_properties_get(s->props, PW_KEY_NODE_NAME), base, name, sizeof(name));
		int kind = s->midi ? JACKBUS_MIDI : JACKBUS_AUDIO;
		s->ports[ch] = jackbus_port_add(s->node, name, kind, jflags);
		for (int k = 2; s->ports[ch] == NULL && k < 100; k++) {
			char unique[310];
			snprintf(unique, sizeof(unique), "%s-%d", name, k);
			s->ports[ch] = jackbus_port_add(s->node, unique, kind, jflags);
		}
		if (s->ports[ch] == NULL) {
			for (uint32_t i = 0; i < ch; i++) {
				jackbus_port_remove(s->ports[i]);
				s->ports[i] = NULL;
			}
			connect_failed(s, -EIO, "can't register a JACK port");
			return -EIO;
		}
	}

	for (uint32_t ch = 0; ch < s->n_ports; ch++)
		shim_own_port(jackbus_port_jack(s->ports[ch]), s->node_id);
	const char *mc = pw_properties_get(s->props, PW_KEY_MEDIA_CLASS);
	bool playback = s->direction == SPA_DIRECTION_OUTPUT;
	shim_own_node(s->node_id, pw_properties_get(s->props, PW_KEY_NODE_NAME), mc ? mc :
			s->midi ? (playback ? "Stream/Output/Midi" : "Stream/Input/Midi") :
			(playback ? "Stream/Output/Audio" : "Stream/Input/Audio"));

	/* valid before the first cycle */
	s->position.clock.rate = SPA_FRACTION(1, s->jack_rate);
	s->position.clock.duration = jack_get_buffer_size(s->client);
	snprintf(s->position.clock.name, sizeof(s->position.clock.name), "jackbus");
	if (s->async) {
		s->timer = pw_loop_add_timer(s->core->context->main_loop, on_timer, s);
		if (s->timer == NULL) {
			for (uint32_t ch = 0; ch < s->n_ports; ch++) {
				shim_own_port_gone(jackbus_port_jack(s->ports[ch]));
				jackbus_port_remove(s->ports[ch]);
				s->ports[ch] = NULL;
			}
			shim_own_node_gone(s->node_id);
			connect_failed(s, -ENOMEM, "can't make a timer");
			return -ENOMEM;
		}
	}
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
	if (s->timer) {
		pw_loop_destroy_source(s->core->context->main_loop, s->timer);
		s->timer = NULL;
	}
	for (uint32_t ch = 0; ch < MAX_CHANNELS; ch++) {
		if (s->ports[ch]) {
			shim_own_port_gone(jackbus_port_jack(s->ports[ch]));
			jackbus_port_remove(s->ports[ch]);
			s->ports[ch] = NULL;
		}
	}
	shim_own_node_gone(s->node_id);
	if (s->underruns || s->overruns || s->dropped)
		shim_log("%s: %llu cycles short of data, %llu cycles dropped (full), %llu frames dropped",
				s->name, (unsigned long long)s->underruns,
				(unsigned long long)s->overruns, (unsigned long long)s->dropped);
	if (s->state == PW_STREAM_STATE_STREAMING || s->state == PW_STREAM_STATE_PAUSED)
		spa_hook_list_call(&s->listeners, struct pw_stream_events, remove_buffer, 0, &s->pwbuf);
	free_buffers(s);
	atomic_store(&s->ring_w, 0);
	atomic_store(&s->ring_r, 0);
	atomic_store(&s->slot_w, 0);
	atomic_store(&s->slot_r, 0);
	atomic_store(&s->counting, false);
	s->n_left = 0;
	s->draining = false;
	s->quantum = 0;
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
	jackbus_cycle_t c;

	spa_zero(t);
	t.rate = SPA_FRACTION(1, s->jack_rate ? s->jack_rate : 48000);
	if (read_time(s, &c)) {
		t.now = (int64_t)c.time_ns;
		t.ticks = c.frame;
		t.size = c.nframes;
	}
	if (s->ports[0] && !s->midi) {
		/* the delay to the device: the JACK latency of the stream's ports */
		jack_latency_range_t r;
		jack_port_get_latency_range(jackbus_port_jack(s->ports[0]),
				s->direction == SPA_DIRECTION_OUTPUT ? JackPlaybackLatency : JackCaptureLatency, &r);
		t.delay = r.max;
	}
	/* queued: what the ring holds, in the application's frames (pw_buffer.size's
	 * unit); buffered: held in the resampler or beyond a request */
	if (s->async && !s->midi && s->jack_rate) {
		uint64_t level = atomic_load(&s->ring_w) - atomic_load(&s->ring_r);
		t.queued = (uint64_t)(level * (double)s->info.rate / s->jack_rate);
		t.queued_buffers = s->quantum ? (uint32_t)(level / s->quantum) : 0;
	}
	t.buffered = atomic_load_explicit(&s->buffered, memory_order_relaxed);
	t.avail_buffers = 1;
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
	/* where process runs: the application's loop for an async stream */
	if (!(s->flags & PW_STREAM_FLAG_RT_PROCESS) && s->node)
		return s->core->context->main_loop;
	return shim_data_loop_get(s->core->context);
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
	if (s->async && s->node) {
		bool playback = s->direction == SPA_DIRECTION_OUTPUT;
		if (drain && playback) {
			s->draining = true;	/* drained once the ring is empty */
			atomic_store(&s->counting, false);	/* the last, short cycle is the end */
			return 0;
		}
		if (playback) {
			atomic_store(&s->flush_rt, true);
			s->n_left = 0;
			if (s->rs)
				shim_resampler_reset(s->rs);
		} else if (s->midi) {
			atomic_store(&s->slot_r, atomic_load(&s->slot_w));
		} else {
			atomic_store(&s->ring_r, atomic_load(&s->ring_w));
		}
		if (drain) {
			atomic_fetch_or(&s->pending, NOTIFY_DRAINED);
			pw_loop_signal_event(s->core->context->main_loop, s->notify);
		}
		return 0;
	}
	/* in the JACK thread nothing is held back beyond the resampler's few frames */
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
