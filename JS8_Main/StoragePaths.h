#ifndef STORAGE_PATHS_H
#define STORAGE_PATHS_H

#include <QStandardPaths>
#include <QString>

namespace StoragePaths {

// Resolve standard-location paths using the historical "JS8Call"
// applicationName regardless of the binary's display branding. This
// pins user data and configuration to their long-standing locations
// (~/.local/share/JS8Call, ~/.config/JS8Call, %APPDATA%/JS8Call) so
// rebranding the binary doesn't strand user data in a new directory.

// AppLocalDataLocation under "JS8Call" -- where data files live
// (ALL.TXT, DIRECTED.TXT, inbox.db3, js8call_log.adi, wisdom, save/).
QString dataLocation();

// AppConfigLocation under "JS8Call" -- the per-app config directory
// (currently used for hamlib_settings.json).
QString configLocation();

// Equivalent to QStandardPaths::locate(AppConfigLocation, fileName)
// with applicationName pinned to "JS8Call".
QString locateConfig(QString const &fileName);

// ConfigLocation pinned to "JS8Call" -- the directory the legacy
// JS8Call.ini sits in (and where diagnostic-log files belong).
// MultiSettings::settings_path() already does this internally for
// the .ini path; this exposes the parent directory for other
// callers (e.g. the early-startup diag-log emitter).
QString settingsDirectory();

// Full path of THIS instance's settings file:
// settingsDirectory()/<pathApplicationName()>.ini. The single
// authority -- MultiSettings and the early diagnostic-log emitter
// both resolve the .ini through this ([oneinstance] TODO #226).
QString settingsFileName();

// applicationName() with the "Subspace Edition" display brand
// substituted back to "JS8Call". Multi-instance rig/test suffixes
// are preserved. Use this whenever the runtime brand is being baked
// into a path or filename so file routing stays on the historical
// names.
QString pathApplicationName();

} // namespace StoragePaths

#endif // STORAGE_PATHS_H
