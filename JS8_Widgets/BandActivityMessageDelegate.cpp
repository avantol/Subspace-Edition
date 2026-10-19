#include "BandActivityMessageDelegate.h"
#include "JS8_Main/Varicode.h"   // [#240] the callsign-shape authority

#include <QAbstractItemView>
#include <QApplication>
#include <QHelpEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QToolTip>

#include <algorithm>

namespace {
// The candidate shape: 3-15 of [A-Z0-9/] with a digit, then ": ".
// A PRE-FILTER only -- acceptance is isSenderPrefix(), the one
// authority (#240).
QRegularExpression const &callPrefixRe() {
    static const QRegularExpression re(
        R"(\b((?=[A-Z0-9/]*[0-9])[A-Z0-9/]{3,15}: ))");
    return re;
}
}

bool BandActivityMessageDelegate::isSenderPrefix(const QString &captured,
                                                 const QString &senderCall,
                                                 bool atStart) {
    QString candidate = captured.trimmed();
    if (candidate.endsWith(QLatin1Char(':')))
        candidate.chop(1);
    if (candidate.length() < 3 || candidate.length() > 15 ||
        !Varicode::isValidCallsign(candidate, nullptr))
        return false;   // "2,KCNA" and the like
    if (atStart)
        return true;    // the group's opening sender
    return !senderCall.isEmpty() &&
           candidate.compare(senderCall, Qt::CaseInsensitive) == 0;
}

namespace {
// One group's text as HTML: the sender prefix(es) bold, body plain.
// The sender as a bare word: a JTTY message carries no "CALL: " prefix
// ("WM8Q K9AVT ...", "K6KH W5NM 73 TU"); its attributed sender is bold
// at its first whole-word occurrence. Case-insensitive, '/' allowed.
QRegularExpressionMatch bareSenderMatch(const QString &text,
                                        const QString &senderCall) {
    if (senderCall.isEmpty()) return QRegularExpressionMatch();
    QRegularExpression const re(
        QStringLiteral("(?<![A-Z0-9/])%1(?![A-Z0-9/])")
            .arg(QRegularExpression::escape(senderCall)),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(text);
}

QString boldSenderBody(const QString &text, const QString &senderCall,
                       bool bareSender = false) {
    QString html;
    int lastEnd = 0;
    bool prefixed = false;
    auto it = callPrefixRe().globalMatch(text);
    while (it.hasNext()) {
        auto const match = it.next();
        if (!BandActivityMessageDelegate::isSenderPrefix(
                match.captured(0), senderCall, match.capturedStart() == 0))
            continue;
        html += text.mid(lastEnd, match.capturedStart() - lastEnd)
                    .toHtmlEscaped();
        QString const prefix = match.captured(0).trimmed();
        html += QStringLiteral("<b>%1</b> ").arg(prefix.toHtmlEscaped());
        lastEnd = match.capturedEnd();
        prefixed = true;
    }
    // [operator 2026-10-13] no prefix form: the bare sender, bold
    if (!prefixed && bareSender) {
        auto const m = bareSenderMatch(text, senderCall);
        if (m.hasMatch())
            return text.left(m.capturedStart()).toHtmlEscaped() +
                   QStringLiteral("<b>%1</b>").arg(m.captured(0).toHtmlEscaped()) +
                   text.mid(m.capturedEnd()).toHtmlEscaped();
    }
    html += text.mid(lastEnd).toHtmlEscaped();
    return html;
}
QString wrapTooltip(const QString &body) {
    // Wrap in a wide container to prevent premature line wrapping
    return QStringLiteral("<div style='white-space:nowrap;'>%1</div>")
        .arg(body);
}
}

QString BandActivityMessageDelegate::boldCallsignsHtml(
    const QString &text, const QString &senderCall) {
    return wrapTooltip(boldSenderBody(text, senderCall));
}

QString BandActivityMessageDelegate::rowTooltipHtml(
    const QVariantList &groups, const QString &joined, bool bareSender) {
    if (groups.isEmpty())
        return boldCallsignsHtml(joined, QString());
    // [TODO #308, operator 2026-10-18] The LAST 10 entries of the line
    // only, JS8 and JTTY alike (a JTTY line keeps up to 100 messages).
    // 10 is the operator's number.
    constexpr int kTipEntries = 10;
    QStringList lines;
    for (int i = qMax(0, int(groups.size()) - kTipEntries); i < groups.size(); ++i) {
        auto const map = groups.at(i).toMap();
        lines << boldSenderBody(map[QStringLiteral("text")].toString(),
                                map[QStringLiteral("call")].toString(),
                                bareSender);
    }
    return wrapTooltip(lines.join(QStringLiteral("<br/>")));
}

namespace {
// [TODO #79 REOPENED 2026-07-15] Strip EVERY character Qt's text
// layout treats as a line break — the build-331 fix handled only
// \n \r \t, but U+2028 LINE SEP, U+2029 PARA SEP, U+0085 NEL,
// vertical tab and form feed also break lines. The probe qWarning
// dumps the exact code points seen so the specimen identifies which
// breaker is arriving on the air (remove probe once confirmed).
QString stripLineBreaks(QString text) {
    bool found = false;
    for (QChar &c : text) {
        ushort const u = c.unicode();
        if (u == u'\n' || u == u'\r' || u == u'\t' || u == 0x0B ||
            u == 0x0C || u == 0x85 || u == 0x2028 || u == 0x2029) {
            found = true;
            c = QChar(' ');
        }
    }
    if (found) {
        static int probeBudget = 20;
        if (probeBudget > 0) {
            --probeBudget;
            qWarning() << "[BAND-ACT #79] line-break char(s) in message;"
                       << "text=" << text.left(60);
        }
    }
    return text;
}
} // namespace

BandActivityMessageDelegate::BandActivityMessageDelegate(QObject *parent)
    : QStyledItemDelegate(parent) {}

QVariantList BandActivityMessageDelegate::getGroups(const QModelIndex &index) {
    return index.data(Qt::UserRole).toList();
}

// [TODO #79 REOPENED] Row height was the OTHER half of the two-line
// symptom: with no sizeHint override, the default implementation
// measured the RAW DisplayRole text — a message with an embedded
// line break produced a double-height row even though paint draws a
// single stripped line. Measure the stripped text instead.
QSize BandActivityMessageDelegate::sizeHint(
    const QStyleOptionViewItem &option, const QModelIndex &index) const {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text = stripLineBreaks(opt.text);
    QSize sz = QStyledItemDelegate::sizeHint(opt, index);
    // Never taller than one text line (+ padding) regardless of what
    // the style computes.
    QFontMetrics const fm(opt.font);
    sz.setHeight(qMin(sz.height(), fm.height() + 8));
    return sz;
}

void BandActivityMessageDelegate::paint(QPainter *painter,
                                        const QStyleOptionViewItem &option,
                                        const QModelIndex &index) const {
    if (!painter || !index.isValid())
        return;

    auto groups = getGroups(index);

    // Single group or no groups: render with bold callsigns but no dividers
    if (groups.size() <= 1) {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        QString text = stripLineBreaks(opt.text);
        opt.text.clear();
        QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter);

        painter->save();
        QRect textRect = opt.rect.adjusted(4, 0, -4, 0);
        QColor textColor = opt.state & QStyle::State_Selected
            ? opt.palette.highlightedText().color()
            : opt.palette.text().color();
        painter->setPen(textColor);

        // Elide from left (show most recent) for consistency
        QFontMetrics fm(opt.font);
        QString elided = fm.elidedText(text, Qt::ElideLeft, textRect.width());

        // Draw text with the group's SENDER bold (#240 layer 1b)
        QString const senderCall =
            groups.isEmpty()
                ? QString()
                : groups[0].toMap()[QStringLiteral("call")].toString();
        // Bold spans, found in the FULL text: accepted "CALL: "
        // prefixes, and [operator 2026-10-13] for a line of JTTY
        // messages (the per-message list in UserRole + 1) each
        // message's sender at its first bare occurrence inside that
        // message -- "like JS8 mode", where the sender is bold.
        QList<QPair<int, int>> spans;
        {
            auto it = callPrefixRe().globalMatch(text);
            while (it.hasNext()) {
                auto match = it.next();
                if (!isSenderPrefix(match.captured(0), senderCall,
                                    match.capturedStart() == 0))
                    continue;   // body text (#240 layer 1b)
                spans.append({match.capturedStart(), match.capturedEnd()});
            }
            int cursor = 0;
            for (auto const &g : index.data(Qt::UserRole + 1).toList()) {
                auto const map = g.toMap();
                QString const mText =
                    stripLineBreaks(map[QStringLiteral("text")].toString());
                int const at = mText.isEmpty() ? -1 : text.indexOf(mText, cursor);
                if (at < 0) continue;
                auto const m = bareSenderMatch(
                    mText, map[QStringLiteral("call")].toString());
                if (m.hasMatch())
                    spans.append({at + m.capturedStart(), at + m.capturedEnd()});
                cursor = at + mText.length();
            }
            // earliest first; of two at one start, the longer (the
            // "K9AVT: " prefix over the bare "K9AVT" inside it)
            std::sort(spans.begin(), spans.end(),
                      [](QPair<int, int> const &a, QPair<int, int> const &b) {
                          return a.first != b.first ? a.first < b.first
                                                    : a.second > b.second;
                      });
        }
        // ElideLeft keeps a SUFFIX of the text behind one ellipsis
        // character, so a kept span shifts by the length difference
        // and a span in the dropped head is skipped.
        int const shift = elided.length() - text.length();
        int const firstKept = (shift == 0) ? 0 : 1;
        QFont boldFont = opt.font;
        boldFont.setBold(true);
        int xPos = textRect.x();
        int remaining = textRect.width();
        int lastEnd = 0;
        for (auto const &s : spans) {
            int const start = s.first + shift, end = s.second + shift;
            if (start < firstKept || start < lastEnd || remaining <= 0)
                continue;
            // Draw text before the callsign (normal)
            if (start > lastEnd) {
                QString before = elided.mid(lastEnd, start - lastEnd);
                painter->setFont(opt.font);
                painter->drawText(QRect(xPos, textRect.y(), remaining, textRect.height()),
                                  Qt::AlignLeft | Qt::AlignVCenter, before);
                int w = QFontMetrics(opt.font).horizontalAdvance(before);
                xPos += w;
                remaining -= w;
            }
            // Draw callsign in bold
            QString call = elided.mid(start, end - start);
            painter->setFont(boldFont);
            painter->drawText(QRect(xPos, textRect.y(), remaining, textRect.height()),
                              Qt::AlignLeft | Qt::AlignVCenter, call);
            int w = QFontMetrics(boldFont).horizontalAdvance(call);
            xPos += w;
            remaining -= w;
            lastEnd = end;
        }
        // Draw remaining text (normal)
        if (lastEnd < elided.length() && remaining > 0) {
            painter->setFont(opt.font);
            painter->drawText(QRect(xPos, textRect.y(), remaining, textRect.height()),
                              Qt::AlignLeft | Qt::AlignVCenter, elided.mid(lastEnd));
        }
        painter->restore();
        return;
    }

    // Draw background (selection highlight, alternating row colors)
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text.clear(); // we'll draw text ourselves
    QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter);

    painter->save();

    const QRect rect = opt.rect;
    const int n = groups.size();
    const int regionWidth = rect.width() / n;
    const QFontMetrics fm(opt.font);
    const int textMargin = 4;
    const bool selected = opt.state & QStyle::State_Selected;

    for (int i = 0; i < n; i++) {
        auto map = groups[i].toMap();
        QString text = stripLineBreaks(map["text"].toString());

        QRect regionRect(rect.x() + i * regionWidth, rect.y(),
                         regionWidth, rect.height());

        // Draw vertical divider (except before first region)
        if (i > 0) {
            // Use background color when selected, gray otherwise
            QColor divColor = selected
                ? opt.palette.base().color()
                : QColor(180, 180, 180);
            painter->setPen(QPen(divColor, 1));
            painter->drawLine(regionRect.topLeft(), regionRect.bottomLeft());
        }

        // Draw text with bold callsigns. Left-justify (ElideRight) for
        // key status messages where the start is most important.
        QRect textRect = regionRect.adjusted(textMargin, 0, -textMargin, 0);
        bool leftJustify = text.contains("@HB HEARTBEAT ")
            || text.contains("HEARTBEAT SNR")
            || text.contains("@ALLCALL CQ")
            || (!m_myCall.isEmpty() && (text.contains(m_myCall + " SNR")
                                       || text.contains(m_myCall + " YES")));
        auto elideMode = leftJustify ? Qt::ElideRight : Qt::ElideLeft;
        QString elided = fm.elidedText(text, elideMode, textRect.width());

        // Draw text with bold callsigns only (CALL: with digit)
        QColor textColor = selected
            ? opt.palette.highlightedText().color()
            : opt.palette.text().color();
        painter->setPen(textColor);

        QFont boldFont = opt.font;
        boldFont.setBold(true);
        int xPos = textRect.x();
        int remaining = textRect.width();
        QString const senderCall = map[QStringLiteral("call")].toString();
        auto it = callPrefixRe().globalMatch(elided);
        int lastEnd = 0;
        while (it.hasNext() && remaining > 0) {
            auto match = it.next();
            if (!isSenderPrefix(match.captured(0), senderCall,
                                match.capturedStart() == 0 &&
                                    elided.size() == text.size()))
                continue;   // body text (#240 layer 1b)
            if (match.capturedStart() > lastEnd) {
                QString before = elided.mid(lastEnd, match.capturedStart() - lastEnd);
                painter->setFont(opt.font);
                painter->drawText(QRect(xPos, textRect.y(), remaining, textRect.height()),
                                  Qt::AlignLeft | Qt::AlignVCenter, before);
                int w = QFontMetrics(opt.font).horizontalAdvance(before);
                xPos += w; remaining -= w;
            }
            QString call = match.captured(0);
            painter->setFont(boldFont);
            painter->drawText(QRect(xPos, textRect.y(), remaining, textRect.height()),
                              Qt::AlignLeft | Qt::AlignVCenter, call);
            int w = QFontMetrics(boldFont).horizontalAdvance(call);
            xPos += w; remaining -= w;
            lastEnd = match.capturedEnd();
        }
        if (lastEnd < elided.length() && remaining > 0) {
            painter->setFont(opt.font);
            painter->drawText(QRect(xPos, textRect.y(), remaining, textRect.height()),
                              Qt::AlignLeft | Qt::AlignVCenter, elided.mid(lastEnd));
        }
    }

    painter->restore();
}

bool BandActivityMessageDelegate::helpEvent(QHelpEvent *event,
                                            QAbstractItemView *view,
                                            const QStyleOptionViewItem &option,
                                            const QModelIndex &index) {
    if (event->type() != QEvent::ToolTip)
        return QStyledItemDelegate::helpEvent(event, view, option, index);

    auto groups = getGroups(index);

    // [operator 2026-10-13] A line holding JTTY messages carries a
    // per-message list (UserRole + 1): one tooltip line per complete
    // message, its sender bold, in both listing modes. The paint
    // regions (UserRole) are untouched -- a line of fifty messages
    // must not become fifty regions.
    auto const perMessage = index.data(Qt::UserRole + 1).toList();
    if (!perMessage.isEmpty()) {
        QToolTip::showText(
            event->globalPos(),
            rowTooltipHtml(perMessage, index.data(Qt::DisplayRole).toString(),
                           /*bareSender=*/true),
            view);
        return true;
    }

    // Single group or no groups: show full text as tooltip if elided
    if (groups.size() <= 1) {
        QString text = index.data(Qt::DisplayRole).toString();
        if (!text.isEmpty()) {
            QStyleOptionViewItem opt = option;
            initStyleOption(&opt, index);
            QFontMetrics fm(opt.font);
            int cellWidth = opt.rect.width();
            if (fm.horizontalAdvance(text) > cellWidth) {
                QToolTip::showText(event->globalPos(),
                                   rowTooltipHtml(groups, text), view);
                return true;
            }
        }
        QToolTip::hideText();
        return true;
    }

    // Determine which sub-region the mouse is over
    QRect rect = option.rect;
    int mouseX = event->pos().x() - rect.x();
    int n = groups.size();
    int regionWidth = rect.width() / n;
    int regionIndex = qBound(0, mouseX / regionWidth, n - 1);

    auto map = groups[regionIndex].toMap();
    QString text = map["text"].toString();
    QToolTip::showText(event->globalPos(),
                       boldCallsignsHtml(text, map["call"].toString()), view);
    return true;
}

QString BandActivityMessageDelegate::callsignAtPosition(
    const QVariantList &groups, int cellWidth, int mouseX) {
    if (groups.isEmpty() || cellWidth <= 0)
        return {};
    int n = groups.size();
    int regionWidth = cellWidth / n;
    int regionIndex = qBound(0, mouseX / regionWidth, n - 1);
    return groups[regionIndex].toMap()["call"].toString();
}
