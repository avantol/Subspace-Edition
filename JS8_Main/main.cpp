/**
 * @file main.cpp
 * @brief Implementation of the MessageTimeStamper class
 */

#include "CrashHandler.h"

#include <cstring>
#include "DriftingDateTime.h"
#include "FrequencyList.h"
#include "JS8MessageBox.h"
#include "JS8_Include/SettingsGroup.h"
#include "JS8_Include/commons.h"
#include "JS8_UI/mainwindow.h"
#include "MetaDataRegistry.h"
#include "MultiSettings.h"
#include "Radio.h"
#include "StoragePathMigration.h"
#include "StoragePaths.h"
#include "TraceFile.h"
#include "revision_utils.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QLibraryInfo>
#include <QLockFile>
#include <QLoggingCategory>
#include <QObject>
#include <QRegularExpression>
#include <QSettings>
#include <QStack>
#include <QStandardPaths>
#include <QStringList>
#include <QSysInfo>
#include <QSysInfo>
#include <fftw3.h>
#include <locale.h>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#if QT_VERSION >= 0x050200
#include <QCommandLineOption>
#include <QCommandLineParser>
#endif

Q_DECLARE_LOGGING_CATEGORY(main_js8)

namespace {
class MessageTimestamper {
  public:
    MessageTimestamper() {
        prior_handlers_.push(qInstallMessageHandler(message_handler));
    }
    ~MessageTimestamper() {
        if (prior_handlers_.size())
            qInstallMessageHandler(prior_handlers_.pop());
    }

  private:
    static void message_handler(QtMsgType type,
                                QMessageLogContext const &context,
                                QString const &msg) {
        // Write timestamped message directly to stderr.
        // This replaces Qt's default handler to ensure 2> redirect
        // works on all platforms (Windows OutputDebugString issue).
        fprintf(stderr, "%s%s\n",
                qPrintable(DriftingDateTime::currentDateTimeUtc().toString(
                    "yy-MM-ddTHH:mm:ss.zzzZ: ")),
                qPrintable(msg));
    }
    static QStack<QtMessageHandler> prior_handlers_;
};
QStack<QtMessageHandler> MessageTimestamper::prior_handlers_;
} // namespace

int main(int argc, char *argv[]) {
    // Install crash handler before anything else — catches unhandled
    // exceptions and writes a mini-dump for analysis.
    installCrashHandler();

    // --crash-test=<kind> deliberately crashes the process via the named
    // mechanism so each handler hook can be verified end-to-end. Parsed
    // here (before QApplication / QCommandLineParser) so the crash fires
    // before any other init can mask it.
    for (int i = 1; i < argc; ++i) {
        char const *eq = nullptr;
        if (strncmp(argv[i], "--crash-test=", 13) == 0) {
            eq = argv[i] + 13;
        } else if (strcmp(argv[i], "--crash-test") == 0 && i + 1 < argc) {
            eq = argv[i + 1];
        }
        if (eq) {
            crashTest(eq);
            return 1;  // crashTest is supposed to terminate; if it didn't, bail
        }
    }

    // Add timestamps to all debug messages
    MessageTimestamper message_timestamper;

    // make the Qt type magic happen
    Radio::register_types();
    register_types();

    QApplication a(argc, argv);

    // MSIX / Microsoft Store packaging: when the app is activated via
    // appxmanifest entry point (Start Menu, etc.), the loader sets the
    // process CWD to %WINDIR%\System32, not to the install dir. That
    // breaks any relative-path lookup done by Qt plugin discovery,
    // Hamlib config, FFTW wisdom cache, Boost::filesystem defaults, or
    // any QFile/QSettings opened with a non-absolute path. Pin CWD to
    // the executable's directory so those lookups behave as if launched
    // directly from the install dir. Harmless on traditional installers
    // (CWD was usually the install dir already) and on Linux/macOS.
    QDir::setCurrent(QCoreApplication::applicationDirPath());

    try {
        setlocale(LC_NUMERIC, "C"); // ensure number forms are in
                                    // consistent format, do this after
                                    // instantiating QApplication so
                                    // that GUI has correct l18n

        // Override programs executable basename as application name.
        a.setApplicationName("Subspace Edition");
        a.setApplicationVersion(version());

        // One-shot migration of user data from the historic
        // "Subspace Edition" AppLocalDataLocation directory back to
        // the canonical "JS8Call" directory shared with older builds.
        StoragePathMigration::run();

#if QT_VERSION >= 0x050200
        QCommandLineParser parser;
        parser.setApplicationDescription("\n" PROJECT_SUMMARY_DESCRIPTION);
        auto help_option = parser.addHelpOption();
        auto version_option = parser.addVersionOption();

        QCommandLineOption output_option(QStringList() << "o" << "output",
                                         "Write debug statements into <file>.",
                                         "file");
        parser.addOption(output_option);

        // support for multiple instances running from a single installation
        QCommandLineOption rig_option(
            QStringList{} << "r" << "rig-name",
            a.translate("main",
                        "Where <rig-name> is for multi-instance support."),
            a.translate("main", "rig-name"));
        parser.addOption(rig_option);

        // support for start up configuration
        QCommandLineOption cfg_option(
            QStringList{} << "c" << "config",
            a.translate("main", "Where <configuration> is an existing one."),
            a.translate("main", "configuration"));
        parser.addOption(cfg_option);

        QCommandLineOption test_option(
            QStringList{} << "test-mode",
            a.translate("main", "Writable files in test location.  Use with "
                                "caution, for testing only."));
        parser.addOption(test_option);

        if (!parser.parse(a.arguments())) {
            std::cerr << parser.errorText().toLocal8Bit().data() << std::endl;
            return -1;
        } else {
            if (parser.isSet(help_option)) {
                parser.showHelp(-1);
                return 0;
            } else if (parser.isSet(version_option)) {
                parser.showVersion();
                return 0;
            }
        }

        QStandardPaths::setTestModeEnabled(parser.isSet(test_option));

        // support for multiple instances running from a single installation
        bool multiple{false};
        if (parser.isSet(rig_option) || parser.isSet(test_option)) {
            auto temp_name = parser.value(rig_option);
            if (!temp_name.isEmpty()) {
                if (temp_name.contains(QRegularExpression{R"([\\/,])"})) {
                    std::cerr << "Invalid rig name - \\ & / not allowed"
                              << std::endl;
                    parser.showHelp(-1);
                }

                a.setApplicationName(a.applicationName() + " - " + temp_name);
            }

            if (parser.isSet(test_option)) {
                a.setApplicationName(a.applicationName() + " - test");
            }

            multiple = true;
        }

        // [oneinstance TODO #226] The instance is fully identified at
        // this point, and applicationName is the ONLY thing that
        // carries it -- exactly as upstream does. Everything
        // downstream (data directory, settings file, lock file,
        // grid DB, trace log, crash directory) resolves through
        // StoragePaths::pathApplicationName(), which strips the
        // display brand and keeps the instance part. The Build 370
        // parallel suffix mechanism is gone.
        //
        // NO MIGRATION of the files builds 370-478 wrote under that
        // other format ("JS8Call-arq.ini") -- operator ruling
        // 2026-09-10. A --rig-name instance starts clean on the
        // upstream names; the old files are simply left on disk.
        // The DEFAULT instance is untouched either way.

        // Diagnostic logging is set up HERE, not before the instance
        // is known: a named instance must read ITS OWN ini and write
        // its diag log beside it. It also has to follow
        // setTestModeEnabled above, which moves every path.
        if (parser.isSet(output_option)) {
            new TraceFile(parser.value(output_option));
        } else {
            // Read DiagnosticLogging straight from QSettings; no
            // Configuration object exists yet.
            QSettings settings(StoragePaths::settingsFileName(),
                               QSettings::IniFormat);
            settings.beginGroup("Configuration");
            bool diagEnabled = settings.value("DiagnosticLogging", false).toBool();
            settings.endGroup();
            if (diagEnabled) {
                auto timestamp = QDateTime::currentDateTimeUtc()
                    .toString("yyyyMMdd_HHmmss");
                auto logPath = StoragePaths::settingsDirectory() +
                               "/js8call-diag-" + timestamp + "Z.log";
                new TraceFile(logPath);
                qWarning() << "[DIAG] Diagnostic logging enabled:" << logPath;
            }
        }

        qWarning() << "[DIAG] Build:" << program_title();
        qWarning() << "[DIAG] Instance:"
                   << StoragePaths::pathApplicationName()
                   << "data:" << StoragePaths::dataLocation();
        qWarning() << "[DIAG] OS:" << QSysInfo::prettyProductName()
                   << "kernel:" << QSysInfo::kernelType()
                   << QSysInfo::kernelVersion()
                   << "arch:" << QSysInfo::currentCpuArchitecture();

        // now we have the application name we can open the settings
        MultiSettings multi_settings{parser.value(cfg_option)};

        // find the temporary files path
        QDir temp_dir{
            QStandardPaths::writableLocation(QStandardPaths::TempLocation)};
        Q_ASSERT(temp_dir.exists()); // sanity check

        // disallow multiple instances with same instance key.
        // [oneinstance TODO #226] Derived from pathApplicationName(),
        // like every other per-instance name. The original concern
        // that motivated the hardcode -- "Subspace Edition.lock" --
        // is handled at the source: pathApplicationName() maps the
        // display brand back to "JS8Call", so the default instance is
        // still exactly "JS8Call.lock".
        QLockFile instance_lock{temp_dir.absoluteFilePath(
            StoragePaths::pathApplicationName() + ".lock")};
        instance_lock.setStaleLockTime(0);
        while (!instance_lock.tryLock()) {
            if (QLockFile::LockFailedError == instance_lock.error()) {
                switch (JS8MessageBox::query_message(
                    nullptr,
                    a.translate("main", "Another instance may be running"),
                    a.translate("main", "try to remove stale lock file?"),
                    QString{},
                    JS8MessageBox::Yes | JS8MessageBox::Retry |
                        JS8MessageBox::No,
                    JS8MessageBox::Yes)) {
                case JS8MessageBox::Yes:
                    instance_lock.removeStaleLockFile();
                    break;

                case JS8MessageBox::Retry:
                    break;

                default:
                    throw std::runtime_error{
                        "Multiple instances must have unique rig names"};
                }
            } else {
                throw std::runtime_error{"Failed to access lock file"};
            }
        }
#endif

#if WSJT_QDEBUG_TO_FILE
        // Open a trace file
        TraceFile trace_file{temp_dir.absoluteFilePath(
            StoragePaths::pathApplicationName() + "_trace.log")};
        qCDebug(main_js8) << program_title() + " - Program startup";
#endif

        // Create a unique writeable temporary directory in a suitable location
        bool temp_ok{false};
        QString unique_directory{StoragePaths::pathApplicationName()};
        do {
            if (!temp_dir.mkpath(unique_directory) ||
                !temp_dir.cd(unique_directory)) {
                JS8MessageBox::critical_message(
                    nullptr,
                    a.translate("main",
                                "Failed to create a temporary directory"),
                    a.translate("main", "Path: \"%1\"")
                        .arg(temp_dir.absolutePath()));
                throw std::runtime_error{
                    "Failed to create a temporary directory"};
            }
            if (!temp_dir.isReadable() ||
                !(temp_ok = QTemporaryFile{temp_dir.absoluteFilePath("test")}
                                .open())) {
                auto button = JS8MessageBox::critical_message(
                    nullptr,
                    a.translate(
                        "main",
                        "Failed to create a usable temporary directory"),
                    a.translate(
                        "main",
                        "Another application may be locking the directory"),
                    a.translate("main", "Path: \"%1\"")
                        .arg(temp_dir.absolutePath()),
                    JS8MessageBox::Retry | JS8MessageBox::Cancel);
                if (JS8MessageBox::Cancel == button) {
                    throw std::runtime_error{
                        "Failed to create a usable temporary directory"};
                }
                temp_dir.cdUp(); // revert to parent as this one is no good
            }
        } while (!temp_ok);

        int result;
        do {
#if WSJT_QDEBUG_TO_FILE
            // announce to trace file and dump settings
            qCDebug(main_js8) << "++++++++++++++++++++++++++++ Settings "
                                 "++++++++++++++++++++++++++++";
            for (auto const &key : multi_settings.settings()->allKeys()) {
                auto const &value = multi_settings.settings()->value(key);
                if (value.canConvert<QVariantList>()) {
                    auto const sequence = value.value<QSequentialIterable>();
                    qCDebug(main_js8).nospace() << key << ": ";
                    for (auto const &item : sequence) {
                        qCDebug(main_js8).nospace() << '\t' << item;
                    }
                } else {
                    qCDebug(main_js8).nospace() << key << ": " << value;
                }
            }
            qCDebug(main_js8) << "---------------------------- Settings "
                                 "----------------------------";
#endif

            // run the application UI
            UI_Constructor w(program_version(), temp_dir, multiple,
                             &multi_settings);
            w.show();
            result = a.exec();
        } while (!result && !multi_settings.exit());

        fftwf_forget_wisdom();
        fftwf_cleanup();

        temp_dir.removeRecursively(); // clean up temp files
        return result;
    } catch (std::exception const &e) {
        JS8MessageBox::critical_message(nullptr, "Fatal error", e.what());
        std::cerr << "Error: " << e.what() << '\n';
    } catch (...) {
        JS8MessageBox::critical_message(nullptr, "Unexpected fatal error");
        std::cerr << "Unexpected fatal error\n";
        throw; // hoping the runtime might tell us more about the exception
    }
    return -1;
}

Q_LOGGING_CATEGORY(main_js8, "main.js8", QtWarningMsg)
