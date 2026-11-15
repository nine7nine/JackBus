/* JackBus: JackMidiRawOutputWriteQueue across the 32-bit frame wrap.
 *
 * Drives the real queue from libjackserver with a mock send queue that, like
 * JackALSARawMidiSendQueue, refuses bytes that are not due yet. A sysex
 * (non-realtime) message is due just before 2^32, a MIDI clock byte
 * (realtime) just after. Stepping "now" across the wrap, the queue must send
 * the sysex first, each byte no earlier than its time, and the clock no
 * earlier than its time and not later than one step after it.
 */
#include "JackMidiRawOutputWriteQueue.h"
#include "JackMidiSendQueue.h"
#include "JackMidiUtil.h"
#include <cstdio>
#include <vector>

using namespace Jack;

struct MockSendQueue : public JackMidiSendQueue {
    using JackMidiSendQueue::EnqueueEvent;
    struct Sent { jack_nframes_t at, stamp; jack_midi_data_t byte; };
    jack_nframes_t now = 0;
    std::vector<Sent> sent;

    EnqueueResult EnqueueEvent(jack_nframes_t time, size_t size, jack_midi_data_t *buf) override
    {
        if (FrameAfter(time, now))
            return EVENT_EARLY;
        for (size_t i = 0; i < size; i++)
            sent.push_back({now, time, buf[i]});
        return OK;
    }
    jack_nframes_t GetNextScheduleFrame() override { return now; }
};

int main()
{
    const jack_nframes_t base = 0xFFFFFF00u;   // 256 frames before the wrap
    const jack_nframes_t t_sysex = base + 250;  // pre-wrap
    const jack_nframes_t t_clock = base + 262;  // post-wrap (= 6)
    jack_midi_data_t sysex[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
    jack_midi_data_t clock[] = {0xF8};
    int failures = 0;

    MockSendQueue send;
    JackMidiRawOutputWriteQueue queue(&send, 4096, 1024, 128);
    send.now = base;

    if (queue.EnqueueEvent(t_sysex, sizeof sysex, sysex) != JackMidiWriteQueue::OK ||
        queue.EnqueueEvent(t_clock, sizeof clock, clock) != JackMidiWriteQueue::OK) {
        printf("FAIL: enqueue\n");
        return 1;
    }
    for (jack_nframes_t step = 0; step < 400; step++) {
        send.now = base + step;  // wraps naturally
        queue.Process();
    }

    size_t clock_at = send.sent.size();
    for (size_t i = 0; i < send.sent.size(); i++) {
        const MockSendQueue::Sent &s = send.sent[i];
        if (FrameBefore(s.at, s.stamp)) {
            printf("FAIL: byte 0x%02x sent early at %u (due %u)\n", s.byte, s.at, s.stamp);
            failures++;
        }
        if (s.byte == 0xF8)
            clock_at = i;
    }
    if (send.sent.size() != sizeof sysex + sizeof clock) {
        printf("FAIL: %zu of %zu bytes sent\n", send.sent.size(), sizeof sysex + sizeof clock);
        failures++;
    }
    if (clock_at != sizeof sysex) {
        printf("FAIL: clock sent at position %zu, expected after the %zu sysex bytes\n", clock_at, sizeof sysex);
        failures++;
    } else if ((jack_nframes_t)(send.sent[clock_at].at - t_clock) > 1) {
        printf("FAIL: clock sent at %u, due %u\n", send.sent[clock_at].at, t_clock);
        failures++;
    }
    printf("%s: %zu bytes sent across the 2^32 frame wrap\n", failures ? "FAIL" : "PASS", send.sent.size());
    return failures ? 1 : 0;
}
