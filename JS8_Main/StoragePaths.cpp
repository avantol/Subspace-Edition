#include "StoragePaths.h"

#include <QCoreApplication>

namespace {

// RAII helper that temporarily swaps QCoreApplication::applicationName
// and restores it on scope exit. QStandardPaths consults
// applicationName when building its app-suffixed paths, so this gives
// us a deterministic way to resolve "JS8Call" paths regardless of
// what name the binary advertises for display.
class AppNamePin {
    QString saved_;

  public:
    explicit AppNamePin(QString const &name)
        : saved_(QCoreApplication::applicationName()) {
        QCoreApplication::setApplicationName(name);
    }
    ~AppNamePin() { QCoreApplication::setApplicationName(saved_); }
    AppNamePin(AppNamePin const &) = delete;
    AppNamePin &operator=(AppNamePin const &) = delete;
};

} // namespace

// [oneinstance TODO #226] Every path below pins to
// pathApplicationName(), NOT to the bare literal "JS8Call".
//
// The literal was the Build 157 mistake. It normalized the display
// brand ("Subspace Edition" -> "JS8Call"), which was the whole point,
// but it ALSO discarded the multi-instance part of the name that
// main.cpp appends for --rig-name / --test-mode. Upstream JS8Call
// (2.2.0 and Improved 3.0.2 alike) derives every per-instance path
// and filename from applicationName() and nothing else, so pinning
// to a constant collapsed every instance into one directory: two
// stations sharing one ALL.TXT, one DIRECTED.TXT, one inbox.
//
// pathApplicationName() substitutes ONLY the brand and preserves the
// suffix, which is exactly what the header has documented all along.
// Default instance -> "JS8Call" (unchanged, Build 154 continuity
// intact); --rig-name arq -> "JS8Call - arq", matching upstream.

QString StoragePaths::dataLocation() {
    AppNamePin pin{pathApplicationName()};
    return QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
}

QString StoragePaths::configLocation() {
    AppNamePin pin{pathApplicationName()};
    return QStandardPaths::writableLocation(
        QStandardPaths::AppConfigLocation);
}

QString StoragePaths::locateConfig(QString const &fileName) {
    AppNamePin pin{pathApplicationName()};
    return QStandardPaths::locate(QStandardPaths::AppConfigLocation,
                                  fileName);
}

QString StoragePaths::settingsDirectory() {
    AppNamePin pin{pathApplicationName()};
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
}

// [oneinstance TODO #226] ONE authority for the settings file path.
// Both MultiSettings (which owns the file) and the early-startup
// diagnostic-log emitter in main.cpp (which must read
// DiagnosticLogging before any Configuration object exists) resolve
// it through here, so they cannot drift apart.
QString StoragePaths::settingsFileName() {
    return settingsDirectory() + QLatin1Char('/') + pathApplicationName() +
           QStringLiteral(".ini");
}

QString StoragePaths::pathApplicationName() {
    QString name = QCoreApplication::applicationName();
    name.replace(QStringLiteral("Subspace Edition"),
                 QStringLiteral("JS8Call"));
    return name;
}
