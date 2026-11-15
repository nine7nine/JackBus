/*
 * jackbus-pw: MIDI between JACK's MIDI buffers and PipeWire's control
 * sequences (a spa_pod_sequence per cycle, events at frame offsets).
 *
 * The application chooses MIDI 1.0 bytes ("8 bit raw midi", SPA_CONTROL_Midi)
 * or UMP ("32 bit raw UMP", SPA_CONTROL_UMP); JACK carries MIDI 1.0. Both
 * directions are realtime-safe: no allocation, the converters work in place.
 */
#include <string.h>

#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/control/control.h>
#include <spa/control/ump-utils.h>

#include <jack/midiport.h>

#include "shim.h"

static uint32_t empty_sequence(void *dst, uint32_t maxsize)
{
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(dst, maxsize);
	struct spa_pod_frame f;
	spa_pod_builder_push_sequence(&b, &f, 0);
	spa_pod_builder_pop(&b, &f);
	return b.state.offset <= maxsize ? b.state.offset : 0;
}

uint32_t shim_midi_from_jack(void *jbuf, void *dst, uint32_t maxsize, bool ump)
{
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(dst, maxsize);
	struct spa_pod_frame f;
	uint32_t count = jbuf ? jack_midi_get_event_count(jbuf) : 0;

	spa_pod_builder_push_sequence(&b, &f, 0);
	for (uint32_t i = 0; i < count; i++) {
		jack_midi_event_t ev;
		if (jack_midi_event_get(&ev, jbuf, i) != 0 || ev.size == 0)
			continue;
		if (!ump) {
			spa_pod_builder_control(&b, ev.time, SPA_CONTROL_Midi);
			spa_pod_builder_bytes(&b, ev.buffer, ev.size);
			continue;
		}
		uint8_t *m = ev.buffer;
		size_t left = ev.size;
		uint64_t state = 0;
		while (left > 0) {
			uint32_t u[4];
			int n = spa_ump_from_midi(&m, &left, u, sizeof(u), 0, &state);
			if (n <= 0)
				break;
			spa_pod_builder_control(&b, ev.time, SPA_CONTROL_UMP);
			spa_pod_builder_bytes(&b, u, n);
		}
	}
	spa_pod_builder_pop(&b, &f);
	if (b.state.offset > maxsize)		/* too many events for the buffer */
		return empty_sequence(dst, maxsize);
	return b.state.offset;
}

void shim_midi_to_jack(const void *base, uint32_t maxsize, uint32_t offset, uint32_t size,
		void *jbuf, uint32_t nframes)
{
	if (size == 0 || nframes == 0 || offset > maxsize || maxsize - offset < sizeof(struct spa_pod))
		return;
	const struct spa_pod *pod = SPA_PTROFF(base, offset, const struct spa_pod);
	/* the pod's own size bounds it: get_dsp_buffer() users leave the chunk
	 * at n_samples * 4 bytes, whatever they wrote */
	if (SPA_POD_SIZE(pod) > maxsize - offset || !spa_pod_is_sequence(pod))
		return;
	const struct spa_pod_sequence *seq = (const struct spa_pod_sequence *)pod;
	const struct spa_pod_control *c;
	uint32_t last = 0;

	SPA_POD_SEQUENCE_FOREACH(seq, c) {
		uint32_t off = SPA_MIN(c->offset, nframes - 1);
		if (off < last)
			off = last;		/* JACK wants them in order */
		last = off;
		const void *body = SPA_POD_BODY_CONST(&c->value);
		uint32_t bsize = SPA_POD_BODY_SIZE(&c->value);
		if (c->type == SPA_CONTROL_Midi) {
			if (bsize > 0)
				jack_midi_event_write(jbuf, off, body, bsize);
		} else if (c->type == SPA_CONTROL_UMP) {
			const uint32_t *u = body;
			size_t left = bsize;
			uint64_t state = 0;
			while (left > 0) {
				uint8_t m[16];
				int n = spa_ump_to_midi(&u, &left, m, sizeof(m), &state);
				if (n <= 0)
					break;
				jack_midi_event_write(jbuf, off, m, n);
			}
		}
	}
}
