/*
 * A node of JackBus's native API (jack/jackbus.h) that does nothing but note,
 * each cycle, how long after the cycle's start its process function began.
 * With "priority", jackbus_node_set_priority() first. Writes, every 200 ms:
 * "median <us> cycles <n> prio <its realtime thread's priority>".
 *
 *   prio_node <name> <file> [priority]
 */
#define _GNU_SOURCE
#include <jack/jack.h>
#include <jack/jackbus.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define N 4096
static uint32_t lag[N];         /* ns after the cycle's start, the last N cycles */
static volatile long cycles;
static volatile int prio = -1;
static volatile sig_atomic_t done;

static int process(jackbus_node_t *node, const jackbus_cycle_t *c, void *arg)
{
	(void)node; (void)arg;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
	if (c->time_ns && now > c->time_ns)
		lag[cycles % N] = (uint32_t)(now - c->time_ns);
	if (prio < 0) {
		struct sched_param sp;
		int policy;
		if (pthread_getschedparam(pthread_self(), &policy, &sp) == 0)
			prio = policy == SCHED_FIFO || policy == SCHED_RR ? sp.sched_priority : 0;
	}
	cycles++;
	return 0;
}

static int cmp(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}

static void on_term(int sig)
{
	(void)sig;
	done = 1;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <name> <file> [priority]\n", argv[0]);
		return 2;
	}
	jackbus_node_t *node = jackbus_node_open(argv[1], JackNoStartServer, NULL);
	if (node == NULL)
		return 1;
	if (argc > 3 && strcmp(argv[3], "priority") == 0 && jackbus_node_set_priority(node, 1) != 0)
		return 1;
	if (jackbus_node_set_process(node, process, NULL) != 0 || jackbus_node_activate(node) != 0)
		return 1;
	signal(SIGTERM, on_term);
	static uint32_t sorted[N];
	char tmp[4096];
	snprintf(tmp, sizeof(tmp), "%s.tmp", argv[2]);
	while (!done) {
		usleep(200 * 1000);
		long n = cycles < N ? cycles : N;
		memcpy(sorted, lag, sizeof(sorted));
		qsort(sorted, (size_t)n, sizeof(sorted[0]), cmp);
		FILE *f = fopen(tmp, "w");
		if (f) {
			fprintf(f, "median %.1f cycles %ld prio %d\n", n ? sorted[n / 2] / 1000.0 : 0.0, cycles, prio);
			fclose(f);
			rename(tmp, argv[2]);
		}
	}
	jackbus_node_close(node);
	return 0;
}
