/*
 jack2-nspa: the sound card bridge driver (jack_bridge.so).

 One card on its own clock, joined to the master's graph: a client named after the card (its
 ALSA id), with physical capture and playback ports, run by the master in its own cycle
 (JackBridgeSet.h); the card has its own realtime thread (JackBridgeAlsa). Added and removed
 while the server runs (JackServer::AddBridge); it is never the master, and never in the
 master's slave list.
*/

#ifndef __JackBridgeDriver__
#define __JackBridgeDriver__

#include "JackAudioDriver.h"
#include "JackBridgeSet.h"
#include "JackBridgeAlsa.h"

namespace Jack
{

class JackBridgeDriver : public JackAudioDriver, public JackBridgeInterface
{
    public:

        JackBridgeDriver(const char* name, JackLockedEngine* engine, JackSynchro* table);
        virtual ~JackBridgeDriver();

        // the card (non-RT): opens and configures it, then the bridge's client in the graph
        int OpenBridgeCard(const JackBridgeAlsaParams& card, int quality);

        virtual int Attach();
        virtual int Close();
        virtual int Start();
        virtual int Stop();
        virtual int Read() { return 0; }    // not a slave: the master calls the bridge
        virtual int Write() { return 0; }

        // JackBridgeInterface (the master's cycle)
        virtual void BridgePlayback(int64_t t_ns, double ns_per_frame, jack_nframes_t n);
        virtual void BridgeCapture(int64_t t_ns, double ns_per_frame, jack_nframes_t n);
        virtual int BridgeBufferSize(jack_nframes_t buffer_size);
        virtual void BridgeMonitor();

    protected:

        virtual void UpdateLatencies();

    private:

        JackBridgeAlsa fCard;
        JackBridgeCapture fCap;
        JackBridgePlayback fPlay;
        char fDevice[JACK_CLIENT_NAME_SIZE + 1];
        bool fHaveCap;
        bool fHavePlay;
        float* fIn[DRIVER_PORT_NUM];
        float* fOut[DRIVER_PORT_NUM];
        // the monitor's (non-RT) view: what the ports report, what was last logged
        jack_nframes_t fReportedCapture;
        jack_nframes_t fReportedPlayback;
        int fLoggedCapState;
        int fLoggedPlayState;
        int fLoggedCard;
        int64_t fLoggedResyncs;
};

} // end of namespace

#endif
