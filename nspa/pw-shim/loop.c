/*
 * jackbus-pw: loops.
 *
 * Applications call pw_loop_* inline: those functions dispatch through the
 * spa_loop / spa_loop_control / spa_loop_utils / spa_system method tables
 * that struct pw_loop points to. The shim gets real implementations of
 * those interfaces from PipeWire's support plugin (libspa-support.so,
 * factories "support.system" and "support.loop"), which has no dependency
 * on libpipewire or on the PipeWire daemon. The loops run only the
 * application's control side; audio runs in the JACK process thread.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>

#define SHIM_EXPORT __attribute__((visibility("default")))
#define PW_API_LOOP_IMPL SHIM_EXPORT
#include <pipewire/loop.h>

#include <spa/utils/names.h>
#include <spa/utils/result.h>

#include "shim.h"

#ifndef SHIM_SPA_PLUGINDIR
#define SHIM_SPA_PLUGINDIR "/usr/lib/spa-0.2"
#endif

/* ------------------------------------------------------ SPA support lib */

static pthread_mutex_t lib_lock = PTHREAD_MUTEX_INITIALIZER;
static void *support_lib;
static spa_handle_factory_enum_func_t support_enum;

static int open_support_lib(void)
{
	const char *dir = getenv("SPA_PLUGIN_DIR");
	char path[PATH_MAX];

	if (support_lib)
		return 0;
	if (dir == NULL || dir[0] == 0)
		dir = SHIM_SPA_PLUGINDIR;
	/* SPA_PLUGIN_DIR may be a list; the first entry is enough here */
	snprintf(path, sizeof(path), "%.*s/support/libspa-support.so",
			(int)strcspn(dir, ":"), dir);
	support_lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (support_lib == NULL) {
		shim_warn("can't load %s: %s", path, dlerror());
		return -ENOENT;
	}
	support_enum = dlsym(support_lib, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	if (support_enum == NULL) {
		shim_warn("%s has no %s", path, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
		dlclose(support_lib);
		support_lib = NULL;
		return -ENOENT;
	}
	return 0;
}

struct spa_handle *shim_load_spa_handle(const char *factory_name,
		const struct spa_dict *info,
		const struct spa_support *support, uint32_t n_support)
{
	const struct spa_handle_factory *f;
	struct spa_handle *handle = NULL;
	uint32_t index = 0;
	int res;

	pthread_mutex_lock(&lib_lock);
	res = open_support_lib();
	pthread_mutex_unlock(&lib_lock);
	if (res < 0) {
		errno = -res;
		return NULL;
	}
	while ((res = support_enum(&f, &index)) > 0) {
		if (strcmp(f->name, factory_name) != 0)
			continue;
		handle = calloc(1, spa_handle_factory_get_size(f, info));
		if (handle == NULL)
			return NULL;
		if ((res = spa_handle_factory_init(f, handle, info, support, n_support)) < 0) {
			free(handle);
			errno = -res;
			return NULL;
		}
		return handle;
	}
	errno = ENOENT;
	return NULL;
}

void shim_unload_spa_handle(struct spa_handle *handle)
{
	if (handle == NULL)
		return;
	spa_handle_clear(handle);
	free(handle);
}

SHIM_EXPORT struct spa_handle *pw_load_spa_handle(const char *lib,
		const char *factory_name, const struct spa_dict *info,
		uint32_t n_support, const struct spa_support support[])
{
	(void)lib;
	return shim_load_spa_handle(factory_name, info, support, n_support);
}

SHIM_EXPORT int pw_unload_spa_handle(struct spa_handle *handle)
{
	shim_unload_spa_handle(handle);
	return 0;
}

SHIM_EXPORT uint32_t pw_get_support(struct spa_support *support, uint32_t max_support)
{
	(void)support; (void)max_support;
	return 0;
}

/* --------------------------------------------------------------- pw_loop */

struct loop_impl {
	struct pw_loop this;
	char name[16];
	struct spa_handle *system_handle;
	struct spa_handle *loop_handle;
};

SHIM_EXPORT struct pw_loop *pw_loop_new(const struct spa_dict *props)
{
	struct loop_impl *impl;
	struct spa_support support[2];
	void *iface;
	const char *str;
	int res;

	if ((impl = calloc(1, sizeof(*impl))) == NULL)
		return NULL;

	impl->system_handle = shim_load_spa_handle(SPA_NAME_SUPPORT_SYSTEM, props, NULL, 0);
	if (impl->system_handle == NULL)
		goto error;
	if ((res = spa_handle_get_interface(impl->system_handle,
					SPA_TYPE_INTERFACE_System, &iface)) < 0)
		goto error;
	impl->this.system = iface;
	support[0] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_System, iface);

	impl->loop_handle = shim_load_spa_handle(SPA_NAME_SUPPORT_LOOP, props, support, 1);
	if (impl->loop_handle == NULL)
		goto error;
	if (spa_handle_get_interface(impl->loop_handle, SPA_TYPE_INTERFACE_Loop, &iface) < 0)
		goto error;
	impl->this.loop = iface;
	if (spa_handle_get_interface(impl->loop_handle, SPA_TYPE_INTERFACE_LoopControl, &iface) < 0)
		goto error;
	impl->this.control = iface;
	if (spa_handle_get_interface(impl->loop_handle, SPA_TYPE_INTERFACE_LoopUtils, &iface) < 0)
		goto error;
	impl->this.utils = iface;

	str = props ? spa_dict_lookup(props, PW_KEY_LOOP_NAME) : NULL;
	snprintf(impl->name, sizeof(impl->name), "%s", str ? str : "main-loop");
	impl->this.name = impl->name;
	return &impl->this;

error:
	shim_warn("can't create a loop: %m");
	shim_unload_spa_handle(impl->loop_handle);
	shim_unload_spa_handle(impl->system_handle);
	free(impl);
	return NULL;
}

SHIM_EXPORT void pw_loop_destroy(struct pw_loop *loop)
{
	struct loop_impl *impl = SPA_CONTAINER_OF(loop, struct loop_impl, this);
	if (loop == NULL)
		return;
	shim_unload_spa_handle(impl->loop_handle);
	shim_unload_spa_handle(impl->system_handle);
	free(impl);
}

SHIM_EXPORT int pw_loop_set_name(struct pw_loop *loop, const char *name)
{
	struct loop_impl *impl = SPA_CONTAINER_OF(loop, struct loop_impl, this);
	snprintf(impl->name, sizeof(impl->name), "%s", name);
	return 0;
}

/* ---------------------------------------------------------- pw_main_loop */

struct pw_main_loop {
	struct pw_loop *loop;
	struct spa_hook_list listener_list;
	bool created;
	bool running;
};

SHIM_EXPORT struct pw_main_loop *pw_main_loop_new(const struct spa_dict *props)
{
	struct pw_main_loop *this = calloc(1, sizeof(*this));
	if (this == NULL)
		return NULL;
	if ((this->loop = pw_loop_new(props)) == NULL) {
		free(this);
		return NULL;
	}
	this->created = true;
	spa_hook_list_init(&this->listener_list);
	return this;
}

SHIM_EXPORT void pw_main_loop_add_listener(struct pw_main_loop *loop,
		struct spa_hook *listener, const struct pw_main_loop_events *events, void *data)
{
	spa_hook_list_append(&loop->listener_list, listener, events, data);
}

SHIM_EXPORT struct pw_loop *pw_main_loop_get_loop(struct pw_main_loop *loop)
{
	return loop->loop;
}

SHIM_EXPORT void pw_main_loop_destroy(struct pw_main_loop *loop)
{
	if (loop == NULL)
		return;
	spa_hook_list_call(&loop->listener_list, struct pw_main_loop_events, destroy, 0);
	if (loop->created)
		pw_loop_destroy(loop->loop);
	spa_hook_list_clean(&loop->listener_list);
	free(loop);
}

static int do_main_stop(struct spa_loop *l, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct pw_main_loop *this = user_data;
	this->running = false;
	return 0;
}

SHIM_EXPORT int pw_main_loop_quit(struct pw_main_loop *loop)
{
	/* may be called from a signal handler's loop callback or another thread */
	return pw_loop_invoke(loop->loop, do_main_stop, 1, NULL, 0, false, loop);
}

SHIM_EXPORT int pw_main_loop_run(struct pw_main_loop *loop)
{
	int res = 0;

	loop->running = true;
	pw_loop_enter(loop->loop);
	while (loop->running) {
		if ((res = pw_loop_iterate(loop->loop, -1)) < 0) {
			if (res == -EINTR)
				continue;
			shim_warn("main loop: %s", spa_strerror(res));
		}
	}
	pw_loop_leave(loop->loop);
	return res > 0 ? 0 : res;
}

/* -------------------------------------------------------- pw_thread_loop */

struct pw_thread_loop {
	struct pw_loop *loop;
	struct spa_hook_list listener_list;
	pthread_t thread;
	bool created;
	bool running;
	bool start_signal;
};

static struct pw_thread_loop *thread_loop_new(struct pw_loop *loop,
		const char *name, const struct spa_dict *props)
{
	struct pw_thread_loop *this = calloc(1, sizeof(*this));
	const char *str;

	if (this == NULL)
		return NULL;
	if (props && (str = spa_dict_lookup(props, "thread-loop.start-signal")))
		this->start_signal = spa_atob(str);
	if (loop == NULL) {
		if ((loop = pw_loop_new(props)) == NULL) {
			free(this);
			return NULL;
		}
		this->created = true;
	}
	this->loop = loop;
	pw_loop_set_name(loop, name ? name : "thread-loop");
	spa_hook_list_init(&this->listener_list);
	return this;
}

SHIM_EXPORT struct pw_thread_loop *pw_thread_loop_new(const char *name, const struct spa_dict *props)
{
	return thread_loop_new(NULL, name, props);
}

SHIM_EXPORT struct pw_thread_loop *pw_thread_loop_new_full(struct pw_loop *loop,
		const char *name, const struct spa_dict *props)
{
	return thread_loop_new(loop, name, props);
}

SHIM_EXPORT void pw_thread_loop_add_listener(struct pw_thread_loop *loop,
		struct spa_hook *listener, const struct pw_thread_loop_events *events, void *data)
{
	spa_hook_list_append(&loop->listener_list, listener, events, data);
}

SHIM_EXPORT struct pw_loop *pw_thread_loop_get_loop(struct pw_thread_loop *loop)
{
	return loop->loop;
}

static void *thread_loop_run(void *user_data)
{
	struct pw_thread_loop *this = user_data;
	int res;

	pw_loop_enter(this->loop);
	if (this->start_signal)
		pw_loop_signal(this->loop, false);
	while (this->running) {
		if ((res = pw_loop_iterate(this->loop, -1)) < 0 && res != -EINTR)
			shim_warn("thread loop: %s", spa_strerror(res));
	}
	pw_loop_leave(this->loop);
	return NULL;
}

SHIM_EXPORT int pw_thread_loop_start(struct pw_thread_loop *loop)
{
	int res;
	if (loop->running)
		return 0;
	loop->running = true;
	if ((res = pthread_create(&loop->thread, NULL, thread_loop_run, loop)) != 0) {
		loop->running = false;
		return -res;
	}
	pthread_setname_np(loop->thread, loop->loop->name);
	return 0;
}

static int do_thread_stop(struct spa_loop *l, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct pw_thread_loop *this = user_data;
	this->running = false;
	return 0;
}

SHIM_EXPORT void pw_thread_loop_stop(struct pw_thread_loop *loop)
{
	if (!loop->running)
		return;
	pw_loop_lock(loop->loop);
	pw_loop_invoke(loop->loop, do_thread_stop, 1, NULL, 0, false, loop);
	pw_loop_unlock(loop->loop);
	pthread_join(loop->thread, NULL);
	loop->running = false;
}

SHIM_EXPORT void pw_thread_loop_destroy(struct pw_thread_loop *loop)
{
	if (loop == NULL)
		return;
	spa_hook_list_call(&loop->listener_list, struct pw_thread_loop_events, destroy, 0);
	pw_thread_loop_stop(loop);
	if (loop->created)
		pw_loop_destroy(loop->loop);
	spa_hook_list_clean(&loop->listener_list);
	free(loop);
}

SHIM_EXPORT void pw_thread_loop_lock(struct pw_thread_loop *loop) { pw_loop_lock(loop->loop); }
SHIM_EXPORT void pw_thread_loop_unlock(struct pw_thread_loop *loop) { pw_loop_unlock(loop->loop); }
SHIM_EXPORT void pw_thread_loop_signal(struct pw_thread_loop *loop, bool wait_for_accept)
{
	pw_loop_signal(loop->loop, wait_for_accept);
}
SHIM_EXPORT void pw_thread_loop_wait(struct pw_thread_loop *loop) { pw_loop_wait(loop->loop, NULL); }
SHIM_EXPORT int pw_thread_loop_timed_wait(struct pw_thread_loop *loop, int wait_max_sec)
{
	struct timespec abstime;
	int res = pw_loop_get_time(loop->loop, &abstime, wait_max_sec * SPA_NSEC_PER_SEC);
	if (res < 0)
		return res;
	return pw_loop_wait(loop->loop, &abstime);
}
SHIM_EXPORT int pw_thread_loop_get_time(struct pw_thread_loop *loop,
		struct timespec *abstime, int64_t timeout)
{
	return pw_loop_get_time(loop->loop, abstime, timeout);
}
SHIM_EXPORT int pw_thread_loop_timed_wait_full(struct pw_thread_loop *loop,
		const struct timespec *abstime)
{
	return pw_loop_wait(loop->loop, abstime);
}
SHIM_EXPORT void pw_thread_loop_accept(struct pw_thread_loop *loop) { pw_loop_accept(loop->loop); }
SHIM_EXPORT bool pw_thread_loop_in_thread(struct pw_thread_loop *loop)
{
	return loop->running && pthread_equal(loop->thread, pthread_self());
}
