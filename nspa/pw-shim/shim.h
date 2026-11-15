/*
 * jackbus-pw: the libpipewire-0.3 client API on JackBus (spike).
 *
 * Built against PipeWire's own public headers, so every struct an
 * application touches inline (pw_loop, pw_properties, spa_io_position,
 * pw_buffer, the pw_core method table) has PipeWire's layout. Each
 * pw_filter becomes one JACK client; its process callback runs in the
 * JACK process thread, on the server's rate and period.
 */
#ifndef JACKBUS_PW_SHIM_H
#define JACKBUS_PW_SHIM_H

#include <stdbool.h>
#include <stdatomic.h>

#include <spa/utils/defs.h>
#include <spa/utils/hook.h>
#include <spa/utils/list.h>
#include <spa/support/plugin.h>
#include <spa/support/loop.h>
#include <spa/support/system.h>

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>

#include <jack/jack.h>

#define SHIM_EXPORT __attribute__((visibility("default")))

/* logging: the shim's own messages, independent of PIPEWIRE_DEBUG */
extern int shim_verbose;
void shim_log(const char *fmt, ...) SPA_PRINTF_FUNC(1, 2);
void shim_warn(const char *fmt, ...) SPA_PRINTF_FUNC(1, 2);

/* SPA support plugin (libspa-support.so): loop and system implementations */
struct spa_handle *shim_load_spa_handle(const char *factory_name,
		const struct spa_dict *info,
		const struct spa_support *support, uint32_t n_support);
void shim_unload_spa_handle(struct spa_handle *handle);

struct pw_context {
	struct pw_loop *main_loop;
	struct pw_properties *props;
	void *user_data;
	struct spa_hook_list listeners;
};

struct pw_core {
	/* must stay first: pw_core_* methods are inline spa_interface calls */
	struct spa_interface iface;
	struct spa_hook_list listeners;
	struct pw_context *context;
	struct pw_properties *props;
	void *user_data;
	struct spa_source *sync_source;
	int sync_seq;
	int pending[16];
	uint32_t pending_id[16];
	uint32_t n_pending;
};

#endif
