/*
 * jackbus-pa: the libpulse client API on JackBus.
 *
 * Built against PulseAudio's own headers, loaded in place of libpulse.so.0
 * by the jackbus-pa launcher, the way jackbus-pw takes libpipewire's place.
 * Only the client side is here (main loops, context, streams, operations,
 * introspection): the rest of libpulse's API (sample specs, channel maps,
 * volumes, property lists, formats, ...) is PulseAudio's own
 * libpulsecommon, which this library links and the dynamic linker finds
 * those symbols in.
 */
#ifndef JACKBUS_PA_SHIM_H
#define JACKBUS_PA_SHIM_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <pulse/pulseaudio.h>

#include <jack/jack.h>
#include <jack/jackbus.h>

#define SHIM_EXPORT __attribute__((visibility("default")))
#define SHIM_MIN(a, b) ((a) < (b) ? (a) : (b))
#define SHIM_MAX(a, b) ((a) > (b) ? (a) : (b))

/* logging (JACKBUS_PA_VERBOSE=1) */
extern int shim_verbose;
void shim_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void shim_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

uint64_t shim_now_usec(void);			/* CLOCK_MONOTONIC */
/* a struct timeval for main loop API a from a monotonic time (a's clock: monotonic for
 * this library's loops, the wall clock for any other, as libpulse's) */
struct timeval *shim_rttime(pa_mainloop_api *a, struct timeval *tv, uint64_t usec);

/* node.c: the application's JackBus node, one per process (a JACK client
 * named after it), in the cycle while a stream is connected */
struct shim_member {
	void (*process)(struct shim_member *m, const jackbus_cycle_t *cycle);	/* realtime */
	struct shim_member *next;
};
/* the node: the live one (a dead one is never handed out again); each holder
 * releases the node it got */
jackbus_node_t *shim_node_ref(const char *name);
void shim_node_unref(jackbus_node_t *n);
jack_client_t *shim_node_client(void);		/* the live node's */
void shim_node_attach(jackbus_node_t *n, struct shim_member *m);	/* runs from the next cycle */
void shim_node_detach(jackbus_node_t *n, struct shim_member *m);	/* returns once no cycle runs it */
/* the node's events (graph changes, shutdown) wake these eventfds */
void shim_node_watch(int efd, bool add);
bool shim_node_dead(jackbus_node_t *n);		/* the server it was on went away */
/* the settings' pa.async (read when the node opened): streams are async endpoints, the
 * node has no realtime thread unless one is needed (shim_node_need_rt) */
bool shim_node_async(jackbus_node_t *n);
void shim_node_need_rt(jackbus_node_t *n);
void shim_node_hold(jackbus_node_t *n, bool on);	/* an async stream: the node active while any */

/* context.c */
struct shim_device {
	uint32_t index;
	bool playback;
	char name[256];			/* jackbus.<client>.playback|capture */
	char desc[300];
	char client[128];
	uint32_t channels;
	char ctl[64];			/* its ALSA card's control device ("hw:PCH"), "" none */
};
pa_operation *shim_operation_new(pa_context *c, pa_stream *s, void (*run)(pa_operation *o),
		void *cb, void *userdata);
void shim_operation_finish(pa_operation *o, int success);	/* a pending one */
struct shim_op_data {
	int success;
	uint32_t index;
	char *name;
	void *extra;
};
struct shim_op_data *shim_operation_data(pa_operation *o);
void shim_operation_cancel_all(pa_context *c, pa_stream *s);
void *shim_operation_callback(pa_operation *o, void **userdata);
pa_stream *shim_operation_stream(pa_operation *o);
pa_context *shim_operation_context(pa_operation *o);
/* an operation that calls a pa_context_success_cb_t (error: the context's
 * errno when it fails) */
pa_operation *shim_context_success_op(pa_context *c, pa_context_success_cb_t cb, void *userdata,
		int success, int error);
pa_mainloop_api *shim_context_api(pa_context *c);
const char *shim_context_app_name(pa_context *c);
int shim_context_fail(pa_context *c, int error);
void shim_context_set_errno(pa_context *c, int error);
uint32_t shim_context_new_stream_index(pa_context *c);
const struct shim_device *shim_context_device(pa_context *c, bool playback, const char *name, uint32_t index);
const struct shim_device *shim_context_default_device(pa_context *c, bool playback);
void shim_context_stream_add(pa_context *c, pa_stream *s);
void shim_context_stream_remove(pa_context *c, pa_stream *s);
pa_stream *shim_context_find_stream(pa_context *c, bool playback, uint32_t index);
struct shim_mixer **shim_context_mixers(pa_context *c);
void shim_context_mixer_changed(pa_context *c, const char *ctl);	/* subscribers hear of it */

/* mixer.c: a device's volume is its card's own mixer */
enum { SHIM_MIXER_VOLUME = 1, SHIM_MIXER_MUTE = 2 };
void shim_mixer_ctl_of(const char *alias, char *ctl, size_t len);	/* "alsa_pcm:hw:PCH:in1": "hw:PCH" */
/* the device's volume and mute: SHIM_MIXER_* it has (0: none, a fixed volume) */
int shim_mixer_volume(pa_context *c, const struct shim_device *d, uint8_t channels, pa_cvolume *v, int *mute);
int shim_mixer_set_volume(pa_context *c, const struct shim_device *d, const pa_cvolume *v);	/* 0 or -PA_ERR_* */
int shim_mixer_set_mute(pa_context *c, const struct shim_device *d, int mute);
void shim_mixer_rescan(pa_context *c, const struct shim_device *devices, int n);
void shim_mixer_watch(pa_context *c, const struct shim_device *devices, int n);
void shim_mixer_close_all(pa_context *c);

/* stream.c */
void shim_stream_fail(pa_stream *s, int error);
int shim_stream_move(pa_stream *s, const struct shim_device *d);	/* 0 or -PA_ERR_* */
void shim_stream_set_gain(pa_stream *s, const pa_cvolume *v, int mute);
void shim_stream_get_volume(pa_stream *s, pa_cvolume *v, int *mute);
bool shim_stream_playback(pa_stream *s);
uint32_t shim_stream_index(pa_stream *s);
const char *shim_stream_name(pa_stream *s);
const pa_sample_spec *shim_stream_spec(pa_stream *s);
const pa_channel_map *shim_stream_map(pa_stream *s);
uint32_t shim_stream_device(pa_stream *s);
pa_usec_t shim_stream_latency_usec(pa_stream *s);

/* resample (../pw-shim/resample.cpp: JackBus's bridge resampler) */
struct shim_resampler;
struct shim_resampler *shim_resampler_new(int channels, double ratio, int max_in, int max_out);
void shim_resampler_free(struct shim_resampler *s);
void shim_resampler_reset(struct shim_resampler *s);
int shim_resampler_push(struct shim_resampler *s, const float *const *in, int frames);
int shim_resampler_pull_available(struct shim_resampler *s, float *const *out, int max);
double shim_resampler_delay(struct shim_resampler *s);

#endif
