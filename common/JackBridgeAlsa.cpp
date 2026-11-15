/*
 JackBus: the card side of the sound card bridge for ALSA (see JackBridgeAlsa.h).
*/

#include "JackBridgeAlsa.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace Jack
{

static const snd_pcm_format_t kFormats[] = {
    SND_PCM_FORMAT_FLOAT_LE, SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S24_3LE,
    SND_PCM_FORMAT_S24_LE, SND_PCM_FORMAT_S16_LE
};

static void ToFloat(const void* src, snd_pcm_format_t format, float* dst, int samples)
{
    switch (format) {
        case SND_PCM_FORMAT_FLOAT_LE:
            memcpy(dst, src, samples * sizeof(float));
            break;
        case SND_PCM_FORMAT_S32_LE: {
            const int32_t* s = (const int32_t*)src;
            for (int i = 0; i < samples; i++) dst[i] = float(s[i]) * (1.0f / 2147483648.0f);
            break;
        }
        case SND_PCM_FORMAT_S24_3LE: {
            const uint8_t* s = (const uint8_t*)src;
            for (int i = 0; i < samples; i++, s += 3) {
                const int32_t v = int32_t(uint32_t(s[0]) << 8 | uint32_t(s[1]) << 16 | uint32_t(s[2]) << 24) >> 8;
                dst[i] = float(v) * (1.0f / 8388608.0f);
            }
            break;
        }
        case SND_PCM_FORMAT_S24_LE: {
            const int32_t* s = (const int32_t*)src;
            for (int i = 0; i < samples; i++) dst[i] = float(int32_t(uint32_t(s[i]) << 8) >> 8) * (1.0f / 8388608.0f);
            break;
        }
        case SND_PCM_FORMAT_S16_LE: {
            const int16_t* s = (const int16_t*)src;
            for (int i = 0; i < samples; i++) dst[i] = float(s[i]) * (1.0f / 32768.0f);
            break;
        }
        default:
            memset(dst, 0, samples * sizeof(float));
    }
}

static inline float Clip(float x)
{
    return x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
}

static void FromFloat(const float* src, snd_pcm_format_t format, void* dst, int samples)
{
    switch (format) {
        case SND_PCM_FORMAT_FLOAT_LE:
            memcpy(dst, src, samples * sizeof(float));
            break;
        case SND_PCM_FORMAT_S32_LE: {
            int32_t* d = (int32_t*)dst;
            for (int i = 0; i < samples; i++) {
                const double v = double(Clip(src[i])) * 2147483647.0;
                d[i] = int32_t(lrint(v));
            }
            break;
        }
        case SND_PCM_FORMAT_S24_3LE: {
            uint8_t* d = (uint8_t*)dst;
            for (int i = 0; i < samples; i++, d += 3) {
                const int32_t v = int32_t(lrintf(Clip(src[i]) * 8388607.0f));
                d[0] = uint8_t(v);
                d[1] = uint8_t(v >> 8);
                d[2] = uint8_t(v >> 16);
            }
            break;
        }
        case SND_PCM_FORMAT_S24_LE: {
            int32_t* d = (int32_t*)dst;
            for (int i = 0; i < samples; i++) d[i] = int32_t(lrintf(Clip(src[i]) * 8388607.0f));
            break;
        }
        case SND_PCM_FORMAT_S16_LE: {
            int16_t* d = (int16_t*)dst;
            for (int i = 0; i < samples; i++) d[i] = int16_t(lrintf(Clip(src[i]) * 32767.0f));
            break;
        }
        default:
            memset(dst, 0, samples * (snd_pcm_format_physical_width(format) / 8));
    }
}

static int64_t TimespecNs(const snd_htimestamp_t& ts)
{
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

static int64_t NowNs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// the card's timestamps' clock (the clock models run on it)
static int64_t NowRawNs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

JackBridgeAlsa::JackBridgeAlsa()
    : fCap(NULL), fPlay(NULL), fCapFormat(SND_PCM_FORMAT_UNKNOWN), fPlayFormat(SND_PCM_FORMAT_UNKNOWN),
      fRate(0), fPeriod(0), fBufferSize(0), fCapChannels(0), fPlayChannels(0), fBatch(false),
      fLinked(false), fHaveCap(false), fHavePlay(false), fStartedCap(NULL), fStartedPlay(NULL), fStartedRate(0), fStartedCapChannels(0), fStartedPlayChannels(0), fCapRead(0), fPlayWritten(0), fPlayOffset(0), fLastRead(0), fLastFill(0), fLastQueued(0), fTimerWakes(0), fConv(NULL),
      fFailWhat(NULL), fFailErr(0), fThreadRunning(false), fRun(false), fState(kAlsaClosed), fXruns(0)
{
    fCaptureName[0] = 0;
    fPlaybackName[0] = 0;
}

JackBridgeAlsa::~JackBridgeAlsa()
{
    Stop();
}

bool JackBridgeAlsa::Configure(snd_pcm_t* pcm, bool capture, int* channels, snd_pcm_format_t* format)
{
    snd_pcm_hw_params_t* hw;
    snd_pcm_sw_params_t* sw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_sw_params_alloca(&sw);
    const char* dir = capture ? "capture" : "playback";

    if (snd_pcm_hw_params_any(pcm, hw) < 0
        || snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_MMAP_INTERLEAVED) < 0) {
        fprintf(stderr, "bridge: %s: no interleaved mmap access\n", dir);
        return false;
    }
    snd_pcm_hw_params_set_rate_resample(pcm, hw, 0);   // the bridge resamples, ALSA must not
    *format = SND_PCM_FORMAT_UNKNOWN;
    for (unsigned i = 0; i < sizeof(kFormats) / sizeof(kFormats[0]); i++) {
        if (snd_pcm_hw_params_test_format(pcm, hw, kFormats[i]) == 0) {
            *format = kFormats[i];
            break;
        }
    }
    if (*format == SND_PCM_FORMAT_UNKNOWN || snd_pcm_hw_params_set_format(pcm, hw, *format) < 0) {
        fprintf(stderr, "bridge: %s: no usable sample format\n", dir);
        return false;
    }
    unsigned ch = *channels;
    if (ch == 0) {
        snd_pcm_hw_params_get_channels_max(hw, &ch);
        if (ch > 64) {
            ch = 64;
        }
    }
    if (snd_pcm_hw_params_set_channels_near(pcm, hw, &ch) < 0) {
        return false;
    }
    unsigned rate = fParams.fRate;
    if (snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL) < 0) {
        return false;
    }
    snd_pcm_uframes_t period = fParams.fPeriod;
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL);
    // A batch card (USB) needs a third period:
    // - playback with no capture to wake the card side (capture-only cards wake it at each
    //   transfer, duplex ones through the linked capture): the card side waits on playback space
    //   alone, and the card takes its frames into transfers well ahead of playing them; it also
    //   wakes for a quarter of a period of space (below), not half the buffer: else the
    //   transfers drain the buffer before the next wakeup;
    // - periods of 256 frames or more: snd-usb's transfers then come in chunks larger than a
    //   period (a U-44 at 256: 480 and 288 frames, every 16 ms), and two periods (512 frames)
    //   overran as soon as a chunk was read a millisecond late
    const bool alone = !capture && (fCap == NULL || !fParams.fLink);
    unsigned periods = fParams.fPeriods;
    if (snd_pcm_hw_params_is_batch(hw) && periods < 3 && (alone || period >= 256)) {
        periods = 3;
    }
    snd_pcm_hw_params_set_periods_near(pcm, hw, &periods, NULL);
    if (snd_pcm_hw_params(pcm, hw) < 0) {
        fprintf(stderr, "bridge: %s: hardware parameters refused\n", dir);
        return false;
    }
    snd_pcm_uframes_t buffer;
    snd_pcm_hw_params_get_buffer_size(hw, &buffer);
    snd_pcm_hw_params_get_period_size(hw, &period, NULL);
    if (fRate == 0) {
        fRate = rate;
        fPeriod = int(period);
        fBufferSize = int(buffer);
    } else if (rate != fRate) {
        fprintf(stderr, "bridge: capture and playback rates differ (%u, %u)\n", fRate, rate);
        return false;
    }
    if (!capture) {
        fBufferSize = int(buffer);
    }
    fBatch = fBatch || snd_pcm_hw_params_is_batch(hw);
    *channels = int(ch);

    snd_pcm_uframes_t boundary;
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_sw_params_get_boundary(sw, &boundary);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, boundary);    // started by hand
    snd_pcm_sw_params_set_stop_threshold(pcm, sw, buffer);       // an xrun stops it
    snd_pcm_uframes_t wake = period;
    if (alone) {
        wake = period / 4 > 16 ? period / 4 : 16;
    }
    snd_pcm_sw_params_set_avail_min(pcm, sw, wake);
    snd_pcm_sw_params_set_tstamp_mode(pcm, sw, SND_PCM_TSTAMP_ENABLE);
    // CLOCK_MONOTONIC_RAW: the domain the master gives its cycle times in (JackBridgeSet.h);
    // MONOTONIC would add NTP's slewing between the card's clock and the master's
    snd_pcm_sw_params_set_tstamp_type(pcm, sw, SND_PCM_TSTAMP_TYPE_MONOTONIC_RAW);
    if (snd_pcm_sw_params(pcm, sw) < 0) {
        fprintf(stderr, "bridge: %s: software parameters refused\n", dir);
        return false;
    }
    return true;
}

bool JackBridgeAlsa::Open(const JackBridgeAlsaParams& params)
{
    fParams = params;
    // Reopen passes our own names back: copy only from elsewhere
    if (params.fCapture && params.fCapture != fCaptureName) {
        snprintf(fCaptureName, sizeof(fCaptureName), "%s", params.fCapture);
    }
    if (params.fPlayback && params.fPlayback != fPlaybackName) {
        snprintf(fPlaybackName, sizeof(fPlaybackName), "%s", params.fPlayback);
    }
    fParams.fCapture = params.fCapture ? fCaptureName : NULL;
    fParams.fPlayback = params.fPlayback ? fPlaybackName : NULL;
    fCapChannels = params.fCaptureChannels;
    fPlayChannels = params.fPlaybackChannels;
    if (fParams.fCapture) {
        const int err = snd_pcm_open(&fCap, fParams.fCapture, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
        if (err < 0) {
            fCap = NULL;
            Failed("opening capture", err);
            Stop();
            return false;
        }
        if (!Configure(fCap, true, &fCapChannels, &fCapFormat)) {
            Failed("configuring capture", -EINVAL);
            Stop();
            return false;
        }
    }
    if (fParams.fPlayback) {
        const int err = snd_pcm_open(&fPlay, fParams.fPlayback, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
        if (err < 0) {
            fPlay = NULL;
            Failed("opening playback", err);
            Stop();
            return false;
        }
        if (!Configure(fPlay, false, &fPlayChannels, &fPlayFormat)) {
            Failed("configuring playback", -EINVAL);
            Stop();
            return false;
        }
    }
    if (fCap && fPlay && params.fLink) {
        fLinked = (snd_pcm_link(fCap, fPlay) == 0);   // one card: start and stop together
    }
    const int ch = (fCapChannels > fPlayChannels) ? fCapChannels : fPlayChannels;
    fConv = (float*)calloc(size_t(fBufferSize > fPeriod * 8 ? fBufferSize : fPeriod * 8) * ch, sizeof(float));
    fState.store(kAlsaClosed, std::memory_order_release);
    return fConv != NULL;
}

// whether the card a PCM name points at is there ("hw:NAME,0", "plughw:CARD=NAME,DEV=0"):
// snd_card_get_index is quiet; opening a missing card makes alsa-lib print an error each time
static bool CardPresent(const char* device)
{
    if (!device) {
        return true;
    }
    const char* p = strchr(device, ':');
    if (!p) {
        return true;    // not a card name ("default"...): let the open decide
    }
    char card[64];
    snprintf(card, sizeof(card), "%s", p + 1);
    char* comma = strchr(card, ',');
    if (comma) {
        *comma = 0;
    }
    if (strncmp(card, "CARD=", 5) == 0) {
        memmove(card, card + 5, strlen(card + 5) + 1);
    }
    return snd_card_get_index(card) >= 0;
}

bool JackBridgeAlsa::Failed(const char* what, int err)
{
    fFailWhat = what;
    fFailErr = err;
    return false;
}

void JackBridgeAlsa::FailReason(char* buf, size_t size) const
{
    if (fFailWhat) {
        snprintf(buf, size, "%s: %s", fFailWhat, snd_strerror(fFailErr));
    } else {
        snprintf(buf, size, "no reason given");
    }
}

int JackBridgeAlsa::Reopen()
{
    if (!CardPresent(fParams.fCapture) || !CardPresent(fParams.fPlayback)) {
        return 0;   // not back yet: nothing to try, nothing to log
    }
    JackBridgeAlsaParams p = fParams;
    const unsigned rate = fStartedRate;
    const int cch = fStartedCapChannels, pch = fStartedPlayChannels;
    Stop();                 // the card thread has ended; closes what is left
    p.fCaptureChannels = cch;
    p.fPlaybackChannels = pch;
    if (!Open(p)) {
        fState.store(kAlsaLost, std::memory_order_release);
        return -1;
    }
    if (fRate != rate || fCapChannels != cch || fPlayChannels != pch) {
        Failed("the card came back different (rate or channels)", -EINVAL);
        Stop();
        fState.store(kAlsaLost, std::memory_order_release);
        return -1;
    }
    if (!Start(fStartedCap, fStartedPlay)) {
        Failed("starting the card thread", -EAGAIN);
        Stop();
        fState.store(kAlsaLost, std::memory_order_release);
        return -1;
    }
    return 1;
}

void JackBridgeAlsa::SetLost(JackBridgeCapture* capture, JackBridgePlayback* playback, unsigned rate)
{
    fStartedCap = capture;
    fStartedPlay = playback;
    fStartedRate = rate;
    fState.store(kAlsaLost, std::memory_order_release);
}

bool JackBridgeAlsa::Start(JackBridgeCapture* capture, JackBridgePlayback* playback)
{
    fStartedCap = capture;
    fStartedPlay = playback;
    fStartedRate = fRate;
    fStartedCapChannels = fCapChannels;
    fStartedPlayChannels = fPlayChannels;
    fHaveCap = (capture && fCap);
    fHavePlay = (playback && fPlay);
    if (fHaveCap) {
        fCardCap.Attach(capture, fRate, 0.0);
    }
    if (fHavePlay) {
        fCardPlay.Attach(playback, fRate, 0.0);
    }
    fRun.store(true, std::memory_order_release);

    // SCHED_FIFO at fPriority; a server that is not realtime (priority 0) gets a plain thread
    int err = EPERM;
    if (fParams.fPriority > 0) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        struct sched_param sp;
        memset(&sp, 0, sizeof(sp));
        sp.sched_priority = fParams.fPriority;
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        pthread_attr_setschedparam(&attr, &sp);
        err = pthread_create(&fThread, &attr, ThreadEntry, this);
        pthread_attr_destroy(&attr);
        if (err == EPERM || err == EINVAL) {
            fprintf(stderr, "bridge: no realtime scheduling for the card thread, running without it\n");
        }
    }
    if (err == EPERM || err == EINVAL) {
        err = pthread_create(&fThread, NULL, ThreadEntry, this);
    }
    if (err != 0) {
        fRun.store(false);
        return false;
    }
    fThreadRunning = true;
    return true;
}

void JackBridgeAlsa::Stop()
{
    fRun.store(false, std::memory_order_release);
    if (fThreadRunning) {
        pthread_join(fThread, NULL);
        fThreadRunning = false;
    }
    if (fCap) {
        if (fLinked) {
            snd_pcm_unlink(fCap);
        }
        snd_pcm_close(fCap);
        fCap = NULL;
    }
    fLinked = false;
    if (fPlay) {
        snd_pcm_close(fPlay);
        fPlay = NULL;
    }
    free(fConv);
    fConv = NULL;
    fRate = 0;
}

void* JackBridgeAlsa::ThreadEntry(void* arg)
{
    ((JackBridgeAlsa*)arg)->Run();
    return NULL;
}

bool JackBridgeAlsa::StartStreams()
{
    // Playback starts with its whole buffer of silence queued (as JACK's ALSA driver starts);
    // the graph side places its frames later than that (its target delay). One period was not
    // enough: a USB card whose buffer is larger than its driver's transfer queue plays in
    // snd-usb's low-latency mode, which fills its transfers from the buffer at once when the
    // stream starts. With one period there, it ran dry at the start: an xrun, a restart with
    // one period again, another xrun (a U-44 at 256 x 2 never locked; 128 x 2, a buffer within
    // the queue, played in the classic mode and did).
    // A new epoch: what was last read or filled says nothing about when the next period is due
    fLastRead = 0;
    fLastFill = 0;
    fLastQueued = 0;
    int err;
    if (fPlay) {
        if ((err = snd_pcm_prepare(fPlay)) < 0) {
            return Failed("preparing playback", err);
        }
        // nothing is queued after a prepare: the playback position is fPlayWritten, the capture
        // one fCapRead, and they start together
        fPlayOffset = fPlayWritten - fCapRead;
        const int ch = fPlayChannels;
        memset(fConv, 0, size_t(fBufferSize) * ch * sizeof(float));
        int left = fBufferSize;
        while (left > 0) {
            const snd_pcm_channel_area_t* areas;
            snd_pcm_uframes_t offset, frames = left;
            if ((err = snd_pcm_mmap_begin(fPlay, &areas, &offset, &frames)) < 0) {
                return Failed("queueing playback", err);
            }
            char* dst = (char*)areas[0].addr + offset * (areas[0].step / 8);
            FromFloat(fConv, fPlayFormat, dst, int(frames) * ch);
            snd_pcm_mmap_commit(fPlay, offset, frames);
            left -= int(frames);
        }
        fPlayWritten += fBufferSize;
    }
    if (fCap && !fLinked && (err = snd_pcm_prepare(fCap)) < 0) {
        return Failed("preparing capture", err);
    }
    if (fHaveCap) {
        fCardCap.Start(fCapRead);
    }
    if (fHavePlay) {
        fCardPlay.Start();
    }
    if (fCap && (err = snd_pcm_start(fCap)) < 0) {
        return Failed("starting capture", err);
    }
    if (fPlay && !(fCap && fLinked) && (err = snd_pcm_start(fPlay)) < 0) {
        return Failed("starting playback", err);
    }
    return true;
}

int JackBridgeAlsa::Recover(snd_pcm_t* pcm, int err, bool capture)
{
    (void)pcm;
    (void)capture;
    if (err == -ENODEV || err == -EBADFD || err == -ENOTTY) {
        fState.store(kAlsaLost, std::memory_order_release);
        return -1;
    }
    if (err == -EPIPE || err == -ESTRPIPE) {
        fXruns.fetch_add(1, std::memory_order_relaxed);
        if (fCap) {
            snd_pcm_drop(fCap);
        }
        if (fPlay && !fLinked) {
            snd_pcm_drop(fPlay);
        }
        int e;
        if (fCap && (e = snd_pcm_prepare(fCap)) < 0) {
            Failed("preparing capture after an xrun", e);
            fState.store(kAlsaFailed, std::memory_order_release);
            return -1;
        }
        // a new epoch from here: positions keep counting, the graph side places again
        if (!StartStreams()) {
            fState.store(kAlsaFailed, std::memory_order_release);
            return -1;
        }
        return 0;
    }
    return 0;
}

bool JackBridgeAlsa::ReadCapture()
{
    snd_pcm_uframes_t avail;
    snd_htimestamp_t ts;
    int err = snd_pcm_htimestamp(fCap, &avail, &ts);
    if (err < 0) {
        return Recover(fCap, err, true) == 0;
    }
    const int64_t pos = fCapRead + int64_t(avail);
    fCardCap.Position(pos, TimespecNs(ts));
    if (fHavePlay && fLinked) {
        fCardPlay.Position(pos + fPlayOffset, TimespecNs(ts));
    }
    if (fHaveCap && avail > 0) {
        fCardCap.Arriving(NowRawNs());     // the lateness's peak, just before these frames are in
    }
    const int ch = fCapChannels;
    while (avail > 0) {
        const snd_pcm_channel_area_t* areas;
        snd_pcm_uframes_t offset, frames = avail;
        if ((err = snd_pcm_mmap_begin(fCap, &areas, &offset, &frames)) < 0) {
            return Recover(fCap, err, true) == 0;
        }
        const char* src = (const char*)areas[0].addr + offset * (areas[0].step / 8);
        ToFloat(src, fCapFormat, fConv, int(frames) * ch);
        if (fHaveCap) {
            fCardCap.Write(fConv, int(frames));
        }
        const snd_pcm_sframes_t done = snd_pcm_mmap_commit(fCap, offset, frames);
        if (done < 0 || snd_pcm_uframes_t(done) != frames) {
            return Recover(fCap, done < 0 ? int(done) : -EPIPE, true) == 0;
        }
        fCapRead += int64_t(frames);
        avail -= frames;
    }
    fLastRead = NowNs();
    return true;
}

bool JackBridgeAlsa::WritePlayback(bool force)
{
    snd_pcm_uframes_t avail;
    snd_htimestamp_t ts;
    int err = snd_pcm_htimestamp(fPlay, &avail, &ts);
    if (err < 0) {
        return Recover(fPlay, err, false) == 0;
    }
    const int queued = fBufferSize - int(avail);
    if (fHavePlay && !fLinked) {
        fCardPlay.Position(fPlayWritten - queued, TimespecNs(ts));   // no capture to go by
    }
    // Keep the card's buffer full: the graph side places its frames further ahead than the
    // buffer reaches (its target delay), so normally they are all there; silence fills only
    // positions nothing was placed for (the start, or a graph side that stalled). A buffer
    // kept only partly full runs dry when a card consumes in bursts (a timer-driven or USB one).
    (void)force;
    int must = int(avail);
    const int ch = fPlayChannels;
    while (avail > 0) {
        const snd_pcm_channel_area_t* areas;
        snd_pcm_uframes_t offset, frames = avail;
        if ((err = snd_pcm_mmap_begin(fPlay, &areas, &offset, &frames)) < 0) {
            return Recover(fPlay, err, false) == 0;
        }
        int got = int(frames);
        if (fHavePlay) {
            got = fCardPlay.Fill(fConv, int(frames), fPlayWritten, must);
            must -= got;
            if (must < 0) {
                must = 0;
            }
        } else {
            memset(fConv, 0, frames * ch * sizeof(float));
        }
        if (got <= 0) {
            snd_pcm_mmap_commit(fPlay, offset, 0);
            break;
        }
        char* dst = (char*)areas[0].addr + offset * (areas[0].step / 8);
        FromFloat(fConv, fPlayFormat, dst, got * ch);
        const snd_pcm_sframes_t done = snd_pcm_mmap_commit(fPlay, offset, got);
        if (done < 0 || done != got) {
            return Recover(fPlay, done < 0 ? int(done) : -EPIPE, false) == 0;
        }
        fPlayWritten += got;
        avail -= got;
        if (got < int(frames)) {
            break;
        }
    }
    fLastFill = NowNs();
    fLastQueued = fBufferSize - int(avail);
    if (fHavePlay) {
        fCardPlay.Filled(fPlayWritten, NowRawNs());   // how far ahead the card needed frames
    }
    return true;
}

void JackBridgeAlsa::Run()
{
    if (!StartStreams()) {
        fState.store(kAlsaFailed, std::memory_order_release);
        return;
    }
    fState.store(kAlsaRunning, std::memory_order_release);
    struct pollfd pfd[16];
    while (fRun.load(std::memory_order_acquire)) {
        int n = 0, ncap = 0, nplay = 0;
        if (fCap) {
            ncap = snd_pcm_poll_descriptors(fCap, pfd, 8);
            n += ncap;
        }
        if (fPlay) {
            nplay = snd_pcm_poll_descriptors(fPlay, pfd + n, 8);
            n += nplay;
        }
        // Wait for the card's notification, but only until a period is due by the frames last
        // handled (plus a little: the interrupt normally comes first). If none came, look at the
        // card's position ourselves: a late or dropped interrupt is not a stall.
        const int64_t now = NowNs();
        const double ns_per_frame = 1e9 / double(fRate);
        int64_t due = now + 100000000;
        if (fPlay && fLastFill) {
            const int64_t d = fLastFill + int64_t((fLastQueued - fPeriod) * ns_per_frame);
            if (d < due) {
                due = d;
            }
        }
        if (fCap && fLastRead) {
            const int64_t d = fLastRead + int64_t(fPeriod * ns_per_frame);
            if (d < due) {
                due = d;
            }
        }
        const int64_t margin = (fParams.fWakeMargin > 0 ? fParams.fWakeMargin : 200) * 1000LL;
        int64_t wait = due - now + margin;
        if (wait < margin) {
            wait = margin;
        }
        // A batch card's position moves only when a transfer completes, which wakes the
        // thread anyway, and asking for the position would pair the old one with a later time;
        // in kWakeIrq mode the interrupt is trusted. Either way the timer is only a backstop.
        const bool sync = !fBatch && fParams.fWake != kWakeIrq;
        if (!sync) {
            wait = due - now + int64_t(2 * fPeriod * ns_per_frame);
            if (wait < margin) {
                wait = margin;   // late (a stall, or a restart): never a negative timeout
            }
        }
        struct timespec tw = { time_t(wait / 1000000000LL), long(wait % 1000000000LL) };
        const int r = ppoll(pfd, n, &tw, NULL);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            Failed("waiting for the card", -errno);
            fState.store(kAlsaFailed, std::memory_order_release);
            return;
        }
        if (r == 0) {
            fTimerWakes++;
            if (fCap) {
                if (sync) {
                    snd_pcm_hwsync(fCap);
                }
                if (!ReadCapture()) {
                    return;
                }
            }
            if (fPlay) {
                if (sync) {
                    snd_pcm_hwsync(fPlay);
                }
                if (!WritePlayback(false)) {
                    return;
                }
            }
            continue;
        }
        unsigned short rev = 0;
        if (fCap) {
            snd_pcm_poll_descriptors_revents(fCap, pfd, ncap, &rev);
            if (rev & (POLLERR | POLLNVAL)) {
                if (Recover(fCap, (rev & POLLNVAL) ? -ENODEV : -EPIPE, true) < 0) {
                    return;
                }
                continue;
            }
            if ((rev & POLLIN) && !ReadCapture()) {
                return;
            }
        }
        if (fPlay) {
            rev = 0;
            snd_pcm_poll_descriptors_revents(fPlay, pfd + ncap, nplay, &rev);
            if (rev & (POLLERR | POLLNVAL)) {
                if (Recover(fPlay, (rev & POLLNVAL) ? -ENODEV : -EPIPE, false) < 0) {
                    return;
                }
                continue;
            }
            // also when capture woke us: the graph side may have placed frames meanwhile
            if (!WritePlayback(false)) {
                return;
            }
        }
        if (fState.load(std::memory_order_relaxed) != kAlsaRunning) {
            return;
        }
    }
}

} // end of namespace
