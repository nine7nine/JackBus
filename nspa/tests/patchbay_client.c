/*
 * JackBus test client: behaves like a patchbay. It wants every notification (port
 * registration and connections, client registration, graph order, latency), has one
 * input connected from system:capture_1, and leaves at once when told the server is
 * going away (it closes its sockets right in the shutdown callback, as a client that
 * drops its connection there does). Stays until its standard input closes.
 *
 *   patchbay_client <name>
 *
 * Used by run.sh (notify-closed) and jackdbus.sh (stop with clients connected).
 */

#include <jack/jack.h>
#include <stdio.h>
#include <unistd.h>

static void port_reg(jack_port_id_t p, int on, void *a) { (void)p; (void)on; (void)a; }
static void port_conn(jack_port_id_t s, jack_port_id_t d, int on, void *a) { (void)s; (void)d; (void)on; (void)a; }
static void client_reg(const char *n, int on, void *a) { (void)n; (void)on; (void)a; }
static int graph_order(void *a) { (void)a; return 0; }
static void latency(jack_latency_callback_mode_t m, void *a) { (void)m; (void)a; }
static int process(jack_nframes_t n, void *a) { (void)n; (void)a; return 0; }

static void shutdown_cb(void *a)
{
    (void)a;
    for (int fd = 3; fd < 1024; fd++)
        close(fd);
    _exit(0);
}

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "patchbay";
    jack_client_t *c = jack_client_open(name, JackNoStartServer, NULL);
    if (!c)
        return 1;
    jack_set_process_callback(c, process, NULL);
    jack_set_port_registration_callback(c, port_reg, NULL);
    jack_set_port_connect_callback(c, port_conn, NULL);
    jack_set_client_registration_callback(c, client_reg, NULL);
    jack_set_graph_order_callback(c, graph_order, NULL);
    jack_set_latency_callback(c, latency, NULL);
    jack_on_shutdown(c, shutdown_cb, NULL);
    jack_port_t *in = jack_port_register(c, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    if (!in || jack_activate(c))
        return 1;
    char full[300];
    snprintf(full, sizeof full, "%s:in", jack_get_client_name(c));
    jack_connect(c, "system:capture_1", full);
    char b;
    while (read(0, &b, 1) > 0)
        ;
    jack_client_close(c);
    return 0;
}
