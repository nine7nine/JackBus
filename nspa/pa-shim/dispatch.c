/*
 * jackbus-pa: the libpulse.so.0 applications load.
 *
 * Each function of libpulse's API (the whole of it: trampolines.S, generated
 * from PulseAudio's libpulse) is a jump through a table, filled once as the
 * library loads, before the application calls any of it, with one
 * implementation's functions for the life of the process:
 *
 *  - JackBus's (libjackbus-pa.so, beside this library) when JackBus runs
 *    (its socket is there), and always with JACKBUS_PA_MODE=jackbus (the
 *    jackbus-pa launcher);
 *  - PulseAudio's own libpulse otherwise: PipeWire's PulseAudio server (or
 *    PulseAudio's) plays it. Always with JACKBUS_PA_MODE=pulse, in
 *    PipeWire's own daemons (a PulseAudio tunnel there is PipeWire's), and
 *    when JackBus's library can't load.
 *
 * So with this directory first on the session's library path, a PulseAudio
 * application goes to JackBus while it runs and to PipeWire while it
 * doesn't, decided as the application starts.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../compat/server.h"
#include "names.h"

__attribute__((visibility("hidden"))) void *jb_pa_table[JB_PA_COUNT];
static const char *const names[JB_PA_COUNT] = { JB_PA_NAMES };

#ifndef PULSE_REAL_LIB
#define PULSE_REAL_LIB "/usr/lib/libpulse.so.0"
#endif

static bool verbose;

static void say(bool always, const char *fmt, ...)
{
	if (!always && !verbose)
		return;
	va_list ap;
	va_start(ap, fmt);
	fputs("jackbus-pa: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* a function neither implementation has (not expected: the tests check
 * every installed libpulse user): what a call gets is 0 */
static long missing(void)
{
	return 0;
}

/* this library's file, resolved */
static char *own_file(void)
{
	Dl_info info;
	if (dladdr((void *)own_file, &info) == 0 || info.dli_fname == NULL)
		return NULL;
	return realpath(info.dli_fname, NULL);
}

static bool pipewire_daemon(void)
{
	const char *n = program_invocation_short_name;
	return strcmp(n, "pipewire") == 0 || strncmp(n, "pipewire-", 9) == 0 || strcmp(n, "wireplumber") == 0;
}

static void *open_jackbus(const char *self, char *used, size_t len)
{
	const char *slash = self ? strrchr(self, '/') : NULL;
	if (slash == NULL)
		return NULL;
	snprintf(used, len, "%.*s/libjackbus-pa.so", (int)(slash - self), self);
	void *h = dlopen(used, RTLD_NOW | RTLD_LOCAL);
	if (h == NULL)
		say(true, "JackBus's libpulse doesn't load (%s): PulseAudio's instead", dlerror());
	return h;
}

static void *open_pulse(const char *self, char *used, size_t len)
{
	const char *cand[] = { getenv("JACKBUS_PA_REAL"), PULSE_REAL_LIB, "/usr/lib/libpulse.so.0",
		"/usr/lib64/libpulse.so.0", "/usr/lib/x86_64-linux-gnu/libpulse.so.0" };
	for (size_t i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
		if (cand[i] == NULL || cand[i][0] != '/')
			continue;
		char *real = realpath(cand[i], NULL);
		bool ours = real == NULL || (self && strcmp(real, self) == 0);
		if (!ours) {
			void *h = dlopen(real, RTLD_NOW | RTLD_LOCAL);
			if (h) {
				snprintf(used, len, "%s", real);
				free(real);
				return h;
			}
		}
		free(real);
	}
	return NULL;
}

__attribute__((constructor)) static void choose(void)
{
	const char *v = getenv("JACKBUS_PA_VERBOSE");
	verbose = v && v[0] && strcmp(v, "0") != 0;
	const char *mode = getenv("JACKBUS_PA_MODE");
	bool jackbus;
	const char *why;
	if (mode && strcmp(mode, "jackbus") == 0) {
		jackbus = true;
		why = "JACKBUS_PA_MODE=jackbus";
	} else if (mode && strcmp(mode, "pulse") == 0) {
		jackbus = false;
		why = "JACKBUS_PA_MODE=pulse";
	} else if (pipewire_daemon()) {
		jackbus = false;
		why = "a PipeWire daemon";
	} else {
		jackbus = jackbus_server_present();
		why = jackbus ? "JackBus runs" : "JackBus isn't running";
	}

	char *self = own_file();
	char used[PATH_MAX] = "";
	void *h = jackbus ? open_jackbus(self, used, sizeof(used)) : NULL;
	if (h == NULL) {
		h = open_pulse(self, used, sizeof(used));
		if (h)
			jackbus = false;
		else if (!jackbus) {
			/* no PulseAudio library: JackBus's after all (it says why it can't connect) */
			h = open_jackbus(self, used, sizeof(used));
			jackbus = h != NULL;
		}
	}
	free(self);

	unsigned int lacking = 0;
	for (unsigned int i = 0; i < JB_PA_COUNT; i++) {
		void *f = h ? dlsym(h, names[i]) : NULL;
		if (f == NULL)
			lacking++;
		jb_pa_table[i] = f ? f : (void *)missing;
	}
	if (h == NULL)
		say(true, "neither JackBus's libpulse nor PulseAudio's loads: no audio");
	else
		say(false, "%s (%s): %s", jackbus ? "JackBus" : "PulseAudio's libpulse", why, used);
	if (lacking && h)
		say(true, "%u libpulse functions missing in %s", lacking, used);
}
