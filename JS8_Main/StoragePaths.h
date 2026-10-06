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

// [#287 2026-10-02] ONE authority for the two per-instance databases
// that sit beside the settings file. The grid bank:
// settingsDirectory()/<pathApplicationName()>-grids.db (the name
// SpotMapWindow always used). The routing corpus the log miner
// writes and auto-route reads: settingsDirectory()/js8reach-intel.db
// for the default instance (its historical name, so no re-mine on
// upgrade) and js8reach-intel - <rig>.db for a --rig-name instance.
// Before this the miner hard-coded the default instance's paths, so
// a second instance mined the first one's logs and both wrote one
// corpus.
QString gridsDbPath();
QString reachIntelDbPath();

// [#288] The multi-instance suffix alone: "" for the default
// instance, " - <rig>" for --rig-name <rig>, " - test" for test mode.
// For names that must keep their historical default-instance form and
// gain the suffix only when there is one (the corpus, the diag log).
QString instanceSuffix();

// [#288] This instance's private scratch directory,
// <TempLocation>/<pathApplicationName()> -- the one main() creates and
// Configuration::temp_dir() serves. For code that has no Configuration
// at hand (dialogs). Shared TempLocation paths with a fixed name are
// NOT per-instance: two instances would write the same file.
QString instanceTempDir();

// applicationName() with the "Subspace Edition" display brand
// substituted back to "JS8Call". Multi-instance rig/test suffixes
// are preserved. Use this whenever the runtime brand is being baked
// into a path or filename so file routing stays on the historical
// names.
QString pathApplicationName();

} // namespace StoragePaths

#endif // STORAGE_PATHS_H
