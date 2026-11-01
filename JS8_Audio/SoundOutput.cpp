/**
 * @file SoundOutput.cpp
 * @brief miniaudio-based playback (Build 141, replaces QAudioSink)
 */
#include "SoundOutput.h"

#include "JS8_Audio/AudioDeviceReaper.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QIODevice>
#include <QLoggingCategory>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include "moc_SoundOutput.cpp"

Q_DECLARE_LOGGING_CATEGORY(soundout_js8)

namespace {

// Resolve an AudioDeviceInfo to a concrete ma_device_id by enumerating
// playback devices. Same shape as SoundInput's resolver.
bool
resolvePlaybackDevice(AudioDeviceInfo const & info,
                      ma_device_id & outId)
{
    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) return false;

    ma_device_info * playInfos = nullptr;
    ma_uint32        playCount = 0;
    ma_device_info * capInfos  = nullptr;
    ma_uint32        capCount  = 0;
    bool             matched   = false;

    if (ma_context_get_devices(&ctx, &playInfos, &playCount, &capInfos, &capCount) == MA_SUCCESS) {
        QByteArray const wantedId   = info.id;
        QString    const wantedName = info.description;

        for (ma_uint32 i = 0; i < playCount; ++i) {
            QByteArray const thisId
                = QByteArray(reinterpret_cast<char const *>(&playInfos[i].id),
                             sizeof(ma_device_id)).toHex();
            QString const thisName = QString::fromUtf8(playInfos[i].name);

            bool const idMatch   = !wantedId.isEmpty() && wantedId == thisId;
            bool const nameMatch = !wantedName.isEmpty() && wantedName == thisName;

            if (idMatch || nameMatch) {
                outId = playInfos[i].id;
                matched = true;
                break;
            }
        }
    }

    ma_context_uninit(&ctx);
    return matched;
}

} // namespace

/**
 * @brief miniaudio playback callback. Runs on miniaudio's worker
 * thread, NOT on the GUI or audio QThread. Pulls from the bound
 * QIODevice source, applies attenuation, writes to the output.
 */
void
SoundOutput::s_dataCallback(ma_device * device,
                            void * pOutput,
                            void const * /* pInput */,
                            ma_uint32 frameCount)
{
    if (auto * self = static_cast<SoundOutput *>(device->pUserData)) {
        self->onPlayback(pOutput, frameCount);
    }
}

void
SoundOutput::onPlayback(void * pOutput, ma_uint32 frameCount)
{
    // [reapguard] Mark ourselves in flight for teardown()'s drain — see
    // m_callbackDepth in the header. This function has no early returns,
    // so the matching release is at the end.
    m_callbackDepth.fetch_add(1, std::memory_order_acquire);

    // [AUDIO-CB-PROBE 2026-06-16 build 288] Measure inter-callback
    // interval to detect audio-thread preemption. miniaudio doesn't
    // expose whether the OS actually granted the realtime priority we
    // asked for in the context config — but we can measure directly.
    // Expected: callback fires every ~periodSizeInMs (~10-50 ms).
    // Anomaly: >2× expected period = audio thread was preempted,
    // sound card likely underran, OS audio layer replayed last DMA
    // chunk to cover the gap (the "sputter" signature).
    // Log only when an interval is anomalous so we don't drown the
    // log in routine callbacks. Static state is single-threaded by
    // miniaudio convention (one playback thread per device).
    static qint64 lastCallbackMs = 0;
    qint64 const nowMs = QDateTime::currentMSecsSinceEpoch();
    if (lastCallbackMs > 0) {
        qint64 const dt = nowMs - lastCallbackMs;
        // [BUILD 291] Threshold 150 ms — actual callback period on
        // this device is 70-82 ms (one buffer period), so any value
        // up to ~100 ms is healthy normal. 150 ms catches genuine
        // preemption (callback missed at least one buffer cycle)
        // without spamming the log on every normal callback.
        // The build 288/290 thresholds (100 / 30 ms) were set based
        // on assumed callback periods that didn't match reality.
        if (dt >= 150) {
            // [#174] RECORD ONLY -- no logging on this thread. See the
            // header: two atomic updates, no allocation, no I/O.
            // reportLateCallbacks() drains these from the GUI thread.
            m_lateCallbacks.fetch_add(1, std::memory_order_relaxed);
            qint64 worst = m_worstLateMs.load(std::memory_order_relaxed);
            while (dt > worst &&
                   !m_worstLateMs.compare_exchange_weak(
                       worst, dt, std::memory_order_relaxed)) {
            }
        }
    }
    lastCallbackMs = nowMs;

    qint64 const totalBytes = m_format.bytesForFrames(frameCount);
    char *       out        = static_cast<char *>(pOutput);

    qint64 written = 0;
    if (m_source) {
        // QIODevice::read may return less than requested when the source
        // is idle / between frames; we pad the rest with silence.
        written = m_source->read(out, totalBytes);
        if (written < 0) written = 0;
    }
    if (written < totalBytes) {
        std::memset(out + written, 0, static_cast<size_t>(totalBytes - written));
    }

    qreal const volume = m_volume;
    if (volume != 1.0) {
        qint16 * samples = reinterpret_cast<qint16 *>(out);
        size_t const n   = static_cast<size_t>(totalBytes / sizeof(qint16));
        for (size_t i = 0; i < n; ++i) {
            samples[i] = static_cast<qint16>(samples[i] * volume);
        }
    }

    m_callbackDepth.fetch_sub(1, std::memory_order_release);
}

// [#174] Drain the realtime probe's counters. Runs on the GUI thread
// (teardown is a normal call, not a callback), so logging here is
// safe in a way it never was inside onPlayback().
void
SoundOutput::reportLateCallbacks()
{
    int const late = m_lateCallbacks.exchange(0, std::memory_order_relaxed);
    if (!late)
        return;
    qint64 const worst = m_worstLateMs.exchange(0, std::memory_order_relaxed);
    qWarning() << "[AUDIO-CB-PROBE]" << late
               << "late playback callback(s) this session, worst"
               << worst << "ms (>=150 ms means the audio thread was"
               << "preempted; the card likely underran)";
}

void
SoundOutput::teardown()
{
    // Report what the callback recorded before the device goes away --
    // one line per playback session instead of one per late callback,
    // and none of it on the realtime thread.
    reportLateCallbacks();
    if (m_deviceInitialized) {
        // Hand off to the reaper: uninit runs on a detached thread so this
        // never blocks even when miniaudio's PulseAudio worker is wedged in
        // ppoll after a device re-enumeration (AudioDeviceReaper.h). That is
        // what lets the audio QThread finish at shutdown/config-switch
        // instead of hanging until the watchdog force-exits.
        retireAudioDevice(m_dev, m_tag);
        m_dev = nullptr;
        m_deviceInitialized = false;
        // [reapguard] Drain any in-flight callback of the retired device
        // before clearing m_source below — otherwise it could observe the
        // NEW source that buildAndStart assigns right after this returns
        // and steal its first samples. New callbacks can't enter (retire
        // cleared pUserData); the WEDGED worker is stuck in ppoll, not in
        // our callback, so depth is 0 there. Bound: 50 ms.
        for (int i = 0;
             m_callbackDepth.load(std::memory_order_acquire) > 0 && i < 1000;
             ++i) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        m_source = nullptr;
    }
    // [AUDIO-PRIORITY 2026-06-16 build 286] Context uninit MUST come
    // after device uninit — the device's callback thread is owned by
    // the context, and uninit'ing the context first would yank the
    // thread out from under a still-running callback. The device uninit is
    // now async (reaper), but the context is only ever init'd in the
    // reverted build-286 path (m_contextInitialized stays false today), so
    // there is no live device/context ordering to honor here.
    if (m_contextInitialized) {
        ma_context_uninit(&m_context_ma);
        m_contextInitialized = false;
    }
}

bool
SoundOutput::buildAndStart(QIODevice * source)
{
    teardown();

    if (m_device.isNull()) {
        if (!source) return false;  // nothing to do
        // Fall through anyway; miniaudio will pick the system default
        // when pDeviceID is null.
    }

    ma_device_id deviceId;
    bool const haveDevice = !m_device.isNull()
                          && resolvePlaybackDevice(m_device, deviceId);

    // [AUDIO-PRIORITY REVERT 2026-06-16 build 290] Build 286 added an
    // explicit ma_context with ma_thread_priority_realtime. The probe
    // data from build 289 showed zero preemption events, suggesting
    // the realtime request wasn't actually helping (silently falling
    // back to default on Linux without CAP_SYS_NICE) — and yet the
    // gap behavior differs vs build 285 which used a null context.
    // Reverting to null context to match build 285 exactly until we
    // understand what changed. m_context_ma stays defined as a member
    // for future re-use but is not initialized in this code path.
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.pDeviceID = haveDevice ? &deviceId : nullptr;
    cfg.playback.format    = ma_format_s16;
    cfg.playback.channels  = static_cast<ma_uint32>(m_format.channelCount);
    cfg.sampleRate         = static_cast<ma_uint32>(m_format.sampleRate);
    if (m_msBuffered > 0u) {
        cfg.periodSizeInMilliseconds = m_msBuffered;
    }
    cfg.dataCallback = &SoundOutput::s_dataCallback;
    cfg.pUserData    = this;

    // teardown() above retired any prior device and nulled m_dev; allocate
    // fresh storage for this open.
    if (!m_dev) m_dev = new ma_device{};

    if (ma_device_init(nullptr, &cfg, m_dev) != MA_SUCCESS) {
        Q_EMIT error(tr("Failed to initialize audio output device."));
        delete m_dev;  // never init'd — free directly, do NOT uninit
        m_dev = nullptr;
        return false;
    }
    m_deviceInitialized = true;
    m_source = source;

    if (ma_device_start(m_dev) != MA_SUCCESS) {
        Q_EMIT error(tr("Failed to start audio output device."));
        teardown();
        m_source = nullptr;
        return false;
    }

    // [TODO #113 2026-07-23] Report what we ACTUALLY opened (see the
    // matching block in SoundInput::start).
    QString const openedName =
        QString::fromUtf8(m_dev->playback.name);
    qWarning() << "[AUDIO-OUT] device started:"
               << "requested=" << (m_device.isNull()
                                       ? QStringLiteral("(default)")
                                       : m_device.description)
               << "opened=" << openedName
               << "| callback:" << m_dev->sampleRate << "Hz"
               << m_dev->playback.channels << "ch"
               << ma_get_format_name(m_dev->playback.format)
               << "| hardware:" << m_dev->playback.internalSampleRate
               << "Hz" << m_dev->playback.internalChannels << "ch"
               << ma_get_format_name(m_dev->playback.internalFormat);

    if (!m_device.isNull() && !haveDevice) {
        qWarning() << "[AUDIO-OUT] CONFIGURED DEVICE NOT FOUND — fell back "
                      "to system default:" << openedName;
        Q_EMIT deviceFallback(
            tr("Your selected audio output device was not found, so the "
               "system default is being used instead.\n\n"
               "Selected: %1\nNow using: %2\n\n"
               "Transmitted audio is probably not reaching the radio. "
               "This usually happens when a USB audio device is "
               "unplugged, re-plugged, or renumbered. Reselect the "
               "device in Settings once it is back.")
                .arg(m_device.description, openedName));
    }

    Q_EMIT status(tr("Sending"));
    return true;
}

/**
 * @brief Convenience overload that constructs an AudioFormat for the
 * given channel count at JS8Call's standard 48 kHz Int16.
 */
void
SoundOutput::setFormat(AudioDeviceInfo const & device,
                       unsigned channels,
                       unsigned msBuffered)
{
    Q_ASSERT(0 < channels && channels < 3);

    AudioFormat fmt;
    fmt.sampleRate   = 48000;
    fmt.channelCount = static_cast<int>(channels);
    fmt.sample       = AudioFormat::Int16;

    setDeviceFormat(device, fmt, msBuffered);
}

/**
 * @brief Stash the device + format. We do NOT init the ma_device here;
 * that happens in restart(source) once we have a source to bind. This
 * is a behavior change from the QAudioSink era's pre-create + warmup
 * pattern, but miniaudio's WASAPI backend doesn't have the cold-start
 * latency Qt6Multimedia did, so init-on-demand is fine.
 */
void
SoundOutput::setDeviceFormat(AudioDeviceInfo const & device,
                             AudioFormat const & format,
                             unsigned msBuffered)
{
    bool const sameParams = (m_device == device
                          && m_format == format
                          && m_msBuffered == msBuffered);
    if (sameParams && m_deviceInitialized) {
        return;
    }

    teardown();
    m_source     = nullptr;
    m_device     = device;
    m_format     = format;
    m_msBuffered = msBuffered;
}

/**
 * @brief Bind a source and (re)start the playback device.
 */
void
SoundOutput::restart(QIODevice * source)
{
    // Race guard: same source already streaming → no-op. Multiple
    // GUI-thread emits of sendMessage can land back-to-back on the
    // audio thread queue. Same intent as the prior QAudioSink code.
    if (m_deviceInitialized
        && m_source == source
        && !m_gone                      // [TODO #335] a flagged stream is rebuilt
        && ma_device_is_started(m_dev)) {
        return;
    }
    buildAndStart(source);
}

// [TODO #335]
void
SoundOutput::rebuild()
{
    teardown();
    m_source = nullptr;
}

void
SoundOutput::suspend()
{
    if (m_deviceInitialized && ma_device_is_started(m_dev)) {
        ma_device_stop(m_dev);
        Q_EMIT status(tr("Suspended"));
    }
}

void
SoundOutput::resume()
{
    if (m_deviceInitialized && !ma_device_is_started(m_dev)) {
        if (ma_device_start(m_dev) != MA_SUCCESS) {
            Q_EMIT error(tr("Failed to resume audio output device."));
        } else {
            Q_EMIT status(tr("Sending"));
        }
    }
}

/**
 * @brief Reset is treated as stop — the QAudioSink semantic of
 * "discard buffered data and idle the stream" maps cleanly onto
 * ma_device_stop. The next restart() will spin a fresh device.
 */
void
SoundOutput::reset()
{
    if (m_deviceInitialized && ma_device_is_started(m_dev)) {
        ma_device_stop(m_dev);
    }
}

void
SoundOutput::stop()
{
    teardown();
    m_source = nullptr;
    Q_EMIT status(tr("Stopped"));
}

qreal
SoundOutput::attenuation() const
{
    return -(20. * std::log(m_volume) / std::log(10.));
}

bool
SoundOutput::isStreaming() const
{
    return m_deviceInitialized && !m_gone && ma_device_is_started(m_dev);
}

void
SoundOutput::setAttenuation(qreal a)
{
    Q_ASSERT(0. <= a && a <= 999.);
    m_volume = std::pow(10.0, -a / 20.0);
}

void
SoundOutput::resetAttenuation()
{
    m_volume = 1.0;
}

/**
 * @brief Destructor. Just tears down; the device (if any) goes to the
 * detached reaper, which outlives this object and frees it whenever
 * ma_device_uninit returns — a wedged uninit can never become a
 * use-after-free here.
 */
SoundOutput::~SoundOutput()
{
    teardown();
}

Q_LOGGING_CATEGORY(soundout_js8, "soundout.js8", QtWarningMsg)
