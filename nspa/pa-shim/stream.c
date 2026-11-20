/*
 * jackbus-pa: pa_stream on the application's JackBus node.
 *
 * A stream is a group of ports on the application's JACK client (node.c), one
 * per channel, named by channel position (FL, FR, ... for playback, in_FL ...
 * for recording; a second stream of the same kind prefixes its index). Its
 * samples pass through a ring at JackBus's rate:
 *  - the realtime side, once per cycle, copies one cycle between the ring and
 *    the ports, applying the stream's volume; it honours cork, prebuffering,
 *    flush and drain, and counts underflows and overflows. No lock, no system
 *    call, no wakeup;
 *  - the application's side runs on its own main loop: a timer, phase-locked
 *    to the cycle, asks for data (write callback) when tlength - queued
 *    reaches minreq, offers recorded data (read callback) by fragsize, and
 *    reports underflow, overflow, started and a finished drain. Writes and
 *    peeks convert the sample format at the boundary.
 * Positions are JackBus's frames: pa_stream_get_time() and the latency
 * functions are exact, interpolated within the cycle from the cycle's own
 * time.
 *
 * The rate: JackBus's whenever the application allows it (the server and
 * every device say JackBus's rate, and PA_STREAM_FIX_RATE takes it); a stream
 * at another rate is resampled by JackBus's bridge resampler, a fallback,
 * unless the settings say no (compat.conf "resample = no", or
 * JACKBUS_PA_RESAMPLE=0), when it is refused with the reason. With autoconnect (on
 * unless JACKBUS_PA_AUTOCONNECT=0) the ports go to the chosen device's (the
 * default's) physical ports in order, a mono playback stream to the first two,
 * a mono record stream from the first.
 *
 * A record stream with PA_STREAM_PEAK_DETECT (a level meter's: pavucontrol's,
 * the desktop's) is not resampled: at its rate (25 Hz as a rule) it gets each
 * channel's largest magnitude in that time, over the ports connected to it
 * averaged (a port sums them, PulseAudio's downmix averages).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "shim.h"
#include "../compat/compat_conf.h"

#define RING_FRAMES 131072u		/* per channel, a power of two (2.7 s at 48 kHz) */
#define CHUNK 8192u			/* frames converted at a time */
#define RS_OUT (CHUNK * 5)		/* resampler output room for one chunk */

struct pa_stream {
	struct shim_member member;	/* first: the node runs it */
	int refs;
	pa_context *context;
	char *name;
	pa_proplist *props;
	pa_sample_spec ss;
	pa_channel_map map;
	pa_format_info *format;
	pa_stream_direction_t dir;
	pa_stream_state_t state;
	pa_stream_flags_t flags;
	uint32_t index;
	uint32_t device_index;
	char *device_name;
	pa_buffer_attr attr;
	pa_timing_info timing;

	pa_stream_notify_cb_t state_cb; void *state_ud;
	pa_stream_request_cb_t write_cb; void *write_ud;
	pa_stream_request_cb_t read_cb; void *read_ud;
	pa_stream_notify_cb_t underflow_cb; void *underflow_ud;
	pa_stream_notify_cb_t overflow_cb; void *overflow_ud;
	pa_stream_notify_cb_t latency_cb; void *latency_ud;
	pa_stream_notify_cb_t started_cb; void *started_ud;
	pa_stream_notify_cb_t moved_cb; void *moved_ud;
	pa_stream_notify_cb_t suspended_cb; void *suspended_ud;
	pa_stream_notify_cb_t buffer_attr_cb; void *buffer_attr_ud;
	pa_stream_event_cb_t event_cb; void *event_ud;

	/* JackBus */
	jackbus_node_t *node;
	bool node_ref, attached;
	uint32_t jack_rate;
	unsigned int n_ports;
	jackbus_port_t *ports[PA_CHANNELS_MAX];
	struct shim_resampler *rs;
	double ratio;			/* application frames per JackBus frame */
	/* a peak-detect stream (PA_STREAM_PEAK_DETECT, a level meter's): one value per
	 * peak_step JackBus frames, each channel's largest magnitude over the ports
	 * connected to it (their average: a port sums them, PulseAudio's downmix
	 * averages) */
	bool peak;
	uint32_t peak_step, peak_count;
	float peak_acc[PA_CHANNELS_MAX];
	void *mem;
	size_t mem_size;
	void *ring_mem;			/* the ring in the cycle (an async stream's is JackBus's) */
	size_t ring_size;
	float *ring[PA_CHANNELS_MAX];
	float *tmp[PA_CHANNELS_MAX];	/* the application's rate */
	float *tmp2[PA_CHANNELS_MAX];	/* JackBus's rate, resampled */
	uint8_t *wbuf;			/* begin_write */
	size_t wbuf_size;
	uint8_t *pbuf;			/* peek */
	size_t pbuf_len, pbuf_size;

	/* shared with the realtime side: the ring's positions (JackBus frames), the controls
	 * (prebuffering, flush, drain, run, gains), the counts and the last cycle (a seqlock),
	 * laid out as an async endpoint's (jack/jackbus.h). In the cycle (the node's realtime
	 * thread, stream_process): local, sh = &local. Async (the settings' pa.async): JackBus's
	 * memory, where JackBus itself moves the ring each cycle, and stream_process never runs. */
	jackbus_endpoint_shared_t *sh;
	jackbus_endpoint_shared_t local;
	jackbus_endpoint_t *ep;
	atomic_bool corked, running;	/* the application's: sh->run is running && !corked */

	/* the application's side: woken by a timerfd on its main loop (an io event: no timer
	 * of the loop's, whose resolution may be a millisecond), else a time event */
	int tfd;
	pa_io_event *tfd_io;
	pa_time_event *timer;
	uint64_t written;		/* application frames written */
	uint64_t taken;			/* application frames read */
	unsigned int seen_underflows, seen_overflows;
	bool started_told, first_request;
	uint64_t last_latency_update;
	pa_operation *drain_op;
	pa_cvolume volume;
	int mute;
	uint32_t tlength, minreq, fragsize;	/* JackBus frames */
};

/* ------------------------------------------------------------- helpers */

static const char *position_short(pa_channel_position_t p, char *buf, size_t size)
{
	static const char *const names[] = {
		[PA_CHANNEL_POSITION_MONO] = "MONO",
		[PA_CHANNEL_POSITION_FRONT_LEFT] = "FL",
		[PA_CHANNEL_POSITION_FRONT_RIGHT] = "FR",
		[PA_CHANNEL_POSITION_FRONT_CENTER] = "FC",
		[PA_CHANNEL_POSITION_REAR_CENTER] = "RC",
		[PA_CHANNEL_POSITION_REAR_LEFT] = "RL",
		[PA_CHANNEL_POSITION_REAR_RIGHT] = "RR",
		[PA_CHANNEL_POSITION_LFE] = "LFE",
		[PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER] = "FLC",
		[PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER] = "FRC",
		[PA_CHANNEL_POSITION_SIDE_LEFT] = "SL",
		[PA_CHANNEL_POSITION_SIDE_RIGHT] = "SR",
		[PA_CHANNEL_POSITION_TOP_CENTER] = "TC",
		[PA_CHANNEL_POSITION_TOP_FRONT_LEFT] = "TFL",
		[PA_CHANNEL_POSITION_TOP_FRONT_RIGHT] = "TFR",
		[PA_CHANNEL_POSITION_TOP_FRONT_CENTER] = "TFC",
		[PA_CHANNEL_POSITION_TOP_REAR_LEFT] = "TRL",
		[PA_CHANNEL_POSITION_TOP_REAR_RIGHT] = "TRR",
		[PA_CHANNEL_POSITION_TOP_REAR_CENTER] = "TRC",
	};
	if (p >= PA_CHANNEL_POSITION_AUX0 && p <= PA_CHANNEL_POSITION_AUX31)
		snprintf(buf, size, "AUX%d", (int)(p - PA_CHANNEL_POSITION_AUX0));
	else if (p >= 0 && (size_t)p < sizeof(names) / sizeof(names[0]) && names[p])
		snprintf(buf, size, "%s", names[p]);
	else
		snprintf(buf, size, "CH%d", (int)p);
	return buf;
}

static bool format_supported(pa_sample_format_t f)
{
	switch (f) {
	case PA_SAMPLE_U8:
	case PA_SAMPLE_S16LE:
	case PA_SAMPLE_S24LE:
	case PA_SAMPLE_S24_32LE:
	case PA_SAMPLE_S32LE:
	case PA_SAMPLE_FLOAT32LE:
		return true;
	default:
		return false;
	}
}

static inline float clampf(float v)
{
	return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
}

/* frames of interleaved application samples to per-channel floats */
static void to_float(const pa_stream *s, const uint8_t *src, float *const *dst, uint32_t frames)
{
	unsigned int nc = s->ss.channels;
	for (uint32_t i = 0; i < frames; i++) {
		for (unsigned int ch = 0; ch < nc; ch++) {
			float v;
			switch (s->ss.format) {
			case PA_SAMPLE_U8: v = ((int)*src - 128) * (1.0f / 128.0f); src += 1; break;
			case PA_SAMPLE_S16LE: v = *(const int16_t *)src * (1.0f / 32768.0f); src += 2; break;
			case PA_SAMPLE_S24LE: {
				int32_t x = (int32_t)((uint32_t)src[0] << 8 | (uint32_t)src[1] << 16 | (uint32_t)src[2] << 24) >> 8;
				v = x * (1.0f / 8388608.0f);
				src += 3;
				break;
			}
			case PA_SAMPLE_S24_32LE:
				v = ((int32_t)((uint32_t)*(const int32_t *)src << 8) >> 8) * (1.0f / 8388608.0f);
				src += 4;
				break;
			case PA_SAMPLE_S32LE: v = (float)(*(const int32_t *)src * (1.0 / 2147483648.0)); src += 4; break;
			case PA_SAMPLE_FLOAT32LE: v = *(const float *)src; src += 4; break;
			default: v = 0.0f; break;
			}
			dst[ch][i] = v;
		}
	}
}

static void from_float(const pa_stream *s, float *const *src, uint8_t *dst, uint32_t frames)
{
	unsigned int nc = s->ss.channels;
	for (uint32_t i = 0; i < frames; i++) {
		for (unsigned int ch = 0; ch < nc; ch++) {
			float v = src[ch][i];
			switch (s->ss.format) {
			case PA_SAMPLE_U8: *dst = (uint8_t)lrintf(clampf(v) * 127.0f + 128.0f); dst += 1; break;
			case PA_SAMPLE_S16LE: *(int16_t *)dst = (int16_t)lrintf(clampf(v) * 32767.0f); dst += 2; break;
			case PA_SAMPLE_S24LE: {
				int32_t x = (int32_t)lrintf(clampf(v) * 8388607.0f);
				dst[0] = (uint8_t)x;
				dst[1] = (uint8_t)(x >> 8);
				dst[2] = (uint8_t)(x >> 16);
				dst += 3;
				break;
			}
			case PA_SAMPLE_S24_32LE: *(int32_t *)dst = (int32_t)lrintf(clampf(v) * 8388607.0f); dst += 4; break;
			case PA_SAMPLE_S32LE: *(int32_t *)dst = (int32_t)lrint(clampf(v) * 2147483647.0); dst += 4; break;
			case PA_SAMPLE_FLOAT32LE: *(float *)dst = v; dst += 4; break;
			default: break;
			}
		}
	}
}

static void ring_put(pa_stream *s, uint64_t pos, float *const *src, uint32_t n)
{
	uint32_t idx = pos & (RING_FRAMES - 1), first = SHIM_MIN(n, RING_FRAMES - idx);
	for (unsigned int ch = 0; ch < s->n_ports; ch++) {
		memcpy(s->ring[ch] + idx, src[ch], first * sizeof(float));
		memcpy(s->ring[ch], src[ch] + first, (n - first) * sizeof(float));
	}
}

static void ring_get(pa_stream *s, uint64_t pos, float *const *dst, uint32_t n)
{
	uint32_t idx = pos & (RING_FRAMES - 1), first = SHIM_MIN(n, RING_FRAMES - idx);
	for (unsigned int ch = 0; ch < s->n_ports; ch++) {
		memcpy(dst[ch], s->ring[ch] + idx, first * sizeof(float));
		memcpy(dst[ch] + first, s->ring[ch], (n - first) * sizeof(float));
	}
}

static uint64_t app_frames(const pa_stream *s, uint64_t jack_frames)
{
	return s->rs || s->peak ? (uint64_t)llround((double)jack_frames * s->ratio) : jack_frames;
}

static uint32_t jack_frames(const pa_stream *s, uint64_t app)
{
	return (uint32_t)(s->rs || s->peak ? llround((double)app / s->ratio) : (long long)app);
}

static uint64_t bytes_to_frames(const pa_stream *s, uint64_t bytes)
{
	size_t fs = pa_frame_size(&s->ss);
	return fs ? bytes / fs : 0;
}

/* the realtime side (the node's, or JackBus's for an async stream) moves frames while
 * the stream runs and isn't corked */
static void update_run(pa_stream *s)
{
	atomic_store_explicit(&s->sh->run,
			atomic_load(&s->running) && !atomic_load(&s->corked) ? 1u : 0u, memory_order_release);
}

/* ------------------------------------------------------- realtime side */

static void stream_process(struct shim_member *m, const jackbus_cycle_t *c)
{
	pa_stream *s = (pa_stream *)m;
	uint32_t n = c->nframes;
	float *port[PA_CHANNELS_MAX];
	uint64_t before = atomic_load_explicit(&s->sh->moved, memory_order_relaxed);
	uint32_t moved = 0;

	for (unsigned int ch = 0; ch < s->n_ports; ch++)
		port[ch] = jackbus_port_audio(s->ports[ch]);

	if (s->dir == PA_STREAM_PLAYBACK) {
		/* a flush: what was in the ring then goes, corked or not */
		uint64_t to = atomic_load_explicit(&s->sh->skip_to, memory_order_acquire);
		uint64_t r = atomic_load_explicit(&s->sh->r, memory_order_relaxed);
		if (to > r)
			atomic_store_explicit(&s->sh->r, to, memory_order_release);
	}
	if (!atomic_load_explicit(&s->sh->run, memory_order_acquire)) {
		if (s->dir == PA_STREAM_PLAYBACK)
			for (unsigned int ch = 0; ch < s->n_ports; ch++)
				memset(port[ch], 0, n * sizeof(float));
		goto out;
	}
	if (s->dir == PA_STREAM_PLAYBACK) {
		uint64_t w = atomic_load_explicit(&s->sh->w, memory_order_acquire);
		uint64_t r = atomic_load_explicit(&s->sh->r, memory_order_relaxed);
		uint64_t fill = w - r;
		bool draining = atomic_load_explicit(&s->sh->draining, memory_order_relaxed);
		if (!atomic_load_explicit(&s->sh->started, memory_order_relaxed)) {
			if (fill >= atomic_load_explicit(&s->sh->prebuf, memory_order_relaxed) || draining)
				atomic_store_explicit(&s->sh->started, true, memory_order_release);
		}
		if (atomic_load_explicit(&s->sh->started, memory_order_relaxed)) {
			moved = (uint32_t)SHIM_MIN(fill, (uint64_t)n);
			uint32_t idx = r & (RING_FRAMES - 1), first = SHIM_MIN(moved, RING_FRAMES - idx);
			for (unsigned int ch = 0; ch < s->n_ports; ch++) {
				float g = atomic_load_explicit(&s->sh->gain[ch], memory_order_relaxed);
				const float *a = s->ring[ch] + idx, *b = s->ring[ch];
				for (uint32_t i = 0; i < first; i++)
					port[ch][i] = a[i] * g;
				for (uint32_t i = first; i < moved; i++)
					port[ch][i] = b[i - first] * g;
			}
			if (moved < n && !draining) {
				atomic_fetch_add_explicit(&s->sh->underflows, 1, memory_order_relaxed);
				if (atomic_load_explicit(&s->sh->prebuf, memory_order_relaxed) > 0)
					atomic_store_explicit(&s->sh->started, false, memory_order_relaxed);
			}
		}
		for (unsigned int ch = 0; ch < s->n_ports; ch++)
			memset(port[ch] + moved, 0, (n - moved) * sizeof(float));
		atomic_store_explicit(&s->sh->r, r + moved, memory_order_release);
	} else {
		uint64_t r = atomic_load_explicit(&s->sh->r, memory_order_acquire);
		uint64_t w = atomic_load_explicit(&s->sh->w, memory_order_relaxed);
		if (RING_FRAMES - (w - r) < n) {
			atomic_fetch_add_explicit(&s->sh->overflows, 1, memory_order_relaxed);
			goto out;
		}
		uint32_t idx = w & (RING_FRAMES - 1), first = SHIM_MIN(n, RING_FRAMES - idx);
		for (unsigned int ch = 0; ch < s->n_ports; ch++) {
			float g = atomic_load_explicit(&s->sh->gain[ch], memory_order_relaxed);
			float *a = s->ring[ch] + idx, *b = s->ring[ch];
			for (uint32_t i = 0; i < first; i++)
				a[i] = port[ch][i] * g;
			for (uint32_t i = first; i < n; i++)
				b[i - first] = port[ch][i] * g;
		}
		moved = n;
		atomic_store_explicit(&s->sh->w, w + n, memory_order_release);
	}
out:
	/* the cycle as it went, for interpolation */
	{
		unsigned int q = atomic_load_explicit(&s->sh->seq, memory_order_relaxed);
		atomic_store_explicit(&s->sh->seq, q + 1, memory_order_relaxed);
		atomic_thread_fence(memory_order_release);
		atomic_store_explicit(&s->sh->last_moved, before, memory_order_relaxed);
		atomic_store_explicit(&s->sh->time_ns, c->time_ns, memory_order_relaxed);
		atomic_store_explicit(&s->sh->period_ns, c->period_ns ? c->period_ns :
				(uint64_t)n * 1000000000ull / SHIM_MAX(c->sample_rate, 1u), memory_order_relaxed);
		atomic_store_explicit(&s->sh->last_frames, moved, memory_order_relaxed);
		atomic_store_explicit(&s->sh->seq, q + 2, memory_order_release);
	}
	atomic_store_explicit(&s->sh->moved, before + moved, memory_order_release);
}

/* frames moved by now, interpolated within the last cycle */
static uint64_t moved_now(pa_stream *s, uint64_t *period_ns, uint64_t *cycle_ns)
{
	uint64_t before = 0, t_ns = 0, period = 0;
	uint32_t n = 0;
	for (int i = 0; i < 100; i++) {
		unsigned int a = atomic_load_explicit(&s->sh->seq, memory_order_acquire);
		if (a & 1)
			continue;
		before = atomic_load_explicit(&s->sh->last_moved, memory_order_relaxed);
		t_ns = atomic_load_explicit(&s->sh->time_ns, memory_order_relaxed);
		period = atomic_load_explicit(&s->sh->period_ns, memory_order_relaxed);
		n = atomic_load_explicit(&s->sh->last_frames, memory_order_relaxed);
		atomic_thread_fence(memory_order_acquire);
		if (atomic_load_explicit(&s->sh->seq, memory_order_relaxed) == a)
			break;
	}
	if (period_ns)
		*period_ns = period;
	if (cycle_ns)
		*cycle_ns = t_ns;
	if (t_ns == 0 || period == 0)
		return atomic_load(&s->sh->moved);
	uint64_t now = shim_now_usec() * 1000ull;
	uint64_t into = now > t_ns ? now - t_ns : 0;
	uint64_t frames = into >= period ? n : (uint64_t)((double)n * (double)into / (double)period);
	return before + SHIM_MIN(frames, (uint64_t)n);
}

/* ------------------------------------------------------ the application's side */

static void set_state(pa_stream *s, pa_stream_state_t st)
{
	if (s->state == st)
		return;
	pa_stream_ref(s);
	s->state = st;
	if (s->state_cb)
		s->state_cb(s, s->state_ud);
	pa_stream_unref(s);
}

static pa_usec_t port_latency_usec(pa_stream *s)
{
	jack_latency_range_t r = { 0, 0 };
	if (s->n_ports == 0 || s->jack_rate == 0)
		return 0;
	jack_port_get_latency_range(jackbus_port_jack(s->ports[0]),
			s->dir == PA_STREAM_PLAYBACK ? JackPlaybackLatency : JackCaptureLatency, &r);
	return (pa_usec_t)r.max * 1000000ull / s->jack_rate;
}

/* playback: application frames written and not yet played */
static uint64_t queued_app(pa_stream *s)
{
	uint64_t played = app_frames(s, atomic_load_explicit(&s->sh->moved, memory_order_acquire));
	return s->written > played ? s->written - played : 0;
}

static size_t writable_bytes(pa_stream *s)
{
	uint64_t target = app_frames(s, s->tlength);
	uint64_t q = queued_app(s);
	return q >= target ? 0 : (size_t)(target - q) * pa_frame_size(&s->ss);
}

static size_t readable_bytes(pa_stream *s)
{
	uint64_t fill = atomic_load_explicit(&s->sh->w, memory_order_acquire) - atomic_load_explicit(&s->sh->r, memory_order_relaxed);
	return (size_t)app_frames(s, fill) * pa_frame_size(&s->ss) + s->pbuf_len;
}

/* cycles (of period_ns) until the application has something to do: a record stream's
 * fragment there, a playback stream's minreq free; 1 while it waits on the application
 * (data not taken, not written), corked, prebuffering or draining */
static uint64_t cycles_to_work(pa_stream *s, uint64_t period_ns)
{
	uint64_t q = (period_ns * s->jack_rate + 500000000ull) / 1000000000ull;
	if (q == 0 || atomic_load(&s->corked) || s->drain_op)
		return 1;
	if (s->dir == PA_STREAM_RECORD) {
		uint64_t fill = atomic_load_explicit(&s->sh->w, memory_order_acquire) -
				atomic_load_explicit(&s->sh->r, memory_order_relaxed);
		uint64_t frag = SHIM_MIN((uint64_t)s->fragsize, (uint64_t)RING_FRAMES / 2);
		if (s->pbuf_len || fill >= frag)
			return 1;
		return (frag - fill + q - 1) / q;
	}
	if (!atomic_load(&s->sh->started))
		return 1;
	uint64_t queued = jack_frames(s, queued_app(s));
	uint64_t low = s->tlength > s->minreq ? s->tlength - s->minreq : 0;
	if (queued <= low)
		return 1;
	return (queued - low + q - 1) / q;
}

static void set_wake(pa_stream *s, uint64_t usec)
{
	if (s->tfd >= 0) {
		struct itimerspec it;
		memset(&it, 0, sizeof(it));
		it.it_value.tv_sec = (time_t)(usec / 1000000ull);
		it.it_value.tv_nsec = (long)(usec % 1000000ull) * 1000;
		timerfd_settime(s->tfd, TFD_TIMER_ABSTIME, &it, NULL);
	} else if (s->timer) {
		pa_context_rttime_restart(s->context, s->timer, usec);
	}
}

/* the application's side wakes when it has something to do, a quarter period after the
 * cycle that brings it; at most 100 ms apart (50 with automatic timing updates) */
static void arm_timer(pa_stream *s)
{
	uint64_t period, cycle;
	moved_now(s, &period, &cycle);
	uint64_t now = shim_now_usec();
	uint64_t next;
	if (period == 0 || cycle == 0) {
		next = now + 2000;
	} else {
		uint64_t period_us = SHIM_MAX(period / 1000, 500ull);
		next = cycle / 1000 + cycles_to_work(s, period) * period_us + period_us / 4;
		while (next <= now)
			next += period_us;
		uint64_t cap = now + ((s->flags & PA_STREAM_AUTO_TIMING_UPDATE) && s->latency_cb ? 50000 : 100000);
		next = SHIM_MIN(next, cap);
	}
	set_wake(s, next);
}

static void update_timing(pa_stream *s)
{
	pa_timing_info *t = &s->timing;
	size_t fs = pa_frame_size(&s->ss);
	memset(t, 0, sizeof(*t));
	pa_gettimeofday(&t->timestamp);
	t->synchronized_clocks = 1;
	if (s->dir == PA_STREAM_PLAYBACK) {
		t->sink_usec = port_latency_usec(s);
		t->playing = atomic_load(&s->sh->started) && !atomic_load(&s->corked);
		t->write_index = (int64_t)(s->written * fs);
		t->read_index = (int64_t)(app_frames(s, moved_now(s, NULL, NULL)) * fs);
	} else {
		t->source_usec = port_latency_usec(s);
		t->playing = !atomic_load(&s->corked);
		t->write_index = (int64_t)(app_frames(s, atomic_load(&s->sh->moved)) * fs);
		t->read_index = (int64_t)(s->taken * fs);
	}
	t->configured_sink_usec = t->sink_usec;
	t->configured_source_usec = t->source_usec;
}

static void stream_tick(pa_stream *s)
{
	if (s->state != PA_STREAM_READY)
		return;
	pa_stream_ref(s);
	if (s->dir == PA_STREAM_PLAYBACK) {
		size_t wb = writable_bytes(s);
		size_t minreq = (size_t)app_frames(s, s->minreq) * pa_frame_size(&s->ss);
		if (s->write_cb && wb > 0 && (wb >= minreq || s->first_request)) {
			s->first_request = false;
			s->write_cb(s, wb, s->write_ud);
		}
		unsigned int u = atomic_load(&s->sh->underflows);
		if (u != s->seen_underflows && s->state == PA_STREAM_READY) {
			s->seen_underflows = u;
			s->started_told = false;
			shim_log("%s: underflow (%u in all; written %llu, played %llu frames)", s->name, u,
					(unsigned long long)s->written, (unsigned long long)app_frames(s, atomic_load(&s->sh->moved)));
			if (s->underflow_cb)
				s->underflow_cb(s, s->underflow_ud);
		}
		if (!s->started_told && atomic_load(&s->sh->started) && s->state == PA_STREAM_READY) {
			s->started_told = true;
			if (s->started_cb)
				s->started_cb(s, s->started_ud);
		}
		if (s->drain_op && atomic_load(&s->sh->w) == atomic_load(&s->sh->r)) {
			atomic_store(&s->sh->draining, false);
			pa_operation *o = s->drain_op;
			s->drain_op = NULL;
			shim_operation_finish(o, 1);
			pa_operation_unref(o);
		}
	} else {
		size_t rb = readable_bytes(s);
		size_t frag = (size_t)app_frames(s, s->fragsize) * pa_frame_size(&s->ss);
		if (s->read_cb && rb > 0 && rb >= SHIM_MIN(frag, (size_t)app_frames(s, RING_FRAMES / 2) * pa_frame_size(&s->ss)))
			s->read_cb(s, rb, s->read_ud);
		unsigned int ov = atomic_load(&s->sh->overflows);
		if (ov != s->seen_overflows && s->state == PA_STREAM_READY) {
			s->seen_overflows = ov;
			if (s->overflow_cb)
				s->overflow_cb(s, s->overflow_ud);
		}
	}
	uint64_t now = shim_now_usec();
	if ((s->flags & PA_STREAM_AUTO_TIMING_UPDATE) && s->latency_cb && s->state == PA_STREAM_READY &&
	    now - s->last_latency_update >= 50000) {
		s->last_latency_update = now;
		update_timing(s);
		s->latency_cb(s, s->latency_ud);
	}
	if (s->state == PA_STREAM_READY && (s->tfd >= 0 || s->timer))
		arm_timer(s);
	pa_stream_unref(s);
}

static void on_timer(pa_mainloop_api *a, pa_time_event *e, const struct timeval *tv, void *userdata)
{
	stream_tick(userdata);
}

static void on_tfd(pa_mainloop_api *a, pa_io_event *e, int fd, pa_io_event_flags_t f, void *userdata)
{
	uint64_t n;
	ssize_t r = read(fd, &n, sizeof(n));
	(void)r;
	stream_tick(userdata);
}

/* --------------------------------------------------------------- creation */

static pa_stream *stream_new(pa_context *c, const char *name, const pa_sample_spec *ss,
		const pa_channel_map *map, const pa_proplist *p)
{
	if (ss == NULL || !pa_sample_spec_valid(ss)) {
		shim_context_set_errno(c, PA_ERR_INVALID);
		return NULL;
	}
	pa_stream *s = calloc(1, sizeof(*s));
	if (s == NULL)
		return NULL;
	s->refs = 1;
	s->member.process = stream_process;
	s->sh = &s->local;
	s->local.size = sizeof(s->local);
	s->local.ring_frames = RING_FRAMES;
	s->context = c;
	pa_context_ref(c);
	s->name = strdup(name ? name : "stream");
	s->props = p ? pa_proplist_copy(p) : pa_proplist_new();
	if (name && !pa_proplist_contains(s->props, PA_PROP_MEDIA_NAME))
		pa_proplist_sets(s->props, PA_PROP_MEDIA_NAME, name);
	s->ss = *ss;
	if (map && pa_channel_map_valid(map) && map->channels == ss->channels)
		s->map = *map;
	else if (!pa_channel_map_init_auto(&s->map, ss->channels, PA_CHANNEL_MAP_DEFAULT))
		pa_channel_map_init_extend(&s->map, ss->channels, PA_CHANNEL_MAP_DEFAULT);
	s->state = PA_STREAM_UNCONNECTED;
	s->tfd = -1;
	s->index = shim_context_new_stream_index(c);
	s->device_index = PA_INVALID_INDEX;
	pa_cvolume_set(&s->volume, ss->channels, PA_VOLUME_NORM);
	for (unsigned int ch = 0; ch < PA_CHANNELS_MAX; ch++)
		atomic_store(&s->sh->gain[ch], 1.0f);
	return s;
}

SHIM_EXPORT pa_stream *pa_stream_new_with_proplist(pa_context *c, const char *name,
		const pa_sample_spec *ss, const pa_channel_map *map, pa_proplist *p)
{
	return stream_new(c, name, ss, map, p);
}

SHIM_EXPORT pa_stream *pa_stream_new(pa_context *c, const char *name, const pa_sample_spec *ss,
		const pa_channel_map *map)
{
	return stream_new(c, name, ss, map, NULL);
}

SHIM_EXPORT pa_stream *pa_stream_new_extended(pa_context *c, const char *name,
		pa_format_info *const *formats, unsigned int n_formats, pa_proplist *p)
{
	for (unsigned int i = 0; i < n_formats; i++) {
		pa_sample_spec ss;
		pa_channel_map map;
		if (formats[i] == NULL || !pa_format_info_is_pcm(formats[i]))
			continue;
		if (pa_format_info_to_sample_spec(formats[i], &ss, &map) < 0)
			continue;
		return stream_new(c, name, &ss, &map, p);
	}
	shim_context_set_errno(c, PA_ERR_NOTSUPPORTED);
	return NULL;
}

static void free_buffers(pa_stream *s)
{
	if (s->mem) {
		munlock(s->mem, s->mem_size);
		free(s->mem);
	}
	s->mem = NULL;
	s->mem_size = 0;
	if (s->ring_mem) {
		munlock(s->ring_mem, s->ring_size);
		free(s->ring_mem);
	}
	s->ring_mem = NULL;
	s->ring_size = 0;
	memset(s->ring, 0, sizeof(s->ring));	/* an endpoint's: unmapped by its close */
	memset(s->tmp, 0, sizeof(s->tmp));
	memset(s->tmp2, 0, sizeof(s->tmp2));
	free(s->wbuf);
	s->wbuf = NULL;
	s->wbuf_size = 0;
	free(s->pbuf);
	s->pbuf = NULL;
	s->pbuf_size = s->pbuf_len = 0;
	shim_resampler_free(s->rs);
	s->rs = NULL;
}

/* the ring, for a stream in the cycle */
static int alloc_ring(pa_stream *s)
{
	s->ring_size = (size_t)s->ss.channels * RING_FRAMES * sizeof(float);
	if (posix_memalign(&s->ring_mem, 64, s->ring_size) != 0) {
		s->ring_mem = NULL;
		return -1;
	}
	memset(s->ring_mem, 0, s->ring_size);
	mlock(s->ring_mem, s->ring_size);
	for (unsigned int ch = 0; ch < s->ss.channels; ch++)
		s->ring[ch] = (float *)s->ring_mem + (size_t)ch * RING_FRAMES;
	return 0;
}

static int alloc_buffers(pa_stream *s, bool with_ring)
{
	if (with_ring && alloc_ring(s) < 0)
		return -1;
	size_t ring = 0;
	size_t tmp = (size_t)CHUNK * sizeof(float), tmp2 = (size_t)RS_OUT * sizeof(float);
	unsigned int nc = s->ss.channels;
	s->mem_size = nc * (ring + tmp + tmp2);
	if (posix_memalign(&s->mem, 64, s->mem_size) != 0) {
		s->mem = NULL;
		return -1;
	}
	memset(s->mem, 0, s->mem_size);
	mlock(s->mem, s->mem_size);
	uint8_t *p = s->mem;
	for (unsigned int ch = 0; ch < nc; ch++) {
		p += ring;
		s->tmp[ch] = (float *)p; p += tmp;
		s->tmp2[ch] = (float *)p; p += tmp2;
	}
	s->pbuf_size = (size_t)RS_OUT * pa_frame_size(&s->ss);
	s->pbuf = malloc(s->pbuf_size);
	return s->pbuf ? 0 : -1;
}

/* the direct route first; then resampling, unless the settings say no */
static bool resampling_allowed(void)
{
	return jackbus_compat_bool("resample", "JACKBUS_PA_RESAMPLE", true);
}

/* the stream's ports to device d's physical ports */
static void connect_to(pa_stream *s, const struct shim_device *d)
{
	jack_client_t *client = shim_node_client();
	if (d == NULL || client == NULL)
		return;
	bool playback = s->dir == PA_STREAM_PLAYBACK;
	const char **ports = jack_get_ports(client, NULL, JACK_DEFAULT_AUDIO_TYPE,
			JackPortIsPhysical | (playback ? JackPortIsInput : JackPortIsOutput));
	const char *mine[PA_CHANNELS_MAX];
	const char *phys[64];
	unsigned int n_phys = 0;
	size_t len = strlen(d->client);
	for (size_t i = 0; ports && ports[i] && n_phys < 64; i++)
		if (strncmp(ports[i], d->client, len) == 0 && ports[i][len] == ':')
			phys[n_phys++] = ports[i];
	for (unsigned int ch = 0; ch < s->n_ports; ch++)
		mine[ch] = jack_port_name(jackbus_port_jack(s->ports[ch]));
	unsigned int n = s->n_ports == 1 ? SHIM_MIN(n_phys, 2u) : SHIM_MIN(n_phys, s->n_ports);
	for (unsigned int i = 0; i < n; i++) {
		unsigned int ch = s->n_ports == 1 ? 0 : i;
		if (playback)
			jack_connect(client, mine[ch], phys[i]);
		else if (i == ch)
			jack_connect(client, phys[i], mine[ch]);
	}
	jack_free(ports);
}

static void autoconnect(pa_stream *s, const struct shim_device *d)
{
	if (jackbus_compat_bool("autoconnect", "JACKBUS_PA_AUTOCONNECT", true))
		connect_to(s, d);
}

static void on_moved(pa_mainloop_api *a, pa_defer_event *e, void *userdata)
{
	pa_stream *s = userdata;
	a->defer_free(e);
	if (s->state == PA_STREAM_READY && s->moved_cb)
		s->moved_cb(s, s->moved_ud);
	pa_stream_unref(s);
}

/* a move (a volume control's): the stream's ports from wherever they are
 * connected to device d's, whatever the autoconnect setting (it is asked for) */
int shim_stream_move(pa_stream *s, const struct shim_device *d)
{
	jack_client_t *client = shim_node_client();
	if (client == NULL || s->state != PA_STREAM_READY || d->playback != (s->dir == PA_STREAM_PLAYBACK))
		return -PA_ERR_INVALID;
	for (unsigned int ch = 0; ch < s->n_ports; ch++)
		jack_port_disconnect(client, jackbus_port_jack(s->ports[ch]));
	connect_to(s, d);
	s->device_index = d->index;
	free(s->device_name);
	s->device_name = strdup(d->name);
	shim_log("%s: moved to %s", s->name ? s->name : "stream", d->name);
	/* the application hears of it from its main loop, as from a server */
	pa_mainloop_api *api = shim_context_api(s->context);
	if (s->moved_cb && api->defer_new(api, on_moved, pa_stream_ref(s)) == NULL)
		pa_stream_unref(s);
	return 0;
}

static void on_ready(pa_mainloop_api *a, pa_defer_event *e, void *userdata)
{
	pa_stream *s = userdata;
	a->defer_free(e);
	if (s->state != PA_STREAM_CREATING) {
		pa_stream_unref(s);
		return;
	}
	set_state(s, PA_STREAM_READY);
	if (s->state == PA_STREAM_READY)
		stream_tick(s);		/* the first request at once */
	pa_stream_unref(s);
}

static void fill_attr(pa_stream *s, const pa_buffer_attr *attr)
{
	uint32_t quantum = shim_node_client() ? jack_get_buffer_size(shim_node_client()) : 256;
	pa_buffer_attr a;
	if (attr)
		a = *attr;
	else
		memset(&a, 0xff, sizeof(a));
	/* libpulse's own defaults */
	if (a.maxlength == (uint32_t)-1)
		a.maxlength = 4 * 1024 * 1024;
	if (a.tlength == (uint32_t)-1)
		a.tlength = (uint32_t)pa_usec_to_bytes(250 * PA_USEC_PER_MSEC, &s->ss);
	if (a.minreq == (uint32_t)-1)
		a.minreq = a.tlength / 5;
	if (a.prebuf == (uint32_t)-1)
		a.prebuf = a.tlength;
	if (a.fragsize == (uint32_t)-1)
		a.fragsize = a.tlength;
	/* in JackBus frames: at least two cycles queued, at most half the ring */
	uint32_t t = jack_frames(s, bytes_to_frames(s, a.tlength));
	if ((s->flags & PA_STREAM_ADJUST_LATENCY) && s->jack_rate) {
		/* tlength is the latency end to end: the device's part is not queued here */
		pa_usec_t dev = port_latency_usec(s);
		uint32_t dev_frames = (uint32_t)(dev * s->jack_rate / 1000000ull);
		t = t > dev_frames ? t - dev_frames : 0;
	}
	/* the settings' cap on what a stream buffers here (max-latency, ms;
	 * 0: what it asks for), never below two cycles */
	long cap_ms = jackbus_compat_int("max-latency", "JACKBUS_PA_MAX_LATENCY", 0);
	uint32_t cap = cap_ms > 0 && s->jack_rate ? (uint32_t)((uint64_t)cap_ms * s->jack_rate / 1000) : UINT32_MAX;
	t = SHIM_MIN(t, cap);
	s->tlength = SHIM_MIN(SHIM_MAX(t, 2 * quantum), RING_FRAMES / 2);
	s->minreq = SHIM_MIN(SHIM_MAX(jack_frames(s, bytes_to_frames(s, a.minreq)), quantum), s->tlength);
	uint32_t pb = jack_frames(s, bytes_to_frames(s, a.prebuf));
	/* resampling, the resampler holds back a few frames of what was written:
	 * the ring alone never reaches tlength, so playback starts a cycle below */
	uint32_t pb_max = s->rs ? (s->tlength > quantum ? s->tlength - quantum : 0) : s->tlength;
	atomic_store(&s->sh->prebuf, SHIM_MIN(pb, pb_max));
	s->fragsize = SHIM_MIN(SHIM_MAX(SHIM_MIN(jack_frames(s, bytes_to_frames(s, a.fragsize)), cap), quantum),
			RING_FRAMES / 2);
	size_t fs = pa_frame_size(&s->ss);
	a.tlength = (uint32_t)(app_frames(s, s->tlength) * fs);
	a.minreq = (uint32_t)(app_frames(s, s->minreq) * fs);
	a.prebuf = (uint32_t)(app_frames(s, atomic_load(&s->sh->prebuf)) * fs);
	a.fragsize = (uint32_t)(app_frames(s, s->fragsize) * fs);
	a.maxlength = SHIM_MAX(a.maxlength, a.tlength);
	s->attr = a;
}

static int stream_connect(pa_stream *s, pa_stream_direction_t dir, const char *dev,
		const pa_buffer_attr *attr, pa_stream_flags_t flags, const pa_cvolume *volume)
{
	pa_context *c = s->context;
	char why[256];
	bool playback = dir == PA_STREAM_PLAYBACK;

	if (s->state != PA_STREAM_UNCONNECTED || pa_context_get_state(c) != PA_CONTEXT_READY) {
		shim_context_set_errno(c, PA_ERR_BADSTATE);
		return -PA_ERR_BADSTATE;
	}
	s->dir = dir;
	s->flags = flags;
	if ((s->node = shim_node_ref(shim_context_app_name(c))) == NULL) {
		shim_context_set_errno(c, PA_ERR_CONNECTIONREFUSED);
		return -PA_ERR_CONNECTIONREFUSED;
	}
	s->node_ref = true;
	jack_client_t *client = shim_node_client();
	s->jack_rate = jack_get_sample_rate(client);

	const struct shim_device *d = NULL;
	if (dev && dev[0]) {
		char *end;
		unsigned long idx = strtoul(dev, &end, 10);
		d = *end == 0 ? shim_context_device(c, playback, NULL, (uint32_t)idx) :
			shim_context_device(c, playback, dev, 0);
	}
	if (d == NULL)
		d = shim_context_default_device(c, playback);
	s->device_index = d ? d->index : PA_INVALID_INDEX;
	free(s->device_name);
	s->device_name = d ? strdup(d->name) : NULL;

	/* the format: PulseAudio's flags take the device's */
	if (flags & PA_STREAM_FIX_FORMAT)
		s->ss.format = PA_SAMPLE_FLOAT32LE;
	if ((flags & PA_STREAM_FIX_CHANNELS) && d && d->channels) {
		s->ss.channels = (uint8_t)SHIM_MIN(d->channels, (uint32_t)PA_CHANNELS_MAX);
		pa_channel_map_init_auto(&s->map, s->ss.channels, PA_CHANNEL_MAP_DEFAULT);
	}
	if (flags & PA_STREAM_FIX_RATE)
		s->ss.rate = s->jack_rate;
	if (!format_supported(s->ss.format)) {
		snprintf(why, sizeof(why), "sample format %s is not converted here",
				pa_sample_format_to_string(s->ss.format));
		goto refuse;
	}
	if (!playback && (flags & PA_STREAM_PEAK_DETECT)) {
		/* peaks at the stream's rate (a meter's 25 Hz), no resampling */
		s->peak = true;
		s->peak_step = s->ss.rate < s->jack_rate ? (uint32_t)lrint((double)s->jack_rate / s->ss.rate) : 1;
		s->ratio = 1.0 / s->peak_step;
		shim_log("%s: peak detect, %u Hz", s->name, s->ss.rate);
	} else if (s->ss.rate != s->jack_rate) {
		if (!resampling_allowed()) {
			snprintf(why, sizeof(why), "rate %u Hz asked, JackBus runs at %u Hz (resampling is off: "
					"resample = no in compat.conf, or JACKBUS_PA_RESAMPLE=0)", s->ss.rate, s->jack_rate);
			goto refuse;
		}
		s->ratio = (double)s->ss.rate / s->jack_rate;
		s->rs = shim_resampler_new(s->ss.channels, playback ? s->ratio : 1.0 / s->ratio, CHUNK, RS_OUT);
		if (s->rs == NULL) {
			snprintf(why, sizeof(why), "no resampler");
			goto refuse;
		}
		shim_log("%s: resampled %u Hz <-> %u Hz (JackBus's)", s->name, s->ss.rate, s->jack_rate);
	}
	s->n_ports = s->ss.channels;
	/* async (the settings' pa.async): JackBus moves the ring itself, the application's
	 * process is never in the graph's cycle */
	bool async = shim_node_async(s->node);
	if (alloc_buffers(s, !async) < 0) {
		snprintf(why, sizeof(why), "out of memory");
		goto refuse;
	}

	/* the ports */
	unsigned long jflags = playback ? JackPortIsOutput : JackPortIsInput;
	for (unsigned int ch = 0; ch < s->n_ports; ch++) {
		char pos[16], name[64];
		position_short(s->map.map[ch], pos, sizeof(pos));
		snprintf(name, sizeof(name), "%s%s", playback ? "" : "in_", pos);
		s->ports[ch] = jackbus_port_add(s->node, name, JACKBUS_AUDIO, jflags);
		if (s->ports[ch] == NULL) {
			snprintf(name, sizeof(name), "%u.%s%s", s->index, playback ? "" : "in_", pos);
			s->ports[ch] = jackbus_port_add(s->node, name, JACKBUS_AUDIO, jflags);
		}
		if (s->ports[ch] == NULL) {
			for (unsigned int i = 0; i < ch; i++)
				jackbus_port_remove(s->ports[i]);
			memset(s->ports, 0, sizeof(s->ports));
			snprintf(why, sizeof(why), "can't register a JACK port");
			goto refuse;
		}
	}
	if (async) {
		s->ep = jackbus_endpoint_open(s->node, s->ports, s->n_ports, RING_FRAMES);
		if (s->ep && jackbus_endpoint_shared(s->ep)->ring_frames != RING_FRAMES) {
			jackbus_endpoint_close(s->ep);
			s->ep = NULL;
		}
		if (s->ep) {
			s->sh = jackbus_endpoint_shared(s->ep);
			for (unsigned int ch = 0; ch < s->n_ports; ch++)
				s->ring[ch] = jackbus_endpoint_ring(s->ep, ch);
		} else {
			/* the server has no endpoints: in the cycle after all */
			shim_warn("%s: no async endpoint (%s): in the graph's cycle", s->name, strerror(errno));
			if (alloc_ring(s) < 0) {
				for (unsigned int i = 0; i < s->n_ports; i++)
					jackbus_port_remove(s->ports[i]);
				memset(s->ports, 0, sizeof(s->ports));
				snprintf(why, sizeof(why), "out of memory");
				goto refuse;
			}
			async = false;
		}
	}
	if (volume && volume->channels)
		shim_stream_set_gain(s, volume, -1);
	fill_attr(s, attr);
	atomic_store(&s->corked, (flags & PA_STREAM_START_CORKED) != 0);
	atomic_store(&s->sh->started, false);
	s->first_request = true;
	s->format = pa_format_info_from_sample_spec(&s->ss, &s->map);
	pa_mainloop_api *api = shim_context_api(c);
	s->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (s->tfd >= 0 && (s->tfd_io = api->io_new(api, s->tfd, PA_IO_EVENT_INPUT, on_tfd, s)) == NULL) {
		close(s->tfd);
		s->tfd = -1;
	}
	if (s->tfd < 0)
		s->timer = pa_context_rttime_new(c, PA_USEC_INVALID, on_timer, s);
	atomic_store_explicit(&s->running, true, memory_order_release);
	update_run(s);
	if (async) {
		shim_node_hold(s->node, true);			/* active, without a realtime thread */
	} else {
		shim_node_need_rt(s->node);
		shim_node_attach(s->node, &s->member);
	}
	s->attached = true;
	autoconnect(s, d);
	shim_log("%s: %s, %s %u Hz %u ch, tlength %u frames (minreq %u, prebuf %u, fragsize %u; asked %d %d %d %d), "
			"flags 0x%x, to %s", s->name,
			playback ? "playback" : "record", pa_sample_format_to_string(s->ss.format),
			s->ss.rate, s->ss.channels, s->tlength, s->minreq, atomic_load(&s->sh->prebuf), s->fragsize,
			attr ? (int)attr->tlength : -2, attr ? (int)attr->minreq : -2, attr ? (int)attr->prebuf : -2,
			attr ? (int)attr->fragsize : -2, (unsigned)flags, d ? d->name : "nothing");

	set_state(s, PA_STREAM_CREATING);
	shim_context_stream_add(c, s);
	pa_stream_ref(s);
	shim_context_api(c)->defer_new(shim_context_api(c), on_ready, s);
	return 0;

refuse:
	shim_warn("%s: %s", s->name, why);
	free_buffers(s);
	if (s->node_ref) {
		s->node_ref = false;
		shim_node_unref(s->node);
	}
	shim_context_set_errno(c, PA_ERR_NOTSUPPORTED);
	set_state(s, PA_STREAM_FAILED);
	return -PA_ERR_NOTSUPPORTED;
}

SHIM_EXPORT int pa_stream_connect_playback(pa_stream *s, const char *dev, const pa_buffer_attr *attr,
		pa_stream_flags_t flags, const pa_cvolume *volume, pa_stream *sync_stream)
{
	(void)sync_stream;
	return stream_connect(s, PA_STREAM_PLAYBACK, dev, attr, flags, volume);
}

SHIM_EXPORT int pa_stream_connect_record(pa_stream *s, const char *dev, const pa_buffer_attr *attr,
		pa_stream_flags_t flags)
{
	return stream_connect(s, PA_STREAM_RECORD, dev, attr, flags, NULL);
}

static void stream_close(pa_stream *s)
{
	if (s->attached) {
		atomic_store(&s->running, false);
		update_run(s);
		if (s->ep) {
			/* JackBus stops moving it; its counts are kept for the log below */
			s->local.underflows = atomic_load(&s->sh->underflows);
			s->local.overflows = atomic_load(&s->sh->overflows);
			jackbus_endpoint_close(s->ep);
			s->ep = NULL;
			s->sh = &s->local;
			memset(s->ring, 0, sizeof(s->ring));
			shim_node_hold(s->node, false);
		} else {
			shim_node_detach(s->node, &s->member);	/* from here no cycle runs it */
		}
		s->attached = false;
	}
	for (unsigned int ch = 0; ch < PA_CHANNELS_MAX; ch++) {
		if (s->ports[ch]) {
			jackbus_port_remove(s->ports[ch]);
			s->ports[ch] = NULL;
		}
	}
	if (s->tfd_io) {
		shim_context_api(s->context)->io_free(s->tfd_io);
		s->tfd_io = NULL;
	}
	if (s->tfd >= 0) {
		close(s->tfd);
		s->tfd = -1;
	}
	if (s->timer) {
		shim_context_api(s->context)->time_free(s->timer);
		s->timer = NULL;
	}
	if (s->drain_op) {
		pa_operation_unref(s->drain_op);
		s->drain_op = NULL;
	}
	shim_operation_cancel_all(s->context, s);
	if (s->format) {
		pa_format_info_free(s->format);
		s->format = NULL;
	}
	free_buffers(s);
	if (s->node_ref) {
		s->node_ref = false;
		shim_node_unref(s->node);
	}
	unsigned int u = atomic_load(&s->sh->underflows), o = atomic_load(&s->sh->overflows);
	if (u || o)
		shim_log("%s: %u underflows, %u overflows", s->name, u, o);
}

/* a disconnected stream's state callback: from the main loop */
static void on_terminated(pa_mainloop_api *a, pa_defer_event *e, void *userdata)
{
	pa_stream *s = userdata;
	a->defer_free(e);
	if (s->state == PA_STREAM_TERMINATED && s->state_cb)
		s->state_cb(s, s->state_ud);
	pa_stream_unref(s);
}

/* The stream stops at once; its state is TERMINATED from here, and the state
 * callback hears of it from the main loop, as PulseAudio's server answers
 * (never inside this call: libcanberra disconnects from a callback holding
 * its own lock). The stream is kept until then. */
SHIM_EXPORT int pa_stream_disconnect(pa_stream *s)
{
	if (s->state != PA_STREAM_READY && s->state != PA_STREAM_CREATING) {
		shim_context_set_errno(s->context, PA_ERR_BADSTATE);
		return -PA_ERR_BADSTATE;
	}
	pa_stream_ref(s);
	stream_close(s);
	shim_context_stream_remove(s->context, s);
	s->state = PA_STREAM_TERMINATED;
	pa_mainloop_api *api = shim_context_api(s->context);
	if (api->defer_new(api, on_terminated, s) == NULL)
		pa_stream_unref(s);
	return 0;
}

void shim_stream_fail(pa_stream *s, int error)
{
	if (s->state != PA_STREAM_READY && s->state != PA_STREAM_CREATING)
		return;
	pa_stream_ref(s);
	stream_close(s);
	shim_context_stream_remove(s->context, s);
	shim_context_set_errno(s->context, error);
	set_state(s, PA_STREAM_FAILED);
	pa_stream_unref(s);
}

SHIM_EXPORT pa_stream *pa_stream_ref(pa_stream *s)
{
	s->refs++;
	return s;
}

SHIM_EXPORT void pa_stream_unref(pa_stream *s)
{
	if (s == NULL || --s->refs > 0)
		return;
	if (s->state == PA_STREAM_READY || s->state == PA_STREAM_CREATING) {
		stream_close(s);
		shim_context_stream_remove(s->context, s);
	}
	if (s->props)
		pa_proplist_free(s->props);
	free(s->device_name);
	free(s->name);
	pa_context_unref(s->context);
	free(s);
}

/* --------------------------------------------------------------- data */

SHIM_EXPORT int pa_stream_begin_write(pa_stream *s, void **data, size_t *nbytes)
{
	if (s->state != PA_STREAM_READY || s->dir != PA_STREAM_PLAYBACK) {
		shim_context_set_errno(s->context, PA_ERR_BADSTATE);
		return -PA_ERR_BADSTATE;
	}
	size_t want = *nbytes;
	if (want == (size_t)-1 || want == 0)
		want = SHIM_MAX(writable_bytes(s), (size_t)app_frames(s, s->minreq) * pa_frame_size(&s->ss));
	size_t fs = pa_frame_size(&s->ss);
	want -= want % fs;
	if (want == 0)
		want = fs;
	if (want > s->wbuf_size) {
		void *p = realloc(s->wbuf, want);
		if (p == NULL) {
			shim_context_set_errno(s->context, PA_ERR_INTERNAL);
			return -PA_ERR_INTERNAL;
		}
		s->wbuf = p;
		s->wbuf_size = want;
	}
	*data = s->wbuf;
	*nbytes = want;
	return 0;
}

SHIM_EXPORT int pa_stream_cancel_write(pa_stream *s)
{
	return 0;
}

static int write_frames(pa_stream *s, const uint8_t *data, uint64_t frames)
{
	size_t fs = pa_frame_size(&s->ss);
	uint64_t done = 0;
	while (done < frames) {
		uint32_t n = (uint32_t)SHIM_MIN(frames - done, (uint64_t)CHUNK);
		to_float(s, data + done * fs, s->tmp, n);
		float *const *src = s->tmp;
		uint32_t out = n;
		if (s->rs) {
			shim_resampler_push(s->rs, (const float *const *)s->tmp, (int)n);
			int got = shim_resampler_pull_available(s->rs, s->tmp2, RS_OUT);
			out = got > 0 ? (uint32_t)got : 0;
			src = s->tmp2;
		}
		uint64_t w = atomic_load_explicit(&s->sh->w, memory_order_relaxed);
		uint64_t r = atomic_load_explicit(&s->sh->r, memory_order_acquire);
		uint32_t room = (uint32_t)(RING_FRAMES - (w - r));
		if (out > room)
			out = room;		/* beyond the ring: dropped (maxlength) */
		if (out) {
			ring_put(s, w, src, out);
			atomic_store_explicit(&s->sh->w, w + out, memory_order_release);
		}
		done += n;
	}
	s->written += frames;
	return 0;
}

SHIM_EXPORT int pa_stream_write_ext_free(pa_stream *s, const void *data, size_t nbytes,
		pa_free_cb_t free_cb, void *free_cb_data, int64_t offset, pa_seek_mode_t seek)
{
	if (s->state != PA_STREAM_READY || s->dir != PA_STREAM_PLAYBACK || (data == NULL && nbytes)) {
		shim_context_set_errno(s->context, PA_ERR_BADSTATE);
		return -PA_ERR_BADSTATE;
	}
	if (seek != PA_SEEK_RELATIVE || offset != 0) {
		static bool logged;
		if (!logged)
			shim_log("%s: a seek (mode %d, offset %lld) is taken as relative", s->name, (int)seek,
					(long long)offset);
		logged = true;
	}
	write_frames(s, data, bytes_to_frames(s, nbytes));
	if (free_cb && data != s->wbuf)
		free_cb(free_cb_data);
	return 0;
}

SHIM_EXPORT int pa_stream_write(pa_stream *s, const void *data, size_t nbytes, pa_free_cb_t free_cb,
		int64_t offset, pa_seek_mode_t seek)
{
	return pa_stream_write_ext_free(s, data, nbytes, free_cb, (void *)data, offset, seek);
}

/* a peak-detect stream's values from n frames in tmp, into tmp2: how many */
static uint32_t peaks(pa_stream *s, uint32_t n)
{
	uint32_t out = 0;
	float div[PA_CHANNELS_MAX];
	for (unsigned int ch = 0; ch < s->n_ports; ch++) {
		int k = jack_port_connected(jackbus_port_jack(s->ports[ch]));
		div[ch] = 1.0f / (float)(k > 1 ? k : 1);
	}
	for (uint32_t i = 0; i < n; i++) {
		for (unsigned int ch = 0; ch < s->n_ports; ch++) {
			float v = fabsf(s->tmp[ch][i]);
			if (v > s->peak_acc[ch])
				s->peak_acc[ch] = v;
		}
		if (++s->peak_count >= s->peak_step) {
			for (unsigned int ch = 0; ch < s->n_ports; ch++) {
				s->tmp2[ch][out] = s->peak_acc[ch] * div[ch];
				s->peak_acc[ch] = 0.0f;
			}
			s->peak_count = 0;
			out++;
		}
	}
	return out;
}

SHIM_EXPORT int pa_stream_peek(pa_stream *s, const void **data, size_t *nbytes)
{
	if (s->state != PA_STREAM_READY || s->dir != PA_STREAM_RECORD) {
		shim_context_set_errno(s->context, PA_ERR_BADSTATE);
		return -PA_ERR_BADSTATE;
	}
	if (s->pbuf_len == 0) {
		uint64_t w = atomic_load_explicit(&s->sh->w, memory_order_acquire);
		uint64_t r = atomic_load_explicit(&s->sh->r, memory_order_relaxed);
		uint32_t n = (uint32_t)SHIM_MIN(w - r, (uint64_t)CHUNK);
		if (n) {
			ring_get(s, r, s->tmp, n);
			atomic_store_explicit(&s->sh->r, r + n, memory_order_release);
			float *const *src = s->tmp;
			uint32_t out = n;
			if (s->peak) {
				out = peaks(s, n);
				src = s->tmp2;
			} else if (s->rs) {
				shim_resampler_push(s->rs, (const float *const *)s->tmp, (int)n);
				int got = shim_resampler_pull_available(s->rs, s->tmp2, RS_OUT);
				out = got > 0 ? (uint32_t)got : 0;
				src = s->tmp2;
			}
			from_float(s, src, s->pbuf, out);
			s->pbuf_len = out * pa_frame_size(&s->ss);
			s->taken += out;
		}
	}
	*data = s->pbuf_len ? s->pbuf : NULL;
	*nbytes = s->pbuf_len;
	return 0;
}

SHIM_EXPORT int pa_stream_drop(pa_stream *s)
{
	if (s->state != PA_STREAM_READY || s->dir != PA_STREAM_RECORD || s->pbuf_len == 0) {
		shim_context_set_errno(s->context, PA_ERR_BADSTATE);
		return -PA_ERR_BADSTATE;
	}
	s->pbuf_len = 0;
	return 0;
}

SHIM_EXPORT size_t pa_stream_writable_size(const pa_stream *cs)
{
	pa_stream *s = (pa_stream *)cs;
	if (s->state != PA_STREAM_READY || s->dir != PA_STREAM_PLAYBACK) {
		shim_context_set_errno(s->context, PA_ERR_BADSTATE);
		return (size_t)-1;
	}
	return writable_bytes(s);
}

SHIM_EXPORT size_t pa_stream_readable_size(const pa_stream *cs)
{
	pa_stream *s = (pa_stream *)cs;
	if (s->state != PA_STREAM_READY || s->dir != PA_STREAM_RECORD) {
		shim_context_set_errno(s->context, PA_ERR_BADSTATE);
		return (size_t)-1;
	}
	return readable_bytes(s);
}

/* --------------------------------------------------------------- control */

static pa_operation *stream_op(pa_stream *s, pa_stream_success_cb_t cb, void *userdata, int success, bool pending)
{
	pa_operation *o = shim_operation_new(s->context, s, NULL, (void *)cb, userdata);
	if (o == NULL)
		return NULL;
	shim_operation_data(o)->success = success;
	if (!pending)
		shim_operation_finish(o, success);
	return o;
}

SHIM_EXPORT pa_operation *pa_stream_drain(pa_stream *s, pa_stream_success_cb_t cb, void *userdata)
{
	if (s->state != PA_STREAM_READY || s->dir != PA_STREAM_PLAYBACK) {
		shim_context_set_errno(s->context, PA_ERR_BADSTATE);
		return NULL;
	}
	if (s->drain_op) {
		shim_operation_finish(s->drain_op, 0);
		pa_operation_unref(s->drain_op);
	}
	atomic_store(&s->sh->draining, true);
	pa_operation *o = stream_op(s, cb, userdata, 1, true);
	if (o)
		s->drain_op = pa_operation_ref(o);
	return o;
}

SHIM_EXPORT pa_operation *pa_stream_flush(pa_stream *s, pa_stream_success_cb_t cb, void *userdata)
{
	if (s->state != PA_STREAM_READY) {
		shim_context_set_errno(s->context, PA_ERR_BADSTATE);
		return NULL;
	}
	if (s->dir == PA_STREAM_PLAYBACK) {
		atomic_store(&s->sh->skip_to, atomic_load(&s->sh->w));
		atomic_store(&s->sh->started, false);
		s->written = app_frames(s, atomic_load(&s->sh->moved)) + 0;
		if (s->rs)
			shim_resampler_reset(s->rs);
		s->first_request = true;
	} else {
		atomic_store(&s->sh->r, atomic_load(&s->sh->w));
		s->pbuf_len = 0;
	}
	shim_log("%s: flush", s->name);
	return stream_op(s, cb, userdata, 1, false);
}

SHIM_EXPORT pa_operation *pa_stream_cork(pa_stream *s, int b, pa_stream_success_cb_t cb, void *userdata)
{
	if (s->state != PA_STREAM_READY) {
		shim_context_set_errno(s->context, PA_ERR_BADSTATE);
		return NULL;
	}
	atomic_store(&s->corked, b != 0);
	update_run(s);
	shim_log("%s: %s", s->name, b ? "cork" : "uncork");
	return stream_op(s, cb, userdata, 1, false);
}

SHIM_EXPORT int pa_stream_is_corked(const pa_stream *s)
{
	return atomic_load(&((pa_stream *)s)->corked);
}

SHIM_EXPORT pa_operation *pa_stream_trigger(pa_stream *s, pa_stream_success_cb_t cb, void *userdata)
{
	if (s->state != PA_STREAM_READY || s->dir != PA_STREAM_PLAYBACK) {
		shim_context_set_errno(s->context, PA_ERR_BADSTATE);
		return NULL;
	}
	atomic_store(&s->sh->started, true);
	return stream_op(s, cb, userdata, 1, false);
}

SHIM_EXPORT pa_operation *pa_stream_prebuf(pa_stream *s, pa_stream_success_cb_t cb, void *userdata)
{
	if (s->state != PA_STREAM_READY || s->dir != PA_STREAM_PLAYBACK) {
		shim_context_set_errno(s->context, PA_ERR_BADSTATE);
		return NULL;
	}
	atomic_store(&s->sh->started, false);
	return stream_op(s, cb, userdata, 1, false);
}

SHIM_EXPORT pa_operation *pa_stream_set_name(pa_stream *s, const char *name, pa_stream_success_cb_t cb,
		void *userdata)
{
	free(s->name);
	s->name = strdup(name ? name : "stream");
	return stream_op(s, cb, userdata, 1, false);
}

SHIM_EXPORT pa_operation *pa_stream_set_buffer_attr(pa_stream *s, const pa_buffer_attr *attr,
		pa_stream_success_cb_t cb, void *userdata)
{
	if (s->state != PA_STREAM_READY) {
		shim_context_set_errno(s->context, PA_ERR_BADSTATE);
		return NULL;
	}
	fill_attr(s, attr);
	shim_log("%s: buffer attr: tlength %u minreq %u fragsize %u (asked %d %d %d)", s->name, s->tlength, s->minreq,
			s->fragsize, attr ? (int)attr->tlength : -2, attr ? (int)attr->minreq : -2, attr ? (int)attr->fragsize : -2);
	return stream_op(s, cb, userdata, 1, false);
}

SHIM_EXPORT pa_operation *pa_stream_update_sample_rate(pa_stream *s, uint32_t rate,
		pa_stream_success_cb_t cb, void *userdata)
{
	return stream_op(s, cb, userdata, rate == s->ss.rate, false);
}

SHIM_EXPORT pa_operation *pa_stream_proplist_update(pa_stream *s, pa_update_mode_t mode, pa_proplist *p,
		pa_stream_success_cb_t cb, void *userdata)
{
	if (p)
		pa_proplist_update(s->props, mode, p);
	return stream_op(s, cb, userdata, 1, false);
}

SHIM_EXPORT pa_operation *pa_stream_proplist_remove(pa_stream *s, const char *const keys[],
		pa_stream_success_cb_t cb, void *userdata)
{
	return stream_op(s, cb, userdata, 1, false);
}

/* the timing update arrives as from a server: later, on the main loop, the
 * latency update callback with it (applications wait for that one: mpv
 * waits with no callback of its own) */
static void run_timing_update(pa_operation *o)
{
	pa_stream *s = shim_operation_stream(o);
	void *ud;
	pa_stream_success_cb_t cb = (pa_stream_success_cb_t)shim_operation_callback(o, &ud);
	if (s->state == PA_STREAM_READY) {
		update_timing(s);
		if (s->latency_cb)
			s->latency_cb(s, s->latency_ud);
	}
	if (cb)
		cb(s, s->state == PA_STREAM_READY, ud);
}

SHIM_EXPORT pa_operation *pa_stream_update_timing_info(pa_stream *s, pa_stream_success_cb_t cb, void *userdata)
{
	if (s->state != PA_STREAM_READY) {
		shim_context_set_errno(s->context, PA_ERR_BADSTATE);
		return NULL;
	}
	return shim_operation_new(s->context, s, run_timing_update, (void *)cb, userdata);
}

/* --------------------------------------------------------------- timing */

SHIM_EXPORT const pa_timing_info *pa_stream_get_timing_info(pa_stream *s)
{
	if (s->state != PA_STREAM_READY) {
		shim_context_set_errno(s->context, PA_ERR_BADSTATE);
		return NULL;
	}
	update_timing(s);
	return &s->timing;
}

SHIM_EXPORT int pa_stream_get_time(pa_stream *s, pa_usec_t *r_usec)
{
	if (s->state != PA_STREAM_READY) {
		shim_context_set_errno(s->context, PA_ERR_BADSTATE);
		return -PA_ERR_BADSTATE;
	}
	uint64_t rate = s->ss.rate ? s->ss.rate : 48000;
	pa_usec_t lat = port_latency_usec(s);
	if (s->dir == PA_STREAM_PLAYBACK) {
		/* the frame being heard: played, less the way to the device */
		pa_usec_t played = app_frames(s, moved_now(s, NULL, NULL)) * 1000000ull / rate;
		*r_usec = played > lat ? played - lat : 0;
	} else {
		/* the frame read, and the way it came */
		*r_usec = s->taken * 1000000ull / rate + lat;
	}
	return 0;
}

SHIM_EXPORT int pa_stream_get_latency(pa_stream *s, pa_usec_t *r_usec, int *negative)
{
	if (s->state != PA_STREAM_READY) {
		shim_context_set_errno(s->context, PA_ERR_BADSTATE);
		return -PA_ERR_BADSTATE;
	}
	uint64_t rate = s->ss.rate ? s->ss.rate : 48000;
	uint64_t frames;
	if (s->dir == PA_STREAM_PLAYBACK) {
		uint64_t played = app_frames(s, moved_now(s, NULL, NULL));
		frames = s->written > played ? s->written - played : 0;
	} else {
		uint64_t captured = app_frames(s, moved_now(s, NULL, NULL));
		frames = captured > s->taken ? captured - s->taken : 0;
	}
	*r_usec = frames * 1000000ull / rate + port_latency_usec(s);
	if (negative)
		*negative = 0;
	return 0;
}

pa_usec_t shim_stream_latency_usec(pa_stream *s)
{
	pa_usec_t u = 0;
	if (s->state == PA_STREAM_READY)
		pa_stream_get_latency(s, &u, NULL);
	return u;
}

/* --------------------------------------------------------------- volume */

void shim_stream_set_gain(pa_stream *s, const pa_cvolume *v, int mute)
{
	if (v && v->channels) {
		s->volume = *v;
		if (v->channels != s->ss.channels)
			pa_cvolume_set(&s->volume, s->ss.channels, pa_cvolume_avg(v));
	}
	if (mute >= 0)
		s->mute = mute;
	for (unsigned int ch = 0; ch < s->ss.channels && ch < PA_CHANNELS_MAX; ch++) {
		float g = s->mute ? 0.0f : (float)pa_sw_volume_to_linear(s->volume.values[ch]);
		atomic_store(&s->sh->gain[ch], g);
	}
}

void shim_stream_get_volume(pa_stream *s, pa_cvolume *v, int *mute)
{
	*v = s->volume;
	*mute = s->mute;
}

/* --------------------------------------------------------------- accessors */

bool shim_stream_playback(pa_stream *s) { return s->dir == PA_STREAM_PLAYBACK; }
uint32_t shim_stream_index(pa_stream *s) { return s->index; }
const char *shim_stream_name(pa_stream *s) { return s->name; }
const pa_sample_spec *shim_stream_spec(pa_stream *s) { return &s->ss; }
const pa_channel_map *shim_stream_map(pa_stream *s) { return &s->map; }
uint32_t shim_stream_device(pa_stream *s) { return s->device_index; }

SHIM_EXPORT pa_stream_state_t pa_stream_get_state(const pa_stream *s) { return s->state; }
SHIM_EXPORT pa_context *pa_stream_get_context(const pa_stream *s) { return s->context; }
SHIM_EXPORT uint32_t pa_stream_get_index(const pa_stream *s) { return s->index; }
SHIM_EXPORT uint32_t pa_stream_get_device_index(const pa_stream *s) { return s->device_index; }
SHIM_EXPORT const char *pa_stream_get_device_name(const pa_stream *s) { return s->device_name; }
SHIM_EXPORT int pa_stream_is_suspended(const pa_stream *s) { return 0; }
SHIM_EXPORT const pa_sample_spec *pa_stream_get_sample_spec(pa_stream *s) { return &s->ss; }
SHIM_EXPORT const pa_channel_map *pa_stream_get_channel_map(pa_stream *s) { return &s->map; }
SHIM_EXPORT const pa_format_info *pa_stream_get_format_info(const pa_stream *s) { return s->format; }
SHIM_EXPORT const pa_buffer_attr *pa_stream_get_buffer_attr(pa_stream *s) { return &s->attr; }
SHIM_EXPORT uint32_t pa_stream_get_monitor_stream(const pa_stream *s) { return PA_INVALID_INDEX; }

SHIM_EXPORT int pa_stream_set_monitor_stream(pa_stream *s, uint32_t sink_input_idx)
{
	shim_context_set_errno(s->context, PA_ERR_NOTSUPPORTED);
	return -PA_ERR_NOTSUPPORTED;
}

#define SETTER(what, type, cbf, udf) \
	SHIM_EXPORT void pa_stream_set_##what##_callback(pa_stream *s, type cb, void *userdata) \
	{ s->cbf = cb; s->udf = userdata; }

SETTER(state, pa_stream_notify_cb_t, state_cb, state_ud)
SETTER(write, pa_stream_request_cb_t, write_cb, write_ud)
SETTER(read, pa_stream_request_cb_t, read_cb, read_ud)
SETTER(overflow, pa_stream_notify_cb_t, overflow_cb, overflow_ud)
SETTER(underflow, pa_stream_notify_cb_t, underflow_cb, underflow_ud)
SETTER(started, pa_stream_notify_cb_t, started_cb, started_ud)
SETTER(latency_update, pa_stream_notify_cb_t, latency_cb, latency_ud)
SETTER(moved, pa_stream_notify_cb_t, moved_cb, moved_ud)
SETTER(suspended, pa_stream_notify_cb_t, suspended_cb, suspended_ud)
SETTER(event, pa_stream_event_cb_t, event_cb, event_ud)
SETTER(buffer_attr, pa_stream_notify_cb_t, buffer_attr_cb, buffer_attr_ud)

SHIM_EXPORT int64_t pa_stream_get_underflow_index(const pa_stream *s)
{
	return -1;
}
