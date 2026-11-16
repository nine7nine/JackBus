/*
 * Whether JackBus runs, for JackBus's compatibility layers: its socket,
 * $XDG_RUNTIME_DIR/jackbus/<server>, is there (the one libjack's JackBus
 * clients connect to; the server name as libjack makes it: JACK_DEFAULT_SERVER,
 * else "default", '/' and '\' as '_').
 *
 * Checked without libjack, which prints an error for every attempt that
 * fails: an application that retries while JackBus is down stays quiet, and
 * the layers that fall back to PipeWire can decide before opening anything.
 */
#ifndef JACKBUS_COMPAT_SERVER_H
#define JACKBUS_COMPAT_SERVER_H

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

/* server: the JACK server's name, NULL for the default one */
static inline bool jackbus_server_running(const char *name)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char path[PATH_MAX];
	struct stat st;

	if (name == NULL || name[0] == '\0')
		name = getenv("JACK_DEFAULT_SERVER");
	if (name == NULL || name[0] == '\0')
		name = "default";
	if (rt == NULL || rt[0] != '/')
		return false;			/* libjack has no JackBus socket then */
	int n = snprintf(path, sizeof(path), "%s/jackbus/", rt);
	if (n < 0 || (size_t)n >= sizeof(path))
		return false;
	for (const char *p = name; *p; p++) {
		if ((size_t)n + 1 >= sizeof(path))
			return false;
		path[n++] = (*p == '/' || *p == '\\') ? '_' : *p;
	}
	path[n] = '\0';
	return stat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

static inline bool jackbus_server_present(void)
{
	return jackbus_server_running(NULL);
}

#endif
