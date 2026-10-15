/**
 * @file Modulator.cpp
 * @brief Implementation of Modulator class
 */
#include "Modulator.h"
#include "JS8Submode.h"
#include "SubspacePreamble.h"
#include "JS8_Audio/SoundOutput.h"
#include "JS8_Include/commons.h"
#include "JS8_Main/DriftingDateTime.h"
#include "JS8_UI/mainwindow.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QtMath>

#include <algorithm>
#include <limits>
#include <numbers>

#include "moc_Modulator.cpp"

Q_DECLARE_LOGGING_CATEGORY(modulator_js8)

namespace {
constexpr double TAU = 2 * std::numbers::pi;
constexpr auto FRAME_RATE = 48000;
constexpr auto MS_PER_SEC = 1000;
} // namespace

/**
 * @brief Start the modulation process
 *
 * @param frequency
 * @param submode
 * @param txDelay
 * @param stream
 * @param channel
 */
void Modulator::start(double const frequency, int const submode,
                      double const txDelay, SoundOutput *const stream,
                      Channel const channel) {
    Q_ASSERT(stream);

    ++m_txCycleCount;
    const State current_state = m_state.load();
    qCDebug(modulator_js8) << "[FT2-TX] Modulator::start() cycle#" << m_txCycleCount
               << "freq=" << frequency
               << "submode=" << submode << "state=" << (int)current_state
               << "tuning=" << m_tuning;
    if (current_state != State::Idle && current_state != State::KeepAlive) {
        qWarning() << "[FT2-TX] Modulator not idle/keepalive, SKIPPING duplicate start"
                    << "cycle#" << m_txCycleCount;
        return;
    }

    m_quickClose = false;
    m_warmChannel = channel;
    m_audioFrequency = frequency;
#ifdef JS8_ENABLE_FT2
    // [JTTY] A staged full-frame waveform (setFullFrameBoltWaveform)
    // plays in waveform mode whatever speed is selected: a JTTY
    // message keys up with Normal selected. The override block below
    // then points m_ft2Wave at it exactly as for the bolt.
    bool const stagedWaveform =
        m_useFullFrameBolt.load() && !m_boltWaveform.isEmpty();
    if ((submode == 16 /* Varicode::JS8CallFT2 */ || stagedWaveform) &&
        !m_tuning) {
        m_ft2Mode = true;
        m_ft2Wave = ft2_txwave;
        m_ft2WaveLen = ft2_txwave_len;
        m_nsps = FT2_TX_NSPS;  // 1152 at 48kHz
        m_toneSpacing = 12000.0 / FT2_NSPS;  // ~41.67 Hz
    } else {
        m_ft2Mode = false;
        m_nsps = JS8::Submode::samplesForOneSymbol(submode);
        m_toneSpacing = JS8::Submode::toneSpacing(submode);
    }
#else
    m_nsps = JS8::Submode::samplesForOneSymbol(submode);
    m_toneSpacing = JS8::Submode::toneSpacing(submode);
#endif
    m_isym0 = std::numeric_limits<unsigned>::max();
    m_amp = std::numeric_limits<qint16>::max();
#ifdef JS8_ENABLE_JTTY
    // [JTTY] Backstop: a JTTY cycle with nothing staged must never fall
    // into the JS8 tone branch (stale itone[] at JTTY symbol timing).
    // Play silence and report the waveform done at once.
    if (submode == 32 /* Varicode::JS8CallJTTY */ && !m_tuning &&
        !(m_ft2Mode && m_ft2Wave && m_ft2WaveLen > 0)) {
        // [audit L4] the cycle actually ends on the stop path's safety
        // cap: with no waveform the audio-end poll never fires
        qWarning() << "[JTTY-TX] Modulator::start: no waveform staged;"
                      " transmitting silence until the safety cap";
        m_ft2Mode = true;
        m_ft2Wave = nullptr;
        m_ft2WaveLen = 0;
        m_amp = 0.0;
    }
#endif
    m_audioFrequency0 = 0.0;
    m_phi = 0.0;
    m_silentFrames = 0;
    m_ic = 0;

    // [BUILD 324] Subspace ⚡ preamble drain buffer — DEAD code, kept
    // for the dead drain logic in readData(). Build 328 pivoted to
    // full-frame bolt that swaps m_ft2Wave below.
    m_boltAudio.clear();
    m_boltPos = 0;

    // [BUILD 328] Full-frame ⚡ bolt override. When the GUI staged a
    // bolt waveform via setFullFrameBoltWaveform(), this TX cycle
    // plays the bolt INSTEAD of the encoded text audio. We override
    // m_ft2Wave/m_ft2WaveLen to point at the bolt buffer; the rest of
    // the FT2 TX path runs unchanged, just with bolt samples as the
    // "FT2 waveform". One-shot — flag clears so next cycle reverts.
    if (m_ft2Mode && !m_tuning && m_useFullFrameBolt.load() &&
        !m_boltWaveform.isEmpty()) {
        m_ft2Wave = m_boltWaveform.data();
        m_ft2WaveLen = m_boltWaveform.size();
        m_useFullFrameBolt.store(false);
        qCDebug(modulator_js8) << "[FT2-TX] ⚡ FULL-FRAME bolt: playing"
                    << m_boltWaveform.size() << "samples ("
                    << (m_boltWaveform.size() * 1000 / FRAME_RATE)
                    << "ms) at" << frequency << "Hz";
    }

    // [AUDIO-CADENCE PROBE 2026-06-09] Reset capture sentinels for this
    // cycle. Stores happen inside readData() on transitions.
    m_audioStartedMs.store(-1);
    m_audioEndedMs.store(-1);

    // If we're not tuning, then we'll need to figure out exactly when we
    // should start transmitting; this will depend on the submode in play.

    if (!m_tuning) {
        // Get the nominal transmit start time for this submode, and determine
        // which millisecond of the current transmit period we're currently at.

        qint64 const nowMS = DriftingDateTime::currentMSecsSinceEpoch();
        unsigned const periodMS = JS8::Submode::periodMS(submode);
        auto const startDelayMS = JS8::Submode::startDelayMS(submode);
        unsigned const periodOffsetMS = nowMS % periodMS;

        // If we haven't yet hit the nominal start time for the period, then we
        // will need to inject some silence into the transmission; determine the
        // number of silent audio samples required to start audio at the correct
        // amount of delay into the period.
        //
        // If we have hit the nominal start time for the period, adjust for late
        // start if we're not exactly at the nominal start time.

        // [TX-CADENCE UNIFIED 2026-06-17 build 297]
        // RESTORED from build 286's design intent: both modes use
        // the SAME pre-silence pad. Build 288 had reverted to mode-
        // dependent padding (ARQ-RELAX: 100ms, TX-DELAY: 200-300ms)
        // which the user instructed must be the same. The build-288
        // "non-ARQ gap disappeared" symptom that motivated the
        // revert was later determined to be a receiver-side display
        // artifact, not a real wire issue. So the unification was
        // never actually broken — only mis-blamed.
        //
        // The pad value is fixed at 250 ms, large enough that the
        // receiver's L2 decoder sees a clean silent region before
        // the leading Costas tones (which build 295 ARQ-RELAX at
        // only 100ms pad apparently didn't provide — frame decode
        // failures correlated with the shorter pad). Period-
        // alignment is now a CALLER-side scheduling decision in
        // prepareSending, not a Modulator-internal concern. The
        // Modulator pads exactly the same amount regardless of
        // m_arqRelax or m_ft2Mode.
        // [BUILD 328] Reverted to original 250 ms pre-roll. The
        // bolt-in-pre-roll approach (builds 324-327) couldn't make
        // the bolt visibly recognizable without breaking FT2 cycle
        // timing. Pivoted to full-frame bolt design: bolt becomes a
        // standalone TX cycle, audio fills the whole frame instead
        // of piggybacking on FT2 pre-roll. Per-TX bolt preamble code
        // below (m_paintBolt, m_boltAudio, m_boltPos) is now dead;
        // the setPaintBoltPreamble setter is no longer called by
        // mainwindow. Full-frame bolt rides through m_ft2Wave swap
        // (see setBoltOnlyWaveform).
        constexpr unsigned UNIFIED_PRE_SILENCE_MS = 250;
        m_silentFrames = UNIFIED_PRE_SILENCE_MS * FRAME_RATE / MS_PER_SEC;
        qCDebug(modulator_js8) << "[TX-CADENCE] Modulator path: UNIFIED"
                   << "periodOffsetMS=" << periodOffsetMS
                   << "padMS=" << UNIFIED_PRE_SILENCE_MS
                   << "silentFrames=" << m_silentFrames
                   << "silentMS=" << (m_silentFrames * 1000 / FRAME_RATE)
                   << "(arqRelax=" << m_arqRelax.load()
                   << " ft2Mode=" << m_ft2Mode << ")";
        Q_UNUSED(startDelayMS);
        Q_UNUSED(txDelay);
    } else {
        qCDebug(modulator_js8) << "Modulator finds it is tuning.";
    }


    if (current_state == State::KeepAlive && m_stream && isOpen()
        && m_stream->isStreaming()) {
        // Warm restart: stream is still running and pulling.
        // Just switch state — readData() will produce waveform on next call.
        // Safe on all platforms when the stream is actively pulling data.
        // Avoids QAudioSink restart which crashes Qt6Multimedia on Windows
        // after rapid stop/start cycles (WASAPI ACCESS_VIOLATION).
        if (0 < m_silentFrames) {
            m_state.store(State::Synchronizing);
        } else {
            m_state.store(State::Active);
        }
        m_stream = stream;
        qCDebug(modulator_js8) << "[FT2-TX] Modulator::start() warm restart from KeepAlive"
                    << "cycle#" << m_txCycleCount
                    << "ft2Mode=" << m_ft2Mode
                    << "state=" << (int)m_state.load();
    } else {
        // Cold start: fallback when stream is not actively pulling.
        // Uses m_stream->restart() which reinitializes the QAudioSink.
        if (current_state == State::KeepAlive) {
            qWarning() << "[FT2-TX] Modulator::start() cold restart from KeepAlive"
                        << "cycle#" << m_txCycleCount
                        << "streaming=" << (m_stream ? m_stream->isStreaming() : false)
                        << "isOpen=" << isOpen();
            m_state.store(State::Idle);
        }
        // Cold start: initialize device and start stream.
        initialize(QIODevice::ReadOnly, channel);

        if (0 < m_silentFrames) {
            m_state.store(State::Synchronizing);
            qCDebug(modulator_js8)
                << "Symbol transmission to start after"
                << ((float)m_silentFrames) / FRAME_RATE * MS_PER_SEC
                << "ms of silence.";
        } else {
            m_state.store(State::Active);
            qCDebug(modulator_js8) << "Symbol transmission to start immediately.";
        }

        m_stream = stream;
        if (m_stream) {
            qCDebug(modulator_js8) << "[FT2-TX] Modulator::start() calling m_stream->restart()"
                        << "cycle#" << m_txCycleCount
                        << "ft2Mode=" << m_ft2Mode
                        << "ft2WaveLen=" << m_ft2WaveLen
                        << "state=" << (int)m_state.load();
            m_stream->restart(this);
        } else {
            qWarning() << "[FT2-TX] Modulator::start: NO audio output stream!";
        }
    }
}

// [TODO #108 keep-warm] See the header comment. Opens the device and
// parks in KeepAlive so the stream's cold-start catch-up (~1.2 s
// consumed instantly on the pulse backend) swallows dither, never a
// waveform head. Idempotent: any live state (KeepAlive/TX) is left
// alone.
void Modulator::warmStart(SoundOutput *const stream,
                          Channel const channel) {
    if (!stream) {
        return;
    }
    auto const s = m_state.load();
    if (s == State::Active || s == State::Synchronizing) {
        return;  // TX in flight — never stomp it
    }
    if (s == State::KeepAlive && m_stream && isOpen() &&
        m_stream->isStreaming()) {
        return;  // already warm and live
    }
    // Idle, or KeepAlive whose stream died underneath (device change
    // teardown) — (re)open into KeepAlive.
    m_warmChannel = channel;
    initialize(QIODevice::ReadOnly, channel);
    m_state.store(State::KeepAlive);
    m_stream = stream;
    qCDebug(modulator_js8)
        << "[FT2-TX] Modulator::warmStart — opening output stream into "
           "KeepAlive (dithered silence)";
    m_stream->restart(this);
}

/**
 * @brief Set tuning mode
 *
 * @param tuning
 */
void Modulator::tune(bool const tuning) {
    m_tuning = tuning;
    if (!m_tuning)
        stop(true);
}

/**
 * @brief Stop the modulation process
 *
 * @param quickClose
 */
void Modulator::stop(bool const quickClose) {
    qCDebug(modulator_js8) << "[FT2-TX] Modulator::stop() quickClose=" << quickClose
               << "cycle#" << m_txCycleCount
               << "state=" << (int)m_state.load();
    if (quickClose) {
        // Hard stop: discard queued audio immediately (tune cancel,
        // etc.). [TODO #108 keep-warm] …but never leave the stream
        // dead: rebuild straight into KeepAlive so the fresh device's
        // start-up catch-up eats dither and the NEXT TX is warm.
        m_quickClose = true;
        close();  // sets Idle, stops stream
        m_quickClose = false;
        if (m_stream) {
            initialize(QIODevice::ReadOnly, m_warmChannel);
            m_state.store(State::KeepAlive);
            m_stream->restart(this);
            qCDebug(modulator_js8)
                << "[FT2-TX] Modulator::stop(quick) — stream re-warmed "
                   "into KeepAlive";
        }
        return;
    }
    auto s = m_state.load();
    if (s == State::Active || s == State::Synchronizing) {
        // Soft stop from active TX: transition to KeepAlive.
        // Stream stays alive, readData() feeds silence.
        m_state.store(State::KeepAlive);
    }
    // If already Idle or KeepAlive, nothing to do.
}

/**
 * @brief Close the modulator
 *
 */
void Modulator::close() {
    if (m_stream) {
        if (m_quickClose)
            m_stream->reset();
        else
            m_stream->stop();
    }

    m_state.store(State::Idle);
    AudioDevice::close();
}

/**
 * @brief Read data from the modulator
 *
 * @param data
 * @param maxSize
 * @return qint64
 */
qint64 Modulator::readData(char *const data, qint64 const maxSize) {
    if (maxSize == 0)
        return 0;

    Q_ASSERT(!(maxSize % qint64(bytesPerFrame()))); // no torn frames
    Q_ASSERT(isOpen());

    qint64 framesGenerated = 0;
    qint64 const maxFrames = maxSize / bytesPerFrame();
    qint16 *samples = reinterpret_cast<qint16 *>(data);
    qint16 const *const samplesEnd =
        samples + maxFrames * (bytesPerFrame() / sizeof(qint16));

    switch (m_state.load()) {
    case State::Synchronizing: {
        if (m_silentFrames) {
            // Send silence up to end of start delay.
            // [BUILD 324] During the first portion of the silent
            // pre-roll, emit Subspace ⚡ bolt samples instead of
            // zeros — when the GUI flagged this TX for a preamble
            // and m_boltAudio was populated by start(). Cycle timing
            // is unchanged: bolt occupies what would have been the
            // first ~155 ms of silence, then zeros for the remainder.

            framesGenerated = qMin(m_silentFrames, maxFrames);

            do {
                qint16 outSample = 0;
                if (m_boltPos < m_boltAudio.size()) {
                    outSample = m_boltAudio[m_boltPos++];
                }
                samples = load(outSample, samples);
            } while (--m_silentFrames && samples != samplesEnd);

            if (!m_silentFrames) {
                qCDebug(modulator_js8) << "[FT2-TX] Modulator: Synchronizing→Active"
                            << "cycle#" << m_txCycleCount
                            << "ft2Mode=" << m_ft2Mode
                            << "boltDrained=" << m_boltPos << "of"
                            << m_boltAudio.size();
                m_state.store(State::Active);
            }
        }
    }
        [[fallthrough]];

    case State::Active: {
#ifdef JS8_ENABLE_FT2
        if (m_ft2Mode && m_ft2Wave && m_ft2WaveLen > 0) {
            // FT2: play back pre-generated GFSK waveform
            if (m_ic == 0)
                qCDebug(modulator_js8) << "[FT2-TX] readData: starting waveform, cycle#"
                           << m_txCycleCount << "len="
                           << m_ft2WaveLen << "first sample="
                           << m_ft2Wave[0] << m_ft2Wave[1] << m_ft2Wave[2]
                           << "maxSize=" << maxSize
                           << "bytesPerFrame=" << bytesPerFrame();
            // [AUDIO-CADENCE PROBE 2026-06-09] Capture the wall-clock
            // moment we're about to write the FIRST waveform sample
            // into the buffer. One atomic store, then never again this
            // cycle (the -1 check short-circuits subsequent calls).
            // Tells us when the Modulator HANDED the first sample to
            // the audio device — not the same as when it played on the
            // wire, but the gap between them is the diagnostic value
            // we're hunting (suspected ~1 s of device buffering).
            if (m_ic == 0 && samples != samplesEnd &&
                m_audioStartedMs.load(std::memory_order_relaxed) == -1) {
                m_audioStartedMs.store(
                    QDateTime::currentMSecsSinceEpoch(),
                    std::memory_order_relaxed);
            }
            while (samples != samplesEnd &&
                   m_ic < static_cast<unsigned>(m_ft2WaveLen)) {
                auto sample = static_cast<qint16>(std::clamp(
                    static_cast<double>(m_ft2Wave[m_ic]) * 32767.0,
                    -32767.0, 32767.0));
                samples = load(sample, samples);
                ++framesGenerated;
                ++m_ic;
                if (m_ic == 2000)
                    qCDebug(modulator_js8) << "[FT2-TX] readData: mid-waveform sample[2000]"
                               << "float=" << m_ft2Wave[2000]
                               << "qint16=" << sample;
            }
            // [AUDIO-CADENCE PROBE 2026-06-09] Capture the wall-clock
            // moment the LAST waveform sample left the Modulator. Once
            // m_ic catches up to m_ft2WaveLen, the waveform is fully
            // emitted into the buffer.
            if (m_ic >= static_cast<unsigned>(m_ft2WaveLen) &&
                m_audioEndedMs.load(std::memory_order_relaxed) == -1) {
                m_audioEndedMs.store(
                    QDateTime::currentMSecsSinceEpoch(),
                    std::memory_order_relaxed);
            }
            // [POST-ROLL 2026-07-13 TODO #72] Written silent tail —
            // m_ic keeps advancing so isFT2WaveformDone() (and thus
            // stopTx) only fires after the post-roll has been written.
            // The queued-but-unplayed span in PulseAudio's client
            // buffer is then padding, not the trailing Costas array,
            // so a server-side rewind can't destroy the frame. See
            // Modulator.h FT2_POSTROLL_FRAMES for the full rationale.
            while (samples != samplesEnd &&
                   m_ic >= static_cast<unsigned>(m_ft2WaveLen) &&
                   m_ic < static_cast<unsigned>(m_ft2WaveLen)
                              + FT2_POSTROLL_FRAMES) {
                samples = load(0, samples);
                ++framesGenerated;
                ++m_ic;
            }
            // After waveform + post-roll end, stay Active and feed
            // silence until stop() is called by stopTx(). Don't set
            // State::Idle here — that causes readData to return 0,
            // triggering QAudioSink UnderrunError which kills audio
            // after a few cycles. Fall through to silence padding
            // below.
        } else
#endif
        {
        // JS8: Fade out parameters; no fade out during tuning.

        unsigned int const i0 =
            (m_tuning ? 9999 : (JS8_NUM_SYMBOLS - 0.017) * 4.0) * m_nsps;
        unsigned int const i1 =
            (m_tuning ? 9999 : JS8_NUM_SYMBOLS * 4.0) * m_nsps;

        while (samples != samplesEnd && m_ic < i1) {
            unsigned int const isym = m_tuning ? 0 : m_ic / (4.0 * m_nsps);

            if (isym != m_isym0 || m_audioFrequency != m_audioFrequency0) {
                double const toneFrequency =
                    m_audioFrequency + itone[isym] * m_toneSpacing;

                m_dphi = TAU * toneFrequency / FRAME_RATE;
                m_isym0 = isym;
                m_audioFrequency0 = m_audioFrequency;
            }

            m_phi += m_dphi;

            if (m_phi > TAU)
                m_phi -= TAU;
            if (m_ic > i0)
                m_amp = 0.98 * m_amp;
            if (m_ic > i1)
                m_amp = 0.0;

            samples = load(qRound(m_amp * qSin(m_phi)), samples);

            ++framesGenerated;
            ++m_ic;
        }

        if (m_amp == 0.0) {
            m_state.store(State::KeepAlive);
            m_phi = 0.0;
            if (m_ft2Mode)
                emit ft2WaveformDone();
            return framesGenerated * bytesPerFrame();
        }
        } // end JS8 else block

        m_audioFrequency0 = m_audioFrequency;

        // Done for this chunk; continue on the next call. Pad the
        // block with silence.

        while (samples != samplesEnd) {
            samples = load(0, samples);
            ++framesGenerated;
        }

        return framesGenerated * bytesPerFrame();
    }
        [[fallthrough]];

    case State::KeepAlive:
        // Feed near-zero dithered samples to keep USB audio codec
        // (and PipeWire userspace) treating the stream as active.
        //
        // [KEEPALIVE-DITHER 2026-06-10 build 234]
        // Constant +1 DC samples satisfied PipeWire's userspace
        // suspend-on-idle (samples are flowing) but NOT hardware
        // codec silence-detect that counts bit transitions (constant
        // value-1 is bit-identically silent to constant zero from a
        // transition-count perspective; the device clock can get
        // suspended despite our intent).
        //
        // PipeWire's client.conf(5) docs explicitly recommend 1-2 LSB
        // of RANDOM noise (`dither.method = wannamaker3, dither.noise
        // = 2`) for this exact case: "Some devices implement their
        // own detection of silence and suspension... a workaround
        // involves adding a small amount of noise."
        //
        // 1-bit dither alternating in {-1, +1} via fast inline
        // xorshift gives a bit transition on every sample at minimum
        // amplitude (-90 dBFS, inaudible). Single-threaded (audio
        // thread only calls readData), so the static seed needs no
        // atomic. Zero allocation, ~5 ns per sample.
        {
            static uint32_t rngState = 0xCAFEBABE;
            while (samples != samplesEnd) {
                rngState ^= rngState << 13;
                rngState ^= rngState >> 17;
                rngState ^= rngState << 5;
                samples = load((rngState & 1u) ? qint16{1} : qint16{-1},
                               samples);
                ++framesGenerated;
            }
        }
        return framesGenerated * bytesPerFrame();

    case State::Idle:
        break;
    }

    Q_ASSERT(isIdle());
    return 0;
}

Q_LOGGING_CATEGORY(modulator_js8, "modulator.js8", QtWarningMsg)
