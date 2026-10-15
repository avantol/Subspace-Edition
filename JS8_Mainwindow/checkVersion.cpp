/** \file
 * @brief UI_Constructor::checkVersion — startup + Help-menu update check.
 *
 * Rewritten 2026-07-23 (was legacy upstream JS8Call code that had NEVER
 * worked in this fork: it fetched a `version.txt` release asset that is
 * not published, so the request 404'd and the function returned silently
 * — the dialog could never appear). Now:
 *
 *   - Reads the latest version from the GitHub RELEASES API `tag_name`,
 *     which the release creates automatically — no hand-published asset
 *     to forget (that omission is exactly what hid the old breakage).
 *   - Compares at BUILD granularity: version() is "4.1.0.<build>" and
 *     QVersionNumber compares all four components.
 *   - Notifies ONCE per new version (LastUpdateNotifiedVersion setting);
 *     the Help-menu manual check (alertOnUpToDate) is unconditional.
 *   - Splits by DELIVERY channel, detected at runtime (the Store build
 *     is an MSIX repackage of the SAME .exe, so only package identity
 *     can tell them apart). [2026-07-24 dcCheck] The MSIX/Store build
 *     checks the REAL Store version via the DisplayCatalog API (the one
 *     public endpoint that carries it — storeedgefd returns "Unknown")
 *     and, if the Store has a newer BUILD, nudges to the Store. It never
 *     contacts GitHub, and because it reads the ACTUAL published Store
 *     version it is immune to the GitHub-publish lag. The PLAIN build
 *     queries the GitHub Releases API. Both show up-to-date / new-version
 *     dialogs; failures degrade safely (Store page / "try later"), never
 *     a false nudge. Packaging-version mismatch is handled by comparing
 *     BUILD NUMBERS (exe 4.1.0.<build> 4th seg vs MSIX 4.0.<build>.0 3rd).
 *   - Logs failures VISIBLY (qWarning). The old code buried them in a
 *     qCDebug, which is why the breakage went unnoticed.
 */

#include "JS8_UI/mainwindow.h"

#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QRegularExpression>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QVersionNumber>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

// True when this process is running from an MSIX/APPX package (the
// Microsoft Store delivery). Resolved dynamically from kernel32 so it
// needs no import lib or appmodel.h on the MinGW toolchain, and is a
// no-op everywhere but Windows. GetCurrentPackageFullName returns
// APPMODEL_ERROR_NO_PACKAGE (15700) when unpackaged; any other result
// (typically ERROR_INSUFFICIENT_BUFFER, because a package name exists)
// means packaged.
bool runningAsAppx() {
#ifdef Q_OS_WIN
    if (auto *k32 = GetModuleHandleW(L"kernel32.dll")) {
        using Fn = LONG(WINAPI *)(UINT32 *, PWSTR);
        if (auto fn = reinterpret_cast<Fn>(
                GetProcAddress(k32, "GetCurrentPackageFullName"))) {
            UINT32 len = 0;
            LONG const rc = fn(&len, nullptr);
            constexpr LONG APPMODEL_ERROR_NO_PACKAGE_ = 15700;
            return rc != APPMODEL_ERROR_NO_PACKAGE_;
        }
    }
    return false;  // pre-Win8 GetProcAddress miss => treat as unpackaged
#else
    return false;
#endif
}

// Microsoft Store product page for Subspace Edition (product id
// 9nhk74bnkxkc). Opening it lands the user on the Store's Update button.
// Store apps auto-update ONLY while signed into the Store — which most
// users are not — so this nudge is the real update path for them, not a
// duplicate of a reliable background updater.
constexpr char kStoreUrl[] = "https://apps.microsoft.com/detail/9nhk74bnkxkc";
constexpr char kReleasesUrl[] =
    "https://github.com/avantol/Subspace-Edition/releases";

// [TODO #299, operator 2026-10-15] MAGNET stations (the persisted
// SuperSpotter latch, m_superSpotterSeen) take their updates ONLY from the
// release-only repository of approved MAGNET builds, on its own cadence:
// "approved" = published there. Its placeholder release carries the
// public release's tag (4.1.0.478) so nothing prompts until a real one.
constexpr char kMagnetReleasesUrl[] =
    "https://github.com/avantol/Subspace-Edition-Magnet/releases";
constexpr char kMagnetLatestApi[] =
    "https://api.github.com/repos/avantol/Subspace-Edition-Magnet/releases/latest";
constexpr char kPublicLatestApi[] =
    "https://api.github.com/repos/avantol/Subspace-Edition/releases/latest";

// [2026-07-24 dcCheck] Microsoft Store DisplayCatalog — the ONE public
// endpoint that carries the REAL Store package version (storeedgefd
// returns "Unknown"; the web page hides it). It returns the version in
// each PackageFullName, e.g.
//   "avantolapps.JS8CallSubspaceEdition_4.0.<build>.0_x64__<hash>"
// This is the version ACTUALLY in the Store, so it is immune to the
// GitHub-publish lag (the Store trails GitHub by the ~1 h listing
// process). Undocumented (reverse-engineered — ThomasPe/MS-Store-API)
// so treated best-effort: any parse/network failure degrades to "open
// the Store", never a false "update available" nudge.
constexpr char kStoreCatalogUrl[] =
    "https://displaycatalog.mp.microsoft.com/v7.0/products/9NHK74BNKXKC"
    "?market=US&languages=en-US&fieldsTemplate=details";

}  // namespace

void UI_Constructor::checkVersion(bool const alertOnUpToDate) {
    // Channel: MSIX/Store build vs plain build. Resolved once.
    // [updtest] JS8_UPDATE_CHANNEL=store|github forces it for testing
    // (default auto = runningAsAppx()); the choice is logged so a single
    // real appx launch confirms runningAsAppx() detection from the log.
    QByteArray const chOverride = qgetenv("JS8_UPDATE_CHANNEL");
    // [TODO #299] A MAGNET station never follows the Store or the public
    // releases: the Store would offer whatever public build is newest.
    bool const magnet = m_superSpotterSeen;
    bool const appx = magnet                     ? false
                      : (chOverride == "store")  ? true
                      : (chOverride == "github") ? false
                                                 : runningAsAppx();
    if (!chOverride.isEmpty()) {
        qWarning() << "[UPDATE-CHECK] channel FORCED by JS8_UPDATE_CHANNEL="
                   << chOverride << "-> appx=" << appx
                   << "(real runningAsAppx=" << runningAsAppx() << ")";
    }

    // [2026-07-24 dcCheck] APPX / Store build: check the REAL Store
    // version via DisplayCatalog (kStoreCatalogUrl) and, if the Store
    // has a newer build, nudge to the Store. Never contacts GitHub.
    // The Store version is what's actually published, so this is immune
    // to the GitHub-publish lag. PACKAGING MISMATCH STAYS (operator):
    // the running .exe is "4.1.0.<build>" (build = 4th segment) while
    // the Store MSIX is "4.0.<build>.0" (build = 3rd segment), so the
    // full versions are NOT directly comparable — we compare the BUILD
    // NUMBER, extracted from each by its slot.
    if (appx) {
        auto *m = new QNetworkAccessManager(this);
        connect(
            m, &QNetworkAccessManager::finished, this,
            [this, alertOnUpToDate](QNetworkReply *reply) {
                reply->manager()->deleteLater();
                reply->deleteLater();

                // Newest Store build from any PackageFullName:
                //   ...JS8CallSubspaceEdition_<a>.<b>.<build>.<rev>_...
                int storeBuild = -1;
                QString storeVer;
                if (!reply->error()) {
                    QString const body =
                        QString::fromUtf8(reply->readAll());
                    static QRegularExpression const re(QStringLiteral(
                        "JS8CallSubspaceEdition_(\\d+\\.\\d+\\.(\\d+)\\.\\d+)_"));
                    auto it = re.globalMatch(body);
                    while (it.hasNext()) {
                        auto const match = it.next();
                        int const b = match.captured(2).toInt();
                        if (b > storeBuild) {
                            storeBuild = b;
                            storeVer = match.captured(1);
                        }
                    }
                }

                // Failure: never a false nudge — offer the Store page.
                if (storeBuild < 0) {
                    qWarning()
                        << "[UPDATE-CHECK] store(DisplayCatalog) check "
                           "failed:"
                        << reply->errorString() << "http="
                        << reply
                               ->attribute(QNetworkRequest::
                                               HttpStatusCodeAttribute)
                               .toInt();
                    if (alertOnUpToDate) {
                        (new SelfDestructMessageBox(
                             30, tr("Update Check Failed"),
                             tr("Could not check the Microsoft Store "
                                "right now. <a href=\"%1\">Open the "
                                "Microsoft Store</a> to check for "
                                "updates.")
                                 .arg(QString::fromLatin1(kStoreUrl)),
                             QMessageBox::Warning, QMessageBox::Ok,
                             QMessageBox::Ok, false, this))
                            ->show();
                    }
                    return;
                }

                // Running build = 4th segment of "4.1.0.<build>".
                // JS8_FAKE_VERSION overrides for testing.
                QString const fakeVer =
                    qEnvironmentVariable("JS8_FAKE_VERSION");
                QString const curStr =
                    fakeVer.isEmpty() ? version() : fakeVer;
                auto const runParts = curStr.split('.');
                int const runBuild =
                    runParts.size() >= 4 ? runParts.at(3).toInt() : -1;
                if (!fakeVer.isEmpty())
                    qWarning() << "[UPDATE-CHECK] running version FAKED "
                                  "by JS8_FAKE_VERSION=" << fakeVer;

                qWarning() << "[UPDATE-CHECK] store(DisplayCatalog) "
                              "storeVer=" << storeVer
                           << "storeBuild=" << storeBuild
                           << "runBuild=" << runBuild
                           << "newer=" << (runBuild < storeBuild);

                if (runBuild >= 0 && runBuild < storeBuild) {
                    // [everyprompt 2026-08-19] Once-per-build
                    // suppression REMOVED (operator): the new-version
                    // nudge shows at EVERY startup while the Store
                    // has a newer build. Manual check unchanged.
                    (new SelfDestructMessageBox(
                         60, tr("New Version Available"),
                         tr("A new version of Subspace Edition is "
                            "available. <a href=\"%1\">%2</a>")
                             .arg(QString::fromLatin1(kStoreUrl),
                                  tr("Update now.")),
                         QMessageBox::Information, QMessageBox::Ok,
                         QMessageBox::Ok, false, this))
                        ->show();
                } else if (alertOnUpToDate) {
                    (new SelfDestructMessageBox(
                         60, tr("No Updates Available"),
                         tr("Your version (%1) of Subspace Edition is "
                            "up-to-date.")
                             .arg(version()),
                         QMessageBox::Information, QMessageBox::Ok,
                         QMessageBox::Ok, false, this))
                        ->show();
                }
            });

        qWarning() << "[UPDATE-CHECK] querying Microsoft Store "
                      "DisplayCatalog for 9NHK74BNKXKC";
        QNetworkRequest r(QUrl(QString::fromLatin1(kStoreCatalogUrl)));
        r.setRawHeader("MS-CV", "0");
        r.setHeader(QNetworkRequest::UserAgentHeader,
                    QStringLiteral("Subspace-Edition/%1").arg(version()));
        m->get(r);
        return;
    }

    // --- Plain build: GitHub Releases API version check ---
    // (a MAGNET station: the MAGNET release-only repository instead)
    QString const releasesUrl = QString::fromLatin1(magnet ? kMagnetReleasesUrl
                                                           : kReleasesUrl);
    QString const edition = magnet ? tr("Subspace Edition for MAGNET")
                                   : tr("Subspace Edition");
    auto *m = new QNetworkAccessManager(this);
    connect(
        m, &QNetworkAccessManager::finished, this,
        [this, alertOnUpToDate, releasesUrl, edition](QNetworkReply *reply) {
            reply->manager()->deleteLater();
            reply->deleteLater();

            QString latestStr, htmlUrl;
            if (!reply->error()) {
                {
                    auto const obj =
                        QJsonDocument::fromJson(reply->readAll()).object();
                    QString tag =
                        obj.value(QStringLiteral("tag_name")).toString().trimmed();
                    bool const isDraft = obj.value(QStringLiteral("draft")).toBool();
                    bool const isPre =
                        obj.value(QStringLiteral("prerelease")).toBool();
                    htmlUrl = obj.value(QStringLiteral("html_url")).toString();
                    if (!tag.isEmpty() && !isDraft && !isPre) {
                        if (tag.startsWith('v') || tag.startsWith('V'))
                            tag = tag.mid(1);
                        latestStr = tag;
                    }
                }
            }

            // Failure = network error OR unparseable/no usable version.
            if (latestStr.isEmpty()) {
                // VISIBLE, not qCDebug — a silent failure here is what hid
                // the old breakage for the life of the fork.
                qWarning()
                    << "[UPDATE-CHECK] github check failed:"
                    << reply->errorString() << "http="
                    << reply
                           ->attribute(
                               QNetworkRequest::HttpStatusCodeAttribute)
                           .toInt();
                if (alertOnUpToDate) {
                    (new SelfDestructMessageBox(
                         30, tr("Update Check Failed"),
                         tr("Could not check for a new version right now. "
                            "Please try again later."),
                         QMessageBox::Warning, QMessageBox::Ok,
                         QMessageBox::Ok, false, this))
                        ->show();
                }
                return;
            }

            // [updtest] JS8_FAKE_VERSION overrides the running version so
            // the up-to-date vs new-version branches (and the Store link)
            // are both reachable on a normal build.
            QString const fakeVer = qEnvironmentVariable("JS8_FAKE_VERSION");
            QString const curStr = fakeVer.isEmpty() ? version() : fakeVer;
            if (!fakeVer.isEmpty())
                qWarning() << "[UPDATE-CHECK] running version FAKED by "
                              "JS8_FAKE_VERSION=" << fakeVer
                           << "(real=" << version() << ")";
            auto const currentVersion = QVersionNumber::fromString(curStr);
            auto const latestVersion = QVersionNumber::fromString(latestStr);

            qWarning() << "[UPDATE-CHECK] github current=" << currentVersion
                       << "latest=" << latestVersion
                       << "newer=" << (currentVersion < latestVersion);

            if (currentVersion < latestVersion) {
                QString const normStr = latestVersion.toString();

                // [everyprompt 2026-08-19] Once-per-version
                // suppression REMOVED (operator): the new-version
                // nudge shows at EVERY startup while a newer release
                // exists. Manual check unchanged.
                Q_UNUSED(normStr);

                QString const link = htmlUrl.isEmpty() ? releasesUrl : htmlUrl;
                (new SelfDestructMessageBox(
                     60, tr("New Version Available"),
                     tr("A new version of %1 is available. "
                        "<a href=\"%2\">%3</a>")
                         .arg(edition, link, tr("More information here.")),
                     QMessageBox::Information, QMessageBox::Ok,
                     QMessageBox::Ok, false, this))
                    ->show();
            } else if (alertOnUpToDate) {
                // Plain build: up-to-date still offers the release page
                // for general info + change log (GitHub is this channel's
                // home). The appx up-to-date dialog stays GitHub-free.
                QString const relLink = htmlUrl.isEmpty() ? releasesUrl : htmlUrl;
                (new SelfDestructMessageBox(
                     60, tr("No Updates Available"),
                     tr("Your version (%1) of %2 is "
                        "up-to-date.<br>General information and change "
                        "log is <a href=\"%3\">here</a>.")
                         .arg(version(), edition, relLink),
                     QMessageBox::Information, QMessageBox::Ok,
                     QMessageBox::Ok, false, this))
                    ->show();
            }
        });

    qWarning() << "[UPDATE-CHECK] querying GitHub releases API"
               << (magnet ? "(MAGNET repository)" : "(public repository)");
    QNetworkRequest r(QUrl(QString::fromLatin1(magnet ? kMagnetLatestApi
                                                      : kPublicLatestApi)));
    // GitHub rejects requests with NO Accept / No User-Agent (403).
    r.setRawHeader("Accept", "application/vnd.github+json");
    r.setHeader(QNetworkRequest::UserAgentHeader,
                QStringLiteral("Subspace-Edition/%1").arg(version()));
    m->get(r);
}
