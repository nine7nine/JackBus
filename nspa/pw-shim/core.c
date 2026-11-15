/*
 * jackbus-pw: pw_context and pw_core.
 *
 * There is no PipeWire daemon behind these objects. pw_context_connect()
 * returns a local pw_core whose method table answers sync with done (so
 * round trips complete) and hands out an empty registry. The JACK
 * connection belongs to each filter: one JACK client per pw_filter, as
 * PipeWire has one node per filter.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define SHIM_EXPORT __attribute__((visibility("default")))
#define PW_API_CORE_IMPL SHIM_EXPORT
#define PW_API_REGISTRY_IMPL SHIM_EXPORT
#include <pipewire/core.h>

#include <spa/utils/result.h>

#include "shim.h"

/* -------------------------------------------------------------- context */

SHIM_EXPORT struct pw_context *pw_context_new(struct pw_loop *main_loop,
		struct pw_properties *props, size_t user_data_size)
{
	struct pw_context *this = calloc(1, sizeof(*this) + user_data_size);
	if (this == NULL) {
		pw_properties_free(props);
		return NULL;
	}
	this->main_loop = main_loop;
	this->props = props ? props : pw_properties_new(NULL, NULL);
	if (user_data_size > 0)
		this->user_data = SPA_PTROFF(this, sizeof(*this), void);
	spa_hook_list_init(&this->listeners);
	return this;
}

SHIM_EXPORT void pw_context_destroy(struct pw_context *context)
{
	if (context == NULL)
		return;
	spa_hook_list_call(&context->listeners, struct pw_context_events, destroy, 0);
	spa_hook_list_call(&context->listeners, struct pw_context_events, free, 0);
	spa_hook_list_clean(&context->listeners);
	pw_properties_free(context->props);
	free(context);
}

SHIM_EXPORT void *pw_context_get_user_data(struct pw_context *context) { return context->user_data; }

SHIM_EXPORT void pw_context_add_listener(struct pw_context *context, struct spa_hook *listener,
		const struct pw_context_events *events, void *data)
{
	spa_hook_list_append(&context->listeners, listener, events, data);
}

SHIM_EXPORT const struct pw_properties *pw_context_get_properties(struct pw_context *context)
{
	return context->props;
}

SHIM_EXPORT int pw_context_update_properties(struct pw_context *context, const struct spa_dict *dict)
{
	return pw_properties_update(context->props, dict);
}

SHIM_EXPORT struct pw_loop *pw_context_get_main_loop(struct pw_context *context)
{
	return context->main_loop;
}

SHIM_EXPORT const char *pw_context_get_conf_section(struct pw_context *context, const char *section)
{
	(void)context; (void)section;
	return NULL;
}

SHIM_EXPORT int pw_context_conf_update_props(struct pw_context *context,
		const char *section, struct pw_properties *props)
{
	(void)context; (void)section; (void)props;
	return 0;
}

SHIM_EXPORT int pw_context_conf_section_match_rules(struct pw_context *context,
		const char *section, const struct spa_dict *props,
		int (*callback) (void *data, const char *location, const char *action,
			const char *str, size_t len), void *data)
{
	(void)context; (void)section; (void)props; (void)callback; (void)data;
	return 0;
}

SHIM_EXPORT int pw_context_conf_section_for_each(struct pw_context *context,
		const char *section,
		int (*callback) (void *data, const char *location, const char *section,
			const char *str, size_t len), void *data)
{
	(void)context; (void)section; (void)callback; (void)data;
	return 0;
}

/* ----------------------------------------------------------------- core */

static void core_sync_dispatch(void *data, uint64_t count)
{
	struct pw_core *core = data;
	uint32_t n = core->n_pending;
	int seqs[16];
	uint32_t ids[16];

	memcpy(seqs, core->pending, n * sizeof(int));
	memcpy(ids, core->pending_id, n * sizeof(uint32_t));
	core->n_pending = 0;
	for (uint32_t i = 0; i < n; i++)
		spa_hook_list_call(&core->listeners, struct pw_core_events, done, 0,
				ids[i], seqs[i]);
}

static int core_add_listener(void *object, struct spa_hook *listener,
		const struct pw_core_events *events, void *data)
{
	struct pw_core *core = object;
	spa_hook_list_append(&core->listeners, listener, events, data);
	return 0;
}

static int core_hello(void *object, uint32_t version) { return 0; }

static int core_sync(void *object, uint32_t id, int seq)
{
	struct pw_core *core = object;
	int res = SPA_RESULT_RETURN_ASYNC(++core->sync_seq & SPA_ASYNC_SEQ_MASK);

	if (core->n_pending == SPA_N_ELEMENTS(core->pending))
		return -ENOSPC;
	core->pending[core->n_pending] = res;
	core->pending_id[core->n_pending] = id;
	core->n_pending++;
	pw_loop_signal_event(core->context->main_loop, core->sync_source);
	return res;
}

static int core_pong(void *object, uint32_t id, int seq) { return 0; }

static int core_error(void *object, uint32_t id, int seq, int res, const char *message)
{
	shim_log("core error from the application: id %u: %s", id, message);
	return 0;
}

/* an empty registry: no PipeWire objects exist behind the shim */
struct shim_registry {
	struct spa_interface iface;
	struct spa_hook_list listeners;
};

static int registry_add_listener(void *object, struct spa_hook *listener,
		const struct pw_registry_events *events, void *data)
{
	struct shim_registry *r = object;
	spa_hook_list_append(&r->listeners, listener, events, data);
	return 0;
}

static void *registry_bind(void *object, uint32_t id, const char *type,
		uint32_t version, size_t user_data_size)
{
	errno = ENOENT;
	return NULL;
}

static int registry_destroy(void *object, uint32_t id) { return -ENOENT; }

static const struct pw_registry_methods registry_methods = {
	PW_VERSION_REGISTRY_METHODS,
	.add_listener = registry_add_listener,
	.bind = registry_bind,
	.destroy = registry_destroy,
};

static struct pw_registry *core_get_registry(void *object, uint32_t version,
		size_t user_data_size)
{
	struct shim_registry *r = calloc(1, sizeof(*r) + user_data_size);
	if (r == NULL)
		return NULL;
	r->iface = SPA_INTERFACE_INIT(PW_TYPE_INTERFACE_Registry,
			PW_VERSION_REGISTRY, &registry_methods, r);
	spa_hook_list_init(&r->listeners);
	shim_log("registry requested: it lists no objects");
	return (struct pw_registry *)r;
}

static void *core_create_object(void *object, const char *factory_name,
		const char *type, uint32_t version, const struct spa_dict *props,
		size_t user_data_size)
{
	shim_log("create_object(%s, %s) is not supported", factory_name, type);
	errno = ENOTSUP;
	return NULL;
}

static int core_destroy_proxy(void *object, void *proxy) { return -ENOTSUP; }

static const struct pw_core_methods core_methods = {
	PW_VERSION_CORE_METHODS,
	.add_listener = core_add_listener,
	.hello = core_hello,
	.sync = core_sync,
	.pong = core_pong,
	.error = core_error,
	.get_registry = core_get_registry,
	.create_object = core_create_object,
	.destroy = core_destroy_proxy,
};

SHIM_EXPORT struct pw_core *pw_context_connect(struct pw_context *context,
		struct pw_properties *properties, size_t user_data_size)
{
	struct pw_core *core = calloc(1, sizeof(*core) + user_data_size);

	if (core == NULL) {
		pw_properties_free(properties);
		return NULL;
	}
	core->iface = SPA_INTERFACE_INIT(PW_TYPE_INTERFACE_Core, PW_VERSION_CORE,
			&core_methods, core);
	spa_hook_list_init(&core->listeners);
	core->context = context;
	core->props = properties ? properties : pw_properties_new(NULL, NULL);
	if (user_data_size > 0)
		core->user_data = SPA_PTROFF(core, sizeof(*core), void);
	core->sync_source = pw_loop_add_event(context->main_loop, core_sync_dispatch, core);
	if (core->sync_source == NULL) {
		pw_properties_free(core->props);
		free(core);
		return NULL;
	}
	shim_log("pw_context_connect: local core, no PipeWire daemon involved");
	return core;
}

SHIM_EXPORT struct pw_core *pw_context_connect_fd(struct pw_context *context, int fd,
		struct pw_properties *properties, size_t user_data_size)
{
	(void)fd;
	return pw_context_connect(context, properties, user_data_size);
}

SHIM_EXPORT struct pw_core *pw_context_connect_self(struct pw_context *context,
		struct pw_properties *properties, size_t user_data_size)
{
	return pw_context_connect(context, properties, user_data_size);
}

SHIM_EXPORT int pw_core_disconnect(struct pw_core *core)
{
	if (core == NULL)
		return 0;
	spa_hook_list_clean(&core->listeners);
	pw_loop_destroy_source(core->context->main_loop, core->sync_source);
	pw_properties_free(core->props);
	free(core);
	return 0;
}

SHIM_EXPORT struct pw_context *pw_core_get_context(struct pw_core *core) { return core->context; }
SHIM_EXPORT void *pw_core_get_user_data(struct pw_core *core) { return core->user_data; }
SHIM_EXPORT const struct pw_properties *pw_core_get_properties(struct pw_core *core) { return core->props; }
SHIM_EXPORT int pw_core_update_properties(struct pw_core *core, const struct spa_dict *dict)
{
	return pw_properties_update(core->props, dict);
}
SHIM_EXPORT int pw_core_set_paused(struct pw_core *core, bool paused) { return 0; }
