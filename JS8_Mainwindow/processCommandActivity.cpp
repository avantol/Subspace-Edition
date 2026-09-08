

/** \file
 * @brief member function of the UI_Constructor class
 *  processes JS8 commands
 */

#include "JS8_UI/mainwindow.h"
#include "JS8_Main/ArqMonitor.h"

#include "JS8_Main/ChunkedArq.h"
#include "JS8_Main/NativeBinary.h"

#include <QPointer>
#include <QRandomGenerator>
#include <QTimer>

void UI_Constructor::processCommandActivity() {
    // [manualask] Expiry sweep, opportunistic on the decode pass (no
    // timer): a hand-sent relay ask with no observed forward inside
    // the settle window becomes a journaled failure -- the absence
    // finally has a deadline-keeper, so manual probes feed the
    // relay-status flag exactly like executor attempts (operator,
    // 2026-08-29: KK7UMM declined two manual asks invisibly).
    if (!m_manualAsks.isEmpty() && m_spotMapWindow) {
        qint64 const nowMs = DriftingDateTime::currentMSecsSinceEpoch();
        for (int i = m_manualAsks.size() - 1; i >= 0; --i) {
            if (nowMs - m_manualAsks.at(i).askedMs
                    < kManualAskSettleMs)
                continue;
            m_spotMapWindow->queueReachEvent(
                {m_config.bands()->find(dialFrequency()),
                 m_manualAsks.at(i).via.toUpper(),
                 QStringLiteral("fwd"), QString{},
                 m_manualAsks.at(i).askedMs / 1000, false});
            qCWarning(mainwindow_js8)
                << "[MANUALASK] no forward from"
                << m_manualAsks.at(i).via
                << "within the settle window -- journaled";
            m_manualAsks.removeAt(i);
        }
    }
#if 0
    if (!m_txFrameQueue.isEmpty()) {
        return;
    }
#endif

    if (m_rxCommandQueue.isEmpty()) {
        return;
    }

#if 0
    bool processed = false;

    int f = currentFreq();
#endif

    auto now = DriftingDateTime::currentDateTimeUtc();

    while (!m_rxCommandQueue.isEmpty()) {
        auto d = m_rxCommandQueue.dequeue();

        auto selectedCallsign = callsignSelected();
        bool isAllCall = isAllCallIncluded(d.to);
        // @APRSIS is bridging traffic, not user-addressed. Don't
        // route to the conversation panel unless the user has
        // explicitly added @APRSIS to their MyGroups. The actual
        // APRS-IS bridging logic (spotAprsCmd / spotAprsGrid / the
        // MSG TO: relay paths later in this file) runs independently
        // of this flag and continues to function regardless.
        bool isGroupCall = isGroupCallIncluded(d.to);

        qCDebug(mainwindow_js8)
            << "try processing command" << d.from << d.to << d.cmd << d.dial
            << d.offset << d.grid << d.extra << isAllCall << isGroupCall;

        // if we need a compound callsign but never got one...skip
        if (d.from == "<....>" || d.to == "<....>") {
            continue;
        }

        // is this to me? Exact match only — WM8Q/P is a different station than WM8Q
        bool toMe = d.to == m_config.my_callsign().trimmed();

        // log call activity BEFORE the command-allowed gate so even
        // unrecognized directed commands refresh the sender's SNR in the
        // callsign list. Gating was rejecting valid SNR updates for
        // any directed message whose cmd wasn't in the known-commands set.
        CallDetail cd = {};
        cd.call = d.from;
        cd.grid = d.grid;
        cd.snr = d.snr;
        cd.dial = d.dial;
        cd.offset = d.offset;
        cd.bits = d.bits;
        cd.ackTimestamp =
            d.text.contains(": ACK") || toMe ? d.utcTimestamp : QDateTime{};
        cd.utcTimestamp = d.utcTimestamp;
        cd.tdrift = d.tdrift;
        cd.submode = d.submode;
        logCallActivity(cd, true);
        logHeardGraph(d.from, d.to,
                      Varicode::isCommandReceptionEvidence(d.cmd));

        // [hearlines] Feed the Spots Map's on-air heard-mesh (blue
        // lines). Runs BEFORE the command-allowed gate so EVERY
        // queued frame contributes (field 2026-08-15: HB-SNR replies
        // fed edges only pre-gate — N01ZE case). GRID replies are
        // order-independent: the grid is parsed from THIS message's
        // text directly (AC7WY sequence, 2026-08-14). Rationale (operator 2026-08-14): works with no
        // internet, and some stations never report to PSK Reporter —
        // on-air evidence is the only mesh they appear in. Sources:
        //   1. every directed exchange: "A: B ..." = A hears/works B;
        //   2. HEARING replies (below), incl. the relayed
        //      "... *DE* CALL" form where the trailing DE call is the
        //      true hearer.
        auto const hearGridFor = [this](QString const &call) -> QString {
            if (call.compare(m_config.my_callsign().trimmed(),
                             Qt::CaseInsensitive) == 0)
                return m_config.my_grid();
            QString g = m_callActivity.value(call).grid;
            // [#164] The grid authority (seeded from the persistent
            // SQLite store at startup) sits between session activity
            // and the logbook in the fallback chain.
            if (g.isEmpty() && m_spotMapWindow)
                g = m_spotMapWindow->knownGrid(call);
            if (g.isEmpty()) {
                // [gridfall] Same fallback the Call Activity list
                // uses: the LOGBOOK's grid for previously-worked
                // stations (operator 2026-08-14 — session call
                // activity is empty for a station that hasn't sent a
                // grid since the last restart).
                QString date, name, comment;
                m_logBook.findCallDetails(call, g, date, name, comment);
                g = g.trimmed();
            }
            return g;
        };
        auto const feedHearing = [&](QString const &hearer,
                                     QStringList const &heard,
                                     int reportedToMeSnr = -99) {
            if (!m_spotMapWindow || hearer.isEmpty())
                return;
            QString const band = m_config.bands()->find(
                static_cast<Radio::Frequency>(d.dial));
            QStringList grids;
            for (QString const &c : heard)
                grids << hearGridFor(c);
            // [gridpolicy 2026-08-15] SANCTIONED grid sources ONLY —
            // the grid must be explicit CONTENT of a grid-bearing
            // message: GRID replies, plain CQs ("CQ CQ CQ DM03"),
            // heartbeats ("HEARTBEAT EN35"). The frame's grid EXTRA
            // field (ping/lead metadata, arrives as " EM38") is NOT
            // trusted (operator directive; W0MQD case). Relayed GRID
            // replies are handled separately below with *DE*
            // attribution. Fallback = the callsign-list/logbook
            // lookup the operator specified. Trailing grid wins.
            QString hearerGrid;
            if (d.cmd == QStringLiteral(" GRID") ||
                d.cmd.startsWith(QStringLiteral(" CQ")) ||
                d.cmd.startsWith(QStringLiteral(" HEARTBEAT")) ||
                d.cmd == QStringLiteral(" HB")) {
                if (auto const gs = Varicode::parseGrids(d.text);
                    !gs.isEmpty())
                    hearerGrid = gs.last();
            }
            if (hearerGrid.isEmpty())
                hearerGrid = hearGridFor(hearer);
            m_spotMapWindow->addHearingReport(band, hearer, hearerGrid,
                                              heard, grids,
                                              reportedToMeSnr);
        };
        // [hbdots] PRESENCE for every on-air sender — heartbeats and
        // all other frames put a hollow dot at the station's grid in
        // the All view (65 HBs with grids went unused in one evening,
        // log 2026-08-14).
        feedHearing(d.from, {});
        // [myears] OUR OWN reception is the most authoritative heard
        // report of all: every frame we decode = an edge ME -> sender
        // (field 2026-08-15: WD5EED decoded here, no line from us).
        // The target's position comes from its own presence dot, so
        // no grid is attached to the edge.
        if (m_spotMapWindow) {
            QString const myC = m_config.my_callsign().trimmed();
            if (!myC.isEmpty() &&
                d.from.compare(myC, Qt::CaseInsensitive) != 0) {
                // [#168] Record HOW WELL we heard it, not just that
                // we did -- see the sibling site in mainwindow.cpp.
                m_spotMapWindow->addHearingReport(
                    m_config.bands()->find(
                        static_cast<Radio::Frequency>(d.dial)),
                    myC, m_config.my_grid(), {d.from}, {QString()},
                    /*reportedToMeSnr=*/-99, QDateTime{},
                    /*heardSnr=*/d.snr);
            }
        }
        // [#167 2026-08-21] Mesh edge A->B ONLY when this frame is
        // evidence A actually received B. Was: every directed frame,
        // which painted a link for every unanswered probe.
        if (!d.to.startsWith(QLatin1Char('@')) &&
            Radio::is_callsign(d.to) &&
            Varicode::isCommandReceptionEvidence(d.cmd)) {
            feedHearing(d.from, {d.to});
        }
        // [#167(d)] OVERHEARD RELAY IN FLIGHT is evidence the app was
        // throwing away. "A: X> payload *DE* C" means A is forwarding
        // C's traffic, so A DECODED C -- a real A->C edge, and one of
        // the few ways a link between two distant stations becomes
        // visible to us at all. Note the direction: the edge is to the
        // *DE* ORIGINATOR, never to the destination X (that pairing is
        // exactly the phantom this todo removes). The *DE* tail is
        // otherwise consumed only for HEARING/GRID attribution below.
        if (d.cmd == QStringLiteral(">") && !d.from.isEmpty()) {
            static QRegularExpression const kDeFromRe(
                QStringLiteral(R"(\*DE\*\s+(?<de>[A-Z0-9/]+))"),
                QRegularExpression::CaseInsensitiveOption);
            if (auto const m = kDeFromRe.match(d.text); m.hasMatch()) {
                QString const de =
                    m.captured(QStringLiteral("de")).toUpper();
                if (Radio::is_callsign(de) &&
                    de.compare(d.from, Qt::CaseInsensitive) != 0)
                    feedHearing(d.from, {de});
                // [fwdjournal 2026-08-27] A station observed
                // forwarding OUR traffic is a forward outcome
                // whoever asked -- the operator's manual K0EMP relay
                // proved the executor-only journaling was discarding
                // real habit data. Journal it always.
                if (m_spotMapWindow &&
                    Radio::same_station(
                        de, m_config.my_callsign().trimmed())) {
                    m_spotMapWindow->queueReachEvent(
                        {m_config.bands()->find(dialFrequency()),
                         d.from.toUpper(), QStringLiteral("fwd"),
                         QString{}, 0, true});
                    // [manualask] The success this ask was waiting
                    // for: clear its pending deadline(s).
                    for (int i = m_manualAsks.size() - 1; i >= 0; --i)
                        if (Radio::same_station(
                                m_manualAsks.at(i).via, d.from))
                            m_manualAsks.removeAt(i);
                }
            }
        }
        // [onairspot] A frame addressed TO ME proves the sender hears
        // me — feed the map's MY view (the offline/PSKR-less
        // equivalent of a reception report). SNR replies carry the
        // actual report of MY signal; other frames are position-only.
        if (m_spotMapWindow && toMe) {
            int reportedSnr = -99;
            // [snrextra] HB ACKs report my signal too ("ACK -12"):
            // the signed value rides in EXTRA. Subspace ARQ chunk
            // acks ("ACK 1") carry their number in d.text with EXTRA
            // empty, so parsing extra alone cannot mistake a chunk
            // number for a dB value (field 2026-08-15, K6NLX).
            if (d.cmd == QStringLiteral(" ACK") ||
                d.cmd == QStringLiteral(" HEARTBEAT SNR")) {
                bool ok = false;
                if (int const v = d.extra.trimmed().toInt(&ok); ok)
                    reportedSnr = v;
            }
            if (d.cmd == QStringLiteral(" SNR")) {
                // [snrwho] The SNR value rides in the frame's EXTRA
                // field ("+06" via formatSNR) — d.text is the BODY
                // ONLY and is empty for SNR replies, so the old
                // d.text parse NEVER succeeded (field 2026-08-15:
                // fresh AC7WY/KJ7VWV replies hovered as "no signal
                // report"). Text parse kept as fallback.
                bool ok = false;
                int v = d.extra.trimmed().toInt(&ok);
                if (!ok)
                    v = d.text.trimmed()
                            .section(QLatin1Char(' '), 0, 0)
                            .toInt(&ok);
                if (ok)
                    reportedSnr = v;
            }
            m_spotMapWindow->addOnAirSpotOfMe(
                m_config.bands()->find(
                    static_cast<Radio::Frequency>(d.dial)),
                d.from, hearGridFor(d.from), reportedSnr);
            // [hbdots] The reported value also colors this station's
            // All-view dot (the ONLY sanctioned color source there).
            if (reportedSnr > -99)
                feedHearing(d.from, {}, reportedSnr);
        }

        // [gridpolicy] Relayed GRID replies ("A: B> GRID DN61 *DE* C")
        // — the grid belongs to the *DE* originator (else the frame
        // sender). Sanctioned source per the operator's grid policy.
        if (d.cmd == QStringLiteral(">")) {
            QString const rt = d.text.trimmed();
            if (rt.toUpper().startsWith(QStringLiteral("GRID"))) {
                QString gridOwner = d.from;
                QString gridPart = rt;
                static QRegularExpression const kDeTail2Re(
                    QStringLiteral(R"(\*DE\*\s+(?<de>[A-Z0-9/]+))"),
                    QRegularExpression::CaseInsensitiveOption);
                if (auto const m = kDeTail2Re.match(rt); m.hasMatch()) {
                    gridOwner = m.captured("de").toUpper();
                    gridPart = rt.left(m.capturedStart());
                }
                if (auto const gs = Varicode::parseGrids(gridPart);
                    !gs.isEmpty() && m_spotMapWindow) {
                    m_spotMapWindow->addHearingReport(
                        m_config.bands()->find(
                            static_cast<Radio::Frequency>(d.dial)),
                        gridOwner, gs.last(), {}, {});
                }
            }
        }

        // HEARING payloads: plain (" HEARING") or riding a relay
        // reply (cmd ">", text "HEARING ... *DE* CALL"). The DE tail
        // names the true hearer; without it the frame's sender is.
        {
            QString hearText;
            if (d.cmd == QStringLiteral(" HEARING")) {
                hearText = d.text;
            } else if (d.cmd == QStringLiteral(">")) {
                // A relayed result can still carry nested relay heads
                // ("KJ7VWV> HEARING ... *DE* X" behind a multi-hop
                // path) — strip every leading "CALL>" token before
                // testing for the HEARING payload.
                QString rt = d.text.trimmed();
                static QRegularExpression const kRelayHeadRe(
                    QStringLiteral(R"(^[A-Z0-9/]+>\s*)"),
                    QRegularExpression::CaseInsensitiveOption);
                while (true) {
                    auto const m = kRelayHeadRe.match(rt);
                    if (!m.hasMatch())
                        break;
                    rt = rt.mid(m.capturedLength());
                }
                // Overheard INTERMEDIATE hop: the payload is
                // "DEST HEARING ..." — bare destination call, no '>'
                // and no *DE* yet (field 2026-08-15: AC7WY's
                // KI4HDU/K1KWC list lost when its final delivery
                // never arrived). The frame's sender is the hearer.
                static QRegularExpression const kDestHeadRe(
                    QStringLiteral(R"(^(?<dest>[A-Z0-9/]+)\s+HEARING)"),
                    QRegularExpression::CaseInsensitiveOption);
                if (auto const dm = kDestHeadRe.match(rt); dm.hasMatch())
                    rt = rt.mid(dm.capturedEnd("dest")).trimmed();
                if (rt.toUpper().startsWith(QStringLiteral("HEARING")))
                    hearText = rt.mid(7); // drop "HEARING"
                // [#161 querycall] Relayed QUERY CALL reply: after
                // head-stripping, "YES +08 (15S) *DE* KJ7VWV" — the
                // *DE* names the true responder (the askee).
                else if (toMe &&
                         rt.toUpper().startsWith(
                             QStringLiteral("YES "))) {
                    static QRegularExpression const kYesDeRe{
                        QStringLiteral(
                            R"(^YES\s+(?<body>.+?)\s*\*DE\*\s+(?<de>[A-Z0-9/]+)\s*$)"),
                        QRegularExpression::CaseInsensitiveOption};
                    if (auto const ym = kYesDeRe.match(rt.trimmed());
                        ym.hasMatch())
                        bindCallQueryReply(
                            ym.captured(QStringLiteral("de")).toUpper(),
                            ym.captured(QStringLiteral("body")),
                            d.dial);
                }
            }
            if (!hearText.isEmpty()) {
                QString hearer = d.from;
                QString listPart = hearText;
                static QRegularExpression const kDeTailRe(
                    QStringLiteral(
                        R"(\*DE\*\s+(?<de>[A-Z0-9/]+))"),
                    QRegularExpression::CaseInsensitiveOption);
                if (auto const m = kDeTailRe.match(hearText);
                    m.hasMatch()) {
                    hearer = m.captured("de").toUpper();
                    listPart = hearText.left(m.capturedStart());
                }
                QStringList heard =
                    Varicode::parseCallsigns(listPart);
                heard.removeAll(hearer);
                if (!heard.isEmpty())
                    feedHearing(hearer, heard);
            }
        }




        // we're only processing a subset of queries at this point
        if (!Varicode::isCommandAllowed(d.cmd)) {
            continue;
        }

        // Placeholder for callbacks to be passed to confirmThenEnqueueMessage
        // and enqueueMessage
        Callback callback = nullptr;

        // PROCESS BUFFERED HEARING FOR EVERYONE
        if (d.cmd == " HEARING") {
            // 1. parse callsigns
            // 2. log it to the heard graph
            auto calls = Varicode::parseCallsigns(d.text);
            foreach (auto call, calls) {
                // [#167] A HEARING list is the station's own report of
                // what it copies -- evidence by definition.
                logHeardGraph(d.from, call, true);
            }
        }

        // PROCESS BUFFERED GRID FOR EVERYONE
        if (d.cmd == " GRID") {
            // 1. parse grids
            // 2. log it to our call activity
            auto grids = Varicode::parseGrids(d.text);
            foreach (auto grid, grids) {
                CallDetail cd = {};
                cd.bits = d.bits;
                cd.call = d.from;
                cd.dial = d.dial;
                cd.offset = d.offset;
                cd.grid = grid;
                cd.snr = d.snr;
                cd.utcTimestamp = d.utcTimestamp;
                cd.tdrift = d.tdrift;
                cd.submode = d.submode;

                // PROCESS GRID SPOTS TO APRSIS FOR EVERYONE
                /**
                 * APRS relay: inbound `MSG TO:` stores for our local station.
                 * Dedupe within 60s using TO + TEXT + DE <SENDER>.
                 */
                if (d.to == "@APRSIS") {

                    spotAprsGrid(cd.dial, cd.offset, cd.snr, cd.call, cd.grid);
                }

                logCallActivity(cd, true);
            }
        }

        // PROCESS @JS8NET, @APRSIS, AND OTHER GROUP SPOTS FOR EVERYONE
        if (d.to.startsWith("@")) {
            spotCmd(d);
        }

        // PROCESS @APRSIS CMD SPOTS FOR EVERYONE
        if (d.to == "@APRSIS") {
            spotAprsCmd(d);
        }



        // PREPARE CMD TEXT
        // Build baseText without the FROM prefix: "TO CMD EXTRA TEXT [eot]"
        // This is used for the TCP API where FROM is sent as a separate field.
        // The full text (with FROM prefix) is used for file logging and display.
        //
        // Build 152: when d.cmd is the literal-space "freetext follows"
        // marker (directed_cmds[" "] == 31), `d.to + d.cmd` already ends
        // with a trailing space, and the trailing baseList.join(" ") below
        // would add another separator, producing "WM8Q  THIS IS A MSG"
        // (doubled). For whitespace-only cmds, drop d.cmd from the head and
        // let join provide the single separator. Real cmds (" SNR",
        // " ACK", "?", ">", etc.) keep their existing concat — for "?" and
        // ">" the result is still "WM8Q?" / "WM8Q>" with no separator,
        // matching prior behavior. Same pattern as the directed-frame fix
        // in DecodedText::tryUnpackDirected (Build 147) and the
        // heartbeat-alt fix in tryUnpackHeartbeat (Build 148).
        QStringList baseList;
        if (d.cmd.trimmed().isEmpty()) {
            baseList << d.to;
        } else {
            baseList << QString("%1%2").arg(d.to).arg(d.cmd);
        }

        if (!d.extra.isEmpty()) {
            baseList.append(d.extra);
        }

        if (!d.text.isEmpty()) {
            baseList.append(d.text);
        }

        QString baseText = baseList.join(" ");

        // Append the user-configured eot (end of transmission) character
        // if this is the final frame of the message
        bool isLast = (d.bits & Varicode::JS8CallLast) == Varicode::JS8CallLast;
        if (isLast) {
            baseText = QString("%1 %2 ")
                           .arg(Varicode::rstrip(baseText))
                           .arg(m_config.eot());
        }

        // Prepend FROM for file logging and UI display: "FROM: TO CMD EXTRA TEXT [eot]"
        QString text = QString("%1: %2").arg(d.from).arg(baseText);

        // Log to DIRECTED.txt (includes FROM prefix)
        writeMsgTxt(text, d.snr, d.offset);

        // [stamon] Station-monitor tee: the same fully-assembled
        // line, with the parties still separable. No-op when no
        // monitor window is open.
        if (!m_stationMonitors.isEmpty())
            feedStationMonitors(d.from, d.to, d.relayPath, text,
                                d.offset, d.utcTimestamp, d.submode);

        // [reachport] The executor's assembled-message feed -- the
        // canonical text, before every reply gate. YES answers reach
        // the hearing store via bindCallQueryReply on this same pass;
        // the executor's router reads the store live.
        if (m_reach.active)
            reachOnDirected(d, text);

        // [attemptviz 2026-08-22] Somebody answered US -- green line.
        // Gate on "addressed to me", not on matching a live attempt:
        // a broadcast sweep draws no red path but its responders are
        // answering us just the same, and HB replies count too
        // (operator's rule). Directed traffic between OTHER stations
        // is deliberately excluded -- greening the whole band would
        // say nothing about our own calls.
        //
        // The TO field can arrive carrying the relay marker -- a chain
        // reply reads "WM8Q> SNR +00 *DE* KL7UT *DE* KB7ITU", and
        // Radio::base_callsign only strips '/', never '>', so a plain
        // same_station() compare misses it. That is why the successful
        // KL7UT chain reply drew no green line (operator, 2026-08-22:
        // "was it the '>' format?"). Strip the marker before comparing.
        if (m_spotMapWindow) {
            QString dest = d.to.trimmed();
            while (dest.endsWith('>'))
                dest.chop(1);
            if (Radio::same_station(dest, m_config.my_callsign()) &&
                m_autoRouteActive) // [firstline] owner's flag gates
                m_spotMapWindow->noteReply(d.from);
        }

        // Send to TCP API — use full text with FROM prefix for JS8 Spotter compat
        if (canSendNetworkMessage()) {
            sendNetworkMessage(
                "RX.DIRECTED", text,
                {{"_ID", QVariant(-1)},
                 {"FROM", QVariant(d.from)},
                 {"TO", QVariant(d.to)},
                 {"CMD", QVariant(d.cmd)},
                 {"GRID", QVariant(d.grid)},
                 {"EXTRA", QVariant(d.extra)},
                 {"TEXT", QVariant(text)},
                 {"FREQ", QVariant(d.dial + d.offset)},
                 {"DIAL", QVariant(d.dial)},
                 {"OFFSET", QVariant(d.offset)},
                 {"SNR", QVariant(d.snr)},
                 {"SPEED", QVariant(d.submode)},
                 {"TDRIFT", QVariant(d.tdrift)},
                 {"UTC", QVariant(d.utcTimestamp.toMSecsSinceEpoch())}});
        }

        // ChunkedArq intercept — chunked-DATA frames addressed to us,
        // with body matching the wire format "<from>: <to> <body>
        // #NN.CC/TT.HHHH". Route them to the manager (CRC, reassembly,
        // ACK, progressive display); skip the rest of this iteration
        // so the raw marker doesn't leak AND so the standard directed-
        // cmd handler (e.g. MSG TO: → addCommandToMyInbox on chunk 1)
        // doesn't fire prematurely. The receiver-side MSG detection
        // now happens inside ChunkedArq::onChunkReceived on the full
        // assembled body. NOTE: we used to gate this on d.cmd == " "
        // (freetext) but that missed multi-chunk MSG-TO sends because
        // JS8 parses the leading "MSG TO:" of the first chunk's body
        // as a directed cmd. Now we accept any d.cmd as long as the
        // text matches the chunked-DATA wire format — false positives
        // are vanishingly unlikely (parseChunkedData validates msg_id,
        // chunk_id, total, and CRC field placement). Vanilla traffic
        // that doesn't match falls through unchanged.
        // [ARQ-DIAG build 301] log every directed-to-me message arrival
        // so we can see whether the JS8 free-text assembler is even
        // delivering the assembled text for chunked-ARQ frames.
        if (toMe) {
            qWarning() << "[ARQ-DIAG] toMe directed msg arrived from=" << d.from
                       << "to=" << d.to << "cmd=" << d.cmd << "isLast=" << isLast
                       << "submode=" << d.submode << "textLen=" << text.length()
                       << "text=" << text;
        }
        if (toMe && m_chunkedArq) {
            ChunkedArq::ParsedChunk parsed;
            if (ChunkedArq::parseChunkedData(text, parsed)) {
                // [BUILD 331-arqRxClean] REMOVED auto-enable-on-RX
                // (was set via actionModeReplicatorProtocol->setChecked
                // (true) here in build arq-autoEnable 2026-06-07).
                // Receiving ARQ messages/files NEVER required ARQ-
                // enabled — `m_arqEnabled` only gates TX-side wrapping
                // (mainwindow.cpp::evaluateArqGateForText). Auto-
                // enabling on RX was setting a TX-side flag from RX
                // activity, which silently turned the operator's NEXT
                // freetext into chunked-ARQ traffic (the "+ ARQ"
                // suffix appeared on the status bar with no operator
                // action). Multi-mode override is still latched
                // directly below — it never needed the toggle path.
                if (!m_arqMultiModeOverride) {
                    m_arqMultiModeOverride = true;
                    qWarning() << "[ARQ-RX] multi-mode RX override latched ON"
                               << "via inbound chunked-DATA from" << d.from;
                }
                // [ssdetect borrow] An incoming ARQ contact must be
                // fully decodable, but unsolicited traffic does not
                // overrule the operator: decode is BORROWED for the
                // session and returned when it ends (any ending,
                // incl. timeout -- see serviceSubspaceDecodeBorrow).
                if (!m_l2Enabled)
                    borrowSubspaceDecodeForArq();
                // [BUILD 316] Auto-match submode to sender, always.
                // Previously had a per-case table (first-chunk vs subseq,
                // subspace-involved vs both-legacy, "at minimum sender's
                // speed"), but the simple rule wins: whatever speed mode
                // the inbound chunk arrived in is the mode we should be
                // in to ACK / NACK / decode the next chunk. The post-
                // ACK/NACK revert (TODO #73) restores the operator's
                // original mode after each response transmits.
                int const targetMode = d.submode;

                if (targetMode != m_nSubMode) {
                    // [TODO #73 build 312] Stash the operator's
                    // pre-switch mode so stopTx can restore it 750 ms
                    // after the ACK / NACK transmits. Only stash on
                    // the FIRST auto-switch of a sequence (don't
                    // overwrite an already-pending stash — that
                    // would lose the operator's real original mode
                    // across consecutive sender chunks where the
                    // restore hasn't fired yet).
                    if (m_arqPreSwitchSubmode == -1) {
                        m_arqPreSwitchSubmode = m_nSubMode;
                        qWarning() << "[ARQ-RX] stashed pre-switch submode="
                                   << m_arqPreSwitchSubmode
                                   << "for post-ACK restore";
                    }
                    qWarning() << "[ARQ-RX] auto-mode: was=" << m_nSubMode
                               << "sender=" << d.submode
                               << "→ now=" << targetMode
                               << "(peer=" << d.from
                               << "msgId=" << parsed.msgId
                               << "chunk=" << parsed.chunkId << ")";
                    setSubmode(targetMode);
                }
                // [TURNHOLD-ACK 2026-07-23] Delay the ACK/NACK keyup to
                // AFTER the sender has finished this chunk and switched
                // TX->RX. The sender's last frame ended at d.absPos on our
                // ring; hold = (its end-of-air still ahead of the write
                // head, clamped to one frame) + TURNAROUND_TAIL_MS
                // (250 post-roll + 1500 margin). Same computation as the
                // Build 346 sender turnhold. Without it the ACK keys
                // ~250 ms after we decode the sender's last frame — right
                // in its turnaround dead zone — and is missed ~half the
                // time (both logs 2026-07-23). absPos==0 (standard
                // decoder / no ring context) => the tail alone, still far
                // better than the old 250 ms.
                int ackHoldMs = ChunkedArq::TURNAROUND_TAIL_MS;
                if (d.absPos > 0) {
                    qint64 remain =
                        (d.absPos + ChunkedArq::TURNAROUND_FRAME_SAMPLES) -
                        m_l2RingPos.load();
                    remain = qBound<qint64>(
                        0, remain, ChunkedArq::TURNAROUND_FRAME_SAMPLES);
                    ackHoldMs += static_cast<int>(remain * 1000 / 12000);
                }
                m_chunkedArq->onChunkReceived(d.from, parsed, ackHoldMs);
                continue;
            }
        }

        // ChunkedArq ACK/NACK dispatch — vanilla manual ACKs (no
        // EXTRA payload) still flow through the existing handler
        // below; ARQ ACK/NACK carry a non-empty EXTRA with the chunk
        // ID and we route those to the manager for sender FSM update.
        if (m_chunkedArq && !d.extra.isEmpty()) {
            bool extraOk = false;
            int const seq = d.extra.toInt(&extraOk);
            if (extraOk) {
                // [TURNHOLD 2026-07-21] The async decoder can decode
                // the peer's ACK/NACK BEFORE its tail finishes airing
                // (bench: keyed 14 ms after decode while the peer was
                // still in TX → its radio missed our first frame,
                // seq-0 lost on ~40% of unmarked chunks). Hold the
                // next keyup until the frame's END-OF-AIR (absPos +
                // one FT2 frame, against the live ring position) plus
                // the peer's post-roll + turnaround margin. absPos==0
                // (no ring context) falls back to the tail alone.
                int holdMs = ChunkedArq::TURNAROUND_TAIL_MS;
                if (d.absPos > 0) {
                    qint64 remain =
                        (d.absPos + ChunkedArq::TURNAROUND_FRAME_SAMPLES) -
                        m_l2RingPos.load();
                    // A just-decoded frame can have at most one frame
                    // still airing — clamp so a stale/bogus absPos can
                    // never produce an unbounded hold.
                    remain = qBound<qint64>(
                        0, remain, ChunkedArq::TURNAROUND_FRAME_SAMPLES);
                    holdMs += static_cast<int>(remain * 1000 / 12000);
                }
                if (d.cmd == " ACK") {
                    m_chunkedArq->onAckReceived(d.from, seq, holdMs);
                    // Fall through — the existing ACK handler below
                    // does notification (tryNotify) and we want that
                    // to fire as the operator-visible cue too.
                } else if (d.cmd == " NACK") {
                    m_chunkedArq->onNackReceived(d.from, seq, holdMs);
                    // Fall through — operator-visible cue in the
                    // conversation window matters for NACK too;
                    // without the display we only saw NACKs in the
                    // band-activity panel. The dedicated NACK skip
                    // block (mirroring " ACK" at the bottom of the
                    // autoreply if/else chain) prevents NACK — which
                    // sits in autoreply_cmds at slot 2 — from
                    // triggering an unintended auto-response.
                }
            }
        }

        // we're only responding to allcalls if we are participating in the
        // allcall group but, don't avoid for heartbeats...those are technically
        // allcalls but are processed differently
        if (isAllCall && m_config.avoid_allcall() && d.cmd != " CQ" &&
            d.cmd != " HB" && d.cmd != " HEARTBEAT") {
            continue;
        }

        // [BUILD 339 TODO #103 / BUILD 353 yesflag] Passive capture of
        // ARQ capability replies. A peer answering QUERY ARQ? sends
        // "<asker> YES <level>" — YES is a directed-command token, so
        // it arrives as cmd=" YES" with the bare level digits in
        // d.text (VERIFIED in sender log 2026-07-17: cmd=" YES"
        // text="2"). Cache the level (full callsign, session-long) so
        // file transfers pick the right wire format AND the waterfall
        // labels can flag Subspace-app stations (TODO #131).
        // [BUILD 353 yesflag] Sits ABOVE the addressed-to-us gate on
        // purpose: an OVERHEARD reply to someone else's query proves
        // the sender's capability just as well as one addressed to us
        // (Andy 2026-07-30: "and Overheard replies too"). The resume
        // and missing-digits branches below stay toMe-gated — an
        // overheard reply must never key up OUR parked transfer into
        // the peer's QSO with a third party, nor burn our one retry.
        // Exact-digits match so "YES MSG ID n" (text="MSG ID n") and
        // "YES +08 (15S)" (QUERY CALL reply; SNR always sign-prefixed)
        // never count. Passive observer: NO continue — display and
        // downstream processing are unchanged.
        // [#161 querycall] "YES +08 (15S)" from a station we asked
        // — bind to the pending query and feed the backdated
        // third-party edge. Passive: display and every other YES
        // dialect below are untouched (the binder's sign-prefixed
        // snr + "(age)" shape is disjoint from the ARQ digits and
        // "MSG ID n" forms). A "NO" answer clears the pending entry.
        if (!isAllCall && toMe && d.cmd == QStringLiteral(" YES"))
            bindCallQueryReply(d.from, d.text, d.dial);
        if (!isAllCall && toMe && d.cmd == QStringLiteral(" NO"))
            m_pendingCallQueries.remove(d.from.toUpper());

        if (!isAllCall && d.cmd == QStringLiteral(" YES")) {
            static QRegularExpression const kArqLevelReplyRe{
                QStringLiteral(R"(^(\d{1,3})$)")};
            if (auto const lm = kArqLevelReplyRe.match(
                    d.text.toUpper().simplified());
                lm.hasMatch()) {
                int const level = lm.captured(1).toInt();
                // [2026-07-23] INVARIANT: this is the ONE and ONLY
                // place a peer's ARQ level is ever cached, and it is
                // reached ONLY by an actual "YES <digits>" reply.
                // SILENCE MUST NEVER CACHE ANYTHING. QUERY ARQ? is
                // YES-or-silence by design — nobody ever answers
                // "NO" — so a missing reply carries NO information
                // about the peer: the query or the reply is easily
                // stepped on (half-duplex collision, QRM). Caching a
                // level-1 assumption from silence would permanently
                // demote a capable peer for the whole session. The
                // timeout path therefore falls back to V1 for THAT
                // TRANSFER ONLY and writes nothing, so the next
                // transfer re-queries. Do not "optimise" that away.
                m_peerArqLevel[d.from.toUpper()] = level;
                qWarning() << "[ARQ] peer capability cached (from an"
                              " actual YES reply):"
                           << d.from << "level=" << level
                           << (toMe ? "(to us)" : "(overheard)");
                // [BUILD 353 yesflag retro] Retro-color the waterfall
                // label: the YES frame's own label was painted BEFORE
                // this capture ran (decode path precedes the command
                // queue), so it went up white. Re-annotate as an
                // in-place upgrade — same call + column matches the
                // just-painted label and overdraws it at its original
                // row, and with the cache now filled the paint-time
                // predicate yields the capability yellow. A YES
                // reply's destination is always a callsign, never a
                // group, so inMyGroup=false here can never downgrade
                // a purple label (purple precedence preserved,
                // operator decision 2026-07-30).
                m_wideGraph->setCallsignOverlayEnabled(
                    m_config.show_calls_on_waterfall());
                m_wideGraph->annotateCall(
                    d.from, d.offset, d.submode,
                    /*inMyGroup=*/false,
                    /*destIsSubspaceGroup=*/false,
                    /*isUpgrade=*/true);
                // [BUILD 339 TODO #103] A file transfer may be
                // parked waiting on exactly this reply — resume it
                // with the just-learned format. Deferred 1.5 s so RX
                // processing settles before we key up. toMe-gated:
                // only OUR reply resumes OUR transfer.
                if (toMe && capabilityNegotiationPending() &&
                    d.from.compare(m_pendingFilePeer,
                                   Qt::CaseInsensitive) == 0) {
                    bool const requiredV2 = m_pendingRequiresV2;
                    QString const formSparse = m_pendingFormSparsePath;
                    QString path, link, pr;
                    // Hold the phase open across the 1.5 s defer below
                    // — see takeCapabilityNegotiation()'s declaration.
                    takeCapabilityNegotiation(&path, &link, &pr, true);
                    // [ICS213 v1gate] Parked FORM send + an actual
                    // "YES 1": the peer is definitively V1 — abort
                    // now (no V1 fallback for forms). The level IS
                    // cached above, so a retry aborts instantly at
                    // the cache-hit gate.
                    if (requiredV2 && level < 2 && link.isEmpty()) {
                        // NO return — we're inside the command-queue
                        // while loop; later queued commands must
                        // still process.
                        qWarning() << "[FT-TX] form transfer aborted"
                                      " — peer" << pr
                                   << "answered YES" << level;
                        notifyFormTransferAborted(
                            pr, QStringLiteral("peer answered YES 1"));
                        endCapabilityNegotiationPhase();
                    } else {
                        qWarning() << "[FT-TX] capability received — "
                                      "resuming pending transfer to"
                                   << pr << "level=" << level
                                   << (link.isEmpty() ? "(file)"
                                                      : "(link)");
                        // [ARQ level 4] Wire shape decided HERE —
                        // the first moment the peer's level is known.
                        QString const wirePath =
                            (requiredV2 && link.isEmpty())
                                ? formWirePath(path, formSparse, level)
                                : path;
                        QPointer<UI_Constructor> const self(this);
                        QTimer::singleShot(1500, this,
                            [self, wirePath, link, pr, level]() {
                                if (!self) return;
                                if (!link.isEmpty()) {
                                    // Cache now holds the level;
                                    // sendWebLink proceeds directly.
                                    self->sendWebLink(link, pr);
                                } else {
                                    self->startFileTransferWithFormat(
                                        wirePath, pr, level);
                                }
                                // Transfer now owns the lock via
                                // m_sends (or failed to start) —
                                // close the phase.
                                self->endCapabilityNegotiationPhase();
                            });
                    }
                }
            } else if (toMe &&
                       (!m_pendingFilePath.isEmpty() ||
                        !m_pendingLinkUrl.isEmpty()) &&
                       d.from.compare(m_pendingFilePeer,
                                      Qt::CaseInsensitive) == 0 &&
                       !d.text.toUpper().simplified()
                            .startsWith(QStringLiteral("MSG"))) {
                // [BUILD 341 capRetry] A YES from the very peer we're
                // querying, but the level digits didn't survive (the
                // reply's second frame lost — operator-observed
                // "YES <missing frame>" 2026-07-17). The peer IS
                // answering; only the level is missing. Fire the
                // timeout logic NOW instead of waiting out the full
                // window — it re-queries once (with a fresh
                // generation + full window) or, if the retry is
                // already spent, falls back to V1 (safe for any
                // ARQ-capable peer). The "MSG" exclusion keeps a
                // concurrent "YES MSG ID n" (QUERY MSGS reply) from
                // burning the retry. toMe-gated (BUILD 353): an
                // overheard garbled YES to a third party must not
                // burn our retry.
                qWarning() << "[FT-TX] YES from" << d.from
                           << "without level digits (text="
                           << d.text.left(20)
                           << ") — immediate re-query";
                onCapQueryTimeout(m_capQueryGen);
            }
        }

        // we're only responding to allcall, groupcalls, and our callsign at
        // this point, so we'll end after logging the callsigns we've heard
        if (!isAllCall && !toMe && !isGroupCall) {
            // [#153 ARQMON tee] Overheard third-party directed text
            // was a pure discard here (fact-verified: no other
            // consumer). Feed the passive monitor — it internally
            // parses the chunked-DATA form, applies the leadmark
            // mirror + file/link family + on-frequency rules, and
            // can never transmit. No-op while the Monitor window is
            // closed. Existing display behavior below is untouched.
            if (m_arqMonitor && m_arqMonitor->active() &&
                d.from.compare(m_config.my_callsign(),
                               Qt::CaseInsensitive) != 0) {
                m_arqMonitor->onDirectedText(d.from, d.to, text,
                                             d.offset, d.submode);
            }
            // [onfreqhdr 2026-08-15, reworked 2026-08-16]
            // DISPLAY-ONLY exception for on-offset traffic to other
            // stations. BUFFERED commands (multi-frame freetext,
            // relays) are EXCLUDED here: their header now displays
            // live at decode time (processDecodeEvent [onfreqhdr2])
            // and their payload frames via the stock on-frequency
            // rule — printing the assembled text again would
            // duplicate everything the operator already watched.
            // Single-frame commands are the remaining case: they
            // never display anywhere else. Discriminator is the
            // entry's OWN isBuffered flag (assembled arrivals are
            // marked at re-enqueue; direct-queued frames are
            // zero-init false) — NOT Varicode::isCommandBuffered(),
            // which is true for nearly every directed cmd (leading
            // space!) and would have silenced single-frame commands
            // entirely (caught in the side-effect sweep,
            // 2026-08-16).
            if (!d.isBuffered &&
                std::abs(d.offset - freq()) <=
                    JS8::Submode::rxThreshold(d.submode)) {
                displayTextForFreq(text, d.offset, d.utcTimestamp, false,
                                   true, false, d.submode);
            }
            continue;
        }

        ActivityDetail ad = {};
        ad.isLowConfidence = false;
        ad.isDirected = true;
        ad.bits = d.bits;
        ad.dial = d.dial;
        ad.offset = d.offset;
        ad.snr = d.snr;
        ad.text = text;
        ad.utcTimestamp = d.utcTimestamp;
        ad.submode = d.submode;

        // we'd be double printing here if were on frequency, so let's be
        // "smart" about this...
        bool shouldDisplay = true;

        // don't display ping allcalls
        if (isAllCall && (d.cmd != " " || ad.text.contains("@HB HEARTBEAT"))) {
            shouldDisplay = false;
        }

        if (shouldDisplay) {
            auto c = ui->textEditRX->textCursor();
            c.movePosition(QTextCursor::End);
            ui->textEditRX->setTextCursor(c);

            // ACKs and SNRs are the most likely source of items to be
            // overwritten (multiple responses at once)... so don't overwrite
            // those (i.e., print each on a new line)
            bool shouldOverwrite =
                (!d.cmd.contains(" ACK") &&
                 !d.cmd.contains(" SNR")); /* && isRecentOffset(d.freq);*/

            if (shouldOverwrite &&
                ui->textEditRX->find(d.utcTimestamp.time().toString(),
                                     QTextDocument::FindBackward)) {
                // ... maybe we could delete the last line that had this message
                // on this frequency...
                c = ui->textEditRX->textCursor();
                c.movePosition(QTextCursor::StartOfBlock);
                c.movePosition(QTextCursor::EndOfBlock,
                               QTextCursor::KeepAnchor);
                qCDebug(mainwindow_js8) << "should display directed message, "
                                           "erasing last rx activity line..."
                                        << c.selectedText().toUpper();
                c.removeSelectedText();
                c.deletePreviousChar();
                c.deletePreviousChar();
                /*
                c.deleteChar();
                c.deleteChar();
                */
            }

            // log it to the display!
            displayTextForFreq(ad.text, ad.offset, ad.utcTimestamp, false, true,
                               false, ad.submode);

            /*
            // and send it to the network in case we want to interact with it
            from an external app... if(canSendNetworkMessage()){
                sendNetworkMessage("RX.DIRECTED.ME", ad.text, {
                    {"_ID", QVariant(-1)},
                    {"FROM", QVariant(d.from)},
                    {"TO", QVariant(d.to)},
                    {"CMD", QVariant(d.cmd)},
                    {"GRID", QVariant(d.grid)},
                    {"EXTRA", QVariant(d.extra)},
                    {"TEXT", QVariant(d.text)},
                    {"FREQ", QVariant(ad.dial+ad.offset)},
                    {"DIAL", QVariant(ad.dial)},
                    {"OFFSET", QVariant(ad.offset)},
                    {"SNR", QVariant(ad.snr)},
                    {"SPEED", QVariant(ad.submode)},
                    {"TDRIFT", QVariant(ad.tdrift)},
                    {"UTC", QVariant(ad.utcTimestamp.toMSecsSinceEpoch())}
                });
            }
            */

            if (!isAllCall) {
                // Only cancel CQ loop if message is directed to us
                if (m_cq_loop->isActive() &&
                    d.to == m_config.my_callsign().trimmed()) {
                    qCDebug(mainwindow_js8)
                        << "Canceling CQ loop: directed message to us";
                    m_cq_loop->onLoopCancel();
                }

                // notification for directed message. @APRSIS is group
                // traffic (anyone gateway-relaying to APRS-IS), not
                // anything directed to this operator, so suppress the
                // "directed-msg" alert there. Spotting / display / any
                // auto-reply logic on @APRSIS is unchanged.
                // [BUILD 331-todo78] Also suppress when an ARQ session
                // (TX or RX) is active. Each chunk arrival in a multi-
                // chunk super-message fires this notification path;
                // chiming every ~3.75 s during a 20-chunk transfer is
                // pure noise. hasActiveSession() catches both directions.
                bool const arqInFlight =
                    m_chunkedArq && m_chunkedArq->hasActiveSession();
                // [BUILD 331-bell] BELL has its own notification kind
                // ("bell" — DingDing). Suppress the generic "directed-
                // msg" chime when the body is a BELL so we don't get
                // both at once.
                bool const isBellMsg =
                    d.cmd == QStringLiteral(" ") &&
                    (d.text.toUpper().endsWith(QStringLiteral(" BELL")) ||
                     d.text.toUpper() == QStringLiteral("BELL"));
                if (d.to != "@APRSIS" && !arqInFlight && !isBellMsg) {
                    tryNotify("directed", d.submode);
                }
            }
        }

        // we're only responding to callsigns in our whitelist if we have one
        // defined... make sure the whitelist is empty (no restrictions) or the
        // from callsign or its base callsign is on it
        auto whitelist = m_config.auto_whitelist();
        if (!whitelist.isEmpty() &&
            !(whitelist.contains(d.from) ||
              whitelist.contains(Radio::base_callsign(d.from)))) {
            qCDebug(mainwindow_js8)
                << "skipping command for whitelist" << d.from;
            continue;
        }

        // we'll never reply to a blacklisted callsign or base callsign
        auto blacklist = m_config.auto_blacklist();
        if (!blacklist.isEmpty() &&
            (blacklist.contains(d.from) ||
             blacklist.contains(Radio::base_callsign(d.from)))) {
            qCDebug(mainwindow_js8)
                << "skipping command for blacklist" << d.from;
            continue;
        }

        // if this is an allcall, check to make sure we haven't replied to their
        // allcall recently (in the past ten minutes) that way we never get
        // spammed by allcalls at too high of a frequency
        if (isAllCall && m_txAllcallCommandCache.contains(d.from) &&
            m_txAllcallCommandCache[d.from]->secsTo(now) / 60 < 15) {
            qCDebug(mainwindow_js8)
                << "skipping command for allcall timeout" << d.from;
            continue;
        }

        // don't actually process any automatic message replies while in idle
        if (m_tx_watchdog) {
            qCDebug(mainwindow_js8)
                << "skipping command for idle timeout" << d.from;
            continue;
        }

        // HACK: if this is an autoreply cmd and relay path is populated and cmd
        // is not MSG or MSG TO:, then swap out the relay path
        if (Varicode::isCommandAutoreply(d.cmd) && !d.relayPath.isEmpty() &&
            !d.cmd.startsWith(" MSG") && !d.cmd.startsWith(" QUERY")) {
            d.from = d.relayPath;
        }

        // construct a reply, if needed
        QString reply;
        int priority = PriorityNormal;
        int freq = -1;
        // [BUILD 341 arqReplyAck] Set by ARQ-protocol replies
        // (currently: QUERY ARQ? capability reply). These are protocol
        // obligations dispatched EXACTLY like ACK / NACK, not courtesy
        // autoreplies:
        //   (a) transmit at the CALLER's speed (same stash/restore
        //       machinery as chunked-DATA ACKs, TODO #73 — applied at
        //       the dispatch site below, after every reply-drop gate,
        //       so a dropped reply can never strand us in the caller's
        //       mode), and
        //   (b) dispatch via onChunkedWantsResponseTx — the SAME slot
        //       Manager::sendAck feeds — which saves the operator's
        //       outgoing-box draft, TXes directly (no autoreply queue,
        //       no PriorityHigh gate, no confirmation dialog), and
        //       restores the draft at TX drain (stopTx, TODO #57).
        //       Operator-observed 2026-07-17, twice: autoreply off
        //       parked the YES in the box; then the queued path
        //       clobbered a live draft the ACK path would have saved.
        //   (c) The relay-'>' exception on the draft / open-buffer
        //       defer gates (the save/restore handles drafts now).
        //   Directed queries only — @ALLCALL probes stay behind the
        //   autoreply gate (band-wide auto-keying stays opt-in, same
        //   consent posture as AVHAIL?).
        bool arqProtocolReply = false;

        // [BUILD 331-avHail2] Hidden AVHAIL? remote-trigger command.
        // When a peer addresses us with an EXACT "AVHAIL?" body,
        // switch to Subspace (FT2) submode and fire an audio-visual
        // HAIL. NOT in the directed-to-XXX menu (operators invoke via
        // TCP API or manual typing).
        //
        // [BUILD 336 TODO #87] The original mode speed is captured
        // here and restored once the hail completes (stopTx).
        //
        // [BUILD 336 avHailFleet] Hardened + fleet-capable:
        //  - EXACT body match (d.text == "AVHAIL?"), no longer
        //    contains() — a message merely mentioning AVHAIL? (e.g.
        //    someone asking what it is) must not key the transmitter.
        //  - @ALLCALL form accepted: one trigger lights every
        //    Subspace station at once. Response delay is randomized
        //    over 5 s so fleet PTT-ups aren't phase-locked and the
        //    encoded HAILs at different offsets stay decodable at the
        //    trigger end. Whitelist / blacklist already ran above.
        //  - Consent / politeness gates (TODO #97, Andy 2026-07-15):
        //    response inhibited when AUTO (autoreply) is off, when the
        //    outgoing text box has ANY operator text (the hail path
        //    would clobber a draft), or when a callsign is selected
        //    (operator is mid-workflow). Every suppression logs at
        //    qWarning — silent misses cost a full gate-by-gate hunt.
        //  - Rate-limit / replay guard (TODO #97): global once-per-
        //    hour cap — a replayed trigger (no anti-replay exists in
        //    the protocol; 2026-06-08 P1RATE incident) can't chain-
        //    key the station.
        if (d.cmd == QStringLiteral(" ") && !m_visibleHailActive &&
            !m_transmitting) {
            // d.text is the message BODY ONLY (to/cmd/extra live in
            // their own fields; the "FROM: TO body ♢" shape seen in
            // logs and DIRECTED.TXT is display text assembled above).
            // Same convention as the BELL handler below. Exact match:
            // the body must BE "AVHAIL?", not merely contain it.
            QString const body = d.text.toUpper().simplified();
            if (body == QStringLiteral("AVHAIL?")) {
                if (!ui->actionModeAutoreply->isChecked()) {
                    qWarning() << "[AVHAIL?] suppressed: auto-reply off"
                               << "from=" << d.from;
                    continue;
                }
                if (!ui->extFreeTextMsgEdit->toPlainText()
                         .trimmed().isEmpty()) {
                    qWarning() << "[AVHAIL?] suppressed: outgoing box"
                                  " not empty, from=" << d.from;
                    continue;
                }
                if (!callsignSelected().trimmed().isEmpty()) {
                    qWarning() << "[AVHAIL?] suppressed: a callsign is"
                                  " selected, from=" << d.from;
                    continue;
                }
                qint64 const nowMs =
                    DriftingDateTime::currentMSecsSinceEpoch();
                if (m_lastAvhailResponseMs > 0 &&
                    nowMs - m_lastAvhailResponseMs < 3600000) {
                    qWarning() << "[AVHAIL?] suppressed: rate limit"
                               << "(once per hour), from=" << d.from
                               << "elapsedMin="
                               << ((nowMs - m_lastAvhailResponseMs)
                                   / 60000);
                    continue;
                }
                m_lastAvhailResponseMs = nowMs;
                int delayMs = 500;
                if (isAllCall) {
                    delayMs += QRandomGenerator::global()->bounded(5000);
                }
                qWarning() << "[AVHAIL?] remote-trigger received from"
                           << d.from << "allcall=" << isAllCall
                           << "delay=" << delayMs
                           << "→ switching to Subspace + firing AV HAIL";
                m_visibleHailRestoreSubmode =
                    (m_nSubMode != Varicode::JS8CallFT2) ? m_nSubMode
                                                         : -1;
                setSubmode(Varicode::JS8CallFT2);
                QPointer<UI_Constructor> const self(this);
                QTimer::singleShot(delayMs, this, [self]() {
                    if (!self) return;
                    self->on_sendVisibleHailAction_triggered();
                });
                continue;
            }
        }

        // [BUILD 353 yesflag] The passive ARQ-capability YES capture
        // used to live here — moved ABOVE the addressed-to-us gate so
        // OVERHEARD third-party "YES <level>" replies also cache (and
        // feed the waterfall capability flag, TODO #131). One fact,
        // one site — see the block before the gate.

        // [BUILD 331-bell TODO #80] BELL command. Peer addresses us
        // with "<us> BELL" — we play the configured "bell" sound
        // (DingDing.wav recommended). Soft summons for a QSO.
        // Guards: addressed-to-us-personally (not @ALLCALL, not a
        // group), freetext cmd, body equals or ends with "BELL", not
        // in active ARQ session (chunked-ARQ body might literally
        // contain "BELL" as payload text — don't trigger on those).
        if (!isAllCall && !isGroupCall && d.cmd == QStringLiteral(" ") &&
            (d.text.toUpper().endsWith(QStringLiteral(" BELL")) ||
             d.text.toUpper() == QStringLiteral("BELL")) &&
            !(m_chunkedArq && m_chunkedArq->hasActiveSession())) {
            qWarning() << "[BELL] received from" << d.from
                       << "→ playing bell notification";
            tryNotify(QStringLiteral("bell"), d.submode);
            continue;
        }

        // QUERIED SNR
        if (d.cmd == " SNR?" && !isAllCall) {
            reply = QString("%1 SNR %2")
                        .arg(d.from)
                        .arg(Varicode::formatSNR(d.snr));
        }

        // QUERIED INFO
        else if (d.cmd == " INFO?" && !isAllCall) {
            QString info = m_config.my_info();
            if (info.isEmpty()) {
                continue;
            }

            reply = QString("%1 INFO %2")
                        .arg(d.from)
                        .arg(replaceMacros(info, buildMacroValues(), true));
        }

        // QUERIED ACTIVE
        else if (d.cmd == " STATUS?" && !isAllCall) {
            QString status = m_config.my_status();
            if (status.isEmpty()) {
                continue;
            }

            reply = QString("%1 STATUS %2")
                        .arg(d.from)
                        .arg(replaceMacros(status, buildMacroValues(), true));
        }

        // QUERIED GRID
        else if (d.cmd == " GRID?" && !isAllCall) {
            QString grid = m_config.my_grid();
            if (grid.isEmpty()) {
                continue;
            }

            reply = QString("%1 GRID %2").arg(d.from).arg(grid);
        }

        // QUERIED STATIONS HEARD
        else if (d.cmd == " HEARING?" && !isAllCall) {
            auto calls = m_callActivity.keys();

            std::stable_sort(calls.begin(), calls.end(),
                             [this](QString const &lhs, QString const &rhs) {
                                 return m_callActivity[rhs].utcTimestamp <
                                        m_callActivity[lhs].utcTimestamp;
                             });

            auto const callsignAging = m_config.callsign_aging();
            auto const maxStations = 4;
            auto i = 0;
            QStringList lines;

            for (auto const &call : calls) {
                if (i >= maxStations)
                    break;
                if (call == d.from)
                    continue;

                auto const &cd = m_callActivity[call];

                if (callsignAging &&
                    cd.utcTimestamp.secsTo(now) / 60 >= callsignAging) {
                    continue;
                }

                lines.append(cd.call);
                i++;
            }

            lines.prepend(QString("%1 HEARING").arg(d.from));
            reply = lines.join(' ');
        }

        // PROCESS RELAY
        // [TODO #149] Gated on the OUTER frame addressee: an overheard
        // relay hop (we are neither the relay station nor the frame's
        // addressee) must never be processed here — its payload can
        // name OUR call as the eventual destination ("WD4KAV: KJ7VWV>
        // WM8Q TEST ..."), and honoring that inner addressee routed
        // the hop into the bystander's conversation window (field
        // 2026-08-12). Overheard relays now fall through to ordinary
        // band-activity display; the legitimate delivery arrives as
        // its own frame addressed to us.
        else if (d.cmd == ">" && toMe && !isAllCall &&
                 !m_config.relay_off()) {

            // 1. see if there are any more hops to process
            // 2. if so, forward
            // 3. otherwise, display alert & reply dialog

            QString callToPattern = {
                R"(^(?<callsign>\b(?<prefix>[A-Z0-9]{1,4}\/)?(?<base>([0-9A-Z])?([0-9A-Z])([0-9])([A-Z])?([A-Z])?([A-Z])?)(?<suffix>\/[A-Z0-9]{1,4})?(?<type>[> ]))\b)"};
            QRegularExpression re(callToPattern);
            auto text = d.text;
            auto match = re.match(text);

            // if the text starts with a callsign, and relay is not disabled,
            // and this is not a group callsign, then relay.
            if (match.hasMatch() && !isGroupCall) {
                // replace freetext with relayed free text
                if (match.captured("type") != ">") {
                    text = text.replace(match.capturedStart("type"),
                                        match.capturedLength("type"), ">");
                }
                reply = QString("%1 *DE* %2").arg(text).arg(d.from);

                // otherwise, as long as we're not an ACK...alert the user and
                // either send an ACK or Message
                //
                // [TODO #177, operator: "nobody cares"] ...unless this
                // exact relayed reply just closed a reaching attempt
                // as REACHED (marked by reachOnDirected earlier in
                // this same pass). The reply's arrival already proves
                // the whole path; an ACK back through the relay is a
                // 218 s-class transmission carrying no information.
            } else if (!m_reachAnsweredText.isEmpty() &&
                       d.from == m_reachAnsweredFrom &&
                       d.text == m_reachAnsweredText) {
                m_reachAnsweredFrom.clear();
                m_reachAnsweredText.clear();
                qCWarning(mainwindow_js8)
                    << "[REACH] relay ACK suppressed (TODO #177):"
                    << d.text.left(40);
            } else if (!d.text.startsWith("ACK")) {

                // parse out the callsign path
                auto calls = parseRelayPathCallsigns(d.from, d.text);

                // put these third party calls in the heard list
                foreach (auto call, calls) {
                    CallDetail cd = {};
                    cd.call = call;
                    cd.snr = -64;
                    cd.dial = d.dial;
                    cd.offset = d.offset;
                    cd.through = d.from;
                    cd.utcTimestamp = DriftingDateTime::currentDateTimeUtc();
                    cd.tdrift = d.tdrift;
                    cd.submode = d.submode;
                    logCallActivity(cd, false);
                }

                d.relayPath = calls.join('>');

                reply = QString("%1 ACK").arg(d.relayPath);

                // check to see if the relay text contains a command that should
                // be replied to instead of an ack.
                QStringList relayedCmds = d.text.split(" ");
                if (!relayedCmds.isEmpty()) {
                    auto first = relayedCmds.first();

                    auto valid = Varicode::isCommandAllowed(first);
                    if (!valid) {
                        first = " " + first;
                        valid = Varicode::isCommandAllowed(first);
                        if (valid) {
                            relayedCmds.removeFirst();
                        }
                    }

                    // HACK: "MSG TO:" should be supported but contains a space
                    // :(
                    if (!relayedCmds.isEmpty()) {
                        if (first == " MSG") {
                            auto second = relayedCmds.first();
                            if (second == "TO:") {
                                first = " MSG TO:";
                                relayedCmds.removeFirst();
                            } else if (second.startsWith("TO:")) {
                                first = " MSG TO:";
                                relayedCmds.replace(0, second.mid(3));
                            }
                        } else if (first == " QUERY") {
                            auto second = relayedCmds.first();
                            if (second == "MSGS" || second == "MSGS?") {
                                first = " QUERY MSGS";
                                relayedCmds.removeFirst();
                            } else if (second == "CALL") {
                                first = " QUERY CALL";
                                relayedCmds.removeFirst();
                            }
                        }
                    }

                    if (Varicode::isCommandAllowed(first) &&
                        Varicode::isCommandAutoreply(first)) {
                        CommandDetail rd = {};
                        rd.bits = d.bits;
                        rd.cmd = first;
                        rd.dial = d.dial;
                        rd.offset = d.offset;
                        rd.from =
                            d.from; // note, MSG and QUERY commands are not set
                                    // with from as the relay path.
                        rd.relayPath = d.relayPath;
                        rd.text = relayedCmds.join(" "); // d.text;
                        rd.to = d.to;
                        rd.utcTimestamp = d.utcTimestamp;

                        m_rxCommandQueue.insert(0, rd);
                        continue;
                    }
                }

#if STORE_RELAY_MSGS_TO_INBOX
                // if we make it here, this is a message
                addCommandToMyInbox(d);
#endif
            }
        }

        // PROCESS MESSAGE STORAGE
        else if (d.cmd == " MSG TO:" && !isAllCall) {

            // store message
            QStringList segs = d.text.split(" ");
            if (segs.isEmpty()) {
                continue;
            }

            auto to = segs.first();
            segs.removeFirst();

            auto text = segs.join(" ").trimmed();

            /**
             * APRS relay: inbound `MSG TO:` stores for our local station.
             * Dedupe within 60s using TO + TEXT + DE <SENDER>.
             */
            if (d.to == "@APRSIS") {
                if (to != m_config.my_callsign().trimmed()) {
                    continue;
                }

                static QRegularExpression aprsDeRe("\\s+DE\\s+(\\S+)\\s*$");
                auto aprsSender = QString("APRS");
                auto deMatch = aprsDeRe.match(text);
                if (deMatch.hasMatch()) {
                    aprsSender = deMatch.captured(1);
                }
                auto dedupeKey =
                    QString("%1|%2|%3").arg(to, text, aprsSender).toUpper();
                auto now = DriftingDateTime::currentDateTimeUtc();
                auto cutoff = now.addSecs(-60);
                auto it = m_aprsRelayDedupCache.begin();
                while (it != m_aprsRelayDedupCache.end()) {
                    if (it.value() < cutoff) {
                        it = m_aprsRelayDedupCache.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (m_aprsRelayDedupCache.contains(dedupeKey)) {
                    continue;
                }

                CommandDetail cd = {};
                cd.bits = d.bits;
                cd.cmd = " MSG ";
                cd.extra = d.extra;
                cd.dial = d.dial;
                cd.offset = d.offset;
                cd.from = "APRS";
                cd.relayPath = "APRS";
                cd.grid = d.grid;
                cd.snr = d.snr;
                cd.tdrift = d.tdrift;
                cd.text = text;
                cd.to = to;
                cd.utcTimestamp = d.utcTimestamp;
                cd.submode = d.submode;

                addCommandToMyInbox(cd);

                m_aprsRelayDedupCache.insert(dedupeKey, now);
                tryNotify("inbox", d.submode);
                continue;
            }

            if (m_config.relay_off()) {
                continue;
            }

            auto calls = parseRelayPathCallsigns(d.from, text);
            d.relayPath = calls.join(">");

            CommandDetail cd = {};
            cd.bits = d.bits;
            cd.cmd = d.cmd;
            cd.extra = d.extra;
            cd.dial = d.dial;
            cd.offset = d.offset;
            cd.from = d.from;
            cd.grid = d.grid;
            cd.relayPath = d.relayPath;
            cd.snr = d.snr;
            cd.tdrift = d.tdrift;
            cd.text = text;
            cd.to = Radio::base_callsign(to);
            cd.utcTimestamp = d.utcTimestamp;
            cd.submode = d.submode;

            qCDebug(mainwindow_js8)
                << "storing message to" << to << ":" << text;

            // [MSG TO: DIAG 2026-06-12 build 258] Surface storage path
            // visibly so we can confirm what actually lands where when
            // the operator reports inbox-routing surprises.
            qWarning() << "[MSG TO:] on-air store: from=" << d.from
                       << "to=K9AVT-style-relay=" << d.to
                       << "addressee=" << to
                       << "baseAddressee=" << Radio::base_callsign(to)
                       << "text=" << text
                       << "→ addCommandToStorage(STORE)";

            addCommandToStorage("STORE", cd);

            // we haven't replaced the from with the relay path, so we have to
            // use it for the ack if there is one
            reply = QString("%1 ACK").arg(calls.length() > 1 ? d.relayPath
                                                             : d.from);
        }

        // PROCESS AGN
        else if (d.cmd == " AGN?" && !isAllCall && !isGroupCall &&
                 !m_lastTxMessage.isEmpty()) {
            reply = Varicode::rstrip(m_lastTxMessage);
        }

        // PROCESS ACTIVE HEARTBEAT
        // if we have hb mode enabled and auto reply enabled <del>and auto ack
        // enabled and no callsign is selected</del> update: if we're in HB
        // mode, doesn't matter if a callsign is selected.
        else if ((d.cmd == " HB" || d.cmd == " HEARTBEAT") &&
                 canCurrentModeAckHeartbeat() &&
                 ui->actionModeJS8HB->isChecked() &&
                 ui->actionModeAutoreply->isChecked() &&
                 ui->actionHeartbeatAcknowledgements->isChecked()) {
            // do not process HB activity if buffer is not empty, this prevents
            // broken incoming MSG's
            if (!m_messageBuffer.isEmpty()) {
                qCDebug(mainwindow_js8) << "hb paused for messageBuffer";
                continue;
            }

            // check to make sure we aren't pausing HB transmissions (ACKs)
            // while a callsign is selected
            if (m_config.heartbeat_qso_pause() && !selectedCallsign.isEmpty()) {
                qCDebug(mainwindow_js8) << "hb paused during qso";
                continue;
            }

            // check to make sure this callsign isn't blacklisted
            if (m_config.hb_blacklist().contains(d.from) ||
                m_config.hb_blacklist().contains(
                    Radio::base_callsign(d.from))) {
                qCDebug(mainwindow_js8) << "hb blacklist blocking" << d.from;
                continue;
            }

            // check to see if we have a message for a station who is
            // heartbeating
            QString extra;
            auto mid = getNextMessageIdForCallsign(d.from);

            // Get number of unread messages
            int pendingCount = countUnreadForCallsign(d.from);
            if (isGroupCall) {
                pendingCount += countGroupUnreadForCallsign(d.to, d.from);
            }

            // We want to send the number of pending messages IN ADDITION TO
            // the one we are about to send, so decrement the actual count
            pendingCount--;

            if (mid != -1) {
                if (pendingCount > 0) {
                    extra = QString("MSG ID %1 +%2)")
                            .arg(mid)
                            .arg(pendingCount);
                }
                else {
                    extra = QString("MSG ID %1").arg(mid);
                }

            }

            // group messaging - if isGroupCall, check to see if there's a
            // message id for the group and return it if there's not an
            // individual message
            // TODO: include group name in response to differentiate from direct
            // messages?
            else if (isGroupCall) {
                mid = getNextGroupMessageIdForCallsign(d.to, d.from);
                if (mid != -1) {
                    if (pendingCount > 0) {
                        extra = QString("MSG ID %1 +%2)")
                            .arg(mid)
                            .arg(pendingCount);
                    }
                    else {
                        extra = QString("MSG ID %1").arg(mid);
                    }
                }
            }

            // TODO: require confirmation?
            sendHeartbeatAck(d.from, d.snr, extra);

            if (isAllCall) {
                // since all pings are technically @ALLCALL, let's bump the
                // allcall cache here...
                m_txAllcallCommandCache.insert(d.from, new QDateTime(now), 5);
            }

            continue;
        }

        // PROCESS HEARTBEAT SNR
        else if (d.cmd == " HEARTBEAT SNR") {
            qCDebug(mainwindow_js8) << "skipping incoming hb snr" << d.text;
            continue;
        }

        // PROCESS CQ
        else if (d.cmd == " CQ") {
            qCDebug(mainwindow_js8) << "skipping incoming cq" << d.text;
            continue;
        }

        // PROCESS MSG
        else if (d.cmd == " MSG" && !isAllCall) {

            auto text = d.text;

            /**
             * APRS relay: inbound `MSG` where the payload starts with
             * `TO:<DEST>` and includes `DE <SENDER>`.
             * Dedupe within 60s using TO + TEXT + DE <SENDER>.
             */
            if (d.to == "@APRSIS") {
                static QRegularExpression aprsToRe("^TO:\\s*(\\S+)\\s+(.*)$");
                auto match = aprsToRe.match(text);
                if (match.hasMatch()) {
                    auto dest = match.captured(1);
                    auto aprsText = match.captured(2).trimmed();
                    if (dest == m_config.my_callsign().trimmed()) {
                        static QRegularExpression aprsDeRe(
                            "\\s+DE\\s+(\\S+)\\s*$");
                        auto aprsSender = QString("APRS");
                        auto deMatch = aprsDeRe.match(aprsText);
                        if (deMatch.hasMatch()) {
                            aprsSender = deMatch.captured(1);
                        }
                        auto dedupeKey = QString("%1|%2|%3")
                                             .arg(dest, aprsText, aprsSender)
                                             .toUpper();
                        auto now = DriftingDateTime::currentDateTimeUtc();
                        auto cutoff = now.addSecs(-60);
                        auto it = m_aprsRelayDedupCache.begin();
                        while (it != m_aprsRelayDedupCache.end()) {
                            if (it.value() < cutoff) {
                                it = m_aprsRelayDedupCache.erase(it);
                            } else {
                                ++it;
                            }
                        }
                        if (m_aprsRelayDedupCache.contains(dedupeKey)) {
                            continue;
                        }

                        CommandDetail cd = d;
                        cd.cmd = " MSG ";
                        cd.from = "APRS";
                        cd.relayPath = "APRS";
                        cd.to = dest;
                        cd.text = aprsText;

                        addCommandToMyInbox(cd);

                        m_aprsRelayDedupCache.insert(dedupeKey, now);
                        tryNotify("inbox", d.submode);
                    }
                }
                continue;
            }

            qCDebug(mainwindow_js8) << "adding message to inbox" << text;

            auto calls = parseRelayPathCallsigns(d.from, text);

            d.cmd = " MSG ";
            d.relayPath = calls.join(">");
            d.text = text;

            addCommandToMyInbox(d);

            // notification
            tryNotify("inbox", d.submode);

            // we haven't replaced the from with the relay path, so we have to
            // use it for the ack if there is one
            reply = QString("%1 ACK").arg(calls.length() > 1 ? d.relayPath
                                                             : d.from);

#define SHOW_ALERT_FOR_MSG 1
#if SHOW_ALERT_FOR_MSG
            SelfDestructMessageBox *m = new SelfDestructMessageBox(
                300, "New Message Received",
                QString("A new message was received at %1 UTC from %2")
                    .arg(d.utcTimestamp.time().toString())
                    .arg(d.from),
                QMessageBox::Information, QMessageBox::Ok, QMessageBox::Ok,
                false, this);

            m->show();
#endif
        }

        // PROCESS ACKS
        else if (d.cmd == " ACK" && !isAllCall) {
            qCDebug(mainwindow_js8) << "skipping incoming ack" << d.text;

            // [BUILD 331-todo78] Suppress chime only for NUMBERED ACK
            // ("ACK 5" etc. — d.extra holds the chunk-id digit). Plain
            // unnumbered ACK ("just ACK") is a normal QSO ACK and
            // SHOULD chime. The numbered form is always an ARQ chunk
            // ack — chiming once per chunk in a long super-message is
            // pure noise. This supersedes the Build 317 gate that only
            // suppressed during our own active TX session (RX-side
            // ARQ activity also benefits from the suppression now).
            bool const isNumberedAck = !d.extra.trimmed().isEmpty();
            if (!isNumberedAck) {
                tryNotify("ack", d.submode);
            }

            // make sure this is explicit
            continue;
        }

        // PROCESS NACKS — mirror the ACK block so chunked-ARQ NACKs
        // that fell through the early dispatch (for conversation-
        // window visibility) don't trigger an unintended autoreply.
        // NACK lives at slot 2 in directed_cmds and IS a member of
        // autoreply_cmds, so without this explicit skip the
        // reply-construction chain below could fire.
        else if (d.cmd == " NACK" && !isAllCall) {
            qCDebug(mainwindow_js8) << "skipping incoming nack" << d.text;
            // [BUILD 331-todo78] Mirror the ACK branch: suppress chime
            // only for NUMBERED NACK (ARQ chunk NACK). Plain unnumbered
            // NACK chimes normally.
            bool const isNumberedNack = !d.extra.trimmed().isEmpty();
            if (!isNumberedNack) {
                tryNotify("ack", d.submode);
            }
            continue;
        }

        // PROCESS BUFFERED CMD
        else if (d.cmd == " CMD" && !isAllCall) {
            qCDebug(mainwindow_js8) << "skipping incoming command" << d.text;

            // make sure this is explicit
            continue;
        }

        // PROCESS BUFFERED QUERY
        //
        // 2026-06-07 (arq-queryAll): dropped the `!isAllCall` gate so
        // "@ALLCALL QUERY ARQ?" and "@CUSTOM_GROUP QUERY ARQ?" reach
        // this handler. The MSG sub-handler below re-gates on !isAllCall
        // (delivering inbox content to an indeterminate audience makes
        // no sense), but ARQ? capability replies are safe to send to
        // any addressee — the response is identical for every querier.
        // The existing m_txAllcallCommandCache 15-minute per-station
        // rate-limit (line ~411) keeps us from flooding when many
        // stations probe the band with @ALLCALL QUERY ARQ?.
        else if (d.cmd == " QUERY") {
            auto who = d.from; // keep in mind, this is the sender, not the
                               // original requestor if relayed
            auto replyPath = d.from;

            if (d.relayPath.contains(">")) {
                auto path = d.relayPath.split(">");
                who = path.last();
                replyPath = d.relayPath;
            }

            QStringList segs = d.text.split(" ");
            if (segs.isEmpty()) {
                continue;
            }

            auto cmd = segs.first();
            segs.removeFirst();

            if (cmd == "MSG" && !segs.isEmpty() && !isAllCall) {
                auto inbox = Inbox(inboxPath());
                if (!inbox.open()) {
                    continue;
                }

                bool ok = false;
                int mid = QString(segs.first()).toInt(&ok);
                if (!ok) {
                    continue;
                }

                auto msg = inbox.value(mid);
                auto params = msg.params();
                if (params.isEmpty()) {
                    continue;
                }

                auto from = params.value("FROM").toString().trimmed();

                auto to = params.value("TO").toString().trimmed();

                // group messaging - allow any message to a @GROUP to be
                // retrieved by anybody
                bool isGroupMsg = to.startsWith("@");

                // [#170] Same one-sided defect: a message stored
                // for "AL0A/P" was unreachable by "AL0A" asking.
                if (!isGroupMsg && !Radio::same_station(to, who)) {
                    continue;
                }

                auto text = params.value("TEXT").toString().trimmed();
                if (text.isEmpty()) {
                    continue;
                }

                /*
                 * mark as delivered (so subsequent HBs and QUERY MSGS don't
                 * receive this message)
                 *
                 * Do this in callbacks so that messages are only marked read
                 * after any potential confirmations have been made by the user,
                 * and the message has been processed in the transaction queue.
                 */
                if (!isGroupMsg) {
                    callback = [this, mid, msg]() {
                        this->markMsgDelivered(mid, msg);
                    };
                } else {
                    callback = [this, mid, who]() {
                        this->markGroupMsgDeliveredForCallsign(mid, who);
                    };
                }

                // Get number of unread messages
                int pendingCount = countUnreadForCallsign(who);
                if (isGroupCall) {
                    pendingCount += countGroupUnreadForCallsign(d.to, who);
                }

                // We want to send the number of pending messages IN ADDITION TO
                // the one we are about to send, so decrement the actual count
                // We need to decrement 2 here, because "mark as read" occurs
                // in a callback after successful transmission
                pendingCount-=2;

                auto lookaheadMid = getLookaheadMessageIdForCallsign(who, mid);
                if (lookaheadMid == -1 && isGroupMsg) {
                    lookaheadMid =
                        getLookaheadGroupMessageIdForCallsign(d.to, who, mid);
                }

                // and reply
                if (lookaheadMid != -1) {
                    if (pendingCount > 0) {
                        reply = QString("%1 MSG %2 FROM %3 NEXT MSG ID %4 +%5");
                        reply = reply.arg(replyPath);
                        reply = reply.arg(text);
                        reply = reply.arg(from);
                        reply = reply.arg(lookaheadMid);
                        reply = reply.arg(pendingCount);
                    }
                    else {
                        reply = QString("%1 MSG %2 FROM %3 NEXT MSG ID %4");
                        reply = reply.arg(replyPath);
                        reply = reply.arg(text);
                        reply = reply.arg(from);
                        reply = reply.arg(lookaheadMid);
                    }

                } else {
                    reply = QString("%1 MSG %2 FROM %3");
                    reply = reply.arg(replyPath);
                    reply = reply.arg(text);
                    reply = reply.arg(from);
                }
            }

            // QUERY ARQ? — capability interrogation. Wire form
            // "<peer> QUERY ARQ?" parses as cmd=" QUERY" + body="ARQ?"
            // (the regex's bare-QUERY alternation, with the trailing
            // "ARQ?" landing in d.text). Reply with "<peer> YES <level>"
            // where level is the ARQ_PROTOCOL_LEVEL constant. The peer
            // already knows the response is ARQ-related from sending
            // the query; future builds may bump the level when wire
            // semantics change so they can decide whether to use ARQ
            // with us.
            // [QUERY ARQ HANDLER WIDEN 2026-06-11 build 249]
            // Also match the question-mark-less form. Build 239's TX
            // checksum-skip and Build 248's RX checksum-skip both
            // accept "ARQ" and "ARQ?", but until now this handler
            // matched only "ARQ?" — so a manually-typed
            // "<peer> QUERY ARQ" would pass through buffered
            // validation but never produce a YES reply. Now both
            // forms get the response. Standard menu still sends
            // "QUERY ARQ?" so the existing flow is unchanged.
            else if (cmd == "ARQ?" || cmd == "ARQ") {
                int level = ChunkedArq::ARQ_PROTOCOL_LEVEL;
#ifdef JS8_ENABLE_FT2
                // [ssdetect arqcap 2026-09-08] With Subspace RX
                // disabled, do not advertise level >= 3: that arms
                // the peer's V3 NATIVE transport, whose raw frames
                // ride ONLY the Subspace speed we cannot hear (and
                // being non-text, they can never trigger the borrow
                // hook -- the session would die in NACK-silence).
                // Level 2 = V2 text ARQ, proven at legacy speeds
                // incl. Turbo. A BORROWED session counts as enabled
                // and advertises full capability.
                if (!m_l2Enabled)
                    level = qMin(level, 2);
#endif
                reply = QString("%1 YES %2")
                            .arg(replyPath)
                            .arg(level);
                // [BUILD 341 arqReplyAck] Dispatched like an ACK —
                // caller's speed, draft save/restore, direct TX; see
                // the arqProtocolReply declaration.
                arqProtocolReply = true;
            }
        }

        // PROCESS BUFFERED QUERY MSGS
        else if (d.cmd == " QUERY MSGS" &&
                 ui->actionModeAutoreply->isChecked()) {
            auto who = d.from; // keep in mind, this is the sender, not the
                               // original requestor if relayed
            auto replyPath = d.from;

            if (d.relayPath.contains(">")) {
                auto path = d.relayPath.split(">");
                who = path.last();
                replyPath = d.relayPath;
            }

            // if this is an allcall or a directed call, check to see if we have
            // a stored message for user. we reply yes if the user would be able
            // to retreive a stored message
            auto mid = getNextMessageIdForCallsign(who);

            // Get number of unread messages
            int pendingCount = countUnreadForCallsign(who);
            if (isGroupCall) {
                pendingCount += countGroupUnreadForCallsign(d.to, d.from);
            }

            // We want to send the number of pending messages IN ADDITION TO
            // the one we are about to send, so decrement the actual count
            pendingCount--;

            if (mid != -1) {
                if (pendingCount > 0) {
                    reply = QString("%1 YES MSG ID %2 +%3")
                                .arg(replyPath)
                                .arg(mid)
                                .arg(pendingCount);
                }
                else {
                    reply = QString("%1 YES MSG ID %2")
                                .arg(replyPath)
                                .arg(mid);
                }
            }

            // Group messaging - if isGroupCall, check to see if there's a
            // message id for the group and return it if there's not an
            // individual message
            // TODO: include group name in response to differentiate from direct
            // messages?
            else if (isGroupCall) {
                mid = getNextGroupMessageIdForCallsign(d.to, d.from);
                if (mid != -1) {
                    if (pendingCount > 0) {
                        reply = QString("%1 YES MSG ID %2 +%3")
                                    .arg(replyPath)
                                    .arg(mid)
                                    .arg(pendingCount);
                    }
                    else {
                        reply = QString("%1 YES MSG ID %2")
                                    .arg(replyPath)
                                    .arg(mid);
                    }
                }
            }

            // if this is not an allcall and we have no messages, reply no.
            if (!isAllCall && reply.isEmpty()) {
                reply = QString("%1 NO").arg(replyPath);
            }
        }

        // PROCESS BUFFERED QUERY CALL
        else if (d.cmd == " QUERY CALL" &&
                 ui->actionModeAutoreply->isChecked()) {
            auto replyPath = d.from;
            if (d.relayPath.contains(">")) {
                replyPath = d.relayPath;
            }

            auto who = d.text;
            if (who.isEmpty()) {
                continue;
            }

            auto callsigns = Varicode::parseCallsigns(who);
            if (callsigns.isEmpty()) {
                continue;
            }

            QStringList replies;
            int callsignAging = m_config.callsign_aging();
            auto baseCall = callsigns.first();
            foreach (auto cd, m_callActivity.values()) {
                if (callsignAging &&
                    cd.utcTimestamp.secsTo(now) / 60 >= callsignAging) {
                    continue;
                }

                // [#170] Was `baseCall == base_callsign(cd.call)`,
                // which only based ONE side: asking about "AL0A/P"
                // missed every station that logged plain "AL0A".
                if (Radio::same_station(baseCall, cd.call)) {
                    auto r = QString("%1 (%2)")
                                 .arg(Varicode::formatSNR(cd.snr))
                                 .arg(since(cd.utcTimestamp))
                                 .trimmed();
                    replies.append(r);
                    break;
                }
            }

            if (!replies.isEmpty()) {
                replies.prepend(QString("%1 YES").arg(replyPath));
            }

            reply = replies.join(" ");

            if (!reply.isEmpty()) {
                if (isAllCall) {
                    m_txAllcallCommandCache.insert(d.from, new QDateTime(now),
                                                   25);
                }
            }
        }

#if 0
        // PROCESS ALERT
        else if (d.cmd == "!" && !isAllCall) {

            // create alert dialog
            processAlertReplyForCommand(d, d.from, " ");

            // make sure this is explicit
            continue;
        }
#endif

        // well, if there's no reply, don't do anything...
        if (reply.isEmpty()) {
            // [REPLY-GATE DIAG 2026-06-10 build 244] track which gate
            // silently drops the relay-cmd forward TX. Only log when
            // we KNOW we'd otherwise have sent something.
            if (d.cmd == QStringLiteral(">")) {
                qWarning() << "[REPLY-GATE] relay: reply is empty"
                           << "from=" << d.from << "to=" << d.to
                           << "text=" << d.text;
            }
            continue;
        }

        if (d.cmd == QStringLiteral(">")) {
            qWarning() << "[REPLY-GATE] relay: reply built"
                       << "reply=" << reply
                       << "from=" << d.from << "to=" << d.to;
        }

        // do not queue @ALLCALL replies if auto-reply is not checked
        if (!ui->actionModeAutoreply->isChecked() && isAllCall) {
            if (d.cmd == QStringLiteral(">")) {
                qWarning() << "[REPLY-GATE] relay: blocked by autoreply-off+allcall";
            }
            continue;
        }

#if 0
        // TODO: jsherer - HB issue here
        // do not queue a reply if it's a HB and HB is not active
        // if((!ui->hbMacroButton->isChecked() || m_hbInterval <= 0) && d.cmd.contains("HB")){
        //     continue;
        // }
#endif

        // do not queue for reply if there's text in the window
        // (exception: relay ">" and ARQ-protocol replies — both are
        //  obligations initiated by a remote station, not autoreplies
        //  that should defer to the local operator's typing. Queue it;
        //  the TX queue will serialise it after whatever else is
        //  pending.)
        bool const isRelayForward = (d.cmd == QStringLiteral(">"));
        bool const isRemoteObligation = isRelayForward || arqProtocolReply;
        if (!ui->extFreeTextMsgEdit->toPlainText().isEmpty() &&
            !isRemoteObligation) {
            continue;
        }

        // do not queue for reply if there's a buffer open to us
        // (same exception as above)
        int bufferOffset = 0;
        if (hasExistingMessageBufferToMe(&bufferOffset) &&
            !isRemoteObligation) {
            qCDebug(mainwindow_js8) << "skipping reply due to open buffer"
                                    << bufferOffset << m_messageBuffer.count();
            continue;
        }

        // [TODO #112 2026-07-23] ...and do not queue an auto-reply while
        // an ARQ transfer is INBOUND to us (file, web link, or plain
        // super-message). Keying up mid-transfer collides with our own
        // session: half-duplex means we go deaf to the sender while we
        // transmit, our ACK is stomped, and the sender burns a retry —
        // on a 98-chunk transfer that window is ~55 minutes, so a
        // third-party query landing inside it is likely on a busy band.
        // This is the transfer-scale twin of the open-buffer gate right
        // above (operator request; RX side only).
        //
        // arqProtocolReply is exempt via isRemoteObligation, so our own
        // ACK/NACK to the transfer peer keep flowing — that traffic IS
        // the session and must not be suppressed.
        // [arqtwin 2026-08-31] ...and the SEND-side twin (#112 was
        // "RX side only" by the original request; the missing half
        // let third-party replies queue during outbound transfers
        // and key into the between-chunk ACK windows). Either
        // direction now: session active = no courtesy replies;
        // protocol obligations stay exempt.
        if (m_chunkedArq &&
            (m_chunkedArq->hasActiveRxTransfer() ||
             m_chunkedArq->hasActiveTxSession()) &&
            !isRemoteObligation) {
            qWarning() << "[REPLY-GATE] suppressed: ARQ session active"
                       << "from=" << d.from << "cmd=" << d.cmd
                       << "reply=" << reply.left(40);
            continue;
        }
        // [autoroute, operator spec 2026-08-28] "ignore all msgs from
        // stations other than the one we're listening for": during
        // auto-route NO auto-reply goes out at all -- the executor is
        // the mode's only speaker, relay obligations included (the
        // relayed answer that closes the mode is handled upstream by
        // reachOnDirected, and its ACK is already suppressed). RX
        // processing itself is untouched; only replies are dropped.
        if (m_autoRouteActive) {
            qWarning() << "[REPLY-GATE] suppressed: auto-route active"
                       << "from=" << d.from << "cmd=" << d.cmd
                       << "reply=" << reply.left(40);
            continue;
        }
        if (isRelayForward) {
            qWarning() << "[REPLY-GATE] relay: enqueuing forward"
                       << "reply=" << reply
                       << "extEditEmpty=" << ui->extFreeTextMsgEdit->toPlainText().isEmpty()
                       << "openBufferToMe=" << hasExistingMessageBufferToMe(nullptr);
        }

        // add @ALLCALLs to the @ALLCALL cache
        if (isAllCall) {
            m_txAllcallCommandCache.insert(d.from, new QDateTime(now), 25);
        }

        // [BUILD 341 arqSpeed] Reply-at-caller's-speed, applied only
        // once every reply-drop gate above has passed. SAME machinery
        // as the chunked-DATA ACK auto-match (TODO #73 / Build 316,
        // ~line 260): stash the operator's mode once, switch to the
        // caller's; stopTx restores the stash 750 ms after the reply
        // transmits. The frame encodes at TX time from m_nSubMode, so
        // switching here (enqueue) is early enough; processTxQueue
        // dequeues on a later tick.
        if (arqProtocolReply && d.submode != m_nSubMode) {
            if (m_arqPreSwitchSubmode == -1) {
                m_arqPreSwitchSubmode = m_nSubMode;
                qWarning() << "[ARQ-RX] stashed pre-switch submode="
                           << m_arqPreSwitchSubmode
                           << "for post-reply restore (QUERY ARQ?)";
            }
            qWarning() << "[ARQ-RX] caller-speed reply: was="
                       << m_nSubMode << "→ now=" << d.submode
                       << "(peer=" << d.from << ")";
            setSubmode(d.submode);
        }

        // queue the reply here to be sent when a free interval is available on
        // the frequency that was sent unless, this is an allcall, to which we
        // should be responding on a clear frequency offset we always want to
        // make sure that the directed cache has been updated at this point so
        // we have the most information available to make a frequency selection.
        // [BUILD 341 arqReplyAck] ARQ-protocol replies bypass the
        // autoreply queue ENTIRELY and dispatch through the exact
        // ACK / NACK path: onChunkedWantsResponseTx saves the
        // operator's outgoing-box draft, TXes the wire text directly,
        // and stopTx restores the draft (TODO #57) and the submode
        // stash (TODO #73) once the response drains. Same 250 ms
        // audio-ramp delay as Manager::sendAck / sendNack. No
        // PriorityHigh gate, no confirmation dialog, no draft
        // deferral — a capability negotiation on the other end times
        // out in 20 s.
        if (arqProtocolReply) {
            // [BUILD 358 leadmark] The Build 344 registerPeerCandidate
            // call that lived here is DELETED along with hash-based
            // session creation: a receive session now requires the
            // addressed chunk-1 TEXT lead marker (a clipped one costs
            // the sender one retry, which re-airs it). Hash matching
            // only ATTACHES frames to transfers already underway.
            QString const responseText = reply;
            QPointer<UI_Constructor> const self(this);
            QTimer::singleShot(
                ChunkedArq::ACK_TX_DELAY_MS, this,
                [self, responseText]() {
                    if (!self) return;
                    qCWarning(chunkedarq_js8)
                        << "[ARQ-RX] protocol reply via ACK path:"
                        << responseText;
                    self->onChunkedWantsResponseTx(responseText);
                });
            continue;
        }

        if (m_config.autoreply_confirmation()) {
            confirmThenEnqueueMessage(90, priority, reply, freq, callback,
                                      /*autoReply=*/true);
        } else {
            enqueueMessage(priority, reply, freq, callback,
                           /*autoReply=*/true);
        }
    }
}
