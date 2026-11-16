/*
 * jackbus-alsa: JackBus's own ALSA plugin. An ALSA application's PCM goes
 * straight to JackBus, the way pipewire-alsa takes it to PipeWire: no
 * PipeWire, no loopback card, no resampling.
 *
 *   pcm.jackbus { type jackbus }          (aplay -D jackbus ..., or the default)
 *
 * Each open PCM is a node of JackBus's native API (jack/jackbus.h): a JACK
 * client named after the application, with one port per channel (out_1..N for
 * playback, in_1..N for capture), in the graph like any other client,
 * removed by JackBus's own death detection if the application dies.
 *
 * The application's buffer is the ring (alsa-lib's, mmap_rw). Each cycle the
 * node's realtime thread moves one cycle of frames between the ring and the
 * ports, converting the sample format at the boundary (float, S16, S24 in 32,
 * S32). That is all it does: no lock, no system call, no wakeup. The
 * application is woken by a timer on its side, armed from the published
 * cycle clock for when enough frames will have moved. Positions are JackBus's
 * frames, so they are exact: the delay the application reads is the ring's
 * fill plus the ports' JACK latency, and a cycle the ring couldn't fill (or
 * empty) is an xrun, reported as ALSA reports one (EPIPE).
 *
 * The rate: JackBus's for an application that asks for it (the direct
 * route). Another rate (a fifth of JackBus's to five times it) is resampled
 * in the realtime thread by JackBus's bridge resampler, a fallback, unless
 * the settings say no (~/.config/jackbus/compat.conf "resample = no", or
 * JACKBUS_ALSA_RESAMPLE=0): then the rate is JackBus's only, as on a card at
 * a fixed rate.
 *
 * Options: name "<client name>" (default: the program's name, or
 * JACKBUS_ALSA_NAME), server "<JACK server>", autoconnect <bool> (default
 * yes, JACKBUS_ALSA_AUTOCONNECT=0: no): the ports go to the physical ports in
 * order (a mono stream to the first two); fallback "<PCM>": opened instead
 * when JackBus isn't running (pcm.!default { type jackbus fallback "pipewire" }:
 * ALSA applications on JackBus while it runs, on PipeWire while it doesn't).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/timerfd.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include <jack/jack.h>
#include <jack/jackbus.h>

#include "../compat/compat_conf.h"
#include "../compat/server.h"
#include "../pw-shim/resample.h"

#define MAX_CHANNELS 64
#define MIN_U(a, b) ((a) < (b) ? (a) : (b))
#define N_ELEMENTS(a) (sizeof(a) / sizeof((a)[0]))

struct jb {
	snd_pcm_ioplug_t io;
	jackbus_node_t *node;
	jack_client_t *client;
	bool playback;
	bool autoconnect, connected, activated;
	unsigned int rate;
	int timer_fd, event_fd;
	unsigned int n_ports;
	jackbus_port_t *ports[MAX_CHANNELS];

	/* the stream as set up: written while the realtime side doesn't read it */
	unsigned int channels;
	snd_pcm_format_t format;
	snd_pcm_uframes_t buffer_size, boundary, avail_min;
	const snd_pcm_channel_area_t *areas;

	/* shared with the realtime thread */
	atomic_bool ready;		/* ports and areas valid for it */
	atomic_bool running;		/* started: it moves frames */
	atomic_bool xrun;
	atomic_bool dead;		/* the server went away */
	atomic_uint seq;		/* odd while process runs */
	_Atomic uint64_t hw;		/* the realtime side's position, modulo boundary */
	_Atomic uint64_t next_ns, period_ns;
	_Atomic uint64_t xruns;

	/* the fallback: the application's rate isn't JackBus's (set up in
	 * hw_params, used by the realtime side) */
	bool resample;			/* allowed: settings */
	struct shim_resampler *rs;
	float *tmp[MAX_CHANNELS];
	void *tmp_mem;
	size_t tmp_size;
	uint32_t tmp_frames;
};

/* ------------------------------------------------------- realtime side */

static inline char *area_at(const snd_pcm_channel_area_t *a, snd_pcm_uframes_t frame)
{
	return (char *)a->addr + (a->first + frame * a->step) / 8;
}

static inline float clampf(float v)
{
	return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
}

/* frames [pos, pos + n) of the ring (no wrap) to dst */
static void ring_to_float(const struct jb *p, unsigned int ch, snd_pcm_uframes_t pos, float *dst, uint32_t n)
{
	const snd_pcm_channel_area_t *a = &p->areas[ch];
	const char *src = area_at(a, pos);
	size_t step = a->step / 8;

	switch (p->format) {
	case SND_PCM_FORMAT_FLOAT_LE:
		for (uint32_t i = 0; i < n; i++, src += step)
			dst[i] = *(const float *)src;
		break;
	case SND_PCM_FORMAT_S16_LE:
		for (uint32_t i = 0; i < n; i++, src += step)
			dst[i] = *(const int16_t *)src * (1.0f / 32768.0f);
		break;
	case SND_PCM_FORMAT_S24_LE:
		for (uint32_t i = 0; i < n; i++, src += step)
			dst[i] = ((int32_t)((uint32_t)*(const int32_t *)src << 8) >> 8) * (1.0f / 8388608.0f);
		break;
	case SND_PCM_FORMAT_S32_LE:
		for (uint32_t i = 0; i < n; i++, src += step)
			dst[i] = (float)(*(const int32_t *)src * (1.0 / 2147483648.0));
		break;
	default:
		memset(dst, 0, n * sizeof(float));
		break;
	}
}

static void float_to_ring(const struct jb *p, unsigned int ch, snd_pcm_uframes_t pos, const float *src, uint32_t n)
{
	const snd_pcm_channel_area_t *a = &p->areas[ch];
	char *dst = area_at(a, pos);
	size_t step = a->step / 8;

	switch (p->format) {
	case SND_PCM_FORMAT_FLOAT_LE:
		for (uint32_t i = 0; i < n; i++, dst += step)
			*(float *)dst = src[i];
		break;
	case SND_PCM_FORMAT_S16_LE:
		for (uint32_t i = 0; i < n; i++, dst += step)
			*(int16_t *)dst = (int16_t)lrintf(clampf(src[i]) * 32767.0f);
		break;
	case SND_PCM_FORMAT_S24_LE:
		for (uint32_t i = 0; i < n; i++, dst += step)
			*(int32_t *)dst = (int32_t)lrintf(clampf(src[i]) * 8388607.0f);
		break;
	case SND_PCM_FORMAT_S32_LE:
		for (uint32_t i = 0; i < n; i++, dst += step)
			*(int32_t *)dst = (int32_t)lrint(clampf(src[i]) * 2147483647.0);
		break;
	default:
		break;
	}
}

/* n frames between the ring at position hw and the ports from offset off,
 * in at most two pieces (the ring wraps at buffer_size) */
static void move(struct jb *p, snd_pcm_uframes_t hw, uint32_t off, uint32_t n)
{
	snd_pcm_uframes_t pos = hw % p->buffer_size;
	uint32_t first = (uint32_t)MIN_U((snd_pcm_uframes_t)n, p->buffer_size - pos);

	for (unsigned int ch = 0; ch < p->channels; ch++) {
		float *port = jackbus_port_audio(p->ports[ch]);
		if (port == NULL)
			continue;
		if (p->playback) {
			ring_to_float(p, ch, pos, port + off, first);
			if (n > first)
				ring_to_float(p, ch, 0, port + off + first, n - first);
		} else {
			float_to_ring(p, ch, pos, port + off, first);
			if (n > first)
				float_to_ring(p, ch, 0, port + off + first, n - first);
		}
	}
}

/* resampling: one cycle of JackBus's frames from (or to) the ring at the
 * application's rate; returns the application frames the ring moved by, and
 * *whole says whether the cycle was served whole */
static uint32_t move_resampled(struct jb *p, snd_pcm_uframes_t hw, uint32_t n, bool *whole)
{
	snd_pcm_uframes_t avail = snd_pcm_ioplug_hw_avail(&p->io, hw, p->io.appl_ptr);
	snd_pcm_uframes_t pos = hw % p->buffer_size;
	float *port[MAX_CHANNELS];
	for (unsigned int ch = 0; ch < p->channels; ch++)
		port[ch] = jackbus_port_audio(p->ports[ch]);

	if (p->playback) {
		uint32_t need = (uint32_t)shim_resampler_needed(p->rs, (int)n);
		uint32_t take = (uint32_t)MIN_U(MIN_U((snd_pcm_uframes_t)need, avail), (snd_pcm_uframes_t)p->tmp_frames);
		if (take) {
			uint32_t first = (uint32_t)MIN_U((snd_pcm_uframes_t)take, p->buffer_size - pos);
			for (unsigned int ch = 0; ch < p->channels; ch++) {
				ring_to_float(p, ch, pos, p->tmp[ch], first);
				if (take > first)
					ring_to_float(p, ch, 0, p->tmp[ch] + first, take - first);
			}
			shim_resampler_push(p->rs, (const float *const *)p->tmp, (int)take);
		}
		*whole = shim_resampler_pull(p->rs, port, (int)n) == (int)n;
		return take;
	}
	shim_resampler_push(p->rs, (const float *const *)port, (int)n);
	int got = shim_resampler_pull_available(p->rs, p->tmp, (int)p->tmp_frames);
	uint32_t put = got > 0 ? (uint32_t)MIN_U((snd_pcm_uframes_t)got, avail) : 0;
	if (put) {
		uint32_t first = (uint32_t)MIN_U((snd_pcm_uframes_t)put, p->buffer_size - pos);
		for (unsigned int ch = 0; ch < p->channels; ch++) {
			float_to_ring(p, ch, pos, p->tmp[ch], first);
			if (put > first)
				float_to_ring(p, ch, 0, p->tmp[ch] + first, put - first);
		}
	}
	*whole = got >= 0 && (uint32_t)got == put;
	return put;
}

static int jb_process(jackbus_node_t *node, const jackbus_cycle_t *c, void *arg)
{
	struct jb *p = arg;
	uint32_t n = c->nframes;

	atomic_fetch_add(&p->seq, 1);
	atomic_store_explicit(&p->next_ns, c->next_ns, memory_order_relaxed);
	atomic_store_explicit(&p->period_ns, c->period_ns, memory_order_relaxed);
	if (!atomic_load(&p->ready))
		goto out;

	uint32_t moved = 0;
	if (atomic_load_explicit(&p->running, memory_order_acquire)) {
		snd_pcm_uframes_t hw = atomic_load_explicit(&p->hw, memory_order_relaxed);
		bool whole;
		uint32_t advance;
		if (p->rs) {
			advance = move_resampled(p, hw, n, &whole);
			moved = whole ? n : 0;		/* the ports: a whole cycle, or silence */
		} else {
			/* playback: the frames written and not played; capture: the room left */
			snd_pcm_uframes_t avail = snd_pcm_ioplug_hw_avail(&p->io, hw, p->io.appl_ptr);
			moved = (uint32_t)MIN_U(avail, (snd_pcm_uframes_t)n);
			if (moved)
				move(p, hw, 0, moved);
			advance = moved;
			whole = moved == n;
		}
		if (!whole && p->io.state != SND_PCM_STATE_DRAINING) {
			/* the ring couldn't give (or take) a whole cycle: an xrun, and
			 * the positions stay where they are until the application prepares */
			atomic_store_explicit(&p->running, false, memory_order_relaxed);
			atomic_store_explicit(&p->xrun, true, memory_order_release);
			atomic_fetch_add_explicit(&p->xruns, 1, memory_order_relaxed);
		}
		atomic_store_explicit(&p->hw, (hw + advance) % p->boundary, memory_order_release);
	}
	if (p->playback && moved < n) {
		for (unsigned int ch = 0; ch < p->channels; ch++) {
			float *port = jackbus_port_audio(p->ports[ch]);
			if (port)
				memset(port + moved, 0, (n - moved) * sizeof(float));
		}
	}
out:
	atomic_fetch_add(&p->seq, 1);
	return 0;
}

/* ------------------------------------------------- application's side */

/* returns once no cycle is inside process: after clearing ready or running,
 * nothing the realtime side read is read again */
static void wait_idle(struct jb *p)
{
	for (int i = 0; (atomic_load(&p->seq) & 1) && i < 20000; i++) {
		struct timespec ts = { 0, 50 * 1000 };
		nanosleep(&ts, NULL);
	}
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t period_ns_of(struct jb *p)
{
	uint64_t period = atomic_load_explicit(&p->period_ns, memory_order_relaxed);
	if (period == 0 && p->rate)
		period = (uint64_t)jack_get_buffer_size(p->client) * 1000000000ull / p->rate;
	return period ? period : 5000000ull;
}

/* the timer: just after the cycle by which `frames` more will have moved
 * (one period from then on, a fallback) */
static void arm_timer(struct jb *p, snd_pcm_uframes_t frames)
{
	uint64_t period = period_ns_of(p);
	uint64_t next = atomic_load_explicit(&p->next_ns, memory_order_relaxed);
	uint64_t now = now_ns();
	uint32_t quantum = jack_get_buffer_size(p->client);
	uint64_t cycles = quantum ? (frames + quantum - 1) / quantum : 1;
	uint64_t t = (next ? next : now) + (cycles ? cycles - 1 : 0) * period + period / 4;

	while (t <= now)
		t += period;
	struct itimerspec its = {
		.it_interval = { (time_t)(period / 1000000000ull), (long)(period % 1000000000ull) },
		.it_value = { (time_t)(t / 1000000000ull), (long)(t % 1000000000ull) },
	};
	timerfd_settime(p->timer_fd, TFD_TIMER_ABSTIME, &its, NULL);
}

static void read_events(struct jb *p)
{
	jackbus_event_t e;
	while (jackbus_node_next_event(p->node, &e) == 1)
		if (e.type == JACKBUS_EVENT_SHUTDOWN)
			atomic_store(&p->dead, true);
}

static void autoconnect(struct jb *p)
{
	const char *env = getenv("JACKBUS_ALSA_AUTOCONNECT");
	if (p->connected || !p->autoconnect || (env && strcmp(env, "0") == 0) || p->n_ports == 0)
		return;
	p->connected = true;
	const char **phys = jack_get_ports(p->client, NULL, JACK_DEFAULT_AUDIO_TYPE,
			JackPortIsPhysical | (p->playback ? JackPortIsInput : JackPortIsOutput));
	/* one device's ports: the settings' output (input) when it has some,
	 * else the first physical client's */
	char who[128];
	jackbus_compat_device(p->playback, who, sizeof(who));
	size_t wl = strlen(who);
	bool there = false;
	for (unsigned int i = 0; wl && phys && phys[i]; i++)
		there |= strncmp(phys[i], who, wl) == 0 && phys[i][wl] == ':';
	if (!there && phys && phys[0]) {
		const char *colon = strchr(phys[0], ':');
		snprintf(who, sizeof(who), "%.*s", colon ? (int)(colon - phys[0]) : 0, phys[0]);
		wl = strlen(who);
	}
	unsigned int k = 0;
	for (unsigned int i = 0; phys && phys[i] && k < (p->n_ports == 1 ? 2u : p->n_ports); i++) {
		if (strncmp(phys[i], who, wl) != 0 || phys[i][wl] != ':')
			continue;
		unsigned int ch = p->n_ports == 1 ? 0 : k;
		const char *mine = jack_port_name(jackbus_port_jack(p->ports[ch]));
		if (p->playback)
			jack_connect(p->client, mine, phys[i]);
		else if (k == ch)
			jack_connect(p->client, phys[i], mine);
		k++;
	}
	jack_free(phys);
}

static int jb_start(snd_pcm_ioplug_t *io)
{
	struct jb *p = io->private_data;
	if (atomic_load(&p->dead))
		return -ENODEV;
	atomic_store(&p->xrun, false);
	atomic_store_explicit(&p->running, true, memory_order_release);
	arm_timer(p, p->avail_min);
	return 0;
}

static int jb_stop(snd_pcm_ioplug_t *io)
{
	struct jb *p = io->private_data;
	atomic_store(&p->running, false);
	return 0;
}

static snd_pcm_sframes_t jb_pointer(snd_pcm_ioplug_t *io)
{
	struct jb *p = io->private_data;
	if (atomic_load(&p->dead))
		return -ENODEV;
	if (atomic_load_explicit(&p->xrun, memory_order_acquire)) {
		snd_pcm_ioplug_set_state(io, SND_PCM_STATE_XRUN);
		return -EPIPE;
	}
	return (snd_pcm_sframes_t)atomic_load_explicit(&p->hw, memory_order_acquire);
}

static void free_resampler(struct jb *p)
{
	shim_resampler_free(p->rs);
	p->rs = NULL;
	if (p->tmp_mem) {
		munlock(p->tmp_mem, p->tmp_size);
		free(p->tmp_mem);
	}
	p->tmp_mem = NULL;
	p->tmp_size = 0;
	memset(p->tmp, 0, sizeof(p->tmp));
}

static int jb_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params)
{
	struct jb *p = io->private_data;
	unsigned long flags = p->playback ? JackPortIsOutput : JackPortIsInput;

	atomic_store(&p->running, false);
	atomic_store(&p->ready, false);
	wait_idle(p);
	if (io->channels > MAX_CHANNELS)
		return -EINVAL;
	while (p->n_ports > io->channels)
		jackbus_port_remove(p->ports[--p->n_ports]);
	while (p->n_ports < io->channels) {
		char name[32];
		snprintf(name, sizeof(name), "%s_%u", p->playback ? "out" : "in", p->n_ports + 1);
		jackbus_port_t *port = jackbus_port_add(p->node, name, JACKBUS_AUDIO, flags);
		if (port == NULL)
			return -ENOMEM;
		p->ports[p->n_ports++] = port;
	}
	p->channels = io->channels;
	p->format = io->format;
	p->buffer_size = io->buffer_size;
	free_resampler(p);
	if (io->rate != p->rate) {
		/* the fallback: resampled at a fixed ratio (the application runs on
		 * JackBus's clock) */
		double ratio = p->playback ? (double)io->rate / p->rate : (double)p->rate / io->rate;
		double most = ratio > 1.0 ? ratio : 1.0 / ratio;
		p->tmp_frames = (uint32_t)(8192 * most) + 256;
		p->tmp_size = (size_t)p->channels * p->tmp_frames * sizeof(float);
		if (posix_memalign(&p->tmp_mem, 64, p->tmp_size) != 0) {
			p->tmp_mem = NULL;
			return -ENOMEM;
		}
		memset(p->tmp_mem, 0, p->tmp_size);
		mlock(p->tmp_mem, p->tmp_size);
		for (unsigned int ch = 0; ch < p->channels; ch++)
			p->tmp[ch] = (float *)p->tmp_mem + (size_t)ch * p->tmp_frames;
		p->rs = shim_resampler_new((int)p->channels, ratio, p->playback ? (int)p->tmp_frames : 8192,
				p->playback ? 8192 : (int)p->tmp_frames);
		if (p->rs == NULL) {
			free_resampler(p);
			return -ENOMEM;
		}
		if (getenv("JACKBUS_ALSA_VERBOSE"))
			fprintf(stderr, "jackbus-alsa: resampled %u Hz <-> %u Hz (JackBus's)\n", io->rate, p->rate);
	}
	/* the ring is buffering the JACK graph sees */
	jackbus_node_set_process_latency(p->node, (jack_nframes_t)io->buffer_size);
	return 0;
}

static int jb_sw_params(snd_pcm_ioplug_t *io, snd_pcm_sw_params_t *params)
{
	struct jb *p = io->private_data;
	snd_pcm_sw_params_get_avail_min(params, &p->avail_min);
	snd_pcm_sw_params_get_boundary(params, &p->boundary);
	return 0;
}

static int jb_prepare(snd_pcm_ioplug_t *io)
{
	struct jb *p = io->private_data;
	snd_pcm_sw_params_t *sw;

	if (atomic_load(&p->dead))
		return -ENODEV;
	atomic_store(&p->running, false);
	atomic_store(&p->ready, false);
	wait_idle(p);
	snd_pcm_sw_params_alloca(&sw);
	if (snd_pcm_sw_params_current(io->pcm, sw) == 0) {
		snd_pcm_sw_params_get_avail_min(sw, &p->avail_min);
		snd_pcm_sw_params_get_boundary(sw, &p->boundary);
	}
	if (p->boundary == 0)
		p->boundary = io->buffer_size;
	p->areas = snd_pcm_ioplug_mmap_areas(io);
	if (p->rs)
		shim_resampler_reset(p->rs);	/* the stream starts again */
	atomic_store(&p->hw, 0);
	atomic_store(&p->xrun, false);
	if (p->areas == NULL || p->n_ports == 0)
		return -EINVAL;
	atomic_store(&p->ready, true);
	if (!p->activated) {
		if (jackbus_node_activate(p->node) != 0)
			return -EIO;
		p->activated = true;
	}
	autoconnect(p);
	arm_timer(p, 1);	/* prepared playback has room at once */
	return 0;
}

static int jb_poll_descriptors_count(snd_pcm_ioplug_t *io)
{
	return 2;
}

static int jb_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int space)
{
	struct jb *p = io->private_data;
	if (space < 2)
		return -EINVAL;
	pfd[0] = (struct pollfd){ .fd = p->timer_fd, .events = POLLIN };
	pfd[1] = (struct pollfd){ .fd = p->event_fd, .events = POLLIN };
	return 2;
}

static int jb_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int nfds,
		unsigned short *revents)
{
	struct jb *p = io->private_data;
	uint64_t expirations;
	ssize_t r = read(p->timer_fd, &expirations, sizeof(expirations));
	(void)r;

	read_events(p);
	if (atomic_load(&p->dead)) {
		*revents = POLLERR;
		return 0;
	}
	if (atomic_load(&p->xrun)) {
		*revents = p->playback ? POLLOUT : POLLIN;
		return 0;
	}
	snd_pcm_uframes_t hw = atomic_load_explicit(&p->hw, memory_order_acquire);
	snd_pcm_uframes_t avail = snd_pcm_ioplug_avail(io, hw, io->appl_ptr);
	snd_pcm_uframes_t want = p->avail_min ? p->avail_min : 1;
	if (avail >= want) {
		*revents = p->playback ? POLLOUT : POLLIN;
		arm_timer(p, 1);
	} else {
		*revents = 0;
		arm_timer(p, want - avail);
	}
	return 0;
}

static int jb_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp)
{
	struct jb *p = io->private_data;
	jack_latency_range_t range = { 0, 0 };
	snd_pcm_uframes_t hw = atomic_load_explicit(&p->hw, memory_order_acquire);

	if (p->n_ports)
		jack_port_get_latency_range(jackbus_port_jack(p->ports[0]),
				p->playback ? JackPlaybackLatency : JackCaptureLatency, &range);
	/* playback: written, not yet played; capture: captured, not yet read */
	snd_pcm_uframes_t queued = p->playback ? snd_pcm_ioplug_hw_avail(io, hw, io->appl_ptr) :
		snd_pcm_ioplug_avail(io, hw, io->appl_ptr);
	/* resampling: the frames the resampler holds, and the ports' latency at
	 * the application's rate */
	double held = p->rs ? shim_resampler_delay(p->rs) : 0.0;
	double lat = p->rs ? (double)range.max * io->rate / p->rate : (double)range.max;
	*delayp = (snd_pcm_sframes_t)(queued + (held > 0 ? held : 0) + lat);
	return 0;
}

static int jb_close(snd_pcm_ioplug_t *io)
{
	struct jb *p = io->private_data;
	uint64_t xruns = atomic_load(&p->xruns);
	if (xruns && getenv("JACKBUS_ALSA_VERBOSE"))
		fprintf(stderr, "jackbus-alsa: %llu xruns\n", (unsigned long long)xruns);
	jackbus_node_close(p->node);	/* deactivates: no cycle runs process after this */
	if (p->timer_fd >= 0)
		close(p->timer_fd);
	free_resampler(p);
	free(p);
	return 0;
}

static const snd_pcm_ioplug_callback_t callbacks = {
	.start = jb_start,
	.stop = jb_stop,
	.pointer = jb_pointer,
	.close = jb_close,
	.hw_params = jb_hw_params,
	.sw_params = jb_sw_params,
	.prepare = jb_prepare,
	.poll_descriptors_count = jb_poll_descriptors_count,
	.poll_descriptors = jb_poll_descriptors,
	.poll_revents = jb_poll_revents,
	.delay = jb_delay,
};

static int set_constraints(struct jb *p)
{
	static const unsigned int access[] = {
		SND_PCM_ACCESS_MMAP_INTERLEAVED, SND_PCM_ACCESS_MMAP_NONINTERLEAVED,
		SND_PCM_ACCESS_RW_INTERLEAVED, SND_PCM_ACCESS_RW_NONINTERLEAVED,
	};
	static const unsigned int formats[] = {
		SND_PCM_FORMAT_FLOAT_LE, SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S24_LE, SND_PCM_FORMAT_S16_LE,
	};
	unsigned int quantum = jack_get_buffer_size(p->client);
	int err;

	if ((err = snd_pcm_ioplug_set_param_list(&p->io, SND_PCM_IOPLUG_HW_ACCESS,
			N_ELEMENTS(access), access)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_list(&p->io, SND_PCM_IOPLUG_HW_FORMAT,
			N_ELEMENTS(formats), formats)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&p->io, SND_PCM_IOPLUG_HW_CHANNELS, 1, MAX_CHANNELS)) < 0 ||
	    /* JackBus's rate; with the resampling fallback, any rate a fifth of it to
	     * five times it (an application asking for JackBus's gets the direct route) */
	    (err = snd_pcm_ioplug_set_param_minmax(&p->io, SND_PCM_IOPLUG_HW_RATE,
			p->resample ? (p->rate + 4) / 5 : p->rate, p->resample ? p->rate * 5 : p->rate)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&p->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES,
			64, 4 * 1024 * 1024)) < 0 ||
	    /* at least two of JackBus's cycles of stereo S16 */
	    (err = snd_pcm_ioplug_set_param_minmax(&p->io, SND_PCM_IOPLUG_HW_BUFFER_BYTES,
			2 * quantum * 4, 64 * 1024 * 1024)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&p->io, SND_PCM_IOPLUG_HW_PERIODS, 2, 1024)) < 0)
		return err;
	return 0;
}

/* the PCM to use while JackBus isn't running; a fallback that leads back
 * here (to the default device that is this one) fails instead of looping */
static int open_fallback(snd_pcm_t **pcmp, const char *fallback, snd_pcm_stream_t stream, int mode)
{
	static __thread int depth;
	if (depth > 0)
		return -ENODEV;
	depth++;
	int err = snd_pcm_open(pcmp, fallback, stream, mode);
	depth--;
	if (getenv("JACKBUS_ALSA_VERBOSE"))
		fprintf(stderr, "jackbus-alsa: JackBus isn't running: %s (%s)\n", fallback, err < 0 ? snd_strerror(err) : "opened");
	return err;
}

SND_PCM_PLUGIN_DEFINE_FUNC(jackbus)
{
	snd_config_iterator_t i, next;
	const char *cname = NULL, *server = NULL, *fallback = NULL;
	int connect = -1, err;		/* autoconnect: -1, the settings' */
	jack_status_t status = 0;

	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id;
		if (snd_config_get_id(n, &id) < 0)
			continue;
		if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0 || strcmp(id, "hint") == 0)
			continue;
		if (strcmp(id, "name") == 0) {
			snd_config_get_string(n, &cname);
			continue;
		}
		if (strcmp(id, "server") == 0) {
			snd_config_get_string(n, &server);
			if (server && server[0] == 0)
				server = NULL;
			continue;
		}
		if (strcmp(id, "fallback") == 0) {
			snd_config_get_string(n, &fallback);
			if (fallback && fallback[0] == 0)
				fallback = NULL;
			continue;
		}
		if (strcmp(id, "autoconnect") == 0) {
			if ((connect = snd_config_get_bool(n)) < 0)
				return -EINVAL;
			continue;
		}
		SNDERR("jackbus: unknown field %s", id);
		return -EINVAL;
	}

	/* JackBus isn't running: the fallback, when there is one (checked without
	 * libjack: nothing printed) */
	if (fallback && !jackbus_server_running(server))
		return open_fallback(pcmp, fallback, stream, mode);

	struct jb *p = calloc(1, sizeof(*p));
	if (p == NULL)
		return -ENOMEM;
	p->playback = stream == SND_PCM_STREAM_PLAYBACK;
	p->autoconnect = connect >= 0 ? connect :
		jackbus_compat_bool("autoconnect", "JACKBUS_ALSA_AUTOCONNECT", true);
	p->resample = jackbus_compat_bool("resample", "JACKBUS_ALSA_RESAMPLE", true);
	p->timer_fd = -1;

	const char *env = getenv("JACKBUS_ALSA_NAME");
	const char *client_name = (cname && cname[0]) ? cname : (env && env[0]) ? env : program_invocation_short_name;
	jack_options_t options = JackNoStartServer | (server ? JackServerName : 0);
	p->node = jackbus_node_open(client_name ? client_name : "alsa", options, &status, server);
	if (p->node == NULL) {
		free(p);
		if (fallback)			/* its socket is there, the server isn't */
			return open_fallback(pcmp, fallback, stream, mode);
		SNDERR("jackbus: can't reach JackBus (status 0x%x)", status);
		return -ENODEV;
	}
	p->client = jackbus_node_client(p->node);
	p->rate = jack_get_sample_rate(p->client);
	p->event_fd = jackbus_node_event_fd(p->node);
	p->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (p->timer_fd < 0 || jackbus_node_set_process(p->node, jb_process, p) != 0) {
		err = errno ? -errno : -EIO;
		goto error;
	}

	p->io.version = SND_PCM_IOPLUG_VERSION;
	p->io.name = "JackBus (direct)";
	p->io.callback = &callbacks;
	p->io.private_data = p;
	p->io.mmap_rw = 1;
	p->io.flags = SND_PCM_IOPLUG_FLAG_BOUNDARY_WA | SND_PCM_IOPLUG_FLAG_MONOTONIC;
	if ((err = snd_pcm_ioplug_create(&p->io, name, stream, mode)) < 0)
		goto error;
	if ((err = set_constraints(p)) < 0) {
		snd_pcm_ioplug_delete(&p->io);	/* calls jb_close */
		return err;
	}
	*pcmp = p->io.pcm;
	return 0;

error:
	jackbus_node_close(p->node);
	if (p->timer_fd >= 0)
		close(p->timer_fd);
	free(p);
	return err;
}

SND_PCM_PLUGIN_SYMBOL(jackbus);
