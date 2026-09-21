
/** \file
 * @brief member function of the UI_Constructor class
 *  builds the callsign query menu
 */

#include "JS8_UI/mainwindow.h"

void UI_Constructor::buildQueryMenu(QMenu *menu, QString call) {
    bool isAllCall = isAllCallIncluded(call);
    // [BUILD 331-bellGroupGate] Group calls (e.g. @SUBSPACE) are
    // also broadcast-targeted; some items shouldn't fire to a group.
    bool isGroupCall = isGroupCallIncluded(call);

    // [#269] Kept before the blanking below, for the entries that name
    // their station.
    QString const target = call.trimmed();

    // [#269 2026-09-21, operator] Say WHICH station. The menu is about
    // one selected call sign, and "selected callsign" made the reader
    // look away to check which. Falls back to the old wording when
    // nothing is selected.
    QString const who = target.isEmpty()
                            ? QStringLiteral("selected callsign")
                            : target;
    QString const whoThe = target.isEmpty()
                               ? QStringLiteral("the selected callsign")
                               : target;

    // for now, we're going to omit displaying the call...delete this if we want
    // the other functionality
    call = "";

    auto grid = m_config.my_grid();

    bool emptyInfo = m_config.my_info().isEmpty();
    bool emptyGrid = m_config.my_grid().isEmpty();

    auto callAction = menu->addAction(
        QString("Send a directed message to %1").arg(who));
    connect(callAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 ").arg(selectedCall), true);
    });

    // [#269 2026-09-21, operator] The same reach question here, under
    // the directed-message entry. STATION ONLY -- a group is not a
    // reach target, and `call` is blanked just below for display, so
    // the name is captured now.
    if (!isAllCall && !isGroupCall && !target.isEmpty()) {
        auto *reachAction =
            menu->addAction(QString("Can I reach %1 now?").arg(target));
        connect(reachAction, &QAction::triggered, this, [this, target]() {
            // [operator 2026-09-21] Clear the outgoing box, as every
            // other entry in this menu does; same queued-transmission
            // guard addMessageText applies.
            if (!isMessageQueuedForTransmit())
                ui->extFreeTextMsgEdit->clear();
            if (m_spotMapWindow)
                m_spotMapWindow->autoRouteFor(target);
        });
    }

    menu->addSeparator();

    auto sendReplyAction = menu->addAction(
        QString("%1 Reply - Send reply message to %2")
            .arg(call)
            .arg(who)
            .trimmed());
    connect(sendReplyAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        auto message = m_config.reply_message();
        message = replaceMacros(message, buildMacroValues(), true);
        addMessageText(QString("%1 %2").arg(selectedCall).arg(message), true);
        autoSendIfDirectedCmd();
    });

    auto sendSNRAction = menu->addAction(
        QString("%1 SNR - Send a signal report to %2")
            .arg(call)
            .arg(whoThe)
            .trimmed());
    sendSNRAction->setEnabled(m_callActivity.contains(callsignSelected()));
    connect(sendSNRAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        if (!m_callActivity.contains(selectedCall)) {
            return;
        }

        auto d = m_callActivity[selectedCall];
        addMessageText(QString("%1 SNR %2")
                           .arg(selectedCall)
                           .arg(Varicode::formatSNR(d.snr)),
                       true);

        autoSendIfDirectedCmd();
    });

    auto infoAction = menu->addAction(
        QString("%1 INFO - Send my station information").arg(call).trimmed());
    infoAction->setDisabled(emptyInfo);
    connect(infoAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(
            QString("%1 INFO %2").arg(selectedCall).arg(m_config.my_info()),
            true);

        // [BUILD 341 chart] Complete one-shot command — the free-text
        // info argument would trip the gate's remainder test, so the
        // menu sends unconditionally.
        toggleTx(true);
    });

    auto gridAction = menu->addAction(
        QString("%1 GRID %2 - Send my current station Maidenhead grid locator")
            .arg(call)
            .arg(grid)
            .trimmed());
    gridAction->setDisabled(emptyGrid);
    connect(gridAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(
            QString("%1 GRID %2").arg(selectedCall).arg(m_config.my_grid()),
            true);

        // [BUILD 341 chart] Complete one-shot — grid argument trips
        // the gate's remainder test; send unconditionally.
        toggleTx(true);
    });

    // [BUILD 331+ TODO #82] BELL — soft summons. Pings the recipient
    // with the configured "bell" notification sound (default
    // DingDing.wav). No protocol reply expected; this is a UX-level
    // attention-getter for slow-mode QSO. Placed immediately under
    // GRID (set-my-grid), just above the separator that ends the
    // parameter-bearing-commands group. Disabled for @ALLCALL AND
    // any group call — BELL is per-recipient, never broadcast.
    auto bellAction = menu->addAction(
        QString("%1 BELL - Trigger the BELL notification sound on the receiver's side")
            .arg(call)
            .trimmed());
    bellAction->setDisabled(isAllCall || isGroupCall);
    connect(bellAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }
        addMessageText(QString("%1 BELL").arg(selectedCall), true);
        // [BUILD 341 chart] BELL is a Subspace freetext convention,
        // not a Varicode command — the gate can never classify it.
        // Complete one-shot; send unconditionally.
        toggleTx(true);
    });

    menu->addSeparator();

    auto snrQueryAction = menu->addAction(
        QString("%1 SNR? - What is my signal report?").arg(call).trimmed());
    snrQueryAction->setDisabled(isAllCall);
    connect(snrQueryAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 SNR?").arg(selectedCall), true);

        autoSendIfDirectedCmd();
    });

    auto infoQueryAction =
        menu->addAction(QString("%1 INFO? - What is your station information?")
                            .arg(call)
                            .trimmed());
    infoQueryAction->setDisabled(isAllCall);
    connect(infoQueryAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 INFO?").arg(selectedCall), true);

        autoSendIfDirectedCmd();
    });

    auto gridQueryAction =
        menu->addAction(QString("%1 GRID? - What is your current grid locator?")
                            .arg(call)
                            .trimmed());
    gridQueryAction->setDisabled(isAllCall);
    connect(gridQueryAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 GRID?").arg(selectedCall), true);

        autoSendIfDirectedCmd();
    });

    auto stationIdleQueryAction = menu->addAction(
        QString("%1 STATUS? - What is your station status message?")
            .arg(call)
            .trimmed());
    stationIdleQueryAction->setDisabled(isAllCall);
    connect(stationIdleQueryAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 STATUS?").arg(selectedCall), true);

        autoSendIfDirectedCmd();
    });

    auto heardQueryAction = menu->addAction(
        QString("%1 HEARING? - What are the stations are you hearing? (Top 4 "
                "ranked by most recently heard)")
            .arg(call)
            .trimmed());
    heardQueryAction->setDisabled(isAllCall);
    connect(heardQueryAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 HEARING?").arg(selectedCall), true);

        autoSendIfDirectedCmd();
    });

#if 0
    auto retransmitAction = menu->addAction(QString("%1|[MESSAGE] - Please ACK and retransmit the following message").arg(call).trimmed());
    retransmitAction->setDisabled(isAllCall);
    connect(retransmitAction, &QAction::triggered, this, [this](){

        QString selectedCall = callsignSelected();
        if(selectedCall.isEmpty()){
            return;
        }

        addMessageText(QString("%1|[MESSAGE]").arg(selectedCall), true, true);
    });
#endif

    auto alertAction = menu->addAction(
        QString("%1>[MESSAGE] - Please relay this message to its destination")
            .arg(call)
            .trimmed());
    alertAction->setDisabled(isAllCall);
    connect(alertAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1>[MESSAGE]").arg(selectedCall), true, true);
    });

    auto msgAction = menu->addAction(
        QString("%1 MSG [MESSAGE] - Please store this message in your inbox")
            .arg(call)
            .trimmed());
    msgAction->setDisabled(isAllCall);
    connect(msgAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 MSG [MESSAGE]").arg(selectedCall), true,
                       true);
    });

    auto msgToAction = menu->addAction(
        QString("%1 MSG TO:[CALLSIGN] [MESSAGE] - Please store this message at "
                "your station for later retreival by [CALLSIGN]")
            .arg(call)
            .trimmed());
    msgToAction->setDisabled(isAllCall);
    connect(msgToAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(
            QString("%1 MSG TO:[CALLSIGN] [MESSAGE]").arg(selectedCall), true,
            true);
    });

    auto qsoQueryAction = menu->addAction(
        QString("%1 QUERY CALL [CALLSIGN]? - Please acknowledge you can "
                "communicate directly with [CALLSIGN]")
            .arg(call)
            .trimmed());
    connect(qsoQueryAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 QUERY CALL [CALLSIGN]?").arg(selectedCall),
                       true, true);
    });

    auto qsoQueryMsgsAction = menu->addAction(
        QString("%1 QUERY MSGS - Do you have any messages for me?")
            .arg(call)
            .trimmed());
    connect(qsoQueryMsgsAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 QUERY MSGS").arg(selectedCall), true, true);
        // [BUILD 341 chart] Bare command, complete as-is — auto-send
        // was simply never wired here.
        autoSendIfDirectedCmd();
    });

    auto qsoQueryMsgAction =
        menu->addAction(QString("%1 QUERY MSG [ID] - Please deliver the "
                                "complete message identified by ID")
                            .arg(call)
                            .trimmed());
    connect(qsoQueryMsgAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 QUERY MSG [ID]").arg(selectedCall), true,
                       true);
    });

    auto qsoQueryArqAction = menu->addAction(
        QString("%1 QUERY ARQ? - Do you support Auto Repeat Request, "
                "and at what protocol level?")
            .arg(call)
            .trimmed());
    connect(qsoQueryArqAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 QUERY ARQ?").arg(selectedCall), true, true);
        // [TODO.md #59 build 268] Auto-TX on selection — same pattern
        // SNR / INFO / STATUS macro buttons use. QUERY ARQ? is a
        // single-frame fire-and-forget capability probe; no draft
        // text to compose, no reason to make the operator click Send
        // separately. Gated on transmit_directed() so users who've
        // opted out of auto-TX-on-directed-action still see the
        // pre-typed text and can fire it manually.
        autoSendIfDirectedCmd();
    });

    menu->addSeparator();

    auto agnAction = menu->addAction(
        QString("%1 AGN? - Please repeat your last transmission")
            .arg(call)
            .trimmed());
    connect(agnAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 AGN?").arg(selectedCall), true);

        autoSendIfDirectedCmd();
    });

    auto qslQueryAction = menu->addAction(
        QString("%1 QSL? - Did you receive my last transmission?")
            .arg(call)
            .trimmed());
    connect(qslQueryAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 QSL?").arg(selectedCall), true);

        autoSendIfDirectedCmd();
    });

    auto qslAction = menu->addAction(
        QString("%1 QSL - I confirm I received your last transmission")
            .arg(call)
            .trimmed());
    connect(qslAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 QSL").arg(selectedCall), true);

        autoSendIfDirectedCmd();
    });

    auto yesAction = menu->addAction(
        QString("%1 YES - I confirm your last inquiry").arg(call).trimmed());
    connect(yesAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 YES").arg(selectedCall), true);

        autoSendIfDirectedCmd();
    });

    auto noAction =
        menu->addAction(QString("%1 NO - I do not confirm your last inquiry")
                            .arg(call)
                            .trimmed());
    connect(noAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 NO").arg(selectedCall), true);

        autoSendIfDirectedCmd();
    });

    auto hwAction = menu->addAction(
        QString("%1 HW CPY? - How do you copy?").arg(call).trimmed());
    connect(hwAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 HW CPY?").arg(selectedCall), true);

        autoSendIfDirectedCmd();
    });

    auto rrAction = menu->addAction(
        QString("%1 RR - Roger. Received. I copy.").arg(call).trimmed());
    connect(rrAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 RR").arg(selectedCall), true);

        autoSendIfDirectedCmd();
    });

    auto fbAction =
        menu->addAction(QString("%1 FB - Fine Business").arg(call).trimmed());
    connect(fbAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 FB").arg(selectedCall), true);

        autoSendIfDirectedCmd();
    });

    auto sevenThreeAction = menu->addAction(
        QString("%1 73 - I send my best regards").arg(call).trimmed());
    connect(sevenThreeAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 73").arg(selectedCall), true);

        autoSendIfDirectedCmd();
    });

    auto skAction =
        menu->addAction(QString("%1 SK - End of contact").arg(call).trimmed());
    connect(skAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 SK").arg(selectedCall), true);

        autoSendIfDirectedCmd();
    });

    auto ditDitAction = menu->addAction(
        QString("%1 DIT DIT - End of contact / Two bits").arg(call).trimmed());
    connect(ditDitAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        addMessageText(QString("%1 DIT DIT").arg(selectedCall), true);

        autoSendIfDirectedCmd();
    });
}
