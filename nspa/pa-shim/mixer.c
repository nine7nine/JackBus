/*
 * jackbus-pa: a device's volume is its card's own mixer.
 *
 * A sink or source that is an ALSA card (JackBus's own card, a bridged card:
 * their ports' alias "alsa_pcm:<device>:in|outN" names the device) has the
 * card's hardware volume and mute: its playback control (Master, else PCM,
 * ...) for the sink, its capture control (Capture, ...) for the source.
 * The card holds the value, so every process sees the same one (alsamixer
 * too): a desktop's volume control and its volume keys set it through this
 * library, and a change made anywhere reaches subscribers as a sink (source)
 * change. The percentages are alsamixer's: perceptual over the control's dB
 * range (PulseAudio's cubic curve), its minimum 0 %, its maximum 100 %.
 *
 * A card's mixer is opened on first use (a device's information, a volume
 * set, a subscription to devices), on the application's main loop, never in
 * the cycle. A device of no card, or of a card without such a
 * control, keeps a fixed volume (a request is accepted, not applied).
 */
#define _GNU_SOURCE
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <alsa/asoundlib.h>

#include "shim.h"

#define MAX_IO 4
#define LINEAR_DB 2400			/* a dB range this small (0.01 dB): linear */

struct shim_mixer {
	char ctl[64];			/* "hw:PCH" */
	snd_mixer_t *m;			/* NULL: it doesn't open (or the card went away) */
	snd_mixer_elem_t *play, *cap;
	pa_io_event *io[MAX_IO];
	int n_io;
	pa_context *c;
	struct shim_mixer *next;
};

/* ------------------------------------------------------------ the card */

void shim_mixer_ctl_of(const char *alias, char *ctl, size_t len)
{
	ctl[0] = '\0';
	if (strncmp(alias, "alsa_pcm:", 9) != 0)
		return;
	const char *dev = alias + 9;
	const char *end = strrchr(dev, ':');		/* ":in1", ":out1" */
	if (end == NULL)
		return;
	char d[128];
	snprintf(d, sizeof(d), "%.*s", (int)(end - dev), dev);
	/* "hw:PCH", "hw:0,0", "plughw:CARD=PCH,DEV=0", "front:PCH": the card is
	 * the first argument; a name without arguments ("default") is no card */
	const char *args = strchr(d, ':');
	if (args == NULL)
		return;
	const char *card = strstr(args + 1, "CARD=");
	card = card ? card + 5 : args + 1;
	if (*card == '"')
		card++;
	size_t n = strcspn(card, ",\"");
	if (n == 0 || n > 32)
		return;
	for (size_t i = 0; i < n; i++)
		if (!(card[i] == '_' || card[i] == '-' || (card[i] >= '0' && card[i] <= '9') ||
		      (card[i] >= 'A' && card[i] <= 'Z') || (card[i] >= 'a' && card[i] <= 'z')))
			return;
	snprintf(ctl, len, "hw:%.*s", (int)n, card);
}

/* ----------------------------------------------- a control, either way */

static bool has_volume(snd_mixer_elem_t *e, bool pb)
{
	return pb ? snd_mixer_selem_has_playback_volume(e) : snd_mixer_selem_has_capture_volume(e);
}

static bool has_switch(snd_mixer_elem_t *e, bool pb)
{
	return pb ? snd_mixer_selem_has_playback_switch(e) : snd_mixer_selem_has_capture_switch(e);
}

static bool has_channel(snd_mixer_elem_t *e, bool pb, int ch)
{
	return pb ? snd_mixer_selem_has_playback_channel(e, ch) : snd_mixer_selem_has_capture_channel(e, ch);
}

static bool is_mono(snd_mixer_elem_t *e, bool pb)
{
	return pb ? snd_mixer_selem_is_playback_mono(e) : snd_mixer_selem_is_capture_mono(e);
}

static int db_range(snd_mixer_elem_t *e, bool pb, long *min, long *max)
{
	return pb ? snd_mixer_selem_get_playback_dB_range(e, min, max) :
		snd_mixer_selem_get_capture_dB_range(e, min, max);
}

static int raw_range(snd_mixer_elem_t *e, bool pb, long *min, long *max)
{
	return pb ? snd_mixer_selem_get_playback_volume_range(e, min, max) :
		snd_mixer_selem_get_capture_volume_range(e, min, max);
}

static int get_db(snd_mixer_elem_t *e, bool pb, int ch, long *v)
{
	return pb ? snd_mixer_selem_get_playback_dB(e, ch, v) : snd_mixer_selem_get_capture_dB(e, ch, v);
}

static int get_raw(snd_mixer_elem_t *e, bool pb, int ch, long *v)
{
	return pb ? snd_mixer_selem_get_playback_volume(e, ch, v) : snd_mixer_selem_get_capture_volume(e, ch, v);
}

static int set_db(snd_mixer_elem_t *e, bool pb, int ch, long v, int dir)
{
	return pb ? snd_mixer_selem_set_playback_dB(e, ch, v, dir) : snd_mixer_selem_set_capture_dB(e, ch, v, dir);
}

static int set_raw(snd_mixer_elem_t *e, bool pb, int ch, long v)
{
	return pb ? snd_mixer_selem_set_playback_volume(e, ch, v) : snd_mixer_selem_set_capture_volume(e, ch, v);
}

/* alsamixer's mapping: 0..1 */
static double get_norm(snd_mixer_elem_t *e, bool pb, int ch)
{
	long min, max, v;
	if (db_range(e, pb, &min, &max) < 0 || min >= max) {
		if (raw_range(e, pb, &min, &max) < 0 || min >= max || get_raw(e, pb, ch, &v) < 0)
			return 1.0;
		return (double)(v - min) / (double)(max - min);
	}
	if (get_db(e, pb, ch, &v) < 0)
		return 1.0;
	if (max - min <= LINEAR_DB)
		return (double)(v - min) / (double)(max - min);
	double n = exp10((double)(v - max) / 6000.0);
	if (min != SND_CTL_TLV_DB_GAIN_MUTE) {
		double nmin = exp10((double)(min - max) / 6000.0);
		n = (n - nmin) / (1.0 - nmin);
	}
	return n < 0.0 ? 0.0 : n > 1.0 ? 1.0 : n;
}

static long round_dir(double x, int dir)
{
	return dir > 0 ? (long)ceil(x) : dir < 0 ? (long)floor(x) : lrint(x);
}

/* dir: the way it moves (so a small step still moves the control a step) */
static int set_norm(snd_mixer_elem_t *e, bool pb, int ch, double n, int dir)
{
	long min, max, rmin, rmax;
	n = n < 0.0 ? 0.0 : n > 1.0 ? 1.0 : n;
	if (raw_range(e, pb, &rmin, &rmax) < 0 || rmin >= rmax)
		return -1;
	if (db_range(e, pb, &min, &max) < 0 || min >= max)
		return set_raw(e, pb, ch, round_dir(n * (double)(rmax - rmin), dir) + rmin);
	if (max - min <= LINEAR_DB)
		return set_db(e, pb, ch, round_dir(n * (double)(max - min), dir) + min, dir);
	if (min != SND_CTL_TLV_DB_GAIN_MUTE) {
		double nmin = exp10((double)(min - max) / 6000.0);
		n = n * (1.0 - nmin) + nmin;
	}
	if (n <= 0.0)
		return set_raw(e, pb, ch, rmin);
	return set_db(e, pb, ch, round_dir(6000.0 * log10(n), dir) + max, dir);
}

/* the control's channel for the device's channel i */
static int channel_for(snd_mixer_elem_t *e, bool pb, int i)
{
	return !is_mono(e, pb) && has_channel(e, pb, i) ? i : SND_MIXER_SCHN_MONO;
}

static snd_mixer_elem_t *find_control(snd_mixer_t *m, bool pb)
{
	static const char *const play[] = { "Master", "PCM", "Speaker", "Headphone", "Front", "Line Out", NULL };
	static const char *const cap[] = { "Capture", "Mic", "Line", "Digital", "Input", NULL };
	snd_mixer_selem_id_t *id;
	snd_mixer_selem_id_alloca(&id);
	for (const char *const *n = pb ? play : cap; *n; n++) {
		snd_mixer_selem_id_set_name(id, *n);
		snd_mixer_selem_id_set_index(id, 0);
		snd_mixer_elem_t *e = snd_mixer_find_selem(m, id);
		if (e && snd_mixer_selem_is_active(e) && has_volume(e, pb))
			return e;
	}
	for (snd_mixer_elem_t *e = snd_mixer_first_elem(m); e; e = snd_mixer_elem_next(e))
		if (snd_mixer_selem_is_active(e) && has_volume(e, pb) && !strstr(snd_mixer_selem_get_name(e), "Boost"))
			return e;
	return NULL;
}

/* -------------------------------------------------- a card's mixer, open */

static void close_mixer(struct shim_mixer *x)
{
	pa_mainloop_api *api = shim_context_api(x->c);
	for (int i = 0; i < x->n_io; i++)
		api->io_free(x->io[i]);
	x->n_io = 0;
	if (x->m)
		snd_mixer_close(x->m);
	x->m = NULL;
	x->play = x->cap = NULL;
}

/* a change on the card (made by anyone): subscribers hear of it */
static void on_mixer(pa_mainloop_api *a, pa_io_event *e, int fd, pa_io_event_flags_t f, void *userdata)
{
	struct shim_mixer *x = userdata;
	if ((f & (PA_IO_EVENT_HANGUP | PA_IO_EVENT_ERROR)) || snd_mixer_handle_events(x->m) < 0) {
		shim_log("%s: the card's mixer went away", x->ctl);
		close_mixer(x);		/* opened again after the devices change */
	}
	shim_context_mixer_changed(x->c, x->ctl);
}

static void quiet(const char *file, int line, const char *function, int err, const char *fmt, va_list arg)
{
}

static void open_mixer(struct shim_mixer *x)
{
	/* alsa-lib's own complaints (no such card, no access in a sandbox)
	 * stay out of the application's output: this thread's only */
	snd_local_error_handler_t old = snd_lib_error_set_local(quiet);
	snd_mixer_t *m = NULL;
	int err = snd_mixer_open(&m, 0);
	if (err >= 0 && (err = snd_mixer_attach(m, x->ctl)) >= 0 &&
	    (err = snd_mixer_selem_register(m, NULL, NULL)) >= 0)
		err = snd_mixer_load(m);
	snd_lib_error_set_local(old);
	if (err < 0) {
		shim_log("%s: no mixer (%s)", x->ctl, snd_strerror(err));
		if (m)
			snd_mixer_close(m);
		return;
	}
	x->m = m;
	x->play = find_control(m, true);
	x->cap = find_control(m, false);
	struct pollfd pfd[MAX_IO];
	int n = snd_mixer_poll_descriptors(m, pfd, MAX_IO);
	pa_mainloop_api *api = shim_context_api(x->c);
	for (int i = 0; i < n; i++) {
		pa_io_event *io = api->io_new(api, pfd[i].fd, PA_IO_EVENT_INPUT, on_mixer, x);
		if (io)
			x->io[x->n_io++] = io;
	}
	shim_log("%s: volume %s, capture %s", x->ctl, x->play ? snd_mixer_selem_get_name(x->play) : "(none)",
			x->cap ? snd_mixer_selem_get_name(x->cap) : "(none)");
}

static snd_mixer_elem_t *control_of(pa_context *c, const struct shim_device *d)
{
	if (d->ctl[0] == '\0')
		return NULL;
	struct shim_mixer **list = shim_context_mixers(c), *x;
	for (x = *list; x; x = x->next)
		if (strcmp(x->ctl, d->ctl) == 0)
			break;
	if (x == NULL) {
		x = calloc(1, sizeof(*x));
		if (x == NULL)
			return NULL;
		snprintf(x->ctl, sizeof(x->ctl), "%s", d->ctl);
		x->c = c;
		x->next = *list;
		*list = x;
		open_mixer(x);
	}
	return x->m ? (d->playback ? x->play : x->cap) : NULL;
}

/* ---------------------------------------------------------------- the API */

int shim_mixer_volume(pa_context *c, const struct shim_device *d, uint8_t channels, pa_cvolume *v, int *mute)
{
	snd_mixer_elem_t *e = control_of(c, d);
	bool pb = d->playback;
	*mute = 0;
	if (e == NULL)
		return 0;
	v->channels = channels;
	for (int i = 0; i < channels; i++)
		v->values[i] = (pa_volume_t)lrint(get_norm(e, pb, channel_for(e, pb, i)) * PA_VOLUME_NORM);
	if (!has_switch(e, pb))
		return SHIM_MIXER_VOLUME;
	int on = 1;
	int ch = channel_for(e, pb, 0);
	if (pb ? snd_mixer_selem_get_playback_switch(e, ch, &on) : snd_mixer_selem_get_capture_switch(e, ch, &on))
		on = 1;
	*mute = !on;
	return SHIM_MIXER_VOLUME | SHIM_MIXER_MUTE;
}

static int set_channel(snd_mixer_elem_t *e, bool pb, int ch, pa_volume_t pv)
{
	double want = (double)SHIM_MIN(pv, PA_VOLUME_NORM) / PA_VOLUME_NORM;
	double now = get_norm(e, pb, ch);
	if (fabs(want - now) < 1e-6)
		return 0;
	return set_norm(e, pb, ch, want, want > now ? 1 : -1);
}

int shim_mixer_set_volume(pa_context *c, const struct shim_device *d, const pa_cvolume *v)
{
	snd_mixer_elem_t *e = control_of(c, d);
	bool pb = d->playback;
	int err = 0;
	if (e == NULL)
		return -PA_ERR_NOTSUPPORTED;
	if (is_mono(e, pb))
		return set_channel(e, pb, SND_MIXER_SCHN_MONO, pa_cvolume_max(v)) < 0 ? -PA_ERR_IO : 0;
	for (int ch = 0; ch <= SND_MIXER_SCHN_LAST; ch++) {
		if (!has_channel(e, pb, ch))
			continue;
		pa_volume_t pv = v->values[v->channels == 1 ? 0 : SHIM_MIN(ch, v->channels - 1)];
		if (set_channel(e, pb, ch, pv) < 0)
			err = -PA_ERR_IO;
	}
	return err;
}

int shim_mixer_set_mute(pa_context *c, const struct shim_device *d, int mute)
{
	snd_mixer_elem_t *e = control_of(c, d);
	bool pb = d->playback;
	if (e == NULL || !has_switch(e, pb))
		return -PA_ERR_NOTSUPPORTED;
	int r = pb ? snd_mixer_selem_set_playback_switch_all(e, !mute) : snd_mixer_selem_set_capture_switch_all(e, !mute);
	return r < 0 ? -PA_ERR_IO : 0;
}

/* a subscriber to sinks or sources: the cards are watched from now on */
void shim_mixer_watch(pa_context *c, const struct shim_device *devices, int n)
{
	for (int i = 0; i < n; i++)
		control_of(c, &devices[i]);
}

/* the devices changed: a card no device has now is closed; one that didn't
 * open (or went away) is tried again on next use */
void shim_mixer_rescan(pa_context *c, const struct shim_device *devices, int n)
{
	struct shim_mixer **p = shim_context_mixers(c);
	while (*p) {
		struct shim_mixer *x = *p;
		bool used = false;
		for (int i = 0; i < n && !used; i++)
			used = strcmp(devices[i].ctl, x->ctl) == 0;
		if (used && x->m) {
			p = &x->next;
			continue;
		}
		*p = x->next;
		close_mixer(x);
		free(x);
	}
}

void shim_mixer_close_all(pa_context *c)
{
	shim_mixer_rescan(c, NULL, 0);
}
