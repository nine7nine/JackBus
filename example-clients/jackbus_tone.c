/*
    jackbus_tone: a node of the native JackBus API.

    A sine on one output, its phase computed from the cycle record's 64-bit
    frame position: the tone stays continuous and sample-accurate whatever
    happens (an xrun, the node being started late), without any phase state.
    The main thread polls the node's event fd and prints what it hears, plus a
    status line each second from what the process function published.

    Copyright (C) 2026 JackBus

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <jack/jack.h>
#include <jack/jackbus.h>

static jackbus_port_t* out;
static double freq = 440.0;
static volatile sig_atomic_t done;

/* published by the process function for the main thread */
static atomic_ullong last_frame, last_cycle, last_period_ns, last_xruns;

static int process(jackbus_node_t* node, const jackbus_cycle_t* c, void* arg)
{
    float* buf = jackbus_port_audio(out);
    (void)node; (void)arg;
    for (uint32_t i = 0; i < c->nframes; i++) {
        /* sin(2 pi f n / rate) from the absolute position n: fmod keeps the
           argument small (n * f stays exact in a double for years of frames) */
        double k = fmod((double)(c->frame + i) * freq, (double)c->sample_rate);
        buf[i] = 0.2f * (float)sin(2.0 * M_PI * k / c->sample_rate);
    }
    atomic_store_explicit(&last_frame, c->frame, memory_order_relaxed);
    atomic_store_explicit(&last_cycle, c->cycle, memory_order_relaxed);
    atomic_store_explicit(&last_period_ns, c->period_ns, memory_order_relaxed);
    atomic_store_explicit(&last_xruns, c->xruns, memory_order_relaxed);
    return 0;
}

static void on_signal(int sig)
{
    (void)sig;
    done = 1;
}

static const char* event_name(uint32_t type)
{
    switch (type) {
        case JACKBUS_EVENT_CLIENT: return "client";
        case JACKBUS_EVENT_PORT: return "port";
        case JACKBUS_EVENT_CONNECT: return "connect";
        case JACKBUS_EVENT_RENAME: return "rename";
        case JACKBUS_EVENT_GRAPH: return "graph";
        case JACKBUS_EVENT_BUFFER_SIZE: return "buffer size";
        case JACKBUS_EVENT_SAMPLE_RATE: return "sample rate";
        case JACKBUS_EVENT_XRUN: return "xrun";
        case JACKBUS_EVENT_FREEWHEEL: return "freewheel";
        case JACKBUS_EVENT_LATENCY: return "latency";
        case JACKBUS_EVENT_PROPERTY: return "property";
        case JACKBUS_EVENT_SHUTDOWN: return "shutdown";
        case JACKBUS_EVENT_LOST: return "lost";
        default: return "?";
    }
}

int main(int argc, char* argv[])
{
    const char* name = "jackbus_tone";
    int opt, seconds = 0, connect = 1;
    jack_status_t status = 0;

    while ((opt = getopt(argc, argv, "n:f:t:ch")) != -1) {
        switch (opt) {
            case 'n': name = optarg; break;
            case 'f': freq = atof(optarg); break;
            case 't': seconds = atoi(optarg); break;
            case 'c': connect = 0; break;
            default:
                fprintf(stderr, "usage: jackbus_tone [-n name] [-f hz] [-t seconds] [-c (no connections)]\n");
                return opt == 'h' ? 0 : 1;
        }
    }

    jackbus_node_t* node = jackbus_node_open(name, JackNoStartServer, &status);
    if (!node) {
        fprintf(stderr, "jackbus_tone: cannot open a node (status 0x%x): no JackBus server?\n", status);
        return 1;
    }
    out = jackbus_port_add(node, "out", JACKBUS_AUDIO, JackPortIsOutput);
    if (!out || jackbus_node_set_process(node, process, NULL) != 0 || jackbus_node_activate(node) != 0) {
        fprintf(stderr, "jackbus_tone: cannot set up the node\n");
        jackbus_node_close(node);
        return 1;
    }
    jack_client_t* client = jackbus_node_client(node);
    printf("jackbus_tone: %.1f Hz on %s\n", freq, jack_port_name(jackbus_port_jack(out)));
    if (connect) {
        const char** ports = jack_get_ports(client, NULL, JACK_DEFAULT_AUDIO_TYPE, JackPortIsPhysical | JackPortIsInput);
        for (int i = 0; ports && ports[i] && i < 2; i++) {
            jack_connect(client, jack_port_name(jackbus_port_jack(out)), ports[i]);
        }
        jack_free(ports);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    struct pollfd p = { jackbus_node_event_fd(node), POLLIN, 0 };
    int ticks = 0;
    while (!done && (seconds == 0 || ticks < seconds)) {
        if (poll(&p, 1, 1000) > 0) {
            jackbus_event_t e;
            while (jackbus_node_next_event(node, &e) == 1) {
                printf("  event %-11s on=%u value=%llu %s%s%s\n", event_name(e.type), e.on,
                       (unsigned long long)e.value, e.name, e.name2[0] ? " " : "", e.name2);
                if (e.type == JACKBUS_EVENT_SHUTDOWN) {
                    done = 1;
                }
            }
            continue;
        }
        ticks++;
        printf("jackbus_tone: cycle %llu, frame %llu, period %.1f us, xruns %llu\n",
               (unsigned long long)atomic_load(&last_cycle), (unsigned long long)atomic_load(&last_frame),
               atomic_load(&last_period_ns) / 1000.0, (unsigned long long)atomic_load(&last_xruns));
        fflush(stdout);
    }
    jackbus_node_close(node);
    return 0;
}
