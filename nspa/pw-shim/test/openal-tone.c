/*
 * jackbus-pw test: an OpenAL Soft application. Lists the devices (OpenAL's
 * PipeWire backend wants audio nodes in the registry before it runs) and
 * plays 3 s of a 440 Hz sine, a 44100 Hz mono buffer that OpenAL mixes at
 * the device's rate.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

int main(void)
{
	const ALCchar *list = alcGetString(NULL, ALC_ALL_DEVICES_SPECIFIER);
	for (const ALCchar *p = list; p && *p; p += strlen(p) + 1)
		printf("device: %s\n", p);
	ALCdevice *dev = alcOpenDevice(NULL);
	if (dev == NULL) {
		fprintf(stderr, "alcOpenDevice failed\n");
		return 1;
	}
	ALCcontext *ctx = alcCreateContext(dev, NULL);
	alcMakeContextCurrent(ctx);
	ALCint rate = 0;
	alcGetIntegerv(dev, ALC_FREQUENCY, 1, &rate);
	printf("opened: %s at %d Hz\n", alcGetString(dev, ALC_ALL_DEVICES_SPECIFIER), rate);

	const int frames = 44100 * 3;
	short *pcm = malloc(sizeof(short) * frames);
	for (int i = 0; i < frames; i++)
		pcm[i] = (short)(0.5 * 32767 * sin(2 * M_PI * 440.0 * i / 44100.0));
	ALuint buf, src;
	alGenBuffers(1, &buf);
	alBufferData(buf, AL_FORMAT_MONO16, pcm, sizeof(short) * frames, 44100);
	alGenSources(1, &src);
	alSourcei(src, AL_BUFFER, (ALint)buf);
	alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
	alSourcePlay(src);
	printf("playing\n");
	fflush(stdout);
	ALint state = AL_PLAYING;
	while (state == AL_PLAYING) {
		usleep(20000);
		alGetSourcei(src, AL_SOURCE_STATE, &state);
	}
	alDeleteSources(1, &src);
	alDeleteBuffers(1, &buf);
	alcMakeContextCurrent(NULL);
	alcDestroyContext(ctx);
	alcCloseDevice(dev);
	free(pcm);
	printf("done\n");
	return 0;
}
