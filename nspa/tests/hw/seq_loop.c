/* jack2-nspa hardware test: MIDI timing through -X seq, both directions.
 *
 * One process is an ALSA sequencer client ("nspaseqloop", ports "out" and
 * "in") and a JACK client. jackd -X seq exposes the ALSA ports as
 * system:midi_capture_N / system:midi_playback_N (found by their alias).
 *
 *   ALSA -> JACK: note events sent directly at a recorded time; the JACK
 *     client notes the frame each one is placed at. Latency = placed frame -
 *     send time in frames; the driver delays input by about one period.
 *   JACK -> ALSA: the JACK client writes notes at varying frame offsets; an
 *     RT thread timestamps their arrival on the ALSA port. Latency = arrival
 *     frame - intended frame.
 * Prints median, 5th-95th percentile spread and min/max (in frames) for both
 * directions, so the fork can be compared with 1.9.22 on the same device.
 *
 * Usage: seq_loop [events=200]
 * Exit 0 if every event arrived in both directions.
 */
#include <alsa/asoundlib.h>
#include <jack/jack.h>
#include <jack/midiport.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXEV 1000
static int nev = 200;
static snd_seq_t *seq;
static int sport_out, sport_in;
static jack_client_t *jc;
static jack_port_t *jin, *jout;

static long send_frame[MAXEV];        /* ALSA -> JACK, send time in frames (converted when sent) */
static atomic_int xruns;
static atomic_long recv_frame[MAXEV];   /* ALSA -> JACK, frame placed */
static atomic_long want_frame[MAXEV];   /* JACK -> ALSA, frame intended */
static atomic_long arrive_frame[MAXEV]; /* JACK -> ALSA, frame arrived */
static atomic_int out_next, out_go, stop;
static atomic_uint cycles;

static int process(jack_nframes_t n, void *arg)
{
    (void)arg;
    unsigned c = atomic_fetch_add(&cycles, 1);
    jack_nframes_t base = jack_last_frame_time(jc);
    void *ib = jack_port_get_buffer(jin, n);
    jack_nframes_t cnt = jack_midi_get_event_count(ib);
    for (jack_nframes_t i = 0; i < cnt; i++) {
        jack_midi_event_t ev;
        if (jack_midi_event_get(&ev, ib, i) || ev.size < 3 || (ev.buffer[0] & 0xf0) != 0x90)
            continue;
        int idx = ev.buffer[1] | ((ev.buffer[2] - 1) << 7);
        if (idx >= 0 && idx < nev)
            atomic_store(&recv_frame[idx], (long)(base + ev.time));
    }
    void *ob = jack_port_get_buffer(jout, n);
    jack_midi_clear_buffer(ob);
    int k = atomic_load(&out_next);
    if (atomic_load(&out_go) && k < nev && c % 3 == 0) {
        jack_nframes_t off = (c * 37u) % n;
        unsigned char *d = jack_midi_event_reserve(ob, off, 3);
        if (d) {
            d[0] = 0x90;
            d[1] = k & 0x7f;
            d[2] = (k >> 7) + 1;
            atomic_store(&want_frame[k], (long)(base + off));
            atomic_store(&out_next, k + 1);
        }
    }
    return 0;
}

static int on_xrun(void *arg)
{
    (void)arg;
    atomic_fetch_add(&xruns, 1);
    return 0;
}

static void *reader(void *arg)
{
    (void)arg;
    struct sched_param sp = {.sched_priority = 60};
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    while (!atomic_load(&stop)) {
        snd_seq_event_t *ev;
        if (snd_seq_event_input(seq, &ev) < 0)
            continue;
        jack_time_t t = jack_get_time();
        if (ev->type == SND_SEQ_EVENT_NOTEON && ev->dest.port == sport_in) {
            int idx = ev->data.note.note | ((ev->data.note.velocity - 1) << 7);
            if (idx >= 0 && idx < nev)
                atomic_store(&arrive_frame[idx], (long)jack_time_to_frames(jc, t));
        }
    }
    return NULL;
}

static const char *find_port(unsigned long flags)
{
    static char name[2][256];
    const char **ports = jack_get_ports(jc, "system:midi_", JACK_DEFAULT_MIDI_TYPE, flags);
    const char *found = NULL;
    for (int i = 0; ports && ports[i] && !found; i++) {
        jack_port_t *p = jack_port_by_name(jc, ports[i]);
        char a0[jack_port_name_size()], a1[jack_port_name_size()];
        char *al[2] = {a0, a1};
        int na = jack_port_get_aliases(p, al);
        for (int j = 0; j < na; j++)
            if (strstr(al[j], "nspaseqloop")) {
                int w = (flags & JackPortIsOutput) ? 0 : 1;
                snprintf(name[w], sizeof name[w], "%s", ports[i]);
                found = name[w];
                break;
            }
    }
    jack_free(ports);
    return found;
}

static int cmpl(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return x < y ? -1 : x > y;
}

static int report(const char *what, long *lat, int n)
{
    if (n == 0) {
        printf("  %s: no events\n", what);
        return 1;
    }
    qsort(lat, n, sizeof(long), cmpl);
    printf("  %-14s %3d/%d events, latency frames: median %ld, p5..p95 %ld..%ld, min..max %ld..%ld\n", what, n, nev,
           lat[n / 2], lat[n * 5 / 100], lat[n * 95 / 100], lat[0], lat[n - 1]);
    return n != nev;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 1)
        nev = atoi(argv[1]);
    if (nev > MAXEV)
        nev = MAXEV;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
        printf("FAIL: no ALSA sequencer\n");
        return 2;
    }
    snd_seq_set_client_name(seq, "nspaseqloop");
    sport_out = snd_seq_create_simple_port(seq, "out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                           SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    sport_in = snd_seq_create_simple_port(seq, "in", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                          SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (!(jc = jack_client_open("seqloop", JackNoStartServer, NULL))) {
        printf("FAIL: cannot open JACK client\n");
        return 2;
    }
    jin = jack_port_register(jc, "in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
    jout = jack_port_register(jc, "out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
    jack_set_process_callback(jc, process, NULL);
    jack_set_xrun_callback(jc, on_xrun, NULL);
    jack_activate(jc);

    const char *cap = NULL, *play = NULL;
    for (int i = 0; i < 100 && (!cap || !play); i++) {
        usleep(50000);
        cap = find_port(JackPortIsOutput);
        play = find_port(JackPortIsInput);
    }
    if (!cap || !play) {
        printf("FAIL: -X seq ports for nspaseqloop not found\n");
        return 2;
    }
    jack_connect(jc, cap, jack_port_name(jin));
    jack_connect(jc, jack_port_name(jout), play);
    printf("  ports: %s -> seqloop:in, seqloop:out -> %s\n", cap, play);
    {
        char a0[jack_port_name_size()], a1[jack_port_name_size()];
        char *al[2] = {a0, a1};
        int na = jack_port_get_aliases(jack_port_by_name(jc, cap), al);
        printf("  aliases of %s:", cap);
        for (int i = 0; i < na; i++)
            printf(" \"%s\"", al[i]);
        printf("\n");
    }

    pthread_t rt;
    pthread_create(&rt, NULL, reader, NULL);
    usleep(300000);

    /* ALSA -> JACK: events at irregular intervals, so they land all over the period */
    for (int i = 0; i < nev; i++) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_source(&ev, sport_out);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        snd_seq_ev_set_noteon(&ev, 0, i & 0x7f, (i >> 7) + 1);
        /* convert now: after an xrun the frame/time mapping moves, so past
         * times can't be converted later */
        send_frame[i] = (long)jack_time_to_frames(jc, jack_get_time());
        snd_seq_event_output_direct(seq, &ev);
        usleep(4000 + (i * 1307) % 3000);
    }
    usleep(200000);

    /* JACK -> ALSA */
    atomic_store(&out_go, 1);
    for (int i = 0; i < 2000 && atomic_load(&out_next) < nev; i++)
        usleep(10000);
    usleep(300000);
    atomic_store(&stop, 1);

    long lat[MAXEV];
    int n = 0, fail = 0;
    jack_nframes_t period = jack_get_buffer_size(jc);
    for (int i = 0; i < nev; i++)
        if (atomic_load(&recv_frame[i]))
            lat[n++] = atomic_load(&recv_frame[i]) - send_frame[i];
    printf("  period %u frames, %d xruns during the test\n", period, atomic_load(&xruns));
    fail |= report("ALSA -> JACK", lat, n);
    n = 0;
    for (int i = 0; i < nev; i++)
        if (atomic_load(&arrive_frame[i]) && atomic_load(&want_frame[i]))
            lat[n++] = atomic_load(&arrive_frame[i]) - atomic_load(&want_frame[i]);
    fail |= report("JACK -> ALSA", lat, n);

    /* unblock the reader: send it one event */
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, sport_out);
    snd_seq_ev_set_dest(&ev, snd_seq_client_id(seq), sport_in);
    snd_seq_ev_set_direct(&ev);
    snd_seq_ev_set_noteoff(&ev, 0, 0, 0);
    snd_seq_event_output_direct(seq, &ev);
    pthread_join(rt, NULL);
    jack_client_close(jc);
    snd_seq_close(seq);
    printf("%s: MIDI through -X seq in both directions\n", fail ? "FAIL" : "PASS");
    return fail;
}
