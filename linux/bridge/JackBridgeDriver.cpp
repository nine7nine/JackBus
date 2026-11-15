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

#include <alsa/asoundlib.h>
#include <stdio.h>
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

namespace Jack
{

JackBridgeDriver::JackBridgeDriver(const char* name, JackLockedEngine* engine, JackSynchro* table)
    : JackAudioDriver(name, "alsa_pcm", engine, table), fHaveCap(false), fHavePlay(false),
      fReportedCapture(0), fReportedPlayback(0), fLoggedCapState(-1), fLoggedPlayState(-1), fLoggedCard(-1),
      fLoggedResyncs(0), fLastReopen(0), fReopenTries(0), fReopening(false), fReopenLogged(false),
      fReservedCard(-1), fReserveFailed(-1)
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
        JackDriver::CloseBridge();
        return -1;
    }
    fWithMonitorPorts = false;
    memset(fCapturePortList, 0, sizeof(fCapturePortList));
    memset(fPlaybackPortList, 0, sizeof(fPlaybackPortList));
    memset(fMonitorPortList, 0, sizeof(fMonitorPortList));

    JackBridgeParams p;
    p.fMasterRate = fEngineControl->fSampleRate;
    p.fCardRate = fCard.Rate();
    p.fMasterPeriod = fEngineControl->fBufferSize;
    p.fMaxMasterPeriod = 4096 > p.fMasterPeriod ? 4096 : p.fMasterPeriod;
    p.fCardPeriod = fCard.Period();
    p.fCardBuffer = fCard.Period() * card.fPeriods;
    p.fQuality = quality;
    p.fMargin = 96;
    if (fCaptureChannels) {
        p.fChannels = fCaptureChannels;
        fHaveCap = fCap.Init(p);
    }
    if (fPlaybackChannels) {
        p.fChannels = fPlaybackChannels;
        fHavePlay = fPlay.Init(p);
    }
    if ((fCaptureChannels && !fHaveCap) || (fPlaybackChannels && !fHavePlay)) {
        jack_error("bridge %s: cannot set up its resampling", fClientControl.fName);
        fCard.Stop();
        JackDriver::CloseBridge();
        return -1;
    }
    jack_info("bridge %s: %s, %u Hz, period %d x %d, %d in / %d out%s", fClientControl.fName, fDevice, fCard.Rate(),
              fCard.Period(), card.fPeriods, fCaptureChannels, fPlaybackChannels, fCard.Batch() ? ", batch" : "");
    return 0;
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
    const double card_to_master = double(fEngineControl->fSampleRate) / double(fCard.Rate());
    const int period = fCard.Period();
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
    const int cs = fHaveCap ? fCap.PublishedState() : kBridgeLocked;
    const int ps = fHavePlay ? fPlay.PublishedState() : kBridgeLocked;
    const bool locked = cs == kBridgeLocked && ps == kBridgeLocked;
    const bool was_locked = fLoggedCapState == kBridgeLocked && fLoggedPlayState == kBridgeLocked;
    const int64_t resyncs = (fHaveCap ? fCap.PublishedResyncs() : 0) + (fHavePlay ? fPlay.PublishedResyncs() : 0);
    if (locked && !was_locked) {
        const double ms = 1e3 / fEngineControl->fSampleRate;
        jack_info("bridge %s: locked, delay %.1f ms in, %.1f ms out%s", fClientControl.fName,
                  cap * ms, play > 0.0 ? (play + n) * ms : 0.0,
                  resyncs > fLoggedResyncs ? " (after a resync)" : "");
        fLoggedResyncs = resyncs;
    }
    fLoggedCapState = cs;
    fLoggedPlayState = ps;
}

} // end of namespace

// the card's ALSA id from its device name ("hw:U44,0" -> "U44"), for the client's name
static void CardId(const char* device, char* id, size_t size)
{
    snprintf(id, size, "bridge");
    char card[64];
    if (!CardName(device, card, sizeof(card))) {
        return;
    }
    const int index = snd_card_get_index(card);
    if (index < 0) {
        snprintf(id, size, "%s", card);
        return;
    }
    char ctl[32];
    snprintf(ctl, sizeof(ctl), "hw:%d", index);
    snd_ctl_t* handle;
    snd_ctl_card_info_t* info;
    snd_ctl_card_info_alloca(&info);
    if (snd_ctl_open(&handle, ctl, 0) == 0) {
        if (snd_ctl_card_info(handle, info) == 0) {
            snprintf(id, size, "%s", snd_ctl_card_info_get_id(info));
        }
        snd_ctl_close(handle);
    }
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
        "duplex, capture or playback", NULL);
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
            case 'c': snprintf(name, sizeof(name), "%s", param->value.str); break;
            case 'm': snprintf(mode, sizeof(mode), "%s", param->value.str); break;
            case 'p': period = param->value.ui; break;
            case 'n': nperiods = param->value.ui; break;
            case 'w': snprintf(wake, sizeof(wake), "%s", param->value.str); break;
            case 'q': quality = param->value.ui; break;
        }
    }
    if (!name[0]) {
        CardId(device, name, sizeof(name));
    }
    Jack::JackBridgeAlsaParams card;
    card.fCapture = strcmp(mode, "playback") ? device : NULL;
    card.fPlayback = strcmp(mode, "capture") ? device : NULL;
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
