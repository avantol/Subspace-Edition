#pragma once

#include <QDialog>
#include <QList>
#include <QString>
#include <QVariantMap>

class QButtonGroup;
class QLabel;
class QLineEdit;
class QListWidget;

// [operator 2026-10-16] Import a radio setup from WSJT-X.
//
// WSJT-X and JS8Call share their configuration code (MultiSettings,
// Configuration, TransceiverFactory): the radio, transmit, audio and
// calibration settings carry the SAME keys with values written the SAME
// way, so they are copied unchanged into a new configuration of ours
// (MultiSettings::create_from_current). Nothing else of WSJT-X's is read.
namespace WsjtxImport {

// One WSJT-X configuration found on this machine.
struct Source {
    QString file;       // the WSJT-X settings file
    QString config;     // its configuration name ("Default", "IC-7300", ...)
    bool current = false; // WSJT-X's active configuration in that file
    QVariantMap values; // the importable keys present (Configuration/ keys)
    QString summary;    // "Icom IC-7300, /dev/ttyUSB0, PTT CAT"
    bool hasRig = false; // a radio is set ("Rig" other than None)
};

// The Configuration/ keys copied: radio, transmit, audio, calibration.
QStringList importKeys();

// Every WSJT-X configuration in the settings files where WSJT-X keeps
// them: <ConfigLocation>/WSJT-X*.ini (Linux, macOS) and
// <LocalAppData>/WSJT-X*/WSJT-X*.ini (Windows; a named radio instance
// "WSJT-X - <rig>" has its own folder).
QList<Source> findConfigurations();

// The configurations of ONE settings file (Browse).
QList<Source> readFile(QString const &file);

// Advisory: a WSJT-X instance lock file (<temp>/WSJT-X*.lock) is held by a
// live process. A lock left by a crash can only cause a false warning.
bool wsjtxRunning();

class Dialog : public QDialog {
    Q_OBJECT
  public:
    Dialog(QList<Source> sources, bool jttyAvailable, QWidget *parent = nullptr);

    Source source() const;     // the configuration chosen
    bool forJtty() const;      // "Use it for: JTTY"
    QString name() const;      // the new configuration's name

  private:
    void addSources(QList<Source> const &more);
    void suggestName();

    QList<Source> m_sources;
    QListWidget *m_list = nullptr;
    QButtonGroup *m_use = nullptr;
    QLineEdit *m_name = nullptr;
    bool m_nameEdited = false;
};

} // namespace WsjtxImport
