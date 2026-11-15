/*
    jackbus_events: the graph's events, from a node of the native JackBus API.

    What jack_evmon shows, without callbacks: the node queues every
    notification as an event, and this program's own thread reads them when
    the node's event fd is readable (it could be one more fd in an
    application's poll loop). Prints one line per event; Ctrl-C ends it.

    Copyright (C) 2026 JackBus

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include <jack/jack.h>
#include <jack/jackbus.h>

static volatile sig_atomic_t done;

static void on_signal(int sig)
{
    (void)sig;
    done = 1;
}

static void print_event(const jackbus_event_t* e)
{
    switch (e->type) {
        case JACKBUS_EVENT_CLIENT:
            printf("client %s %s\n", e->name, e->on ? "registered" : "gone");
            break;
        case JACKBUS_EVENT_PORT:
            printf("port %s %s (id %llu)\n", e->name[0] ? e->name : "?", e->on ? "registered" : "gone",
                   (unsigned long long)e->value);
            break;
        case JACKBUS_EVENT_CONNECT:
            printf("%s %s %s\n", e->name, e->on ? "->" : "-/-", e->name2);
            break;
        case JACKBUS_EVENT_RENAME:
            printf("port %s renamed %s\n", e->name2, e->name);
            break;
        case JACKBUS_EVENT_GRAPH:
            printf("graph order changed\n");
            break;
        case JACKBUS_EVENT_BUFFER_SIZE:
            printf("buffer size %llu\n", (unsigned long long)e->value);
            break;
        case JACKBUS_EVENT_SAMPLE_RATE:
            printf("sample rate %llu\n", (unsigned long long)e->value);
            break;
        case JACKBUS_EVENT_XRUN:
            printf("xrun (%llu us late)\n", (unsigned long long)e->value);
            break;
        case JACKBUS_EVENT_FREEWHEEL:
            printf("freewheel %s\n", e->on ? "on" : "off");
            break;
        case JACKBUS_EVENT_LATENCY:
            printf("%s latencies updated\n", e->value ? "playback" : "capture");
            break;
        case JACKBUS_EVENT_PROPERTY:
            printf("property %s of %llu %s\n", e->name, (unsigned long long)e->id,
                   e->value == 0 ? "created" : e->value == 1 ? "changed" : "deleted");
            break;
        case JACKBUS_EVENT_SHUTDOWN:
            printf("the server went away: %s\n", e->name);
            done = 1;
            break;
        case JACKBUS_EVENT_LOST:
            printf("%llu events lost: read the graph again\n", (unsigned long long)e->value);
            break;
    }
    fflush(stdout);
}

int main(int argc, char* argv[])
{
    const char* name = (argc > 1) ? argv[1] : "jackbus_events";
    jack_status_t status = 0;
    jackbus_node_t* node = jackbus_node_open(name, JackNoStartServer, &status);
    if (!node) {
        fprintf(stderr, "jackbus_events: cannot open a node (status 0x%x): no JackBus server?\n", status);
        return 1;
    }
    jackbus_node_activate(node);    // active: graph order and xrun events too

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    struct pollfd p = { jackbus_node_event_fd(node), POLLIN, 0 };
    while (!done) {
        if (poll(&p, 1, 500) > 0) {
            jackbus_event_t e;
            while (jackbus_node_next_event(node, &e) == 1) {
                print_event(&e);
            }
        }
    }
    jackbus_node_close(node);
    return 0;
}
