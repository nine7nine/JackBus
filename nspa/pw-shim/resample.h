/* jackbus-pw: resampling for pw_streams (resample.cpp) */
#ifndef JACKBUS_PW_RESAMPLE_H
#define JACKBUS_PW_RESAMPLE_H

struct shim_resampler;

/* ratio: input frames per output frame; quality "normal" of the bridge resampler */
struct shim_resampler *shim_resampler_new(int channels, double ratio, int max_in, int max_out);
void shim_resampler_free(struct shim_resampler *s);
void shim_resampler_reset(struct shim_resampler *s);
int shim_resampler_push(struct shim_resampler *s, const float *const *in, int frames);
/* exactly frames outputs, or 0 (nothing changes) when the input isn't there */
int shim_resampler_pull(struct shim_resampler *s, float *const *out, int frames);
int shim_resampler_pull_available(struct shim_resampler *s, float *const *out, int max);
/* input frames still to push before frames outputs can be pulled */
int shim_resampler_needed(struct shim_resampler *s, int frames);
/* input frames held beyond the next output's centre */
double shim_resampler_delay(struct shim_resampler *s);

#endif
