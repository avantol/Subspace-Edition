#ifndef REMOTEDIALOG_H
#define REMOTEDIALOG_H

/**
 * @file RemoteDialog.h
 * @brief File > Phone remote...: turn the phone remote on, its port,
 *        its pass code, and the addresses (with a QR code) to open on
 *        the phone. Settings are per profile, group SsRemote.
 */

#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;
class QSettings;
class QSpinBox;

class RemoteDialog : public QDialog {
    Q_OBJECT
  public:
    RemoteDialog(QSettings *settings, QWidget *parent = nullptr);

    // A fresh pass code: 8 characters, no look-alikes (0/O, 1/I/L).
    static QString newPasscode();

  private:
    void refresh();    // addresses + QR from the current fields
    void save();

    QSettings *m_settings;
    QCheckBox *m_enabled;
    QSpinBox *m_port;
    QLineEdit *m_passcode;
    QLabel *m_urls;
    QLabel *m_qr;
};

#endif // REMOTEDIALOG_H
