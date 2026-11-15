/*
 jack2-nspa: the sound card bridge driver (see JackBridgeDriver.h).
*/

#include "JackBridgeDriver.h"
#include "JackEngineControl.h"
#include "JackGraphManager.h"
#include "JackLockedEngine.h"
#include "JackPort.h"
#include "JackError.h"
#include "driver_interface.h"
#include "JackDriverLoader.h"
#include "JackServerGlobals.h"

#include "JackControlNspa.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// the card's ALSA id in a device name ("hw:U44,0", "plughw:CARD=U44,DEV=0" -> "U44"); false: none
static bool CardName(const char* device, char* card, size_t size)
{
    const char* p = strchr(device, ':');
    if (!p) {
        return false;
    }
    snprintf(card, size, "%s", p + 1);
    char* comma = strchr(card, ',');
    if (comma) {
        *comma = 0;
    }
    if (strncmp(card, "CARD=", 5) == 0) {
        memmove(card, card + 5, strlen(card + 5) + 1);
    }
    return card[0] != 0;
}

// the card's index now (-1: not a card's device name, or the card is not there); quiet
static int CardIndex(const char* device)
{
    char card[64];
    return CardName(device, card, sizeof(card)) ? snd_card_get_index(card) : -1;
}

// the PCM device number in a device name ("hw:PCH,3" -> 3, "hw:CARD=PCH,DEV=3" -> 3; else 0)
static int DeviceNumber(const char* device)
{
    const char* d = strstr(device, "DEV=");
    if (d) {
        return atoi(d + 4);
    }
    d = strchr(device, ',');
    return (d && d[1] >= '0' && d[1] <= '9') ? atoi(d + 1) : 0;
}

// Whether a card's PCM device has this stream (a USB microphone has no playback, a USB DAC no
// capture), asked quietly of the card's control device. Names other than "hw:"/"plughw:" ones,
// or a card not there: assumed (the open says).
static bool HasStream(const char* device, snd_pcm_stream_t stream)
{
    if (strncmp(device, "hw:", 3) != 0 && strncmp(device, "plughw:", 7) != 0) {
        return true;
    }
    const int index = CardIndex(device);
    if (index < 0) {
        return true;
    }
    const int dev = DeviceNumber(device);
    char ctl[32];
    snprintf(ctl, sizeof(ctl), "hw:%d", index);
    snd_ctl_t* handle;
    if (snd_ctl_open(&handle, ctl, 0) < 0) {
        return true;
    }
    snd_pcm_info_t* info;
    snd_pcm_info_alloca(&info);
    snd_pcm_info_set_device(info, unsigned(dev));
    snd_pcm_info_set_subdevice(info, 0);
    snd_pcm_info_set_stream(info, stream);
    const int err = snd_ctl_pcm_info(handle, info);
    snd_ctl_close(handle);
    return err != -ENOENT;
}

namespace Jack
{

JackBridgeDriver::JackBridgeDriver(const char* name, JackLockedEngine* engine, JackSynchro* table)
    : JackAudioDriver(name, "alsa_pcm", engine, table), fHaveCap(false), fHavePlay(false),
      fReportedCapture(0), fReportedPlayback(0), fLoggedCapState(-1), fLoggedPlayState(-1), fLoggedCard(-1),
      fLoggedResyncs(0), fLastReopen(0), fReopenTries(0), fReopening(false), fReopenLogged(false),
      fReservedCard(-1), fReserveFailed(-1), fLoggedXruns(0), fLoggedUnderruns(0), fLoggedMissing(0), fXrunsLoggedAt(0), fLoggedCapMs(0.0), fLoggedPlayMs(0.0), fQuality(1), fPeriods(2), fCardRate(0), fCardPeriod(0)
{
    fDevice[0] = 0;
    memset(fIn, 0, sizeof(fIn));
    memset(fOut, 0, sizeof(fOut));
}

JackBridgeDriver::~JackBridgeDriver()
{
    fCard.Stop();
    Release();
}

// The card's device reservation (org.freedesktop.ReserveDevice1.Audio<index>, as the master's
// card has): the desktop's sound server leaves the card alone, and lets go of it if it has it.
// Taken before the card is opened, kept while it is unplugged (it may come back with the same
// index), moved if it comes back with another. Without one (no session bus) the card is opened
// anyway: whether it can be is the open's to say.
bool JackBridgeDriver::Reserve()
{
    if (JackServerGlobals::on_device_acquire == NULL) {
        return true;
    }
    const int index = CardIndex(fDevice);
    if (index < 0 || index == fReservedCard) {
        return true;
    }
    if (index == fReserveFailed) {
        return false;   // tried for this index, and said so: not again each second
    }
    Release();
    char name[32];
    snprintf(name, sizeof(name), "Audio%d", index);
    if (!JackServerGlobals::on_device_acquire(name)) {
        fReserveFailed = index;
        return false;
    }
    fReservedCard = index;
    fReserveFailed = -1;
    return true;
}

void JackBridgeDriver::Release()
{
    if (fReservedCard >= 0 && JackServerGlobals::on_device_release != NULL) {
        char name[32];
        snprintf(name, sizeof(name), "Audio%d", fReservedCard);
        JackServerGlobals::on_device_release(name);
    }
    fReservedCard = -1;
}

int JackBridgeDriver::OpenBridgeCard(const JackBridgeAlsaParams& requested, int quality)
{
    // the client in the graph first: the master's rate and priority come with it
    snprintf(fDevice, sizeof(fDevice), "%s", requested.fCapture ? requested.fCapture : requested.fPlayback);
    if (OpenBridge(fDevice, fDevice, 0, 0) < 0) {
        return -1;
    }
    JackBridgeAlsaParams card = requested;
    card.fRate = fEngineControl->fSampleRate;     // the master's: the bridge only corrects drift
    card.fPriority = fEngineControl->fRealTime ? fEngineControl->fServerPriority - 1 : 0;
    fQuality = quality;
    fPeriods = card.fPeriods;
    if (!Reserve()) {
        jack_error("bridge %s: the card %s is not reserved: the desktop's sound server may hold it", fClientControl.fName, fDevice);
    }
    if (!fCard.Open(card)) {
        char reason[160];
        fCard.FailReason(reason, sizeof(reason));
        jack_error("bridge %s: cannot open the card %s (%s)", fClientControl.fName, fDevice, reason);
        Release();
        JackDriver::CloseBridge();
        return -1;
    }
    fCaptureChannels = card.fCapture ? fCard.CaptureChannels() : 0;
    fPlaybackChannels = card.fPlayback ? fCard.PlaybackChannels() : 0;
    if (fCaptureChannels > DRIVER_PORT_NUM || fPlaybackChannels > DRIVER_PORT_NUM) {
        jack_error("bridge %s: too many channels", fClientControl.fName);
        fCard.Stop();
        Release();
        JackDriver::CloseBridge();
        return -1;
    }
    fWithMonitorPorts = false;
    memset(fCapturePortList, 0, sizeof(fCapturePortList));
    memset(fPlaybackPortList, 0, sizeof(fPlaybackPortList));
    memset(fMonitorPortList, 0, sizeof(fMonitorPortList));

    if (!SetupDirections(fCard.Rate(), fCard.Period())) {
        jack_error("bridge %s: cannot set up its resampling", fClientControl.fName);
        fCard.Stop();
        Release();
        JackDriver::CloseBridge();
        return -1;
    }
    jack_info("bridge %s: %s, %u Hz, period %d x %d, %d in / %d out%s", fClientControl.fName, fDevice, fCard.Rate(),
              fCard.Period(), fCard.Periods(), fCaptureChannels, fPlaybackChannels, fCard.Batch() ? ", batch" : "");
    return 0;
}

bool JackBridgeDriver::SetupDirections(unsigned card_rate, int card_period)
{
    // new objects in the same places: nothing of the old state (clock, delays, FIFO) is kept,
    // and the card side's pointers to them stay valid
    fCap.~JackBridgeCapture();
    new (&fCap) JackBridgeCapture();
    fPlay.~JackBridgePlayback();
    new (&fPlay) JackBridgePlayback();
    fHaveCap = false;
    fHavePlay = false;
    fCardRate = card_rate;
    fCardPeriod = card_period;

    JackBridgeParams p;
    p.fMasterRate = fEngineControl->fSampleRate;
    p.fCardRate = card_rate;
    p.fMasterPeriod = fEngineControl->fBufferSize;
    p.fMaxMasterPeriod = 4096 > p.fMasterPeriod ? 4096 : p.fMasterPeriod;
    p.fCardPeriod = card_period;
    // the buffer the card opened with (a USB card may have been given a third period)
    p.fCardBuffer = fCard.BufferSize() > 0 ? fCard.BufferSize() : card_period * fPeriods;
    p.fQuality = fQuality;
    p.fMargin = 96;
    if (fCaptureChannels) {
        p.fChannels = fCaptureChannels;
        fHaveCap = fCap.Init(p);
    }
    if (fPlaybackChannels) {
        p.fChannels = fPlaybackChannels;
        fHavePlay = fPlay.Init(p);
    }
    return (!fCaptureChannels || fHaveCap) && (!fPlaybackChannels || fHavePlay);
}

int JackBridgeDriver::BridgeSampleRate(jack_nframes_t sample_rate)
{
    // the card at the new rate (the ports stay, so the same channels); a card that cannot do it
    // runs at the rate it can, resampled
    fCard.Stop();
    JackBridgeAlsaParams card = fCard.Params();
    card.fRate = sample_rate;
    card.fCaptureChannels = fCaptureChannels;
    card.fPlaybackChannels = fPlaybackChannels;
    bool open = fCard.Open(card);
    if (open && ((card.fCapture && fCard.CaptureChannels() != fCaptureChannels)
                 || (card.fPlayback && fCard.PlaybackChannels() != fPlaybackChannels))) {
        fCard.Stop();
        open = false;
    }
    const unsigned card_rate = open ? fCard.Rate() : sample_rate;
    const int card_period = open ? fCard.Period() : fCardPeriod;
    if (!SetupDirections(card_rate, card_period)) {
        jack_error("bridge %s: cannot set up its resampling for %u Hz", fClientControl.fName, sample_rate);
        fCard.Stop();
        return -1;
    }
    JackBridgeCapture* cap = fHaveCap ? &fCap : NULL;
    JackBridgePlayback* play = fHavePlay ? &fPlay : NULL;
    if (open && fCard.Start(cap, play)) {
        if (card_rate == sample_rate) {
            jack_info("bridge %s: %u Hz now, as the master", fClientControl.fName, card_rate);
        } else {
            jack_info("bridge %s: %u Hz now (the master's %u Hz, resampled)", fClientControl.fName, card_rate, sample_rate);
        }
    } else {
        // away or busy: lost, and opened again at the new rate when it can be (BridgeMonitor)
        char reason[160];
        fCard.FailReason(reason, sizeof(reason));
        if (open) {
            fCard.Stop();
        }
        jack_error("bridge %s: the card is not open at %u Hz (%s): trying again", fClientControl.fName, sample_rate, reason);
        fCard.SetLost(cap, play, card_rate);
    }
    // the monitor's view of it starts again
    fLoggedCapState = -1;
    fLoggedPlayState = -1;
    fLoggedResyncs = 0;
    UpdateLatencies();
    return 0;
}

void JackBridgeDriver::BridgeStatus(jackctl_bridge_status* st)
{
    memset(st, 0, sizeof(*st));
    snprintf(st->name, sizeof(st->name), "%s", fClientControl.fName);
    snprintf(st->device, sizeof(st->device), "%s", fDevice);
    st->card = fCard.State();
    st->capture = fHaveCap ? fCap.PublishedState() : -1;
    st->playback = fHavePlay ? fPlay.PublishedState() : -1;
    st->rate = fCardRate;
    st->period = fCardPeriod;
    st->nperiods = fCard.Periods() > 0 ? fCard.Periods() : fPeriods;
    st->capture_channels = fCaptureChannels;
    st->playback_channels = fPlaybackChannels;
    const double cap = fHaveCap ? fCap.PublishedLatency() : 0.0;
    const double play = fHavePlay ? fPlay.PublishedLatency() : 0.0;
    st->capture_latency = cap;
    st->playback_latency = play > 0.0 ? play + fEngineControl->fBufferSize : 0.0;
    // capture takes more card frames per master frame from a faster card; playback fewer
    // master frames per card frame
    if (cap > 0.0) {
        st->drift_ppm = (fCap.PublishedRatio() - 1.0) * 1e6;
    } else if (play > 0.0 && fPlay.PublishedRatio() > 0.0) {
        st->drift_ppm = (1.0 / fPlay.PublishedRatio() - 1.0) * 1e6;
    }
    st->xruns = fCard.Xruns();
    st->resyncs = (fHaveCap ? fCap.PublishedResyncs() : 0) + (fHavePlay ? fPlay.PublishedResyncs() : 0);
    // capture underruns, playback overflows, and playback frames the card played as silence
    st->underruns = (fHaveCap ? fCap.PublishedUnderruns() : 0) + (fHavePlay ? fPlay.PublishedUnderruns() : 0)
        + fCard.PlaybackMissing();
}

int JackBridgeDriver::Attach()
{
    JackPort* port;
    jack_port_id_t port_index;
    char name[REAL_JACK_PORT_NAME_SIZE + 1];
    char alias[REAL_JACK_PORT_NAME_SIZE + 1];

    // ID:capture_N / ID:playback_N, physical; aliases as the ALSA driver's, plus system:ID_...
    for (int i = 0; i < fCaptureChannels; i++) {
        snprintf(name, sizeof(name), "%s:capture_%d", fClientControl.fName, i + 1);
        if (fEngine->PortRegister(fClientControl.fRefNum, name, JACK_DEFAULT_AUDIO_TYPE, CaptureDriverFlags,
                                  fEngineControl->fBufferSize, &port_index) < 0) {
            jack_error("bridge: cannot register port %s", name);
            return -1;
        }
        port = fGraphManager->GetPort(port_index);
        snprintf(alias, sizeof(alias), "alsa_pcm:%s:out%d", fDevice, i + 1);
        port->SetAlias(alias);
        snprintf(alias, sizeof(alias), "system:%s_capture_%d", fClientControl.fName, i + 1);
        port->SetAlias(alias);
        fCapturePortList[i] = port_index;
    }
    for (int i = 0; i < fPlaybackChannels; i++) {
        snprintf(name, sizeof(name), "%s:playback_%d", fClientControl.fName, i + 1);
        if (fEngine->PortRegister(fClientControl.fRefNum, name, JACK_DEFAULT_AUDIO_TYPE, PlaybackDriverFlags,
                                  fEngineControl->fBufferSize, &port_index) < 0) {
            jack_error("bridge: cannot register port %s", name);
            return -1;
        }
        port = fGraphManager->GetPort(port_index);
        snprintf(alias, sizeof(alias), "alsa_pcm:%s:in%d", fDevice, i + 1);
        port->SetAlias(alias);
        snprintf(alias, sizeof(alias), "system:%s_playback_%d", fClientControl.fName, i + 1);
        port->SetAlias(alias);
        fPlaybackPortList[i] = port_index;
    }
    UpdateLatencies();
    return 0;
}

void JackBridgeDriver::UpdateLatencies()
{
    // The delays the bridge holds, in master frames. Before a direction has placed its delay
    // (capture learns the card side's lateness first), the usual one. Playback also waits a
    // cycle: it takes the previous cycle's buffers, as the master's own ports in async mode.
    const jack_nframes_t n = fEngineControl->fBufferSize;
    // the card as the directions are set up for it (the card itself may be away now)
    const double card_to_master = fCardRate ? double(fEngineControl->fSampleRate) / double(fCardRate) : 1.0;
    const int period = fCardPeriod;
    jack_latency_range_t range;
    double cap = fHaveCap ? fCap.PublishedLatency() : 0.0;
    if (cap <= 0.0) {
        cap = (double(n) + 32.0 + 2.0 * period + 96.0);
    }
    range.min = range.max = jack_nframes_t(cap + 0.5);
    fReportedCapture = range.min;
    for (int i = 0; i < fCaptureChannels; i++) {
        fGraphManager->GetPort(fCapturePortList[i])->SetLatencyRange(JackCaptureLatency, &range);
    }
    double play = fHavePlay ? fPlay.PublishedLatency() : 0.0;
    if (play <= 0.0) {
        play = (2.0 * period + 32.0 + 96.0) * card_to_master;
    }
    range.min = range.max = jack_nframes_t(play + n + 0.5);
    fReportedPlayback = range.min;
    for (int i = 0; i < fPlaybackChannels; i++) {
        fGraphManager->GetPort(fPlaybackPortList[i])->SetLatencyRange(JackPlaybackLatency, &range);
    }
}

int JackBridgeDriver::Close()
{
    fCard.Stop();
    Release();
    return JackDriver::CloseBridge();   // never JackDriver::Close: it would count fDriverNum down
}

int JackBridgeDriver::Start()
{
    if (!fCard.Start(fHaveCap ? &fCap : NULL, fHavePlay ? &fPlay : NULL)) {
        return -1;
    }
    return JackDriver::Start();
}

int JackBridgeDriver::Stop()
{
    JackDriver::Stop();
    fCard.Stop();   // joins the card's thread, closes the card
    return 0;
}

void JackBridgeDriver::BridgePlayback(int64_t t_ns, double ns_per_frame, jack_nframes_t n)
{
    if (!fHavePlay) {
        return;
    }
    for (int i = 0; i < fPlaybackChannels; i++) {
        fOut[i] = GetOutputBuffer(i);   // the mix of what is connected (the previous cycle's)
    }
    fPlay.Cycle(t_ns, int(n), ns_per_frame, (const float* const*)fOut);
}

void JackBridgeDriver::BridgeCapture(int64_t t_ns, double ns_per_frame, jack_nframes_t n)
{
    if (fHaveCap) {
        for (int i = 0; i < fCaptureChannels; i++) {
            fIn[i] = GetInputBuffer(i);
        }
        fCap.Cycle(t_ns, int(n), ns_per_frame, fIn);
    }
    // always, even with nothing (silence): the clients reading the bridge's ports wait for it
    ResumeRefNum();
}

int JackBridgeDriver::BridgeBufferSize(jack_nframes_t buffer_size)
{
    if (fHaveCap && !fCap.SetMasterPeriod(int(buffer_size))) {
        return -1;
    }
    if (fHavePlay && !fPlay.SetMasterPeriod(int(buffer_size))) {
        return -1;
    }
    UpdateLatencies();
    return 0;
}

void JackBridgeDriver::BridgeMonitor()
{
    // the ports' latencies: as placed (a frame or more off: tell clients)
    const jack_nframes_t n = fEngineControl->fBufferSize;
    const double cap = fHaveCap ? fCap.PublishedLatency() : 0.0;
    const double play = fHavePlay ? fPlay.PublishedLatency() : 0.0;
    const bool cap_moved = cap > 0.0 && jack_nframes_t(cap + 0.5) != fReportedCapture;
    const bool play_moved = play > 0.0 && jack_nframes_t(play + n + 0.5) != fReportedPlayback;
    if (cap_moved || play_moved) {
        UpdateLatencies();
        fEngine->ComputeTotalLatencies();
    }

    // The card: gone, failed, back, each said once. A lost or failed card is opened again,
    // once a second (every 5 s after ten tries that found it there but could not start it);
    // its ports and their connections stay. Tries that fail are said once, from the third (a
    // card just plugged in may not open at once), the next ones are quiet; after that, the
    // card is said back once it has run for 10 s.
    const int64_t now = GetMicroSeconds();
    char reason[160];
    int card = fCard.State();
    if (fReopening && card == kAlsaRunning) {
        if (!fReopenLogged || now - fLastReopen >= 10000000) {
            jack_info("bridge %s: the card is back", fClientControl.fName);
            fReopening = false;
            fReopenTries = 0;
            fReopenLogged = false;
        }
        fLoggedCard = card;
    } else if (fReopening && (card == kAlsaLost || card == kAlsaFailed)) {
        fReopening = false;
        if (!fReopenLogged && fReopenTries >= 3) {
            fCard.FailReason(reason, sizeof(reason));
            jack_error("bridge %s: the card is back but does not run (%s): trying again", fClientControl.fName,
                       card == kAlsaLost ? "gone again" : reason);
            fReopenLogged = true;
        }
        fLoggedCard = card;
    }
    if (card != fLoggedCard) {
        if (card == kAlsaLost) {
            jack_info("bridge %s: the card is gone (its ports stay, silent, until it is back)", fClientControl.fName);
        } else if (card == kAlsaFailed) {
            fCard.FailReason(reason, sizeof(reason));
            jack_error("bridge %s: the card failed (%s): trying again", fClientControl.fName, reason);
        }
        fLoggedCard = card;
    }
    if (!fReopening && (card == kAlsaLost || card == kAlsaFailed)) {
        if (now - fLastReopen >= (fReopenTries < 10 ? 1000000 : 5000000)) {
            fLastReopen = now;
            Reserve();      // back with another index: the reservation follows (quiet if not back)
            const int r = fCard.Reopen();
            if (r != 0) {
                fReopenTries++;
            }
            if (r > 0) {
                fReopening = true;
            } else if (r < 0 && !fReopenLogged && fReopenTries >= 3) {
                fCard.FailReason(reason, sizeof(reason));
                jack_error("bridge %s: the card is back but cannot be opened (%s): trying again", fClientControl.fName, reason);
                fReopenLogged = true;
            }
            card = fCard.State();
            fLoggedCard = card;
        }
    }
    // What went wrong, said at most once a minute (the first at once): the card's own xruns
    // (the card thread recovers them), capture underruns (the card's frames came later than the
    // delay covers: placed again, longer), playback frames the card played as silence (the
    // graph side's came late)
    const long xruns = fCard.Xruns();
    const long underruns = long(fHaveCap ? fCap.PublishedUnderruns() : 0);
    const long missing = long(fCard.PlaybackMissing());
    if ((xruns > fLoggedXruns || underruns > fLoggedUnderruns || missing > fLoggedMissing)
        && (fXrunsLoggedAt == 0 || now - fXrunsLoggedAt >= 60000000)) {
        jack_error("bridge %s: %ld card xrun%s, %ld capture underrun%s, %ld playback frames missed%s", fClientControl.fName,
                   xruns - fLoggedXruns, xruns - fLoggedXruns == 1 ? "" : "s",
                   underruns - fLoggedUnderruns, underruns - fLoggedUnderruns == 1 ? "" : "s",
                   missing - fLoggedMissing, fXrunsLoggedAt == 0 ? "" : " since the last report");
        fLoggedXruns = xruns;
        fLoggedUnderruns = underruns;
        fLoggedMissing = missing;
        fXrunsLoggedAt = now;
    }
    const int cs = fHaveCap ? fCap.PublishedState() : kBridgeLocked;
    const int ps = fHavePlay ? fPlay.PublishedState() : kBridgeLocked;
    const bool locked = cs == kBridgeLocked && ps == kBridgeLocked;
    const bool was_locked = fLoggedCapState == kBridgeLocked && fLoggedPlayState == kBridgeLocked;
    const int64_t resyncs = (fHaveCap ? fCap.PublishedResyncs() : 0) + (fHavePlay ? fPlay.PublishedResyncs() : 0);
    const double ms = 1e3 / fEngineControl->fSampleRate;
    const double cap_ms = cap * ms;
    const double play_ms = play > 0.0 ? (play + n) * ms : 0.0;
    if (locked && !was_locked) {
        jack_info("bridge %s: locked, delay %.1f ms in, %.1f ms out%s", fClientControl.fName,
                  cap_ms, play_ms, resyncs > fLoggedResyncs ? " (after a resync)" : "");
        fLoggedResyncs = resyncs;
        fLoggedCapMs = cap_ms;
        fLoggedPlayMs = play_ms;
    } else if (locked && (fabs(cap_ms - fLoggedCapMs) >= 0.5 || fabs(play_ms - fLoggedPlayMs) >= 0.5)) {
        // the delays follow what the card was measured to need (JackBridgeDirection::Tune)
        jack_info("bridge %s: delay %.1f ms in, %.1f ms out, as measured", fClientControl.fName, cap_ms, play_ms);
        fLoggedCapMs = cap_ms;
        fLoggedPlayMs = play_ms;
    }
    fLoggedCapState = cs;
    fLoggedPlayState = ps;
}

} // end of namespace


// the card's ALSA id from its device name ("hw:U44,0" -> "U44"), for the client's name; another
// device than the first gets its number ("hw:PCH,3" -> "PCH-3": two bridges on one card)
static void CardId(const char* device, char* id, size_t size)
{
    snprintf(id, size, "bridge");
    char card[64];
    if (!CardName(device, card, sizeof(card))) {
        return;
    }
    const int index = snd_card_get_index(card);
    if (index >= 0) {
        char ctl[32];
        snprintf(ctl, sizeof(ctl), "hw:%d", index);
        snd_ctl_t* handle;
        snd_ctl_card_info_t* info;
        snd_ctl_card_info_alloca(&info);
        if (snd_ctl_open(&handle, ctl, 0) == 0) {
            if (snd_ctl_card_info(handle, info) == 0) {
                snprintf(card, sizeof(card), "%s", snd_ctl_card_info_get_id(info));
            }
            snd_ctl_close(handle);
        }
    }
    // the number kept whole: the card's id is cut instead (ids are at most 15 characters anyway)
    char suffix[16] = "";
    const int dev = DeviceNumber(device);
    if (dev > 0) {
        snprintf(suffix, sizeof(suffix), "-%d", dev);
    }
    const int room = int(size) - 1 - int(strlen(suffix));
    snprintf(id, size, "%.*s%s", room > 0 ? room : 0, card, suffix);
}

#ifdef __cplusplus
extern "C"
{
#endif

SERVER_EXPORT const jack_driver_desc_t* driver_get_descriptor()
{
    jack_driver_desc_t* desc;
    jack_driver_desc_filler_t filler;
    jack_driver_param_value_t value;

    desc = jack_driver_descriptor_construct("bridge", JackDriverSlave,
        "A sound card on its own clock, joined to the master's graph (adaptive resampling)", &filler);

    strcpy(value.str, "hw:0");
    jack_driver_descriptor_add_parameter(desc, &filler, "device", 'd', JackDriverParamString, &value, NULL,
        "ALSA device", NULL);
    value.str[0] = 0;
    jack_driver_descriptor_add_parameter(desc, &filler, "name", 'c', JackDriverParamString, &value, NULL,
        "Client name (default: the card's ALSA id)", NULL);
    strcpy(value.str, "duplex");
    jack_driver_descriptor_add_parameter(desc, &filler, "mode", 'm', JackDriverParamString, &value, NULL,
        "duplex, capture or playback",
        "duplex: whichever of capture and playback the card has; capture or playback: that one only");
    value.ui = 128;
    jack_driver_descriptor_add_parameter(desc, &filler, "period", 'p', JackDriverParamUInt, &value, NULL,
        "The card's period (frames)", NULL);
    value.ui = 2;
    jack_driver_descriptor_add_parameter(desc, &filler, "nperiods", 'n', JackDriverParamUInt, &value, NULL,
        "The card's periods", NULL);
    strcpy(value.str, "auto");
    jack_driver_descriptor_add_parameter(desc, &filler, "wake", 'w', JackDriverParamString, &value, NULL,
        "Card thread wakeups: auto, timer or irq",
        "auto: a timer that asks for the position on interrupt-driven cards, a backstop on batch "
        "(USB) ones; timer: always; irq: the interrupt only (a backstop that does not ask)");
    value.ui = 1;
    jack_driver_descriptor_add_parameter(desc, &filler, "quality", 'q', JackDriverParamUInt, &value, NULL,
        "Resampling quality: 0, 1 or 2", NULL);
    return desc;
}

SERVER_EXPORT Jack::JackDriverClientInterface* driver_initialize(Jack::JackLockedEngine* engine, Jack::JackSynchro* table, const JSList* params)
{
    char device[JACK_DRIVER_PARAM_STRING_MAX + 1] = "hw:0";
    char name[JACK_CLIENT_NAME_SIZE + 1] = "";
    char mode[16] = "duplex";
    char wake[16] = "auto";
    unsigned period = 128, nperiods = 2, quality = 1;
    for (const JSList* node = params; node; node = jack_slist_next(node)) {
        const jack_driver_param_t* param = (const jack_driver_param_t*)node->data;
        switch (param->character) {
            case 'd': snprintf(device, sizeof(device), "%s", param->value.str); break;
            case 'c': snprintf(name, sizeof(name), "%.*s", int(sizeof(name) - 1), param->value.str); break;
            case 'm': snprintf(mode, sizeof(mode), "%.*s", int(sizeof(mode) - 1), param->value.str); break;
            case 'p': period = param->value.ui; break;
            case 'n': nperiods = param->value.ui; break;
            case 'w': snprintf(wake, sizeof(wake), "%.*s", int(sizeof(wake) - 1), param->value.str); break;
            case 'q': quality = param->value.ui; break;
        }
    }
    if (!name[0]) {
        CardId(device, name, sizeof(name));
    }
    Jack::JackBridgeAlsaParams card;
    // duplex: what the card has (a USB microphone has no playback, a USB DAC no capture)
    const bool duplex = strcmp(mode, "capture") && strcmp(mode, "playback");
    card.fCapture = strcmp(mode, "playback") && (!duplex || HasStream(device, SND_PCM_STREAM_CAPTURE)) ? device : NULL;
    card.fPlayback = strcmp(mode, "capture") && (!duplex || HasStream(device, SND_PCM_STREAM_PLAYBACK)) ? device : NULL;
    if (!card.fCapture && !card.fPlayback) {
        jack_error("bridge: %s has neither capture nor playback", device);
        return NULL;
    }
    card.fPeriod = int(period);
    card.fPeriods = int(nperiods);
    card.fWake = !strcmp(wake, "irq") ? Jack::kWakeIrq : !strcmp(wake, "timer") ? Jack::kWakeTimer : Jack::kWakeAuto;
    Jack::JackBridgeDriver* driver = new Jack::JackBridgeDriver(name, engine, table);
    if (driver->OpenBridgeCard(card, int(quality)) == 0) {
        return driver;
    }
    delete driver;
    return NULL;
}

#ifdef __cplusplus
}
#endif
