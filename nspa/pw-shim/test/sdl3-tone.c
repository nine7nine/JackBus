/*
 * jackbus-pw test: an SDL3 application. Lists SDL's playback and recording
 * devices (SDL's PipeWire backend: the registry) and plays 3 s of a 440 Hz
 * sine at 44100 Hz through the default device (SDL converts to the device's
 * rate, so the shim's stream is at JackBus's).
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

	SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, 44100 };
	SDL_AudioStream *st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
	if (st == NULL) {
		fprintf(stderr, "SDL_OpenAudioDeviceStream: %s\n", SDL_GetError());
		return 1;
	}
	const int frames = 44100 * 3;
	float *buf = malloc(sizeof(float) * 2 * frames);
	for (int i = 0; i < frames; i++)
		buf[2 * i] = buf[2 * i + 1] = 0.5f * sinf(2.0f * (float)M_PI * 440.0f * i / 44100.0f);
	SDL_PutAudioStreamData(st, buf, sizeof(float) * 2 * frames);
	SDL_FlushAudioStream(st);
	SDL_ResumeAudioStreamDevice(st);
	printf("playing\n");
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
