

/** \file
 * @brief member function of the UI_Constructor class
 *  process Rx queue activity
 */

#include "JS8_UI/mainwindow.h"

void UI_Constructor::processRxActivity() {
    if (m_rxActivityQueue.isEmpty()) {
        return;
    }

    int freqOffset = freq();

    qCDebug(mainwindow_js8)
        << m_messageBuffer.count() << "message buffers open";

    while (!m_rxActivityQueue.isEmpty()) {
        ActivityDetail d = m_rxActivityQueue.dequeue();

        // [reachport] The executor's per-frame watcher feed -- same
        // stream RX.ACTIVITY pushes to API clients, in process.
        if (m_reach.active)
            reachOnFrame(d);

        // [congestion] every decoded frame marks its slot occupied.
        {
            qint64 const slot =
                QDateTime::currentSecsSinceEpoch() /
                qMax(1, static_cast<int>(m_TRperiod));
            m_congestionSlots.insert(slot);
            for (auto it = m_congestionSlots.begin();
                 it != m_congestionSlots.end();) {
                if (*it < slot - 40)
                    it = m_congestionSlots.erase(it);
                else
                    ++it;
            }
        }

        if (canSendNetworkMessage()) {
            sendNetworkMessage(
                "RX.ACTIVITY", d.text,
                {{"_ID", QVariant(-1)},
                 {"FREQ", QVariant(d.dial + d.offset)},
                 {"DIAL", QVariant(d.dial)},
                 {"OFFSET", QVariant(d.offset)},
                 {"SNR", QVariant(d.snr)},
                 {"SPEED", QVariant(d.submode)},
                 {"TDRIFT", QVariant(d.tdrift)},
                 {"UTC", QVariant(d.utcTimestamp.toMSecsSinceEpoch())},
                 {"BITS", QVariant(d.bits)}});
        }

        // use the actual frequency and check its delta from our current
        // frequency meaning, if our current offset is 1502 and the d.freq is
        // 1492, the delta is <= 10;
        bool shouldDisplay =
            abs(d.offset - freqOffset) <= JS8::Submode::rxThreshold(d.submode);

        int prevOffset = d.offset;
        if (hasExistingMessageBuffer(d.submode, d.offset, false, &prevOffset) &&
            ((m_messageBuffer[prevOffset].cmd.to == m_config.my_callsign()) ||
             // (isAllCallIncluded(m_messageBuffer[prevOffset].cmd.to))     ||
             // // uncomment this if we want to incrementally print allcalls
             (isGroupCallIncluded(m_messageBuffer[prevOffset].cmd.to)) ||
             // [onfreqhdr2 2026-08-16] Overheard buffered traffic ON
             // OUR OFFSET displays incrementally exactly like to-me
             // traffic — header frame included (field: "N0JLO:
             // KB1JCU" showed in band activity while the convo got
             // orphaned payloads; the isDirected skip below killed
             // the header, and assembly — the only other display
             // path — is cleared by the station's next header).
             // Single-frame commands never open a buffer and stay
             // with processCommandActivity's gate — no duplicates.
             (std::abs(d.offset - freqOffset) <=
              JS8::Submode::rxThreshold(d.submode)))) {
            d.isBuffered = true;
            shouldDisplay = true;

            if (!m_messageBuffer[prevOffset].compound.isEmpty()) {
                // [BUILD 358 cppos] Prefer the entry that ON-AIR
                // precedes this fragment; arrival-order .last() only
                // as fallback (standard decoder / no position).
                auto const &cmpsE = m_messageBuffer[prevOffset].compound;
                int const ciE = compoundIndexBefore(cmpsE, d.absPos);
                auto lastCompound = (ciE >= 0) ? cmpsE.at(ciE)
                                               : cmpsE.last();

                // [oneprefix #176, 2026-09-06] The sender prefix
                // belongs to the frame that OPENS the display line,
                // not to every frame: this ran per frame while the
                // compound entry sat pending (it is only consumed
                // later, at buffer close), so an N-frame @GROUP
                // broadcast printed "CALL: " up to N times on one
                // line (field: "WM8Q: WM8Q: WM8Q: WM8Q: @PUBLIC",
                // "KJ5MIW: KJ5MIW: KJ5MIW: @SITREP"). Prepend ONLY
                // when no display block is open for this offset --
                // the same 10 Hz bucket triple displayTextForFreq
                // itself consults; when the line is already open
                // (usually by the compound frame's own display), the
                // sender is already on it.
                int const lowKey = d.offset / 10 * 10;
                bool const lineOpen =
                    m_rxFrameBlockNumbers.contains(d.offset) ||
                    m_rxFrameBlockNumbers.contains(lowKey) ||
                    m_rxFrameBlockNumbers.contains(lowKey + 10);
                if (!lineOpen) {
                    d.text = QString("%1: %2")
                                 .arg(lastCompound.call)
                                 .arg(d.text);
                }
                d.utcTimestamp =
                    qMin(d.utcTimestamp, lastCompound.utcTimestamp);
            }

        } else if (hasClosedExistingMessageBuffer(d.offset)) {
            // incremental typeahead should just be displayed...
            // TODO: should the buffer be reopened?
            shouldDisplay = true;

        } else if (d.isDirected) {
            // All directed messages (commands, SNR, heartbeats, partials)
            // are displayed by processCommandActivity. Skip here to
            // prevent duplicate display.
            continue;
        }

        // if this is the first data frame of a standard message, parse the
        // first word callsigns and spot them :)
        if ((d.bits & Varicode::JS8CallFirst) == Varicode::JS8CallFirst &&
            !d.isDirected && !d.isCompound) {

            QString theirCall;

            auto calls = Varicode::parseCallsigns(d.text);
            if (!calls.isEmpty()) {
                auto call = calls.first();
                if (d.text.startsWith(call) &&
                    d.text.mid(call.length(), 1) == ":")
                    theirCall = call;
            }

            if (!theirCall.isEmpty()) {
                CallDetail cd = {};
                cd.call = theirCall;
                cd.dial = d.dial;
                cd.offset = d.offset;
                cd.snr = d.snr;
                cd.bits = d.bits;
                cd.tdrift = d.tdrift;
                cd.utcTimestamp = d.utcTimestamp;
                cd.submode = d.submode;
                logCallActivity(cd, true);
            }
        }

        // TODO: incremental printing of directed messages
        // Display if:
        // 1) this is a directed message header "to" us and should be
        // buffered... 2) or, this is a buffered message frame for a buffer with
        // us as the recipient.

        if (!shouldDisplay) {
            continue;
        }

        bool isFirst =
            (d.bits & Varicode::JS8CallFirst) == Varicode::JS8CallFirst;
        bool isLast = (d.bits & Varicode::JS8CallLast) == Varicode::JS8CallLast;

        // if we're the last message, let's display our EOT character
        if (isLast) {
            d.text = QString("%1 %2 ")
                         .arg(Varicode::rstrip(d.text))
                         .arg(m_config.eot());
        }

        // log it to the display!
        displayTextForFreq(d.text, d.offset, d.utcTimestamp, false, isFirst,
                           isLast, d.submode);

        // Only cancel CQ loop if message is directed to us specifically
        if (m_cq_loop->isActive() && d.isDirected &&
            d.text.contains(m_config.my_callsign())) {
            qCDebug(mainwindow_js8)
                << "Canceling CQ loop: message directed to us.";
            m_cq_loop->onLoopCancel();
        }

        if (isLast) {
            clearOffsetDirected(d.offset);
        }

        if (isLast && !d.isBuffered) {
            // buffered commands need the rxFrameBlockNumbers cache so it can
            // fixup its display all other "last" data frames can clear the
            // rxFrameBlockNumbers cache so the next message will be on a new
            // line.
            m_rxFrameBlockNumbers.remove(d.offset);
        }
    }

#if 0
    // TODO: this works but should also print in the rx window.
    foreach(auto offset, m_bandActivity.keys()){
        if(seen.contains(offset)){
            continue;
        }

        if(m_bandActivity[offset].isEmpty()){
            continue;
        }

        auto last = m_bandActivity[offset].last();
        if((last.bits & Varicode::JS8CallLast) == Varicode::JS8CallLast){
            continue;
        }

        auto now = DriftingDateTime::currentDateTimeUtc();
        if(last.utcTimestamp.secsTo(now) < m_TRperiod){
            continue;
        }

        ActivityDetail d = {};
        d.text = " . . . ";
        d.utcTimestamp = now;
        d.snr = -99;

        m_bandActivity[offset].append(d);
    }
#endif
}
