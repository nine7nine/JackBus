/*
 * A JACK client that runs every cycle and counts what JACK tells every client
 * of: xruns. Passes its input to its output (connected to nothing unless the
 * caller connects it). Its counts go to a file, rewritten every 100 ms:
 * "xruns <n> cycles <n>".
 *
 *   xrun_victim <name> <file>
 */
#include <jack/jack.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static jack_port_t *in, *out;
static volatile int xruns;
static volatile long cycles;
static volatile sig_atomic_t done;

static int process(jack_nframes_t n, void *arg)
{
	(void)arg;
	memcpy(jack_port_get_buffer(out, n), jack_port_get_buffer(in, n), n * sizeof(float));
	cycles++;
	return 0;
}

static int on_xrun(void *arg)
{
	(void)arg;
	xruns++;
	return 0;
}

static void on_term(int sig)
{
	(void)sig;
	done = 1;
}

static void write_counts(const char *path)
{
	char tmp[4096];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (f == NULL)
		return;
	fprintf(f, "xruns %d cycles %ld\n", xruns, cycles);
	fclose(f);
	rename(tmp, path);
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <name> <file>\n", argv[0]);
		return 2;
	}
	jack_client_t *c = jack_client_open(argv[1], JackNoStartServer, NULL);
	if (c == NULL)
		return 1;
	in = jack_port_register(c, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
	out = jack_port_register(c, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
	if (!in || !out || jack_set_process_callback(c, process, NULL) || jack_set_xrun_callback(c, on_xrun, NULL)
	    || jack_activate(c))
		return 1;
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	while (!done) {
		write_counts(argv[2]);
		usleep(100 * 1000);
	}
	write_counts(argv[2]);
	jack_client_close(c);
	return 0;
}
