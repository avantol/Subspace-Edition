

/** \file
 * @brief member function of the UI_Constructor class
 *  displays the callsign activity in the right pane of the UI
 */

#include "JS8_UI/mainwindow.h"

#include <QFont>

namespace {
// The icon column renders Misc-Symbols glyphs (☎ U+260E, ⚑ U+2691,
// ★ U+2605, ⚟ U+269F). System UI fonts on Linux (Liberation Sans,
// Cantarell) lack this block and the glyphs render as tofu boxes;
// Windows' Segoe UI is also patchy. Force a font with guaranteed
// coverage per platform so the icons render reliably. macOS is left
// to the default — its system font fallback already finds the glyph.
// Size is bumped one point larger than the table font for visual
// emphasis on these status indicators.
QFont iconFont(QFont const &base) {
    QFont f = base;
    int const basePt = base.pointSize();
    if (basePt > 0) {
        f.setPointSize(basePt + 1);
    }
#if defined(Q_OS_LINUX)
    f.setFamily("DejaVu Sans");
#elif defined(Q_OS_WIN)
    f.setFamily("Segoe UI Symbol");
#endif
    return f;
}
} // namespace

void UI_Constructor::displayCallActivity() {

    auto now = DriftingDateTime::currentDateTimeUtc();

    // Reset the header label text to accommodate minimal label setting
    int cols = ui->tableWidgetCalls->columnCount();
    for (int c = 0; c < cols; ++c) {
        ui->tableWidgetCalls->horizontalHeaderItem(c)->setText(
            columnLabel(m_origCallActivityHeaderLabelMap[c]));
    }

    ui->tableWidgetCalls->setFont(m_config.table_font());

    // Selected callsign
    QString selectedCall = callsignSelected();

    auto currentScrollPos = ui->tableWidgetCalls->verticalScrollBar()->value();

    ui->tableWidgetCalls->setUpdatesEnabled(false);
    {
        // Clear the table
        ui->tableWidgetCalls->setRowCount(0);
        ui->tableWidgetCalls->horizontalHeaderItem(8)->setText(
            m_config.miles() ? "mi" : "km");

        bool showIconColumn = false;
        // [JTTY step 3, operator 2026-10-12] In JTTY mode the list is
        // the JTTY stations' own (shownCallActivity) and shows no
        // group rows.
        if (!jttyMode())
            createGroupCallsignTableRows(
                ui->tableWidgetCalls, selectedCall,
                showIconColumn); // isAllCallIncluded(selectedCall)); // ||
        // isGroupCallIncluded(selectedCall));
        auto const &activity = shownCallActivity();

        // Build the table

        auto const sort = getSortByReverse("callActivity", "callsign");
        auto keys = activity.keys();

        auto const compareOffset = [this, &activity](QString const &lhsKey,
                                          QString const &rhsKey) {
            return activity[lhsKey].offset <
                   activity[rhsKey].offset;
        };

        auto const compareAzimuth =
            [this, &activity, reverse = sort.reverse, my_grid = m_config.my_grid()](
                QString const &lhsKey, QString const &rhsKey) {
                auto const lhs =
                    Geodesic::vector(my_grid, activity[lhsKey].grid)
                        .azimuth();
                auto const rhs =
                    Geodesic::vector(my_grid, activity[rhsKey].grid)
                        .azimuth();

                // We always want invalid azimuths to be at the end of the list,
                // and the list is going to be reversed if reverse is set, so we
                // want to set things up so that invalid elements are either all
                // at the beginning in the case of a reverse, or all at the end
                // in the standard case.

                if (!lhs)
                    return reverse && rhs;
                else if (!rhs)
                    return !reverse;
                else
                    return lhs < rhs;
            };

        auto const compareDistance =
            [this, &activity, reverse = sort.reverse, my_grid = m_config.my_grid()](
                QString const &lhsKey, QString const &rhsKey) {
                auto const lhs =
                    Geodesic::vector(my_grid, activity[lhsKey].grid)
                        .distance();
                auto const rhs =
                    Geodesic::vector(my_grid, activity[rhsKey].grid)
                        .distance();

                // We always want invalid distances to be at the end of the
                // list, and the list is going to be reversed if reverse is set,
                // so we want to set things up so that invalid elements are
                // either all at the beginning in the case of a reverse, or all
                // at the end in the standard case.

                if (!lhs)
                    return reverse && rhs;
                else if (!rhs)
                    return !reverse;
                else
                    return lhs < rhs;
            };

        auto const compareTimestamp = [this, &activity](QString const &lhsKey,
                                             QString const &rhsKey) {
            return activity[lhsKey].utcTimestamp <
                   activity[rhsKey].utcTimestamp;
        };

        auto const compareAckTimestamp = [this, &activity](QString const &lhsKey,
                                                QString const &rhsKey) {
            return activity[rhsKey].ackTimestamp <
                   activity[lhsKey].ackTimestamp;
        };

        auto const compareSNR =
            [this, &activity, reverse = sort.reverse](QString const &lhsKey,
                                                      QString const &rhsKey) {
                auto lhs = activity[lhsKey].snr;
                auto rhs = activity[rhsKey].snr;

                // We always want insane SNR values to be at the end of the
                // list, and the list is going to be reversed if reverse is set,
                // so we want to set things up so that insane elements are
                // either all at the beginning in the case of a reverse, or all
                // at the end in the standard case. Reverse takes care of
                // itself; we just need to sort out standard.

                if (!reverse) {
                    if (lhs < -60 || lhs > 60)
                        lhs = -lhs;
                    if (rhs < -60 || rhs > 60)
                        rhs = -rhs;
                }

                return lhs < rhs;
            };

        auto const compareSubmode = [this, &activity](QString const &lhsKey,
                                           QString const &rhsKey) {
            auto lhs = activity[lhsKey].submode;
            auto rhs = activity[rhsKey].submode;

            // Slow mode isn't at the start of the enumeration; it's in the
            // middle of it. All the other modes are in the expected order.

            if (lhs == Varicode::JS8CallSlow)
                lhs = -lhs;
            if (rhs == Varicode::JS8CallSlow)
                rhs = -rhs;

            return lhs < rhs;
        };

        // Always perform an initial sort by callsign.

        std::stable_sort(keys.begin(), keys.end());

        // If something other than callsign was requested as the sort by,
        // perform an additional stable sort by the field requested.

        if (sort.by == "offset")
            std::stable_sort(keys.begin(), keys.end(), compareOffset);
        else if (sort.by == "distance")
            std::stable_sort(keys.begin(), keys.end(), compareDistance);
        else if (sort.by == "azimuth")
            std::stable_sort(keys.begin(), keys.end(), compareAzimuth);
        else if (sort.by == "timestamp")
            std::stable_sort(keys.begin(), keys.end(), compareTimestamp);
        else if (sort.by == "ackTimestamp")
            std::stable_sort(keys.begin(), keys.end(), compareAckTimestamp);
        else if (sort.by == "snr")
            std::stable_sort(keys.begin(), keys.end(), compareSNR);
        else if (sort.by == "submode")
            std::stable_sort(keys.begin(), keys.end(), compareSubmode);

        // The sort comparators leave things in forward order. If a reverse sort
        // was requested, reverse the keys.

        if (sort.reverse)
            std::reverse(keys.begin(), keys.end());

        // pin messages to the top
        std::stable_sort(keys.begin(), keys.end(),
                         [this, &activity](QString const &lhsKey, QString const &rhsKey) {
                             auto const lhs = (int)!(
                                 m_rxInboxCountCache.value(lhsKey, 0) > 0);
                             auto const rhs = (int)!(
                                 m_rxInboxCountCache.value(rhsKey, 0) > 0);

                             return lhs < rhs;
                         });

        int callsignAging = m_config.callsign_aging();
        foreach (QString call, keys) {
            if (call.trimmed().isEmpty()) {
                continue;
            }

            CallDetail d = activity[call];
            if (d.call.trimmed().isEmpty()) {
                continue;
            }

            bool isCallSelected = (call == selectedCall);

            // icon flags (flag -> star -> empty)
            bool hasMessage = m_rxInboxCountCache.value(d.call, 0) > 0;

            // display telephone icon if called cq in the past 5 minutes
            bool hasCQ =
                d.cqTimestamp.isValid() && d.cqTimestamp.secsTo(now) / 60 < 5;

            // display star if they've acked a message from us
            bool hasACK = d.ackTimestamp.isValid();

            if (!isCallSelected && !hasMessage && callsignAging &&
                d.utcTimestamp.secsTo(now) / 60 >= callsignAging) {
                continue;
            }

            ui->tableWidgetCalls->insertRow(ui->tableWidgetCalls->rowCount());
            int row = ui->tableWidgetCalls->rowCount() - 1;
            int col = 0;

#if SHOW_THROUGH_CALLS
            QString displayCall =
                d.through.isEmpty()
                    ? d.call
                    : QString("%1>%2").arg(d.through).arg(d.call);
#else
            QString displayCall = d.call;
#endif
            bool hasThrough = !d.through.isEmpty();

            auto iconItem = new QTableWidgetItem(hasMessage   ? "\u2691"
                                                 : hasACK     ? "\u2605"
                                                 : hasCQ      ? "\u260E"
                                                 : hasThrough ? "\u269F"
                                                              : "");
            iconItem->setFont(iconFont(ui->tableWidgetCalls->font()));
            iconItem->setData(Qt::UserRole, QVariant(d.call));
            iconItem->setToolTip(
                hasMessage ? "Message Available"
                : hasACK   ? QString("Hearing Your Station (%1)")
                               .arg(since(d.ackTimestamp))
                : hasCQ ? QString("Calling CQ (%1)").arg(since(d.cqTimestamp))
                : hasThrough
                    ? QString("Heard Through Relay (%1)").arg(d.through)
                    : "");
            iconItem->setTextAlignment(Qt::AlignCenter);
            ui->tableWidgetCalls->setItem(row, col++, iconItem);
            if (hasMessage || hasACK || hasCQ || hasThrough) {
                showIconColumn = true;
            }

            auto displayItem = new QTableWidgetItem(displayCall);
            displayItem->setData(Qt::UserRole, QVariant(d.call));
            displayItem->setToolTip(generateCallDetail(displayCall));
            ui->tableWidgetCalls->setItem(row, col++, displayItem);

#if ONLY_SHOW_HEARD_CALLSIGNS
            if (d.utcTimestamp.isValid()) {
#else
            if (true) {
#endif
                auto tdriftItem = new QTableWidgetItem(
                    QString(columnLabel("%1 ms")).arg((int)(1000 * d.tdrift)));
                tdriftItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                ui->tableWidgetCalls->setItem(row, col++, tdriftItem);

                auto ageItem = new QTableWidgetItem(since(d.utcTimestamp));
                ageItem->setTextAlignment(Qt::AlignCenter);
                ageItem->setToolTip(d.utcTimestamp.toString());
                ui->tableWidgetCalls->setItem(row, col++, ageItem);

                auto snrText = Varicode::formatSNR(d.snr);
                auto snrItem = new QTableWidgetItem(
                    snrText.isEmpty()
                        ? ""
                        : QString(columnLabel("%1 dB")).arg(snrText));
                snrItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                ui->tableWidgetCalls->setItem(row, col++, snrItem);

                auto offsetItem = new QTableWidgetItem(
                    QString(columnLabel("%1 Hz")).arg(d.offset));
                offsetItem->setData(Qt::UserRole, QVariant(d.offset));
                offsetItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                ui->tableWidgetCalls->setItem(row, col++, offsetItem);

                auto name = JS8::Submode::name(d.submode);
                auto displayChar = (d.submode == Varicode::JS8CallFT2)
                    ? QString::fromUtf8("\u26A1")
                    : name.left(1).replace("H", "N");
                auto modeItem = new QTableWidgetItem(displayChar);
                modeItem->setToolTip(name);
                modeItem->setData(Qt::UserRole, QVariant(name));
                modeItem->setTextAlignment(Qt::AlignCenter);
                ui->tableWidgetCalls->setItem(row, col++, modeItem);

                auto gridItem = new QTableWidgetItem(
                    QString("%1").arg(d.grid.trimmed().left(4)));
                gridItem->setToolTip(d.grid.trimmed());
                ui->tableWidgetCalls->setItem(row, col++, gridItem);

                auto const vector =
                    Geodesic::vector(m_config.my_grid(), d.grid);
                auto const units = !showColumn("call", "labels");

                auto distanceItem = new QTableWidgetItem(
                    vector.distance().toString(m_config.miles(), units));
                distanceItem->setTextAlignment(Qt::AlignRight |
                                               Qt::AlignVCenter);
                ui->tableWidgetCalls->setItem(row, col++, distanceItem);

                auto azimuthItem =
                    new QTableWidgetItem(vector.azimuth().toString(units));
                if (auto const azimuth = vector.azimuth())
                    azimuthItem->setToolTip(azimuth.compass().toString());
                azimuthItem->setTextAlignment(Qt::AlignRight |
                                              Qt::AlignVCenter);
                ui->tableWidgetCalls->setItem(row, col++, azimuthItem);

                QString flag;
                if (m_logBook.hasWorkedBefore(d.call, "")) {
                    // unicode checkmark
                    flag = "\u2713";
                }
                auto workedBeforeItem = new QTableWidgetItem(flag);
                workedBeforeItem->setTextAlignment(Qt::AlignCenter);
                ui->tableWidgetCalls->setItem(row, col++, workedBeforeItem);

                QString logDetailGrid;
                QString logDetailDate;
                QString logDetailName;
                QString logDetailComment;
                bool gridItemEmpty = gridItem->text().isEmpty();

                if ((gridItemEmpty && showColumn("call", "grid")) ||
                    showColumn("call", "log") ||
                    showColumn("call", "logName") ||
                    showColumn("call", "logComment")) {
                    m_logBook.findCallDetails(d.call, logDetailGrid,
                                              logDetailDate, logDetailName,
                                              logDetailComment);
                }

                if (gridItemEmpty && !logDetailGrid.isEmpty()) {
                    gridItem->setText(logDetailGrid.trimmed().left(4));
                    gridItem->setToolTip(logDetailGrid.trimmed());

                    auto const vector =
                        Geodesic::vector(m_config.my_grid(), d.grid);
                    auto const units = !showColumn("call", "labels");

                    distanceItem->setText(
                        vector.distance().toString(m_config.miles(), units));
                    azimuthItem->setText(vector.azimuth().toString(units));
                    if (auto const azimuth = vector.azimuth())
                        azimuthItem->setToolTip(azimuth.compass().toString());

                    // update the call activity cache with the loaded grid
                    if (!jttyMode() && m_callActivity.contains(d.call)) {
                        m_callActivity[call].grid = logDetailGrid.trimmed();
                    }
                }

                if (!logDetailDate.isEmpty()) {
                    auto lastLogged =
                        QDate::fromString(logDetailDate, "yyyyMMdd");

                    workedBeforeItem->setToolTip(
                        QString("Last Logged: %1").arg(lastLogged.toString()));
                }

                auto logNameItem = new QTableWidgetItem(logDetailName);
                logNameItem->setTextAlignment(Qt::AlignCenter);
                logNameItem->setToolTip(logDetailName);
                ui->tableWidgetCalls->setItem(row, col++, logNameItem);

                auto logCommentItem = new QTableWidgetItem(logDetailComment);
                logCommentItem->setTextAlignment(Qt::AlignCenter);
                logCommentItem->setToolTip(logDetailComment);
                ui->tableWidgetCalls->setItem(row, col++, logCommentItem);

            } else {
                ui->tableWidgetCalls->setItem(
                    row, col++, new QTableWidgetItem("")); // tdrift
                ui->tableWidgetCalls->setItem(row, col++,
                                              new QTableWidgetItem("")); // age
                ui->tableWidgetCalls->setItem(row, col++,
                                              new QTableWidgetItem("")); // snr
                ui->tableWidgetCalls->setItem(row, col++,
                                              new QTableWidgetItem("")); // freq
                ui->tableWidgetCalls->setItem(row, col++,
                                              new QTableWidgetItem("")); // mode
                ui->tableWidgetCalls->setItem(row, col++,
                                              new QTableWidgetItem("")); // grid
                ui->tableWidgetCalls->setItem(
                    row, col++, new QTableWidgetItem("")); // distance
                ui->tableWidgetCalls->setItem(
                    row, col++, new QTableWidgetItem("")); // azimuth
                ui->tableWidgetCalls->setItem(
                    row, col++, new QTableWidgetItem("")); // worked before
                ui->tableWidgetCalls->setItem(
                    row, col++, new QTableWidgetItem("")); // log name
                ui->tableWidgetCalls->setItem(
                    row, col++, new QTableWidgetItem("")); // log comment
            }

            if (isCallSelected) {
                for (int i = 0; i < ui->tableWidgetCalls->columnCount(); i++) {
                    ui->tableWidgetCalls->item(row, i)->setSelected(true);
                }
            }

            if (hasCQ) {
                for (int i = 0; i < ui->tableWidgetCalls->columnCount(); i++) {
                    ui->tableWidgetCalls->item(row, i)->setBackground(
                        QBrush(m_config.color_CQ()));
                }
            }

            if (m_config.secondary_highlight_words().contains(call)) {
                for (int i = 0; i < ui->tableWidgetCalls->columnCount(); i++) {
                    ui->tableWidgetCalls->item(row, i)->setBackground(
                        QBrush(m_config.color_secondary_highlight()));
                }
            }

            if (m_config.primary_highlight_words().contains(call)) {
                for (int i = 0; i < ui->tableWidgetCalls->columnCount(); i++) {
                    ui->tableWidgetCalls->item(row, i)->setBackground(
                        QBrush(m_config.color_primary_highlight()));
                }
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
        ui->tableWidgetCalls->setStyleSheet(style);

        // Set the table palette for inactive selected row
        auto p = ui->tableWidgetCalls->palette();
        p.setColor(QPalette::Highlight, m_config.color_table_highlight());
        p.setColor(QPalette::HighlightedText,
                   m_config.color_table_foreground());
        p.setColor(QPalette::Inactive, QPalette::Highlight,
                   p.color(QPalette::Active, QPalette::Highlight));
        ui->tableWidgetCalls->setPalette(p);

        // Set item fonts
        for (int row = 0; row < ui->tableWidgetCalls->rowCount(); row++) {
            auto bold = ui->tableWidgetCalls->item(row, 0)->text() == "\u2691";
            for (int col = 0; col < ui->tableWidgetCalls->columnCount();
                 col++) {
                auto item = ui->tableWidgetCalls->item(row, col);
                if (item) {
                    auto f = m_config.table_font();
                    if (bold) {
                        f.setBold(true);
                    }
                    item->setFont(f);
                }
            }
        }

        // Column labels
        ui->tableWidgetCalls->horizontalHeader()->setVisible(
            showColumn("call", "labels"));

        // Hide columns
        ui->tableWidgetCalls->setColumnHidden(0, !showIconColumn);
        ui->tableWidgetCalls->setColumnHidden(1,
                                              !showColumn("call", "callsign"));
        ui->tableWidgetCalls->setColumnHidden(
            2, !showColumn("call", "tdrift") || jttyMode()); // [JTTY] no drift
        ui->tableWidgetCalls->setColumnHidden(3,
                                              !showColumn("call", "timestamp"));
        ui->tableWidgetCalls->setColumnHidden(4, !showColumn("call", "snr"));
        ui->tableWidgetCalls->setColumnHidden(5, !showColumn("call", "offset"));
        ui->tableWidgetCalls->setColumnHidden(
            6, !showColumn("call", "submode", false));
        ui->tableWidgetCalls->setColumnHidden(
            7, !showColumn("call", "grid", false));
        ui->tableWidgetCalls->setColumnHidden(
            8, !showColumn("call", "distance", false));
        ui->tableWidgetCalls->setColumnHidden(
            9, !showColumn("call", "azimuth", false));
        ui->tableWidgetCalls->setColumnHidden(10, !showColumn("call", "log"));
        ui->tableWidgetCalls->setColumnHidden(11,
                                              !showColumn("call", "logName"));
        ui->tableWidgetCalls->setColumnHidden(
            12, !showColumn("call", "logComment"));

        // Resize the table columns
        ui->tableWidgetCalls->resizeColumnToContents(0);
        ui->tableWidgetCalls->resizeColumnToContents(1);
        ui->tableWidgetCalls->resizeColumnToContents(2);
        ui->tableWidgetCalls->resizeColumnToContents(3);
        ui->tableWidgetCalls->resizeColumnToContents(4);
        ui->tableWidgetCalls->resizeColumnToContents(5);
        ui->tableWidgetCalls->resizeColumnToContents(6);
        ui->tableWidgetCalls->resizeColumnToContents(7);
        ui->tableWidgetCalls->resizeColumnToContents(8);
        ui->tableWidgetCalls->resizeColumnToContents(9);
        ui->tableWidgetCalls->resizeColumnToContents(10);
        ui->tableWidgetCalls->resizeColumnToContents(11);

        // Reset the scroll position
        ui->tableWidgetCalls->verticalScrollBar()->setValue(currentScrollPos);
    }
    ui->tableWidgetCalls->setUpdatesEnabled(true);

    // The full rebuild now reflects the current model version. Record
    // it so the once-per-second tick can skip the rebuild on subsequent
    // ticks until logCallActivity bumps the version again.
    m_callActivityRenderedVersion = m_callActivityVersion;
}

// Cheap per-second Age-only refresh. Walks existing rows and rewrites
// the Age cell from the live m_callActivity timestamp. No sort, no
// allocations, no table rebuild, no aging filter re-check.
//
// Column indices match the full-rebuild layout in displayCallActivity
// above: [0]=icon, [1]=displayCall (UserRole stores the callsign key),
// [2]=tdrift, [3]=age, [4]=snr, ...
//
// Called from guiUpdate's once-per-second tick when the model version
// hasn't advanced since the last full render. If the user scrolled or
// selected a row, selection/scroll state is preserved because we don't
// touch the row count or order.
void UI_Constructor::refreshCallActivityAgeOnly() {
    constexpr int kCallsignCol = 1;
    constexpr int kAgeCol      = 3;

    int const rows = ui->tableWidgetCalls->rowCount();
    for (int r = 0; r < rows; ++r) {
        auto *callCell = ui->tableWidgetCalls->item(r, kCallsignCol);
        auto *ageCell  = ui->tableWidgetCalls->item(r, kAgeCol);
        if (!callCell || !ageCell) continue;

        QString const call = callCell->data(Qt::UserRole).toString();
        if (call.isEmpty()) continue;

        auto const &activity = shownCallActivity(); // [JTTY] the list shown
        auto it = activity.constFind(call);
        if (it == activity.constEnd()) continue;

        ageCell->setText(since(it->utcTimestamp));
    }
}
