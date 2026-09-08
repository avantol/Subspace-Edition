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
        auto text = message.value();
        if (!text.isEmpty()) {
            auto priStr = message.params().value("PRIORITY", "HIGH")
                              .toString().toUpper();
            int pri = (priStr == "LOW") ? PriorityLow
                    : (priStr == "NORMAL") ? PriorityNormal
                    : PriorityHigh;
            enqueueMessage(pri, text, -1, nullptr);
            processTxQueue();
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
            if (!extra.isEmpty()) built += " " + extra;
            built = built.trimmed();

            int pri = (priStr == "LOW") ? PriorityLow
                    : (priStr == "NORMAL") ? PriorityNormal
                    : PriorityHigh;
            enqueueMessage(pri, built, -1, nullptr);
            processTxQueue();
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
    // Subspace-decode menu switch. Disclosed to Magnet management
    // (do not change the burst without telling us; successor is an
    // explicit client-ID message).
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
