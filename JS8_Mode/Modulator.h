#ifndef MODULATOR_HPP__
#define MODULATOR_HPP__

#include "JS8_Audio/AudioDevice.h"

#include <QDebug>
#include <QPointer>
#include <QVector>
#include <atomic>

class SoundOutput;

/**
 * Audio device that generates PCM audio frames that encode a message.
 *
 * Output can be muted while underway, preserving waveform timing when
 * transmission is resumed.
 *
 * This is intended to run in a thread different from the GUI thread.
 * It is **not** generally thread-safe, see remarks below.
 */
class Modulator final : public AudioDevice {
    Q_OBJECT;

  public:
    enum class State { Synchronizing, Active, KeepAlive, Idle };

    // Constructor

    explicit Modulator(QObject *parent = nullptr) : AudioDevice{parent} {}

    // Inline accessors

    /**
     * Whether the device is idle.
     *
     * This method is thread-safe, i.e., can be called from a different thread.
     */
    bool isIdle() const {
        auto s = m_state.load();
        return s == State::Idle || s == State::KeepAlive;
    }
    // [POST-ROLL 2026-07-13 TODO #72] 250 ms of written silence after
    // the FT2 waveform, symmetric with the 250 ms pre-roll (build 297).
    // PulseAudio's client buffer holds ~200 ms of not-yet-played audio
    // when the waveform-done poll fires stopTx; server-side buffer
    // rewinds can discard that queued tail (proven on-wire 2026-07-13:
    // modulator wrote 2507 ms, wire carried 2320 ms — the trailing
    // Costas array vanished, ~5% of frames, Linux/Pulse only; WASAPI
    // has no rewind concept, which is why Windows TX never showed it).
    // With the post-roll, any discarded tail eats padding, not Costas.
    // The PTT interval floor (3750 ms) already spaces frames, so the
    // extra 250 ms costs no throughput.
    static constexpr int FT2_POSTROLL_FRAMES = 12000;  // 250 ms @ 48 kHz

    bool isFT2WaveformDone() const {
        auto s = m_state.load();
        return s == State::Idle || s == State::KeepAlive
            || (m_ft2Mode &&
                m_ic >= static_cast<unsigned>(m_ft2WaveLen)
                            + FT2_POSTROLL_FRAMES);
    }

    // [BUILD 336 TODO #94] Expected playout of the CURRENT FT2
    // waveform (incl. post-roll) in ms — lets the guiUpdate stuck-TX
    // safety scale its cap to the waveform actually playing instead
    // of assuming single-frame length (the Visible Hail composite is
    // ~8.8 s). Heuristic read across threads: m_ft2WaveLen is set in
    // start() before any samples play, and the safety only consults
    // this seconds later.
    int ft2ExpectedDurationMs() const {
        return m_ft2WaveLen > 0
            ? static_cast<int>((static_cast<qint64>(m_ft2WaveLen)
                                + FT2_POSTROLL_FRAMES) * 1000 / 48000)
            : 0;
    }

    // [AUDIO-CADENCE PROBE 2026-06-09] Wall-clock timestamps captured
    // INSIDE readData() at the moment we WRITE the first waveform sample
    // into the audio-device buffer and the moment we WRITE the last one.
    // These tell us when the Modulator handed samples to the device,
    // not when the device actually played them on the wire — that's
    // typically later by ~one buffer fill (suspected ~1 s for our setup).
    // Sentinel -1 = not yet captured for this cycle. Cleared by start().
    // Both stores are once-per-cycle on transitions; no per-sample cost.
    qint64 audioStartedMs() const { return m_audioStartedMs.load(); }
    qint64 audioEndedMs() const { return m_audioEndedMs.load(); }

    // [ARQ TX TIMING ASYNC-FINISH STALE-SENTINEL FIX 2026-06-10 (build 231)]
    // Called synchronously by mainwindow.cpp at PTT-up to clear the audio
    // sentinels BEFORE the queued Modulator::start() runs. Without this
    // synchronous reset, the guiUpdate poll could see the previous
    // frame's stale audioStartedMs value (>0) and fire stopTx instantly,
    // producing the "transmit, skip, transmit, skip" pattern observed in
    // build 230. Always safe to call (idempotent); does not affect audio.
    void resetCadenceCapture() {
        m_audioStartedMs.store(-1);
        m_audioEndedMs.store(-1);
    }

    // Manipulators

    void close() override;

    /**
     * Sets the audio frequency.
     *
     * This is **not** by itself thread-safe, but ok if fed
     * via the Qt signalling mechanism.
     */
    Q_SLOT void setAudioFrequency(double const audioFrequency) {
        m_audioFrequency = audioFrequency;
    }

    // Slots

    Q_SLOT void start(double audioFrequency, int submode, double tx_delay,
                      SoundOutput *stream, Channel channel);
    // [TODO #108 keep-warm 2026-07-21] Open the output stream and park
    // in KeepAlive (dithered silence) WITHOUT a transmission. Called at
    // app startup and after audio-device changes so the FIRST real TX
    // is a warm restart. Root cause being fixed: a cold ma_device start
    // (pulse backend) consumes ~1.2 s of the stream instantly (buffer
    // fill / clock catch-up) and never renders it — on a cold first TX
    // that swallowed the 250 ms pre-roll plus ~870 ms of waveform head
    // incl. the leading Costas (captured on-wire 2026-07-21 20:01Z,
    // "waveform already done" at msSincePtt=1801 of a 3020 ms write).
    // Warmed this way, the catch-up eats KeepAlive dither instead.
    // No-op unless the state is Idle (dead stream).
    Q_SLOT void warmStart(SoundOutput *stream, Channel channel);
    Q_SLOT void stop(bool quick = false);
    Q_SLOT void tune(bool state = true);

    // Set by main thread (UI_Constructor) when ARQ messaging is in
    // flight. Modulator's silent-frames decision branch checks this
    // flag and bypasses the period-aligned WAIT-NEXT-PERIOD pad —
    // start immediately, no boundary wait, no SV1UY-style silent TX
    // (the bypass is bounded because m_arqRelax only goes true when
    // ARQ is in flight, and ARQ only runs in FT2 / Subspace where
    // continuous-frame TX makes the relaxed start safe). Atomic so
    // the audio-thread read is well-defined.
    void setArqRelax(bool relax) { m_arqRelax.store(relax); }

    // [BUILD 328] Full-frame ⚡ Hellschreiber bolt-beacon mode.
    // Pivot from the per-TX pre-roll preamble (builds 324-327, dead
    // code below). When the GUI passes a non-empty bolt waveform here,
    // the NEXT start() invocation uses that waveform as the FT2 data
    // for the cycle — instead of the encoded text bits — and PLAYS
    // THE BOLT. One-shot: the flag clears inside start() so the cycle
    // after the bolt reverts to normal FT2 modulation. Caller must
    // ensure Modulator is Idle when calling (no mid-stream swap).
    void setFullFrameBoltWaveform(QVector<float> waveform) {
        // [JTTY 499.5] Enforced here, not trusted to the caller: the
        // tone-generation block in prepareSending runs TWICE per key-up
        // (auto_tx_mode's direct call, then guiUpdate's, before m_iptt0
        // catches up), so a second staging arrives ~70 ms after the
        // first. If start() has already consumed the first, m_ft2Wave
        // points INTO m_boltWaveform and replacing it frees the buffer
        // the audio thread is reading. Before start() the replacement
        // is harmless (same content), so only a playing state refuses.
        State const s = m_state.load();
        if (s != State::Idle && s != State::KeepAlive) {
            qWarning() << "[FT2-TX] staging refused: waveform already"
                          " playing (" << waveform.size() << "samples)";
            return;
        }
        m_boltWaveform = std::move(waveform);
        m_useFullFrameBolt.store(!m_boltWaveform.isEmpty());
    }

    // [DEAD: builds 324-327 preamble code below, kept compiling but
    // setPaintBoltPreamble is no longer called by mainwindow.]
    void setPaintBoltPreamble(bool paint) { m_paintBolt.store(paint); }

  signals:
    /// Emitted when FT2 waveform finishes playing (transitions to KeepAlive).
    void ft2WaveformDone();

  protected:
    // QIODevice protocol

    qint64 readData(char *, qint64) override;
    qint64 writeData(char const *, qint64) override {
        return -1; // we don't consume data
    }

    // In current Qt versions, bytesAvailable() must return a size
    // that exceeds some threshold in order for the AudioSink to go
    // into Active state and start pulling data. This behavior began
    // on Windows with the 6.4 release, on Mac with 6.8, and on Linux
    // with 6.9.
    //
    // See: https://bugreports.qt.io/browse/QTBUG-108672

    qint64 bytesAvailable() const override { return 8000; }

  private:
    // Data members

    QPointer<SoundOutput> m_stream;
    std::atomic<State> m_state = State::Idle;
    // [TODO #108 keep-warm] Channel of the last start()/warmStart(),
    // so the quickClose path can re-open straight into KeepAlive.
    Channel m_warmChannel{Channel::Mono};
    bool m_quickClose = false;
    bool m_tuning = false;
    double m_audioFrequency;
    double m_audioFrequency0;
    double m_toneSpacing;
    double m_phi;
    double m_dphi;
    double m_amp;
    double m_nsps;
    qint64 m_silentFrames;
    std::atomic<bool> m_arqRelax{false};

    // [BUILD 324] ⚡ preamble plumbing — DEAD code, retained for the
    // dead setPaintBoltPreamble setter above. Build 328 pivoted to
    // full-frame bolt (see m_boltWaveform / m_useFullFrameBolt below).
    std::atomic<bool> m_paintBolt{false};
    QVector<qint16> m_boltAudio;
    int m_boltPos{0};

    // [BUILD 328] Full-frame ⚡ bolt plumbing.
    // m_useFullFrameBolt: GUI sets via setFullFrameBoltWaveform()
    //   before triggering TX; start() reads, overrides m_ft2Wave to
    //   point at m_boltWaveform's data, then clears the flag (one-shot).
    // m_boltWaveform: float audio samples (~144000 at 48 kHz = 3 s).
    //   Stays alive for the duration of the bolt TX cycle since
    //   start() points m_ft2Wave INTO this buffer's storage.
    std::atomic<bool> m_useFullFrameBolt{false};
    QVector<float> m_boltWaveform;
    unsigned m_ic;
    unsigned m_isym0;
#ifdef JS8_ENABLE_FT2
    bool m_ft2Mode = false;        // True when transmitting FT2 GFSK waveform
    float *m_ft2Wave = nullptr;    // Pointer to pre-generated waveform
    int m_ft2WaveLen = 0;          // Length of pre-generated waveform
#endif
    unsigned m_txCycleCount = 0;   // TX cycle counter for diagnostics

    // [AUDIO-CADENCE PROBE 2026-06-09] see audioStartedMs/audioEndedMs
    // public accessors above. Atomics because written from audio thread
    // (readData) and read from GUI thread (stopTx). Sentinel -1 = unset.
    std::atomic<qint64> m_audioStartedMs{-1};
    std::atomic<qint64> m_audioEndedMs{-1};
};

#endif
