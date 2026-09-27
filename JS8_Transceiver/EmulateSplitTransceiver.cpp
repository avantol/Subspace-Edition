/**
 * @file DXLabSuiteCommanderTransceiver.cpp
 * @brief Implementation of the DXLabSuiteCommanderTransceiver class
 * Helper decorator class that encapsulates  the emulation of split TX
 * operation.
 */

#include "EmulateSplitTransceiver.h"

#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(emulatesplittransceiver_js8)

EmulateSplitTransceiver::EmulateSplitTransceiver(
    std::unique_ptr<Transceiver> wrapped, QObject *parent)
    : Transceiver{parent}, wrapped_{std::move(wrapped)}, rx_frequency_{0},
      tx_frequency_{0}, split_{false} {
    // Connect update signal of wrapped Transceiver object instance to ours.
    connect(wrapped_.get(), &Transceiver::update, this,
            &EmulateSplitTransceiver::handle_update);

    // Connect other signals of wrapped Transceiver object to our
    // parent matching signals.
    connect(wrapped_.get(), &Transceiver::resolution, this,
            &Transceiver::resolution);
    connect(wrapped_.get(), &Transceiver::finished, this,
            &Transceiver::finished);
    connect(wrapped_.get(), &Transceiver::failure, this, &Transceiver::failure);
}

void EmulateSplitTransceiver::set(TransceiverState const &s,
                                  unsigned sequence_number) noexcept {
    qCDebug(emulatesplittransceiver_js8)
        << "EmulateSplitTransceiver::set: state:" << s
        << "#:" << sequence_number;

    // save for use in updates
    rx_frequency_ = s.frequency();
    tx_frequency_ = s.tx_frequency();
    split_ = s.split();

    TransceiverState emulated_state{s};
    if (s.ptt() && split_)
        emulated_state.frequency(s.tx_frequency());
    emulated_state.split(false);
    emulated_state.tx_frequency(0);
    wrapped_->set(emulated_state, sequence_number);
}

void EmulateSplitTransceiver::handle_update(TransceiverState const &state,
                                            unsigned sequence_number) {
    qCDebug(emulatesplittransceiver_js8)
        << "EmulateSplitTransceiver::handle_update: from wrapped:" << state;

    if (state.split()) {
        Q_EMIT failure(
            tr("Emulated split mode requires rig to be in simplex mode"));
    } else {
        TransceiverState new_state{state};
        // Follow the rig if in RX mode.
        if (state.ptt())
            new_state.frequency(rx_frequency_);
        // [#277 2026-09-26, operator] AND FOR AS LONG AS THE RIG STILL
        // REPORTS THE TX DIAL AFTER PTT DROPS. Emulated split moves the
        // rig's one VFO for transmit and moves it back afterwards, and
        // that restore is a CAT write: the rig can report ptt=false
        // while physically still on the transmit frequency. The line
        // above stops substituting the moment ptt clears, so such a
        // reading would reach the main window as a genuine dial change.
        // Not fixed with a settling delay: the duration belongs to the
        // radio and no honest constant exists for it. Not fixed by
        // confirming across two polls: both reads agree, because the
        // transmit dial is genuinely where the rig is. Fixed by
        // RECOGNISING THE VALUE -- this class asked for that frequency
        // itself and still holds both sides of the swap, so the tail of
        // the artifact is identifiable with no new state and no timer.
        // Cost, accepted: an operator who tunes DELIBERATELY to exactly
        // the emulated transmit dial (a few hundred Hz off the operating
        // frequency, and only while emulated split is in use) is not
        // followed until the dial moves off that value again.
        //
        // [#277 2026-09-27, HONEST CORRECTION] THIS BRANCH HAS NEVER
        // BEEN OBSERVED TO FIRE. It is a guard for a slow CAT restore,
        // and it is kept as one. The 2026-09-26 field capture it was
        // written for had no ptt in the record, so it was ASSUMED to be
        // this window; the same symptom captured WITH ptt on 2026-09-27
        // showed ptt still TRUE, which is a different window entirely
        // and is closed in UI_Constructor::operatingDial(). The earlier
        // claim here that m_freqNominal absorbs the stale value is also
        // withdrawn: the 09-27 record shows the nominal staying correct,
        // because the unkeyed branch of the rig-state handler reads the
        // frequency this class has already substituted.
        else if (split_ && tx_frequency_ && rx_frequency_ != tx_frequency_ &&
                 state.frequency() == tx_frequency_)
            new_state.frequency(rx_frequency_);

        // These are always what was requested in prior set state operation
        new_state.tx_frequency(tx_frequency_);
        new_state.split(split_);

        qCDebug(emulatesplittransceiver_js8)
            << "EmulateSplitTransceiver::handle_update: signalling:" << state;

        // signal emulated state
        Q_EMIT update(new_state, sequence_number);
    }
}

Q_LOGGING_CATEGORY(emulatesplittransceiver_js8, "emulatesplittransceiver.js8",
                   QtWarningMsg)
