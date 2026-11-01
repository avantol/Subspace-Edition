#include "AudioDeviceWatch.h"

#include <QTimer>
#include <QDebug>

#include "JS8_Audio/SoundInput.h"
#include "JS8_Audio/SoundOutput.h"

#include "moc_AudioDeviceWatch.cpp"

namespace {
// How often the devices are re-enumerated. CHOSEN: a replug takes
// seconds (12 s and 25 s on WM8Q's station, 2026-11-01), so 2 s sees
// the gap; the cost is one ma_context_get_devices call, milliseconds.
// A device that disappears and returns inside one period (a hub reset
// without a cable) can be missed for playback; the capture side is
// still caught by its stopped device.
int const kPollMs = 2000;
}

AudioDeviceWatch::AudioDeviceWatch(SoundInput *input, SoundOutput *output,
                                   QObject *parent)
    : QObject{parent}, m_input{input}, m_output{output} {
    m_ctxOk = (ma_context_init(nullptr, 0, nullptr, &m_ctx) == MA_SUCCESS);
    if (!m_ctxOk)
        qWarning() << "[AUDIO] device watch: no audio context, not watching";
}

AudioDeviceWatch::~AudioDeviceWatch() {
    if (m_ctxOk) ma_context_uninit(&m_ctx);
}

void AudioDeviceWatch::setDevices(AudioDeviceInfo const &capture,
                                  AudioDeviceInfo const &playback) {
    m_capture  = capture;
    m_playback = playback;
    m_gone     = false;
    if (m_output) m_output->setDeviceGone(false);
    if (!m_ctxOk) return;
    if (!m_timer) {
        m_timer = new QTimer{this};
        connect(m_timer, &QTimer::timeout, this, &AudioDeviceWatch::poll);
    }
    m_timer->start(kPollMs);
}

void AudioDeviceWatch::acknowledge() {
    m_gone = false;
    if (m_output) m_output->setDeviceGone(false);
}

// The same match SoundInput/SoundOutput use to open a device: the
// hex-encoded miniaudio id, or the description.
bool AudioDeviceWatch::present(AudioDeviceInfo const &info, bool playback) {
    if (info.isNull()) return true;   // "default": nothing specific to lose
    ma_device_info *playInfos = nullptr, *capInfos = nullptr;
    ma_uint32 playCount = 0, capCount = 0;
    if (ma_context_get_devices(&m_ctx, &playInfos, &playCount, &capInfos,
                               &capCount) != MA_SUCCESS)
        return true;                  // cannot tell: never a false alarm
    ma_device_info *infos = playback ? playInfos : capInfos;
    ma_uint32 count = playback ? playCount : capCount;
    for (ma_uint32 i = 0; i < count; ++i) {
        QByteArray const thisId
            = QByteArray(reinterpret_cast<char const *>(&infos[i].id),
                         sizeof(ma_device_id)).toHex();
        if ((!info.id.isEmpty() && info.id == thisId) ||
            (!info.description.isEmpty() &&
             info.description == QString::fromUtf8(infos[i].name)))
            return true;
    }
    return false;
}

void AudioDeviceWatch::poll() {
    bool const inPresent  = present(m_capture, false);
    bool const outPresent = present(m_playback, true);
    bool const inStarted  = m_capture.isNull() || !m_input || m_input->isStarted();

    if (!m_gone) {
        QString what;
        if (!inPresent)       what = tr("capture %1 gone").arg(m_capture.description);
        else if (!outPresent) what = tr("playback %1 gone").arg(m_playback.description);
        else if (!inStarted)  what = tr("capture %1 stopped").arg(m_capture.description);
        if (what.isEmpty()) return;
        m_gone = true;
        m_what = what;
        m_goneSince.start();
        if (m_output) m_output->setDeviceGone(true);
        Q_EMIT deviceGone(m_what, 0);
        return;
    }
    if (inPresent && outPresent) {
        Q_EMIT deviceBack(m_what);   // every poll until acknowledged
    } else {
        Q_EMIT deviceGone(m_what, int(m_goneSince.elapsed() / 1000));
    }
}
