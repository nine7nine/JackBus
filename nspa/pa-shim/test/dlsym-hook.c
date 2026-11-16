/*
 * An interposed dlsym, as teams-sdl-cef's audio probe (libtnaudio.so) has:
 * asked for pa_context_new or pa_context_new_with_proplist, it hands back a
 * wrapper of its own, which calls "the real one" found with the real dlsym
 * (RTLD_NEXT: libpulse.so.0). LD_PRELOADed in front of a libpulse program,
 * it must not make jackbus-pa's dispatcher loop.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include <pulse/context.h>

static void *(*real_dlsym)(void *, const char *);
static int wrapped;

static void init(void)
{
	if (real_dlsym == NULL)
		real_dlsym = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
	if (real_dlsym == NULL)
		real_dlsym = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
}

static pa_context *w_context_new(pa_mainloop_api *api, const char *name)
{
	init();
	pa_context *(*real)(pa_mainloop_api *, const char *) =
		(pa_context *(*)(pa_mainloop_api *, const char *))real_dlsym(RTLD_NEXT, "pa_context_new");
	if (!wrapped++)
		fprintf(stderr, "dlsym-hook: pa_context_new wrapped\n");
	return real(api, name);
}

static pa_context *w_context_new_with_proplist(pa_mainloop_api *api, const char *name, const pa_proplist *p)
{
	init();
	pa_context *(*real)(pa_mainloop_api *, const char *, const pa_proplist *) =
		(pa_context *(*)(pa_mainloop_api *, const char *, const pa_proplist *))
		real_dlsym(RTLD_NEXT, "pa_context_new_with_proplist");
	if (!wrapped++)
		fprintf(stderr, "dlsym-hook: pa_context_new_with_proplist wrapped\n");
	return real(api, name, p);
}

void *dlsym(void *handle, const char *name)
{
	init();
	if (strcmp(name, "pa_context_new") == 0)
		return (void *)w_context_new;
	if (strcmp(name, "pa_context_new_with_proplist") == 0)
		return (void *)w_context_new_with_proplist;
	return real_dlsym(handle, name);
}
