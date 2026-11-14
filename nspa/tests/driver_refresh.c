/*
 * driver_refresh: jackctl_driver_refresh_constraints (JackControlNspa.h) on the ALSA driver.
 *
 * Loads the server's drivers, prints the ALSA device list, then refreshes it N times and checks
 * that nothing changes when no card comes or goes, that the list stays the same, and that no
 * file descriptor is leaked (the device enumeration used to leak a control handle on its error
 * paths). With -w it instead watches: it refreshes once a second and prints the list whenever a
 * refresh reports a change (plug a card in or out meanwhile).
 *
 *   driver_refresh [-n count] [-w seconds]
 *
 * Exit status: 0 ok, 1 failure, 77 no ALSA driver (skipped).
 */
#include <jack/control.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "JackControlNspa.h"

static int count_fds(void)
{
	DIR *d = opendir("/proc/self/fd");
	int n = 0;
	if (!d)
		return -1;
	while (readdir(d))
		n++;
	closedir(d);
	return n;
}

static jackctl_parameter_t *find_param(jackctl_driver_t *drv, const char *name)
{
	for (const JSList *n = jackctl_driver_get_parameters(drv); n; n = jack_slist_next(n)) {
		jackctl_parameter_t *p = (jackctl_parameter_t *)n->data;
		if (!strcmp(jackctl_parameter_get_name(p), name))
			return p;
	}
	return NULL;
}

/* the device list as one string ("value=description;..."), for comparing */
static char *list_string(jackctl_parameter_t *p)
{
	size_t cap = 4096, len = 0;
	char *s = malloc(cap);
	s[0] = 0;
	uint32_t n = jackctl_parameter_get_enum_constraints_count(p);
	for (uint32_t i = 0; i < n; i++) {
		union jackctl_parameter_value v = jackctl_parameter_get_enum_constraint_value(p, i);
		const char *d = jackctl_parameter_get_enum_constraint_description(p, i);
		size_t need = strlen(v.str) + strlen(d) + 3;
		if (len + need >= cap) { cap = (cap + need) * 2; s = realloc(s, cap); }
		len += snprintf(s + len, cap - len, "%s=%s;", v.str, d);
	}
	return s;
}

static void print_list(jackctl_parameter_t *p)
{
	uint32_t n = jackctl_parameter_get_enum_constraints_count(p);
	for (uint32_t i = 0; i < n; i++)
		printf("  %s  %s\n", jackctl_parameter_get_enum_constraint_value(p, i).str,
		       jackctl_parameter_get_enum_constraint_description(p, i));
}

int main(int argc, char **argv)
{
	int count = 500, watch = 0, o;
	while ((o = getopt(argc, argv, "n:w:")) != -1) {
		if (o == 'n') count = atoi(optarg);
		else if (o == 'w') watch = atoi(optarg);
		else return 2;
	}
	jackctl_server_t *server = jackctl_server_create2(NULL, NULL, NULL);
	if (!server) { fprintf(stderr, "no server object\n"); return 1; }
	jackctl_driver_t *alsa = NULL;
	for (const JSList *n = jackctl_server_get_drivers_list(server); n; n = jack_slist_next(n))
		if (!strcmp(jackctl_driver_get_name((jackctl_driver_t *)n->data), "alsa"))
			alsa = (jackctl_driver_t *)n->data;
	if (!alsa) { printf("no alsa driver: skipped\n"); jackctl_server_destroy(server); return 77; }

	jackctl_parameter_t *dev = find_param(alsa, "device");
	if (!dev) { fprintf(stderr, "alsa driver without a device parameter\n"); return 1; }
	char *before = list_string(dev);
	printf("ALSA devices at start:\n");
	print_list(dev);

	int fail = 0;
	if (watch) {
		for (int t = 0; t < watch; t++) {
			int r = jackctl_driver_refresh_constraints(alsa);
			if (r < 0) { printf("refresh failed\n"); fail = 1; break; }
			if (r > 0) {
				printf("changed (%d parameter%s) after %d s:\n", r, r == 1 ? "" : "s", t);
				print_list(dev);
			}
			sleep(1);
		}
	} else {
		/* warm up once: dlopen/libasound bring their own descriptors the first time */
		if (jackctl_driver_refresh_constraints(alsa) < 0) { printf("refresh failed\n"); return 1; }
		const int fds0 = count_fds();
		int changed = 0;
		for (int i = 0; i < count; i++) {
			int r = jackctl_driver_refresh_constraints(alsa);
			if (r < 0) { printf("refresh %d failed\n", i); fail = 1; break; }
			changed += r;
		}
		const int fds1 = count_fds();
		char *after = list_string(dev);
		printf("%d refreshes: %d reported changes, list %s, fds %d -> %d\n", count, changed,
		       strcmp(before, after) ? "DIFFERENT" : "the same", fds0, fds1);
		if (changed || strcmp(before, after) || fds1 != fds0)
			fail = 1;
		free(after);
	}
	free(before);
	jackctl_server_destroy(server);
	printf(fail ? "FAIL\n" : "ok\n");
	return fail;
}
