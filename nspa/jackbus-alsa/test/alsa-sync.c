/*
 * jackbus-alsa test: sample-accurate timing. An ALSA application writes to
 * the jackbus PCM for a few seconds; every 50 ms it compares the frames
 * played by its own account (written - snd_pcm_delay) with JACK's frame clock
 * (jack_frame_time, from a plain JACK client). The difference must stay the
 * same, within one JACK cycle (the instant within a cycle the two are read),
 * however long it runs: no drift, no resampling, positions in JackBus's
 * frames.
 *
 *   alsa-sync <seconds>    (PCM "jackbus", S16, stereo, JackBus's rate)
 *   alsa-sync xrun         its writing thread late: 100 ms buffered, a 300 ms
 *                          sleep after 1 s; the write after it must get EPIPE
 *                          (an underrun), and after snd_pcm_recover() it goes on
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <alsa/asoundlib.h>
#include <jack/jack.h>

int main(int argc, char *argv[])
{
	int xrun_mode = argc > 1 && strcmp(argv[1], "xrun") == 0;
	int seconds = xrun_mode ? 3 : argc > 1 ? atoi(argv[1]) : 4;
	int epipes = 0, slept = 0;
	jack_client_t *jc = jack_client_open("alsa-sync-clock", JackNoStartServer, NULL);
	if (jc == NULL) {
		fprintf(stderr, "no JACK server\n");
		return 1;
	}
	unsigned int rate = jack_get_sample_rate(jc), quantum = jack_get_buffer_size(jc);
	snd_pcm_t *pcm;
	int err;
	if ((err = snd_pcm_open(&pcm, "jackbus", SND_PCM_STREAM_PLAYBACK, 0)) < 0 ||
	    (err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
			2, rate, 0, xrun_mode ? 100000 : 100000)) < 0) {
		fprintf(stderr, "jackbus PCM: %s\n", snd_strerror(err));
		return 1;
	}
	snd_pcm_uframes_t buffer, period;
	snd_pcm_get_params(pcm, &buffer, &period);
	int16_t *silence = calloc(period * 2, sizeof(int16_t));
	long long written = 0, min_off = 0, max_off = 0;
	int samples = 0;
	struct timespec start, now, last = { 0, 0 };
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		snd_pcm_sframes_t n = snd_pcm_writei(pcm, silence, period);
		if (n == -EPIPE && xrun_mode) {
			epipes++;
			if ((err = snd_pcm_recover(pcm, (int)n, 1)) < 0) {
				fprintf(stderr, "recover: %s\n", snd_strerror(err));
				return 1;
			}
			continue;
		}
		if (n < 0) {
			fprintf(stderr, "write: %s\n", snd_strerror((int)n));
			return 1;
		}
		written += n;
		clock_gettime(CLOCK_MONOTONIC, &now);
		double t = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) * 1e-9;
		double since = (now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) * 1e-9;
		if (t > 0.5 && since >= 0.05) {		/* once it plays steadily */
			snd_pcm_sframes_t delay;
			if (snd_pcm_delay(pcm, &delay) == 0) {
				long long played = written - delay;
				long long off = (long long)jack_frame_time(jc) - played;
				if (samples++ == 0)
					min_off = max_off = off;
				if (off < min_off)
					min_off = off;
				if (off > max_off)
					max_off = off;
			}
			last = now;
		}
		if (xrun_mode && !slept && t > 1.0) {
			struct timespec late = { 0, 300 * 1000 * 1000 };
			nanosleep(&late, NULL);		/* its realtime side runs on */
			slept = 1;
		}
		if (t >= seconds)
			break;
	}
	if (xrun_mode) {
		printf("alsa-sync xrun: %d EPIPE after a 300 ms stall with %lu frames buffered; "
				"written %lld frames, then on\n", epipes, buffer, written);
		printf("%s\n", epipes >= 1 && written > (long long)rate * 2 ? "PASS" : "FAIL");
		snd_pcm_close(pcm);
		jack_client_close(jc);
		return 0;
	}
	printf("alsa-sync: %d samples over %d s, buffer %lu period %lu frames; JACK frames - played: "
			"spread %lld frames (one cycle: %u)\n", samples, seconds, buffer, period,
			max_off - min_off, quantum);
	printf("%s\n", samples > 10 && max_off - min_off <= (long long)quantum ? "PASS" : "FAIL");
	snd_pcm_close(pcm);
	jack_client_close(jc);
	return 0;
}
