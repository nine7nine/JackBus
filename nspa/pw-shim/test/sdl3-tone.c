/*
 * jackbus-pw test: an SDL3 application. Lists SDL's playback and recording
 * devices (SDL's PipeWire backend: the registry) and plays 3 s of a 440 Hz
 * sine through the default device, opened at the device's own rate (as a game
 * that leaves the rate to the device does): the direct route, no resampling.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL3/SDL.h>

int main(void)
{
	if (!SDL_Init(SDL_INIT_AUDIO)) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	printf("driver: %s\n", SDL_GetCurrentAudioDriver());
	int n = 0;
	SDL_AudioDeviceID *devs = SDL_GetAudioPlaybackDevices(&n);
	for (int i = 0; i < n; i++) {
		SDL_AudioSpec sp;
		int frames = 0;
		SDL_GetAudioDeviceFormat(devs[i], &sp, &frames);
		printf("playback device: %s (%d Hz, %d ch)\n", SDL_GetAudioDeviceName(devs[i]), sp.freq, sp.channels);
	}
	SDL_free(devs);
	devs = SDL_GetAudioRecordingDevices(&n);
	for (int i = 0; i < n; i++)
		printf("recording device: %s\n", SDL_GetAudioDeviceName(devs[i]));
	SDL_free(devs);

	SDL_AudioSpec dev;
	int dev_frames = 0;
	if (!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &dev, &dev_frames))
		dev.freq = 48000;
	const int rate = dev.freq;
	SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, rate };
	SDL_AudioStream *st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
	if (st == NULL) {
		fprintf(stderr, "SDL_OpenAudioDeviceStream: %s\n", SDL_GetError());
		return 1;
	}
	const int frames = rate * 3;
	float *buf = malloc(sizeof(float) * 2 * frames);
	for (int i = 0; i < frames; i++)
		buf[2 * i] = buf[2 * i + 1] = 0.5f * sinf(2.0f * (float)M_PI * 440.0f * i / (float)rate);
	SDL_PutAudioStreamData(st, buf, sizeof(float) * 2 * frames);
	SDL_FlushAudioStream(st);
	SDL_ResumeAudioStreamDevice(st);
	printf("playing at %d Hz\n", rate);
	fflush(stdout);
	while (SDL_GetAudioStreamQueued(st) > 0)
		SDL_Delay(20);
	SDL_Delay(200);
	SDL_DestroyAudioStream(st);
	free(buf);
	SDL_Quit();
	printf("done\n");
	return 0;
}
