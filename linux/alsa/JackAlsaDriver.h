/*
Copyright (C) 2001 Paul Davis
Copyright (C) 2004 Grame

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#ifndef __JackAlsaDriver__
#define __JackAlsaDriver__

#include "JackAudioDriver.h"
#include "JackThreadedDriver.h"
#include "JackTime.h"
#include "alsa_driver.h"

namespace Jack
{

/*!
\brief The ALSA driver.
*/

class JackAlsaDriver : public JackAudioDriver
{

    private:

        jack_driver_t* fDriver;
        /* JackBus: the cards whose device reservations Open took (-1: none), released by
           Close. Not looked up again at Close: a card that was unplugged has no index left. */
        int fReservedCaptureCard;
        int fReservedPlaybackCard;
        jack_native_thread_t fReservationLoopThread;

        void UpdateLatencies();

    public:

        JackAlsaDriver(const char* name, const char* alias, JackLockedEngine* engine, JackSynchro* table)
		: JackAudioDriver(name, alias, engine, table),fDriver(NULL),
          fReservedCaptureCard(-1), fReservedPlaybackCard(-1)
        {}
        virtual ~JackAlsaDriver()
        {}

        int Open(jack_nframes_t buffer_size,
                 jack_nframes_t user_nperiods,
                 jack_nframes_t samplerate,
                 bool hw_monitoring,
                 bool hw_metering,
                 bool capturing,
                 bool playing,
                 DitherAlgorithm dither,
                 bool soft_mode,
                 bool monitor,
                 int inchannels,
                 int outchannels,
                 bool shorts_first,
                 const char* capture_driver_name,
                 const char* playback_driver_name,
                 jack_nframes_t capture_latency,
                 jack_nframes_t playback_latency,
                 const char* midi_driver_name);

        int Close();
        int Attach();
        int Detach();

        int Start();
        int Stop();
        int RestartStreams();   // xrun recovery: PCM streams only

        int Read();
        int Write();

        // BufferSize can be changed
        /* JackBus: the wake timer (alsa_driver_wait): "adaptive" (the default), "on", "off",
           or a margin in us (on) */
        void SetWakeTimer(const char* mode)
        {
            if (fDriver == NULL) {
                return;
            }
            alsa_driver_t* d = (alsa_driver_t *)fDriver;
            d->wake_timer_us = -1;
            d->wake_timer_adaptive = 0;
            if (strcmp(mode, "off") == 0) {
                return;
            }
            const bool adaptive = strcmp(mode, "adaptive") == 0;
            const bool on = strcmp(mode, "on") == 0;
            const bool margin = mode[0] >= '0' && mode[0] <= '9';
            if (!adaptive && !on && !margin) {
                jack_error("ALSA: wake timer \"%s\" not understood (adaptive, on, off or a margin in us): adaptive", mode);
            }
            if (d->batch) {
                if (on || margin) {     /* the default says nothing about it */
                    jack_info("ALSA: wake timer not used: a batch device (its position moves only when it reports)");
                }
                return;
            }
            if (on) {
                d->wake_timer_us = 0;
            } else if (margin) {
                d->wake_timer_us = atoi(mode);
            } else {
                d->wake_timer_adaptive = 1;
                return;
            }
            jack_info("ALSA: wake timer on, %d us after a period is due",
                      d->wake_timer_us > 0 ? d->wake_timer_us : alsa_driver_wake_margin(d));
        }

        bool IsDeviceLost()
        {
            return fDriver != NULL && ((alsa_driver_t *)fDriver)->device_lost;
        }

        bool IsFixedBufferSize()
        {
            return false;
        }

        int SetBufferSize(jack_nframes_t buffer_size);

        void ReadInputAux(jack_nframes_t orig_nframes, snd_pcm_sframes_t contiguous, snd_pcm_sframes_t nread);
        void MonitorInputAux();
        void ClearOutputAux();
        void WriteOutputAux(jack_nframes_t orig_nframes, snd_pcm_sframes_t contiguous, snd_pcm_sframes_t nwritten);
        void SetTimetAux(jack_time_t time);

        int PortSetDefaultMetadata(jack_port_id_t port_id, const char* pretty_name);

        // JACK API emulation for the midi driver
        int is_realtime() const;
        int create_thread(pthread_t *thread, int prio, int rt, void *(*start_func)(void*), void *arg);

        jack_port_id_t port_register(const char *port_name, const char *port_type, unsigned long flags, unsigned long buffer_size);
        int port_unregister(jack_port_id_t port_index);
        void* port_get_buffer(int port, jack_nframes_t nframes);
        int port_set_alias(int port, const char* name);

        jack_nframes_t get_sample_rate() const;
        jack_nframes_t frame_time() const;
        jack_nframes_t last_frame_time() const;
};

} // end of namespace

#endif
