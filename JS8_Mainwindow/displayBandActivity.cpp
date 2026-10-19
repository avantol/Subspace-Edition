

/** \file
 * @brief member function of the UI_Constructor class
 *  displays the current band activity in the left pane of the UI
 */

#include "JS8_UI/mainwindow.h"

#include "JS8_Main/ChunkedArq.h"
#include "JS8_Widgets/BandActivityMessageDelegate.h"   // [#240] one callsign-prefix authority

// [bandcall] Shared "CALL: " prefix parse -- the ONE authority for
// attributing a frame to its sender in the band activity view (was
// inline in the render loop). Format is always "CALLSIGN: @GROUP/msg".
QString UI_Constructor::frameFromCall(QString const &text, int bits) {
    // [#240 LAYER 1, operator 2026-09-20: "a sender prefix is
    // meaningful only on a JS8CallFirst frame"] A message's sender
    // rides its FIRST frame: the compound frame precedes the directed
    // frame in buildMessageFrames, a plain-text message's "CALL: " is
    // typed at its start, HB/ACK/SNR are single First|Last frames. A
    // "X: " on any later frame is body text -- a CommStat remark
    // ("2,KCNA: N."), a MeshCore tag ("JS8MESH: ..."), a quoted call --
    // and layer 0's shape test cannot tell a real callsign in a body
    // from a sender. The protocol says where a message starts (bit 72);
    // use it. A missed First frame yields nothing here and falls to the
    // offset fallback (mostLikelyCallAtOffset), never to body text.
    if ((bits & Varicode::JS8CallFirst) != Varicode::JS8CallFirst)
        return QString();
    int colonPos = text.indexOf(": ");
    if (colonPos > 0 && colonPos <= 15) {
        QString candidate = text.left(colonPos).trimmed();
        // [operator field-caught 2026-09-14] "2,KCNA: N. KOREA ..." --
        // the remarks field of a CommStat report, mid-message -- was
        // attributed to a station "2,KCNA" because the old test was
        // only "3-15 chars with a letter and a digit". The ONE
        // authority for callsign shape is Varicode::isValidCallsign
        // (the same test the add-callsign dialog applies), which a
        // comma, a period or a bare word never passes.
        if (candidate.length() >= 3 && candidate.length() <= 15 &&
            Varicode::isValidCallsign(candidate, nullptr)) {
            return candidate;
        }
    }
    return QString();
}

// [bandcall2] Orphan-frame fallback (operator proposal 2026-09-06):
// a frame whose bucket has no identifiable sender is attributed to
// the station LAST SEEN nearest its offset, within the submode's rx
// tolerance -- the same proximity rule the click handler's fallback
// uses. Returns empty when no station is close enough (the frame
// then stays out of callsign mode, as before).
QString UI_Constructor::mostLikelyCallAtOffset(int offset,
                                               int submode) const {
    int const threshold =
        submode >= 0 ? JS8::Submode::rxThreshold(submode) : 10;
    // [orphan15, operator ruling 2026-09-07] A station only claims an
    // unattributed frame by offset if it was heard RECENTLY: last
    // message older than 15 minutes disqualifies it. Keeps a long-gone
    // station's stale offset from swallowing fresh fragments.
    // [audit D1 2026-10-14] the ONE constant, shared with the JTTY
    // continuation rule (jttyLineSenders)
    QDateTime const now = DriftingDateTime::currentDateTimeUtc();
    QString best;
    int bestDist = threshold + 1;
    for (auto it = m_callActivity.constBegin();
         it != m_callActivity.constEnd(); ++it) {
        if (it.value().utcTimestamp.secsTo(now) > kOrphanSenderMaxAgeSecs)
            continue;
        int const dist = qAbs(it.value().offset - offset);
        if (dist <= threshold && dist < bestDist) {
            bestDist = dist;
            best = it.value().call;
        }
    }
    return best;
}

void UI_Constructor::displayBandActivity() {
    auto now = DriftingDateTime::currentDateTimeUtc();
    bool const listByCall = bandListByCall();

    // Reset the header label text to accommodate minimal label setting
    int cols = ui->tableWidgetRXAll->columnCount();
    for (int c = 0; c < cols; ++c) {
        ui->tableWidgetRXAll->horizontalHeaderItem(c)->setText(
            columnLabel(m_origRxHeaderLabelMap[c]));
    }

    ui->tableWidgetRXAll->setFont(m_config.table_font());

    // Selected row identity, read from FIXED columns (mode-aware).
    // [bandcall] The old read took selectedItems().first(), which is
    // whatever column happens to be first VISIBLE -- with the Offset
    // column hidden it read the Time Delta float as an offset.
    int selectedOffset = -1;
    QString selectedCall;
    auto selectedItems = ui->tableWidgetRXAll->selectedItems();
    if (!selectedItems.isEmpty()) {
        int const selRow = selectedItems.first()->row();
        if (auto *it = ui->tableWidgetRXAll->item(selRow, BAOffset))
            selectedOffset = it->data(Qt::UserRole).toInt();
        if (auto *it = ui->tableWidgetRXAll->item(selRow, BACallsign))
            selectedCall = it->data(Qt::UserRole).toString();
    }

    ui->tableWidgetRXAll->setUpdatesEnabled(false);
    {
        // Scroll Position
        auto const currentScrollPos =
            ui->tableWidgetRXAll->verticalScrollBar()->value();

        // Clear the table
        ui->tableWidgetRXAll->setRowCount(0);

        // Sort!
        auto const sort = getSortByReverse("bandActivity", "offset");
        // [operator 2026-10-15, truth table] a kind of decode that is not
        // shown here contributes no rows: rows already listed vanish the
        // moment the switch goes off or the dial moves onto a frequency
        // that does not carry that mode
        bool const showJs8 = js8DecodesShown();
        auto keys = showJs8 ? m_bandActivity.keys() : QList<int>{};
#ifdef JS8_ENABLE_JTTY
        bool const showJtty = jttyDecodesShown();
        // [JTTY] the JTTY map is merged here and nowhere else; a
        // JTTY-only offset becomes a row of its own.
        if (showJtty)
            for (int key : m_jttyActivity.keys())
                if (!keys.contains(key)) keys.append(key);
#endif

        // Compute shouldDisplay for every item per offset BEFORE sort, using
        // the same rules the render loop will use. Sort keys then read from
        // this filtered set so a row's position reflects only items the user
        // will actually see -- preventing the row from being placed by a
        // hidden HB / MSG ID / etc. while displaying a much older visible
        // item's age. Render below reuses these pre-marked items directly.
        int activityAging = m_config.activity_aging();
        bool showHB = ui->actionShow_Band_Heartbeats_and_ACKs->isChecked();
        auto const &myCall = m_config.my_callsign();
        auto const &eot = m_config.eot();
        QMap<int, QList<ActivityDetail>> filtered;
        QMap<int, QStringList> attributed; // [bandcall] per-frame sender
        QMap<int, ActivityDetail> lastVisible;
        for (int key : keys) {
            bool isOffsetSelected = (key == selectedOffset);
            // value(), not []: a JTTY-only key must not create an empty
            // JS8 bucket.
            QList<ActivityDetail> items =
                showJs8 ? m_bandActivity.value(key) : QList<ActivityDetail>{};
#ifdef JS8_ENABLE_JTTY
            if (showJtty)
                items.append(m_jttyActivity.value(key));
#endif

            // [bandcall] Attribute every frame in bucket order: a frame
            // with a "CALL: " prefix belongs to that call; a
            // continuation frame belongs to the last attributed call in
            // this bucket (same rule the per-offset subdivision has
            // always used). [bandcall2] A frame arriving before any
            // call is known in its bucket goes to the station last
            // seen nearest its offset (operator proposal); only when
            // that finds nothing is it left out of callsign mode.
            QStringList attrib;
            // [operator 2026-10-13] The pass runs in BOTH listing modes:
            // a JTTY message's sender is wanted per message by the
            // tooltip and the bolding whichever mode shows it. JS8
            // frames are attributed in callsign mode only, as before.
            {
                QString lastCall;
#ifdef JS8_ENABLE_JTTY
                // [operator 2026-10-13] the JTTY items of this key are
                // m_jttyActivity.value(key) appended in order (above);
                // their attribution comes from the ONE authority
                QStringList const jttySenders =
                    jttyLineSenders(m_jttyActivity.value(key), key);
                int jttyIdx = 0;
#endif
                for (auto const &it : items) {
                    QString const c =
                        listByCall ? frameFromCall(it.text, it.bits) : QString();
                    if (!c.isEmpty())
                        lastCall = c;
                    // [JTTY] a JTTY message is never attributed to a JS8
                    // station seen nearby. [operator 2026-10-13] PER
                    // MESSAGE: the line's own parsed sender first (CQ
                    // FROM, TO FROM, FROM:, DE FROM, second call), the
                    // operator's pick for the offset as the fallback,
                    // else UNKNOWN. In callsign mode a different obvious
                    // sender at one offset starts its own row (2076 Hz:
                    // K9AVT's lines sat unknown until picked, then split).
#ifdef JS8_ENABLE_JTTY
                    // [operator 2026-10-13] ...and a piece with no sender
                    // of its own belongs to the last parsed JTTY sender
                    // earlier in this bucket -- the JS8 continuation rule
                    // (lastCall) on the JTTY side, which the first cut
                    // skipped: K9AVT's four continuation pieces sat on
                    // an UNKNOWN row until a click picked K9AVT for them.
                    if (it.submode == Varicode::JS8CallJTTY) {
                        attrib.append(jttyIdx < jttySenders.size()
                                          ? jttySenders.at(jttyIdx)
                                          : QString());
                        ++jttyIdx;
                        continue;
                    }
#endif
                    attrib.append(!listByCall ? QString()
                                  : lastCall.isEmpty()
                                      ? mostLikelyCallAtOffset(it.offset, it.submode)
                                      : lastCall);
                }
#ifdef JS8_ENABLE_JTTY
                // the attribution rides the item into both renderers
                for (int i = 0; i < items.size(); ++i)
                    if (items[i].submode == Varicode::JS8CallJTTY)
                        items[i].sender = attrib[i];
#endif
            }

            for (int i = 0; i < items.length(); ++i) {
                auto &item = items[i];
                bool shouldDisplay = true;

                // hide aged items. The selected row bypasses aging:
                // the selected offset in offset mode, the selected
                // station's frames in callsign mode.
                bool const agingExempt =
                    listByCall
                        ? ((!selectedCall.isEmpty() &&
                            attrib[i] == selectedCall) ||
                           // [unkrow] selected UNKNOWN row: its
                           // bucket's unattributed frames are exempt
                           (selectedCall.isEmpty() &&
                            key == selectedOffset &&
                            attrib[i].isEmpty()))
                        : isOffsetSelected;
                if (!agingExempt && activityAging &&
                    item.utcTimestamp.secsTo(now) / 60 >= activityAging) {
                    shouldDisplay = false;
                }

                // [audit L10] The heartbeat/ACK filter and its look-backs
                // (items[i-1]) are JS8 frame rules. A JTTY message is a
                // whole message: it neither gets them nor serves as the
                // "preceding frame" of one -- the JTTY items follow the
                // JS8 frames in this list, so the first one looked back
                // into JS8.
                bool const jttyItem =
#ifdef JS8_ENABLE_JTTY
                    item.submode == Varicode::JS8CallJTTY;
#else
                    false;
#endif
                if (!showHB && !jttyItem) {
                    // hide heartbeats; also hide preceding bare "CALL:" prefix
                    if (item.text.contains(" @HB ") ||
                        item.text.contains(" HEARTBEAT ")) {
                        shouldDisplay = false;
                        if (i > 0 && items[i - 1].shouldDisplay &&
                            items[i - 1].text.endsWith(": ")) {
                            items[i - 1].shouldDisplay = false;
                        }
                    }

                    // hide HAIL beacons (bare "CALL:" with no content)
                    {
                        QString stripped = item.text;
                        stripped.remove(eot).trimmed();
                        if (stripped.endsWith(":") &&
                            !stripped.contains(" ")) {
                            shouldDisplay = false;
                        }
                    }

                    // hide ACK messages not directed to me
                    if (item.text.contains(" ACK ") &&
                        !item.text.contains(myCall + " ACK")) {
                        shouldDisplay = false;
                    }

                    // [TODO #151] HB-ACK replies carrying a stored-
                    // message notice leak past the patterns above in
                    // two ways:
                    // (a) the "YES MSG ID n" QUERY MSGS reply form has
                    //     no signed SNR, so yesSnrRe below misses it;
                    static const QRegularExpression yesMsgIdRe(
                        QStringLiteral(
                            R"(\w+:\s+\w+\s+YES\s+MSG\s+ID)"));
                    if (yesMsgIdRe.match(item.text).hasMatch() &&
                        !item.text.contains(myCall + " YES")) {
                        shouldDisplay = false;
                    }
                    // (b) a multi-frame reply's LATER fragment holding
                    //     only the "MSG ID nn" tail matches nothing —
                    //     hide it when its preceding frame in this
                    //     offset group was itself filtered (same
                    //     adjacency mechanics as the bare "CALL:"
                    //     prefix hide above).
                    // [#151 rev2, field 2026-08-15 WB7TSQ] The frame
                    // split can land between "MSG" and "ID 1261" —
                    // accept the bare ID tail too (safe: only applies
                    // when the PRECEDING frame was already filtered).
                    static const QRegularExpression msgIdFragRe(
                        QStringLiteral(R"((MSG\s+)?ID\s+\d+)"));
                    if (i > 0 && !items[i - 1].shouldDisplay &&
                        msgIdFragRe.match(item.text).hasMatch()) {
                        shouldDisplay = false;
                    }

                    // NO messages are intentionally NOT filtered here —
                    // even when "Show band heartbeats and acks" is off,
                    // a "CALL: CALL NO" reply is meaningful traffic
                    // worth seeing. (Reverted prior filter that hid
                    // these alongside HBs/ACKs.)

                    // hide YES SNR replies not directed to me
                    {
                        static const QRegularExpression yesSnrRe(
                            R"(\b\w+:\s+\w+\s+YES\s+[+-]\d{2}\b)");
                        if (yesSnrRe.match(item.text).hasMatch() &&
                            !item.text.contains(myCall + " YES")) {
                            shouldDisplay = false;
                        }
                    }

                    // cascade: hide MSG ID if previous was hidden (or this is first)
                    if ((i == 0 ||
                         (i > 0 && !items[i - 1].shouldDisplay)) &&
                        (item.text.contains(" MSG ID "))) {
                        shouldDisplay = false;
                    }
                }

                // hide empty items
                if (item.text.isEmpty()) {
                    shouldDisplay = false;
                }

                items[i].shouldDisplay = shouldDisplay;
            }

            filtered[key] = items;
            attributed[key] = attrib;
            // [BANDROW] Render-state trace (#156): one compact line
            // per bucket whose COMPOSITION changed since the last
            // render — closes the "header absent at paint" open link
            // by showing exactly what each row was built from.
            {
                QString sig;
                for (auto const &it : items)
                    sig += QStringLiteral("|%1 b%2 %3 '%4'")
                               .arg(it.utcTimestamp.toString("hhmmss"))
                               .arg(it.bits)
                               .arg(it.shouldDisplay ? "v" : "h")
                               .arg(it.text.left(14));
                static QHash<int, QString> lastSig;
                if (lastSig.value(key) != sig) {
                    lastSig[key] = sig;
                    qCWarning(mainwindow_js8)
                        << "[BANDROW] render" << key << sig;
                }
            }

            // last visible = newest item where shouldDisplay == true
            for (int i = items.size() - 1; i >= 0; --i) {
                if (items[i].shouldDisplay) {
                    lastVisible[key] = items[i];
                    break;
                }
            }
        }

        // Field comparators shared by both listing modes.

        auto const detailTimestamp = [](ActivityDetail const &lhs,
                                        ActivityDetail const &rhs) {
            return lhs.utcTimestamp < rhs.utcTimestamp;
        };

        // SNR comparison;  we always want insane SNR values to be at the end
        // of the list and the list is going to be reversed if reverse is set,
        // so we want to set things up so that insane elements are either all
        // at the beginning in the case of a reverse, or all at the end in the
        // standard case. Reverse takes care of itself; we just need to sort
        // out standard.
        auto const detailSNR = [reverse = sort.reverse](
                                   ActivityDetail const &lhs,
                                   ActivityDetail const &rhs) {
            auto lhsSNR = lhs.snr;
            auto rhsSNR = rhs.snr;

            if (!reverse) {
                if (lhsSNR < -60 || lhsSNR > 60)
                    lhsSNR = -lhsSNR;
                if (rhsSNR < -60 || rhsSNR > 60)
                    rhsSNR = -rhsSNR;
            }

            return lhsSNR < rhsSNR;
        };

        // Submode comparison; slow mode isn't at the start of the enumeration;
        // it's in the middle of it. All the other modes are in the expected
        // order.
        auto const detailSubmode = [](ActivityDetail const &lhs,
                                      ActivityDetail const &rhs) {
            auto lhsSubmode = lhs.submode;
            auto rhsSubmode = rhs.submode;

            if (lhsSubmode == Varicode::JS8CallSlow)
                lhsSubmode = -lhsSubmode;
            if (rhsSubmode == Varicode::JS8CallSlow)
                rhsSubmode = -rhsSubmode;

            return lhsSubmode < rhsSubmode;
        };

        // [bandcall] Callsign-mode station buckets: every VISIBLE
        // attributed frame, merged across offsets AND submode classes
        // (operator ruling: no separate Subspace line in this mode),
        // ordered by timestamp. [unkrow 2026-09-07] Unattributed
        // frames are no longer dropped: they collect into one
        // "unknown" row per offset bucket (blank Callsign cell,
        // operator ruling) so a station whose header never decoded
        // is still visible.
        QMap<QString, QList<ActivityDetail>> callItems;
        QMap<int, QList<ActivityDetail>> unknownItems;
        if (listByCall) {
            for (int key : keys) {
                auto const &items = filtered[key];
                auto const &attrib = attributed[key];
                for (int i = 0; i < items.size(); ++i) {
                    if (!items[i].shouldDisplay)
                        continue;
                    if (attrib[i].isEmpty())
                        unknownItems[key].append(items[i]);
                    else
                        callItems[attrib[i]].append(items[i]);
                }
            }
            for (auto &list : callItems)
                std::stable_sort(list.begin(), list.end(), detailTimestamp);
            // [audit L10] an unknown row mixing JS8 frames and JTTY
            // messages at one key is time-ordered too (its Age, SNR and
            // speed come from the newest item)
            for (auto &list : unknownItems)
                std::stable_sort(list.begin(), list.end(), detailTimestamp);
        }

        // Row builder shared by both modes: creates all 7 cells for
        // one table row, applies selection and the 4-tier tinting.
        auto const addRow = [&](QString const &call, int offset,
                                float tdrift, QString const &age,
                                QDateTime const &timestamp, int snr,
                                bool snrSuspect, int submode,
                                QString const &joined,
                                QVariantList const &groupData,
                                QVariantList const &tipData,
                                QString const &lastText, bool selected) {
            ui->tableWidgetRXAll->insertRow(
                ui->tableWidgetRXAll->rowCount());
            int row = ui->tableWidgetRXAll->rowCount() - 1;

            auto callItem = new QTableWidgetItem(call);
            callItem->setData(Qt::UserRole, QVariant(call));
            callItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            ui->tableWidgetRXAll->setItem(row, BACallsign, callItem);

            auto offsetItem = new QTableWidgetItem(
                QString(columnLabel("%1 Hz")).arg(offset));
            offsetItem->setData(Qt::UserRole, QVariant(offset));
            offsetItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            ui->tableWidgetRXAll->setItem(row, BAOffset, offsetItem);

            auto tdriftItem = new QTableWidgetItem(
                QString(columnLabel("%1 ms")).arg((int)(1000 * tdrift)));
            tdriftItem->setData(Qt::UserRole, QVariant(tdrift));
            tdriftItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            ui->tableWidgetRXAll->setItem(row, BATDrift, tdriftItem);

            auto ageItem = new QTableWidgetItem(age);
            ageItem->setTextAlignment(Qt::AlignCenter);
            ageItem->setToolTip(timestamp.toString());
            // [#298] what the once-per-second age pass needs to rewrite
            // this cell in place and to know when a REBUILD is due:
            // the row's timestamp, and whether it was already past the
            // aging limit when built (then it is shown as exempt, and
            // the pass must not ask for a rebuild every second).
            ageItem->setData(Qt::UserRole, timestamp);
            {
                int const aging = m_config.activity_aging();
                bool const agedOut =
                    aging && timestamp.secsTo(
                                 DriftingDateTime::currentDateTimeUtc()) / 60 >= aging;
                ageItem->setData(Qt::UserRole + 2, agedOut);
            }
            ui->tableWidgetRXAll->setItem(row, BAAge, ageItem);

            auto snrText = snrSuspect ? QString() : Varicode::formatSNR(snr);
            auto snrItem = new QTableWidgetItem(
                snrText.isEmpty()
                    ? ""
                    : QString(columnLabel("%1 dB")).arg(snrText));
            snrItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            ui->tableWidgetRXAll->setItem(row, BASnr, snrItem);

            // [JTTY, operator 2026-10-12] a JTTY row shows the whole
            // word in JTTY mode; in JS8 mode JTTY is a speed like any
            // other and gets its initial, "J".
            auto name = JS8::Submode::name(submode);
            auto displayChar =
#ifdef JS8_ENABLE_JTTY
                (submode == Varicode::JS8CallJTTY)
                ? (jttyMode() ? QStringLiteral("JTTY") : QStringLiteral("J"))
                :
#endif
                (submode == Varicode::JS8CallFT2)
                ? QString::fromUtf8("\u26A1")
                : name.left(1).replace("H", "N");
            auto submodeItem = new QTableWidgetItem(displayChar);
            submodeItem->setToolTip(name);
            submodeItem->setData(Qt::UserRole, QVariant(submode));
            submodeItem->setTextAlignment(Qt::AlignCenter);
            ui->tableWidgetRXAll->setItem(row, BASpeed, submodeItem);

            auto textItem = new QTableWidgetItem(joined);
            textItem->setData(Qt::UserRole, groupData);
            // [operator 2026-10-13] the per-message list of a JTTY line
            // (empty otherwise): tooltip and sender bolding read it
            textItem->setData(Qt::UserRole + 1, tipData);
            textItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            ui->tableWidgetRXAll->setItem(row, BAMessage, textItem);

            // Show full line contents as tooltip on Callsign, Offset,
            // Time Delta, and SNR columns (not Age or Speed — they
            // have their own tips). Bold callsign: patterns for
            // readability.
            {
                // [#240] the delegate's ONE authority, not a copy:
                // one line per group, each group's sender bold; one
                // line per complete message for a JTTY line
                auto const fullTip =
                    BandActivityMessageDelegate::rowTooltipHtml(
                        tipData.isEmpty() ? groupData : tipData, joined,
                        /*bareSender=*/!tipData.isEmpty()); // JTTY only
                callItem->setToolTip(fullTip);
                offsetItem->setToolTip(fullTip);
                tdriftItem->setToolTip(fullTip);
                snrItem->setToolTip(fullTip);
            }

            if (selected) {
                for (int i = 0; i < ui->tableWidgetRXAll->columnCount();
                     i++) {
                    ui->tableWidgetRXAll->item(row, i)->setSelected(true);
                }
            }

            // [#258 2026-09-20, operator: "highlighted in red, as if for
            // me"] The "my call" color means ADDRESSED TO ME: a directed
            // message to this station was heard at this offset within
            // the last two minutes (markOffsetDirected). The old second
            // test, isMyCallIncluded(lastText), colored any row that
            // MENTIONED my call anywhere -- a relay body naming me as the
            // next hop ("W5TTA: W0IFM> WM8Q F!701C ..."), a *DE* tail, a
            // free-text mention. Legacy (stock 2.2.0 identical). Meaning
            // is not position: a mention is not an address.
            // [operator 2026-10-19] one rule (rowDirectedToMe), shared with
            // the age pass. A JTTY-mode mark colours only its sender's row,
            // and that red wins over the CQ and highlight-word colours
            // below (the selection still covers it). JS8: as before.
            bool stationMark = false;
            bool const directedTint = rowDirectedToMe(call, offset, &stationMark);
            if (directedTint) {
                // [DIRMARK] once per row and mark (redraws repeat it)
                {
                    static QSet<QString> logged;
                    auto const *m = m_rxDirectedCache[offset / 10 * 10];
                    QString const key = QStringLiteral("%1|%2|%3")
                        .arg(call).arg(offset / 10 * 10)
                        .arg(m ? m->date.toMSecsSinceEpoch() : 0);
                    if (!logged.contains(key)) {
                        logged.insert(key);
                        qWarning() << "[DIRMARK] paint row" << call << "offset"
                                   << offset << "selected=" << selected;
                    }
                }
                for (int i = 0; i < ui->tableWidgetRXAll->columnCount();
                     i++) {
                    ui->tableWidgetRXAll->item(row, i)->setBackground(
                        QBrush(m_config.color_MyCall()));
                }
            }
            // [#298] the age pass asks for a rebuild when this expires
            ageItem->setData(Qt::UserRole + 1, directedTint);

            if (!joined.isEmpty() && !(directedTint && stationMark)) {
                auto const list = joined.split(QRegularExpression("[:> ]"),
                                               Qt::SkipEmptyParts);
                QSet<QString> words(list.begin(), list.end());

                if (words.contains("CQ")) {
                    for (int i = 0; i < ui->tableWidgetRXAll->columnCount();
                         i++) {
                        ui->tableWidgetRXAll->item(row, i)->setBackground(
                            QBrush(m_config.color_CQ()));
                    }
                }

                auto matchingSecondaryWords =
                    m_config.secondary_highlight_words() & words;
                if (!matchingSecondaryWords.isEmpty()) {
                    for (int i = 0; i < ui->tableWidgetRXAll->columnCount();
                         i++) {
                        ui->tableWidgetRXAll->item(row, i)->setBackground(
                            QBrush(m_config.color_secondary_highlight()));
                    }
                }

                auto matchingPrimaryWords =
                    m_config.primary_highlight_words() & words;
                if (!matchingPrimaryWords.isEmpty()) {
                    for (int i = 0; i < ui->tableWidgetRXAll->columnCount();
                         i++) {
                        ui->tableWidgetRXAll->item(row, i)->setBackground(
                            QBrush(m_config.color_primary_highlight()));
                    }
                }
            }
        };

        // [audit M1(c), operator 2026-10-14] The age pass watches every
        // piece of a row, not only its newest: the row just added records
        // its OLDEST visible piece and whether that one was already past
        // the aging limit when built (shown because exempt), so the pass
        // asks for a rebuild only when an old piece crosses the limit.
        auto const noteOldest = [&](QDateTime const &oldest) {
            int const r = ui->tableWidgetRXAll->rowCount() - 1;
            auto *ai = r >= 0 ? ui->tableWidgetRXAll->item(r, BAAge) : nullptr;
            if (!ai || !oldest.isValid()) return;
            int const aging = m_config.activity_aging();
            ai->setData(Qt::UserRole + 3, oldest);
            ai->setData(Qt::UserRole + 4,
                        aging && oldest.secsTo(
                                     DriftingDateTime::currentDateTimeUtc()) / 60 >= aging);
        };

        if (!listByCall) {
            // ------------------- classic offset mode -----------------

            auto const compare = [&lastVisible](int const lhsKey,
                                                int const rhsKey,
                                                auto &&detail) {
                bool lhsHas = lastVisible.contains(lhsKey);
                bool rhsHas = lastVisible.contains(rhsKey);

                if (!lhsHas)
                    return false;
                if (!rhsHas)
                    return true;

                return detail(lastVisible[lhsKey], lastVisible[rhsKey]);
            };

            // Always perform an initial sort by offset.
            std::stable_sort(keys.begin(), keys.end());

            // If something other than offset was requested as the sort
            // by, perform an additional stable sort by the field
            // requested. Note: "call" (the callsign-mode-only sort)
            // matches no branch here, so it falls back to the offset
            // baseline by design.
            if (sort.by == "timestamp")
                std::stable_sort(keys.begin(), keys.end(),
                                 [&](int l, int r) {
                                     return compare(l, r, detailTimestamp);
                                 });
            else if (sort.by == "snr")
                std::stable_sort(keys.begin(), keys.end(),
                                 [&](int l, int r) {
                                     return compare(l, r, detailSNR);
                                 });
            else if (sort.by == "submode")
                std::stable_sort(keys.begin(), keys.end(),
                                 [&](int l, int r) {
                                     return compare(l, r, detailSubmode);
                                 });

            // The sort comparators leave things in forward order. If a
            // reverse sort was requested, reverse the keys.
            if (sort.reverse)
                std::reverse(keys.begin(), keys.end());

            // Build the table
            foreach (int offset, keys) {
                bool isOffsetSelected = (offset == selectedOffset);

                // Build 145: split offset items into Subspace (FT2) vs
                // standard groups so each class gets its own row. Without
                // this split, both classes collapse into one row whose
                // displayed mode reflects whichever submode landed last in
                // the bucket — relabeling a Subspace row as Normal when
                // newer Normal traffic arrived at the same offset. The
                // merge-on-nearby logic in processDecodeEvent already
                // separates classes across adjacent offsets; this closes
                // the gap for exact-offset collisions.
                // [JTTY] a third class, so a JTTY message never merges into
                // a JS8 row at the same offset.
                QList<ActivityDetail> filteredStandard, filteredSubspace,
                    filteredJtty;
                for (auto const & item : filtered[offset]) {
                    if (item.submode == Varicode::JS8CallJTTY)
                        filteredJtty.append(item);
                    else if (item.submode == Varicode::JS8CallFT2)
                        filteredSubspace.append(item);
                    else
                        filteredStandard.append(item);
                }

                for (QList<ActivityDetail> const * itemsPtr :
                     {&filteredStandard, &filteredSubspace, &filteredJtty}) {
                QList<ActivityDetail> const & items = *itemsPtr;
                if (items.length() > 0) {
                    QDateTime timestamp;
                    QDateTime oldest; // [audit M1(c)]
                    QStringList text;
                    QString age;
                    int snr = 0;
                    bool snrSuspect = false;
                    float tdrift = 0;
                    int submode = -1;

                    // shouldDisplay was computed in the pre-sort filter
                    // pass above; render directly from
                    // items[i].shouldDisplay here.

                    // show the items that should appear, grouped by callsign
                    // Each group: {callsign, accumulated text}
                    struct CallGroup { QString call; QString text; };
                    QList<CallGroup> callGroups;
                    // [operator 2026-10-13] the per-message tooltip list
                    // of a JTTY line (UserRole + 1): one entry per
                    // complete message with its attributed sender
                    QVariantList tipData;

                    foreach (ActivityDetail item, items) {
                        if (!item.shouldDisplay) {
                            continue;
                        }

                        if (item.isLowConfidence) {
                            item.text = QString("[%1]").arg(item.text);
                        }

                        if ((item.bits & Varicode::JS8CallLast) ==
                            Varicode::JS8CallLast) {
                            item.text = QString("%1 %2 ")
                                            .arg(Varicode::rstrip(item.text))
                                            .arg(m_config.eot());
                        }

#ifdef JS8_ENABLE_JTTY
                        if (item.submode == Varicode::JS8CallJTTY) {
                            QVariantMap m;
                            m["call"] = item.sender;
                            m["text"] = Varicode::rstrip(item.text);
                            tipData.append(m);
                        }
#endif

                        // Extract "from" callsign via the shared parse
                        QString frameCall = frameFromCall(item.text, item.bits);

                        // Assign frame to callsign group — consolidate by callsign
                        int maxGroups = m_config.message_subdivisions();
                        if (!frameCall.isEmpty()) {
                            // Find existing group for this callsign
                            int existingIdx = -1;
                            for (int g = 0; g < callGroups.size(); g++) {
                                if (callGroups[g].call == frameCall) {
                                    existingIdx = g;
                                    break;
                                }
                            }
                            if (existingIdx >= 0) {
                                // Append to existing group for this callsign
                                callGroups[existingIdx].text += item.text;
                            } else if (callGroups.size() < maxGroups) {
                                // New callsign — create new group
                                callGroups.append({frameCall, item.text});
                            } else {
                                // No room — drop oldest subdivision to make
                                // room for the most recent callsign.
                                callGroups.removeFirst();
                                callGroups.append({frameCall, item.text});
                            }
                        } else {
                            // Anonymous frame — append to current callsign's group
                            // as continuation. If no group exists yet, skip it
                            // (orphan fragment, visible via tooltip on other columns).
                            if (!callGroups.isEmpty()) {
                                callGroups.last().text += item.text;
                            }
                        }

                        text.append(item.text);
                        snr = item.snr;
                        snrSuspect = item.snrSuspect;
                        age = since(item.utcTimestamp);
                        timestamp = item.utcTimestamp;
                        if (!oldest.isValid() || item.utcTimestamp < oldest)
                            oldest = item.utcTimestamp;
                        tdrift = item.tdrift;
                        submode = item.submode;
                    }

                    auto joined = Varicode::rstrip(text.join(""));
                    if (joined.isEmpty()) {
                        continue;
                    }
                    // Chunked-DATA wire markers ("#NN.CC/TT.HHHH") are
                    // intentionally left in the band activity row.
                    // Operator-call 2026-06-04: ham operators are used to
                    // coded protocol traffic on JS8; the marker is
                    // diagnostic in the live-decode stream view. The
                    // assembled-message summary in the conversation panel
                    // (♦ line) is the operator-friendly readout.

                    // Store callsign groups for sub-divided rendering
                    QVariantList groupData;
                    for (const auto &g : callGroups) {
                        QVariantMap m;
                        m["call"] = g.call;
                        m["text"] = Varicode::rstrip(g.text);
                        groupData.append(m);
                    }

                    addRow(QString(), offset, tdrift, age, timestamp,
                           snr, snrSuspect, submode, joined, groupData,
                           tipData, text.last(), isOffsetSelected);
                    noteOldest(oldest);
                }
                } // end split-by-submode for loop (Build 145)
            }
        } else {
            // ------------------- callsign mode -----------------------
            // [bandcall] One row per station; scalars come from its
            // newest visible frame; message text is its frames joined
            // chronologically across offsets and classes.

            // [unkrow] Row sources: known stations (call, -1), then
            // "unknown" offset buckets (blank call, bucket key). The
            // BASELINE order -- calls alphabetical, unknowns after
            // them by ascending offset -- IS the "Callsign" sort:
            // unknowns sink to the end ascending, and the plain
            // reverse puts them on top descending (operator ruling).
            QList<QPair<QString, int>> rows;
            for (auto const &c : callItems.keys())
                rows.append(qMakePair(c, -1));
            for (int k : unknownItems.keys())
                rows.append(qMakePair(QString(), k));

            auto const rowItems =
                [&](QPair<QString, int> const &r)
                -> QList<ActivityDetail> const & {
                return r.second < 0 ? callItems[r.first]
                                    : unknownItems[r.second];
            };
            auto const compareRow = [&](QPair<QString, int> const &l,
                                        QPair<QString, int> const &r,
                                        auto &&detail) {
                return detail(rowItems(l).last(), rowItems(r).last());
            };

            // Other sorts compare each row's newest visible frame;
            // "call" (and unknown values) keep the baseline.
            if (sort.by == "offset")
                std::stable_sort(rows.begin(), rows.end(),
                                 [&](auto const &l, auto const &r) {
                                     return compareRow(
                                         l, r,
                                         [](ActivityDetail const &a,
                                            ActivityDetail const &b) {
                                             return a.offset < b.offset;
                                         });
                                 });
            else if (sort.by == "timestamp")
                std::stable_sort(rows.begin(), rows.end(),
                                 [&](auto const &l, auto const &r) {
                                     return compareRow(l, r,
                                                       detailTimestamp);
                                 });
            else if (sort.by == "snr")
                std::stable_sort(rows.begin(), rows.end(),
                                 [&](auto const &l, auto const &r) {
                                     return compareRow(l, r, detailSNR);
                                 });
            else if (sort.by == "submode")
                std::stable_sort(rows.begin(), rows.end(),
                                 [&](auto const &l, auto const &r) {
                                     return compareRow(l, r,
                                                       detailSubmode);
                                 });

            if (sort.reverse)
                std::reverse(rows.begin(), rows.end());

            for (auto const &rowSrc : rows) {
                QString const call = rowSrc.first; // empty = unknown
                QDateTime timestamp;
                QDateTime oldest; // [audit M1(c)]
                QStringList text;
                QString age;
                int snr = 0;
                bool snrSuspect = false;
                float tdrift = 0;
                int submode = -1;
                int offset = -1;

                // [operator 2026-10-13] The per-message tooltip list of
                // a row holding JTTY messages: one entry per complete
                // JTTY message with its attributed sender (the row's
                // station for a known row); the row's JS8 frames, if
                // any, stay one entry as today. Empty for a JS8-only
                // row, which keeps its single-group tooltip.
                struct Tip { QString call; QString text; bool jtty; };
                QList<Tip> tips;
                bool anyJtty = false;

                foreach (ActivityDetail item, rowItems(rowSrc)) {
                    if (item.isLowConfidence) {
                        item.text = QString("[%1]").arg(item.text);
                    }

                    if ((item.bits & Varicode::JS8CallLast) ==
                        Varicode::JS8CallLast) {
                        item.text = QString("%1 %2 ")
                                        .arg(Varicode::rstrip(item.text))
                                        .arg(m_config.eot());
                    }

#ifdef JS8_ENABLE_JTTY
                    if (item.submode == Varicode::JS8CallJTTY) {
                        tips.append({item.sender.isEmpty() ? call : item.sender,
                                     item.text, true});
                        anyJtty = true;
                    } else
#endif
                    if (tips.isEmpty() || tips.last().jtty)
                        tips.append({call, item.text, false});
                    else
                        tips.last().text += item.text;

                    text.append(item.text);
                    snr = item.snr;
                    snrSuspect = item.snrSuspect;
                    age = since(item.utcTimestamp);
                    timestamp = item.utcTimestamp;
                    if (!oldest.isValid() || item.utcTimestamp < oldest)
                        oldest = item.utcTimestamp;
                    tdrift = item.tdrift;
                    submode = item.submode;
                    offset = item.offset;
                }

                // [unkrow] An unknown row's identity is its offset
                // BUCKET key (stable across frame-level drift).
                if (rowSrc.second >= 0)
                    offset = rowSrc.second;

                auto joined = Varicode::rstrip(text.join(""));
                if (joined.isEmpty()) {
                    continue;
                }

                // Single group: the delegate paints one region with
                // the station's call bolded, exactly like a one-call
                // offset row.
                QVariantList groupData;
                {
                    QVariantMap m;
                    m["call"] = call;
                    m["text"] = joined;
                    groupData.append(m);
                }
                QVariantList tipData;
                if (anyJtty) {
                    for (auto const &t : tips) {
                        QVariantMap m;
                        m["call"] = t.call;
                        m["text"] = Varicode::rstrip(t.text);
                        tipData.append(m);
                    }
                }

                // Selection identity: callsign for station rows; the
                // offset bucket for unknown rows (only when no
                // station is selected).
                bool const selected =
                    rowSrc.second < 0
                        ? (!selectedCall.isEmpty() &&
                           call == selectedCall)
                        : (selectedCall.isEmpty() &&
                           selectedOffset == rowSrc.second);

                addRow(call, offset, tdrift, age, timestamp, snr,
                       snrSuspect, submode, joined, groupData, tipData,
                       text.last(), selected);
                noteOldest(oldest);
            }
        }

        // Set table color
        auto style = QString(
            "QTableWidget { background:%1; selection-background-color:%2; "
            "alternate-background-color:%1; color:%3; } "
            "QTableWidget::item:selected { background-color: %2; color: %3; }");
        style = style.arg(m_config.color_table_background().name());
        style = style.arg(m_config.color_table_highlight().name());
        style = style.arg(m_config.color_table_foreground().name());
        ui->tableWidgetRXAll->setStyleSheet(style);

        // Set the table palette for inactive selected row
        auto p = ui->tableWidgetRXAll->palette();

        p.setColor(QPalette::Highlight, m_config.color_table_highlight());
        p.setColor(QPalette::HighlightedText,
                   m_config.color_table_foreground());
        p.setColor(QPalette::Inactive, QPalette::Highlight,
                   p.color(QPalette::Active, QPalette::Highlight));
        ui->tableWidgetRXAll->setPalette(p);

        // Set item fonts
        for (int row = 0; row < ui->tableWidgetRXAll->rowCount(); row++) {
            for (int col = 0; col < ui->tableWidgetRXAll->columnCount();
                 col++) {
                auto item = ui->tableWidgetRXAll->item(row, col);
                if (item) {
                    item->setFont(m_config.table_font());
                }
            }
        }

        // Column labels
        ui->tableWidgetRXAll->horizontalHeader()->setVisible(
            showColumn("band", "labels"));

        // Hide columns. The Callsign column exists only in callsign
        // mode, where it is hideable like the others ([bandcall3]).
        ui->tableWidgetRXAll->setColumnHidden(
            BACallsign, !listByCall || !showColumn("band", "callsign"));
        ui->tableWidgetRXAll->setColumnHidden(
            BAOffset, !showColumn("band", "offset"));
        ui->tableWidgetRXAll->setColumnHidden(
            BATDrift, !showColumn("band", "tdrift"));
        ui->tableWidgetRXAll->setColumnHidden(
            BAAge, !showColumn("band", "timestamp"));
        ui->tableWidgetRXAll->setColumnHidden(
            BASnr, !showColumn("band", "snr"));
        ui->tableWidgetRXAll->setColumnHidden(
            BASpeed, !showColumn("band", "submode", false));

        // Resize the table columns
        ui->tableWidgetRXAll->resizeColumnToContents(BACallsign);
        ui->tableWidgetRXAll->resizeColumnToContents(BAOffset);
        ui->tableWidgetRXAll->resizeColumnToContents(BATDrift);
        ui->tableWidgetRXAll->resizeColumnToContents(BAAge);
        ui->tableWidgetRXAll->resizeColumnToContents(BASnr);
        ui->tableWidgetRXAll->resizeColumnToContents(BASpeed);

        // Reset the scroll position
        ui->tableWidgetRXAll->verticalScrollBar()->setValue(currentScrollPos);
    }
    ui->tableWidgetRXAll->setUpdatesEnabled(true);
    // [#298] what the table now shows; displayActivity rebuilds only
    // when the content has moved away from it
    m_bandActivitySignature = bandActivitySignature();
}
