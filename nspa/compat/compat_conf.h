/*
 * JackBus's compatibility layers (jackbus-pw, jackbus-pa, jackbus-alsa): their
 * shared settings, a small file of "key = value" lines ('#' starts a comment):
 *
 *   $XDG_CONFIG_HOME/jackbus/compat.conf (else ~/.config/jackbus/compat.conf),
 *   then /etc/jackbus/compat.conf
 *
 *   resample = yes      a stream at a rate other than JackBus's is resampled
 *                       (after the direct route was looked for); no: refused
 *   autoconnect = yes   new streams connect to the default device
 *
 * An environment variable given by the caller overrides the file (per layer,
 * e.g. JACKBUS_PW_RESAMPLE=0). Read when a stream is set up, never in a
 * realtime thread.
 */
#ifndef JACKBUS_COMPAT_CONF_H
#define JACKBUS_COMPAT_CONF_H

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline int jackbus_compat_parse_bool(const char *v)
{
	if (strcasecmp(v, "yes") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "on") == 0 ||
	    strcmp(v, "1") == 0)
		return 1;
	if (strcasecmp(v, "no") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "off") == 0 ||
	    strcmp(v, "0") == 0)
		return 0;
	return -1;
}

/* the value of key in one file: 1, 0, or -1 when not set there */
static inline int jackbus_compat_file_bool(const char *path, const char *key)
{
	FILE *f = fopen(path, "re");
	char line[256];
	int res = -1;
	if (f == NULL)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char *p = line, *eq, *v, *end;
		if ((end = strchr(p, '#')))
			*end = 0;
		if ((eq = strchr(p, '=')) == NULL)
			continue;
		*eq = 0;
		v = eq + 1;
		while (isspace((unsigned char)*p))
			p++;
		for (end = p + strlen(p); end > p && isspace((unsigned char)end[-1]); end--)
			*--end = 0;
		while (isspace((unsigned char)*v))
			v++;
		for (end = v + strlen(v); end > v && isspace((unsigned char)end[-1]); end--)
			*--end = 0;
		if (strcmp(p, key) == 0) {
			int b = jackbus_compat_parse_bool(v);
			if (b >= 0)
				res = b;
		}
	}
	fclose(f);
	return res;
}

static inline bool jackbus_compat_bool(const char *key, const char *env, bool def)
{
	const char *e = env ? getenv(env) : NULL;
	int b;
	if (e && e[0] && (b = jackbus_compat_parse_bool(e)) >= 0)
		return b;
	char path[512];
	const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	if (xdg && xdg[0])
		snprintf(path, sizeof(path), "%s/jackbus/compat.conf", xdg);
	else if (home && home[0])
		snprintf(path, sizeof(path), "%s/.config/jackbus/compat.conf", home);
	else
		path[0] = 0;
	if (path[0] && (b = jackbus_compat_file_bool(path, key)) >= 0)
		return b;
	if ((b = jackbus_compat_file_bool("/etc/jackbus/compat.conf", key)) >= 0)
		return b;
	return def;
}

#endif
