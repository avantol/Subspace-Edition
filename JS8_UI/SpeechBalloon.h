#pragma once

#include <QPointer>
#include <QRect>
#include <QString>
#include <QWidget>

#include <functional>

class QPaintEvent;
class QMouseEvent;
class QPushButton;

/**
 * Cartoon-style word balloon that points at a target widget.
 *
 * Frameless, semi-transparent top-level widget. Paints a rounded
 * rectangle with a triangular "tail" that points toward the target
 * widget. Click anywhere on the balloon to dismiss it; optional
 * auto-dismiss timer.
 *
 * Typical usage (first-run onboarding hint):
 *
 *   auto *balloon = new SpeechBalloon(
 *       tr("Click here to send messages via ARQ\n"
 *          "or to send a file. Both are reliable."),
 *       m_sendMenuButton);                       // target
 *   balloon->setTailSide(SpeechBalloon::TailSide::Right);
 *   balloon->setAutoDismissMs(15000);            // 15s
 *   balloon->showAtTarget();
 *
 * The balloon parents itself to the target's top-level window so it
 * stays attached if the user moves the JS8Call window, and
 * disappears when the parent window closes.
 */
class SpeechBalloon : public QWidget {
    Q_OBJECT
  public:
    /**
     * Which side of the balloon body the tail sticks out of.
     * The tail tip points TOWARD the target widget, so the side
     * is opposite the target: target above → tail on Top,
     * target to the right → tail on Right, etc.
     */
    enum class TailSide { Top, Bottom, Left, Right };

    explicit SpeechBalloon(QString const &text, QWidget *target);

    void setTailSide(TailSide side) { m_tailSide = side; updateShape(); }
    void setAutoDismissMs(int ms)   { m_autoDismissMs = ms; }

    /**
     * Point at a sub-rectangle of the target widget (target-local
     * coordinates) instead of the whole widget — e.g. one menu
     * title's actionGeometry() within a menu bar.
     */
    void setTargetRectOverride(QRect const &localRect) {
        m_targetRectOverride = localRect;
    }

    /**
     * [bandhint] Turn the balloon into a Yes/No question: two
     * buttons appear under the text. Yes runs onYes then closes;
     * No closes; a click on the bubble body or the auto-dismiss
     * timeout counts as No. Call BEFORE showAtTarget().
     */
    void setYesNoChoice(std::function<void()> onYes);

    /**
     * [#227 withdraw] A modal dialog that appears AFTER this balloon is
     * on screen buries it -- the operator never gets to read it, but
     * the one-per-startup chain has already marked the hint as shown.
     * With this set, the balloon watches for a modal window becoming
     * visible while it is up, closes itself, and calls `onWithdrawn`
     * so the caller can UN-mark the hint and offer it another time.
     * The callback means "withdrawn unseen", never "dismissed": a
     * click, a Yes/No answer or the auto-dismiss timeout do not call
     * it. Call BEFORE showAtTarget().
     */
    void setWithdrawOnModal(std::function<void()> onWithdrawn);

    /** Compute position relative to target and show. */
    void showAtTarget();

  protected:
    void paintEvent(QPaintEvent *)      override;
    void mousePressEvent(QMouseEvent *) override;
    void resizeEvent(QResizeEvent *)    override;
    bool eventFilter(QObject *, QEvent *) override;

  private:
    void updateShape();
    void layoutButtons();
    void withdraw(); // [#227] close unseen and report

    QString             m_text;
    QPointer<QWidget>   m_target;
    QPushButton        *m_yesButton{nullptr}; // [bandhint] choice mode
    QPushButton        *m_noButton{nullptr};
    int                 m_buttonRowH{0};      // extra body height
    QRect               m_targetRectOverride; // local coords; invalid = whole widget
    std::function<void()> m_onWithdrawn;      // [#227] set = watch for modals
    bool                m_withdrawn{false};   // [#227] report once
    TailSide            m_tailSide{TailSide::Top};
    int                 m_autoDismissMs{0};
    int                 m_cornerRadius{8};
    int                 m_tailWidth{16};
    int                 m_tailHeight{12};
    int                 m_paddingX{14};
    int                 m_paddingY{10};
};
