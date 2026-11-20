/*
 * jackbus-pa: the libpulse.so.0 applications load.
 *
 * Each function of libpulse's API (the whole of it: trampolines.S, generated
 * from PulseAudio's libpulse) is a jump through a table, filled once as the
 * library loads, before the application calls any of it, with one
 * implementation's functions for the life of the process:
 *
 *  - JackBus's (libjackbus-pa.so, beside this library, or the build named by
 *    JACKBUS_PA_IMPL, for testing): with
 *    JACKBUS_PA_MODE=jackbus (the jackbus-pa launcher), or when the user's
 *    setting says so ("pulse = yes" in ~/.config/jackbus/compat.conf,
 *    PatchBox's switch) and JackBus runs (its socket is there);
 *  - PulseAudio's own libpulse otherwise: PipeWire's PulseAudio server (or
 *    PulseAudio's) plays it, as without this library. Always with
 *    JACKBUS_PA_MODE=pulse, in PipeWire's own daemons (a PulseAudio tunnel
 *    there is PipeWire's), and when JackBus's library can't load.
 *
 * The functions are found in the chosen library's own symbol table (and in
 * libpulsecommon's, where libpulse's utility functions are for JackBus's),
 * not with dlsym: a program may interpose dlsym (teams-sdl-cef's audio
 * probe hands back its own wrappers for pa_ functions, which call "the real
 * one" through libpulse.so.0, this library: dlsym here made that a loop).
 *
 * The package puts this directory in the dynamic linker's configuration
 * (/etc/ld.so.conf.d/jackbus-pa.conf), so every program loading libpulse
 * gets this library, whatever library path its launcher gave it; with the
 * setting off (the default) each one uses PulseAudio's libpulse as before.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <limits.h>
#include <link.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../compat/compat_conf.h"
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

/* ------------------------------------------- symbols, without dlsym */

struct obj {
	ElfW(Addr) base;
	const ElfW(Sym) *sym;
	const char *str;
	const uint32_t *gnu;		/* DT_GNU_HASH */
	const ElfW(Word) *hash;		/* DT_HASH */
	const ElfW(Half) *versym;
};

static const void *dyn_ptr(ElfW(Addr) base, ElfW(Addr) p)
{
	/* the dynamic linker relocates these in place; if not, they are offsets */
	return (const void *)(p < base ? p + base : p);
}

static bool obj_init(struct obj *o, ElfW(Addr) base, const ElfW(Dyn) *dyn)
{
	memset(o, 0, sizeof(*o));
	o->base = base;
	for (const ElfW(Dyn) *d = dyn; d && d->d_tag != DT_NULL; d++) {
		switch (d->d_tag) {
		case DT_SYMTAB: o->sym = dyn_ptr(base, d->d_un.d_ptr); break;
		case DT_STRTAB: o->str = dyn_ptr(base, d->d_un.d_ptr); break;
		case DT_GNU_HASH: o->gnu = dyn_ptr(base, d->d_un.d_ptr); break;
		case DT_HASH: o->hash = dyn_ptr(base, d->d_un.d_ptr); break;
		case DT_VERSYM: o->versym = dyn_ptr(base, d->d_un.d_ptr); break;
		}
	}
	return o->sym && o->str && (o->gnu || o->hash);
}

/* a function this object defines (its default version) */
static void *obj_match(const struct obj *o, uint32_t i, const char *name)
{
	const ElfW(Sym) *s = &o->sym[i];
	if (s->st_shndx == SHN_UNDEF || s->st_value == 0 ||
	    (ELF64_ST_TYPE(s->st_info) != STT_FUNC && ELF64_ST_TYPE(s->st_info) != STT_GNU_IFUNC))
		return NULL;
	if (o->versym && (o->versym[i] & 0x8000))
		return NULL;			/* a hidden (older) version */
	if (strcmp(o->str + s->st_name, name) != 0)
		return NULL;
	return ELF64_ST_TYPE(s->st_info) == STT_GNU_IFUNC ? NULL : (void *)(o->base + s->st_value);
}

static void *obj_lookup(const struct obj *o, const char *name)
{
	if (o->gnu) {
		const uint32_t *g = o->gnu;
		uint32_t nbuckets = g[0], symoffset = g[1], bloom_size = g[2];
		const uint32_t *buckets = (const uint32_t *)((const ElfW(Addr) *)&g[4] + bloom_size);
		const uint32_t *chain = buckets + nbuckets;
		uint32_t h = 5381;
		for (const unsigned char *c = (const unsigned char *)name; *c; c++)
			h = h * 33 + *c;
		if (nbuckets == 0)
			return NULL;
		uint32_t i = buckets[h % nbuckets];
		if (i < symoffset)
			return NULL;
		for (;; i++) {
			uint32_t h2 = chain[i - symoffset];
			if ((h | 1) == (h2 | 1)) {
				void *f = obj_match(o, i, name);
				if (f)
					return f;
			}
			if (h2 & 1)
				return NULL;
		}
	}
	for (uint32_t i = 0; i < o->hash[1]; i++) {	/* nchain: the number of symbols */
		void *f = obj_match(o, i, name);
		if (f)
			return f;
	}
	return NULL;
}

/* libpulsecommon, loaded with JackBus's library: where libpulse's utility
 * functions are */
static int find_common(struct dl_phdr_info *info, size_t size, void *data)
{
	struct obj *o = data;
	if (info->dlpi_name == NULL || strstr(info->dlpi_name, "/libpulsecommon-") == NULL)
		return 0;
	for (int i = 0; i < info->dlpi_phnum; i++)
		if (info->dlpi_phdr[i].p_type == PT_DYNAMIC)
			return obj_init(o, info->dlpi_addr,
					(const ElfW(Dyn) *)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr)) ? 1 : 0;
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
	/* a build of JackBus's library to try instead of the installed one (testing:
	 * it reaches processes whose launcher sets its own preloads and library path) */
	const char *impl = getenv("JACKBUS_PA_IMPL");
	if (impl && impl[0] == '/') {
		snprintf(used, len, "%s", impl);
		void *h = dlopen(used, RTLD_NOW | RTLD_LOCAL);
		if (h)
			return h;
		say(true, "JACKBUS_PA_IMPL=%s doesn't load (%s): the installed library", impl, dlerror());
	}
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
	} else if (!jackbus_compat_bool("pulse", NULL, false)) {
		jackbus = false;
		why = "the setting is off";
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

	struct obj objs[2];
	int n_objs = 0;
	struct link_map *lm = NULL;
	if (h && dlinfo(h, RTLD_DI_LINKMAP, &lm) == 0 && lm && obj_init(&objs[n_objs], lm->l_addr, lm->l_ld))
		n_objs++;
	if (h && dl_iterate_phdr(find_common, &objs[n_objs]) > 0)
		n_objs++;
	unsigned int lacking = 0;
	for (unsigned int i = 0; i < JB_PA_COUNT; i++) {
		void *f = NULL;
		for (int k = 0; k < n_objs && f == NULL; k++)
			f = obj_lookup(&objs[k], names[i]);
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
