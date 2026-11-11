/**
 * @file TransceiverBase.cpp
 * @brief Implementation of the TransceiverBase class
 * Implements the Transceiver abstract  interface as template methods and
 * provides  a new abstract interface  with similar functionality
 */

#include "TransceiverBase.h"

#include <QDebug>
#include <QString>
#include <QThread>
#include <QTimer>

#include <exception>

#include "moc_TransceiverBase.cpp"

namespace {
auto const unexpected = TransceiverBase::tr("Unexpected rig error");
}

void TransceiverBase::start(unsigned sequence_number) noexcept {
    QString message;
    try {
        may_update u{this, true};
        shutdown();
        startup();
        last_sequence_number_ = sequence_number;
    } catch (std::exception const &e) {
        message = e.what();
    } catch (...) {
        message = unexpected;
    }
    if (!message.isEmpty()) {
        offline(message);
    }
}

void TransceiverBase::set(TransceiverState const &s,
                          unsigned sequence_number) noexcept {
    TRACE_CAT("TransceiverBase", "#:" << sequence_number << s);

    QString message;
    try {
        may_update u{this, true};
        // [TODO #316] whether a phone is connected: read before the PTT
        // change below, so a key-up already knows
        requested_.want_meters(s.want_meters());
        bool was_online{requested_.online()};
        if (!s.online() && was_online) {
            shutdown();
        } else if (s.online() && !was_online) {
            shutdown();
            startup();
        }
        if (requested_.online()) {
            // [ownptt 2026-11-11] requested_.ptt() is the PTT we commanded,
            // never the PTT read from the radio, which includes other
            // programs' transmissions on a shared radio (inherited code
            // copied the reading in at the end of every set()). W8UFO
            // 10 Nov: VarAC keyed 185 ms before our slot and our PTT on
            // looked like no change, so none was sent (19:48:44); a set()
            // during CommsPlan's tune left "on" behind and our next frame
            // sent PTT off first, then keyed 284 ms late (22:08:44).
            //   on:  we want it and have not commanded it, or the radio
            //        reads not keyed (something else dropped it: Hold PTT
            //        keys again at the next frame, as before)
            //   off: only a PTT on we commanded, so we never un-key
            //        another program's transmission
            bool const ptt_on{s.ptt() &&
                              (!requested_.ptt() || !actual_.ptt())};
            bool const ptt_off{!s.ptt() && requested_.ptt()};
            if (ptt_off) {
                do_ptt(false);
                do_post_ptt(false);
                QThread::msleep(100); // some rigs cannot process CAT
                                      // commands while switching from
                                      // Tx to Rx
            }
            if (s.frequency() // ignore bogus zero frequencies
                && ((s.frequency() != requested_.frequency() // and QSY
                     || (s.mode() != UNK &&
                         s.mode() != requested_.mode())) // or mode change
                    || ptt_off)) // or just returned to rx
            {
                do_frequency(s.frequency(), s.mode(), ptt_off);
                do_post_frequency(s.frequency(), s.mode());

                // record what actually changed
                requested_.frequency(actual_.frequency());
                requested_.mode(actual_.mode());
            }
            if (!s.tx_frequency() ||
                (s.tx_frequency() > 10000 // ignore bogus startup values
                 && s.tx_frequency() <
                        std::numeric_limits<Frequency>::max() - 10000)) {
                if ((s.tx_frequency() != requested_.tx_frequency() // and QSY
                     || (s.mode() != UNK &&
                         s.mode() != requested_.mode())) // or mode change
                    // || s.split () != requested_.split ())) // or split change
                    || (s.tx_frequency() && ptt_on)) // or about to tx split
                {
                    hamlib_bug_bandaid(s);
                    do_tx_frequency(s.tx_frequency(), s.mode(), ptt_on);
                    do_post_tx_frequency(s.tx_frequency(), s.mode());

                    // record what actually changed
                    requested_.tx_frequency(actual_.tx_frequency());
                    requested_.split(actual_.split());
                }
            }
            if (ptt_on) {
                do_ptt(true);
                do_post_ptt(true);
                QThread::msleep(100); // some rigs cannot process CAT
                                      // commands while switching from
                                      // Rx to Tx
            }

            // [ownptt] record what we commanded, only when we commanded it
            if (ptt_on || ptt_off)
                requested_.ptt(s.ptt());
        }
        last_sequence_number_ = sequence_number;
    } catch (std::exception const &e) {
        message = e.what();
    } catch (...) {
        message = unexpected;
    }
    if (!message.isEmpty()) {
        offline(message);
    }
}

void TransceiverBase::startup() {
    Q_EMIT resolution(do_start());
    do_post_start();
    actual_.online(true);
    requested_.online(true);
}

void TransceiverBase::shutdown() {
    may_update u{this};
    if (requested_.online()) {
        try {
            // try and ensure PTT isn't left set
            do_ptt(false);
            do_post_ptt(false);
            if (requested_.split()) {
                // try and reset split mode
                do_tx_frequency(0, UNK, true);
                do_post_tx_frequency(0, UNK);
            }
        } catch (...) {
            // don't care about exceptions
        }
    }
    do_stop();
    do_post_stop();
    actual_.online(false);
    requested_.online(false);
    // [ownptt] the PTT off above was sent: nothing of ours is keyed, so the
    // first PTT on after a re-open is a change (from K7RIE's 11 Nov patch)
    requested_.ptt(false);
}

void TransceiverBase::stop() noexcept {
    QString message;
    try {
        shutdown();
    } catch (std::exception const &e) {
        message = e.what();
    } catch (...) {
        message = unexpected;
    }
    if (!message.isEmpty()) {
        offline(message);
    } else {
        Q_EMIT finished();
    }
}

void TransceiverBase::update_rx_frequency(Frequency rx) {
    actual_.frequency(rx);
    requested_.frequency(rx); // track rig changes
}

void TransceiverBase::update_other_frequency(Frequency tx) {
    actual_.tx_frequency(tx);
}

void TransceiverBase::update_split(bool state) { actual_.split(state); }

void TransceiverBase::update_mode(MODE m) {
    actual_.mode(m);
    requested_.mode(m); // track rig changes
}

void TransceiverBase::update_PTT(bool state) {
    if (state && !actual_.ptt())
        actual_.meters(-1.f, -1.f); // [TODO #316] a new transmission
    actual_.ptt(state);
}

void TransceiverBase::update_meters(float watts, float swr) {
    actual_.meters(watts, swr);
}

void TransceiverBase::update_complete(bool force_signal) {
    if ((do_pre_update() &&
         (actual_ != last_ || !actual_.same_meters(last_))) ||
        force_signal) {
        Q_EMIT update(actual_, last_sequence_number_);
        last_ = actual_;
    }
}

void TransceiverBase::offline(QString const &reason) {
    Q_EMIT failure(reason);
    try {
        shutdown();
    } catch (...) {
        // don't care
    }
}

Q_LOGGING_CATEGORY(transceiverbase_js8, "transceiverbase.js8", QtWarningMsg)
