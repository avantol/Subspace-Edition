/**
 * [stamon, TODO #210] Per-station conversation follower. See header.
 */

#include "StationMonitorWindow.h"

#include "JS8_Include/SettingsGroup.h"
#include "JS8_Main/Radio.h"
#include "JS8_Main/Varicode.h"
#include "JS8_Mode/JS8Submode.h"

#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QTextBrowser>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSettings>
#include <QTimeZone>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {
// A member offset stays joinable this long after its last use; a
// station keys the same offset across a conversation, minutes apart.
constexpr qint64 OFFSET_TTL_MS = 10 * 60 * 1000;
// Backfill window: trailing hours of history replayed at open
// (operator 2026-09-01: 4.7 days of tail overgrew the member set).
constexpr int BACKFILL_HOURS = 6;
// And at most this much of each log's tail is even read.
constexpr qint64 BACKFILL_TAIL_BYTES = 512 * 1024;

// Real single-station callsigns only: no @groups, no empty parts.
QStringList validParties(QString const &joined) {
    QStringList out;
    for (QString const &p : joined.split(QLatin1Char('>'),
                                         Qt::SkipEmptyParts)) {
        QString const c = p.trimmed();
        if (!c.isEmpty() && !c.startsWith(QLatin1Char('@')) &&
            Radio::is_callsign(c))
            out << c;
    }
    return out;
}
} // namespace

StationMonitorWindow::StationMonitorWindow(
    QSettings *settings, QString const &station,
    QString const &directedTxtPath, QString const &allTxtPath,
    QString const &myCall, QWidget *parent)
    : QWidget{parent}, m_station{station}, m_myCall{myCall},
      m_directedTxtPath{directedTxtPath}, m_allTxtPath{allTxtPath},
      m_settings{settings} {
    setWindowFlag(Qt::Window);
    setWindowTitle(tr("Station monitor: %1").arg(station));
    // Resizable down to a sliver (operator 2026-09-01) -- a corner
    // strip showing just the latest lines is a valid use.
    setMinimumSize(180, 100);
    // [operator 2026-09-01] New windows take the last monitor's
    // DIMENSIONS (never its position); resizeEvent keeps the value
    // current, and the same key persists it across sessions.
    QSize sz{560, 360};
    if (m_settings) {
        SettingsGroup g{m_settings, "StationMonitor"};
        sz = m_settings->value("size", sz).toSize();
    }
    resize(sz.expandedTo(minimumSize()));
    auto *lay = new QVBoxLayout(this);
    // [operator 2026-09-01] No headline -- the log is the window.
    // [monlinks 2026-09-03] QTextBrowser instead of QPlainTextEdit:
    // callsigns render as anchors; single click selects the call,
    // double click also QSYs (handled by the owner via signals).
    m_log = new QTextBrowser(this);
    m_log->setReadOnly(true);
    m_log->setOpenLinks(false);
    m_log->setOpenExternalLinks(false);
    // [operator 2026-09-03] Quiet links: plain blue, no underline.
    // No background rule -- an explicit one (even transparent)
    // overrides the seed-line tint span and punches holes in it;
    // links inherit the line's tint (operator: "bring the tint
    // back for the links"). Set BEFORE any content is inserted.
    m_log->document()->setDefaultStyleSheet(QStringLiteral(
        "a { color: #2a6fc9; text-decoration: none; }"));
    QFont mono{QStringLiteral("monospace")};
    mono.setStyleHint(QFont::Monospace);
    m_log->setFont(mono);
    lay->addWidget(m_log);
    connect(m_log, &QTextBrowser::anchorClicked, this,
            [this](QUrl const &url) {
                if (url.scheme() == QStringLiteral("call"))
                    Q_EMIT callClicked(url.path());
            });
    m_log->viewport()->installEventFilter(this);
    // [TODO #214] Right-click on a linked callsign offers "Open
    // station monitor" (replacing Qt's default copy-link menu
    // there); anywhere else keeps the standard menu.
    m_log->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_log, &QWidget::customContextMenuRequested, this,
            [this](QPoint const &pos) {
                QString const href = m_log->anchorAt(pos);
                auto *menu =
                    href.startsWith(QStringLiteral("call:"))
                        ? nullptr
                        : m_log->createStandardContextMenu(pos);
                if (menu) {
                    // [operator 2026-09-04] "Select All" not wanted
                    // either -- the standard menu keeps Copy only.
                    for (QAction *a : menu->actions())
                        if (a->text().contains(
                                QStringLiteral("Select All")))
                            menu->removeAction(a);
                }
                if (!menu) {
                    menu = new QMenu(this);
                    QString const call = href.mid(5);
                    connect(menu->addAction(
                                tr("Open station monitor for %1")
                                    .arg(call)),
                            &QAction::triggered, this,
                            [this, call]() {
                                Q_EMIT openMonitorRequested(call);
                            });
                }
                menu->setAttribute(Qt::WA_DeleteOnClose);
                menu->popup(
                    m_log->viewport()->mapToGlobal(pos));
            });

    // Pastel tint for the station's own lines: hue from the
    // callsign hash (stable across sessions, distinct per window),
    // barely-there saturation so black text stays fully readable.
    m_tint = QColor::fromHsv(
        static_cast<int>(qHash(station) % 360u), 45, 255);

    m_members.insert(station);
    updateHeadline();
    refreshTitle();
    // Keep the title's age honest between events.
    auto *ageTimer = new QTimer(this);
    ageTimer->setInterval(30 * 1000);
    connect(ageTimer, &QTimer::timeout, this,
            &StationMonitorWindow::refreshTitle);
    ageTimer->start();
}

void StationMonitorWindow::feed(QString const &from, QString const &to,
                                QString const &relayPath,
                                QString const &text, int offset,
                                QDateTime const &utc, int submode,
                                bool historical) {
    // [operator field-caught 2026-09-02, WM8Q/P monitor] A body
    // MENTION is not participation: "...I'M AT WM8Q, NOT WM8Q/P"
    // admitted itself into the WM8Q/P window via parseCallsigns.
    // Parties are STRUCTURAL roles only -- sender (itself possibly
    // a relay path "A>B", processCommandActivity.cpp:1070), the
    // addressee, relay hops, and the *DE* origin of a relayed
    // answer. Free-text mentions never admit. Full-callsign
    // identity throughout.
    QStringList parties = validParties(from);
    parties += validParties(to);
    parties += validParties(relayPath);
    // Leading addressee token after the "FROM: " prefix -- the
    // structural position; covers backfilled lines whose to/relay
    // fields are empty ("WM8Q: WM8Q/P SNR?" -> WM8Q/P).
    if (int const colon = text.indexOf(QStringLiteral(": "));
        colon > 0) {
        QString const rest = text.mid(colon + 2).trimmed();
        parties +=
            validParties(rest.section(QLatin1Char(' '), 0, 0));
    }
    // [operator ruling 2026-09-04] NO *DE* extraction: the monitor
    // traces the TWO stations exchanging a message, and *DE* lives
    // in the BODY, not the addressing -- "only the from and to are
    // to be treated as data." (Second instance of promoting a
    // meaningful body token to structural status; meaning is not
    // position.) Cost, accepted per spec: a relayed answer names
    // its origin only in *DE*, so the ORIGIN's window does not show
    // it -- the two windows of the stations actually keying do.
    parties.removeDuplicates();

    // [operator ruling 2026-09-01, second revision] Transitive
    // growth is OUT -- the measured 6h replay showed HB-ACK webs
    // connect the whole band in two hops (KQ4KLX arrived via
    // XE2MAM with no seed relation). Rules now:
    //   SHOW: lines the SEED station is party to, plus OUR OWN
    //         transmissions ("seed lines only plus my
    //         transmissions").
    //   GROW: interlocutors from seed lines only (headline count).
    //   OFFSET: live-only, and only the SEED's own offsets -- an
    //         unattributed transmission at the seed's current
    //         offset is (probably) the seed mid-message.
    bool seedLine = parties.contains(m_station);
    // [operator ruling 2026-09-03] The offset match must NOT
    // override a directed MISMATCH: a line whose addressing is
    // readable and does not involve the seed ("WD4KAV: AC0Z ...")
    // is someone else's conversation, however close the offset
    // (field-caught: we answer stations ON their offset, so
    // near-offset attributed traffic is systematic). Offset only
    // admits lines with NO readable parties at all.
    if (!seedLine && parties.isEmpty() && !historical && offset > 0) {
        int const range = JS8::Submode::rxThreshold(
            submode >= 0 ? submode : Varicode::JS8CallNormal);
        qint64 const nowMs = utc.toMSecsSinceEpoch();
        for (auto it = m_memberOffsets.constBegin();
             it != m_memberOffsets.constEnd(); ++it)
            if (qAbs(it.key() - offset) <= range &&
                nowMs - it.value() <= OFFSET_TTL_MS) {
                seedLine = true;
                break;
            }
    }
    // [operator 2026-09-01] Our transmissions show only when
    // DIRECTED TO the target -- and such a line names the target,
    // so it is already a seed line. No separate our-TX admit.
    if (!seedLine)
        return;

    if (seedLine) {
        for (QString const &c : parties)
            m_members.insert(c);
        // Record only offsets the seed itself TRANSMITS on -- a
        // partner ACKing the seed keys its own offset, not his.
        bool const seedSent =
            validParties(from).contains(m_station) ||
            text.startsWith(m_station + QStringLiteral(":"));
        // [operator 2026-09-01] Title bar carries the target's most
        // recent transmit: age, offset, speed when known.
        if (seedSent && (!m_lastSeedUtc.isValid() ||
                         utc >= m_lastSeedUtc)) {
            m_lastSeedUtc = utc;
            m_lastSeedOffset = offset;
            m_lastSeedSubmode = historical ? -1 : submode;
            refreshTitle();
        }
        if (seedSent && !historical && offset > 0) {
            m_memberOffsets[offset] = utc.toMSecsSinceEpoch();
            for (auto it = m_memberOffsets.begin();
                 it != m_memberOffsets.end();)
                if (utc.toMSecsSinceEpoch() - it.value() >
                    OFFSET_TTL_MS)
                    it = m_memberOffsets.erase(it);
                else
                    ++it;
        }
    }

    // The conversation panel's exact preamble (mainwindow.cpp
    // writeMessageTextToUI): mode letter - time - (offset) - text.
    // Heartbeat-related lines skipped when the View menu hides
    // them -- SAME tokens as the band-activity filter
    // (displayBandActivity.cpp showHB block).
    if (m_showHb && !m_showHb() &&
        (text.contains(QStringLiteral(" HEARTBEAT ")) ||
         text.contains(QStringLiteral(" @HB "))))
        return;

    // [operator 2026-09-01] The MONITORED station's own lines get
    // the bold leading call AND a very light background tint --
    // per-window color, derived from the callsign hash (stable
    // across sessions), applied to the whole line.
    bool const seedOwn =
        text.startsWith(m_station + QStringLiteral(":"));
    QString shown = text.toHtmlEscaped();
    if (seedOwn)
        shown = QStringLiteral("<b>%1</b>%2")
                    .arg(m_station.toHtmlEscaped(),
                         text.mid(m_station.size()).toHtmlEscaped());
    // [monlinks 2026-09-03] The line's STRUCTURAL callsigns (the
    // same parties the admit rule extracted) become anchors --
    // single-pass left-to-right rebuild so an inserted href can
    // never itself be re-matched; lookarounds keep "WM8Q" from
    // matching inside "WM8Q/P".
    // [operator 2026-09-03] Our OWN callsign is never a link --
    // selecting or QSYing to ourselves is meaningless.
    QStringList pats = parties;
    pats.removeAll(m_myCall);
    if (!pats.isEmpty()) {
        std::sort(pats.begin(), pats.end(),
                  [](QString const &a, QString const &b) {
                      return a.size() > b.size();
                  });
        for (QString &c : pats)
            c = QRegularExpression::escape(c);
        QRegularExpression const linkRe{
            QStringLiteral("(?<![A-Z0-9/])(%1)(?![A-Z0-9/])")
                .arg(pats.join(QLatin1Char('|')))};
        QString out;
        int last = 0;
        auto it = linkRe.globalMatch(shown);
        while (it.hasNext()) {
            auto const m = it.next();
            out += shown.mid(last, m.capturedStart() - last);
            out += QStringLiteral("<a href=\"call:%1\">%1</a>")
                       .arg(m.captured(1));
            last = m.capturedEnd();
        }
        out += shown.mid(last);
        shown = out;
    }
    // [operator 2026-09-01] Conditional auto-scroll: follow new
    // lines only when the view was ALREADY at the bottom -- a
    // scrolled-back review position stays put.
    auto *sb = m_log->verticalScrollBar();
    bool const wasAtBottom = sb->value() == sb->maximum();
    QString line =
        QStringLiteral("%1 - %2 - (%3) - %4")
            .arg(historical ? QStringLiteral("?")
                            : JS8::Submode::indicator(submode))
            .arg(utc.time().toString())
            .arg(offset > 0 ? QString::number(offset)
                            : QStringLiteral("?"))
            .arg(shown);
    if (seedOwn)
        line = QStringLiteral("<span style=\"background-color:%1;"
                              "color:black\">%2</span>")
                   .arg(m_tint.name(), line);
    // QTextBrowser::append treats input as HTML only when it LOOKS
    // rich; the span wrapper forces that for every line.
    m_log->append(QStringLiteral("<span>%1</span>").arg(line));
    // Deferred one turn: QTextBrowser's rich-text layout is lazy,
    // so maximum() right after append() is STALE -- the immediate
    // setValue landed one line shy and every later wasAtBottom
    // check then failed (field-caught 2026-09-03: auto-scroll died
    // after the first line).
    if (wasAtBottom)
        QTimer::singleShot(0, m_log, [log = m_log]() {
            auto *sb2 = log->verticalScrollBar();
            sb2->setValue(sb2->maximum());
        });
    // [operator 2026-09-01] Visual ping on LIVE lines only: a brief
    // green border pulse (theme-neutral), plus the platform's
    // attention flash when the window is not the active one.
    if (!historical) {
        // Selector-free: the monlinks swap to QTextBrowser silently
        // broke a "QPlainTextEdit {...}" rule (field-caught).
        m_log->setStyleSheet(QStringLiteral(
            "border: 4px solid rgb(200,60,60);"));
        QTimer::singleShot(2000, m_log, [log = m_log]() {
            log->setStyleSheet(QString());
        });
        if (!isActiveWindow())
            QApplication::alert(this, 0);
    }
    updateHeadline();
}

namespace {
struct HistLine {
    QDateTime utc;
    QString from;
    QString text;
    int offset;
};

QList<QByteArray> tailLines(QString const &path) {
    QFile f{path};
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    if (f.size() > BACKFILL_TAIL_BYTES)
        f.seek(f.size() - BACKFILL_TAIL_BYTES);
    QList<QByteArray> lines = f.readAll().split('\n');
    // First line after a mid-file seek is almost surely partial.
    if (f.size() > BACKFILL_TAIL_BYTES && !lines.isEmpty())
        lines.removeFirst();
    return lines;
}

QDateTime parseUtc(QString const &s) {
    QDateTime utc = QDateTime::fromString(
        s, QStringLiteral("yyyy-MM-dd hh:mm:ss"));
    utc.setTimeZone(QTimeZone::utc());
    return utc;
}
} // namespace

// Replay history through the SAME membership engine so history and
// live traffic obey identical rules -- except the offset rule,
// which is live-only (see feed). Sources, merged chronologically,
// bounded to the trailing BACKFILL_HOURS:
//  - DIRECTED.TXT (writeMsgTxt): "utc\tMHz\toffset\tSNR\ttext",
//    text = "FROM: TO ...". No mode column -> "?" indicator.
//  - ALL.TXT "Transmitting" lines: OUR side, per-frame with no
//    offset; consecutive frames <=30 s apart stitch into one
//    assembled message credited to myCall.
void StationMonitorWindow::runBackfill() {
    QDateTime const cutoff = QDateTime::currentDateTimeUtc().addSecs(
        -BACKFILL_HOURS * 3600);
    QList<HistLine> hist;

    for (QByteArray const &raw : tailLines(m_directedTxtPath)) {
        QString const line = QString::fromUtf8(raw).trimmed();
        QStringList const cols = line.split(QLatin1Char('\t'));
        if (cols.size() < 5)
            continue;
        QDateTime const utc = parseUtc(cols[0]);
        if (!utc.isValid() || utc < cutoff)
            continue;
        QString const text = cols.mid(4).join(QLatin1Char('\t'));
        // FROM is the text's own "CALL: " prefix; membership also
        // sees every mention, so extraction can stay lax.
        QString from;
        if (int const colon = text.indexOf(QStringLiteral(": "));
            colon > 0)
            from = text.left(colon);
        hist.append({utc, from, text, cols[2].toInt()});
    }

    // Our transmissions: stitch consecutive per-frame lines.
    HistLine tx{QDateTime(), m_myCall, QString(), 0};
    QDateTime lastFrame;
    auto const flushTx = [&]() {
        if (tx.utc.isValid() && !tx.text.trimmed().isEmpty()) {
            // [txcksum2 2026-09-21, operator: "checksums were not
            // removed from my transmits"] The LIVE own-TX feed strips
            // the checksum (mainwindow.cpp, m_txChecksumTail); this
            // path rebuilds our side from ALL.TXT's raw WIRE frames,
            // which still carry it -- field 17:30Z: "WM8Q: @MAGNET
            // QUERY CALL W7SUA? FFC". Received lines never reach here
            // with one (the RX side strips before DIRECTED.TXT), so
            // only this stitch needs it.
            //
            // VALIDATE, never guess: strip the trailing group only
            // when it actually hashes the body, the same test
            // buildMessageFrames uses to drop a stale checksum on
            // recall. A real trailing word cannot be removed by
            // accident.
            QString t = Varicode::rstrip(tx.text);
            for (int const tail : {6, 3}) {
                if (t.length() < tail + 2 ||
                    t.at(t.length() - tail - 1) != QLatin1Char(' '))
                    continue;
                QString const suffix = t.right(tail);
                QString const body = t.left(t.length() - tail - 1);
                bool const valid =
                    tail == 6 ? Varicode::checksum32Valid(suffix, body)
                              : Varicode::checksum16Valid(suffix, body);
                if (valid) {
                    tx.text = body;
                    break;
                }
            }
            hist.append(tx);
        }
        tx = {QDateTime(), m_myCall, QString(), 0};
    };
    // write_transmit_entry's separator is "JS8:" + exactly TWO
    // spaces; matching \s{2} keeps the frame text verbatim (a
    // single \s left every stitched message with a leading space,
    // which broke startsWith("CALL:") -- the WM8Q window's own
    // lines lost their bold/tint, field-caught 2026-09-03).
    static QRegularExpression const txRe{QStringLiteral(
        R"(^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\s+Transmitting .*JS8:\s{2}(.*)$)")};
    QString prevFrame;
    QDateTime prevUtc;
    for (QByteArray const &raw : tailLines(m_allTxtPath)) {
        // [monspace 2026-09-04, field: "WHENSELECTED"] NO trimmed()
        // here: ALL.TXT frames carry their word-boundary space as a
        // TRAILING space, and trimming each line glued words at
        // every frame join in stitched historic messages. tailLines
        // opens with QIODevice::Text, so there is no \r to strip.
        auto const m = txRe.match(QString::fromUtf8(raw));
        if (!m.hasMatch())
            continue;
        QDateTime const utc = parseUtc(m.captured(1));
        if (!utc.isValid() || utc < cutoff)
            continue;
        QString const frame = m.captured(2);
        // ALL.TXT double-logs the FIRST frame of a directed send
        // (measured 2026-09-01: identical text at the identical
        // second) -- drop the twin.
        if (utc == prevUtc && frame == prevFrame)
            continue;
        prevUtc = utc;
        prevFrame = frame;
        // A frame opening with our "CALL: " prefix IS a first
        // frame: a retransmission of the same message starts a NEW
        // stitched line (the 30 s gap alone merged a measured
        // 20 s-apart repeat into one doubled line).
        bool const firstFrame = frame.startsWith(
            m_myCall + QStringLiteral(": "));
        if (tx.utc.isValid() &&
            (firstFrame || lastFrame.secsTo(utc) > 30))
            flushTx();
        if (!tx.utc.isValid())
            tx.utc = utc;
        tx.text += frame;
        lastFrame = utc;
    }
    flushTx();

    std::stable_sort(hist.begin(), hist.end(),
                     [](HistLine const &a, HistLine const &b) {
                         return a.utc < b.utc;
                     });
    for (HistLine const &h : hist)
        feed(h.from, QString(), QString(), h.text, h.offset, h.utc,
             -1, true);
    // [operator 2026-09-03] A fresh window opens showing the LATEST
    // lines. Deferred one event-loop turn so the document layout
    // (and thus the scrollbar maximum) is settled first.
    QTimer::singleShot(0, m_log, [log = m_log]() {
        auto *sb = log->verticalScrollBar();
        sb->setValue(sb->maximum());
    });
}

bool StationMonitorWindow::eventFilter(QObject *obj, QEvent *event) {
    // Double-click on an anchor: QTextBrowser has no double-click
    // signal, so read the anchor under the cursor ourselves. The
    // preceding single click has already fired callClicked --
    // double's action is additive (select + QSY), so that is fine.
    if (obj == m_log->viewport() &&
        event->type() == QEvent::MouseButtonDblClick) {
        auto const *me = static_cast<QMouseEvent *>(event);
        if (QString const href =
                m_log->anchorAt(me->position().toPoint());
            href.startsWith(QStringLiteral("call:"))) {
            Q_EMIT callDoubleClicked(href.mid(5));
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

void StationMonitorWindow::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (m_settings) {
        SettingsGroup g{m_settings, "StationMonitor"};
        m_settings->setValue("size", size());
    }
}

void StationMonitorWindow::refreshTitle() {
    QString title = tr("Station monitor: %1").arg(m_station);
    if (m_lastSeedUtc.isValid()) {
        qint64 const s =
            m_lastSeedUtc.secsTo(QDateTime::currentDateTimeUtc());
        QString const age =
            s < 60      ? tr("%1s ago").arg(qMax<qint64>(0, s))
            : s < 3600  ? tr("%1m ago").arg(s / 60)
                        : tr("%1h %2m ago").arg(s / 3600).arg(s % 3600 / 60);
        title += QStringLiteral(" -- TX %1").arg(age);
        if (m_lastSeedOffset > 0)
            title += tr(", %1 Hz").arg(m_lastSeedOffset);
        if (m_lastSeedSubmode >= 0)
            title += QStringLiteral(", %1").arg(
                JS8::Submode::name(m_lastSeedSubmode));
    }
    setWindowTitle(title);
}

void StationMonitorWindow::updateHeadline() {
    // [operator 2026-09-01] Dropped from the UI ("drop
    // Following..."); the member set remains the engine's state.
}
