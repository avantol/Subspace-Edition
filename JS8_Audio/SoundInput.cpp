/**
 * @file SoundInput.cpp
 * @brief miniaudio-based capture (Build 140, replaces QAudioSource)
 */
#include "SoundInput.h"

#include "JS8_Audio/AudioDeviceReaper.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <chrono>
#include <cstring>
#include <thread>

#include "moc_SoundInput.cpp"

Q_DECLARE_LOGGING_CATEGORY(soundin_js8)

namespace {

// Resolve an AudioDeviceInfo to a concrete ma_device_id by enumerating
// capture devices and matching on either the hex-encoded id (preferred,
// available once Configuration is on miniaudio in Build 142) or the
// human-readable description (the Build 140 bridge while Configuration
// still uses QMediaDevices). Returns true on match; on miss the caller
// should fall back to miniaudio's default capture device.
bool
resolveCaptureDevice(AudioDeviceInfo const & info,
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
        QByteArray const wantedId = info.id;
        QString    const wantedName = info.description;

        for (ma_uint32 i = 0; i < capCount; ++i) {
            QByteArray const thisId
                = QByteArray(reinterpret_cast<char const *>(&capInfos[i].id),
                             sizeof(ma_device_id)).toHex();
            QString const thisName = QString::fromUtf8(capInfos[i].name);

            bool const idMatch   = !wantedId.isEmpty() && wantedId == thisId;
            bool const nameMatch = !wantedName.isEmpty() && wantedName == thisName;

            if (idMatch || nameMatch) {
                outId = capInfos[i].id;
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
 * @brief miniaudio capture callback. Forwards to the SoundInput
 * instance whose ma_device this is. Runs on miniaudio's worker thread,
 * NOT on the GUI or audio QThread.
 */
void
SoundInput::s_dataCallback(ma_device * device,
                           void * /* pOutput */,
                           void const * pInput,
                           ma_uint32    frameCount)
{
    if (auto * self = static_cast<SoundInput *>(device->pUserData)) {
        self->onCapture(pInput, frameCount);
    }
}

void
SoundInput::onCapture(void const * pInput, ma_uint32 frameCount)
{
    // [reapguard] Mark ourselves in flight for stop()'s drain — see
    // m_callbackDepth in the header. Single-exit so the release below
    // always runs.
    m_callbackDepth.fetch_add(1, std::memory_order_acquire);

    // [TODO #108 keep-warm] Monitor-off discards samples here; the
    // device itself keeps running (see m_discarding in the header).
    if (!m_discarding.load(std::memory_order_relaxed)
        && m_sink && frameCount != 0) {
        qint64 const bytes = static_cast<qint64>(frameCount) * m_sink->bytesPerFrame();
        m_sink->write(static_cast<char const *>(pInput), bytes);
    }

    m_callbackDepth.fetch_sub(1, std::memory_order_release);
}

/**
 * @brief Starts audio input from the specified device.
 */
void
SoundInput::start(AudioDeviceInfo const & device,
                  int                    framesPerBuffer,
                  AudioDevice *          sink,
                  AudioDevice::Channel   channel)
{
    Q_ASSERT(sink);

    // Idempotent guard: skip rebuild if the same device/channel/sink/buffer
    // is requested again and the stream is already running. Prevents the
    // destroy-and-rebuild churn that happens when the GUI emits
    // startAudioInputStream more than once during init or settings reload.
    if (m_deviceInitialized
        && m_sink == sink
        && m_lastDevice == device
        && m_lastChannel == channel
        && m_lastFramesPerBuffer == framesPerBuffer
        && ma_device_is_started(m_dev)) {
        return;
    }

    stop();

    m_sink = sink;
    m_lastDevice = device;
    m_lastChannel = channel;
    m_lastFramesPerBuffer = framesPerBuffer;

    if (!sink->initialize(QIODevice::WriteOnly, channel)) {
        Q_EMIT error(tr("Failed to initialize audio sink device"));
        return;
    }

    ma_device_id deviceId;
    bool const haveDevice = resolveCaptureDevice(device, deviceId);

    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.pDeviceID = haveDevice ? &deviceId : nullptr;  // null → default
    cfg.capture.format    = ma_format_s16;
    cfg.capture.channels  = (channel == AudioDevice::Mono) ? 1 : 2;
    cfg.sampleRate        = 48000;
    cfg.periodSizeInFrames = static_cast<ma_uint32>(framesPerBuffer);
    cfg.dataCallback      = &SoundInput::s_dataCallback;
    cfg.pUserData         = this;

    // stop() may have handed a previous (possibly wedged) device off to the
    // reaper and nulled m_dev; allocate fresh storage for this open.
    if (!m_dev) m_dev = new ma_device{};

    if (ma_device_init(nullptr, &cfg, m_dev) != MA_SUCCESS) {
        Q_EMIT error(tr("Failed to initialize audio input device."));
        delete m_dev;  // never init'd — free directly, do NOT uninit
        m_dev = nullptr;
        return;
    }
    m_deviceInitialized = true;

    if (ma_device_start(m_dev) != MA_SUCCESS) {
        Q_EMIT error(tr("Failed to start audio input device."));
        // init'd but not started — uninit safely
        retireAudioDevice(m_dev, QStringLiteral("[AUDIO-IN]"));
        m_dev = nullptr;
        m_deviceInitialized = false;
        return;
    }

    // [TODO #113 2026-07-23] Report what we ACTUALLY opened. Until now
    // nothing recorded the negotiated device/format, so a silent switch
    // to the wrong input was undetectable from the logs.
    QString const openedName = QString::fromUtf8(m_dev->capture.name);
    qWarning() << "[AUDIO-IN] device started:"
               << "requested=" << (device.isNull()
                                       ? QStringLiteral("(default)")
                                       : device.description)
               << "opened=" << openedName
               << "| callback:" << m_dev->sampleRate << "Hz"
               << m_dev->capture.channels << "ch"
               << ma_get_format_name(m_dev->capture.format)
               << "| hardware:" << m_dev->capture.internalSampleRate << "Hz"
               << m_dev->capture.internalChannels << "ch"
               << ma_get_format_name(m_dev->capture.internalFormat);

    // A configured device that could not be resolved means miniaudio was
    // given a null id and picked the SYSTEM DEFAULT — we are now
    // capturing something other than the radio, with a normal-looking
    // waterfall and zero decodes. Tell the operator, by name.
    if (!device.isNull() && !haveDevice) {
        qWarning() << "[AUDIO-IN] CONFIGURED DEVICE NOT FOUND — fell back "
                      "to system default:" << openedName;
        Q_EMIT deviceFallback(
            tr("Your selected audio input device was not found, so the "
               "system default is being used instead.\n\n"
               "Selected: %1\nNow using: %2\n\n"
               "You are probably not listening to the radio — the "
               "waterfall may look normal but nothing will decode. This "
               "usually happens when a USB audio device is unplugged, "
               "re-plugged, or renumbered. Reselect the device in "
               "Settings, or restart it there, once it is back.")
                .arg(device.description, openedName));
    } else if (m_dev->sampleRate != 48000u) {
        // Informational only: miniaudio resamples to our requested rate,
        // so this should never differ. If it ever does, decode timing is
        // wrong and the log will say so.
        qWarning() << "[AUDIO-IN] UNEXPECTED callback sample rate"
                   << m_dev->sampleRate << "(expected 48000) — decode "
                      "timing will be wrong";
    }

    Q_EMIT status(tr("Receiving"));
}

/**
 * @brief "Suspends" audio input — [TODO #108 keep-warm 2026-07-21]
 * the device is NOT stopped anymore. Stopping it (the old
 * ma_device_stop) meant every monitor-off/on cycle cold-started the
 * capture device; the original design intent was full-time input
 * audio with monitor as a display/decode gate only. Suspend now just
 * discards captured samples in onCapture — identical decode
 * semantics (the sink sees nothing), warm device.
 */
bool
SoundInput::isStarted() const
{
    return m_deviceInitialized && m_dev && ma_device_is_started(m_dev);
}

// [TODO #335] A fresh open on the last device, through the same stop()
// and start() every other re-open uses (the old device goes to the
// reaper; m_discarding, the monitor state, is untouched).
void
SoundInput::rebuild()
{
    if (!m_sink) return;
    AudioDeviceInfo const device = m_lastDevice;
    int const frames = m_lastFramesPerBuffer;
    AudioDevice::Channel const channel = m_lastChannel;
    AudioDevice *sink = m_sink;
    stop();
    start(device, frames, sink, channel);
}

void
SoundInput::suspend()
{
    m_discarding.store(true, std::memory_order_relaxed);
    Q_EMIT status(tr("Suspended"));
}

void
SoundInput::resume()
{
    if (m_sink) m_sink->reset();

    m_discarding.store(false, std::memory_order_relaxed);
    // Safety: if some path left the device stopped, restart it.
    if (m_deviceInitialized && !ma_device_is_started(m_dev)) {
        if (ma_device_start(m_dev) != MA_SUCCESS) {
            Q_EMIT error(tr("Failed to resume audio input device."));
            return;
        }
    }
    Q_EMIT status(tr("Receiving"));
}

/**
 * @brief Stops audio input and releases the device. The device is handed
 * to the reaper (AudioDeviceReaper.h) which uninits it on a detached
 * thread, so this NEVER blocks even when miniaudio's PulseAudio worker is
 * wedged in ppoll after a device re-enumeration. That non-blocking
 * teardown is what lets the audio QThread finish at shutdown/config-switch
 * instead of hanging until the watchdog force-exits (which killed the
 * in-place config restart). Idempotent: safe to call twice (m_dev nulled).
 */
void
SoundInput::stop()
{
    if (m_deviceInitialized) {
        // Takes ownership; frees when uninit ends.
        retireAudioDevice(m_dev, QStringLiteral("[AUDIO-IN]"));
        m_dev = nullptr;
        m_deviceInitialized = false;
        // [reapguard] Drain any in-flight callback before closing the
        // sink below. New callbacks can't enter (retire cleared
        // pUserData); one already past that check finishes in µs. The
        // WEDGED worker is stuck in ppoll, not in our callback, so
        // depth is 0 there and this doesn't wait. Bound: 50 ms.
        for (int i = 0;
             m_callbackDepth.load(std::memory_order_acquire) > 0 && i < 1000;
             ++i) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
    if (m_sink) m_sink->close();
}

/**
 * @brief Destructs the SoundInput object. Just stops; the device (if
 * any) is handed to the detached reaper, which outlives this object
 * and frees it whenever ma_device_uninit returns — so a wedged uninit
 * can never turn into a use-after-free here.
 */
SoundInput::~SoundInput()
{
    stop();
}

Q_LOGGING_CATEGORY(soundin_js8, "soundin.js8", QtWarningMsg)
