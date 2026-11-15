/*
    jackbus_simple: a JACK client on the JackBus socket.

    Like jack_simple_client: a stereo sine (440 Hz left, 660 Hz right), connected
    to the first two physical playback ports. The difference is how it connects:
    jackbus_client_open() reaches the server through $XDG_RUNTIME_DIR/jackbus/
    and gets its shared memory and futexes as file descriptors, so it needs no
    names in /dev/shm and no shm registry. After the open it is an ordinary JACK
    client: every jack_* call below is the usual one.

    Copyright (C) 2026 JackBus

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <jack/jack.h>
#include <jack/jackbus.h>

#define TABLE_SIZE 4800     /* 0.1 s at 48 kHz: one table period per 100 ms */

static jack_port_t* out_left;
static jack_port_t* out_right;
static float table[TABLE_SIZE];
static double phase_left, phase_right, step_left, step_right;
static volatile unsigned long cycles, xruns;
static volatile sig_atomic_t done;

static int process(jack_nframes_t nframes, void* arg)
{
    float* l = jack_port_get_buffer(out_left, nframes);
    float* r = jack_port_get_buffer(out_right, nframes);
    (void)arg;
    for (jack_nframes_t i = 0; i < nframes; i++) {
        l[i] = 0.2f * table[(int)phase_left];
        r[i] = 0.2f * table[(int)phase_right];
        phase_left += step_left;
        phase_right += step_right;
        if (phase_left >= TABLE_SIZE) phase_left -= TABLE_SIZE;
        if (phase_right >= TABLE_SIZE) phase_right -= TABLE_SIZE;
    }
    cycles++;
    return 0;
}

static int on_xrun(void* arg)
{
    (void)arg;
    xruns++;
    return 0;
}

static void on_shutdown(jack_status_t code, const char* reason, void* arg)
{
    (void)code; (void)arg;
    fprintf(stderr, "jackbus_simple: the server went away: %s\n", reason);
    done = 1;
}

static void on_signal(int sig)
{
    (void)sig;
    done = 1;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: jackbus_simple [-n name] [-s server] [-t seconds] [-c] [-f]\n"
            "  -n name     client name (default jackbus_simple)\n"
            "  -s server   server name (default: $JACK_DEFAULT_SERVER, else \"default\")\n"
            "  -t seconds  run that long (default: until interrupted)\n"
            "  -c          don't connect to the physical playback ports\n"
            "  -f          if the server has no JackBus socket, open over protocol 9\n");
}

int main(int argc, char* argv[])
{
    const char* name = "jackbus_simple";
    const char* server = NULL;
    int seconds = 0, connect = 1, fallback = 0, opt;
    jack_options_t options = JackNoStartServer;
    jack_status_t status = 0;
    jack_client_t* client;
    const char* how = "the JackBus socket";

    while ((opt = getopt(argc, argv, "n:s:t:cfh")) != -1) {
        switch (opt) {
            case 'n': name = optarg; break;
            case 's': server = optarg; options |= JackServerName; break;
            case 't': seconds = atoi(optarg); break;
            case 'c': connect = 0; break;
            case 'f': fallback = 1; break;
            default: usage(); return opt == 'h' ? 0 : 1;
        }
    }

    client = jackbus_client_open(name, options, &status, 0, server);
    if (!client && fallback) {
        /* a stock server, or no $XDG_RUNTIME_DIR: the usual way */
        how = "protocol 9 (no JackBus socket)";
        status = 0;
        client = jack_client_open(name, options, &status, server);
    }
    if (!client) {
        fprintf(stderr, "jackbus_simple: cannot open a client (status 0x%x)%s\n", status,
                fallback ? "" : "; -f falls back to protocol 9");
        return 1;
    }

    for (int i = 0; i < TABLE_SIZE; i++) {
        table[i] = (float)sin(2.0 * M_PI * i / TABLE_SIZE);
    }
    /* frequency = step * rate / TABLE_SIZE */
    step_left = 440.0 * TABLE_SIZE / jack_get_sample_rate(client);
    step_right = 660.0 * TABLE_SIZE / jack_get_sample_rate(client);

    jack_set_process_callback(client, process, NULL);
    jack_set_xrun_callback(client, on_xrun, NULL);
    jack_on_info_shutdown(client, on_shutdown, NULL);
    out_left = jack_port_register(client, "out_left", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    out_right = jack_port_register(client, "out_right", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    if (!out_left || !out_right) {
        fprintf(stderr, "jackbus_simple: no more JACK ports available\n");
        jack_client_close(client);
        return 1;
    }
    if (jack_activate(client) != 0) {
        fprintf(stderr, "jackbus_simple: cannot activate\n");
        jack_client_close(client);
        return 1;
    }

    printf("jackbus_simple: \"%s\" opened over %s, %u Hz, %u frames per period\n",
           jack_get_client_name(client), how, jack_get_sample_rate(client), jack_get_buffer_size(client));

    if (connect) {
        const char** ports = jack_get_ports(client, NULL, JACK_DEFAULT_AUDIO_TYPE, JackPortIsPhysical | JackPortIsInput);
        if (ports && ports[0]) {
            jack_connect(client, jack_port_name(out_left), ports[0]);
            jack_connect(client, jack_port_name(out_right), ports[1] ? ports[1] : ports[0]);
            printf("jackbus_simple: connected to %s and %s\n", ports[0], ports[1] ? ports[1] : ports[0]);
        } else {
            printf("jackbus_simple: no physical playback ports\n");
        }
        jack_free(ports);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    for (int t = 0; !done && (seconds == 0 || t < seconds * 10); t++) {
        usleep(100000);
    }

    jack_deactivate(client);
    printf("jackbus_simple: %lu cycles, %lu xruns\n", cycles, xruns);
    jack_client_close(client);
    return 0;
}
