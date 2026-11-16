/*
 * jackbus-pa: what a PulseAudio server has and JackBus doesn't.
 *
 * Cards (and their profiles and ports), modules, autoload entries, the
 * sample cache, statistics, object messages and the server's extensions
 * (stream-restore, device-manager, device-restore). libpulse's whole client
 * API is exported, so every application and library that links libpulse
 * loads (Arch links with immediate binding: one missing function and the
 * library fails to load), and what doesn't apply answers as a server
 * without it would: an empty list, no such entity, not supported. Desktop
 * volume controls (KDE's, through libKF6PulseAudioQt) and event sounds
 * (libcanberra, which plays a sound as a stream when the sample cache
 * fails) go on working.
 */
#define _GNU_SOURCE
#include <string.h>

#include <pulse/ext-device-manager.h>
#include <pulse/ext-device-restore.h>
#include <pulse/ext-stream-restore.h>

#include "shim.h"

/* every info list callback: (context, info, eol, userdata) */
typedef void (*list_cb_t)(pa_context *c, const void *info, int eol, void *userdata);
typedef void (*index_cb_t)(pa_context *c, uint32_t idx, void *userdata);
typedef void (*version_cb_t)(pa_context *c, uint32_t version, void *userdata);

static void run_empty_list(pa_operation *o)
{
	void *ud;
	list_cb_t cb = (list_cb_t)shim_operation_callback(o, &ud);
	if (cb)
		cb(shim_operation_context(o), NULL, 1, ud);
}

static void run_no_entity(pa_operation *o)
{
	void *ud;
	list_cb_t cb = (list_cb_t)shim_operation_callback(o, &ud);
	pa_context *c = shim_operation_context(o);
	shim_context_set_errno(c, shim_operation_data(o)->index);
	if (cb)
		cb(c, NULL, -1, ud);
}

static void run_index_fail(pa_operation *o)
{
	void *ud;
	index_cb_t cb = (index_cb_t)shim_operation_callback(o, &ud);
	pa_context *c = shim_operation_context(o);
	shim_context_set_errno(c, shim_operation_data(o)->index);
	if (cb)
		cb(c, PA_INVALID_INDEX, ud);
}

static void run_version(pa_operation *o)
{
	void *ud;
	version_cb_t cb = (version_cb_t)shim_operation_callback(o, &ud);
	pa_context *c = shim_operation_context(o);
	uint32_t v = shim_operation_data(o)->index;
	if (v == PA_INVALID_INDEX)
		shim_context_set_errno(c, PA_ERR_NOEXTENSION);
	if (cb)
		cb(c, v, ud);
}

static pa_operation *empty_list(pa_context *c, void *cb, void *userdata)
{
	return shim_operation_new(c, NULL, run_empty_list, cb, userdata);
}

/* error: the context's errno when the callback runs */
static pa_operation *fail_op(pa_context *c, void (*run)(pa_operation *o), void *cb, void *userdata, int error)
{
	pa_operation *o = shim_operation_new(c, NULL, run, cb, userdata);
	if (o)
		shim_operation_data(o)->index = (uint32_t)error;
	return o;
}

static pa_operation *no_entity(pa_context *c, void *cb, void *userdata)
{
	return fail_op(c, run_no_entity, cb, userdata, PA_ERR_NOENTITY);
}

static pa_operation *unsupported(pa_context *c, pa_context_success_cb_t cb, void *userdata)
{
	return shim_context_success_op(c, cb, userdata, 0, PA_ERR_NOTSUPPORTED);
}

static pa_operation *version_op(pa_context *c, void *cb, void *userdata, uint32_t version)
{
	pa_operation *o = shim_operation_new(c, NULL, run_version, cb, userdata);
	if (o)
		shim_operation_data(o)->index = version;
	return o;
}

/* ------------------------------------------------------------------ cards */

SHIM_EXPORT pa_operation *pa_context_get_card_info_list(pa_context *c, pa_card_info_cb_t cb, void *userdata)
{
	return empty_list(c, (void *)cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_get_card_info_by_index(pa_context *c, uint32_t idx,
		pa_card_info_cb_t cb, void *userdata)
{
	return no_entity(c, (void *)cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_get_card_info_by_name(pa_context *c, const char *name,
		pa_card_info_cb_t cb, void *userdata)
{
	return no_entity(c, (void *)cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_set_card_profile_by_index(pa_context *c, uint32_t idx, const char *profile,
		pa_context_success_cb_t cb, void *userdata)
{
	return unsupported(c, cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_set_card_profile_by_name(pa_context *c, const char *name,
		const char *profile, pa_context_success_cb_t cb, void *userdata)
{
	return unsupported(c, cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_set_port_latency_offset(pa_context *c, const char *card_name,
		const char *port_name, int64_t offset, pa_context_success_cb_t cb, void *userdata)
{
	return unsupported(c, cb, userdata);
}

/* a device's ports: JackBus's devices have none to choose */
SHIM_EXPORT pa_operation *pa_context_set_sink_port_by_index(pa_context *c, uint32_t idx, const char *port,
		pa_context_success_cb_t cb, void *userdata)
{
	return unsupported(c, cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_set_sink_port_by_name(pa_context *c, const char *name, const char *port,
		pa_context_success_cb_t cb, void *userdata)
{
	return unsupported(c, cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_set_source_port_by_index(pa_context *c, uint32_t idx, const char *port,
		pa_context_success_cb_t cb, void *userdata)
{
	return unsupported(c, cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_set_source_port_by_name(pa_context *c, const char *name, const char *port,
		pa_context_success_cb_t cb, void *userdata)
{
	return unsupported(c, cb, userdata);
}

/* suspending a device: JackBus's run with the server */
SHIM_EXPORT pa_operation *pa_context_suspend_sink_by_name(pa_context *c, const char *sink_name, int suspend,
		pa_context_success_cb_t cb, void *userdata)
{
	return unsupported(c, cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_suspend_sink_by_index(pa_context *c, uint32_t idx, int suspend,
		pa_context_success_cb_t cb, void *userdata)
{
	return unsupported(c, cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_suspend_source_by_name(pa_context *c, const char *source_name,
		int suspend, pa_context_success_cb_t cb, void *userdata)
{
	return unsupported(c, cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_suspend_source_by_index(pa_context *c, uint32_t idx, int suspend,
		pa_context_success_cb_t cb, void *userdata)
{
	return unsupported(c, cb, userdata);
}

/* ---------------------------------------------------------------- modules */

SHIM_EXPORT pa_operation *pa_context_get_module_info_list(pa_context *c, pa_module_info_cb_t cb, void *userdata)
{
	return empty_list(c, (void *)cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_get_module_info(pa_context *c, uint32_t idx, pa_module_info_cb_t cb,
		void *userdata)
{
	return no_entity(c, (void *)cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_load_module(pa_context *c, const char *name, const char *argument,
		pa_context_index_cb_t cb, void *userdata)
{
	return fail_op(c, run_index_fail, (void *)cb, userdata, PA_ERR_NOTSUPPORTED);
}

SHIM_EXPORT pa_operation *pa_context_unload_module(pa_context *c, uint32_t idx, pa_context_success_cb_t cb,
		void *userdata)
{
	return shim_context_success_op(c, cb, userdata, 0, PA_ERR_NOENTITY);
}

/* autoload (obsolete in PulseAudio itself) */
SHIM_EXPORT pa_operation *pa_context_get_autoload_info_list(pa_context *c, pa_autoload_info_cb_t cb,
		void *userdata)
{
	return empty_list(c, (void *)cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_get_autoload_info_by_name(pa_context *c, const char *name,
		pa_autoload_type_t type, pa_autoload_info_cb_t cb, void *userdata)
{
	return no_entity(c, (void *)cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_get_autoload_info_by_index(pa_context *c, uint32_t idx,
		pa_autoload_info_cb_t cb, void *userdata)
{
	return no_entity(c, (void *)cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_add_autoload(pa_context *c, const char *name, pa_autoload_type_t type,
		const char *module, const char *argument, pa_context_index_cb_t cb, void *userdata)
{
	return fail_op(c, run_index_fail, (void *)cb, userdata, PA_ERR_NOTSUPPORTED);
}

SHIM_EXPORT pa_operation *pa_context_remove_autoload_by_name(pa_context *c, const char *name,
		pa_autoload_type_t type, pa_context_success_cb_t cb, void *userdata)
{
	return unsupported(c, cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_remove_autoload_by_index(pa_context *c, uint32_t idx,
		pa_context_success_cb_t cb, void *userdata)
{
	return unsupported(c, cb, userdata);
}

/* ----------------------------------------------------------- sample cache */

/* nothing is ever in the cache: playing a sample fails with no such entity
 * (libcanberra then plays the sound as a stream), uploading isn't
 * supported */
SHIM_EXPORT pa_operation *pa_context_get_sample_info_list(pa_context *c, pa_sample_info_cb_t cb, void *userdata)
{
	return empty_list(c, (void *)cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_get_sample_info_by_name(pa_context *c, const char *name,
		pa_sample_info_cb_t cb, void *userdata)
{
	return no_entity(c, (void *)cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_get_sample_info_by_index(pa_context *c, uint32_t idx,
		pa_sample_info_cb_t cb, void *userdata)
{
	return no_entity(c, (void *)cb, userdata);
}

SHIM_EXPORT pa_operation *pa_context_play_sample(pa_context *c, const char *name, const char *dev,
		pa_volume_t volume, pa_context_success_cb_t cb, void *userdata)
{
	return shim_context_success_op(c, cb, userdata, 0, PA_ERR_NOENTITY);
}

SHIM_EXPORT pa_operation *pa_context_play_sample_with_proplist(pa_context *c, const char *name, const char *dev,
		pa_volume_t volume, const pa_proplist *proplist, pa_context_index_cb_t cb, void *userdata)
{
	return fail_op(c, run_index_fail, (void *)cb, userdata, PA_ERR_NOENTITY);
}

SHIM_EXPORT pa_operation *pa_context_remove_sample(pa_context *c, const char *name, pa_context_success_cb_t cb,
		void *userdata)
{
	return shim_context_success_op(c, cb, userdata, 0, PA_ERR_NOENTITY);
}

SHIM_EXPORT int pa_stream_connect_upload(pa_stream *s, size_t length)
{
	shim_context_set_errno(pa_stream_get_context(s), PA_ERR_NOTSUPPORTED);
	return -PA_ERR_NOTSUPPORTED;
}

SHIM_EXPORT int pa_stream_finish_upload(pa_stream *s)
{
	shim_context_set_errno(pa_stream_get_context(s), PA_ERR_NOTSUPPORTED);
	return -PA_ERR_NOTSUPPORTED;
}

/* ------------------------------------------------------- statistics, misc */

static void run_stat(pa_operation *o)
{
	void *ud;
	pa_stat_info_cb_t cb = (pa_stat_info_cb_t)shim_operation_callback(o, &ud);
	pa_stat_info i;
	memset(&i, 0, sizeof(i));
	if (cb)
		cb(shim_operation_context(o), &i, ud);
}

SHIM_EXPORT pa_operation *pa_context_stat(pa_context *c, pa_stat_info_cb_t cb, void *userdata)
{
	return shim_operation_new(c, NULL, run_stat, (void *)cb, userdata);
}

static void run_message(pa_operation *o)
{
	void *ud;
	pa_context_string_cb_t cb = (pa_context_string_cb_t)shim_operation_callback(o, &ud);
	pa_context *c = shim_operation_context(o);
	shim_context_set_errno(c, PA_ERR_NOENTITY);
	if (cb)
		cb(c, 0, NULL, ud);
}

SHIM_EXPORT pa_operation *pa_context_send_message_to_object(pa_context *c, const char *recipient_name,
		const char *message, const char *message_parameters, pa_context_string_cb_t cb, void *userdata)
{
	return shim_operation_new(c, NULL, run_message, (void *)cb, userdata);
}

SHIM_EXPORT int pa_direction_valid(pa_direction_t direction)
{
	return direction == PA_DIRECTION_INPUT || direction == PA_DIRECTION_OUTPUT ||
		direction == (PA_DIRECTION_INPUT | PA_DIRECTION_OUTPUT);
}

SHIM_EXPORT const char *pa_direction_to_string(pa_direction_t direction)
{
	if (direction == PA_DIRECTION_INPUT)
		return "input";
	if (direction == PA_DIRECTION_OUTPUT)
		return "output";
	if (direction == (PA_DIRECTION_INPUT | PA_DIRECTION_OUTPUT))
		return "bidirectional";
	return "invalid";
}

/* ------------------------------------------------------------- extensions */

/* stream-restore: there (a volume control asks for it), nothing kept: a
 * stream's volume is its own, for as long as it runs */
SHIM_EXPORT pa_operation *pa_ext_stream_restore_test(pa_context *c, pa_ext_stream_restore_test_cb_t cb,
		void *userdata)
{
	return version_op(c, (void *)cb, userdata, 1);
}

SHIM_EXPORT pa_operation *pa_ext_stream_restore_read(pa_context *c, pa_ext_stream_restore_read_cb_t cb,
		void *userdata)
{
	return empty_list(c, (void *)cb, userdata);
}

SHIM_EXPORT pa_operation *pa_ext_stream_restore_write(pa_context *c, pa_update_mode_t mode,
		const pa_ext_stream_restore_info data[], unsigned n, int apply_immediately,
		pa_context_success_cb_t cb, void *userdata)
{
	return shim_context_success_op(c, cb, userdata, 1, 0);
}

SHIM_EXPORT pa_operation *pa_ext_stream_restore_delete(pa_context *c, const char *const s[],
		pa_context_success_cb_t cb, void *userdata)
{
	return shim_context_success_op(c, cb, userdata, 1, 0);
}

SHIM_EXPORT pa_operation *pa_ext_stream_restore_subscribe(pa_context *c, int enable, pa_context_success_cb_t cb,
		void *userdata)
{
	return shim_context_success_op(c, cb, userdata, 1, 0);
}

SHIM_EXPORT void pa_ext_stream_restore_set_subscribe_cb(pa_context *c, pa_ext_stream_restore_subscribe_cb_t cb,
		void *userdata)
{
	/* nothing kept, nothing changes */
}

/* device-manager and device-restore: not there (as on a PulseAudio server
 * without those modules) */
SHIM_EXPORT pa_operation *pa_ext_device_manager_test(pa_context *c, pa_ext_device_manager_test_cb_t cb,
		void *userdata)
{
	return version_op(c, (void *)cb, userdata, PA_INVALID_INDEX);
}

SHIM_EXPORT pa_operation *pa_ext_device_manager_read(pa_context *c, pa_ext_device_manager_read_cb_t cb,
		void *userdata)
{
	return fail_op(c, run_no_entity, (void *)cb, userdata, PA_ERR_NOEXTENSION);
}

SHIM_EXPORT pa_operation *pa_ext_device_manager_set_device_description(pa_context *c, const char *device,
		const char *description, pa_context_success_cb_t cb, void *userdata)
{
	return shim_context_success_op(c, cb, userdata, 0, PA_ERR_NOEXTENSION);
}

SHIM_EXPORT pa_operation *pa_ext_device_manager_delete(pa_context *c, const char *const s[],
		pa_context_success_cb_t cb, void *userdata)
{
	return shim_context_success_op(c, cb, userdata, 0, PA_ERR_NOEXTENSION);
}

SHIM_EXPORT pa_operation *pa_ext_device_manager_enable_role_device_priority_routing(pa_context *c, int enable,
		pa_context_success_cb_t cb, void *userdata)
{
	return shim_context_success_op(c, cb, userdata, 0, PA_ERR_NOEXTENSION);
}

SHIM_EXPORT pa_operation *pa_ext_device_manager_reorder_devices_for_role(pa_context *c, const char *role,
		const char **devices, pa_context_success_cb_t cb, void *userdata)
{
	return shim_context_success_op(c, cb, userdata, 0, PA_ERR_NOEXTENSION);
}

SHIM_EXPORT pa_operation *pa_ext_device_manager_subscribe(pa_context *c, int enable, pa_context_success_cb_t cb,
		void *userdata)
{
	return shim_context_success_op(c, cb, userdata, 0, PA_ERR_NOEXTENSION);
}

SHIM_EXPORT void pa_ext_device_manager_set_subscribe_cb(pa_context *c, pa_ext_device_manager_subscribe_cb_t cb,
		void *userdata)
{
}

SHIM_EXPORT pa_operation *pa_ext_device_restore_test(pa_context *c, pa_ext_device_restore_test_cb_t cb,
		void *userdata)
{
	return version_op(c, (void *)cb, userdata, PA_INVALID_INDEX);
}

SHIM_EXPORT pa_operation *pa_ext_device_restore_subscribe(pa_context *c, int enable, pa_context_success_cb_t cb,
		void *userdata)
{
	return shim_context_success_op(c, cb, userdata, 0, PA_ERR_NOEXTENSION);
}

SHIM_EXPORT void pa_ext_device_restore_set_subscribe_cb(pa_context *c, pa_ext_device_restore_subscribe_cb_t cb,
		void *userdata)
{
}

SHIM_EXPORT pa_operation *pa_ext_device_restore_read_formats_all(pa_context *c,
		pa_ext_device_restore_read_device_formats_cb_t cb, void *userdata)
{
	return fail_op(c, run_no_entity, (void *)cb, userdata, PA_ERR_NOEXTENSION);
}

SHIM_EXPORT pa_operation *pa_ext_device_restore_read_formats(pa_context *c, pa_device_type_t type, uint32_t idx,
		pa_ext_device_restore_read_device_formats_cb_t cb, void *userdata)
{
	return fail_op(c, run_no_entity, (void *)cb, userdata, PA_ERR_NOEXTENSION);
}

SHIM_EXPORT pa_operation *pa_ext_device_restore_save_formats(pa_context *c, pa_device_type_t type, uint32_t idx,
		uint8_t n_formats, pa_format_info **formats, pa_context_success_cb_t cb, void *userdata)
{
	return shim_context_success_op(c, cb, userdata, 0, PA_ERR_NOEXTENSION);
}
