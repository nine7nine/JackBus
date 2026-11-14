/*
 * underrun_probe: what an ALSA device does when the application is late, in two modes.
 *
 *   stop   today's JACK behaviour: stop_threshold = buffer size, so an underrun stops the
 *          stream (XRUN); the probe prepares and restarts it, as alsa_driver_xrun_recovery does.
 *   spare  the alternative under study: a ring one period longer than the queue (latency
 *          unchanged), a spare period written beyond the committed data after every write
 *          (not committed), and stop_threshold = boundary, so the stream never stops. A late
 *          wake finds the hardware inside the spare; the probe skips past it (snd_pcm_forward)
 *          and catches up the captured periods, one period of output per captured period.
 *
 * Lateness is injected: every K cycles, after the wake, the probe sleeps L periods before it services
 * the device (a wake that comes L periods late).
 * It writes zeros only, so it is silent whatever the device's volume.
 *
 * Measured, per late cycle and in total: whether the stream kept running, what avail / delay
 * report when the hardware has passed the application pointer, what snd_pcm_forwardable and
 * snd_pcm_forward do there, captured frames lost (an overrun or a restart), and how long a
 * stop-mode restart takes.
 *
 *   underrun_probe -d hw:U44 [-r 48000] [-p 128] [-q 2] [-c 2] [-m stop|spare] [-s seconds]
 *                  [-k every] [-l late-periods] [-o log] [-P (playback only)]
 *                  [-C (spare mode: capture never stops either; default: it stops on an overrun)]
 *                  [-T margin_us (timer fallback: wait for the interrupt at most until the position
 *                   says the period is due, plus margin_us; then read the position instead)]
 *
 * Build: cc -O2 -Wall -o underrun_probe underrun_probe.c -lasound
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *dev = "hw:U44";
static unsigned rate = 48000, period = 128, queue = 2, chans = 2;
static int spare_mode = 0, playback_only = 0, cap_boundary = 0;
static double seconds = 20, late_periods = 1.5;
static int timer_margin_us = -1;   /* -T: wait at most until the next period is due + this */
static unsigned every = 250;
static FILE *logf;

static int64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#define CHECK(x) do { int e_ = (x); if (e_ < 0) { fprintf(stderr, "%s:%d %s: %s\n", __FILE__, __LINE__, #x, snd_strerror(e_)); exit(1); } } while (0)

static snd_pcm_format_t fmt;
static unsigned bytes_per_frame;

static void setup(snd_pcm_t *h, int playback, unsigned nper)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	snd_pcm_uframes_t ps = period, bs = (snd_pcm_uframes_t)period * nper, boundary;
	unsigned r = rate, ch = chans, np = nper;
	int dir = 0;

	snd_pcm_hw_params_alloca(&hw);
	CHECK(snd_pcm_hw_params_any(h, hw));
	CHECK(snd_pcm_hw_params_set_access(h, hw, SND_PCM_ACCESS_MMAP_INTERLEAVED));
	if (snd_pcm_hw_params_set_format(h, hw, SND_PCM_FORMAT_S32_LE) == 0)
		fmt = SND_PCM_FORMAT_S32_LE;
	else {
		CHECK(snd_pcm_hw_params_set_format(h, hw, SND_PCM_FORMAT_S16_LE));
		fmt = SND_PCM_FORMAT_S16_LE;
	}
	if (snd_pcm_hw_params_set_channels(h, hw, ch) < 0) {
		CHECK(snd_pcm_hw_params_set_channels_near(h, hw, &ch));
	}
	CHECK(snd_pcm_hw_params_set_rate(h, hw, r, 0));
	CHECK(snd_pcm_hw_params_set_period_size(h, hw, ps, 0));
	CHECK(snd_pcm_hw_params_set_periods(h, hw, np, 0));
	CHECK(snd_pcm_hw_params(h, hw));
	CHECK(snd_pcm_hw_params_get_buffer_size(hw, &bs));
	CHECK(snd_pcm_hw_params_get_period_size(hw, &ps, &dir));
	if (playback)
		bytes_per_frame = snd_pcm_format_physical_width(fmt) / 8 * ch;

	snd_pcm_sw_params_alloca(&sw);
	CHECK(snd_pcm_sw_params_current(h, sw));
	CHECK(snd_pcm_sw_params_get_boundary(sw, &boundary));
	CHECK(snd_pcm_sw_params_set_start_threshold(h, sw, boundary));          /* started explicitly */
	/* spare mode: playback never stops; capture keeps stopping on a real overrun (lost input is
	   reported, not overwritten silently) unless -C asks for boundary there too */
	const int never_stop = spare_mode && (playback || cap_boundary);
	CHECK(snd_pcm_sw_params_set_stop_threshold(h, sw, never_stop ? boundary : bs));
	CHECK(snd_pcm_sw_params_set_silence_threshold(h, sw, 0));
	CHECK(snd_pcm_sw_params_set_silence_size(h, sw, 0));
	/* As JACK: playback wakes when (ring - queue + 1) periods are free, i.e. one period left queued */
	CHECK(snd_pcm_sw_params_set_avail_min(h, sw, playback ? period * (nper - queue + 1) : period));
	CHECK(snd_pcm_sw_params_set_tstamp_mode(h, sw, SND_PCM_TSTAMP_ENABLE));
	CHECK(snd_pcm_sw_params_set_tstamp_type(h, sw, SND_PCM_TSTAMP_TYPE_MONOTONIC));
	CHECK(snd_pcm_sw_params(h, sw));
	fprintf(logf, "# %s: %s %u ch %u Hz period %lu buffer %lu (%u periods) stop_threshold %s boundary %lu\n",
		playback ? "playback" : "capture", snd_pcm_format_name(fmt), ch, r, ps, bs, nper,
		never_stop ? "boundary" : "buffer", boundary);
	if (ps != period || bs != (snd_pcm_uframes_t)period * nper) {
		fprintf(stderr, "device gave period %lu buffer %lu, wanted %u x %u\n", ps, bs, period, nper);
		exit(1);
	}
}

/* Write `commit` zero frames at the application pointer and commit them, then `spare` more zero
   frames beyond them, not committed (written straight into the ring, wrapping at its end). */
static void write_zeros(snd_pcm_t *h, snd_pcm_uframes_t commit, snd_pcm_uframes_t spare, snd_pcm_uframes_t ring)
{
	const snd_pcm_channel_area_t *a;
	snd_pcm_uframes_t off, done = 0;
	while (done < commit) {
		snd_pcm_uframes_t f = commit - done;
		CHECK(snd_pcm_mmap_begin(h, &a, &off, &f));
		if (f == 0)
			break;
		memset((char *)a[0].addr + a[0].first / 8 + off * bytes_per_frame, 0, f * bytes_per_frame);
		CHECK(snd_pcm_mmap_commit(h, off, f));
		done += f;
	}
	if (!spare)
		return;
	snd_pcm_uframes_t f = 1;
	CHECK(snd_pcm_mmap_begin(h, &a, &off, &f));   /* the ring's base and the application pointer's offset */
	char *base = (char *)a[0].addr + a[0].first / 8;
	snd_pcm_uframes_t first = spare < ring - off ? spare : ring - off;
	memset(base + off * bytes_per_frame, 0, first * bytes_per_frame);
	if (spare > first)
		memset(base, 0, (spare - first) * bytes_per_frame);
	CHECK(snd_pcm_mmap_commit(h, off, 0));
}

/* Read (discard) n captured frames. */
static void read_frames(snd_pcm_t *h, snd_pcm_uframes_t n)
{
	const snd_pcm_channel_area_t *a;
	snd_pcm_uframes_t off;
	while (n) {
		snd_pcm_uframes_t f = n;
		CHECK(snd_pcm_mmap_begin(h, &a, &off, &f));
		if (f == 0)
			break;
		CHECK(snd_pcm_mmap_commit(h, off, f));
		n -= f;
	}
}

static const char *state_name(snd_pcm_t *h)
{
	return snd_pcm_state_name(snd_pcm_state(h));
}

int main(int argc, char **argv)
{
	const char *logpath = NULL;
	int o;
	while ((o = getopt(argc, argv, "d:r:p:q:c:m:s:k:l:o:PCT:")) != -1) {
		switch (o) {
		case 'd': dev = optarg; break;
		case 'r': rate = atoi(optarg); break;
		case 'p': period = atoi(optarg); break;
		case 'q': queue = atoi(optarg); break;
		case 'c': chans = atoi(optarg); break;
		case 'm': spare_mode = !strcmp(optarg, "spare"); break;
		case 's': seconds = atof(optarg); break;
		case 'k': every = atoi(optarg); break;
		case 'l': late_periods = atof(optarg); break;
		case 'o': logpath = optarg; break;
		case 'P': playback_only = 1; break;
		case 'C': cap_boundary = 1; break;
		case 'T': timer_margin_us = atoi(optarg); break;
		default: fprintf(stderr, "see the header comment\n"); return 2;
		}
	}
	logf = logpath ? fopen(logpath, "w") : stdout;
	if (!logf) { perror(logpath); return 1; }

	const unsigned nper = spare_mode ? queue + 1 : queue;
	const snd_pcm_uframes_t P = period, B = (snd_pcm_uframes_t)period * nper;
	snd_pcm_t *pb = NULL, *cap = NULL;

	CHECK(snd_pcm_open(&pb, dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK));
	setup(pb, 1, nper);
	if (!playback_only) {
		CHECK(snd_pcm_open(&cap, dev, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK));
		setup(cap, 0, nper);
		if (snd_pcm_link(pb, cap) < 0) {
			fprintf(logf, "# snd_pcm_link failed: streams started separately\n");
		}
	}
	fprintf(logf, "# mode %s, queue %u periods, lateness %.2f periods every %u cycles, %.0f s\n",
		spare_mode ? "spare" : "stop", queue, late_periods, every, seconds);
	fprintf(logf, "# late cycles: cycle t_us slept_us pb_state pb_avail pb_delay forwardable forwarded cap_state cap_avail cap_lost catchup restart_us\n");

	const unsigned pfd_n = snd_pcm_poll_descriptors_count(pb) + (cap ? snd_pcm_poll_descriptors_count(cap) : 0);
	struct pollfd pfd[16];
	if (pfd_n > 16) { fprintf(stderr, "too many poll fds\n"); return 1; }

	/* prefill: the queue, plus the spare (uncommitted) */
	write_zeros(pb, (snd_pcm_uframes_t)P * queue, spare_mode ? P : 0, B);
	CHECK(snd_pcm_start(pb));
	if (cap && snd_pcm_state(cap) != SND_PCM_STATE_RUNNING)
		snd_pcm_start(cap);

	const int64_t t0 = now_us(), period_us = (int64_t)P * 1000000 / rate;
	uint64_t cycle = 0, late_n = 0, stopped = 0, beyond_spare = 0, forwards = 0;
	uint64_t cap_lost_total = 0, max_catchup = 0, xrun_restarts = 0;
	int64_t restart_us_total = 0, restart_us_max = 0;
	uint64_t pb_written = 0, cap_read = 0;
	snd_pcm_sframes_t min_queued = (snd_pcm_sframes_t)B;
	uint64_t still_past = 0;
#define MAXD 400000
#define HBINS 256
	static uint64_t hist[HBINS];
	static snd_pcm_sframes_t delays[MAXD];
	size_t ndelay = 0;

	/* Outputs still to drop: the periods the spare stood in for. Each captured period owns one
	   output slot, written or stood in for, so the input-to-output latency never moves. */
	uint64_t drop_out = 0, dropped = 0, overflow = 0, spin = 0, timer_wakes = 0;
	const snd_pcm_sframes_t pmin = (snd_pcm_sframes_t)P * (nper - queue + 1);
	const snd_pcm_sframes_t spare = spare_mode ? (snd_pcm_sframes_t)P : 0;

	while (now_us() - t0 < (int64_t)(seconds * 1e6)) {
		int64_t slept = 0;
		snd_pcm_sframes_t pav = 0, cav = 0;
		/* wait until both directions are ready, polling only the one that is not (as
		   alsa_driver_wait does), so a ready stream never makes poll spin */
		for (unsigned guard = 0;; guard++) {
			pav = snd_pcm_avail(pb);
			cav = cap ? snd_pcm_avail(cap) : 0;
			if (pav < 0 || cav < 0)
				break;                                   /* an xrun (stop mode): handled below */
			const int need_p = pav < pmin, need_c = cap && cav < (snd_pcm_sframes_t)P;
			if (!need_p && !need_c)
				break;
			unsigned n = 0;
			if (need_p) n += snd_pcm_poll_descriptors(pb, pfd + n, 16 - n);
			if (need_c) n += snd_pcm_poll_descriptors(cap, pfd + n, 16 - n);
			int r;
			if (timer_margin_us >= 0) {
				/* when the next period is due, from the position read just now */
				snd_pcm_sframes_t need = 0;
				if (need_p && pmin - pav > need) need = pmin - pav;
				if (need_c && (snd_pcm_sframes_t)P - cav > need) need = (snd_pcm_sframes_t)P - cav;
				int64_t us = (int64_t)need * 1000000 / rate + timer_margin_us;
				struct timespec ts = { us / 1000000, (us % 1000000) * 1000 };
				r = ppoll(pfd, n, &ts, NULL);
				if (r == 0) {
					timer_wakes++;   /* no interrupt by then: the loop reads the position itself */
					continue;
				}
			} else {
				r = poll(pfd, n, 1000);
			}
			if (r <= 0) { fprintf(logf, "# poll %s at cycle %" PRIu64 "\n", r ? strerror(errno) : "timeout", cycle); goto out; }
			if (guard > 100000) { spin++; fprintf(logf, "# wait spun at cycle %" PRIu64 "\n", cycle); goto out; }
		}
		/* a late wake: the device is ready, the application gets to it L periods later */
		if (every && cycle && cycle % every == 0) {
			int64_t want = (int64_t)(late_periods * period_us), s0 = now_us();
			struct timespec ts = { want / 1000000, (want % 1000000) * 1000 };
			clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
			slept = now_us() - s0;
			pav = snd_pcm_avail(pb);
			cav = cap ? snd_pcm_avail(cap) : 0;
		}
		const int64_t t = now_us();
		cycle++;
		snd_pcm_sframes_t pdelay = 0;
		snd_pcm_delay(pb, &pdelay);

		const int pb_xrun = pav == -EPIPE || snd_pcm_state(pb) == SND_PCM_STATE_XRUN;
		const int cap_xrun = cap && (cav == -EPIPE || snd_pcm_state(cap) == SND_PCM_STATE_XRUN);
		const int late = slept || pb_xrun || cap_xrun || (pav >= 0 && pav > (snd_pcm_sframes_t)B);
		if (!slept && !pb_xrun && pav >= 0 && pav <= (snd_pcm_sframes_t)B && ndelay < MAXD)
			delays[ndelay++] = pdelay;   /* output delay at an ordinary wake (before this cycle's write) */
		if (!slept && pav >= 0) {
			/* how far the hardware had got at the wake, past the point the wake was for
			   (avail_min): 0 = on time; a period = a full period late */
			snd_pcm_sframes_t at = pav - pmin;
			int bin = (int)((at + 2 * (snd_pcm_sframes_t)P) / 8);
			if (bin < 0) bin = 0;
			if (bin >= HBINS) bin = HBINS - 1;
			hist[bin]++;
		}
		if (!slept && pav >= 0 && (snd_pcm_sframes_t)B - pav < min_queued)
			min_queued = (snd_pcm_sframes_t)B - pav;

		if (pb_xrun || cap_xrun) {
			/* stop mode: what JACK does today: prepare, refill, restart */
			const int64_t r0 = now_us();
			stopped++;
			xrun_restarts++;
			snd_pcm_drop(pb);
			CHECK(snd_pcm_prepare(pb));
			if (cap) {
				snd_pcm_drop(cap);
				snd_pcm_prepare(cap);
			}
			write_zeros(pb, (snd_pcm_uframes_t)P * queue, spare_mode ? P : 0, B);
			CHECK(snd_pcm_start(pb));
			if (cap && snd_pcm_state(cap) != SND_PCM_STATE_RUNNING)
				snd_pcm_start(cap);
			const int64_t rus = now_us() - r0;
			restart_us_total += rus;
			if (rus > restart_us_max) restart_us_max = rus;
			fprintf(logf, "%" PRIu64 " %" PRId64 " %" PRId64 " XRUN pb_avail=%ld pb_delay=%ld - - cap=%s cap_avail=%ld - 0 %" PRId64 "\n",
				cycle, t - t0, slept, (long)pav, (long)pdelay, cap_xrun ? "XRUN" : "ok", (long)cav, rus);
			late_n++;
			continue;
		}

		snd_pcm_sframes_t cap_lost = 0;
		if (cap && cav > (snd_pcm_sframes_t)B) {     /* boundary mode: an overrun overwrote unread frames */
			cap_lost = cav - (snd_pcm_sframes_t)B;
			cap_lost_total += cap_lost;
			snd_pcm_forward(cap, cap_lost);
			cav -= cap_lost;
		}

		/* the hardware has passed the committed data: it is playing the spare, or beyond it */
		snd_pcm_sframes_t fwd_able = 0, fwd = 0;
		if (spare_mode && pav > (snd_pcm_sframes_t)B) {
			const snd_pcm_sframes_t past = pav - (snd_pcm_sframes_t)B;
			if (past >= (snd_pcm_sframes_t)P)
				beyond_spare++;
			const snd_pcm_uframes_t k = ((snd_pcm_uframes_t)past + P - 1) / P;   /* periods it is in */
			fwd_able = snd_pcm_forwardable(pb);
			fwd = snd_pcm_forward(pb, k * P);
			if (fwd != (snd_pcm_sframes_t)(k * P))
				still_past++;
			drop_out += k;
			forwards++;
		}

		/* lockstep, one output period per captured period (playback-only: one per free period) */
		const snd_pcm_uframes_t cycles = cap ? (snd_pcm_uframes_t)cav / P
			: (pav > spare ? (snd_pcm_uframes_t)(pav - spare) / P : 0);
		if (cycles > max_catchup) max_catchup = cycles;
		for (snd_pcm_uframes_t i = 0; i < cycles; i++) {
			if (cap)
				read_frames(cap, P);
			cap_read += P;
			if (drop_out) {
				drop_out--;
				dropped++;
				continue;
			}
			const snd_pcm_sframes_t room = snd_pcm_avail(pb);
			if (room - spare < (snd_pcm_sframes_t)P) {
				overflow++;           /* no slot for this output: would only happen if the lockstep broke */
				continue;
			}
			write_zeros(pb, P, 0, B);
			pb_written += P;
		}
		if (spare_mode)
			write_zeros(pb, 0, P, B);

		if (late) {
			late_n++;
			fprintf(logf, "%" PRIu64 " %" PRId64 " %" PRId64 " %s %ld %ld %ld %ld %s %ld %ld %lu -\n",
				cycle, t - t0, slept, state_name(pb), (long)pav, (long)pdelay, (long)fwd_able, (long)fwd,
				cap ? state_name(cap) : "-", (long)cav, (long)cap_lost, (unsigned long)cycles);
		}
	}
out:
	if (timer_margin_us >= 0)
		fprintf(logf, "# timer fallback (margin %d us): %" PRIu64 " waits ended by the timer\n", timer_margin_us, timer_wakes);
	fprintf(logf, "# output slots: written %" PRIu64 " + stood in by the spare %" PRIu64 " - captured %" PRIu64 " = %+" PRId64 " periods (prefill %u; constant means the latency never moved, except across stop-mode restarts); overflows %" PRIu64 ", wait spins %" PRIu64 "\n",
		pb_written / P, dropped, cap_read / P, (int64_t)(pb_written / P + dropped) - (int64_t)(cap_read / P), queue, overflow, spin);
	const int64_t t_end = now_us();
	snd_pcm_sframes_t cav_end = cap ? snd_pcm_avail(cap) : 0;
	const double expected = (double)(t_end - t0) * rate / 1e6;
	const double missing = cap ? expected - (double)cap_read - (cav_end > 0 ? cav_end : 0) : 0;
	fprintf(logf, "# capture against the wall clock: expected %.0f frames, read %" PRIu64 " + %ld waiting, missing %.0f (%.2f periods)\n",
		expected, cap_read, (long)(cav_end > 0 ? cav_end : 0), missing, missing / P);
	if (ndelay) {
		int cmp(const void *a, const void *b) { snd_pcm_sframes_t x = *(const snd_pcm_sframes_t *)a, y = *(const snd_pcm_sframes_t *)b; return (x > y) - (x < y); }
		qsort(delays, ndelay, sizeof delays[0], cmp);
		fprintf(logf, "# output delay at ordinary wakes (frames): min %ld median %ld p99 %ld max %ld; after the write add %u\n",
			(long)delays[0], (long)delays[ndelay / 2], (long)delays[ndelay * 99 / 100], (long)delays[ndelay - 1], period);
	}
	fprintf(logf, "# wake position past the wake point (frames, 8-frame bins, ordinary wakes):");
	for (int i = 0; i < HBINS; i++)
		if (hist[i])
			fprintf(logf, " %ld:%" PRIu64, (long)i * 8 - 2 * (long)P, hist[i]);
	fprintf(logf, "\n");
	fprintf(logf, "# least committed audio left at a wake that was not injected: %ld frames (%.2f periods)\n",
		(long)min_queued, (double)min_queued / P);
	fprintf(logf, "# summary: cycles %" PRIu64 ", late %" PRIu64 ", stream stopped %" PRIu64 " (restarts, total %" PRId64 " us, max %" PRId64 " us), "
		"spare forwards %" PRIu64 " (still past after one: %" PRIu64 "), hardware beyond the spare %" PRIu64 ", captured frames lost %" PRIu64 ", most periods caught up at once %" PRIu64 "\n",
		cycle, late_n, stopped, restart_us_total, restart_us_max, forwards, still_past, beyond_spare, cap_lost_total, max_catchup);
	fprintf(logf, "# final: playback %s, capture %s, written %" PRIu64 " read %" PRIu64 "\n",
		state_name(pb), cap ? state_name(cap) : "-", pb_written, cap_read);
	(void)xrun_restarts;
	snd_pcm_close(pb);
	if (cap) snd_pcm_close(cap);
	return 0;
}
