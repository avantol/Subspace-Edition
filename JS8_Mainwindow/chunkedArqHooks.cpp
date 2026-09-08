/**
 * @file chunkedArqHooks.cpp
 * @brief UI_Constructor slots that bridge ChunkedArq::Manager into
 *        the rest of JS8Call (TX enqueue path and conversation-panel
 *        display).
 *
 * The Manager itself lives in JS8_Main/ChunkedArq.{cpp,h} and owns
 * all per-peer state. The hooks here only translate between
 * Manager signals/slots and JS8Call's existing send-message and
 * display-text functions.
 *
 * Per the approved plan
 * (/home/john/.claude/plans/functional-swimming-avalanche.md):
 *   - wantToTransmit → enqueueMessage(PriorityHigh, ...) — same path
 *     the TCP API's TX.SEND_MESSAGE uses; inherits queue + autoreply
 *     + rig-control plumbing.
 *   - chunkAdded → displayTextForFreq with "<body> (CC/TT)" formatted
 *     line and NO diamond — progressive display per chunk.
 *   - messageDelivered → displayTextForFreq with assembled body + ♢
 *     end-of-message marker — single clean summary line on completion.
 *   - sendProgress / sendComplete / sendFailed are TCP API push
 *     events (TODO: emit via MessageServer once those API push
 *     formats are wired in networkMessage.cpp).
 */

#include "JS8_UI/mainwindow.h"
#include "JS8_UI/ICS213Dialog.h"

#include <QAbstractButton>
#include <QDateTime>
#include <QMessageBox>

#include "JS8_Main/ChunkedArq.h"
#include "JS8_Main/DriftingDateTime.h"
#include "JS8_Main/FileTransfer.h"
#include "JS8_Main/NativeBinary.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QPushButton>

namespace {

// JS8 end-of-message marker character (U+2666 BLACK DIAMOND SUIT).
// Reserved here for the final assembled-message summary; per-chunk
// progressive lines deliberately omit it so the operator sees the
// "more coming" signal.
QChar const kJs8DiamondMarker(0x2666);

}  // namespace

void UI_Constructor::onChunkedWantToTransmit(QString const &text) {
    // ChunkedArq has a chunk/ACK/NACK ready to TX. We do the minimum
    // directly here and bypass enqueueMessage / processTxQueue:
    //
    //   - For chunk 1 (user clicked Send) the start button is already
    //     CHECKED, so processTxQueue's toggleTx(true) was a no-op and
    //     never re-entered startTx — m_nextFreeTextMsg would have
    //     stayed empty without an explicit prepareNextMessageFrame.
    //
    //   - For chunks 2..N (driven by ACK → sendNextChunk), the start
    //     button is UNCHECKED (chunk 1's stopTx → resetMessage cleared
    //     it). Now toggleTx(true) inside processTxQueue calls
    //     setChecked(true), which fires on_startTxButton_toggled(true)
    //     → INNER startTx() → prepareNextMessageFrame succeeds and
    //     sets m_nextFreeTextMsg. Then control returns to us and our
    //     own explicit prepareNextMessageFrame runs against an empty
    //     m_txFrameQueue (already drained by the inner call), pops an
    //     empty frame, and CLEARS m_nextFreeTextMsg as the
    //     "no frame to send" exit. Chunk 2 dies on the spot.
    //
    // Direct path avoids both quirks: write the chunk into the widget,
    // run prepareNextMessageFrame ONCE, open the m_auto gate, and
    // ensure the start button reflects the in-flight state without
    // re-entering startTx (QSignalBlocker on setChecked).
    if (!ui->extFreeTextMsgEdit) {
        return;
    }
    addMessageText(text, /*clear=*/true);
    if (!ui->startTxButton->isChecked()) {
        QSignalBlocker const block(ui->startTxButton);
        ui->startTxButton->setChecked(true);
    }
    if (!prepareNextMessageFrame()) {
        qCWarning(chunkedarq_js8)
            << "[ARQ] onChunkedWantToTransmit: prepareNextMessageFrame "
               "returned false; chunk stranded:"
            << text.left(40);
        return;
    }
    // Open the guiUpdate gate (mainwindow.cpp:2987 — `m_transmitting
    // || m_auto || m_tune`). Without this prepareSending is never
    // called and PTT never fires.
    if (!m_auto) {
        auto_tx_mode(true);
    }
}

void UI_Constructor::onChunkedWantsResponseTx(QString const &text) {
    // [modegate 2026-09-04, special-case enumeration item 2] The
    // executor is the mode's ONLY speaker: an ARQ transfer STARTED
    // AT US mid-auto-route gets no ACK/NACK -- keying one lands in
    // our own listening windows. The sender's protocol retries and
    // fails over normally; RX processing is untouched.
    if (m_autoRouteActive) {
        qCWarning(chunkedarq_js8)
            << "[REPLY-GATE] ARQ response suppressed (auto-route"
            << "active):" << text.left(40);
        return;
    }
    // [TODO.md #57 build 268] Before sending an ACK / NACK, snapshot
    // any draft text the operator has typed into the outgoing-msg
    // widget. stopTx() restores it once the response TX completes.
    // If a prior restore is still pending (back-to-back ACK / NACK
    // events), do NOT overwrite the saved text — the existing snapshot
    // is the genuine operator draft we still owe a restore for. The
    // outgoing-box content at this moment is the previous response's
    // wire text, which we'd be re-saving and then over-restoring on
    // the next stopTx, producing a stale ACK / NACK line back in the
    // box.
    if (ui->extFreeTextMsgEdit && !m_arqResponseRestorePending) {
        // [boxrace 2026-09-04, WM8Q/P field jam] Only an IDLE box
        // holds an operator draft. If a transmission is armed or
        // running, the box holds WIRE TEXT (a queue-drained relay
        // forward in the field case) -- saving that and restoring
        // it later parks dead wire text in the box, and a non-empty
        // box blocks the whole transmit queue. Save NOTHING then:
        // the restore becomes a plain unlock.
        bool const boxHoldsWire =
            m_transmitting || m_txFrameCount > 0 ||
            !m_txFrameQueue.isEmpty() ||
            ui->startTxButton->isChecked();
        m_arqResponseSavedText =
            boxHoldsWire ? QString()
                         : ui->extFreeTextMsgEdit->toPlainText();
        if (boxHoldsWire)
            qCWarning(chunkedarq_js8)
                << "[ARQ-RX] outgoing-text save SKIPPED: box holds"
                << "armed wire text, not a draft";
        m_arqResponseRestorePending = true;
        // [TODO #146] Lock the box for the whole save/inject/restore
        // window: keystrokes during a response TX either ride into
        // the wire text being transmitted or are wiped by the
        // deferred restore. Read-only makes the frozen box explicit;
        // unlocked in the same deferred restore that returns the
        // draft (stopTx path). Back-to-back responses keep the lock
        // (this save is skipped while a restore is pending).
        ui->extFreeTextMsgEdit->setReadOnly(true);
        // [operator 2026-09-08] Status message removed -- "outgoing
        // text locked" wasn't strictly true; the read-only box is
        // its own indication.
        qCWarning(chunkedarq_js8)
            << "[ARQ-RX] outgoing-text save: chars="
            << m_arqResponseSavedText.size()
            << "for response=" << text.left(40);
    }
    // [BUILD 342.18 bannerNack] While the in-progress banner is
    // up, ANY response keyup (ACK, or the watchdog's NACK probes)
    // proves this station is still busy with the session —
    // refresh the banner so the operator isn't invited to type/send
    // while we're still keying replies (bench 2026-07-20: banner
    // dropped at +60 s, NACK probes ran to +2 min).
    // [BUILD 353 rxbanner] Banner now covers ALL multi-part ARQ
    // receives (text / V1 / V2 via onChunkedChunkAdded, V3 via the
    // native signals), so this refresh serves every path.
    // [BUILD 354 rxsession] Banner refresh on our reply keyups now
    // originates in the Manager (sendAck/tryNack → rxSessionActivity)
    // — the machine owns it; nothing to do here.
    // Delegate to the existing chunk-emit path. addMessageText(/*clear=*/true)
    // inside that slot replaces the widget contents with the response
    // wire text; the restore later in stopTx swaps the operator's
    // draft back in.
    onChunkedWantToTransmit(text);
}

void UI_Constructor::onChunkedChunkAdded(QString const &fromCall,
                                         QString const &chunkBody,
                                         int            chunkId,
                                         int            total) {
    // No UI write — per-chunk "(CC/TT)" progressive display was
    // backed out 2026-06-04 (the typeahead path paints body fragments
    // before we ever see the marker, so any "clean" per-chunk line
    // ends up alongside the raw typeahead text rather than replacing
    // it). Operator readability comes from the final " ♦ " summary
    // in messageDelivered. This signal still fires for TCP API push
    // (RX.CHUNKED_PROGRESS etc.); just no longer drives the panel.
    qCDebug(chunkedarq_js8)
        << "[ARQ-RX] chunkAdded from=" << fromCall
        << "chunk=" << chunkId << "/" << total
        << "bodyLen=" << chunkBody.size();

    // [BUILD 353 rxbanner, operator request 2026-08-02] Multi-part
    // TEXT / V1-file / V2 receives now show the same in-progress
    // banner V3 always had — banner ONLY, no button disabling for
    // these paths (unlike V3's RX lock). Cleared by: delivery
    // (onChunkedMessageDelivered), operator Halt/Esc
    // (restoreArqPlaceholder in the operator slot), or the cosmetic
    // speed-derived stall timeout in refreshArqPlaceholder — which
    // reverts the BANNER only and cancels NOTHING; a late chunk
    // still assembles and simply re-raises the banner here.
    // [BUILD 354 rxsession] Banner/progress now driven by the
    // Manager's rxSessionChanged — no direct UI writes here.
}

void UI_Constructor::onChunkedMessageDelivered(QString const &fromCall,
                                               QString const &toCall,
                                               QString const &assembledBody,
                                               int            msgId) {
    // Multi-chunk message fully assembled. The per-chunk (CC/TT)
    // lines already show the operator what arrived; we still write
    // ONE final " assembled-body ♦" line so the complete message
    // appears as a single coherent block (useful for QSO log scroll-
    // back). Diamond uses U+2666 (black diamond) to distinguish from
    // the standard JS8 ♢ end-of-frame marker.
    QString const line = QStringLiteral("%1: %2 %3")
                             .arg(fromCall, assembledBody)
                             .arg(kJs8DiamondMarker);
    auto const now = DriftingDateTime::currentDateTimeUtc();
    displayTextForFreq(line, freq(), now,
                       /*isTx=*/false,
                       /*isNewLine=*/true,
                       /*isLast=*/true,
                       m_nSubMode);
    // [stamon 2026-09-03] ARQ-delivered messages never pass the
    // processCommandActivity tee (field-caught: WD4KAV's ARQ MSG
    // missing from his monitor) -- feed the assembled message here,
    // the ARQ path's one-per-message point.
    if (!m_stationMonitors.isEmpty())
        feedStationMonitors(fromCall, toCall, QString(), line,
                            freq(), now, m_nSubMode);

    // TCP API push: full inbound reliable message delivered.
    sendNetworkMessage("RX.CHUNKED_DELIVERED", assembledBody,
                       {
                           {"FROM",   fromCall},
                           {"TO",     toCall},
                           {"MSG_ID", msgId},
                       });
    // [BUILD 331-arqRxClean] No auto-disable needed here — the
    // auto-enable on first chunk RX was removed (see
    // processCommandActivity.cpp). ARQ state purely reflects
    // operator/TX intent now; RX side never toggles it.
}

void UI_Constructor::onChunkedSendProgress(QString const &peer, int msgId,
                                           int delivered, int total) {
    // TCP API push: per-ACK outbound progress. Lets external clients
    // (Python prototype, JS8 Spotter, custom bots) render a chunked-
    // send progress bar without having to model the protocol
    // themselves.
    qCDebug(chunkedarq_js8)
        << "[ARQ-TX] sendProgress peer=" << peer << "msgId=" << msgId
        << "delivered=" << delivered << "of" << total;
    sendNetworkMessage("TX.CHUNKED_PROGRESS", "",
                       {
                           {"PEER",      peer},
                           {"MSG_ID",    msgId},
                           {"DELIVERED", delivered},
                           {"TOTAL",     total},
                       });
}

void UI_Constructor::onChunkedSendComplete(QString const &peer, int msgId,
                                           int total, int totalRetries) {
    // TCP API push: outbound chunked send finished cleanly.
    qCWarning(chunkedarq_js8)
        << "[ARQ-TX] sendComplete peer=" << peer << "msgId=" << msgId
        << "total=" << total << "totalRetries=" << totalRetries;

    // [TODO #143 fullrestore] Success-path counterpart of the five
    // failure-path restores (halted/busy/too_long/nack_exhausted/
    // timeout_exhausted all call setPlainText(originalBody)): clear
    // the outgoing box so the final sub-msg's as-aired WIRE text
    // (addressing + chunk marker) is not left behind as ordinary
    // editable text. Field incident 2026-08-06 (WD4KAV): the operator
    // edited the leftover — "sent" styling reads as gone, but
    // toPlainText() ships the whole document — and the stale
    // "#84.01/01.6D79" rode into the next body as payload, forcing a
    // wrong 2-sub-msg split and a receiver NACK loop. The message
    // itself stays recoverable via "Restore Previous Message", which
    // now holds the pristine full body.
    if (ui->extFreeTextMsgEdit) {
        ui->extFreeTextMsgEdit->blockSignals(true);
        ui->extFreeTextMsgEdit->clear();
        ui->extFreeTextMsgEdit->blockSignals(false);
    }

    // [stamon 2026-09-03, operator] COMPLETED outgoing ARQ text
    // messages feed the monitors as one assembled line (sub-msgs
    // already flow the normal directed path). File and web-link
    // transfers are skipped by ruling: the file-send msgId and the
    // F/V1 / L/V1 wire prefixes identify them.
    if (!m_stationMonitors.isEmpty() &&
        msgId != m_fileSendMsgId && !m_lastTxMessage.isEmpty() &&
        !m_lastTxMessage.startsWith(QStringLiteral("F/V1 ")) &&
        !m_lastTxMessage.startsWith(QStringLiteral("L/V1 "))) {
        QString const myC = m_config.my_callsign().trimmed();
        feedStationMonitors(
            myC, peer, QString(),
            QStringLiteral("%1: %2 %3 %4")
                .arg(myC, peer, m_lastTxMessage,
                     QString(kJs8DiamondMarker)),
            freq(), DriftingDateTime::currentDateTimeUtc(),
            m_nSubMode);
    }

    // [FILE-XFER build 282] If this msgId matches the file-send we
    // auto-enabled ARQ for, restore ARQ to its prior state.
    if (m_fileSendMsgId != 0 && msgId == m_fileSendMsgId) {
        m_fileSendMsgId = 0;
        if (ui->actionModeReplicatorProtocol &&
            ui->actionModeReplicatorProtocol->isChecked() !=
                m_arqStateBeforeFileSend) {
            ui->actionModeReplicatorProtocol->setChecked(
                m_arqStateBeforeFileSend);
            qCWarning(chunkedarq_js8)
                << "[FT-TX] file send delivered; ARQ restored to"
                << (m_arqStateBeforeFileSend ? "ON" : "OFF");
        }
    }
    // [K-FALLBACK 2026-07-21] Delivered — the retained retry envelope
    // is moot.
    if (m_v3SendMsgId != 0 && msgId == m_v3SendMsgId) {
        m_v3SendMsgId = 0;
        m_v3SendEnvelope.clear();
        m_v3SendPeer.clear();
        m_v3SendChunkBytes = 0;
    }
    // [TODO #107] V3 send over — release the binary queue-guard and
    // flush any still-queued native frames (a NACK-crossed retransmit
    // can be sitting in the queue when the final ACK lands; without
    // the flush TX keeps airing the already-delivered transfer).
    if (m_nativeBinaryTxActive) {
        m_nativeBinaryTxActive = false;
        if (!m_txFrameQueue.isEmpty()) {
            qCWarning(chunkedarq_js8)
                << "[V3-TX] flushing" << m_txFrameQueue.size()
                << "queued frames on sendComplete";
            resetMessageTransmitQueue();
        }
    }
    // [BUILD 303] ARQ is opt-in per super-message. Disable on EVERY
    // sendComplete regardless of how the message was initiated (menu
    // action, plain Send, TCP API, etc.). The prior build-298 gate on
    // m_arqTextSendMsgId only fired for menu-initiated sends, leaving
    // plain-Send ARQ sessions latched. m_arqTextSendMsgId is still
    // cleared if set so the sentinel doesn't dangle.
    m_arqTextSendMsgId = 0;
    if (ui->actionModeReplicatorProtocol &&
        ui->actionModeReplicatorProtocol->isChecked()) {
        ui->actionModeReplicatorProtocol->setChecked(false);
        qCWarning(chunkedarq_js8)
            << "[ARQ] super-message delivered (msgId="
            << msgId << "); ARQ auto-disabled";
    }
    sendNetworkMessage("TX.CHUNKED_COMPLETE", "",
                       {
                           {"PEER",          peer},
                           {"MSG_ID",        msgId},
                           {"TOTAL",         total},
                           {"TOTAL_RETRIES", totalRetries},
                       });

    // Modeless super-message-success dialog. Parented to the main
    // window so it inherits z-order; WA_DeleteOnClose plus show()
    // (NOT exec()) keeps it non-blocking — operator can keep typing /
    // hitting Send while it's up. All N delivered, so successful =
    // total.
    auto *box = new QMessageBox(QMessageBox::Information,
                                tr("ARQ super-message delivered"),
                                tr("To: %1\n"
                                   "Super-message #%2\n\n"
                                   "Sub-messages: %3 of %3 delivered\n"
                                   "Total retries: %4")
                                    .arg(peer)
                                    .arg(msgId)
                                    .arg(total)
                                    .arg(totalRetries),
                                QMessageBox::Ok, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    box->show();
}

void UI_Constructor::onChunkedSendFailed(QString const &peer, int msgId,
                                         int delivered, int total,
                                         int totalRetries,
                                         QString const &reason) {
    // TCP API push: outbound chunked send gave up (timeout, too_long,
    // busy, etc.). Reason is the same short token used in
    // ChunkedArq::Manager::sendFailed.
    qCWarning(chunkedarq_js8)
        << "[ARQ-TX] sendFailed peer=" << peer << "msgId=" << msgId
        << "delivered=" << delivered << "of" << total
        << "totalRetries=" << totalRetries << "reason=" << reason;

    // [FILE-XFER build 282] Mirror of the sendComplete restore. The
    // file send didn't make it, but ARQ still needs to go back to
    // its pre-file state so the operator's UI returns to normal.
    if (m_fileSendMsgId != 0 && msgId == m_fileSendMsgId) {
        m_fileSendMsgId = 0;
        if (ui->actionModeReplicatorProtocol &&
            ui->actionModeReplicatorProtocol->isChecked() !=
                m_arqStateBeforeFileSend) {
            ui->actionModeReplicatorProtocol->setChecked(
                m_arqStateBeforeFileSend);
            qCWarning(chunkedarq_js8)
                << "[FT-TX] file send failed; ARQ restored to"
                << (m_arqStateBeforeFileSend ? "ON" : "OFF");
        }
    }
    // [TODO #107] V3 send over (failed) — release the binary
    // queue-guard AND flush the queued native frames of the dead
    // transfer. Bench 2026-07-19: timeout_exhausted fired with 8
    // frames still queued — the fail dialog was dismissed and TX
    // kept airing them for another 30 s.
    if (m_nativeBinaryTxActive) {
        m_nativeBinaryTxActive = false;
        if (!m_txFrameQueue.isEmpty()) {
            qCWarning(chunkedarq_js8)
                << "[V3-TX] flushing" << m_txFrameQueue.size()
                << "queued frames on sendFailed";
            resetMessageTransmitQueue();
        }
    }
    // [BUILD 303] Mirror of sendComplete. Disable ARQ on EVERY failure
    // regardless of initiation path. ARQ is opt-in per-message.
    m_arqTextSendMsgId = 0;
    if (ui->actionModeReplicatorProtocol &&
        ui->actionModeReplicatorProtocol->isChecked()) {
        ui->actionModeReplicatorProtocol->setChecked(false);
        qCWarning(chunkedarq_js8)
            << "[ARQ] super-message failed (msgId=" << msgId
            << "reason=" << reason << "); ARQ auto-disabled";
    }
    sendNetworkMessage("TX.CHUNKED_FAILED", reason,
                       {
                           {"PEER",          peer},
                           {"MSG_ID",        msgId},
                           {"DELIVERED",     delivered},
                           {"TOTAL",         total},
                           {"TOTAL_RETRIES", totalRetries},
                           {"REASON",        reason},
                       });

    // [K-FALLBACK 2026-07-21] One-shot K=4 retry offer, operator-in-
    // loop (spec held 2026-07-21, then approved). Consume the retained
    // V3 send context either way; offer only when ALL of:
    //   - this failure IS that V3 send (msgId match),
    //   - reason is timeout_exhausted (the link-quality failure the
    //     smaller chunk addresses),
    //   - the failed run was K=8 (single step 8 → 4, never 4 → 2),
    //   - the envelope FITS at K=4: ≤ 99 × 32 = 3168 B. Too large →
    //     the option simply doesn't appear.
    QByteArray retryEnvelope;
    QString    retryPeer;
    bool       offerK4 = false;
    if (m_v3SendMsgId != 0 && msgId == m_v3SendMsgId) {
        retryEnvelope = m_v3SendEnvelope;
        retryPeer     = m_v3SendPeer;
        int const kb  = m_v3SendChunkBytes;
        m_v3SendMsgId = 0;
        m_v3SendEnvelope.clear();
        m_v3SendPeer.clear();
        m_v3SendChunkBytes = 0;
        offerK4 =
            reason == QLatin1String("timeout_exhausted") &&
            kb == NativeBinary::DEFAULT_CHUNK_BYTES &&
            NativeBinary::chunksNeeded(
                retryEnvelope.size(),
                NativeBinary::FALLBACK_CHUNK_BYTES) <=
                ChunkedArq::MAX_CHUNKS_ROLLOVER;
    }

    // Modeless super-message-failure dialog. Same pattern as the
    // success path; Warning icon distinguishes at a glance.
    QString body;
    if (total <= 0) {
        // Pre-flight rejection — no chunks were ever in flight.
        // [TODO #90 2026-07-14] Plain language for the one case an
        // operator can act on (message length); every other pre-
        // flight reason is already in the log (qCWarning above) and
        // gets NO dialog — jargon reasons confused more than helped.
        if (reason == QLatin1String("too_long")) {
            body = tr("Message is too long.");
        } else {
            return; // logged; no dialog
        }
    } else {
        body = tr("To: %1\n"
                  "Super-message #%2\n\n"
                  "Sub-messages: %3 of %4 delivered\n"
                  "Total retries: %5\n"
                  "Reason: %6")
                   .arg(peer)
                   .arg(msgId)
                   .arg(delivered)
                   .arg(total)
                   .arg(totalRetries)
                   .arg(reason);
    }
    auto *box = new QMessageBox(QMessageBox::Warning,
                                tr("ARQ super-message failed"),
                                body,
                                QMessageBox::Ok, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    if (offerK4) {
        auto *retryBtn = box->addButton(
            tr("Retry with smaller sub-messages"),
            QMessageBox::ActionRole);
        connect(box, &QMessageBox::buttonClicked, this,
                [this, retryBtn, retryEnvelope,
                 retryPeer](QAbstractButton *clicked) {
                    if (clicked != retryBtn) {
                        return;
                    }
                    // Native frames are Subspace-only (design decision
                    // 2026-07-20) — if the operator left Subspace while
                    // the dialog sat open, don't start a V3 send.
                    if (m_nSubMode != Varicode::JS8CallFT2) {
                        JS8MessageBox::warning_message(
                            this, tr("Cannot retry"),
                            tr("Switch your Speed back to Subspace, "
                               "then send the file again."));
                        return;
                    }
                    qCWarning(chunkedarq_js8)
                        << "[V3-TX] operator retry at K=4: peer="
                        << retryPeer
                        << "envelopeBytes=" << retryEnvelope.size();
                    dispatchArqBodyBinary(
                        retryEnvelope, retryPeer,
                        NativeBinary::FALLBACK_CHUNK_BYTES);
                });
    }
    box->show();
}

// [TODO #51 2026-06-10 build 235]
// Restore the original outbound body to the outgoing-text widget on
// non-success terminal paths (halted, nack_exhausted, timeout_exhausted,
// too_long, busy). Operator sees their text reappear so they can edit
// and retry. The status-bar tip names the failure reason briefly.
//
// Fires AFTER onChunkedSendFailed in normal Qt signal ordering (both
// signals come from the same Manager call). The failure dialog from
// onChunkedSendFailed and the restored text from this slot do not
// race — they happen in deterministic sequence on the GUI thread.
void UI_Constructor::onChunkedSendRestoreRequested(QString const &body,
                                                   QString const &reason) {
    if (!ui->extFreeTextMsgEdit) {
        return;
    }
    if (body.isEmpty()) {
        return;
    }
    // [FILE-XFER 2026-06-16] Suppress restore for file-transfer
    // super-messages. The body is base32 gibberish ("F/V1 GZIP/
    // BASE32 .ABCD...") with no operator value — pasting it back
    // into the outgoing box just creates a mess for them to clear
    // before composing their next message. Halt / timeout / fail /
    // complete all route through this slot; for files we drop all
    // four restore paths uniformly.
    // [BUILD 354 norestore 2026-08-02, operator request] File/link
    // WIRE bodies ("F/Vn ...", "L/V1 BASE32 ...") are machine-built —
    // not re-sendable text — so restoring them is misleading. And the
    // box may hold leftover wire text from the TX path, so merely
    // suppressing the restore (the old PREFIX_V1-only check did that,
    // missing V2/V3/links entirely) leaves gibberish behind: CLEAR
    // the box instead. Plain text falls through to the restore below,
    // unchanged — it can be re-sent as-is.
    static QRegularExpression const kWireBodyRe{
        QStringLiteral(R"(^[FL]/V\d+(\s|$))")};
    if (kWireBodyRe.match(body.trimmed()).hasMatch()) {
        qCWarning(chunkedarq_js8)
            << "[FT-TX] clearing outgoing text (file/link wire body,"
            << "not re-sendable) reason=" << reason;
        ui->extFreeTextMsgEdit->blockSignals(true);
        ui->extFreeTextMsgEdit->clear();
        ui->extFreeTextMsgEdit->blockSignals(false);
        return;
    }
    qCWarning(chunkedarq_js8)
        << "[ARQ-TX] restoring outgoing text after fail reason="
        << reason << "chars=" << body.size();
    // Replace whatever is currently in the box (probably a partially-
    // sent chunk's wrapped text) with the original super-msg text.
    ui->extFreeTextMsgEdit->blockSignals(true);
    ui->extFreeTextMsgEdit->setPlainText(body);
    ui->extFreeTextMsgEdit->blockSignals(false);
    // Brief status hint so the operator understands why the text
    // reappeared. Translatable; reason token is for the log only.
    QString const tip = tr("ARQ send failed — text restored for retry");
    statusBar()->showMessage(tip, 5000);
}

void UI_Constructor::onChunkedMsgDelivered(QString const &peer,
                                           QString const &addressee,
                                           QString const &body,
                                           int            msgId) {
    // [SENDER-SIDE INBOX FIX 2026-06-12 build 261]
    // Previously this slot called addCommandToMyInbox on the SENDER,
    // depositing a copy of every successfully-delivered ARQ MSG into
    // the operator's own inbox. That mirrored neither the on-air bare
    // MSG path (no sender-side auto-copy at all) nor the on-air
    // MSG TO: path (stores via addCommandToStorage("STORE") on the
    // RECEIVER's machine, not on the sender's). Per operator 2026-06-12:
    // "when i'm sending a message with the MSG command, a copy goes
    // into my inbox, not the desired action."
    //
    // Drop the local deposit. The operator already sees:
    //   • Standard outbound TX rendering in the conversation panel
    //   • onChunkedSendComplete's success dialog confirming delivery
    //   • Status-bar ARQ progress while in flight
    //
    // The receiver-side hook (onChunkedInboxMessageReceived) still
    // routes the body to the appropriate inbox slot on the peer's
    // machine.
    qCWarning(chunkedarq_js8)
        << "[ARQ-TX] msgDelivered: peer=" << peer
        << "addressee=" << addressee << "msgId=" << msgId
        << "bodyChars=" << body.size()
        << "(local-inbox deposit removed Build 261)";
}

void UI_Constructor::onChunkedInboxMessageReceived(QString const &fromCall,
                                                   QString const &addressee,
                                                   QString const &body,
                                                   int            msgId) {
    // We just RECEIVED a fully-assembled ARQ super-message whose first
    // chunk was tagged with a JS8 MSG directive. Mirror the inbox-store
    // pattern at processCommandActivity.cpp:641-655 (the existing
    // single-frame MSG TO: handler) so the operator's inbox shows this
    // exactly like a short-form MSG TO: arrival. Then pop a modeless
    // dialog confirming the inbox deposit — parallels the sender-side
    // success dialog in onChunkedSendComplete.
    qCWarning(chunkedarq_js8)
        << "[ARQ-RX] inboxMessageReceived → inbox: from=" << fromCall
        << "addressee=" << addressee << "msgId=" << msgId
        << "bodyChars=" << body.size();

    // [MSG TO: VIA ARQ 2026-06-12 build 255]
    // Distinguish bare MSG vs MSG TO: <addr> and route accordingly,
    // mirroring the on-air handler at processCommandActivity.cpp:675-
    // 768. Two routing paths and a prefix-strip step:
    //
    //   • Bare MSG (addressee empty) → cd.to = m_baseCall,
    //     addCommandToMyInbox(cd) (our UNREAD slot).
    //   • MSG TO: <addr> where addr == us → same as bare MSG
    //     (we're the addressee, so it lands in our UNREAD).
    //   • MSG TO: <addr> where addr is someone else → cd.to =
    //     base_callsign(addr), addCommandToStorage("STORE", cd) so
    //     the addressee can later QUERY-retrieve. Gated on
    //     !m_config.relay_off() (don't store-and-forward if relaying
    //     is off).
    //
    // Also: strip the "MSG TO: <addr>" or "MSG " prefix from the
    // assembled body before storing — cd.text should contain only the
    // actual message content. The on-air pattern at
    // processCommandActivity.cpp:683-686 strips the addressee from
    // d.text; ARQ side now does the equivalent on the assembled body.
    QString const baseAddr = addressee.isEmpty()
                                 ? QString()
                                 : Radio::base_callsign(addressee);
    bool const addressedToUs =
        baseAddr.isEmpty() || baseAddr == m_baseCall;
    QString const to = addressedToUs ? m_baseCall : baseAddr;

    // Strip the leading "MSG TO: <addr>" or "MSG " prefix from body.
    static QRegularExpression const msgToPrefixRe(
        QStringLiteral(R"(^\s*MSG\s+TO\s*:\s*[A-Z0-9/@]+\s*)"),
        QRegularExpression::CaseInsensitiveOption);
    static QRegularExpression const msgPrefixRe(
        QStringLiteral(R"(^\s*MSG\s+)"),
        QRegularExpression::CaseInsensitiveOption);
    QString cleanText = body.trimmed();
    if (auto const m = msgToPrefixRe.match(cleanText); m.hasMatch()) {
        cleanText = cleanText.mid(m.capturedLength()).trimmed();
    } else if (auto const m2 = msgPrefixRe.match(cleanText); m2.hasMatch()) {
        cleanText = cleanText.mid(m2.capturedLength()).trimmed();
    }

    CommandDetail cd = {};
    cd.cmd          = addressee.isEmpty() ? QStringLiteral(" MSG ")
                                          : QStringLiteral(" MSG TO:");
    cd.from         = fromCall;
    cd.to           = to;
    cd.text         = cleanText;
    cd.utcTimestamp = DriftingDateTime::currentDateTimeUtc();
    cd.submode      = m_nSubMode;
    cd.offset       = freq();
    cd.dial         = m_freqNominal;
    cd.snr          = 0;
    cd.tdrift       = 0;
    // [MSG TO: FIELD COVERAGE 2026-06-12 build 259]
    // Populate the same fields the on-air MSG TO: handler at
    // processCommandActivity.cpp:749-763 sets, so the inbox display
    // has every value the on-air store would have produced. Previously
    // these were left at their default-init values (0/""), which
    // matches the storage-time params for FROM but apparently breaks
    // some downstream display logic per operator's report that the
    // FROM column appears empty for ARQ-deposited entries.
    cd.bits         = Varicode::JS8CallLast;
    cd.relayPath    = fromCall;  // single-hop: relay-path = sender
    // (extra and grid stay empty — they come from on-air wire
    //  metadata that ARQ chunks don't carry forward)

    QString storageType;
    if (addressedToUs) {
        // Land in our own UNREAD inbox.
        addCommandToMyInbox(cd);
        storageType = QStringLiteral("UNREAD");
    } else {
        // Store-and-forward for the third-party addressee, but only if
        // relaying is enabled (matches the on-air gate at
        // processCommandActivity.cpp:742).
        if (m_config.relay_off()) {
            qCWarning(chunkedarq_js8)
                << "[ARQ-RX] MSG TO: addressed to" << baseAddr
                << "but relay_off=true — DISCARDED, not stored";
            return;
        }
        addCommandToStorage(QStringLiteral("STORE"), cd);
        storageType = QStringLiteral("STORE");
    }

    qCWarning(chunkedarq_js8)
        << "[ARQ-RX] MSG-via-ARQ stored: type=" << storageType
        << "cd.from=" << cd.from << "cd.to=" << cd.to
        << "cd.cmd=" << cd.cmd
        << "cd.text(first40)=" << cd.text.left(40)
        << "cleanTextChars=" << cleanText.size();

    // Operator-visible notification (matches the existing single-frame
    // MSG-TO handler at processCommandActivity.cpp:660 — `tryNotify(
    // "inbox", ...)`).
    tryNotify(QStringLiteral("inbox"), m_nSubMode);

    auto *box = new QMessageBox(QMessageBox::Information,
                                tr("ARQ super-message saved to inbox"),
                                tr("From: %1\n"
                                   "Addressee: %2\n"
                                   "Storage: %3\n"
                                   "Super-message #%4\n\n"
                                   "Reassembled %5 chars; saved to %6.")
                                    .arg(fromCall)
                                    .arg(to)
                                    .arg(storageType)
                                    .arg(msgId)
                                    .arg(cleanText.size())
                                    .arg(storageType.toLower()),
                                QMessageBox::Ok, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    box->show();
}

void UI_Constructor::onChunkedProgressUpdate(int chunkId, int total,
                                             int retries) {
    // [STATUS-BAR ARQ PROGRESS 2026-06-11 build 252]
    // Override the leftmost status-bar widget (last_tx_label) with
    // "ARQ: #x/y (z repeats)". On the FIRST update of a session, cache
    // whatever was there so onChunkedProgressEnd can restore it.
    // Refreshed on each chunk-send / retransmit.
    if (!m_lastTxLabelCacheValid) {
        m_lastTxLabelCache = last_tx_label.text();
        m_lastTxLabelCacheValid = true;
    }
    last_tx_label.setText(QString("ARQ: #%1/%2 (%3 repeats)")
                              .arg(chunkId)
                              .arg(total)
                              .arg(retries));
}

void UI_Constructor::onChunkedProgressEnd() {
    if (m_lastTxLabelCacheValid) {
        last_tx_label.setText(m_lastTxLabelCache);
        m_lastTxLabelCache.clear();
        m_lastTxLabelCacheValid = false;
    }
}

void UI_Constructor::onChunkedRelayMessageReceived(QString const &fromCall,
                                                   QString const &body,
                                                   int            msgId) {
    // [RELAY-VIA-ARQ RX hook 2026-06-10 build 243]
    // Assembled super-msg body shape: "<targetCall>> <inner text> <CRC>"
    // where <CRC> is the 3-char 16-bit checksum the sender computed
    // over <inner text>. Populate m_messageBuffer to look like a
    // freshly-arrived multi-frame on-air ">" cmd; processBufferedActivity
    // will validate the checksum, then the existing relay handler at
    // processCommandActivity.cpp:561 fires and TXs the forward.
    //
    // We don't strip the checksum here — processBufferedActivity does
    // that (its message.right(3) / message.left(len-4) logic).
    // [RELAY REGEX 2026-06-10 build 245] Allow no-space form
    // "<via>>rest" (matches the send-side regex in mainwindow.cpp).
    static QRegularExpression const re(
        QStringLiteral(R"(^\s*(?<via>[A-Z0-9/]+)>\s*(?<rest>\S.*)$)"),
        QRegularExpression::CaseInsensitiveOption);
    auto const m = re.match(body);
    if (!m.hasMatch()) {
        qCWarning(chunkedarq_js8)
            << "[ARQ-RX] relay body parse failed — discarding:" << body;
        return;
    }

    QString const via  = m.captured("via").toUpper();
    QString const rest = m.captured("rest").trimmed();

    qCWarning(chunkedarq_js8)
        << "[ARQ-RX] relay → injecting into m_messageBuffer: from="
        << fromCall << "via=" << via << "msgId=" << msgId
        << "innerLen=" << rest.size();

    int const offset  = freq();
    auto const nowUtc = DriftingDateTime::currentDateTimeUtc();

    CommandDetail cd = {};
    cd.from         = fromCall;
    cd.to           = via;
    cd.cmd          = QStringLiteral(">");
    cd.bits         = Varicode::JS8CallFirst;
    cd.utcTimestamp = nowUtc;
    cd.submode      = m_nSubMode;
    cd.offset       = offset;
    cd.dial         = m_freqNominal;
    cd.snr          = 0;
    cd.tdrift       = 0;

    ActivityDetail msg = {};
    msg.text         = rest;                  // "N7W STATUS? XYZ"
    msg.bits         = Varicode::JS8CallLast; // closes the buffer
    msg.utcTimestamp = nowUtc;
    msg.submode      = m_nSubMode;
    msg.offset       = offset;
    msg.dial         = m_freqNominal;
    msg.snr          = 0;
    msg.tdrift       = 0;
    msg.shouldDisplay = false;

    auto &buf = m_messageBuffer[offset];
    buf.cmd = cd;
    buf.msgs.clear();
    buf.msgs.append(msg);
    // processBufferedActivity runs on the next RX-activity tick; it
    // concatenates buffer.msgs into "N7W STATUS? XYZ", strips and
    // validates the 3-char CRC, then appends cd to m_rxCommandQueue.
    // processCommandActivity then fires the ">" handler at
    // processCommandActivity.cpp:561, which TXs the forward via the
    // standard TX queue.
}

void UI_Constructor::onChunkedFileMessageReceived(QString const &fromCall,
                                                  QString const &body,
                                                  int            msgId) {
    // [FILE-XFER 2026-06-16 build 276] RX hook for ARQ file-transfer
    // super-messages. body is the fully-assembled
    // "F/V1 <header-b32> <payload-b32>" wire form. Decode header,
    // confirm with operator, verify SHA-256, write to disk.
    FileTransfer::FileHeader header;
    QString payloadBase32;       // V1: still-encoded payload
    QByteArray payloadBytes;     // V2: decoded + VERIFIED payload
    int wireVersion = 0;
    // [BUILD 340] Web link (L/V1): NOT a file — no save dialog, no
    // Downloads folder. Show the link directly (clickable, escaped,
    // FULL URL as the link text — receiver sees exactly where it
    // goes) plus a conversation-panel line for scrollback. The
    // decoder enforces the http(s) scheme, so a corrupt or hostile
    // payload can't produce a link to who-knows-what.
    if (QString linkUrl; FileTransfer::splitLinkBody(body, linkUrl)) {
        qCWarning(chunkedarq_js8)
            << "[LT-RX] web link received — peer=" << fromCall
            << "msgId=" << msgId << "url=" << linkUrl;
        auto const summary =
            QStringLiteral("%1: [web link received: %2]")
                .arg(fromCall, linkUrl);
        displayTextForFreq(summary, freq(),
                           DriftingDateTime::currentDateTimeUtc(),
                           /*isTx=*/false, /*isNewLine=*/true,
                           /*isLast=*/true, m_nSubMode);
        tryNotify(QStringLiteral("directed"), m_nSubMode);
        auto *linkBox = new QMessageBox(this);
        linkBox->setWindowTitle(tr("Web link received"));
        linkBox->setTextFormat(Qt::RichText);
        linkBox->setText(
            QStringLiteral("<p>%1 sent a web link:</p>"
                           "<p><a href=\"%2\">%2</a></p>")
                .arg(fromCall.toHtmlEscaped(),
                     linkUrl.toHtmlEscaped()));
        linkBox->setIcon(QMessageBox::Information);
        linkBox->setStandardButtons(QMessageBox::Ok);
        linkBox->setWindowModality(Qt::NonModal);
        linkBox->setAttribute(Qt::WA_DeleteOnClose);
        linkBox->show();
        return;
    }
    if (FileTransfer::splitWireBody(body, header, payloadBase32)) {
        wireVersion = 1;
    } else if (FileTransfer::splitWireBodyV2(body, header,
                                             payloadBytes)) {
        wireVersion = 2;
    } else {
        // [BUILD 339 TODO #103] F/V-family body that neither decoder
        // accepts: either a NEWER format than this build supports, or
        // corruption that survived per-chunk CRC. Polite notice
        // instead of silence (and never base32 gibberish — the family
        // detector kept it out of the conversation panel).
        static QRegularExpression const verRe{
            QStringLiteral(R"(^F/V(\d+))")};
        auto const vm = verRe.match(body);
        QString const verText = vm.hasMatch() ? vm.captured(1)
                                              : QStringLiteral("?");
        qCWarning(chunkedarq_js8)
            << "[FT-RX] body parse failed — peer=" << fromCall
            << "msgId=" << msgId << "bodyLen=" << body.size()
            << "wireVersion=" << verText;
        auto *box = new QMessageBox(this);
        box->setWindowTitle(tr("File transfer not supported"));
        box->setText(tr("%1 sent a file in format F/V%2, which this "
                        "build cannot decode.\n\nIf the format is "
                        "newer than this build, update to receive "
                        "it; otherwise the transfer arrived "
                        "corrupted.").arg(fromCall, verText));
        box->setIcon(QMessageBox::Information);
        box->setStandardButtons(QMessageBox::Ok);
        box->setWindowModality(Qt::NonModal);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->show();
        return;
    }
    qCWarning(chunkedarq_js8)
        << "[FT-RX] wire format V" << wireVersion
        << "accepted — peer=" << fromCall << "msgId=" << msgId;

    qCWarning(chunkedarq_js8)
        << "[FT-RX] header parsed — peer=" << fromCall
        << "msgId=" << msgId
        << "name=" << header.name
        << "bytes=" << header.bytes;

    promptAndSaveReceivedFile(fromCall, header, payloadBase32,
                              payloadBytes, wireVersion, msgId);
}

// [TODO #107] Accept-dialog + save + link-scan tail, extracted from
// onChunkedFileMessageReceived so the V3 native path shares it from
// the header-parsed point on — V1 (base32 text), V2 (verified bytes
// from text wire), V3 (verified bytes from native frames) all
// converge here.
void UI_Constructor::promptAndSaveReceivedFile(
    QString const &fromCall, FileTransfer::FileHeader const &header,
    QString const &payloadBase32, QByteArray const &payloadBytes,
    int const wireVersion, int const msgId) {
    // [build 277 2026-06-16] Modeless accept dialog. The Build 276
    // version used QMessageBox::question() which is blocking-modal —
    // operator missed it when not actively watching the screen, and
    // it tied up the GUI thread. Now we new up a QMessageBox on the
    // heap, parent it to `this` so it survives but doesn't block,
    // and connect to its finished() signal for the save/discard
    // decision. Operator can keep using the rest of JS8Call while
    // the dialog sits; the dialog stays open until clicked.
    // [ICS213 2026-08-18] Detection first — the prompt itself says
    // "ICS-213 form" for a recognized form. Our filename convention,
    // or the form banner in the decoded bytes (V2/V3; V1 payload is
    // still base32 here, so the name prefix carries V1 detection).
    bool const isIcs213Form =
        header.name.startsWith(QStringLiteral("ICS213_"),
                               Qt::CaseInsensitive) ||
        payloadBytes.contains("GENERAL MESSAGE (ICS-213)");
    // A _REPLY form completes the exchange — no reply-to-reply.
    bool const isReplyForm =
        header.name.contains(QStringLiteral("_REPLY"),
                             Qt::CaseInsensitive);
    // [ICS213] Form format shown on the prompt — it can bear on the
    // chosen disposition. Sparse replies label as reply data; V1
    // payload (still base32 here) probes as unrecognized.
    QString fmtLine;
    if (isIcs213Form) {
        QString label;
        if (ICS213Dialog::isSparseReply(payloadBytes))
            label = QStringLiteral("Reply data");
        else switch (ICS213Dialog::probeFormat(payloadBytes)) {
        case 0: label = QStringLiteral("Standard layout"); break;
        case 1: label = QStringLiteral("Compact"); break;
        case 2: label = QStringLiteral("Winlink XML"); break;
        default: label = QStringLiteral("(unrecognized)"); break;
        }
        fmtLine = QStringLiteral("    Format:  %1\n").arg(label);
    }
    // [ICS213 2026-08-18] Reply arrivals carry the ORIGINAL form's
    // serial in the name (ICS213_<serial>_..._REPLY.*) — surface it
    // in the title (operator spec: "ICS-213 Reply - WM8Q-0006").
    QString const replySerial =
        (isIcs213Form && isReplyForm)
            ? ICS213Dialog::serialFromFormName(header.name)
            : QString();
    auto const msg = QStringLiteral(
        "Incoming %1 from %2\n\n"
        "    Name:  %3\n"
        "    Size:  %4 bytes\n%5\n"
        "Save to file storage?")
        .arg(isIcs213Form
                 ? (isReplyForm ? QStringLiteral("ICS-213 reply")
                                : QStringLiteral("ICS-213 form"))
                 : QStringLiteral("file"),
             fromCall, header.name)
        .arg(header.bytes)
        .arg(fmtLine);
    auto *box = new QMessageBox(this);
    box->setWindowTitle(
        !isIcs213Form ? QStringLiteral("Incoming file")
        : isReplyForm
            ? (replySerial.isEmpty()
                   ? QStringLiteral("ICS-213 Reply")
                   : QStringLiteral("ICS-213 Reply - %1")
                         .arg(replySerial))
            : QStringLiteral("Incoming ICS-213 form"));
    box->setText(msg);
    box->setIcon(QMessageBox::Question);
    box->setStandardButtons(QMessageBox::Save | QMessageBox::Discard);
    box->setDefaultButton(QMessageBox::Save);
    box->setWindowModality(Qt::NonModal);
    box->setAttribute(Qt::WA_DeleteOnClose);
    QPushButton *showBtn =
        isIcs213Form
            ? box->addButton(QStringLiteral("Save && Show now"),
                             QMessageBox::AcceptRole)
            : nullptr;
    // [ICS213 reply] "Reply" = save + open the form in reply mode.
    // Gated by the shared interpretability probe — if the bytes
    // don't parse (foreign layout, or a V1 payload still in base32
    // at this point), the button shows but stays disabled.
    QPushButton *replyBtn = nullptr;
    if (isIcs213Form && !isReplyForm) {
        replyBtn = box->addButton(QStringLiteral("Reply"),
                                  QMessageBox::AcceptRole);
        if (ICS213Dialog::probeFormat(payloadBytes) < 0) {
            replyBtn->setEnabled(false);
            replyBtn->setToolTip(
                QStringLiteral("Unable to interpret the form"));
        } else {
            replyBtn->setToolTip(QStringLiteral(
                "The form will be saved, and you may reply "
                "immediately"));
        }
    }

    // Capture by value — payloadBase32 / fromCall / header / msgId
    // need to survive the dialog's lifetime even if the original
    // signal-emit frame is long gone. Save dir resolved at click
    // time so a Settings change between receive and accept is
    // honored (Phase 2 makes the dir operator-configurable).
    QString const payloadCopy = payloadBase32;
    QByteArray const bytesCopy = payloadBytes;
    int const verCopy = wireVersion;
    QString const fromCopy    = fromCall;
    FileTransfer::FileHeader const headerCopy = header;
    int const msgIdCopy = msgId;

    connect(box, &QMessageBox::finished, this,
            [this, box, payloadCopy, bytesCopy, verCopy, fromCopy,
             headerCopy, msgIdCopy, showBtn, replyBtn]
            (int result) {
        QMessageBox::StandardButton const choice =
            static_cast<QMessageBox::StandardButton>(result);
        bool const wantShow =
            showBtn && box->clickedButton() == showBtn;
        bool const wantReply =
            replyBtn && box->clickedButton() == replyBtn;
        if (choice != QMessageBox::Save && !wantShow && !wantReply) {
            qCWarning(chunkedarq_js8)
                << "[FT-RX] operator declined save — peer=" << fromCopy
                << "msgId=" << msgIdCopy
                << "name=" << headerCopy.name;
            return;
        }

        // [TODO #106] Path shared with the File-menu "open file
        // transfer directory" action — single definition.
        QString const saveDir = FileTransfer::receiveDirectory();
        QString err;
        // V1: decode + verify + write. V2/V3: the split already
        // decoded and verified — write the bytes directly.
        QString const savedPath =
            (verCopy >= 2)
                ? FileTransfer::writeReceivedFile(saveDir, headerCopy,
                                                  bytesCopy, &err)
                : FileTransfer::assembleReceivedFile(
                      saveDir, headerCopy, payloadCopy, &err);
        if (savedPath.isEmpty()) {
            qCWarning(chunkedarq_js8)
                << "[FT-RX] assembleReceivedFile failed:" << err
                << "peer=" << fromCopy << "msgId=" << msgIdCopy;
            auto *errBox = new QMessageBox(this);
            errBox->setWindowTitle(QStringLiteral("File transfer failed"));
            errBox->setText(
                QStringLiteral("Could not save received file from %1:\n%2")
                    .arg(fromCopy, err));
            errBox->setIcon(QMessageBox::Warning);
            errBox->setStandardButtons(QMessageBox::Ok);
            errBox->setWindowModality(Qt::NonModal);
            errBox->setAttribute(Qt::WA_DeleteOnClose);
            errBox->show();
            return;
        }

        qCWarning(chunkedarq_js8)
            << "[FT-RX] saved file — peer=" << fromCopy
            << "msgId=" << msgIdCopy << "path=" << savedPath;

        // [ICS213 sparse merge] A reply wire file carries ONLY the
        // new 9/10 data — rejoin it with OUR retained copy of the
        // original form (REF names it; it lives in our ICS213 send
        // folder). Read the SAVED file, not payloadBytes: this way
        // V1 (decoded at save) and V2/V3 take the same path. On any
        // mismatch the sparse file is kept as-is — still readable.
        QString finalPath = savedPath;
        if (QFile sf{savedPath}; sf.open(QIODevice::ReadOnly)) {
            QByteArray const savedBytes = sf.readAll();
            sf.close();
            if (ICS213Dialog::isSparseReply(savedBytes)) {
                QString const sparse = QString::fromUtf8(savedBytes);
                QString ref;
                if (int const i =
                        sparse.indexOf(QStringLiteral("REF: "));
                    i >= 0) {
                    int const e =
                        sparse.indexOf(QLatin1Char('\n'), i);
                    ref = sparse
                              .mid(i + 5,
                                   (e < 0 ? sparse.size() : e) -
                                       (i + 5))
                              .trimmed();
                }
                QString merged;
                if (!ref.isEmpty() && !ref.contains(QLatin1Char('/'))) {
                    QString const origPath =
                        QDir{FileTransfer::receiveDirectory()}
                            .filePath(QStringLiteral("ICS213/") + ref);
                    if (QFile of{origPath};
                        of.open(QIODevice::ReadOnly)) {
                        merged = ICS213Dialog::mergeReply(
                            QString::fromUtf8(of.readAll()), sparse);
                        if (merged.isEmpty())
                            qCWarning(chunkedarq_js8)
                                << "[FT-RX] reply merge shape "
                                   "mismatch — keeping sparse file;"
                                << "orig=" << origPath;
                    } else {
                        qCWarning(chunkedarq_js8)
                            << "[FT-RX] reply original not found —"
                            << "keeping sparse file; ref=" << ref;
                    }
                }
                if (!merged.isEmpty()) {
                    // Completed form wears the ORIGINAL's extension
                    // (an .xml form's reply arrives as .txt wire).
                    QString const outName =
                        QFileInfo{headerCopy.name}.completeBaseName() +
                        QLatin1Char('.') + QFileInfo{ref}.suffix();
                    QString const outPath =
                        QDir{FileTransfer::receiveDirectory()}
                            .filePath(outName);
                    if (QFile out{outPath};
                        out.open(QIODevice::WriteOnly |
                                 QIODevice::Truncate)) {
                        out.write(merged.toUtf8());
                        out.close();
                        if (outPath != savedPath)
                            QFile::remove(savedPath);
                        finalPath = outPath;
                        qCWarning(chunkedarq_js8)
                            << "[FT-RX] sparse reply merged into"
                            << outPath;
                    }
                }
            }
        }
        // Surface the save in the conversation panel so it survives
        // panel scrollback (the modeless dialog will be dismissed by
        // the operator at some point).
        auto const summary =
            QStringLiteral("%1: [file received: %2 → %3]")
                .arg(fromCopy, headerCopy.name, finalPath);
        auto const now = DriftingDateTime::currentDateTimeUtc();
        displayTextForFreq(summary, freq(), now,
                           /*isTx=*/false,
                           /*isNewLine=*/true,
                           /*isLast=*/true,
                           m_nSubMode);

        // [ICS213 reply] Save is done — open the reply form wired
        // back to the original sender.
        if (wantReply)
            openIcs213Reply(finalPath, fromCopy);

        // [ICS213] "Show now": open the saved form in a modeless
        // read-only viewer.
        if (wantShow) {
            QFile vf{finalPath};
            if (vf.open(QIODevice::ReadOnly)) {
                auto *view = new QDialog(this);
                // [ICS213 2026-08-18] Same title style for BOTH
                // viewer flavors (operator spec): the serial IS the
                // form's identity — "ICS-213 Form - K9AVT-0006" /
                // "ICS-213 Reply - K9AVT-0006". Sender-call fallback
                // only when the name carries no serial.
                QString const vSerial =
                    ICS213Dialog::serialFromFormName(headerCopy.name);
                bool const vReply =
                    headerCopy.name.contains(QStringLiteral("_REPLY"),
                                             Qt::CaseInsensitive);
                view->setWindowTitle(
                    vSerial.isEmpty()
                        ? QStringLiteral("ICS-213 - from %1")
                              .arg(fromCopy)
                        : QStringLiteral("ICS-213 %1 - %2")
                              .arg(vReply ? QStringLiteral("Reply")
                                          : QStringLiteral("Form"),
                                   vSerial));
                view->setAttribute(Qt::WA_DeleteOnClose);
                auto *lay = new QVBoxLayout(view);
                auto *txt = new QPlainTextEdit(view);
                txt->setReadOnly(true);
                QFont mono{QStringLiteral("Monospace")};
                mono.setStyleHint(QFont::TypeWriter);
                txt->setFont(mono);
                txt->setPlainText(
                    QString::fromUtf8(vf.readAll()));
                lay->addWidget(txt);
                view->resize(560, 620);
                view->show();
                view->raise();
            }
        }

        // [BUILD 337 TODO #95] If the received file is TEXT and
        // contains http(s) URLs, show them in the confirmation
        // dialog as clickable links. Security rules: EVERYTHING is
        // HTML-escaped before injection (a hostile file must not
        // inject markup), only well-formed http(s) URLs linkify, and
        // the FULL URL is the link text — the receiver sees exactly
        // where a link goes before clicking. Binary files (NUL in
        // the first 64 KB) are skipped entirely.
        QFile saved(finalPath);
        if (saved.open(QIODevice::ReadOnly)) {
            QByteArray const head = saved.read(64 * 1024);
            saved.close();
            if (!head.contains('\0')) {
                static QRegularExpression const kUrlRe{
                    QStringLiteral(R"(\bhttps?://[^\s<>"']+)"),
                    QRegularExpression::CaseInsensitiveOption};
                QStringList urls;
                auto it = kUrlRe.globalMatch(QString::fromUtf8(head));
                while (it.hasNext() && urls.size() < 10) {
                    QString u = it.next().captured(0);
                    // Trailing sentence punctuation isn't part of
                    // the URL.
                    while (!u.isEmpty() &&
                           QStringLiteral(").,;:!?'").contains(u.back())) {
                        u.chop(1);
                    }
                    if (!u.isEmpty() && !urls.contains(u)) {
                        urls << u;
                    }
                }
                if (!urls.isEmpty()) {
                    QString html =
                        QStringLiteral("<p>File received from %1:"
                                       "<br>%2</p>"
                                       "<p>Link%3 found in the file:"
                                       "</p>")
                            .arg(fromCopy.toHtmlEscaped(),
                                 headerCopy.name.toHtmlEscaped(),
                                 urls.size() > 1 ? QStringLiteral("s")
                                                 : QString());
                    for (QString const &u : urls) {
                        html += QStringLiteral(
                                    "<a href=\"%1\">%1</a><br>")
                                    .arg(u.toHtmlEscaped());
                    }
                    auto *linkBox = new QMessageBox(this);
                    linkBox->setWindowTitle(tr("File received"));
                    linkBox->setTextFormat(Qt::RichText);
                    linkBox->setText(html);
                    linkBox->setIcon(QMessageBox::Information);
                    linkBox->setStandardButtons(QMessageBox::Ok);
                    linkBox->setWindowModality(Qt::NonModal);
                    linkBox->setAttribute(Qt::WA_DeleteOnClose);
                    linkBox->show();
                }
            }
        }
    });

    box->show();
    box->raise();
    box->activateWindow();
}

// [TODO #107] V3 TX hook: transmit one native chunk — the marker text
// (when present) through the proven box path, then the raw binary
// frames injected behind it. ORDER IS LOAD-BEARING: marker frames are
// built BY prepareNextMessageFrame's typeahead branch, so the
// m_nativeBinaryTxActive queue-guard arms only AFTER that build; on
// markerless chunks the guard arms BEFORE prepare (nothing to build —
// the guard protects the injected frames from any stale-box refill).
void UI_Constructor::onNativeChunkWantToTransmit(
    QString const &peer, QString const &markerText, int const chunkId,
    int const totalChunks, QByteArray const &markerFrame9,
    QByteArray const &chunkBytes) {
    if (!ui->extFreeTextMsgEdit) {
        return;
    }
    // A (re)send REPLACES the chunk's frames — flush leftovers from
    // the previous transmission first (V2's counterpart: the typeahead
    // rebuild clears the queue before re-framing the box). Without
    // this every NACK/timeout retransmit APPENDED 8 more frames
    // (bench 2026-07-19: depth 19, TX keyed near-continuously and
    // went deaf to the peer's ACK/NACK).
    if (!m_txFrameQueue.isEmpty()) {
        qCWarning(chunkedarq_js8)
            << "[V3-TX] flushing" << m_txFrameQueue.size()
            << "stale queued frames before chunk (re)send";
        resetMessageTransmitQueue();
    }
    if (!markerText.isEmpty()) {
        m_nativeBinaryTxActive = false;  // allow the marker build
        addMessageText(markerText, /*clear=*/true);
        {
            QSignalBlocker const block(ui->startTxButton);
            ui->startTxButton->setChecked(true);
        }
        if (!prepareNextMessageFrame()) {
            qCWarning(chunkedarq_js8)
                << "[V3-TX] marker frame build failed; chunk stranded:"
                << markerText.left(40);
            return;
        }
        // [BUILD 344 binMarker] Binary marker AHEAD of the payload —
        // opens/refreshes the receiver window even if the text
        // marker's varicode frames are clipped (on-air msg-33).
        injectNativeMarkerFrame(markerFrame9);
        injectNativeBinaryFrames(chunkId, chunkBytes);
        m_nativeBinaryTxActive = true;
    } else {
        injectNativeMarkerFrame(markerFrame9);
        injectNativeBinaryFrames(chunkId, chunkBytes);
        m_nativeBinaryTxActive = true;
        {
            QSignalBlocker const block(ui->startTxButton);
            ui->startTxButton->setChecked(true);
        }
        if (!prepareNextMessageFrame()) {
            qCWarning(chunkedarq_js8)
                << "[V3-TX] binary frame pop failed; chunk stranded";
            return;
        }
    }
    // Per-burst conversation-window feedback (operator request
    // 2026-07-19): one standard-furniture line per transmitted burst
    // — directed format + EOT marker so it renders/parses like any
    // other message (retransmitted bursts print again, which is the
    // point: each keyup is visible). MARKERLESS bursts only: marker
    // chunks (1, 5, 9…) already display their marker text through
    // the normal TX path, and the bracket line duplicated it
    // (operator 2026-07-19).
    if (markerText.isEmpty()) {
        displayTextForFreq(
            QStringLiteral("%1: %2 [Submsg %3 of %4] %5")
                .arg(m_config.my_callsign().trimmed(), peer)
                .arg(chunkId)
                .arg(totalChunks)
                .arg(m_config.eot()),
            freq(), DriftingDateTime::currentDateTimeUtc(),
            /*isTx=*/true, /*isNewLine=*/true, /*isLast=*/true,
            m_nSubMode);
    }
    if (!m_auto) {
        auto_tx_mode(true);
    }
}

// [TODO #107] RX side collected one V3 burst (all frames, PCRC OK,
// ACK staged) — mirror of the TX-side per-burst line above.
void UI_Constructor::onNativeChunkCollected(QString const &peer,
                                            int const chunkId,
                                            int const totalChunks) {
    // [BUILD 354 rxsession] Banner driven by rxSessionChanged.
    displayTextForFreq(
        QStringLiteral("%1: %2 [Submsg %3 of %4] %5")
            .arg(peer, m_config.my_callsign().trimmed())
            .arg(chunkId)
            .arg(totalChunks)
            .arg(m_config.eot()),
        freq(), DriftingDateTime::currentDateTimeUtc(),
        /*isTx=*/false, /*isNewLine=*/true, /*isLast=*/true,
        m_nSubMode);
}

void UI_Constructor::onNativeMarkerSeen(QString const &peer,
                                        int const chunkId,
                                        int const totalChunks) {
    // [BUILD 354 rxsession] Marker handling feeds the Manager's
    // receive-session machine directly; nothing to render here.
    Q_UNUSED(peer);
    Q_UNUSED(chunkId);
    Q_UNUSED(totalChunks);
}

// [BUILD 354 rxsession] The UI is now a PURE RENDERER of the
// Manager's receive-session machine: phase == Receiving → banner up
// (with live (N/T) when known); any other phase → default
// placeholder. All timing, progress, and lifecycle live in the
// Manager (rxSessionActivity / rxSessionEnd / the per-peer stall
// timer). The Build 352-354 flag pile this replaces —
// m_arqPlaceholderTimer, m_arqBannerProgress, m_arqBannerCycleFrames
// and four scattered refresh call-sites — is DELETED (operator,
// 2026-08-02: "are we operating outside of the control of a state
// machine, with flags/patches/hacks?" — we were).
// [BUILD 355 oneban] SINGLE-WRITER: this slot no longer writes the
// placeholder itself — it only updates m_rxBannerText and asks the
// ONE writer (refreshOutgoingPlaceholder) to repaint. The previous
// design had TWO writers (this slot + refreshOutgoingPlaceholder's
// seven call sites: selection changes, negotiation/lock toggles…);
// any of those triggers firing mid-receive clobbered the banner with
// the default prompt until the next session event re-raised it —
// operator saw the text alternate ("race?"-looking, actually
// deterministic last-writer-wins). Same one-fact-one-home rule as
// [[feedback_new_phase_is_same_machine]]. The save/restore snapshot
// (m_arqPlaceholderOrig) is DELETED — the writer recomputes the
// correct default itself, nothing to snapshot.
void UI_Constructor::onRxSessionChanged(QString const &peer,
                                        int const phase,
                                        int const chunkId,
                                        int const totalChunks) {
    Q_UNUSED(peer);  // single-operator: latest session renders
    if (phase == static_cast<int>(ChunkedArq::RxPhase::Receiving)) {
        QString const progress =
            (chunkId > 0 && totalChunks > 0)
                ? tr(" (%1/%2)").arg(chunkId).arg(totalChunks)
                : QString();
        m_rxBannerText =
            tr("MULTI-PART MSG IN PROGRESS%1...\n"
               "TYPE AN OUTGOING MESSAGE HERE,\n"
               "WAIT TO SEND ('HALT' TO CANCEL).")
                .arg(progress);
    } else {
        if (!m_rxBannerText.isEmpty()) {
            qCWarning(chunkedarq_js8)
                << "[RX-BANNER] default placeholder restored";
        }
        m_rxBannerText.clear();
    }
    refreshOutgoingPlaceholder();
}

void UI_Constructor::restoreArqPlaceholder() {
    // [BUILD 355 oneban] Kept as a thin alias for existing callers:
    // banner down + repaint via the single writer.
    m_rxBannerText.clear();
    refreshOutgoingPlaceholder();
}

// [TODO #107] V3 RX hook: transfer fully collected — parse the raw
// envelope (decompress + header + size + sha16 verify) and hand off
// to the shared accept/save flow.
void UI_Constructor::onNativeBinaryMessageReceived(
    QString const &fromCall, QByteArray const &envelope,
    int const msgId) {
    FileTransfer::FileHeader header;
    QByteArray payloadBytes;
    if (!FileTransfer::splitWireBodyV3(envelope, header,
                                       payloadBytes)) {
        qCWarning(chunkedarq_js8)
            << "[V3-RX] envelope parse failed — peer=" << fromCall
            << "msgId=" << msgId << "bytes=" << envelope.size();
        auto *box = new QMessageBox(this);
        box->setWindowTitle(tr("File transfer failed"));
        box->setText(tr("%1 sent a Subspace native file transfer "
                        "that did not verify (size or hash "
                        "mismatch).").arg(fromCall));
        box->setIcon(QMessageBox::Warning);
        box->setStandardButtons(QMessageBox::Ok);
        box->setWindowModality(Qt::NonModal);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->show();
        return;
    }
    qCWarning(chunkedarq_js8)
        << "[V3-RX] envelope verified — peer=" << fromCall
        << "msgId=" << msgId << "name=" << header.name
        << "bytes=" << header.bytes;
    promptAndSaveReceivedFile(fromCall, header, QString(),
                              payloadBytes, /*wireVersion=*/3, msgId);
}
