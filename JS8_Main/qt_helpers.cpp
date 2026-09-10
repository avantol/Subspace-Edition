/**
 * @file qt_helpers.cpp
 * @brief Qt style sheets and widgets
 */

#include "qt_helpers.h"

#include <QFont>
#include <QString>
#include <QStyle>
#include <QVariant>
#include <QWidget>

QString font_as_stylesheet(QFont const &font) {
    QString font_weight;
    switch (font.weight()) {
    case QFont::Thin:
        font_weight = "thin";
        break;
    case QFont::ExtraLight:
        font_weight = "extralight";
        break;
    case QFont::Light:
        font_weight = "light";
        break;
    case QFont::Normal:
        font_weight = "normal";
        break;
    case QFont::Medium:
        font_weight = "medium";
        break;
    case QFont::DemiBold:
        font_weight = "demibold";
        break;
    case QFont::Bold:
        font_weight = "bold";
        break;
    case QFont::ExtraBold:
        font_weight = "extrabold";
        break;
    case QFont::Black:
        font_weight = "black";
        break;
    }
    return QString{" font-family: %1;\n"
                   " font-size: %2pt;\n"
                   " font-style: %3;\n"
                   " font-weight: %4;\n"}
        .arg(font.family())
        .arg(font.pointSize())
        .arg(font.styleName())
        .arg(font_weight);
}

void update_dynamic_property(QWidget *widget, char const *property,
                             QVariant const &value) {
    // [polishguard TODO #223] Do nothing when the value is already set.
    //
    // unpolish/polish is Qt's correct idiom for "a dynamic property
    // changed", but with a stylesheet installed the active style is
    // QStyleSheetStyle, so the pair re-resolves the WHOLE stylesheet for
    // this widget -- re-matching selectors, re-evaluating properties and
    // rebuilding the render-rule cache. displayTransmit() calls this
    // twice from guiUpdate(), which ticks every UI_POLL_INTERVAL_MS
    // (100 ms), so we were doing 20 full re-resolutions per second while
    // "transmitting" changes only at start/end tx.
    //
    // MEASURED (heaptrack, 42 min, 2026-09-10): 36,970 allocation calls
    // per second, 31.8 M of them under QCss::Parser::parse, plus 99,672
    // icon rebuilds via QToolButton::paintEvent because both buttons
    // carry stylesheet icon URLs. Live heap stayed at 15.77 MB against
    // 343 MB RSS -- the growth was allocator retention driven by that
    // churn, not leaked objects, which is why no leak detector found it.
    //
    // NO LATENCY COST: on the tick where the value actually changes the
    // comparison differs and everything below runs exactly as before, on
    // the same call. Only the no-op repeats are skipped.
    if (widget->property(property) == value) {
        return;
    }
    widget->setProperty(property, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}
