/*
 * JackBus's shims (jackbus-pw, jackbus-pa, jackbus-alsa): their shared
 * settings, a small file of "key = value" lines ('#' starts a comment):
 *
 *   $XDG_CONFIG_HOME/jackbus/compat.conf (else ~/.config/jackbus/compat.conf),
 *   then /etc/jackbus/compat.conf
 *
 * Each shim reads its own key first, "<shim>.<key>" (pw., pa. or alsa.: PatchBox
 * writes these, a section per kind of application), then the shared "<key>":
 *
 *   resample = yes          a stream at a rate other than JackBus's is resampled
 *                           (after the direct route was looked for); no: refused
 *   resample-quality = normal   low, normal or high (JackBus's bridge resampler:
 *                           32, 56 or 96 taps)
 *   autoconnect = yes       new streams connect to the output (input) device
 *   output = <client>       the JACK client new playback streams go to, and
 *                           PulseAudio's default sink (default: the first with
 *                           physical playback ports, system)
 *   input = <client>        the same for recording
 *   pulse = no              yes: PulseAudio applications (every program loading
 *                           libpulse) use JackBus while it runs
 *   max-latency = 0         ms: the most a stream may buffer in the shim (0: what
 *                           it asks for; PulseAudio's streams)
 *
 * An environment variable given by the caller overrides the file (per layer,
 * e.g. JACKBUS_PW_RESAMPLE=0). JACKBUS_COMPAT_CONF names the one file to read
 * instead of both (the tests: settings of their own). Read when a stream is
 * set up, never in a realtime thread.
 */
#ifndef JACKBUS_COMPAT_CONF_H
#define JACKBUS_COMPAT_CONF_H

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the shim's name, given when it is built (-DJACKBUS_COMPAT_LAYER='"pa"') */
#ifndef JACKBUS_COMPAT_LAYER
#define JACKBUS_COMPAT_LAYER ""
#endif

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

/* the value of key in one file (its last line; "<shim>.<key>" over "<key>"),
 * trimmed: true when set there */
static inline bool jackbus_compat_file_str(const char *path, const char *key, char *buf, size_t len)
{
	FILE *f = fopen(path, "re");
	char line[512], layered[96];
	bool found = false, found_layered = false;
	if (f == NULL)
		return false;
	snprintf(layered, sizeof(layered), "%s.%s", JACKBUS_COMPAT_LAYER, key);
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
		if (*v == 0)
			continue;
		if (JACKBUS_COMPAT_LAYER[0] && strcmp(p, layered) == 0) {
			snprintf(buf, len, "%s", v);
			found = found_layered = true;
		} else if (!found_layered && strcmp(p, key) == 0) {
			snprintf(buf, len, "%s", v);
			found = true;
		}
	}
	fclose(f);
	return found;
}

/* a setting's text: the environment variable env (if given and set), else the
 * user's file, else the system's; true when set somewhere */
static inline bool jackbus_compat_str(const char *key, const char *env, char *buf, size_t len)
{
	const char *e = env ? getenv(env) : NULL;
	if (e && e[0]) {
		snprintf(buf, len, "%s", e);
		return true;
	}
	char path[512];
	const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	const char *only = getenv("JACKBUS_COMPAT_CONF");
	if (only && only[0])
		return jackbus_compat_file_str(only, key, buf, len);
	if (xdg && xdg[0])
		snprintf(path, sizeof(path), "%s/jackbus/compat.conf", xdg);
	else if (home && home[0])
		snprintf(path, sizeof(path), "%s/.config/jackbus/compat.conf", home);
	else
		path[0] = 0;
	if (path[0] && jackbus_compat_file_str(path, key, buf, len))
		return true;
	return jackbus_compat_file_str("/etc/jackbus/compat.conf", key, buf, len);
}

static inline bool jackbus_compat_bool(const char *key, const char *env, bool def)
{
	char v[64];
	int b;
	if (jackbus_compat_str(key, env, v, sizeof(v)) && (b = jackbus_compat_parse_bool(v)) >= 0)
		return b;
	return def;
}

static inline long jackbus_compat_int(const char *key, const char *env, long def)
{
	char v[64], *end;
	if (!jackbus_compat_str(key, env, v, sizeof(v)))
		return def;
	long n = strtol(v, &end, 10);
	return end != v ? n : def;
}

/* the resampler's quality: 0 low, 1 normal, 2 high */
static inline int jackbus_compat_quality(void)
{
	char v[64];
	if (!jackbus_compat_str("resample-quality", "JACKBUS_RESAMPLE_QUALITY", v, sizeof(v)))
		return 1;
	if (strcasecmp(v, "low") == 0 || strcmp(v, "0") == 0)
		return 0;
	if (strcasecmp(v, "high") == 0 || strcmp(v, "2") == 0)
		return 2;
	return 1;
}

/* the JACK client new streams go to (output or input), "" when not set */
static inline void jackbus_compat_device(bool playback, char *buf, size_t len)
{
	if (!jackbus_compat_str(playback ? "output" : "input", playback ? "JACKBUS_OUTPUT" : "JACKBUS_INPUT", buf, len))
		buf[0] = 0;
}

#endif
