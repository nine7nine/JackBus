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
#include <time.h>
#include <stdatomic.h>
#include <sys/mman.h>

#define SHIM_EXPORT __attribute__((visibility("default")))
#define PW_API_LOOP_IMPL SHIM_EXPORT
#include <pipewire/loop.h>
#include <pipewire/data-loop.h>
#include <pipewire/context.h>

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

/* ------------------------------------------------------------ data loop */

/*
 * The data loop of PipeWire is the realtime thread that runs process
 * callbacks; here that is the JACK realtime thread of the application's
 * node. pw_loop_invoke() on it queues the function in a slot; the JACK
 * thread runs the queued functions at the start of each cycle, before any
 * process callback, so they are serialized with process as in PipeWire. The
 * realtime side takes no lock and makes no system call: a blocking caller
 * polls its slot. Called in the JACK thread itself, the function runs at
 * once; while the node is not in the cycle, the caller runs it.
 *
 * Sources (timers, fds) added to the data loop go to the main loop: no fd
 * is polled in the JACK thread.
 */
#define INVOKE_SLOTS 64
#define INVOKE_DATA 1024

enum { SLOT_FREE, SLOT_FILLED, SLOT_DONE, SLOT_CANCELLED };

struct invoke_slot {
	atomic_int state;
	bool block;
	int res;
	spa_invoke_func_t func;
	struct spa_loop *loop;
	uint32_t seq;
	size_t size;
	void *user_data;
	uint8_t data[INVOKE_DATA] __attribute__((aligned(16)));
};

static struct {
	pthread_mutex_t lock;		/* callers */
	uint32_t w;			/* under lock */
	uint32_t r;			/* the JACK thread, or a drain while no cycle runs */
	atomic_bool running;
	struct invoke_slot *slots;	/* locked memory, made on first use */
} dl = { .lock = PTHREAD_MUTEX_INITIALIZER };

struct pw_data_loop {
	struct pw_loop *loop;
	struct spa_hook_list listener_list;
	bool emulated;			/* the JACK thread's, else a thread of its own */
	/* emulated */
	struct pw_loop this;
	struct spa_loop spa_loop;
	struct pw_loop *main;
	bool warned;
	/* a thread of its own (pw_data_loop_new) */
	pthread_t thread;
	bool running;
	char name[32];
};

bool shim_in_rt_thread(void)
{
	jack_client_t *c = shim_app_client();
	return c != NULL && atomic_load(&dl.running) &&
		pthread_equal(pthread_self(), jack_client_thread_id(c));
}

/* the queued functions, in order; the caller is the only consumer */
static void run_slots(void)
{
	for (;;) {
		struct invoke_slot *sl = &dl.slots[dl.r % INVOKE_SLOTS];
		int st = atomic_load_explicit(&sl->state, memory_order_acquire);
		if (st == SLOT_CANCELLED) {
			atomic_store_explicit(&sl->state, SLOT_FREE, memory_order_release);
			dl.r++;
			continue;
		}
		if (st != SLOT_FILLED)
			break;
		int res = sl->func(sl->loop, true, sl->seq, sl->size ? sl->data : NULL,
				sl->size, sl->user_data);
		int expected = SLOT_FILLED;
		if (sl->block) {
			sl->res = res;
			if (!atomic_compare_exchange_strong(&sl->state, &expected, SLOT_DONE))
				atomic_store_explicit(&sl->state, SLOT_FREE, memory_order_release);
		} else {
			atomic_store_explicit(&sl->state, SLOT_FREE, memory_order_release);
		}
		dl.r++;
	}
}

void shim_data_loop_run(void)
{
	if (dl.slots)
		run_slots();
}

/* called once the node is in the cycle, and once no cycle runs any more */
void shim_data_loop_set_running(bool running)
{
	pthread_mutex_lock(&dl.lock);
	atomic_store(&dl.running, running);
	if (!running && dl.slots)
		run_slots();		/* what was queued for a cycle that won't come */
	pthread_mutex_unlock(&dl.lock);
}

static void sleep_us(long us)
{
	struct timespec ts = { 0, us * 1000 };
	nanosleep(&ts, NULL);
}

static int data_invoke(void *object, spa_invoke_func_t func, uint32_t seq,
		const void *data, size_t size, bool block, void *user_data)
{
	struct pw_data_loop *d = object;
	int res;

	if (shim_in_rt_thread())
		return func(&d->spa_loop, false, seq, data, size, user_data);

	pthread_mutex_lock(&dl.lock);
	if (!atomic_load(&dl.running) || dl.slots == NULL || size > INVOKE_DATA) {
		if (size > INVOKE_DATA && atomic_load(&dl.running) && !d->warned) {
			shim_warn("data loop: an invoke of %zu bytes runs in the caller", size);
			d->warned = true;
		}
		res = func(&d->spa_loop, false, seq, data, size, user_data);
		pthread_mutex_unlock(&dl.lock);
		return res;
	}
	struct invoke_slot *sl = &dl.slots[dl.w % INVOKE_SLOTS];
	for (int i = 0; atomic_load_explicit(&sl->state, memory_order_acquire) != SLOT_FREE; i++) {
		if (i == 20000) {		/* 2 s: no cycle has run */
			pthread_mutex_unlock(&dl.lock);
			return -EPIPE;
		}
		sleep_us(100);
	}
	sl->func = func;
	sl->loop = &d->spa_loop;
	sl->seq = seq;
	sl->size = size;
	sl->user_data = user_data;
	sl->block = block;
	if (size)
		memcpy(sl->data, data, size);
	atomic_store_explicit(&sl->state, SLOT_FILLED, memory_order_release);
	dl.w++;
	pthread_mutex_unlock(&dl.lock);

	if (!block)
		return seq == SPA_ID_INVALID ? 0 : SPA_RESULT_RETURN_ASYNC(seq);
	for (int i = 0; i < 40000; i++) {	/* 2 s */
		if (atomic_load_explicit(&sl->state, memory_order_acquire) == SLOT_DONE) {
			res = sl->res;
			atomic_store_explicit(&sl->state, SLOT_FREE, memory_order_release);
			return res;
		}
		sleep_us(50);
	}
	int expected = SLOT_FILLED;
	if (atomic_compare_exchange_strong(&sl->state, &expected, SLOT_CANCELLED))
		return -ETIMEDOUT;
	res = sl->res;				/* it finished meanwhile */
	atomic_store_explicit(&sl->state, SLOT_FREE, memory_order_release);
	return res;
}

static void warn_source(struct pw_data_loop *d)
{
	if (!d->warned)
		shim_log("data loop: sources run on the main loop");
	d->warned = true;
}

static int data_add_source(void *object, struct spa_source *source)
{
	struct pw_data_loop *d = object;
	warn_source(d);
	return spa_loop_add_source(d->main->loop, source);
}

static int data_update_source(void *object, struct spa_source *source)
{
	struct pw_data_loop *d = object;
	return spa_loop_update_source(d->main->loop, source);
}

static int data_remove_source(void *object, struct spa_source *source)
{
	struct pw_data_loop *d = object;
	return spa_loop_remove_source(d->main->loop, source);
}

static const struct spa_loop_methods data_loop_methods = {
	SPA_VERSION_LOOP_METHODS,
	.add_source = data_add_source,
	.update_source = data_update_source,
	.remove_source = data_remove_source,
	.invoke = data_invoke,
};

static struct pw_data_loop *data_loop_of(struct pw_context *context)
{
	if (context->data_loop)
		return context->data_loop;
	pthread_mutex_lock(&dl.lock);
	if (dl.slots == NULL) {
		void *mem = NULL;
		if (posix_memalign(&mem, 64, INVOKE_SLOTS * sizeof(struct invoke_slot)) == 0) {
			memset(mem, 0, INVOKE_SLOTS * sizeof(struct invoke_slot));
			mlock(mem, INVOKE_SLOTS * sizeof(struct invoke_slot));
			dl.slots = mem;
		}
	}
	pthread_mutex_unlock(&dl.lock);

	struct pw_data_loop *d = calloc(1, sizeof(*d));
	if (d == NULL)
		return NULL;
	d->emulated = true;
	d->main = context->main_loop;
	d->spa_loop.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Loop, SPA_VERSION_LOOP,
			&data_loop_methods, d);
	d->this.system = d->main->system;
	d->this.loop = &d->spa_loop;
	d->this.control = d->main->control;
	d->this.utils = d->main->utils;
	snprintf(d->name, sizeof(d->name), "data-loop.0");
	d->this.name = d->name;
	d->loop = &d->this;
	spa_hook_list_init(&d->listener_list);
	context->data_loop = d;
	return d;
}

struct pw_loop *shim_data_loop_get(struct pw_context *context)
{
	struct pw_data_loop *d = data_loop_of(context);
	return d ? d->loop : context->main_loop;
}

void shim_data_loop_free(struct pw_context *context)
{
	struct pw_data_loop *d = context->data_loop;
	if (d == NULL)
		return;
	spa_hook_list_call(&d->listener_list, struct pw_data_loop_events, destroy, 0);
	spa_hook_list_clean(&d->listener_list);
	context->data_loop = NULL;
	free(d);
}

SHIM_EXPORT struct pw_data_loop *pw_context_get_data_loop(struct pw_context *context)
{
	return data_loop_of(context);
}

SHIM_EXPORT struct pw_loop *pw_context_acquire_loop(struct pw_context *context, const struct spa_dict *props)
{
	(void)props;
	return shim_data_loop_get(context);
}

SHIM_EXPORT void pw_context_release_loop(struct pw_context *context, struct pw_loop *loop)
{
	(void)context; (void)loop;
}

/* pw_data_loop_new(): an application's own loop thread, as in PipeWire */
static void *data_loop_thread(void *user_data)
{
	struct pw_data_loop *d = user_data;
	int res;

	pw_loop_enter(d->loop);
	while (d->running) {
		if ((res = pw_loop_iterate(d->loop, -1)) < 0 && res != -EINTR)
			shim_warn("data loop: %s", spa_strerror(res));
	}
	pw_loop_leave(d->loop);
	return NULL;
}

SHIM_EXPORT struct pw_data_loop *pw_data_loop_new(const struct spa_dict *props)
{
	struct pw_data_loop *d = calloc(1, sizeof(*d));
	if (d == NULL)
		return NULL;
	if ((d->loop = pw_loop_new(props)) == NULL) {
		free(d);
		return NULL;
	}
	spa_hook_list_init(&d->listener_list);
	snprintf(d->name, sizeof(d->name), "%s", d->loop->name);
	return d;
}

SHIM_EXPORT void pw_data_loop_add_listener(struct pw_data_loop *loop, struct spa_hook *listener,
		const struct pw_data_loop_events *events, void *data)
{
	spa_hook_list_append(&loop->listener_list, listener, events, data);
}

SHIM_EXPORT int pw_data_loop_wait(struct pw_data_loop *loop, int timeout)
{
	return loop->emulated ? -ENOTSUP : pw_loop_iterate(loop->loop, timeout);
}

static int do_data_stop(struct spa_loop *l, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct pw_data_loop *d = user_data;
	d->running = false;
	return 0;
}

SHIM_EXPORT void pw_data_loop_exit(struct pw_data_loop *loop)
{
	if (!loop->emulated)
		loop->running = false;
}

SHIM_EXPORT struct pw_loop *pw_data_loop_get_loop(struct pw_data_loop *loop)
{
	return loop->loop;
}

SHIM_EXPORT const char *pw_data_loop_get_name(struct pw_data_loop *loop)
{
	return loop->name;
}

SHIM_EXPORT const char *pw_data_loop_get_class(struct pw_data_loop *loop)
{
	return "data.rt";
}

SHIM_EXPORT int pw_data_loop_start(struct pw_data_loop *loop)
{
	int res;
	if (loop->emulated || loop->running)
		return 0;
	loop->running = true;
	if ((res = pthread_create(&loop->thread, NULL, data_loop_thread, loop)) != 0) {
		loop->running = false;
		return -res;
	}
	return 0;
}

SHIM_EXPORT int pw_data_loop_stop(struct pw_data_loop *loop)
{
	if (loop->emulated || !loop->running)
		return 0;
	pw_loop_invoke(loop->loop, do_data_stop, 1, NULL, 0, false, loop);
	pthread_join(loop->thread, NULL);
	return 0;
}

SHIM_EXPORT void pw_data_loop_destroy(struct pw_data_loop *loop)
{
	if (loop == NULL || loop->emulated)
		return;		/* the context's: freed with the context */
	pw_data_loop_stop(loop);
	spa_hook_list_call(&loop->listener_list, struct pw_data_loop_events, destroy, 0);
	spa_hook_list_clean(&loop->listener_list);
	pw_loop_destroy(loop->loop);
	free(loop);
}

SHIM_EXPORT bool pw_data_loop_in_thread(struct pw_data_loop *loop)
{
	if (loop->emulated)
		return shim_in_rt_thread();
	return loop->running && pthread_equal(pthread_self(), loop->thread);
}

SHIM_EXPORT struct spa_thread *pw_data_loop_get_thread(struct pw_data_loop *loop)
{
	if (loop->emulated || !loop->running)
		return NULL;
	return (struct spa_thread *)loop->thread;
}

SHIM_EXPORT int pw_data_loop_invoke(struct pw_data_loop *loop, spa_invoke_func_t func,
		uint32_t seq, const void *data, size_t size, bool block, void *user_data)
{
	return pw_loop_invoke(loop->loop, func, seq, data, size, block, user_data);
}

SHIM_EXPORT void pw_data_loop_set_thread_utils(struct pw_data_loop *loop,
		struct spa_thread_utils *thread_utils)
{
	(void)loop; (void)thread_utils;
}
