#ifndef AUDIO_DEVICE_WATCH_H
#define AUDIO_DEVICE_WATCH_H

#include <QObject>
#include <QString>
#include <QElapsedTimer>

#include "JS8_Audio/AudioDeviceInfo.h"
#include "JS8_Audio/miniaudio/miniaudio.h"

class QTimer;
class SoundInput;
class SoundOutput;

// [TODO #335, field 2026-11-01] One watch for the two audio streams.
//
// A USB re-enumeration of a rig with a built-in codec (IC-7300: serial
// adapter and PCM2901 codec on one hub) takes the audio device away
// under the running streams. Nothing in the audio layer is told:
// PipeWire terminated the capture stream (no source-output left for
// JS8Call: RX dead, no log line; W5TTA's "rx off" of the same day was
// NOT this, her audio started fine, see TODO 336) and re-routed the
// playback stream (the first Tune after the replug was silent, the
// modulator believed its stream warm). Only a later stop rebuilt the
// output, by accident.
//
// Every poll this object re-enumerates the devices and checks that the
// CONFIGURED capture and playback devices are still present, and that
// the capture device (designed to run full-time) is still started.
// Gone: deviceGone() every poll with the seconds so far, and the output
// is flagged so the modulator takes no warm start on a dead stream.
// Back (both present again): deviceBack() every poll until the main
// window has rebuilt both streams and called acknowledge(). The main
// window does the rebuilding, because only it knows whether a
// transmission is in progress (never rebuild under a send).
//
// Lives on the audio thread with the streams: direct calls are safe.
class AudioDeviceWatch : public QObject {
    Q_OBJECT

  public:
    AudioDeviceWatch(SoundInput *input, SoundOutput *output,
                     QObject *parent = nullptr);
    ~AudioDeviceWatch();

    // The devices the streams were (re)started on; starts the polling.
    Q_SLOT void setDevices(AudioDeviceInfo const &capture,
                           AudioDeviceInfo const &playback);
    // The main window rebuilt both streams on the returned devices.
    Q_SLOT void acknowledge();

    Q_SIGNAL void deviceGone(QString what, int seconds) const;
    Q_SIGNAL void deviceBack(QString what) const;

  private:
    Q_SLOT void poll();
    bool present(AudioDeviceInfo const &info, bool playback);

    SoundInput  *m_input;
    SoundOutput *m_output;
    QTimer      *m_timer = nullptr;
    ma_context   m_ctx;
    bool         m_ctxOk = false;
    AudioDeviceInfo m_capture;
    AudioDeviceInfo m_playback;
    bool          m_gone = false;
    QElapsedTimer m_goneSince;
    QString       m_what;
};

#endif
