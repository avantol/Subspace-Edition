/**
 * @file networkMessage.cpp
 * @brief member function of the UI_Constructor class
 * API commands for external control and data retrieval.
 * sends data to external clients via the Network Message API
 * defgroup API Network Message API
 */

#include "JS8_UI/mainwindow.h"
#include "JS8_UI/SpotMapWindow.h"

#include "JS8_Main/ChunkedArq.h"
#include "JS8_Main/Modes.h" // [phone remote] UI.GET_FREQ_LIST

/**
 * @brief Processes an incoming API network message
 * This function acts as the primary router for the JS8Call API. It handles
 * RIG, STATION, RX, and TX commands by either updating the application
 * state or querying current values to send back to the client.
 *
 * @param message The network message to process
 */
// [TODO #112 2026-07-23] "Is an ARQ transfer in flight?" — either
// direction. Used both to synthesise a non-zero TX queue depth (so
// existing API clients, which already treat non-zero as "don't send",
// need no changes) and to populate the explicit BUSY fields.
bool UI_Constructor::arqBusyNow() const {
    return m_chunkedArq && (m_chunkedArq->hasActiveTxSession() ||
                            m_chunkedArq->hasActiveRxTransfer());
}

// Short machine-readable reason, for clients that want to distinguish
// "wait for a long transfer" from "wait for one frame".
QString UI_Constructor::busyReason() const {
    // [2026-07-23 negophase] Negotiation is the opening phase of a TX
    // session, so BUSY is already true via hasActiveTxSession(); name
    // it distinctly so a client can tell "20-130 s of setup" from
    // "a transfer is actually running". BUSY itself is unchanged, so
    // clients that only read the flag need no update.
    if (m_chunkedArq && m_chunkedArq->isNegotiating())
        return QStringLiteral("arq_negotiating");
    if (m_chunkedArq && m_chunkedArq->hasActiveTxSession())
        return QStringLiteral("arq_tx");
    if (m_chunkedArq && m_chunkedArq->hasActiveRxTransfer())
        return QStringLiteral("arq_rx");
    if (m_transmitting)
        return QStringLiteral("transmitting");
    return QString{};
}

void UI_Constructor::networkMessage(Message const &message) {
    auto type = message.type();

#if 0  // TCP diagnostic logging — enable for JS8 Spotter debugging
    qWarning() << "[TCP-CMD] type=" << type
               << "value=" << message.value().left(100);
#endif

    if (type == "PING") {
        return;
    }

    auto id = message.id();

    qCDebug(mainwindow_js8) << "try processing network message" << type << id;

#ifdef JS8_ENABLE_JTTY
    // [audit D3, operator 2026-10-14: "fail for all api requests in JTTY
    // mode (for now) as gracefully as possible"] Every request is
    // answered with its own type and the REFUSED / REASON "jtty_mode"
    // shape TX.SEND_MESSAGE already uses -- a client that waits for a
    // reply gets one, and a known field never carries a new value.
    // RIG.TX_HALT is the one exception: a request to STOP transmitting
    // is honoured in every mode. Each refused type is logged once per
    // JTTY-mode spell, so a polling client does not flood the log.
    // [operator 2026-10-14, "yes"] ...and at JTTY SPEED in JS8 mode too:
    // a form or any message over one 80-character JTTY message arrives
    // cut after its first piece (only that piece carries "FROM: TO"),
    // its checksum is dropped, and the JTTY receive path sends an API
    // client no progressive RX.ACTIVITY -- a clear refusal beats a form
    // that silently arrives broken. The REASON stays "jtty_mode": a new
    // value in a known field is what the API rule forbids.
    // [operator 2026-10-17, "good design! build 1 - 4"] ...replaced by a
    // mode STATEMENT. A client that knows about JTTY says so in the
    // request -- "MODE":"JTTY", or at JTTY speed in JS8 mode
    // "SPEED":"JTTY" -- and only then may it transmit, or move the
    // station INTO JTTY (MODE.SET_SPEED 32, RIG.SET_FREQ to a JTTY
    // dial), or change speed/frequency while in JTTY. A client that does
    // not say so is refused with REASON "mode_mismatch", so an old client
    // behaves exactly as before (refused) and nobody lands in JTTY by
    // accident. Reads and local settings are open to every client; every
    // reply says which mode the station is in (sendNetworkMessage).
    // ARQ, chunked sends and reaching do not exist in JTTY: "jtty_mode".
    {
        static QSet<QString> refusedLogged;
        auto const params = message.params();
        auto const isJttyWord = [](QVariant const &v) {
            return v.toString().trimmed().compare(QStringLiteral("JTTY"),
                                                  Qt::CaseInsensitive) == 0;
        };
        bool const stated =
            isJttyWord(params.value("MODE")) || isJttyWord(params.value("SPEED"));
        bool const jttyNow = jttyMode() || m_nSubMode == Varicode::JS8CallJTTY;
        QString reason;
        if (jttyNow && (type == "TX.SEND_CHUNKED" || type == "TX.REACH" ||
                        type == "MODE.SET_ARQ")) {
            reason = QStringLiteral("jtty_mode");
        } else if (type == "MODE.SET_SPEED") {
            bool ok = false;
            int const speed = params.value("SPEED").toInt(&ok);
            bool const toJtty = ok && speed == Varicode::JS8CallJTTY;
            if (toJtty && m_superSpotterSeen)
                reason = QStringLiteral("jtty_mode"); // MAGNET: no JTTY at all
            else if ((toJtty || jttyNow) && !stated)
                reason = QStringLiteral("mode_mismatch");
        } else if (type == "RIG.SET_FREQ") {
            bool ok = false;
            auto const dial = params.value("DIAL").toLongLong(&ok);
            bool const toJtty = ok && isJttyFrequency(dial);
            bool const moves = params.contains("DIAL") || params.contains("OFFSET");
            if ((toJtty || (jttyNow && moves)) && !stated)
                reason = QStringLiteral("mode_mismatch");
        } else if (jttyNow && (type == "TX.SEND_MESSAGE" ||
                               type == "TX.SEND_DIRECTED" ||
                               type == "RIG.SET_TUNE") && !stated) {
            reason = QStringLiteral("mode_mismatch");
        }
        if (!reason.isEmpty()) {
            if (!refusedLogged.contains(type + reason)) {
                refusedLogged.insert(type + reason);
                qWarning() << "[API] refused" << type << reason
                           << (reason == QStringLiteral("jtty_mode")
                                   ? "(not available in JTTY)"
                                   : "(JTTY: the request must state MODE or SPEED JTTY)");
            }
            sendNetworkMessage(type, "",
                               {{"_ID", params.value("_ID", QVariant(-1))},
                                {"REFUSED", true},
                                {"REASON", reason}});
            return;
        }
        if (!jttyNow)
            refusedLogged.clear();
    }
#endif

    // Inspired by FLDigi
    // TODO: MAIN.RX - Turn on RX
    // TODO: MAIN.TX - Transmit
    // TODO: MAIN.AUTO - Auto
    // TODO: MAIN.HB - HB

    // RIG.GET_PTT  - Returns PTT status
    // RIG.SET_TUNE - Turns TUNE on and off
    // RIG.TX_HALT  - Stops transmission immediately
    // RIG.GET_FREQ - Get the current Frequency
    // RIG.SET_FREQ - Set the current Frequency
    /**
     * @name RIG Commands
     * RIG related API calls
     */
     /** @{ */

    /** @brief RIG.GET_PTT
     * Returns the PTT status
     * @note API 2.6+
     */
    if (type == "RIG.GET_PTT") {
        bool isPTT = m_transmitting;
        sendNetworkMessage("RIG.PTT_STATUS", "",
            {
                {"_ID", id},
                {"PTT", QVariant(isPTT)},
                {"MESSAGE", QVariant(isPTT ? m_currentMessage : "")}
           });
        return;
    }

    /** @brief RIG.SET_TUNE
     * Turns TUNE on and off
     * @note API 2.6+
     */
    if (type == "RIG.SET_TUNE") {
        auto value = QVariant(message.value());
        UI_Constructor::on_tuneButton_clicked(value.toBool());
          sendNetworkMessage("RIG.SET_TUNE", "", {
            {"_ID", id},
            {"value", ui->tuneButton->isChecked()}
          });
        return;
    }

    /** @brief RIG.TX_HALT
     * Stops transmission immediately, consider this an E-stop for the rig
     * @note API 2.6+
     */
    if (type == "RIG.TX_HALT") {
        auto value = QVariant(message.value());
        UI_Constructor::stopTxMechanical(); // [BUILD 353 haltwrap] API E-stop: mechanical only, never haltAll
          sendNetworkMessage("RIG.TX_HALT", "", {
            {"_ID", id},
            {"value", ui->monitorTxButton->isChecked()}
          });
        return;
    }

    /**
     * @brief RIG.GET_FREQ: Retrieves the current dial and offset frequencies.
     *
     * If the WSJT-X protocol is enabled, this also triggers a status update
     * via the @ref m_wsjtxMessageMapper.
     */
    if (type == "RIG.GET_FREQ") {
        // Send WSJT-X Status message if protocol is enabled
        if (m_wsjtxMessageMapper && m_config.wsjtx_protocol_enabled()) {
            QString dx_call = callsignSelected();
            QString dx_grid = "";
            if (!dx_call.isEmpty() && m_callActivity.contains(dx_call)) {
                dx_grid = m_callActivity[dx_call].grid;
            }
            QString tx_message = m_transmitting ? m_currentMessage : "";

            m_wsjtxMessageMapper->sendStatusUpdate(
                dialFrequency(), freq(),
                "JS8", // mode
                dx_call, m_config.my_callsign(), m_config.my_grid(), dx_grid,
                true, // tx_enabled
                m_transmitting,
                m_decoderBusy || m_monitoring, // decoding
                tx_message);
        }

        // Send native JSON message only if not conflicting with WSJT-X
        bool skip_json = false;
        if (m_config.wsjtx_protocol_enabled() &&
            m_config.wsjtx_server_port() == m_config.udp_server_port() &&
            m_config.wsjtx_server_name() == m_config.udp_server_name()) {
            skip_json = true;
        }

        if (!skip_json) {
            sendNetworkMessage(
                "RIG.FREQ", "",
                {{"_ID", id},
                 {"FREQ", QVariant((quint64)dialFrequency() + freq())},
                 {"DIAL", QVariant((quint64)dialFrequency())},
                 {"OFFSET", QVariant((quint64)freq())}});
        }
        return;
    }
    /**
     * @brief RIG.SET_FREQ: Updates the rig dial frequency and/or frequency
     * offset.
     */
    if (type == "RIG.SET_FREQ") {
        auto params = message.params();
        if (params.contains("DIAL")) {
            bool ok = false;
            auto f = params["DIAL"].toInt(&ok);
            if (ok) {
                setRig(f);
                displayDialFrequency();
            }
        }
        if (params.contains("OFFSET")) {
            bool ok = false;
            auto f = params["OFFSET"].toInt(&ok);
            if (ok) {
                setFreqOffsetForRestore(f, false);
            }
        }
    }
    /** @} */ // End RIG Commands

    // STATION refers to JS8Call station settings
    // STATION.GET_CALLSIGN - Get the current callsign
    // STATION.GET_GRID - Get the current grid locator
    // STATION.SET_GRID - Set the current grid locator
    // STATION.GET_INFO - Get the current station qth
    // STATION.SET_INFO - Set the current station qth
    // STATION.GET_SPOT - Get the current spotting status
    // STATION.SET_SPOT - Set the current spotting status
    // STATION.GET_OS   - Get basic info about the OS we are running on
    // STATION.VERSION  - Get the JS8Call version
    /**
     * @name STATION Commands
     * STATION related API calls
     * These calls refer to JS8Call station settings
     */
    /** @{ */

    /** @brief STATION.GET_CALLSIGN: Returns the configured station callsign. */
    if (type == "STATION.GET_CALLSIGN") {
        sendNetworkMessage("STATION.CALLSIGN", m_config.my_callsign(),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /** @brief STATION.GET_GRID: Returns the current Maidenhead grid locator. */
    if (type == "STATION.GET_GRID") {
        sendNetworkMessage("STATION.GRID", m_config.my_grid(),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /** @brief STATION.SET_GRID: Updates the dynamic grid locator for the
     * station. */
    if (type == "STATION.SET_GRID") {
        m_config.set_dynamic_location(message.value());
        sendNetworkMessage("STATION.GRID", m_config.my_grid(),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /** @brief STATION.GET_INFO: Retrieves the station information (QTH). */
    if (type == "STATION.GET_INFO") {
        sendNetworkMessage("STATION.INFO", m_config.my_info(),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /** @brief STATION.SET_INFO: Updates the dynamic station information (QTH).
     */
    if (type == "STATION.SET_INFO") {
        m_config.set_dynamic_station_info(message.value());
        sendNetworkMessage("STATION.INFO", m_config.my_info(),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /** @brief STATION.GET_STATUS: Retrieves the current station status message.
     */
    if (type == "STATION.GET_STATUS") {
        sendNetworkMessage("STATION.STATUS", m_config.my_status(),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /** @brief STATION.GET_BUSY: [TODO #112] Is the program busy — i.e.
     * would transmitting right now collide with something already in
     * progress? True while an ARQ transfer is in flight in EITHER
     * direction (file, web link or plain super-message) or while we are
     * keyed. Clients that cannot be updated should instead just honour
     * TX.QUEUE_DEPTH, which now reads non-zero for the whole session. */
    if (type == "STATION.GET_BUSY") {
        sendNetworkMessage("STATION.BUSY", busyReason(),
                           {
                               {"_ID", id},
                               {"BUSY", QVariant(arqBusyNow() ||
                                                 m_transmitting)},
                               {"BUSY_REASON", busyReason()},
                               {"PTT", QVariant(m_transmitting)},
                               {"ARQ_TX_ACTIVE",
                                QVariant(m_chunkedArq &&
                                         m_chunkedArq->hasActiveTxSession())},
                               {"ARQ_RX_ACTIVE",
                                QVariant(m_chunkedArq &&
                                         m_chunkedArq->hasActiveRxTransfer())},
                           });
        return;
    }
    /** @brief STATION.SET_STATUS: Updates the dynamic station status message.
     */
    if (type == "STATION.SET_STATUS") {
        m_config.set_dynamic_station_status(message.value());
        sendNetworkMessage("STATION.STATUS", m_config.my_status(),
                           {
                               {"_ID", id},
                           });
        return;
    }

    /** @brief STATION.VERSION
     * Returns the JS8Call version
     * @note API 2.6+
     */
    if (type == "STATION.VERSION") {
        QString ver = version();
        sendNetworkMessage("STATION.VERSION", "",
            {
                {"_ID", id},
                {"VERSION", QVariant(ver)}
            });
        return;
    }

    /** @brief STATION.GET_OS
     * Returns OS information for the station
     * @note API 2.6+
     *
     * Thanks to N0GQ Jeff Francis
     */
    if(type == "STATION.GET_OS"){
      sendNetworkMessage("STATION.GET_OS", "", {
	      {"OS_NAME", QSysInfo::prettyProductName()},
	      {"OS_KERNEL", QSysInfo::kernelType()},
	      {"OS_KERNEL_VERSION", QSysInfo::kernelVersion()},
	      {"_ID", id}
        });
        return;
    }

    /** @brief STATION.GET_SPOT
     * Get the current spotting status
     * @note API 2.6+
     *
     * Thanks to N0GQ Jeff Francis
     */
    if(type == "STATION.GET_SPOT") {
        sendNetworkMessage("STATION.SPOT", "", {
          {"value", ui->spotButton->isChecked()},
	      {"_ID", id}
        });
        return;
    }

    /** @brief STATION.SET_SPOT
     * Set the current spotting status
     * @note API 2.6+
     *
     * Thanks to N0GQ Jeff Francis
     */
if(type == "STATION.SET_SPOT") {
        auto value = QVariant(message.value());
          UI_Constructor::on_spotButton_clicked(value.toBool());
          sendNetworkMessage("STATION.SPOT", "", {
            {"value", ui->spotButton->isChecked()},
            {"_ID", id}
          });
          return;
    }

    /** @brief MODE.GET_ARQ
     * Returns whether the chunked-ARQ button is currently enabled
     * (TODO.md #60 build 268).
     *
     * Response: `MODE.ARQ` with `VALUE` = true / false (boolean).
     */
    if (type == "MODE.GET_ARQ") {
        bool const enabled =
            ui->actionModeReplicatorProtocol &&
            ui->actionModeReplicatorProtocol->isChecked();
        sendNetworkMessage("MODE.ARQ", "", {
            {"VALUE", enabled},
            {"_ID", id},
        });
        return;
    }

    /** @brief MODE.SET_ARQ
     * Sets the chunked-ARQ enabled state programmatically. Pairs with
     * TX.SEND_CHUNKED so external clients (msg-rotator, scripts,
     * custom UIs) can flip ARQ on before sending and off after
     * (TODO.md #60 build 268). Mirrors the GUI button — fires the
     * same `on_actionModeReplicatorProtocol_toggled` slot which
     * handles the Manager wiring, modulator relax, mode_label,
     * updateButtonDisplay, AND the multi-mode RX override latch.
     *
     * Param VALUE = true / false (boolean). Response: `MODE.ARQ`
     * with current state after the toggle.
     */
    if (type == "MODE.SET_ARQ") {
        bool const requested = QVariant(message.value()).toBool();
        if (ui->actionModeReplicatorProtocol) {
            ui->actionModeReplicatorProtocol->setChecked(requested);
        }
        bool const enabled =
            ui->actionModeReplicatorProtocol &&
            ui->actionModeReplicatorProtocol->isChecked();
        sendNetworkMessage("MODE.ARQ", "", {
            {"VALUE", enabled},
            {"_ID", id},
        });
        return;
    }

    /** @} */ // End STATION Commands

    // RX.GET_CALL_ACTIVITY
    // RX.GET_CALL_SELECTED
    // RX.GET_BAND_ACTIVITY
    // RX.GET_TEXT
    /**
     * @name RX Commands
     * RX related API calls
     * Refers to received data and activity
     */
    /** @{ */

    /**
     * @brief RX.GET_CALL_ACTIVITY: Returns a list of active callsigns.
     * Filters results based on the `callsign_aging` configuration.
     */
    /** @brief RX.GET_SPOT_MAP: [#168 2026-08-21] DEBUG/TEST dump of the
     * Spots Map's live state — the hearing mesh (who hears whom, with
     * ages and SNRs), every spot including internet-sourced ones with
     * the reporting station, and the grid authority.
     *
     * Exists because that state lives only in RAM and no API reached
     * it, so offline tooling was blind to the single most useful fact
     * for routing: who is on the air THIS MINUTE and who is hearing
     * them. Optional "BAND" param selects a band; default is the one
     * currently displayed. Read-only. Structure is a debug surface,
     * not a compatibility promise. */
    if (type == "RX.GET_SPOT_MAP") {
        if (!m_spotMapWindow) {
            sendNetworkMessage("RX.SPOT_MAP", "",
                               {{"_ID", id}, {"ERROR", "no spot map"}});
            return;
        }
        auto dump = m_spotMapWindow->dumpState(
            message.params().value("BAND").toString());
        dump["_ID"] = id;
        sendNetworkMessage("RX.SPOT_MAP", "", dump);
        return;
    }
    if (type == "RX.GET_CALL_ACTIVITY") {
        auto now = DriftingDateTime::currentDateTimeUtc();
        int callsignAging = m_config.callsign_aging();
        QVariantMap calls = {
            {"_ID", id},
        };

        foreach (auto cd, m_callActivity.values()) {
            if (callsignAging &&
                cd.utcTimestamp.secsTo(now) / 60 >= callsignAging) {
                continue;
            }
            QVariantMap detail;
            detail["SNR"] = QVariant(cd.snr);
            detail["GRID"] = QVariant(cd.grid);
            detail["UTC"] = QVariant(cd.utcTimestamp.toMSecsSinceEpoch());
            calls[cd.call] = QVariant(detail);
        }

        sendNetworkMessage("RX.CALL_ACTIVITY", "", calls);
        return;
    }
    /** @brief RX.GET_CALL_SELECTED: Returns the currently selected callsign. */
    if (type == "RX.GET_CALL_SELECTED") {
        sendNetworkMessage("RX.CALL_SELECTED", callsignSelected(),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /** @brief RX.SET_CALL_SELECTED: Select (or deselect) a callsign.
     *
     * value = callsign to select, empty string = deselect.
     * Uses the same selectCallsign() / clearCallsignSelected() paths as
     * the click handlers, so HB-pause-on-QSO + restore-on-deselect
     * (Build 144) and the per-callsign auto-mode-switch behave
     * identically to a UI click. Responds with RX.CALL_SELECTED
     * carrying the resulting selection (empty if deselect succeeded
     * or the requested call wasn't accepted).
     */
    if (type == "RX.SET_CALL_SELECTED") {
        auto const requested = message.value().trimmed().toUpper();
        if (requested.isEmpty()) {
            clearCallsignSelected();
        } else {
            selectCallsign(requested);
        }
        sendNetworkMessage("RX.CALL_SELECTED", callsignSelected(),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /**
     * @brief RX.GET_BAND_ACTIVITY: Returns recent band activity details.
     * Includes frequency, offset, text, SNR, and UTC timestamp for each entry.
     */
    if (type == "RX.GET_BAND_ACTIVITY") {
        QVariantMap offsets = {
            {"_ID", id},
        };
        for (auto const [offset, activity] : m_bandActivity.asKeyValueRange()) {
            if (activity.isEmpty())
                continue;

            auto const d = activity.last();

            // Backward-compatible fields: latest entry at this offset
            QVariantMap entry{
                {"FREQ", QVariant(d.dial + d.offset)},
                {"DIAL", QVariant(d.dial)},
                {"OFFSET", QVariant(d.offset)},
                {"TEXT", QVariant(d.text)},
                {"SNR", QVariant(d.snr)},
                {"UTC", QVariant(d.utcTimestamp.toMSecsSinceEpoch())}};

            // Add a HISTORY array containing *all* recent decodes at this
            // offset, not just the last one. Fixes API/UI divergence where
            // the UI walks the full list but the API used to drop earlier
            // callsigns that landed on the same offset within the
            // 10-deep ring buffer.
            QVariantList history;
            for (auto const &h : activity) {
                history.append(QVariant(QVariantMap{
                    {"TEXT", QVariant(h.text)},
                    {"SNR", QVariant(h.snr)},
                    {"UTC", QVariant(h.utcTimestamp.toMSecsSinceEpoch())}}));
            }
            entry["HISTORY"] = history;

            offsets[QString("%1").arg(offset)] = QVariant(entry);
        }

        sendNetworkMessage("RX.BAND_ACTIVITY", "", offsets);
        return;
    }
    /** @brief RX.GET_TEXT: Retrieves the current RX text buffer. */
    if (type == "RX.GET_TEXT") { /** RX.GET_TEXT */
        sendNetworkMessage("RX.TEXT", ui->textEditRX->toPlainText().right(1024),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /** @brief RX.CLEAR_OFFSET: Purge band-activity history for a specific
     * offset. Useful for automation to drop a stale or unwanted entry without
     * clearing the whole band pane. Param OFFSET (int, Hz). */
    if (type == "RX.CLEAR_OFFSET") {
        auto ok = false;
        int off = message.params().value("OFFSET", -1).toInt(&ok);
        bool cleared = false;
        if (ok && m_bandActivity.contains(off)) {
            m_bandActivity.remove(off);
            displayBandActivity();
            cleared = true;
        }
        sendNetworkMessage("RX.CLEAR_OFFSET", "",
                           {
                               {"_ID", id},
                               {"OFFSET", off},
                               {"CLEARED", cleared},
                           });
        return;
    }
    /** @} */ // End RX Commands

    // TX.GET_TEXT - Retrieves the current TX text buffer.
    // TX.SET_TEXT - Updates the TX text buffer with new content.
    // TX.SEND_MESSAGE - Enqueues a message for transmission.
    // TX.GET_QUEUE_DEPTH - Return the number of items in the transmit queue.
    /**
     * @name TX Commands
     * TX related API calls
     * Refers to transmitted data and activity
     */
    /** @{ */

    /** @brief TX.GET_TEXT: Retrieves the current TX text buffer. */
    if (type == "TX.GET_TEXT") {
        sendNetworkMessage("TX.TEXT",
                           ui->extFreeTextMsgEdit->toPlainText().right(1024),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /** @brief TX.SET_TEXT: Updates the TX text buffer with new content. */
    if (type == "TX.SET_TEXT") {
#ifdef JS8_ENABLE_FT2
        // [ss701] Second SuperSpotter signature (operator ruling
        // 2026-09-08): the 701A/B/C split forms exist ONLY in the
        // @MAGNET ecosystem (upstream JS8Spotter ships a single
        // MCF701), and a pure SERVING station -- whose Expect
        // auto-replies never emit the probe burst -- must pass the
        // form ID through the TX injection API to answer a pull.
        // API-INJECTED text only; received RF proves nothing about
        // the local client.
        if (isMagnetFormText(message.value()))   // one authority (#222)
            noteSuperSpotterSeen();
#endif
        addMessageText(message.value(), true);
        sendNetworkMessage("TX.TEXT",
                           ui->extFreeTextMsgEdit->toPlainText().right(1024),
                           {
                               {"_ID", id},
                           });
        return;
    }
    /** @brief TX.SEND_MESSAGE: Enqueues a message for transmission.
     * Optional param PRIORITY: "HIGH" (default), "NORMAL", or "LOW".
     */
    /** @brief TX.ATTEMPT_DONE: the caller has STOPPED WAITING on the
     * calls it made, so the Spots Map should blank their dashed paths
     * now instead of running its own countdown to the end. The map's
     * budget is an estimate of how long a reply might take; only the
     * caller knows when it actually gave up, and it is usually sooner
     * (operator, 2026-08-22: "blank it the moment we declare failure,
     * helps the user move along to next thing").
     *
     * Replied attempts are untouched -- a green path is a result, not
     * an outstanding wait. Read-only otherwise; affects display only.
     */
    if (type == "TX.ATTEMPT_DONE") {
        if (m_spotMapWindow)
            m_spotMapWindow->clearAttempts();
        return;
    }

    /** @brief TX.REACH: run one reaching attempt at the callsign in
     * `value` -- the in-app port of tools/js8reach/attempt.py
     * ([reachport], reachExecutor.cpp). Optional param MAX_MOVES.
     * Ledger lines land in the diag log under [REACH].
     * TX.REACH_STOP halts the attempt and restores speed.
     */
    if (type == "TX.REACH") {
        auto ok = false;
        auto const mm =
            message.params().value("MAX_MOVES", QVariant(6)).toInt(&ok);
        reachStart(message.value(), ok ? mm : 6,
                   message.params().value("VIA").toString());
        return;
    }
    if (type == "TX.REACH_STOP") {
        reachStop(QStringLiteral("stopped by operator"));
        return;
    }

    if (type == "TX.SEND_MESSAGE") {
        // [apiquiet, operator 2026-09-01] The mode owns the radio: an
        // API-injected send mid-auto-route would drain into a
        // listening window between moves (JS8Assistant-style
        // companions inject sends routinely). Refused loudly, same
        // pattern as the MODE.SET_SPEED lock.
        if (m_autoRouteActive) {
            qWarning() << "[API] TX.SEND_MESSAGE refused:"
                       << "auto-route active";
            sendNetworkMessage("TX.SEND_MESSAGE", "",
                               {{"_ID", message.params().value(
                                            "_ID", QVariant(-1))},
                                {"REFUSED", true},
                                {"REASON",
                                 QStringLiteral("auto_route_active")}});
            return;
        }
        // [JTTY] Here only with the mode stated (gate at the top).
        auto text = message.value();
#ifdef JS8_ENABLE_JTTY
        // [operator 2026-10-17] A JTTY message holds 80 characters. At
        // JTTY speed in JS8 mode a MSG / MSG TO: is stored only from the
        // message carrying the command, so a longer one is REFUSED
        // ("too_long", the operator's "Message is too long for JTTY
        // speed."). Anything else longer is split, and the reply says
        // into how many JTTY messages.
        int jttyMessages = 0;
        if (!text.isEmpty() && m_nSubMode == Varicode::JS8CallJTTY) {
            if (jttyMsgTooLong(text, &jttyMessages)) {
                qWarning() << "[API] TX.SEND_MESSAGE refused: a MSG needs"
                           << jttyMessages << "JTTY messages";
                sendNetworkMessage("TX.SEND_MESSAGE", "",
                                   {{"_ID", id},
                                    {"REFUSED", true},
                                    {"REASON", QStringLiteral("too_long")},
                                    {"JTTY_MESSAGES", jttyMessages}});
                return;
            }
            if (!jttyMessages) jttyMessages = jttyMessagesFor(text);
        }
#endif
#ifdef JS8_ENABLE_FT2
        // [ss701] Same second signature as TX.SET_TEXT above -- the
        // Expect auto-reply path sends the form text through HERE
        // (SET_TEXT "" first, full text only in SEND_MESSAGE).
        if (isMagnetFormText(text))              // one authority (#222)
            noteSuperSpotterSeen();
#endif
        if (!text.isEmpty()) {
            auto priStr = message.params().value("PRIORITY", "HIGH")
                              .toString().toUpper();
            int pri = (priStr == "LOW") ? PriorityLow
                    : (priStr == "NORMAL") ? PriorityNormal
                    : PriorityHigh;
            enqueueMessage(pri, text, -1, nullptr);
            processTxQueue();
#ifdef JS8_ENABLE_JTTY
            if (jttyMessages > 1)
                sendNetworkMessage("TX.SEND_MESSAGE", "",
                                   {{"_ID", id},
                                    {"SPLIT", true},
                                    {"JTTY_MESSAGES", jttyMessages}});
#endif
            return;
        }
    }

    /**
     * @brief Return the number of items in the transmit queue.
     * @note API 2.6+
     * 
     * Thanks to N0GQ Jeff Francis
     */
    if(type == "TX.GET_QUEUE_DEPTH"){
      int depth = m_txMessageQueue.size();
      // [TODO #112 2026-07-23] Report BUSY as a non-zero depth so that
      // EXISTING clients need no changes: they already treat non-zero as
      // "don't send". This extends the convention the line below has
      // always used (transmitting + empty queue => report 1) from a
      // single frame to a whole ARQ session. Without it a client polling
      // this sees an IDLE station in the middle of a ~55 minute
      // transfer, because the message queue genuinely empties between
      // chunks — the same false-idle reading that once armed our own ACK
      // timer mid-chunk. Value stays 1 rather than the remaining-chunk
      // count so clients that size buffers or display this are not
      // surprised; precise state is in the explicit fields on
      // QSO.CONTEXT / STATION.BUSY.
      if(arqBusyNow() && depth==0) depth=1;
      if(m_transmitting && depth==0) depth=1;
      sendNetworkMessage("TX.QUEUE_DEPTH", "", {
	  {"_ID", id},
	  {"DEPTH", QVariant(depth)}
	});
      return;
    }
    /** @brief TX.GET_ENABLED: [txquery, operator 2026-10-28] the TX
     * enable button's state, for an outside program (phone, arbiter)
     * that needs to know whether the station may transmit at all.
     * Reply TX.ENABLED: ENABLED (the button), REMOTE (true when it was
     * turned off by a remote command, UI.ACTION txenable, and has not
     * changed since). Read-only; UI.ACTION txenable changes it.
     */
    if (type == "TX.GET_ENABLED") {
        bool const enabled = ui->monitorTxButton->isChecked();
        sendNetworkMessage("TX.ENABLED", "",
                           {{"_ID", id},
                            {"ENABLED", enabled},
                            {"REMOTE", m_txDisabledRemote && !enabled},
                            {"REASON", m_txDisabledRemote && !enabled ? m_txHoldReason : QString()}});
        return;
    }
    /** @brief TX.SET_ENABLED: [txset, operator 2026-10-28] the set form
     * of UI.ACTION txenable, for an arbiter: ENABLED true/false is applied
     * only when it differs from the button, through the button's own
     * click (off = the full operator halt, #253), so a watcher never
     * toggles the wrong way when the operator's click lands between its
     * query and its command. Reply TX.ENABLED with ENABLED, REMOTE,
     * CHANGED (false when already in that state), OK/REASON ("disabled"
     * when the button is unavailable, "missing" without ENABLED). The
     * _ID -1 TX.ENABLED event follows every real change.
     */
    if (type == "TX.SET_ENABLED") {
        QAbstractButton *b = ui->monitorTxButton;
        QVariant const want = message.params().value("ENABLED");
        QString reason;
        if (!want.isValid())
            reason = QStringLiteral("missing");
        else if (!b->isVisible() || !b->isEnabled())
            reason = QStringLiteral("disabled");
        bool const changed = reason.isEmpty() && b->isChecked() != want.toBool();
        qWarning() << "[REMOTE] TX.SET_ENABLED" << want.toBool()
                   << (reason.isEmpty() ? (changed ? "applied" : "no change") : reason);
        // [holdreason] REASON names the holder's cause ("VARA link");
        // "remote command" when none given
        QString const why = message.params().value("REASON").toString().simplified();
        if (changed) {
            m_txHoldReason = why.isEmpty() ? QStringLiteral("remote command") : why.left(40);
            m_txEnableByRemote = true;
            b->click();
            m_txEnableByRemote = false;
        }
        bool const enabled = b->isChecked();
        bool const held = m_txDisabledRemote && !enabled;
        // [holdtimeout] a hold lives kRemoteHoldMaxMs; the same SET again
        // while held renews it (and may rename the reason)
        if (held && reason.isEmpty() && !want.toBool()) {
            if (!changed && !why.isEmpty()) m_txHoldReason = why.left(40);
            armRemoteHold(true);
        }
        sendNetworkMessage("TX.ENABLED", "",
                           {{"_ID", id},
                            {"ENABLED", enabled},
                            {"REMOTE", held},
                            {"HOLD_S", held ? kRemoteHoldMaxMs / 1000 : 0},
                            {"CHANGED", changed},
                            {"OK", reason.isEmpty()},
                            // the refusal when refused, else the hold's reason
                            {"REASON", !reason.isEmpty() ? reason : held ? m_txHoldReason : QString()}});
        remotePushChanged();
        return;
    }
    /** @brief TX.GET_AUTOREPLY / TX.SET_AUTOREPLY: [autoset, operator
     * 2026-10-28] the Autoreply switch (Mode > Enable Autoreply, the
     * AUTO gate), same shape as TX.GET_ENABLED / TX.SET_ENABLED: the
     * soft hold for an arbiter -- no NEW auto-replies start, the
     * operator's own sends are untouched, nothing on the air is cut.
     * SET applies ENABLED only when it differs, through the menu
     * action's own trigger. Reply TX.AUTOREPLY with ENABLED, REMOTE
     * (off by a remote command), and for SET also CHANGED, OK/REASON
     * ("disabled" = the switch is unavailable, JTTY mode; "missing").
     * The _ID -1 TX.AUTOREPLY event follows every real change.
     */
    if (type == "TX.GET_AUTOREPLY") {
        bool const enabled = ui->actionModeAutoreply->isChecked();
        sendNetworkMessage("TX.AUTOREPLY", "",
                           {{"_ID", id},
                            {"ENABLED", enabled},
                            {"REMOTE", m_autoDisabledRemote && !enabled},
                            {"REASON", m_autoDisabledRemote && !enabled ? m_autoHoldReason : QString()}});
        return;
    }
    if (type == "TX.SET_AUTOREPLY") {
        QAction *a = ui->actionModeAutoreply;
        QVariant const want = message.params().value("ENABLED");
        QString reason;
        if (!want.isValid())
            reason = QStringLiteral("missing");
        else if (!a->isEnabled())
            reason = QStringLiteral("disabled");
        bool const changed = reason.isEmpty() && a->isChecked() != want.toBool();
        qWarning() << "[REMOTE] TX.SET_AUTOREPLY" << want.toBool()
                   << (reason.isEmpty() ? (changed ? "applied" : "no change") : reason);
        QString const why = message.params().value("REASON").toString().simplified();
        if (changed) {
            m_autoHoldReason = why.isEmpty() ? QStringLiteral("remote command") : why.left(40);
            m_autoByRemote = true;
            a->trigger();
            m_autoByRemote = false;
        }
        bool const enabled = a->isChecked();
        bool const held = m_autoDisabledRemote && !enabled;
        if (held && reason.isEmpty() && !want.toBool()) { // [holdtimeout] arm / renew
            if (!changed && !why.isEmpty()) m_autoHoldReason = why.left(40);
            armRemoteHold(false);
        }
        sendNetworkMessage("TX.AUTOREPLY", "",
                           {{"_ID", id},
                            {"ENABLED", enabled},
                            {"REMOTE", held},
                            {"HOLD_S", held ? kRemoteHoldMaxMs / 1000 : 0},
                            {"CHANGED", changed},
                            {"OK", reason.isEmpty()},
                            {"REASON", !reason.isEmpty() ? reason : held ? m_autoHoldReason : QString()}});
        remotePushChanged();
        return;
    }
    /** @brief TX.SEND_DIRECTED: Build and enqueue a properly-formatted
     * directed message. Agent supplies TO, CMD, optional EXTRA; server
     * composes `<MYCALL>: <TO> <CMD> <EXTRA>` and validates the TO
     * callsign. Prevents the classic FROM/TO reversal mistake.
     *
     * Params:
     *   TO       — destination callsign or @GROUP (required)
     *   CMD      — directive/body word like "SNR", "SNR?", "STATUS?", "DN61"
     *   EXTRA    — optional trailing token (e.g. "-13" for SNR report)
     *   PRIORITY — "HIGH" (default), "NORMAL", "LOW"
     */
    if (type == "TX.SEND_DIRECTED") {
        auto to = message.params().value("TO").toString().trimmed();
        auto cmd = message.params().value("CMD").toString().trimmed();
        auto extra = message.params().value("EXTRA").toString().trimmed();
        auto priStr = message.params().value("PRIORITY", "HIGH")
                          .toString().toUpper();

        // Minimal validation: TO must be a plausible callsign or @GROUP.
        static const QRegularExpression callRe(
            R"(^(@[A-Z0-9]+|[A-Z0-9/]{3,15})$)");
        bool okCall = callRe.match(to.toUpper()).hasMatch();

        QString built;
        QString err;
        if (!okCall || to.isEmpty()) {
            err = QString("invalid TO '%1'").arg(to);
        } else if (cmd.isEmpty() && extra.isEmpty()) {
            err = "empty CMD and EXTRA";
        } else {
            built = QString("%1: %2 %3").arg(m_config.my_callsign())
                        .arg(to.toUpper()).arg(cmd);
#ifdef JS8_ENABLE_JTTY
            // [operator 2026-10-17] JTTY mode: the WSJT-X order,
            // "<TO> <MYCALL> <CMD>", the form a JTTY station reads.
            if (jttyMode())
                built = QString("%1 %2 %3").arg(to.toUpper())
                            .arg(m_config.my_callsign()).arg(cmd);
#endif
            if (!extra.isEmpty()) built += " " + extra;
            built = built.trimmed();

            int pri = (priStr == "LOW") ? PriorityLow
                    : (priStr == "NORMAL") ? PriorityNormal
                    : PriorityHigh;
#ifdef JS8_ENABLE_JTTY
            // Same one-JTTY-message rule as TX.SEND_MESSAGE.
            if (jttyMsgTooLong(built))
                err = QStringLiteral("too_long");
            else
#endif
            {
                enqueueMessage(pri, built, -1, nullptr);
                processTxQueue();
            }
        }

        sendNetworkMessage("TX.SEND_DIRECTED", built,
                           {
                               {"_ID", id},
                               {"OK", err.isEmpty()},
                               {"ERROR", err},
                               {"COMPOSED", built},
                           });
        return;
    }
    /** @brief TX.SEND_CHUNKED: Send a reliable chunked message to a peer
     * via the Phase 1 ARQ protocol (stop-and-wait, per-chunk CRC, retries).
     *
     * Params:
     *   PEER  — destination callsign (required, must be a real station call,
     *           not @GROUP — Phase 1 is unicast only)
     *   value — the message body (required, any length; will be split into
     *           ~60-char chunks on whitespace boundaries)
     *
     * Response:
     *   TX.SEND_CHUNKED with OK / ERROR / PEER / MSG_ID / TOTAL params.
     *   Asynchronous progress: RX.CHUNKED_PROGRESS pushes per ACK.
     *   Final: RX.CHUNKED_DELIVERED or RX.CHUNKED_FAILED.
     */
    if (type == "TX.SEND_CHUNKED") {
        auto peer = message.params().value("PEER").toString().trimmed().toUpper();
        auto text = message.value();

        // Same callsign gate as TX.SEND_DIRECTED, minus the @GROUP branch —
        // Phase 1 ARQ is point-to-point.
        static const QRegularExpression peerRe(R"(^[A-Z0-9/]{3,15}$)");
        bool okPeer = peerRe.match(peer).hasMatch();

        QString err;
        int msgId = 0;
        int total = 0;
        if (!okPeer || peer.isEmpty()) {
            err = QString("invalid PEER '%1'").arg(peer);
        } else if (text.isEmpty()) {
            err = "empty message body";
        } else if (!m_chunkedArq) {
            err = "chunked-ARQ manager not initialised";
        } else {
            auto const result =
                m_chunkedArq->sendChunked(peer, text, m_nSubMode);
            if (!result.ok) {
                err = result.error;
            } else {
                msgId = result.msgId;
                total = result.totalChunks;
            }
        }

        sendNetworkMessage("TX.SEND_CHUNKED", text,
                           {
                               {"_ID", id},
                               {"OK", err.isEmpty()},
                               {"ERROR", err},
                               {"PEER", peer},
                               {"MSG_ID", msgId},
                               {"TOTAL", total},
                           });
        return;
    }
    /** @} */ // End TX Commands

    /**
     * @name QSO Commands
     * Convenience queries that bundle frequently-needed state for
     * automation clients — reduces round-trips and keeps the agent's
     * world-view coherent.
     */
    /** @{ */
    /** @brief QSO.GET_CONTEXT: One-shot structured snapshot of current QSO
     * state. Replaces a handful of separate polls (STATION.*, MODE.*,
     * RIG.*, TX.*) with a single response. Useful for LLM agents that
     * need a cheap "what's the current picture?" call. */
    if (type == "QSO.GET_CONTEXT") {
        int depth = m_txMessageQueue.size();
        // [TODO #112] See TX.GET_QUEUE_DEPTH — busy reads as depth 1 so
        // existing clients behave correctly with no changes.
        if (arqBusyNow() && depth == 0) depth = 1;
        if (m_transmitting && depth == 0) depth = 1;

        auto now = DriftingDateTime::currentDateTimeUtc();
        int recentDecodes = 0;
        for (auto const &list : m_bandActivity) {
            for (auto const &item : list) {
                if (item.utcTimestamp.secsTo(now) < 120) {
                    recentDecodes++;
                    break;
                }
            }
        }

        sendNetworkMessage("QSO.CONTEXT", "",
            {
                {"_ID", id},
                {"CALLSIGN", m_config.my_callsign()},
                {"GRID", m_config.my_grid()},
                {"SUBMODE", m_nSubMode},
                {"SUBMODE_NAME", JS8::Submode::name(m_nSubMode)},
                {"DIAL", QVariant((quint64)dialFrequency())},
                {"OFFSET", QVariant((quint64)freq())},
                {"PTT", QVariant(m_transmitting)},
                {"TX_QUEUE_DEPTH", depth},
                // [TODO #112] Explicit busy state for clients that want
                // precision rather than inferring it from depth.
                {"BUSY", QVariant(arqBusyNow() || m_transmitting)},
                {"BUSY_REASON", busyReason()},
                {"ARQ_TX_ACTIVE",
                 QVariant(m_chunkedArq &&
                          m_chunkedArq->hasActiveTxSession())},
                {"ARQ_RX_ACTIVE",
                 QVariant(m_chunkedArq &&
                          m_chunkedArq->hasActiveRxTransfer())},
                {"CALL_SELECTED", callsignSelected()},
                {"ACTIVE_OFFSETS_2MIN", recentDecodes},
                {"UTC", QVariant(now.toMSecsSinceEpoch())},
            });
        return;
    }
    /** @} */ // End QSO Commands

    // MODE.GET_SPEED
    // MODE.SET_SPEED
    /**
     * @name MODE Commands
     * MODE related API calls
     */
    /** @{ */
    /** @brief MODE.GET_SPEED: Retrieves the current transmission speed mode. */
    if (type == "MODE.GET_SPEED") {
        sendNetworkMessage("MODE.SPEED", "",
                           {
                               {"_ID", id},
                               {"SPEED", m_nSubMode},
                           });
        return;
    }
    /** @brief MODE.SET_SPEED: Updates the transmission speed mode.
     *
     * Subspace edition does not support Ultra (JS8I) — no panel button,
     * decoder gated off (JS8_ENABLE_JS8I=0 in commons.h). SPEED=8 (the
     * historical Ultra value) is therefore re-mapped to Subspace (FT2)
     * so any TCP-API client that still sends 8 lands in a sane mode
     * instead of silently switching to a hidden TX-only mode that we
     * can't decode either side of.
     */
    if (type == "MODE.SET_SPEED") {
        auto ok = false;
        auto const speed =
            message.params().value("SPEED", QVariant(m_nSubMode)).toInt(&ok);
        // [TODO #112 2026-07-23] Honour the SAME speed lock the UI uses.
        // Until now this path forced the change regardless: the lock only
        // calls setEnabled(false) on the mode actions, and setChecked()
        // on a DISABLED action still works programmatically — so an API
        // client could switch speed in the middle of a V3 native session
        // even while every button was greyed out, killing the transfer
        // (V3 binary frames exist only in the Subspace transport). Seen
        // with msg-rotator.sh, which sets speed 2-3x per cycle.
        if (ok && !canChangeSpeedNow()) {
            qWarning() << "[API] MODE.SET_SPEED refused: speed is locked"
                       << "(transmitting/tuning/frames queued/native TX or"
                          " RX session in progress)";
            sendNetworkMessage("MODE.SET_SPEED", "",
                               {
                                   {"_ID", id},
                                   {"SPEED", m_nSubMode},
                                   {"REFUSED", true},
                                   {"REASON", busyReason().isEmpty()
                                                  ? QStringLiteral("tx_busy")
                                                  : busyReason()},
                               });
            return;
        }
        if (ok) {
            if (speed == Varicode::JS8CallNormal)
                ui->actionModeJS8Normal->setChecked(true);
            else if (speed == Varicode::JS8CallFast)
                ui->actionModeJS8Fast->setChecked(true);
            else if (speed == Varicode::JS8CallTurbo)
                ui->actionModeJS8Turbo->setChecked(true);
            else if (speed == Varicode::JS8CallSlow)
                ui->actionModeJS8Slow->setChecked(true);
#ifdef JS8_ENABLE_JTTY
            else if (speed == Varicode::JS8CallJTTY)
                ui->actionModeJS8JTTY->setChecked(true); // the SPEED, JS8 mode
#endif
            else if (speed == Varicode::JS8CallUltra
                  || speed == Varicode::JS8CallFT2) {
                ui->actionModeFT2->setChecked(true);
#ifdef JS8_ENABLE_FT2
                // [ssdetect ruling 2026-09-08] API demand for
                // Subspace (exactly what SuperSpotter's Expect
                // engine sends) overrides a disabled decoder,
                // visibly: decode re-enables and the menu switch
                // re-checks; the operator may turn it off again.
                if (!m_l2Enabled)
                    setSubspaceDecodeEnabled(
                        true, QStringLiteral("API MODE.SET_SPEED 16"));
#endif
            }
            setupJS8();
            speedChosenForTransmit(m_nSubMode); // [2026-10-15] every speed's rule
        }
        sendNetworkMessage("MODE.SET_SPEED", "",
                           {
                               {"_ID", id},
                               {"SPEED", m_nSubMode},
                           });
        return;
    }
    /** @brief MODE.GET_SUBMODE_NAME: Returns human-readable name for the
     * current submode (e.g. "Normal", "Fast", "Turbo", "Slow", "Ultra",
     * "Subspace"). Avoids clients needing to maintain the speed-int mapping. */
    if (type == "MODE.GET_SUBMODE_NAME") {
        sendNetworkMessage("MODE.SUBMODE_NAME", "",
                           {
                               {"_ID", id},
                               {"SPEED", m_nSubMode},
                               {"NAME", JS8::Submode::name(m_nSubMode)},
                           });
        return;
    }
    /** @} */ // End MODE Commands

    /**
     * @name EVENTS Commands
     * Lightweight connection management for persistent API clients.
     */
    /** @{ */
    /** @brief EVENTS.KEEPALIVE: Round-trip ping. A persistent client can send
     * this periodically to verify the TCP socket and event stream are alive
     * without touching any state. */
    if (type == "EVENTS.KEEPALIVE") {
        sendNetworkMessage("EVENTS.PONG", "",
                           {
                               {"_ID", id},
                               {"UTC", QVariant(
                                   DriftingDateTime::currentDateTimeUtc()
                                       .toMSecsSinceEpoch())},
                           });
        return;
    }
    /** @} */ // End EVENTS Commands

    // INBOX.GET_MESSAGES
    // INBOX.STORE_MESSAGE
    /**
     * @name INBOX Commands
     * INBOX related API calls
     */
    /** @{ */
    /** @brief INBOX.GET_MESSAGES: Retrieves messages for a specified callsign. */
    if (type == "INBOX.GET_MESSAGES") {
        QString selectedCall =
            message.params().value("CALLSIGN", "").toString();
        if (selectedCall.isEmpty()) {
            selectedCall = "%";
        }

        Inbox inbox(inboxPath());
        if (!inbox.open()) {
            return;
        }

        QList<QPair<int, Message>> msgs;
        msgs.append(
            inbox.values("STORE", "$.params.TO", selectedCall, 0, 1000));
        msgs.append(
            inbox.values("READ", "$.params.FROM", selectedCall, 0, 1000));
        foreach (auto pair, inbox.values("UNREAD", "$.params.FROM",
                                         selectedCall, 0, 1000)) {
            msgs.append(pair);
        }
        std::stable_sort(
            msgs.begin(), msgs.end(),
            [](QPair<int, Message> const &a, QPair<int, Message> const &b) {
                return QVariant::compare(a.second.params().value("UTC"),
                                         b.second.params().value("UTC")) ==
                       QPartialOrdering::Greater;
            });

        QVariantList l;
        foreach (auto pair, msgs) {
            l << pair.second.toVariantMap();
        }

        sendNetworkMessage("INBOX.MESSAGES", "",
                           {
                               {"_ID", id},
                               {"MESSAGES", l},
                           });
        return;
    }
    /** @brief INBOX.STORE_MESSAGE: Stores a message in the inbox for a
     * callsign. */
    if (type == "INBOX.STORE_MESSAGE") {
        QString selectedCall =
            message.params().value("CALLSIGN", "").toString();
        if (selectedCall.isEmpty()) {
            return;
        }

        QString text = message.params().value("TEXT", "").toString();
        if (text.isEmpty()) {
            return;
        }

        CommandDetail d = {};
        d.cmd = " MSG ";
        d.to = selectedCall;
        d.from = m_config.my_callsign();
        d.relayPath = d.from;
        d.text = text;
        d.utcTimestamp = DriftingDateTime::currentDateTimeUtc();
        d.submode = m_nSubMode;

        auto mid = addCommandToStorage("STORE", d);

        sendNetworkMessage("INBOX.MESSAGE", "",
                           {
                               {"_ID", id},
                               {"ID", mid},
                           });
        return;
    }
    /** @} */ // End INBOX Commands

    // WINDOW.RAISE
    /**
     * @name WINDOW Commands
     * WINDOW related API calls
     */
    /** @{ */
    /** @brief WINDOW.RAISE: Brings the main application window to the
     * foreground.
     * NOTE: Some OSes block this from happening
     */
    /** @brief UI.GET_SNAPSHOT: [phone remote, operator 2026-10-21] what
     * the screen shows now -- LINES (the conversation window's last
     * lines, TEXT + TX), ROWS (the band activity rows as displayed:
     * CALL, OFFSET, AGE, SNR, SPEED, TEXT, TOME, SELECTED) and STATE
     * (MODE, SPEED, DIAL, OFFSET, SELECTED, TX_ENABLED, TRANSMITTING,
     * DIRECTED_LATCH, BAND_BY_CALL, OUTGOING). Afterwards each changed
     * part is pushed as RX.TEXT.TAIL / RX.BAND.TABLE / UI.STATE.
     */
    /** @brief UI.ACTION: [phone remote, operator 2026-10-21] press a desktop
     * button: value send, halt, txenable, cq, snr, reply, info, status,
     * directed, deselect. The button's own click -- the desktop behaviour
     * and every check behind it (TX enable, the watchdog, the send path);
     * a button the desktop shows disabled is refused (REFUSED, REASON
     * disabled). "directed" in JTTY mode only (in JS8 mode it opens a menu
     * on the station's screen).
     */
    if (type == "UI.ACTION") {
        QString const action = message.value().trimmed().toLower();
        // [operator 2026-10-21] two that are not desktop buttons:
        // "repeat" = the box's "Restore previous message"; "agn" = the
        // Directed-to menu's "<CALL> AGN?" (JS8), the JTTY macro in JTTY
        // mode. Enabled exactly when remoteUiState says so.
        if (action == QLatin1String("agn") || action == QLatin1String("repeat") ||
            action == QLatin1String("snrq")) {
            bool const ok = remoteUiState().value("BUTTONS").toMap().value(action).toBool();
            qWarning() << "[REMOTE] action" << action << (ok ? "pressed" : "disabled");
            if (ok && action == QLatin1String("repeat")) {
                restoreMessage();
            } else if (ok && action == QLatin1String("snrq")) {
                // [operator 2026-10-21] the Directed-to menu's "<CALL> SNR?"
                addMessageText(QString("%1 SNR?").arg(callsignSelected()), true);
                autoSendIfDirectedCmd();
            } else if (ok) {
#ifdef JS8_ENABLE_JTTY
                if (!jttyPlaceMacro(QStringLiteral("AGN?")))
#endif
                {
                    addMessageText(QString("%1 AGN?").arg(callsignSelected()), true);
                    autoSendIfDirectedCmd();
                }
            }
            sendNetworkMessage("UI.ACTION", action,
                               {{"_ID", message.params().value("_ID")},
                                {"OK", ok},
                                {"REASON", ok ? QString() : QStringLiteral("disabled")}});
            remotePushChanged();
            return;
        }
        QAbstractButton *b = remoteButton(action);
        QString reason;
        if (!b)
            reason = QStringLiteral("unknown");
        else if (!b->isVisible() || !b->isEnabled())
            reason = QStringLiteral("disabled");
        else if (action == QLatin1String("directed") && !jttyMode())
            reason = QStringLiteral("js8_menu");
        QString logged;
        if (reason.isEmpty() && action == QLatin1String("log") &&
            callsignSelected().isEmpty())
            reason = QStringLiteral("no_call");
        // [operator 2026-10-21, log 00:45:03-05] CQ had already queued its
        // message (Send down, waiting for the slot); the phone's Send then
        // CLICKED Send again, which on the desktop means cancel, and the CQ
        // never went out. From the phone, Send only ever sends: a message
        // already queued stays queued; HALT is the way to cancel.
        if (reason.isEmpty() && action == QLatin1String("send") &&
            ui->startTxButton->isChecked())
            reason = QStringLiteral("queued");
        // [operator 2026-10-21] JTTY: a box holding only the address
        // "<TO> <MYCALL>" has nothing to say -- not sent
        if (reason.isEmpty() && action == QLatin1String("send") && jttyMode()) {
            QStringList const w = ui->extFreeTextMsgEdit->toPlainText().simplified()
                                      .split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (w.size() == 2 &&
                w.at(1).compare(m_config.my_callsign().trimmed(), Qt::CaseInsensitive) == 0 &&
                Varicode::isValidCallsign(w.at(0), nullptr))
                reason = QStringLiteral("no_payload");
        }
        // [operator 2026-10-21] the phone's state says which are off with a
        // group selected (CQ, Reply, SNR); refused here too
        if (reason.isEmpty() &&
            !remoteUiState().value("BUTTONS").toMap().value(action, true).toBool())
            reason = QStringLiteral("disabled");
        qWarning() << "[REMOTE] action" << action
                   << (reason.isEmpty() ? QStringLiteral("pressed") : reason);
        if (reason.isEmpty() && action == QLatin1String("log")) {
            // [operator 2026-10-21: "one tap, no form"] the desktop's Log
            // QSO, filled exactly as the button fills it, and its OK
            // pressed: the same log files, ADIF and forwarding. Nobody is
            // at the station to answer an open dialog.
            logged = callsignSelected();
            on_logQSOButton_clicked();
            if (m_logDlg->isVisible()) m_logDlg->accept();
        } else if (reason.isEmpty()) {
            // [txquery, operator 2026-10-28] the toggled slot reads this
            // to mark "TX disabled by remote command" (status bar, API)
            m_txEnableByRemote = (action == QLatin1String("txenable"));
            if (m_txEnableByRemote) m_txHoldReason = QStringLiteral("remote command");
            b->click();
            m_txEnableByRemote = false;
            // [holdtimeout] the toggle's off is a hold too, same lease
            if (m_txDisabledRemote && !b->isChecked()) armRemoteHold(true);
        }
        sendNetworkMessage("UI.ACTION", action,
                           {{"_ID", message.params().value("_ID")},
                            {"OK", reason.isEmpty()},
                            {"REASON", reason},
                            {"LOGGED", logged}});
        remotePushChanged();
        return;
    }

    /** @brief UI.BAND_CLICK: [phone remote] a click (DOUBLE true: a
     * double-click) on the band-activity row of CALL (at OFFSET), through
     * the table's own click handlers. Rows without a callsign are refused:
     * their station pick is a menu on the station's screen.
     */
    if (type == "UI.BAND_CLICK") {
        QString const call = message.params().value("CALL").toString().trimmed();
        int const offset = message.params().value("OFFSET").toInt();
        bool const dbl = message.params().value("DOUBLE").toBool();
        auto *t = ui->tableWidgetRXAll;
        int row = -1;
        for (int r = 0; r < t->rowCount() && !call.isEmpty(); ++r) {
            auto *ci = t->item(r, BACallsign);
            auto *oi = t->item(r, BAOffset);
            if (!ci || ci->data(Qt::UserRole).toString() != call) continue;
            if (row < 0) row = r; // the call's row, even if it moved
            if (oi && oi->data(Qt::UserRole).toInt() == offset) {
                row = r;
                break;
            }
        }
        QString reason = call.isEmpty() ? QStringLiteral("no_call")
                         : row < 0      ? QStringLiteral("gone")
                                        : QString();
        qWarning() << "[REMOTE] band" << (dbl ? "double-click" : "click") << call
                   << offset << (reason.isEmpty() ? QStringLiteral("row %1").arg(row) : reason);
        if (reason.isEmpty()) {
            if (dbl)
                bandActivityDoubleClick(row, BAMessage);
            else
                on_tableWidgetRXAll_cellClicked(row, BAMessage);
        }
        // [operator 2026-10-21] the double-click selects but does not QSY
        // into the heartbeat zone (qsyOffsetAllowed); say so on the phone
        bool const noQsy = reason.isEmpty() && dbl && !qsyOffsetAllowed(offset);
        sendNetworkMessage("UI.BAND_CLICK", call,
                           {{"_ID", message.params().value("_ID")},
                            {"OK", reason.isEmpty()},
                            {"REASON", reason},
                            {"NO_QSY", noQsy ? QStringLiteral("hb_zone") : QString()}});
        remotePushChanged();
        return;
    }

    /** @brief UI.GET_FREQ_LIST: [phone remote, operator 2026-10-21] the
     * station's own frequency list, as the Frequency menu shows it
     * (sorted by frequency): ITEMS of FREQ (Hz), MHZ, BAND, MODE (the
     * entry's kind: JS8, JTTY, JS8+JTTY, ...), DESC, CURRENT.
     */
    if (type == "UI.GET_FREQ_LIST") {
        auto list = m_config.frequencies()->frequency_list();
        std::sort(list.begin(), list.end(), [](auto const &a, auto const &b) {
            return a.frequency_ < b.frequency_;
        });
        Radio::Frequency const dial = operatingDial();
        QVariantList items;
        for (auto const &f : list)
            items.append(QVariantMap{
                {"FREQ", static_cast<quint64>(f.frequency_)},
                {"MHZ", Radio::pretty_frequency_MHz_string(f.frequency_)},
                {"BAND", m_config.bands()->find(f.frequency_)},
                {"MODE", QString::fromLatin1(Modes::name(f.mode_))},
                {"DESC", f.description_},
                {"CURRENT", f.frequency_ == dial},
#ifdef JS8_ENABLE_JTTY
                // a JS8+JTTY entry: the mode is the operator's choice
                {"SHARED", f.mode_ == Modes::JS8_JTTY && !m_superSpotterSeen},
#endif
            });
        sendNetworkMessage("UI.FREQ_LIST", QString(),
                           {{"_ID", message.params().value("_ID")}, {"ITEMS", items}});
        return;
    }

    /** @brief UI.SET_FREQ_ENTRY: [phone remote] go to the list entry at FREQ
     * (Hz) -- the Frequency menu's own action (setRig), so the entry's
     * kind (JTTY / JS8) takes effect exactly as on the desktop. Only a
     * frequency in the list; refused while a message is going out.
     */
    if (type == "UI.SET_FREQ_ENTRY") {
        auto const want = message.params().value("FREQ").toULongLong();
        QString reason;
        bool listed = false;
        for (auto const &f : m_config.frequencies()->frequency_list())
            if (static_cast<quint64>(f.frequency_) == want) listed = true;
        if (!listed)
            reason = QStringLiteral("not_listed");
        else if (isMessageQueuedForTransmit())
            reason = QStringLiteral("transmitting");
        QString const mode = message.params().value("MODE").toString().toUpper();
        qWarning() << "[REMOTE] frequency entry" << want << mode
                   << (reason.isEmpty() ? QStringLiteral("set") : reason);
        // [operator 2026-10-21] the current entry too: setRig re-sends it
        if (reason.isEmpty()) setRig(static_cast<Radio::Frequency>(want));
#ifdef JS8_ENABLE_JTTY
        // [operator 2026-10-21] a JS8+JTTY entry with the mode chosen on the
        // phone: the Mode menu's own entries once the dial has ARRIVED
        // (arriving can itself switch mode, e.g. leaving a JTTY-only
        // frequency restores the earlier speed). JTTY = the "JTTY" entry;
        // JS8 = keep the JS8 speed, or Normal when coming from JTTY mode.
        // Gives up after 10 s (CHOSEN) if the rig never gets there.
        if (reason.isEmpty() && (mode == QLatin1String("JTTY") || mode == QLatin1String("JS8"))) {
            auto const tries = std::make_shared<int>(0);
            auto const apply = std::make_shared<std::function<void()>>();
            *apply = [this, want, mode, tries, apply]() {
                if (static_cast<quint64>(operatingDial()) != want) {
                    if (++*tries < 40) QTimer::singleShot(250, this, *apply);
                    else qWarning() << "[REMOTE] mode choice dropped: dial never reached" << want;
                    return;
                }
                if (!onSharedJttyFrequency()) return; // not a JS8+JTTY dial after all
                bool const wantJtty = mode == QLatin1String("JTTY");
                if (wantJtty && !ui->actionModeJTTY->isChecked() && ui->actionModeJTTY->isEnabled())
                    ui->actionModeJTTY->trigger();
                else if (!wantJtty && ui->actionModeJTTY->isChecked() &&
                         ui->actionModeJS8Normal->isEnabled())
                    ui->actionModeJS8Normal->trigger();
                qWarning() << "[REMOTE] mode on" << want << "->" << (jttyMode() ? "JTTY" : "JS8");
                remotePushChanged();
            };
            QTimer::singleShot(250, this, *apply);
        }
#endif
        sendNetworkMessage("UI.SET_FREQ_ENTRY", QString(),
                           {{"_ID", message.params().value("_ID")},
                            {"OK", reason.isEmpty()},
                            {"REASON", reason}});
        remotePushChanged();
        return;
    }

    /** @brief UI.OFFSET_STEP: [phone remote, operator 2026-10-21] move the
     * offset one step up (STEP > 0) or down (STEP < 0) -- 100 Hz in JS8
     * mode, 25 Hz in JTTY mode, rounded onto that grid -- through the
     * desktop's own changeFreq, refused there while a message is going
     * out unless "QSY while transmitting" is allowed.
     */
    if (type == "UI.OFFSET_STEP") {
        // [operator 2026-10-21] a step is one grid in the direction of STEP's
        // sign, landing ON the grid: 100 Hz in JS8 mode, 25 Hz in JTTY mode
        // (1237 Hz, JS8, + -> 1300; - -> 1200)
        int const dir = message.params().value("STEP").toInt() < 0 ? -1 : 1;
        int const grid = jttyMode() ? 25 : 100;
        int const before = freq();
        int const below = before / grid * grid; // grid at or below
        int const target = dir > 0 ? below + grid
                                   : (below == before ? below - grid : below);
        int const step = target - before;
        // [operator 2026-10-21] the desktop's QSY rule (qsyOffsetAllowed):
        // in JS8 not into the heartbeat zone at or below 1000 Hz
        bool const hbZone = !qsyOffsetAllowed(target);
        if (!hbZone) setFreqOffsetForRestore(target, false);
        qWarning() << "[REMOTE] offset step" << step << before << "->" << freq()
                   << (hbZone ? "refused: HB zone" : "");
        sendNetworkMessage("UI.OFFSET_STEP", QString(),
                           {{"_ID", message.params().value("_ID")},
                            {"OK", freq() != before},
                            {"REASON", hbZone ? QStringLiteral("hb_zone") : QString()},
                            {"OFFSET", freq()}});
        remotePushChanged();
        return;
    }

    /** @brief UI.GET_SPEEDS / UI.SET_SPEED: [phone remote, operator
     * 2026-10-21] the Mode menu's speed entries (Slow, Normal, Fast,
     * Turbo, Subspace, JTTY speed) as the menu shows them -- its own
     * wording, only the visible ones -- and choosing one by triggering
     * that menu entry. ITEMS: ID, TEXT, CHECKED, ENABLED. Not in JTTY
     * mode (its speed is fixed); a disabled entry is refused.
     */
    if (type == "UI.GET_SPEEDS" || type == "UI.SET_SPEED") {
        QList<QPair<QString, QAction *>> const speeds = {
            {QStringLiteral("slow"), ui->actionModeJS8Slow},
            {QStringLiteral("normal"), ui->actionModeJS8Normal},
            {QStringLiteral("fast"), ui->actionModeJS8Fast},
            {QStringLiteral("turbo"), ui->actionModeJS8Turbo},
            {QStringLiteral("subspace"), ui->actionModeFT2},
            {QStringLiteral("jtty"), ui->actionModeJS8JTTY},
        };
        if (type == "UI.GET_SPEEDS") {
            QVariantList items;
            for (auto const &[id, a] : speeds)
                if (a && a->isVisible())
                    items.append(QVariantMap{
                        {"ID", id},
                        {"TEXT", a->text().remove(QLatin1Char('&'))},
                        {"CHECKED", a->isChecked()},
                        {"ENABLED", a->isEnabled() && !jttyMode()},
                    });
            sendNetworkMessage("UI.SPEEDS", QString(),
                               {{"_ID", message.params().value("_ID")}, {"ITEMS", items}});
            return;
        }
        QString const want = message.value().trimmed().toLower();
        QAction *chosen = nullptr;
        for (auto const &[id, a] : speeds)
            if (id == want) chosen = a;
        QString const reason = !chosen                                      ? QStringLiteral("unknown")
                               : jttyMode()                                 ? QStringLiteral("jtty_mode")
                               : !chosen->isVisible() || !chosen->isEnabled() ? QStringLiteral("disabled")
                                                                              : QString();
        qWarning() << "[REMOTE] speed" << want << (reason.isEmpty() ? QStringLiteral("set") : reason);
        if (reason.isEmpty() && !chosen->isChecked()) chosen->trigger();
        sendNetworkMessage("UI.SET_SPEED", want,
                           {{"_ID", message.params().value("_ID")},
                            {"OK", reason.isEmpty()},
                            {"REASON", reason}});
        remotePushChanged();
        return;
    }

    /** @brief UI.GET_GROUPS / UI.SELECT_GROUP: [phone remote, operator
     * 2026-10-21] the groups the call list shows (@ALLCALL unless avoided,
     * then My Groups, sorted) -- ITEMS: NAME, SELECTED -- and selecting one
     * as a click on it there does (selectCallsign). JS8 mode only.
     */
    if (type == "UI.GET_GROUPS" || type == "UI.SELECT_GROUP") {
        QStringList groups;
        if (!m_config.avoid_allcall()) groups << QStringLiteral("@ALLCALL");
        auto mine = m_config.my_groups().values();
        std::sort(mine.begin(), mine.end());
        groups << mine;
        if (type == "UI.GET_GROUPS") {
            QVariantList items;
            for (auto const &g : groups)
                items.append(QVariantMap{{"NAME", g}, {"SELECTED", g == callsignSelected()}});
            sendNetworkMessage("UI.GROUPS", QString(),
                               {{"_ID", message.params().value("_ID")}, {"ITEMS", items}});
            return;
        }
        QString const want = message.value().trimmed().toUpper();
        QString const reason = jttyMode()               ? QStringLiteral("jtty_mode")
                               : !groups.contains(want) ? QStringLiteral("unknown")
                                                        : QString();
        qWarning() << "[REMOTE] group" << want << (reason.isEmpty() ? QStringLiteral("selected") : reason);
        if (reason.isEmpty()) selectCallsign(want);
        sendNetworkMessage("UI.SELECT_GROUP", want,
                           {{"_ID", message.params().value("_ID")},
                            {"OK", reason.isEmpty()},
                            {"REASON", reason}});
        remotePushChanged();
        return;
    }

    /** @brief UI.CONVO_CLICK: [phone remote, operator 2026-10-21] a tap on a
     * conversation line (value = the line's text): select the FIRST call
     * sign written with a trailing colon ("WD4KAV: ..."); DOUBLE true also
     * moves to the line's offset "(NNNN)", under the QSY rule
     * (qsyOffsetAllowed: not into the heartbeat zone in JS8).
     */
    if (type == "UI.CONVO_CLICK") {
        QString const line = message.value();
        bool const dbl = message.params().value("DOUBLE").toBool();
        QString call;
        QString const mycall = m_config.my_callsign().trimmed();
        // [operator 2026-10-21] never my own call -- the IDENTICAL call only
        // ("WM8Q/P" is another station's form and may be chosen)
        auto const isMe = [&](QString const &c) {
            return c.compare(mycall, Qt::CaseInsensitive) == 0;
        };
        // the message itself, without the window's "J - 12:34:56 - (1500) - "
        static QRegularExpression const headRe(
            QStringLiteral(R"(^\S+\s+-\s+\d{1,2}:\d{2}:\d{2}\s+-\s+\(\d+\)\s+-\s+)"));
        QString body = line;
        if (auto const h = headRe.match(line); h.hasMatch()) body = line.mid(h.capturedEnd());
#ifdef JS8_ENABLE_JTTY
        if (jttyMode()) {
            // [operator 2026-10-21] JTTY: the desktop's own sender rule --
            // "<TO> <FROM>", "<TO> DE <FROM>", "CQ <FROM>", "FROM:" -- only a
            // sender FORM, never the bare second-word guess
            bool form = false;
            QString const s = jttyLineSender(body, &form);
            if (form && !s.isEmpty() && !isMe(s)) call = s.toUpper();
        } else
#endif
        {
            // JS8: the first "CALL:" (the JS8 "FROM:" form). [operator
            // 2026-10-21] On my own line ("WM8Q: WD4KAV ...") the station
            // after the colon, as the desktop's double-click does.
            QStringList const words = body.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            for (int i = 0; i < words.size(); ++i) {
                QString w = words.at(i);
                if (!w.endsWith(QLatin1Char(':')) || w.size() < 2) continue;
                w.chop(1);
                if (!Varicode::isValidCallsign(w, nullptr) && !w.startsWith(QLatin1Char('@')))
                    continue;
                if (isMe(w)) {
                    QString const to = i + 1 < words.size() ? words.at(i + 1) : QString();
                    if ((Varicode::isValidCallsign(to, nullptr) || to.startsWith(QLatin1Char('@'))) &&
                        !isMe(to))
                        call = to.toUpper();
                } else {
                    call = w.toUpper();
                }
                break;
            }
        }
        int offset = -1;
        static QRegularExpression const offRe(QStringLiteral(R"(\((\d{2,4})\))"));
        if (auto const m = offRe.match(line); m.hasMatch()) offset = m.captured(1).toInt();
        QString reason = call.isEmpty() ? QStringLiteral("no_call") : QString();
        QString noQsy;
        if (reason.isEmpty()) {
            selectCallsign(call);
            if (dbl && offset > 0) {
                if (qsyOffsetAllowed(offset)) setFreqOffsetForRestore(offset, false);
                else noQsy = QStringLiteral("hb_zone");
            }
        }
        qWarning() << "[REMOTE] convo" << (dbl ? "double-click" : "click") << call << offset
                   << (reason.isEmpty() ? noQsy : reason);
        sendNetworkMessage("UI.CONVO_CLICK", call,
                           {{"_ID", message.params().value("_ID")},
                            {"OK", reason.isEmpty()},
                            {"REASON", reason},
                            {"CALL", call},
                            {"OFFSET", offset},
                            {"NO_QSY", noQsy}});
        remotePushChanged();
        return;
    }

    if (type == "UI.GET_SNAPSHOT") {
        sendNetworkMessage("UI.SNAPSHOT", QString(),
                           {{"_ID", message.params().value("_ID")},
                            {"LINES", remoteConversationTail()},
                            {"ROWS", remoteBandTable()},
                            {"STATE", remoteUiState()}});
        return;
    }

    if (type == "WINDOW.RAISE") {
        setWindowState(Qt::WindowActive);
        activateWindow();
        raise();
        return;
    }

#ifdef JS8_ENABLE_FT2
    // [ssdetect] SuperSpotter client signature: its selection-probe
    // burst sends API types that DO NOT EXIST (js8spotter.py
    // 9557-9561 in 2.9 / 8397-8401 in 2.6); no other client emits
    // nonexistent types. First sighting permanently unlocks the
    // Subspace-decode menu switch. No coordination with the
    // maintainer needed (operator ruling 2026-09-08): if a future
    // SuperSpotter drops this burst, the 701-form text signature in
    // the TX handlers keys on their CORE wire format and still
    // latches every deployment shape, pure servers included.
    if (type == "RX.GET_SELECTED_CALL" || type == "RX.GET_SELECTED" ||
        type == "TX.GET_SELECTED_CALL" ||
        type == "STATION.GET_SELECTED_CALL") {
        noteSuperSpotterSeen();
        return;
    }
#endif

    qCDebug(mainwindow_js8) << "Unable to process networkMessage:" << type;
}
    /** @} */ // end of WINDOW Commands
/** @} */ // end of API
