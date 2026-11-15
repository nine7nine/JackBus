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
#include <pipewire/stream.h>
#include <spa/node/io.h>

#include <jack/jack.h>
#include <jack/jackbus.h>

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

/* A pw_filter or pw_stream as the application's JACK client runs it */
struct shim_member {
	void (*process)(struct shim_member *m, const jackbus_cycle_t *cycle);	/* realtime */
	void (*shutdown)(struct shim_member *m);	/* the server went away */
	jack_nframes_t (*latency)(struct shim_member *m);	/* its process latency */
};

/* the application's JACK client (app.c): one JackBus node per process */
jackbus_node_t *shim_app_ref(const char *name);
void shim_app_unref(void);
const char *shim_app_client_name(void);		/* the name asked for */
void shim_app_attach(struct shim_member *m);	/* it runs from the next cycle */
void shim_app_detach(struct shim_member *m);	/* returns once no cycle runs it */
void shim_app_update_latency(void);
void shim_app_retire(void *old);		/* free once no cycle can read it */
/* spa_io_position (filter and stream) from the cycle record, realtime thread */
void shim_fill_position(struct spa_io_position *pos, const jackbus_cycle_t *cycle, jack_client_t *client);
/* the JACK port name for a port of node node_name: "<node>/<port>", or the
 * port alone when the node has the client's name */
void shim_app_port_name(const char *node_name, const char *port, char *buf, size_t size);

#endif
