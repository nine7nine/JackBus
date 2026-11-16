/*
 * A sound card's mixer for the tests, never a real one: an ALSA control
 * plugin ("jbtest") with the controls of an HDA card's (Master: 0..86,
 * -64.50..0 dB; Capture: 0..63, -17.25..+30 dB; a switch each, stereo).
 * The values are in a file (dir/<card>), so every process sees the same
 * ones; a change made by any process is an event for every other (inotify
 * on the file), as a card's would be.
 *
 * A test's own ALSA configuration (ALSA_CONFIG_PATH) puts it in place of
 * every card's control device:
 *
 *   ctl.hw { @args [ CARD ] @args.CARD { type string } type jbtest card $CARD dir "/tmp/x" }
 *   ctl_type.jbtest { lib "/path/libasound_module_ctl_jbtest.so" }
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <alsa/control_external.h>

enum { MVOL, MSW, CVOL, CSW, N_ELEMS };
static const char *const names[N_ELEMS] = {
	"Master Playback Volume", "Master Playback Switch", "Capture Volume", "Capture Switch",
};
static const long maxima[N_ELEMS] = { 86, 1, 63, 1 };
static const int db_min[N_ELEMS] = { -6450, 0, -1725, 0 };

struct fake {
	snd_ctl_ext_t ext;
	char path[PATH_MAX];
	int ifd;
	unsigned pending;
	long v[N_ELEMS * 2];
};

static void load(struct fake *f)
{
	long v[N_ELEMS * 2];
	FILE *fp = fopen(f->path, "r");
	if (fp == NULL)
		return;
	int i;
	for (i = 0; i < N_ELEMS * 2; i++)
		if (fscanf(fp, "%ld", &v[i]) != 1)
			break;
	fclose(fp);
	if (i == N_ELEMS * 2)
		memcpy(f->v, v, sizeof(v));
}

static void save(struct fake *f)
{
	FILE *fp = fopen(f->path, "w");
	if (fp == NULL)
		return;
	for (int i = 0; i < N_ELEMS * 2; i++)
		fprintf(fp, "%ld%c", f->v[i], i == N_ELEMS * 2 - 1 ? '\n' : ' ');
	fclose(fp);
}

static void fake_close(snd_ctl_ext_t *ext)
{
	struct fake *f = ext->private_data;
	close(f->ifd);
	free(f);
}

static int elem_count(snd_ctl_ext_t *ext)
{
	return N_ELEMS;
}

static int elem_list(snd_ctl_ext_t *ext, unsigned int offset, snd_ctl_elem_id_t *id)
{
	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, names[offset]);
	return 0;
}

static snd_ctl_ext_key_t find_elem(snd_ctl_ext_t *ext, const snd_ctl_elem_id_t *id)
{
	const char *n = snd_ctl_elem_id_get_name(id);
	for (int i = 0; i < N_ELEMS; i++)
		if (strcmp(n, names[i]) == 0)
			return i;
	return SND_CTL_EXT_KEY_NOT_FOUND;
}

static int get_attribute(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, int *type, unsigned int *acc,
		unsigned int *count)
{
	bool sw = key == MSW || key == CSW;
	*type = sw ? SND_CTL_ELEM_TYPE_BOOLEAN : SND_CTL_ELEM_TYPE_INTEGER;
	*acc = SND_CTL_EXT_ACCESS_READWRITE |
		(sw ? 0 : SND_CTL_EXT_ACCESS_TLV_READ | SND_CTL_EXT_ACCESS_TLV_CALLBACK);
	*count = 2;
	return 0;
}

static int get_integer_info(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *imin, long *imax, long *istep)
{
	*imin = 0;
	*imax = maxima[key];
	*istep = 1;
	return 0;
}

static int read_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value)
{
	struct fake *f = ext->private_data;
	load(f);
	value[0] = f->v[key * 2];
	value[1] = f->v[key * 2 + 1];
	return 0;
}

static int write_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value)
{
	struct fake *f = ext->private_data;
	load(f);
	int changed = 0;
	for (int i = 0; i < 2; i++) {
		long v = value[i] < 0 ? 0 : value[i] > maxima[key] ? maxima[key] : value[i];
		if (f->v[key * 2 + i] != v)
			changed = 1;
		f->v[key * 2 + i] = v;
	}
	if (changed)
		save(f);
	return changed;
}

static void subscribe_events(snd_ctl_ext_t *ext, int subscribe)
{
}

static int read_event(snd_ctl_ext_t *ext, snd_ctl_elem_id_t *id, unsigned int *event_mask)
{
	struct fake *f = ext->private_data;
	char buf[4096];
	while (read(f->ifd, buf, sizeof(buf)) > 0)
		f->pending = (1u << N_ELEMS) - 1;
	if (f->pending == 0)
		return -EAGAIN;
	int k = __builtin_ctz(f->pending);
	f->pending &= ~(1u << k);
	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, names[k]);
	*event_mask = SND_CTL_EVENT_MASK_VALUE;
	return 1;
}

static int tlv(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, int op_flag, unsigned int numid, unsigned int *t,
		unsigned int size)
{
	if (op_flag != 0)
		return -ENXIO;
	if (size < 4 * sizeof(unsigned int))
		return -ENOMEM;
	t[0] = SND_CTL_TLVT_DB_SCALE;
	t[1] = 2 * sizeof(unsigned int);
	t[2] = (unsigned int)db_min[key];
	t[3] = 75;		/* 0.75 dB a step */
	return 0;
}

static const snd_ctl_ext_callback_t callbacks = {
	.close = fake_close,
	.elem_count = elem_count,
	.elem_list = elem_list,
	.find_elem = find_elem,
	.get_attribute = get_attribute,
	.get_integer_info = get_integer_info,
	.read_integer = read_integer,
	.write_integer = write_integer,
	.subscribe_events = subscribe_events,
	.read_event = read_event,
};

SND_CTL_PLUGIN_DEFINE_FUNC(jbtest)
{
	snd_config_iterator_t i, next;
	const char *card = NULL, *dir = NULL;
	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *k;
		if (snd_config_get_id(n, &k) < 0)
			continue;
		if (strcmp(k, "card") == 0)
			snd_config_get_string(n, &card);
		else if (strcmp(k, "dir") == 0)
			snd_config_get_string(n, &dir);
	}
	if (card == NULL || dir == NULL)
		return -EINVAL;
	struct fake *f = calloc(1, sizeof(*f));
	if (f == NULL)
		return -ENOMEM;
	snprintf(f->path, sizeof(f->path), "%s/%s", dir, card);
	if (access(f->path, R_OK | W_OK) != 0) {
		free(f);
		return -ENODEV;		/* no such card */
	}
	f->ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	inotify_add_watch(f->ifd, f->path, IN_CLOSE_WRITE);
	load(f);
	f->ext.version = SND_CTL_EXT_VERSION;
	f->ext.card_idx = 0;
	snprintf(f->ext.id, sizeof(f->ext.id), "%s", card);
	snprintf(f->ext.driver, sizeof(f->ext.driver), "jbtest");
	snprintf(f->ext.name, sizeof(f->ext.name), "JackBus test mixer");
	snprintf(f->ext.longname, sizeof(f->ext.longname), "JackBus test mixer %s", card);
	snprintf(f->ext.mixername, sizeof(f->ext.mixername), "JackBus test mixer");
	f->ext.poll_fd = f->ifd;
	f->ext.callback = &callbacks;
	f->ext.private_data = f;
	f->ext.tlv.c = tlv;
	int err = snd_ctl_ext_create(&f->ext, name, mode);
	if (err < 0) {
		close(f->ifd);
		free(f);
		return err;
	}
	*handlep = f->ext.handle;
	return 0;
}

SND_CTL_PLUGIN_SYMBOL(jbtest);
