// -*- Mode: C++ -*-
#ifndef SOUNDOUT_H__
#define SOUNDOUT_H__

#include "JS8_Audio/AudioDeviceInfo.h"
#include "JS8_Audio/AudioFormat.h"
#include "JS8_Audio/miniaudio/miniaudio.h"

#include <QObject>
#include <QString>

#include <atomic>

class QIODevice;

// Streams audio samples from a QIODevice source (the Modulator) to the
// system's selected output device.
//
// Build 141 swapped this from QAudioSink (Qt6Multimedia) to miniaudio's
// ma_device. Same rationale as SoundInput (Build 140): Qt 6.9.x WASAPI
// teardown races caused a long crash family. The busywait,
// deferred-delete drain, m_tearingDown gate, platform-specific
// stop-vs-recreate logic, and Build 138 first-only warmup are all gone.
//
// [audioreap 2026-07-24] Teardown is NOT synchronous: the heap-owned
// ma_device goes to a detached reaper (AudioDeviceReaper.h) because
// ma_device_uninit's worker join can wedge forever on a PulseAudio
// device re-enumeration. teardown() drains in-flight callbacks
// (m_callbackDepth) before m_source may be reassigned.
class SoundOutput : public QObject {
    Q_OBJECT;

  public:
    // `tag` is the log prefix so TX audio and notification-audio
    // instances are distinguishable. Default matches the historical
    // "[FT2-TX]" for source compatibility.
    explicit SoundOutput(QString const & tag = QStringLiteral("FT2-TX"))
        : m_tag("[" + tag + "]") {}

    ~SoundOutput() override;

    qreal       attenuation() const;
    AudioFormat format() const { return m_format; }
    bool        isStreaming() const;   // false while the device is flagged gone
    // [TODO #335] the device watch found the configured device missing:
    // isStreaming() says false so the modulator never warm-starts on a
    // stream that may be dead or re-routed; cleared by a successful
    // buildAndStart or by the watch
    void        setDeviceGone(bool gone) { m_gone = gone; }
    // [TODO #335] retire the device; the next restart()/warmStart opens
    // a fresh one on the configured device
    Q_SLOT void rebuild();

  public Q_SLOTS:
    void setFormat(AudioDeviceInfo const & device, unsigned channels,
                   unsigned msBuffered = 0u);
    void setDeviceFormat(AudioDeviceInfo const & device,
                         AudioFormat const & format,
                         unsigned msBuffered = 0u);
    void restart(QIODevice *);
    void suspend();
    void resume();
    void reset();
    void stop();
    void setAttenuation(qreal); /* unsigned */
    void resetAttenuation();    /* to zero */

  Q_SIGNALS:
    void error(QString message) const;
    void status(QString message) const;
    // [TODO #113 2026-07-23] Configured playback device unresolvable →
    // miniaudio opened the SYSTEM DEFAULT instead. See the matching
    // comment in SoundInput.h; dialog-only by design (error() would
    // also clear the operator's device selection).
    void deviceFallback(QString message) const;

  private:
    static void s_dataCallback(ma_device * device,
                               void * pOutput,
                               void const * pInput,
                               ma_uint32 frameCount);
    void onPlayback(void * pOutput, ma_uint32 frameCount);

    void teardown();   // ma_device_uninit guard
    bool buildAndStart(QIODevice * source);

    QString         m_tag;
    AudioDeviceInfo m_device;
    AudioFormat     m_format;
    unsigned        m_msBuffered = 0u;
    qreal           m_volume     = 1.0;
    QIODevice *     m_source     = nullptr;
    // Heap-owned so a wedged ma_device_uninit can be handed to the detached
    // reaper (JS8_Audio/AudioDeviceReaper.h) and outlive this object without
    // a use-after-free. null when no device is open.
    ma_device *     m_dev        = nullptr;
    bool            m_deviceInitialized = false;
    bool            m_gone       = false;   // [TODO #335]
    // [reapguard 2026-07-25] Callbacks in flight on miniaudio's worker.
    // With teardown now async (reaper), teardown() drains this (bounded,
    // µs in practice) after retiring the device and before m_source is
    // reassigned — otherwise an in-flight callback of the OLD device
    // could read the NEW source and steal its first samples. See the
    // matching member in SoundInput.h.
    std::atomic<int> m_callbackDepth{0};

    // [#174] LATE-CALLBACK PROBE, recorded on the realtime thread and
    // reported off it. The callback used to qWarning() directly, which
    // put disk I/O on the audio thread at exactly the moment that
    // thread was already being preempted -- deepening the fault it
    // exists to detect. Detection is worth keeping (the audio wedge
    // was a real bug); the I/O is not. Counting is allocation-free
    // and wait-free, so it is safe where logging never was.
    std::atomic<int> m_lateCallbacks{0};
    std::atomic<qint64> m_worstLateMs{0};
    // [cbprobereset 2026-11-03] Time of this device's previous callback,
    // 0 = none yet. Was a function-static in onPlayback(): shared by the
    // TX and notification instances, and never cleared when the stream
    // stopped, so the time a stream was OFF (Tune release rebuilds it;
    // suspend()/resume()) was reported as one "late callback" -- 14 to
    // 30 s values in the field, impossible for a running stream. Cleared
    // before every ma_device_start (buildAndStart, resume), while no
    // callback can run, so only gaps inside a running stream count.
    std::atomic<qint64> m_lastCallbackMs{0};
    void reportLateCallbacks();   // GUI thread only
    // [AUDIO-PRIORITY 2026-06-16 build 286] miniaudio puts thread
    // priority on the context, not the device — so we own a context.
    // Init'd once per buildAndStart and torn down with the device.
    ma_context      m_context_ma {};
    bool            m_contextInitialized = false;
};

#endif
