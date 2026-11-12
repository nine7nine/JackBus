/* jack2-nspa hardware test helper: create N ALSA sequencer ports (half
 * readable, half writable, all subscribable) and keep them for S seconds.
 * Usage: seq_ports N seconds */
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 100, secs = argc > 2 ? atoi(argv[2]) : 10;
    snd_seq_t *seq;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) { perror("snd_seq_open"); return 2; }
    snd_seq_set_client_name(seq, "nspa-seq-test");
    for (int i = 0; i < n; i++) {
        char name[32];
        snprintf(name, sizeof name, "port %d", i);
        unsigned caps = (i & 1) ? (SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE)
                                : (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ);
        if (snd_seq_create_simple_port(seq, name, caps, SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION) < 0) {
            fprintf(stderr, "port %d failed\n", i); return 2;
        }
    }
    fprintf(stderr, "created %d sequencer ports\n", n);
    sleep(secs);
    snd_seq_close(seq);
    return 0;
}
