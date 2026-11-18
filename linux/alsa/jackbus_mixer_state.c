/*
 * JackBus: a sound card's mixer state, the user's own (jackbus_mixer_state.h).
 */
#define _GNU_SOURCE
#include "jackbus_mixer_state.h"
#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int jackbus_mixer_card_id(const char *device, char *id, size_t len)
{
	char name[64];
	const char *p, *card = strstr(device ? device : "", "CARD=");
	if (card) {
		p = card + 5;
	} else if ((p = strchr(device ? device : "", ':'))) {
		p++;
	} else {
		return -1;
	}
	size_t n = strcspn(p, ",");
	if (n == 0 || n >= sizeof(name))
		return -1;
	memcpy(name, p, n);
	name[n] = 0;
	int index = snd_card_get_index(name);
	if (index < 0)
		return -1;
	/* the card's id as the kernel has it (and alsamixer, /proc/asound/cards) */
	char proc[64];
	snprintf(proc, sizeof(proc), "/proc/asound/card%d/id", index);
	FILE *f = fopen(proc, "re");
	if (f) {
		char line[64];
		bool ok = fgets(line, sizeof(line), f) != NULL;
		fclose(f);
		line[ok ? strcspn(line, "\n") : 0] = 0;
		if (ok && line[0]) {
			snprintf(id, len, "%s", line);
			return index;
		}
	}
	char hw[16];
	snprintf(hw, sizeof(hw), "hw:%d", index);
	snd_ctl_t *ctl;
	if (snd_ctl_open(&ctl, hw, 0) < 0)
		return -1;
	snd_ctl_card_info_t *info;
	snd_ctl_card_info_alloca(&info);
	int r = snd_ctl_card_info(ctl, info);
	if (r == 0)
		snprintf(id, len, "%s", snd_ctl_card_info_get_id(info));
	snd_ctl_close(ctl);
	return r == 0 ? index : -1;
}

int jackbus_mixer_state_path(const char *id, char *path, size_t len)
{
	const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	int n;
	if (xdg && xdg[0])
		n = snprintf(path, len, "%s/jackbus/mixer/%s.state", xdg, id);
	else if (home && home[0])
		n = snprintf(path, len, "%s/.config/jackbus/mixer/%s.state", home, id);
	else
		return -1;
	return (n > 0 && (size_t)n < len) ? 0 : -1;
}

/* the card's simple mixer, loaded */
static int open_mixer(const char *device, snd_mixer_t **mixer, char *id, size_t idlen)
{
	int index = jackbus_mixer_card_id(device, id, idlen);
	if (index < 0)
		return -ENODEV;
	char hw[64];
	snprintf(hw, sizeof(hw), "hw:%d", index);
	int r;
	if ((r = snd_mixer_open(mixer, 0)) < 0)
		return r;
	if ((r = snd_mixer_attach(*mixer, hw)) < 0 || (r = snd_mixer_selem_register(*mixer, NULL, NULL)) < 0 ||
	    (r = snd_mixer_load(*mixer)) < 0) {
		snd_mixer_close(*mixer);
		return r;
	}
	return 0;
}

static void mkdirs(const char *path)
{
	char dir[4096];
	snprintf(dir, sizeof(dir), "%s", path);
	for (char *p = dir + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(dir, 0755);
			*p = '/';
		}
	}
}

static void save_values(FILE *f, snd_mixer_elem_t *e, const char *what, int pb, int sw)
{
	const char *name = snd_mixer_selem_get_name(e);
	unsigned idx = snd_mixer_selem_get_index(e);
	bool any = false;
	for (int ch = 0; ch <= SND_MIXER_SCHN_LAST; ch++) {
		if (!(pb ? snd_mixer_selem_has_playback_channel(e, ch) : snd_mixer_selem_has_capture_channel(e, ch)))
			continue;
		long v = 0;
		int iv = 0, r;
		if (sw) {
			r = pb ? snd_mixer_selem_get_playback_switch(e, ch, &iv) : snd_mixer_selem_get_capture_switch(e, ch, &iv);
			v = iv;
		} else {
			r = pb ? snd_mixer_selem_get_playback_volume(e, ch, &v) : snd_mixer_selem_get_capture_volume(e, ch, &v);
		}
		if (r < 0)
			continue;
		if (!any)
			fprintf(f, "%s|%u|%s|", name, idx, what);
		fprintf(f, "%s%d:%ld", any ? " " : "", ch, v);
		any = true;
	}
	if (any)
		fputc('\n', f);
}

int jackbus_mixer_save(const char *device)
{
	snd_mixer_t *mixer;
	char id[64], path[4096], tmp[4200];
	int r = open_mixer(device, &mixer, id, sizeof(id));
	if (r < 0)
		return r;
	if (jackbus_mixer_state_path(id, path, sizeof(path)) < 0) {
		snd_mixer_close(mixer);
		return -ENOENT;
	}
	mkdirs(path);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "we");
	if (f == NULL) {
		r = -errno;
		snd_mixer_close(mixer);
		return r;
	}
	time_t now = time(NULL);
	char when[64];
	strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&now));
	fprintf(f, "# JackBus: the mixer of card %s, saved %s. JackBus puts it back whenever it\n"
		   "# opens the card. name|index|what|channel:value (raw) ...\n", id, when);
	for (snd_mixer_elem_t *e = snd_mixer_first_elem(mixer); e; e = snd_mixer_elem_next(e)) {
		if (!snd_mixer_selem_is_active(e))
			continue;
		if (snd_mixer_selem_is_enumerated(e)) {
			unsigned item;
			char iname[128];
			if (snd_mixer_selem_get_enum_item(e, 0, &item) == 0 &&
			    snd_mixer_selem_get_enum_item_name(e, item, sizeof(iname), iname) == 0)
				fprintf(f, "%s|%u|enum|%s\n", snd_mixer_selem_get_name(e), snd_mixer_selem_get_index(e), iname);
			continue;
		}
		if (snd_mixer_selem_has_playback_volume(e))
			save_values(f, e, "pvol", 1, 0);
		if (snd_mixer_selem_has_playback_switch(e))
			save_values(f, e, "psw", 1, 1);
		if (snd_mixer_selem_has_capture_volume(e))
			save_values(f, e, "cvol", 0, 0);
		if (snd_mixer_selem_has_capture_switch(e))
			save_values(f, e, "csw", 0, 1);
	}
	snd_mixer_close(mixer);
	if (fclose(f) != 0 || rename(tmp, path) != 0) {
		r = -errno;
		unlink(tmp);
		return r;
	}
	return 0;
}

static int restore_line(snd_mixer_t *mixer, char *line)
{
	char *name = line, *bar1 = strchr(line, '|');
	if (!bar1)
		return 0;
	*bar1 = 0;
	char *idx = bar1 + 1, *bar2 = strchr(idx, '|');
	if (!bar2)
		return 0;
	*bar2 = 0;
	char *what = bar2 + 1, *bar3 = strchr(what, '|');
	if (!bar3)
		return 0;
	*bar3 = 0;
	char *vals = bar3 + 1;
	vals[strcspn(vals, "\n")] = 0;

	snd_mixer_selem_id_t *sid;
	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_name(sid, name);
	snd_mixer_selem_id_set_index(sid, (unsigned)atoi(idx));
	snd_mixer_elem_t *e = snd_mixer_find_selem(mixer, sid);
	if (e == NULL)
		return 0;		/* the card no longer has it */

	if (strcmp(what, "enum") == 0) {
		int n = snd_mixer_selem_get_enum_items(e);
		for (int i = 0; i < n; i++) {
			char iname[128];
			if (snd_mixer_selem_get_enum_item_name(e, (unsigned)i, sizeof(iname), iname) == 0 &&
			    strcmp(iname, vals) == 0) {
				for (int ch = 0; ch <= SND_MIXER_SCHN_LAST; ch++)
					if (snd_mixer_selem_set_enum_item(e, ch, (unsigned)i) < 0 && ch > 0)
						break;
				return 1;
			}
		}
		return 0;
	}
	bool pb = what[0] == 'p', sw = strstr(what, "sw") != NULL;
	long lo = 0, hi = 0;
	if (!sw && (pb ? snd_mixer_selem_get_playback_volume_range(e, &lo, &hi) :
			 snd_mixer_selem_get_capture_volume_range(e, &lo, &hi)) < 0)
		return 0;
	int done = 0;
	for (char *tok = strtok(vals, " "); tok; tok = strtok(NULL, " ")) {
		int ch;
		long v;
		if (sscanf(tok, "%d:%ld", &ch, &v) != 2 || ch < 0 || ch > SND_MIXER_SCHN_LAST)
			continue;
		if (!(pb ? snd_mixer_selem_has_playback_channel(e, ch) : snd_mixer_selem_has_capture_channel(e, ch)))
			continue;
		int r;
		if (sw) {
			r = pb ? snd_mixer_selem_set_playback_switch(e, ch, v != 0) : snd_mixer_selem_set_capture_switch(e, ch, v != 0);
		} else {
			v = v < lo ? lo : v > hi ? hi : v;
			r = pb ? snd_mixer_selem_set_playback_volume(e, ch, v) : snd_mixer_selem_set_capture_volume(e, ch, v);
		}
		done |= r == 0;
	}
	return done;
}

int jackbus_mixer_restore(const char *device)
{
	char id[64], path[4096];
	if (jackbus_mixer_card_id(device, id, sizeof(id)) < 0 || jackbus_mixer_state_path(id, path, sizeof(path)) < 0)
		return -ENODEV;
	FILE *f = fopen(path, "re");
	if (f == NULL)
		return errno == ENOENT ? 0 : -errno;
	snd_mixer_t *mixer;
	int r = open_mixer(device, &mixer, id, sizeof(id));
	if (r < 0) {
		fclose(f);
		return r;
	}
	char line[1024];
	int count = 0;
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '#' || line[0] == '\n')
			continue;
		count += restore_line(mixer, line);
	}
	fclose(f);
	snd_mixer_close(mixer);
	return count;
}
