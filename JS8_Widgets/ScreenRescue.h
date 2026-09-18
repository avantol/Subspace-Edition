#pragma once

// [TODO #246 offscreen 2026-09-18] One authority for "a restored window
// must be reachable".
//
// Windows 11 field report (Build 478, three monitors down to two): the
// main window came back on the monitor that no longer existed. No title
// bar to grab, so no drag and no minimize -- only Alt+Tab could reach
// it. Qt is supposed to clamp a restored frame to the available
// screens; on that Qt 6.9 Windows build it did not. Andy hit the
// identical thing with WSJT-X, so this is not ours alone.
//
// Called after every restoreGeometry(). Does NOTHING while the window
// lands on a real screen, which is every ordinary start -- it only acts
// when the frame intersects no available screen area at all, i.e. the
// window is unreachable.

#include <QGuiApplication>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QWidget>

namespace JS8 {

inline void rescueOffScreen(QWidget *widget) {
    if (!widget)
        return;

    // Partly visible is fine -- a sliver of title bar is enough to drag
    // the window back, and moving a window the operator deliberately
    // parked half off-screen would be its own annoyance.
    QRect const frame = widget->frameGeometry();
    for (auto const *screen : QGuiApplication::screens())
        if (screen->availableGeometry().intersects(frame))
            return;

    auto const *primary = QGuiApplication::primaryScreen();
    if (!primary)
        return;

    QRect const available = primary->availableGeometry();
    // Keep the operator's size unless it cannot fit the screen we are
    // moving to; then shrink to fit rather than hang off the edge.
    QSize const size = widget->size().boundedTo(available.size());
    if (size != widget->size())
        widget->resize(size);
    widget->move(available.x() + (available.width() - size.width()) / 2,
                 available.y() + (available.height() - size.height()) / 2);
}

} // namespace JS8
