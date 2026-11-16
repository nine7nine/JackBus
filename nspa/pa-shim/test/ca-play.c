/*
 * libcanberra (KDE's and GNOME's event sounds) through its PulseAudio driver,
 * as the desktop uses it:
 *
 *   ca-play plain <wav> <rounds> <seconds>   ca_context_play without a callback,
 *                                            the stream cleaned up by libcanberra
 *                                            when it ends (a notification sound)
 *   ca-play callback <wav> <rounds>          ca_context_play_full, waiting for its
 *                                            callback (a settings page's preview)
 *   ca-play cancel <wav>                     destroyed while playing (leaving the
 *                                            page during a preview)
 *
 * Then the context is destroyed; "ok" once everything returned.
 */
#include <canberra.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int finished;
static int result;

static void done(ca_context *c, uint32_t id, int code, void *userdata)
{
	pthread_mutex_lock(&lock);
	finished = 1;
	result = code;
	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&lock);
}

int main(int argc, char **argv)
{
	if (argc < 3)
		return 2;
	const char *mode = argv[1], *wav = argv[2];
	int rounds = argc > 3 ? atoi(argv[3]) : 1;
	double secs = argc > 4 ? atof(argv[4]) : 1.0;
	ca_context *c;
	int r;
	if ((r = ca_context_create(&c)) < 0 || (r = ca_context_set_driver(c, "pulse")) < 0 ||
	    (r = ca_context_change_props(c, CA_PROP_APPLICATION_NAME, "ca-play", NULL)) < 0 ||
	    (r = ca_context_open(c)) < 0) {
		fprintf(stderr, "ca-play: open: %s\n", ca_strerror(r));
		return 1;
	}
	for (int i = 0; i < rounds; i++) {
		if (strcmp(mode, "plain") == 0) {
			r = ca_context_play(c, (uint32_t)i, CA_PROP_MEDIA_FILENAME, wav, NULL);
			if (r < 0) {
				fprintf(stderr, "ca-play: play: %s\n", ca_strerror(r));
				return 1;
			}
			usleep((useconds_t)(secs * 1e6) + 400000);
		} else {
			ca_proplist *p;
			ca_proplist_create(&p);
			ca_proplist_sets(p, CA_PROP_MEDIA_FILENAME, wav);
			finished = 0;
			r = ca_context_play_full(c, (uint32_t)i, p, done, NULL);
			ca_proplist_destroy(p);
			if (r < 0) {
				fprintf(stderr, "ca-play: play: %s\n", ca_strerror(r));
				return 1;
			}
			if (strcmp(mode, "cancel") == 0) {
				usleep(300000);
				break;
			}
			struct timespec t;
			clock_gettime(CLOCK_REALTIME, &t);
			t.tv_sec += 10;
			pthread_mutex_lock(&lock);
			while (!finished && pthread_cond_timedwait(&cond, &lock, &t) == 0)
				;
			int ok = finished && result == CA_SUCCESS;
			pthread_mutex_unlock(&lock);
			if (!ok) {
				fprintf(stderr, "ca-play: round %d: %s\n", i,
						finished ? ca_strerror(result) : "no callback in 10 s");
				return 1;
			}
		}
	}
	ca_context_destroy(c);
	printf("ok\n");
	return 0;
}
