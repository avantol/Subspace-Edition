#ifndef SELFDESTRUCTMESSAGEBOX_H
#define SELFDESTRUCTMESSAGEBOX_H

#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QTimer>
#include <QWidget>

class SelfDestructMessageBox : public QMessageBox {
    Q_OBJECT

  public:
    SelfDestructMessageBox(
        int timeout, const QString &title, const QString &text,
        QMessageBox::Icon icon,
        QMessageBox::StandardButtons buttons = QMessageBox::Ok |
                                               QMessageBox::Cancel,
        QMessageBox::StandardButton defaultButton = QMessageBox::Ok,
        bool show_countdown = false, QWidget *parent = nullptr,
        Qt::WindowFlags flags = Qt::WindowFlags());

    void showEvent(QShowEvent *event) override;

    void setShowCountdown(bool countdown) { m_show_countdown = countdown; }

    // [confirmcoalesce 2026-09-28] A coalescing box gains entries while
    // it is open, and a later entry can carry an EARLIER deadline than
    // the one the box was built with. The countdown must be able to
    // move in, never out, so the earliest real deadline always governs.
    void shortenTimeout(int seconds) {
        if (seconds > 0 && seconds < m_timeout) m_timeout = seconds;
    }

    void stopTimer();

  private slots:
    void tick();

  private:
    bool m_show_countdown;
    int m_timeout;
    QString m_text;
    QTimer m_timer;
};

#endif // SELFDESTRUCTMESSAGEBOX_H
