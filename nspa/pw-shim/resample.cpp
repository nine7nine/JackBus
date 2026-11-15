/*
 * jackbus-pw: a C face for JackBus's bridge resampler (common/JackBridgeResampler),
 * for pw_streams at a rate other than JackBus's (fixed ratio: the application
 * runs on JackBus's clock, so nothing drifts).
 *
 * Init allocates; push and pull are realtime-safe.
 */
#include <new>
#include <stdlib.h>

#include "JackBridgeResampler.h"

extern "C" {
#include "resample.h"
}

struct shim_resampler {
	Jack::JackBridgeResampler r;
	double ratio;
};

extern "C" struct shim_resampler *shim_resampler_new(int channels, double ratio, int max_in, int max_out)
{
	void *mem = malloc(sizeof(shim_resampler));
	if (mem == NULL)
		return NULL;
	shim_resampler *s = new (mem) shim_resampler;
	if (!s->r.Init(channels, ratio, 1, max_in, max_out)) {
		s->~shim_resampler();
		free(mem);
		return NULL;
	}
	s->r.SetRatioSmoothing(0);
	s->ratio = ratio;
	return s;
}

extern "C" void shim_resampler_free(struct shim_resampler *s)
{
	if (s == NULL)
		return;
	s->~shim_resampler();
	free(s);
}

extern "C" void shim_resampler_reset(struct shim_resampler *s)
{
	s->r.Reset();
}

extern "C" int shim_resampler_push(struct shim_resampler *s, const float *const *in, int frames)
{
	return s->r.Push(in, frames);
}

extern "C" int shim_resampler_pull(struct shim_resampler *s, float *const *out, int frames)
{
	return s->r.Pull(out, frames, s->ratio);
}

extern "C" int shim_resampler_pull_available(struct shim_resampler *s, float *const *out, int max)
{
	return s->r.PullAvailable(out, max, s->ratio);
}

extern "C" int shim_resampler_needed(struct shim_resampler *s, int frames)
{
	long long need = s->r.NeededFor(frames, s->ratio) - s->r.Pushed();
	return need > 0 ? (int)need : 0;
}

extern "C" double shim_resampler_delay(struct shim_resampler *s)
{
	/* input frames pushed beyond the centre of the next output */
	return (double)s->r.Pushed() - s->r.Center();
}
