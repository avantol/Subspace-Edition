

/** \file
 * @brief member function of the UI_Constructor class
 *  displays the current band activity in the left pane of the UI
 */

#include "JS8_UI/mainwindow.h"

#include "JS8_Main/ChunkedArq.h"

// [bandcall] Shared "CALL: " prefix parse -- the ONE authority for
// attributing a frame to its sender in the band activity view (was
// inline in the render loop). Format is always "CALLSIGN: @GROUP/msg".
QString UI_Constructor::frameFromCall(QString const &text) {
    int colonPos = text.indexOf(": ");
    if (colonPos > 0 && colonPos <= 15) {
        QString candidate = text.left(colonPos).trimmed();
        // Valid callsign: 3-15 chars, has letter and digit
        if (candidate.length() >= 3 && candidate.length() <= 15
            && candidate.contains(QRegularExpression("[A-Z]"))
            && candidate.contains(QRegularExpression("[0-9]"))) {
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
    static constexpr qint64 kMaxAgeSecs = 15 * 60;
    QDateTime const now = DriftingDateTime::currentDateTimeUtc();
    QString best;
    int bestDist = threshold + 1;
    for (auto it = m_callActivity.constBegin();
         it != m_callActivity.constEnd(); ++it) {
        if (it.value().utcTimestamp.secsTo(now) > kMaxAgeSecs)
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
        auto keys = m_bandActivity.keys();

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
            QList<ActivityDetail> items = m_bandActivity[key];

            // [bandcall] Attribute every frame in bucket order: a frame
            // with a "CALL: " prefix belongs to that call; a
            // continuation frame belongs to the last attributed call in
            // this bucket (same rule the per-offset subdivision has
            // always used). [bandcall2] A frame arriving before any
            // call is known in its bucket goes to the station last
            // seen nearest its offset (operator proposal); only when
            // that finds nothing is it left out of callsign mode.
            QStringList attrib;
            if (listByCall) {
                QString lastCall;
                for (auto const &it : items) {
                    QString const c = frameFromCall(it.text);
                    if (!c.isEmpty())
                        lastCall = c;
                    attrib.append(
                        lastCall.isEmpty()
                            ? mostLikelyCallAtOffset(it.offset, it.submode)
                            : lastCall);
                }
            } else {
                for (int i = 0; i < items.size(); ++i)
                    attrib.append(QString());
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

                if (!showHB) {
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
        }

        // Row builder shared by both modes: creates all 7 cells for
        // one table row, applies selection and the 4-tier tinting.
        auto const addRow = [&](QString const &call, int offset,
                                float tdrift, QString const &age,
                                QDateTime const &timestamp, int snr,
                                bool snrSuspect, int submode,
                                QString const &joined,
                                QVariantList const &groupData,
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
            ui->tableWidgetRXAll->setItem(row, BAAge, ageItem);

            auto snrText = snrSuspect ? QString() : Varicode::formatSNR(snr);
            auto snrItem = new QTableWidgetItem(
                snrText.isEmpty()
                    ? ""
                    : QString(columnLabel("%1 dB")).arg(snrText));
            snrItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            ui->tableWidgetRXAll->setItem(row, BASnr, snrItem);

            auto name = JS8::Submode::name(submode);
            auto displayChar = (submode == Varicode::JS8CallFT2)
                ? QString::fromUtf8("\u26A1")
                : name.left(1).replace("H", "N");
            auto submodeItem = new QTableWidgetItem(displayChar);
            submodeItem->setToolTip(name);
            submodeItem->setData(Qt::UserRole, QVariant(submode));
            submodeItem->setTextAlignment(Qt::AlignCenter);
            ui->tableWidgetRXAll->setItem(row, BASpeed, submodeItem);

            auto textItem = new QTableWidgetItem(joined);
            textItem->setData(Qt::UserRole, groupData);
            textItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            ui->tableWidgetRXAll->setItem(row, BAMessage, textItem);

            // Show full line contents as tooltip on Callsign, Offset,
            // Time Delta, and SNR columns (not Age or Speed — they
            // have their own tips). Bold callsign: patterns for
            // readability.
            {
                auto fullTip = joined.toHtmlEscaped();
                static const QRegularExpression callRe(R"((\b(?=[A-Z0-9/]*[0-9])[A-Z0-9/]{3,15}:)(?=\s))");
                fullTip.replace(callRe, "<br/><b>\\1</b>");
                if (fullTip.startsWith("<br/>"))
                    fullTip = fullTip.mid(5);
                fullTip = QString("<div style='white-space:nowrap;'>%1</div>").arg(fullTip);
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

            bool isDirectedAllCall = false;
            if ((isDirectedOffset(offset, &isDirectedAllCall) &&
                 !isDirectedAllCall) ||
                isMyCallIncluded(lastText)) {
                for (int i = 0; i < ui->tableWidgetRXAll->columnCount();
                     i++) {
                    ui->tableWidgetRXAll->item(row, i)->setBackground(
                        QBrush(m_config.color_MyCall()));
                }
            }

            if (!joined.isEmpty()) {
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
                QList<ActivityDetail> filteredStandard, filteredSubspace;
                for (auto const & item : filtered[offset]) {
                    if (item.submode == Varicode::JS8CallFT2)
                        filteredSubspace.append(item);
                    else
                        filteredStandard.append(item);
                }

                for (QList<ActivityDetail> const * itemsPtr :
                     {&filteredStandard, &filteredSubspace}) {
                QList<ActivityDetail> const & items = *itemsPtr;
                if (items.length() > 0) {
                    QDateTime timestamp;
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

                        // Extract "from" callsign via the shared parse
                        QString frameCall = frameFromCall(item.text);

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
                           text.last(), isOffsetSelected);
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
                QStringList text;
                QString age;
                int snr = 0;
                bool snrSuspect = false;
                float tdrift = 0;
                int submode = -1;
                int offset = -1;

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

                    text.append(item.text);
                    snr = item.snr;
                    snrSuspect = item.snrSuspect;
                    age = since(item.utcTimestamp);
                    timestamp = item.utcTimestamp;
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
                       snrSuspect, submode, joined, groupData,
                       text.last(), selected);
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
}
