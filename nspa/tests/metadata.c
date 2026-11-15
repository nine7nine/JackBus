/* JackBus: metadata across libjack builds.
 *
 * The metadata store is a Berkeley DB environment in /dev/shm/jack_db-<uid>,
 * shared by the server and every client. A client linked against another
 * Berkeley DB version than the one that created the environment cannot open
 * it ("Cannot open DB environment") and loses metadata. Arch builds jack2
 * against db5.3, so the fork must too.
 *
 *   metadata set [secs]   client "metaset": sets a pretty-name on its port,
 *                         reads it back, stays alive for secs (default 5)
 *   metadata get          client "metaget": reads metaset:out's pretty-name
 * Exit 0 on success.
 */
#include <jack/jack.h>
#include <jack/metadata.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VALUE "nspa pretty name"

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    int set = !strcmp(argv[1], "set");
    jack_client_t *c = jack_client_open(set ? "metaset" : "metaget", JackNoStartServer, NULL);
    if (!c) {
        printf("FAIL: cannot open client\n");
        return 2;
    }
    jack_port_t *p;
    if (set) {
        p = jack_port_register(c, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (jack_set_property(c, jack_port_uuid(p), JACK_METADATA_PRETTY_NAME, VALUE, NULL)) {
            printf("FAIL: jack_set_property failed\n");
            return 1;
        }
    } else {
        p = jack_port_by_name(c, "metaset:out");
        if (!p) {
            printf("FAIL: no port metaset:out\n");
            return 1;
        }
    }
    char *value = NULL, *type = NULL;
    int r = jack_get_property(jack_port_uuid(p), JACK_METADATA_PRETTY_NAME, &value, &type);
    int ok = r == 0 && value && !strcmp(value, VALUE);
    printf("%s: %s: jack_get_property -> %d, \"%s\"\n", ok ? "PASS" : "FAIL", set ? "set" : "get", r, value ? value : "(null)");
    if (value)
        jack_free(value);
    if (type)
        jack_free(type);
    if (set && ok)
        sleep(argc > 2 ? atoi(argv[2]) : 5);
    jack_client_close(c);
    return ok ? 0 : 1;
}
