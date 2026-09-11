/**
 * @file SpotMapWindow.cpp
 * @brief "Spots Map" implementation. See header for the design story.
 */

#include "SpotMapWindow.h"

#include "JS8_Include/SettingsGroup.h"
#include "JS8_Include/commons.h"
#include "JS8_Main/Bands.h"
#include "JS8_Main/DriftingDateTime.h"
#include "JS8_Main/Geodesic.h"
#include "JS8_Network/MqttClient.h"
#include "JS8_Main/MultiSettings.h"
#include "JS8_Main/StoragePaths.h"
#include <QFileInfo>
#include "JS8_UI/Configuration.h"

#include "SpotMapGeoData.h"

#include <QCloseEvent>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QApplication>
#include <QSet>
#include <QSettings>
#include <QLabel>
#include <QToolButton>
#include <QFrame>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QButtonGroup>
#include <QMessageBox>
#include <QRadioButton>
#include <QSpinBox>
#include <QToolTip>

#include "JS8_Include/Maidenhead.h"
#include <QTimer>

#include <algorithm>
#include <cmath>

Q_DECLARE_LOGGING_CATEGORY(mqttclient_js8)

namespace {
constexpr double DEG2RAD = 0.017453292519943295;
constexpr int TITLE_STRIP_PX = 24;
constexpr int LEGEND_STRIP_PX = 40;
// [pskrbusy 2026-09-01] PSKR congestion scale: 10 = peak 5-minute
// MQTT spot count observed in recent diag logs (1121, busy session
// 2026-08-31 02:44Z) + the operator's 30% margin.
constexpr int PSKR_BUSY_CEILING_5MIN = 1457;
constexpr qint64 PSKR_BUSY_WINDOW_MS = 5 * 60 * 1000;
constexpr int MARGIN_PX = 18;
constexpr int DOT_RADIUS_PX = 3;
constexpr float DEFAULT_SCALE_KM = 5000.0f;
// [ladder] Closest zoom. Lowered 500 -> 150 km (operator 2026-08-21:
// "add 2 more levels of zoom in... like 200 miles. 100 miles").
// 100 mi is 161 km, so the floor has to reach ~150; the rungs were
// always in the ladder, the old floor just clamped them away. NOTE
// the ladder is multiplicative, so opening it this far necessarily
// exposes the intermediate rungs (400/250/200) as well — two extra
// levels is not expressible without a second, additive ladder.
// CAVEAT at this range: Geodesic treats anything under CLOSE=120 km
// as co-located and a 4-char grid is ~100 km across, so the closest
// rungs show more resolution than the position data actually has.
constexpr float FLOOR_SCALE_KM = 150.0f;
// Manual-zoom range: floor shared with auto-scale; ceiling just past
// the antipode (~20000 km) so one more "−" from any auto scale always
// shows the whole reachable Earth.
// Ceiling lowered 20000 -> 15000 km (operator 2026-08-21: delete
// the widest rung) - it existed only to clear the antipode, and at
// that scale the view is mostly empty ocean for any realistic spot
// density. TRADE-OFF, stated: a near-antipodal path (>15000 km,
// e.g. VK/ZL long path) can no longer be framed whole; 15000 km
// still covers every path we have actually worked.
constexpr float MAX_SCALE_KM = 15000.0f;
// [ladder] THE zoom ladder (audit item 6 — niceCeil and stepScale
// each owned a copy; one authority now). Values per decade; a step
// past either end shifts the decade.
constexpr float kLadderMults[] = {1.0f, 1.25f, 1.5f, 2.0f, 2.5f,
                                  3.0f, 4.0f,  5.0f, 6.0f, 8.0f};

QString const kMqttHost = QStringLiteral("mqtt.pskreporter.info");
constexpr quint16 kMqttPort = 1883;
} // namespace

SpotMapWindow::SpotMapWindow(QSettings *settings,
                             Configuration const *config, QWidget *parent)
    : QWidget{parent}, m_settings{settings}, m_config{config},
      m_mqtt{new MqttClient{this}} {
    setWindowFlag(Qt::Window);
    setWindowTitle(tr("Spots Map"));
    setMinimumSize(320, 320);
    setMouseTracking(true); // hover tooltips on spots

    // [attemptviz] Animates the dash gap while a call is outstanding.
    // Started on demand by noteAttempt and stopped by tickAttempts, so
    // a map with nothing in flight never repaints on its account.
    m_attemptTimer = new QTimer{this};
    m_attemptTimer->setInterval(500);
    connect(m_attemptTimer, &QTimer::timeout, this,
            &SpotMapWindow::tickAttempts);

    // [#164] Open the persistent grid store and SEED the in-session
    // authority before any feed can run. File sits beside the
    // settings ini, honoring the per-instance suffix ([multiinst]).
    {
        QString const dir =
            QFileInfo{m_settings->fileName()}.absolutePath();
        if (m_gridDb.open(dir + QLatin1Char('/') +
                          StoragePaths::pathApplicationName() +
                          QStringLiteral("-grids.db"))) {
            m_gridByCall = m_gridDb.loadAll();
            // [#168 part 3] Restore happens in setStation(), not
            // here: m_myGrid is unknown at ctor time (bearings) and
            // the first setStation() used to clear everything.
        } else {
            // [sqlerr, operator 2026-08-30] A silent open failure
            // masqueraded as an empty band (no grid bank, no habit
            // journal, no reach events). Say it ONCE, at startup,
            // with the class-specific remedy. Deferred to the event
            // loop: the constructor runs before any window shows.
            QString const text =
                m_gridDb.openFailure() ==
                        GridDb::OpenFailure::DriverMissing
                    ? tr("Grid database driver missing -- the "
                         "installation is incomplete (sqldrivers\\"
                         "qsqlite.dll or libsqlite3-0.dll not "
                         "found/loadable). Grids, habit history "
                         "and relay events will not persist.")
                    : tr("Grid database could not be opened (%1). "
                         "Check file permissions beside the "
                         "settings file. Grids, habit history and "
                         "relay events will not persist.")
                          .arg(m_gridDb.openErrorText());
            QTimer::singleShot(0, this, [this, text]() {
                QMessageBox::warning(this, tr("Grid database"),
                                     text);
            });
        }
    }
    // [#168 part 3] Write-behind: RAM answers every query, the queue
    // drains to disk in one transaction on this timer. 45 s bounds
    // what a crash can cost to a window of band observations we can
    // simply re-hear.
    m_dbFlushTimer.setInterval(45 * 1000);
    connect(&m_dbFlushTimer, &QTimer::timeout, this,
            [this] { m_gridDb.flush(); });
    m_dbFlushTimer.start();

    {
        SettingsGroup g{m_settings, "SpotMap"};
        restoreGeometry(
            m_settings->value("geometry", saveGeometry()).toByteArray());
        m_restoreVisible = m_settings->value("WindowVisible", false).toBool();
        m_showCallsigns = m_settings->value("ShowCallsigns", false).toBool();
        // [persistui 2026-08-15] Connections overlay, map type, and
        // PSKR toggle persist across sessions AND across hide/show.
        m_showConnections =
            m_settings->value("ShowConnections", false).toBool();
        m_viewAll = m_settings->value("ViewAll", false).toBool();
        m_showPskr = m_settings->value("ShowPskr", true).toBool();
        // [persistui] Zoom level (0 = Auto) and spots time range.
        // [audit 2026-08-21] VALIDATE persisted values. Both are
        // free-form ints/floats in the ini and both are used directly;
        // a value the current build cannot express silently breaks the
        // UI while the controls look normal.
        //   ManualScaleKm: the ladder bounds moved this session
        //   (FLOOR 500->150, MAX 20000->15000), so a value banked by
        //   an older build can now be out of range.
        //   ViewWindowSecs: must be one of the button values, or the
        //   reflection below checks 60m while the filter uses the
        //   stale number -- indistinguishable from "the age selection
        //   does nothing". An experimental build with other buttons
        //   is exactly how such a value gets written.
        m_manualScaleKm =
            m_settings->value("ManualScaleKm", 0.0f).toFloat();
        if (m_manualScaleKm > 0.0f)
            m_manualScaleKm = std::clamp(m_manualScaleKm,
                                         FLOOR_SCALE_KM, MAX_SCALE_KM);
        else
            m_manualScaleKm = 0.0f; // negatives/NaN -> Auto
        m_viewWindowSecs =
            m_settings->value("ViewWindowSecs", DEFAULT_VIEW_SECS)
                .toInt();
        if (m_viewWindowSecs == 5 * 60)      // [#209] migrate 5m
            m_viewWindowSecs = 450;
        if (m_viewWindowSecs != 450 && m_viewWindowSecs != 15 * 60 &&
            m_viewWindowSecs != 30 * 60 && m_viewWindowSecs != 60 * 60) {
            qCWarning(mqttclient_js8)
                << "[SPOTMAP] discarding out-of-set ViewWindowSecs"
                << m_viewWindowSecs << "-> " << DEFAULT_VIEW_SECS;
            m_viewWindowSecs = DEFAULT_VIEW_SECS;
        }
        m_panPx = QPointF{m_settings->value("PanX", 0.0).toDouble(),
                          m_settings->value("PanY", 0.0).toDouble()};
    }

    m_replotTimer.setSingleShot(true);
    m_replotTimer.setInterval(150);
    connect(&m_replotTimer, &QTimer::timeout, this, &SpotMapWindow::redraw);

    m_pruneTimer.setInterval(30 * 1000);
    connect(&m_pruneTimer, &QTimer::timeout, this, &SpotMapWindow::onPruneTick);
    m_pruneTimer.start();

    connect(m_mqtt, &MqttClient::messageReceived,
            this, &SpotMapWindow::onMqttMessage);
    connect(m_mqtt, &MqttClient::stateChanged,
            this, &SpotMapWindow::onMqttState);

    m_mqtt->setServer(kMqttHost, kMqttPort);
    // Station (and therefore topics + start) is seeded by the main
    // window via setStation() right after construction — the client
    // runs from app launch, independent of window visibility.

    // Zoom controls: small vertical stack in the upper-left, below
    // the title strip. Plain child widgets (no layout — the chart is
    // one custom-painted surface), fixed positions.
    auto const makeZoomButton = [this](QString const &text) {
        auto *b = new QToolButton(this);
        b->setText(text);
        b->setFixedSize(36, 20);
        QFont f = b->font();
        f.setPointSize(8);
        b->setFont(f);
        b->setStyleSheet(QStringLiteral(
            "QToolButton { background-color: rgba(40,40,55,200);"
            " color: rgb(210,210,225); border: 1px solid rgb(70,70,90);"
            " border-radius: 3px; }"
            "QToolButton:hover { background-color: rgba(60,60,80,220); }"
            "QToolButton:checked { background-color: rgba(80,100,150,230);"
            " color: white; border-color: rgb(120,140,190); }"
            "QToolButton:disabled { color: rgb(120,120,140); }"));
        return b;
    };
    m_zoomInBtn = makeZoomButton(QStringLiteral("+"));
    m_zoomAutoBtn = makeZoomButton(tr("Auto"));
    m_zoomOutBtn = makeZoomButton(QStringLiteral("−")); // minus sign
    // +/− glyphs bold and 50% larger (8 → 12 pt) for legibility;
    // "Auto" keeps the small face (operator, 2026-08-03).
    for (QToolButton *b : {m_zoomInBtn, m_zoomOutBtn}) {
        QFont f = b->font();
        f.setPointSize(12);
        f.setBold(true);
        b->setFont(f);
    }
    int const zx = 6;
    int y = TITLE_STRIP_PX + 6;
    for (QToolButton *b : {m_zoomInBtn, m_zoomAutoBtn, m_zoomOutBtn}) {
        b->move(zx, y);
        y += 22;
    }
    m_zoomInBtn->setToolTip(tr("Zoom in"));
    m_zoomAutoBtn->setToolTip(tr("Auto zoom (fit all spots)"));
    m_zoomOutBtn->setToolTip(tr("Zoom out"));

    // [spotwin] View-window buttons, upper right. Checkable +
    // auto-exclusive (siblings); selection persists ([persistui]).
    // Storage always keeps the full hour; these only filter the
    // display.
    m_win5Btn = makeZoomButton(QStringLiteral("7.5m")); // [#209]
    m_win15Btn = makeZoomButton(QStringLiteral("15m"));
    m_win30Btn = makeZoomButton(QStringLiteral("30m"));
    m_win60Btn = makeZoomButton(QStringLiteral("60m"));
    struct WinDef { QToolButton *b; int secs; };
    for (WinDef const wd : {WinDef{m_win5Btn, 450}, // [#209] 7.5m: spans PSKR batch cycles (see #205)
                            WinDef{m_win15Btn, 15 * 60},
                            WinDef{m_win30Btn, 30 * 60},
                            WinDef{m_win60Btn, 60 * 60}}) {
        wd.b->setCheckable(true);
        wd.b->setAutoExclusive(true);
        wd.b->setToolTip(tr("Show spots from the last %1 minutes")
                             .arg(QString::number(wd.secs / 60.0)));
        connect(wd.b, &QToolButton::clicked, this, [this, wd]() {
            m_viewWindowSecs = wd.secs;
            qCWarning(mqttclient_js8)
                << "[SPOTMAP][ui] window" << wd.secs; // [uilog]
            requestReplot();
        });
    }
    // [persistui] Reflect the persisted time range (default 60m).
    (m_viewWindowSecs == 450       ? m_win5Btn
     : m_viewWindowSecs == 15 * 60 ? m_win15Btn
     : m_viewWindowSecs == 30 * 60 ? m_win30Btn
                                   : m_win60Btn)
        ->setChecked(true);

    // [viewall] View-selector buttons, lower-left vertical stack: my
    // call on top (who hears ME), "All" below (every station on the
    // band). Exclusive via QButtonGroup — autoExclusive would merge
    // them into the win5/15/30/60 sibling group. View persists
    // ([persistui]).
    m_viewMineBtn = makeZoomButton(tr("Me")); // real call set in setStation
    m_viewAllBtn = makeZoomButton(tr("All"));
    auto *viewGroup = new QButtonGroup(this);
    viewGroup->setExclusive(true);
    for (QToolButton *b : {m_viewMineBtn, m_viewAllBtn}) {
        b->setCheckable(true);
        viewGroup->addButton(b);
    }
    (m_viewAll ? m_viewAllBtn : m_viewMineBtn)->setChecked(true); // [persistui]
    m_viewMineBtn->setToolTip(tr("Show stations spotting MY signal"));
    m_viewAllBtn->setToolTip(
        tr("Show all spotted stations on this band (all reporters)"));
    // [viewpan 2026-08-16] A view switch in Auto clears the user
    // pan: the drag framing belonged to the OTHER dataset, and added
    // to the fresh auto-fit it shifts the new view off-window —
    // field: "failed to re-scale" once, unreproducible (needed a
    // prior drag). Manual zoom keeps pan as deliberate framing.
    auto const viewSwitchPan = [this]() {
        if (m_manualScaleKm <= 0.0f) {
            m_panPx = QPointF{};
            m_lastAutoScaleKm = 0.0f; // [fitdamp] new dataset = fresh fit
        }
    };
    connect(m_viewMineBtn, &QToolButton::clicked, this,
            [this, viewSwitchPan]() {
        // [relaysel] Choosing my view exits relay-select entirely
        // (mode + button state + selections; operator 2026-08-14).
        if (m_relaySelBtn && m_relaySelBtn->isChecked())
            m_relaySelBtn->setChecked(false); // toggled() clears path
        if (!m_viewAll) return;
        m_viewAll = false;
        viewSwitchPan();
        qCWarning(mqttclient_js8) << "[SPOTMAP][ui] view mine"; // [uilog]
        requestReplot();
    });
    connect(m_viewAllBtn, &QToolButton::clicked, this,
            [this, viewSwitchPan]() {
        if (m_viewAll) return;
        m_viewAll = true;
        viewSwitchPan();
        qCWarning(mqttclient_js8) << "[SPOTMAP][ui] view all"; // [uilog]
        requestReplot();
    });

    // [connlines] Connections toggle, lower right — dark-yellow
    // PSKR lines + blue on-air mesh; state persists ([persistui]).
    // [pskrtoggle] Internet-sourced spots are OPT-OUT: the button
    // sits below the view stack as its own function group.
    m_pskrBtn = makeZoomButton(tr("Add PSKReporter spots"));
    m_pskrBtn->setCheckable(true);
    m_pskrBtn->setChecked(m_showPskr); // [persistui]
    m_pskrBtn->setAutoExclusive(false);
    m_pskrBtn->setToolTip(
        tr("Add PSKReporter (internet-sourced) spots to your on-air "
           "radio spots"));
    connect(m_pskrBtn, &QToolButton::toggled, this, [this](bool on) {
        m_showPskr = on;
        qCWarning(mqttclient_js8) << "[SPOTMAP][ui] pskr" << on; // [uilog]
        requestReplot();
    });

    m_connBtn = makeZoomButton(tr("Show connections"));
    m_connBtn->setCheckable(true);
    m_connBtn->setChecked(m_showConnections); // [persistui]
    m_connBtn->setToolTip(
        tr("Draw lines between stations hearing each other"));
    connect(m_connBtn, &QToolButton::toggled, this, [this](bool on) {
        m_showConnections = on;
        requestReplot();
    });
    // [callsbtn] On-map callsign labels, both views (operator
    // 2026-08-15). Sits directly above View connections.
    m_callsBtn = makeZoomButton(tr("Show call signs"));
    m_callsBtn->setCheckable(true);
    m_callsBtn->setChecked(m_showCallsigns);
    m_callsBtn->setAutoExclusive(false);
    m_callsBtn->setToolTip(
        tr("Show each station's call sign next to its dot"));
    connect(m_callsBtn, &QToolButton::toggled, this, [this](bool on) {
        m_showCallsigns = on;
        requestReplot();
    });

    // [relaysel] Relay-path builder rows above Connections:
    //   [Select relay(s)]
    //   [Done] [Undo]
    //   [Connections]
    // Click-to-append while the toggle is on; Done emits the
    // template; Undo pops one hop; untoggling clears everything.
    m_relaySelBtn = makeZoomButton(tr("Select relay(s)"));
    m_relaySelBtn->setCheckable(true);
    m_relaySelBtn->setToolTip(
        tr("Build a relay path: click at least two stations, in "
           "outbound order."));
    m_relayDoneBtn = makeZoomButton(tr("Done"));
    m_relayDoneBtn->setToolTip(
        tr("Put the relay message template in the outgoing box"));
    m_relayUndoBtn = makeZoomButton(tr("Undo"));
    m_relayUndoBtn->setToolTip(tr("Remove the last selected station"));
    connect(m_relaySelBtn, &QToolButton::toggled, this, [this](bool on) {
        m_relaySelect = on;
        // [operator 2026-09-01] Options is unavailable while the
        // relay builder owns the map's click semantics.
        if (m_optionsBtn)
            m_optionsBtn->setEnabled(!on);
        if (on && !m_viewAll) {
            // [relaysel] Relay hops are picked from ALL heard
            // stations — selecting flips the map to the All view
            // (operator revision 2026-08-14).
            m_viewAll = true;
            m_viewAllBtn->setChecked(true);
        }
        if (on)
            showToast(
                tr("Click to select relay stations, in outbound order"));
        if (!on) {
            m_relayPath.clear(); // disable = discard selections
            m_relayPathSpots.clear();
        }
        updateRelayButtons();
        requestReplot();
    });
    connect(m_relayUndoBtn, &QToolButton::clicked, this, [this]() {
        if (!m_relayPath.isEmpty()) {
            m_relayPath.removeLast();
            m_relayPathSpots.removeLast(); // [relaykeep]
        }
        showRelayPathToast();
        updateRelayButtons();
        requestReplot();
    });
    connect(m_relayDoneBtn, &QToolButton::clicked, this, [this]() {
        if (m_relayPath.isEmpty())
            return;
        // [operator 2026-08-30] Same gate as auto-route Start: a
        // busy radio is a WAIT, not a failure. Toast which kind and
        // keep the selected path so Done can simply be clicked again.
        if (m_txBusyProbe) {
            if (QString const busy = m_txBusyProbe();
                !busy.isEmpty()) {
                showToast(busy);
                return;
            }
        }
        // "HOP1>HOP2>DEST [MESSAGE]" — plain directed relay text, no
        // ARQ wrapping on any hop (operator directive 2026-08-14).
        QString const tpl = m_relayPath.join(QLatin1Char('>')) +
                            QStringLiteral(" [MESSAGE]");
        Q_EMIT relayTemplateReady(tpl);
        showToast(tr("Relay template ready — type your message"));
        m_relaySelBtn->setChecked(false); // clears path via toggled()
    });
    updateRelayButtons();

    // [autoroute 2026-08-28] One button, three states:
    //   "Auto-route" unchecked  -> click arms the mode (prompt shows)
    //   checked, no target yet  -> waiting for typed/clicked target
    //   "Halt auto-route"       -> executor running; click cancels
    // Disabled while an ARQ session is active (pushed by the
    // mainwindow); arming cancels relay-select the same way clicking
    // its button would.
    m_autoRouteBtn = makeZoomButton(tr("Auto-route"));
    m_autoRouteBtn->setCheckable(true);
    // Start/Cancel for the target prompt: SAME type and style as the
    // map buttons (operator, 2026-08-28). Created here because the
    // style lambda is constructor-local; the prompt panel adopts
    // them into its layout on first show.
    m_autoRouteStartBtn = makeZoomButton(tr("Start"));
    m_autoRouteStartBtn->setEnabled(false);
    m_autoRouteStartBtn->hide();
    m_autoRouteCancelBtn = makeZoomButton(tr("Cancel"));
    m_autoRouteCancelBtn->hide();
    m_autoRouteBtn->setToolTip(
        tr("Automatically find a relay route to a station or grid"));
    connect(m_autoRouteBtn, &QToolButton::toggled, this, [this](bool on) {
        if (on) {
            if (m_relaySelBtn->isChecked())
                m_relaySelBtn->setChecked(false);
            m_relaySelBtn->setEnabled(false);
            m_autoRouteArmed = true;
            autoRouteShowPanel();
            return;
        }
        // Unchecked. Programmatic teardown (autoRouteEnded) clears
        // the flags first, so reaching here with a flag still set
        // means the OPERATOR clicked to cancel.
        if (m_autoRouteActive) {
            Q_EMIT autoRouteHalt(); // mainwindow stops the executor,
                                    // then calls autoRouteEnded(true)
        } else if (m_autoRouteArmed) {
            m_autoRouteArmed = false;
            if (m_autoRoutePanel)
                m_autoRoutePanel->hide();
            m_relaySelBtn->setEnabled(true);
        }
    });

    // [#207 waitopts] Options button, below Auto-route: opens the
    // response-wait dialog. Close button created here because the
    // style lambda is constructor-local (same as Start/Cancel).
    m_optionsBtn = makeZoomButton(tr("Options"));
    m_optionsBtn->setToolTip(tr("Auto-route response wait settings"));
    m_optionsCloseBtn = makeZoomButton(tr("Close"));
    m_optionsCloseBtn->hide();
    connect(m_optionsBtn, &QToolButton::clicked, this, [this]() {
        if (m_optionsPanel && m_optionsPanel->isVisible())
            m_optionsPanel->hide();
        else
            optionsShowPanel();
    });
    // [operator 2026-09-01] The Options dialog closes when either
    // mode button is clicked -- their own panels take the spot.
    for (QToolButton *b : {m_autoRouteBtn, m_relaySelBtn})
        connect(b, &QToolButton::clicked, this, [this]() {
            if (m_optionsPanel)
                m_optionsPanel->hide();
        });

    positionWindowButtons();
    // [autochk 2026-08-16] Auto shows as SELECTED while auto zoom is
    // in effect — same checked styling as every other toggle, not
    // disabled-looking (operator). Always clickable: re-fit/recenter.
    m_zoomAutoBtn->setCheckable(true);
    m_zoomAutoBtn->setAutoExclusive(false);
    m_zoomAutoBtn->setChecked(m_manualScaleKm <= 0.0f);
    connect(m_zoomInBtn, &QToolButton::clicked,
            this, &SpotMapWindow::zoomIn);
    connect(m_zoomAutoBtn, &QToolButton::clicked,
            this, &SpotMapWindow::zoomAuto);
    connect(m_zoomOutBtn, &QToolButton::clicked,
            this, &SpotMapWindow::zoomOut);
}

SpotMapWindow::~SpotMapWindow() = default;

bool SpotMapWindow::wasVisibleAtShutdown() const { return m_restoreVisible; }

void SpotMapWindow::saveSettings() {
    SettingsGroup g{m_settings, "SpotMap"};
    m_settings->setValue("geometry", saveGeometry());
    // [visrace] The tracked flag, NEVER live isVisible(): on quit,
    // Qt can close this window before the main window's shutdown
    // save runs — sampling visibility here recorded false and the
    // map failed to reopen at startup (field 2026-08-15, several
    // occurrences; order-dependent race).
    m_settings->setValue("WindowVisible", m_restoreVisible);
    m_settings->setValue("ShowCallsigns", m_showCallsigns);
    m_settings->setValue("ShowConnections", m_showConnections); // [persistui]
    m_settings->setValue("ViewAll", m_viewAll);
    m_settings->setValue("ShowPskr", m_showPskr);
    m_settings->setValue("ManualScaleKm", m_manualScaleKm); // [persistui]
    m_settings->setValue("ViewWindowSecs", m_viewWindowSecs);
    m_settings->setValue("PanX", m_panPx.x()); // [persistui] pan
    m_settings->setValue("PanY", m_panPx.y());
}

// -------------------------------------------------------------------------
// Station / band plumbing
// -------------------------------------------------------------------------

void SpotMapWindow::rebuildTopics() {
    if (m_myCall.isEmpty()) {
        m_mqtt->stop();
        return;
    }
    // The broker encodes '/' in callsigns as '.' at the topic level
    // — verified live 2026-07-15 with WM8Q/P: topic level "WM8Q.P",
    // payload sc "WM8Q/P". Subscribe on the EXACT call, dot-encoded;
    // the payload filter then exact-matches sc against m_myCall.
    QString topicCall = m_myCall.toUpper();
    topicCall.replace(QLatin1Char('/'), QLatin1Char('.'));

    // pskr/filter/v2/{band}/{mode}/{sender}/{receiver}/{senderLoc}/...
    // All bands (caches fill in the background), mode JS8 (spotters
    // report our transmissions with the "JS8" mode string).
    // [viewall] Subscribe to ALL JS8 senders (superset of the old
    // exact-call topic) so the "All" view accumulates from launch
    // like the my-view always has. Volume is modest — JS8-mode spots
    // only, latest-per-sender storage — and the my-view dataset is
    // still gated by the exact payload sc match below, unchanged.
    QString topic = QStringLiteral("pskr/filter/v2/+/JS8/#");

    // Documented debug hook: flood-filter override for protocol
    // bring-up without transmitting, e.g.
    //   JS8_SPOTMAP_TOPIC_OVERRIDE=pskr/filter/v2/20m/FT8/#
    if (QString const ov =
            qEnvironmentVariable("JS8_SPOTMAP_TOPIC_OVERRIDE");
        !ov.isEmpty()) {
        qCWarning(mqttclient_js8) << "topic override active:" << ov;
        topic = ov;
    }

    // [spotfmt] Accumulation clock: restarts whenever the
    // subscription (re)starts — the title's "last X of Y min" is
    // honest about how much history can possibly be on screen.
    m_accumStart = DriftingDateTime::currentDateTimeUtc();
    m_mqtt->setClientIdPrefix(QStringLiteral("JS8Call_%1").arg(topicCall));
    m_mqtt->setTopics({topic});
    m_pskrArrivals.clear(); // [pskrbusy] new band = new count
    m_debugDumpsLeft = 20;
    m_mqtt->start();
}

// [pskrbusy 2026-09-01, operator spec] Internet twin of the on-air
// congestion index: 1-10 = MQTT spots on the current band in the
// trailing 5 minutes, scaled so 10 = the peak 5-minute count in the
// recent diag logs (1121, busy session 2026-08-31 02:44Z) + the
// operator's 30% margin = 1457.
int SpotMapWindow::pskrCongestionIndex() const {
    qint64 const cutMs =
        QDateTime::currentMSecsSinceEpoch() - PSKR_BUSY_WINDOW_MS;
    int n = 0;
    for (qint64 const t : m_pskrArrivals)
        if (t >= cutMs)
            ++n;
    return qBound(1, (n * 10 + PSKR_BUSY_CEILING_5MIN - 1) /
                         PSKR_BUSY_CEILING_5MIN,
                  10);
}

// [pskrbusy] Available = at least one spot arrived within the
// metric's own 5-minute window. Covers internet loss, broker loss,
// and an up-but-dead feed alike (the 2026-08-23 outage mode) --
// with zero data points in the window the index is meaningless
// whatever the socket says.
bool SpotMapWindow::pskrDataAvailable() const {
    return !m_pskrArrivals.isEmpty() &&
           m_pskrArrivals.last() >=
               QDateTime::currentMSecsSinceEpoch() -
                   PSKR_BUSY_WINDOW_MS;
}

void SpotMapWindow::setStation(QString const &callsign, QString const &grid) {
    // Compare NORMALISED, or a raw-vs-trimmed mismatch re-runs the
    // whole station-change path (clearing every store) on every call.
    if (callsign.trimmed() == m_myCall && grid.trimmed() == m_myGrid)
        return;
    // [#168 part 3] The FIRST set is startup, not a station CHANGE.
    // Treating them alike silently destroyed everything restored from
    // disk: the ctor loaded the mesh, logged that it had, and then
    // this ran moments later and cleared it -- so the log proved the
    // LOAD while the data never survived to be used (operator caught
    // the symptom: "on-air and PSKR spots that didn't show upon
    // restart"). Restoring HERE also fixes a second defect: bearings
    // are derived from m_myGrid, which the ctor does not yet know, so
    // a ctor-time restore would have placed every dot at distance 0.
    bool const firstSet = m_myCall.isEmpty();
    // [callcase 2026-08-22] NORMALISE AT THE ONE DOOR. Callers pass
    // m_config.my_callsign() raw; every OTHER consumer in the codebase
    // trims it, this one did not. An untrimmed call makes myUp match
    // nothing, so `heard.contains(myUp)` and `ed.key() != myUp` both
    // fail -- the mesh registers no stations in the MY view and draws
    // no lines, while the SPOT store (which never compares against
    // myUp) still paints the dots. Symptom: "spots circles but no
    // lines for them" (operator). Same shape as [gridcase]: normalise
    // where the value enters the authority, not at each use.
    m_myCall = callsign.trimmed();
    m_myGrid = grid.trimmed();
    if (firstSet) {
        restoreMeshFromDisk();
        restoreStationsFromDisk();
    } else {
        // A genuine move: azimuth/distance and subscription are both
        // stale, so start over. Banked rows keep RAW grids, so the
        // next restore recomputes correctly against the new one.
        qCWarning(mqttclient_js8)
            << "[SPOTMAP] station changed — ALL spot data cleared:"
            << callsign << grid; // [showfix] attribute real data loss
        m_infoByBand.clear();
        m_hearingByBand.clear();  // [hearlines] az/dist keyed to grid
    }
    positionWindowButtons();  // [viewall] my-call button text/width
    rebuildTopics();
    requestReplot();
}

void SpotMapWindow::setDialFrequency(qint64 const hz) {
    if (hz == m_dialHz)
        return;   // guiUpdate pushes this once a second; no churn
    m_dialHz = hz;
    // [passband #218] A dial change now changes what is DRAWN -- the
    // passband verdict reads m_dialHz at paint time -- so it must
    // request a paint, exactly like an incoming report does. Before
    // the filter the dial only fed hover text and the double-click,
    // and no repaint was needed. Without this the map re-judged the
    // dots only when the next report happened to arrive: on-air
    // stations within seconds, PSKR-only stations when the next batch
    // landed 60-90 s later, at whatever dial the operator had reached
    // by then. Field 2026-09-11: dots vanished on a click up and did
    // not return on the click down -- "the map seems really random".
    requestReplot();
}

// [passband #218 evidence] One observation into the station's set.
void SpotMapWindow::noteFreq(QString const &band, QString const &call,
                             qint64 const hz, QDateTime const &when,
                             bool const radio, bool const tx) {
    if (band.isEmpty() || call.isEmpty() || hz <= 0 || !when.isValid())
        return;
    auto &seen = m_infoByBand[band][call.toUpper()].freqSeen;
    auto const now = DriftingDateTime::currentDateTimeUtc();
    // Prune on the way in, so the set never grows past one window's
    // worth of distinct frequencies. A skimmer's worst case is one
    // entry per distinct offset it reported in the last hour.
    seen.erase(std::remove_if(seen.begin(), seen.end(),
                              [&](StationInfo::FreqSeen const &f) {
                                  return f.when.secsTo(now) >=
                                         JS8_FREQ_STALE_SECS;
                              }),
               seen.end());
    for (auto &f : seen) {
        if (f.hz == hz) {
            if (when > f.when)
                f.when = when;   // forward-only
            f.radio = f.radio || radio;
            f.tx = f.tx || tx;
            return;
        }
    }
    seen.append(StationInfo::FreqSeen{hz, when, radio, tx});
}

// [passband #218] See the header for the three-way contract. Every
// ruling from TODO_notes.md item-218 is a line here, in order.
SpotMapWindow::Passband
SpotMapWindow::passbandVerdict(QString const &band, QString const &call,
                               int const staleSecs, bool const includeRx,
                               bool const pskrAllowed) const {
    // Our own dial unknown (no rig control, or CAT not read yet):
    // the filter disables itself entirely, or it would reject
    // everything.
    if (m_dialHz <= 0)
        return Passband::Unknown;
    auto const bandIt = m_infoByBand.constFind(band);
    if (bandIt == m_infoByBand.constEnd())
        return Passband::Unknown;
    auto const it = bandIt->constFind(call.toUpper());
    if (it == bandIt->constEnd())
        return Passband::Unknown;
    auto const now = DriftingDateTime::currentDateTimeUtc();
    bool anyFresh = false;
    for (auto const &f : it->freqSeen) {
        // Admissibility: the caller's freshness window (past it a
        // frequency is UNKNOWN, not "last known" -- stations QSY, and
        // a stale number must never reject a station we can hear
        // perfectly well); listening-only evidence only if the
        // caller wants it; internet evidence only if allowed.
        if (f.when.secsTo(now) >= staleSecs)
            continue;
        if (!f.tx && !includeRx)
            continue;
        if (!f.radio && !pskrAllowed)
            continue;
        anyFresh = true;
        // Boundaries inclusive: offset 0 and offset WIDTH are both in.
        qint64 const audio = f.hz - m_dialHz;
        if (audio >= 0 && audio <= JS8_PASSBAND_WIDTH_HZ)
            return Passband::In;   // any one hit is enough
    }
    return anyFresh ? Passband::Out : Passband::Unknown;
}

void SpotMapWindow::setBand(QString const &band) {
    if (band == m_currentBand)
        return;
    m_currentBand = band;
    // [relaykeep] A relay path is per-band by nature — band change
    // clears the selections AND the mode/button (operator,
    // 2026-08-14). setChecked(false) routes through toggled(), which
    // clears path + snapshots.
    if (m_relaySelBtn && m_relaySelBtn->isChecked())
        m_relaySelBtn->setChecked(false);
    requestReplot();
}

// -------------------------------------------------------------------------
// MQTT ingest
// -------------------------------------------------------------------------

// [onairspot] See header — my-view spots from on-air evidence.
void SpotMapWindow::addOnAirSpotOfMe(QString const &band,
                                     QString const &call,
                                     QString const &grid, int const snr,
                                     qint64 const callRfHz) {
    // [oneobs 2026-08-22] A frame addressed to us IS an observation:
    // "call heard WM8Q". Record it as one, and the dot for `call` plus
    // the line to my triangle both fall out of that single record. The
    // old path built a Spot in a parallel store, which is how a dot
    // could refresh while its line went stale.
    if (band.isEmpty() || call.isEmpty() || grid.size() < 4 ||
        m_myGrid.size() < 4 || m_myCall.isEmpty())
        return;
    m_infoByBand[band][call.toUpper()].sawAsSender = true;
    addHearingReport(band, call, grid, {m_myCall.toUpper()}, {m_myGrid},
                     /*reportedToMeSnr=*/snr, QDateTime{},
                     /*heardSnr=*/snr, QStringLiteral("radio"),
                     /*hearerRfHz=*/callRfHz);
    journalStation(band, call);   // [maptruth #11] AFTER the update
}

// [hearlines] See header. Latest position per station; edges keyed
// per heard call with their own timestamps, so a conversation with a
// new partner never evicts an older edge (it just ages out).
void SpotMapWindow::addHearingReport(QString const &band,
                                     QString const &hearer,
                                     QString const &hearerGrid,
                                     QStringList const &heardCalls,
                                     QStringList const &heardGrids,
                                     int const reportedToMeSnr,
                                     QDateTime const &heardWhen,
                                     int const heardSnr,
                                     QString const &source,
                                     qint64 const hearerRfHz) {
    if (band.isEmpty() || hearer.isEmpty())
        return;
    auto const now = DriftingDateTime::currentDateTimeUtc();

    // [passband #218] Record the hearer's own transmit frequency when
    // the caller knew it first-hand. Radio evidence OUTRANKS PSKR: a
    // station we just decoded is provably inside our passband at that
    // instant, so a radio observation always overwrites, while a PSKR
    // one never overwrites a radio one that is still current. The
    // freshness test uses the SAME 60-minute window the filter does,
    // so "still current" means one thing in one place.
    if (hearerRfHz > 0) {
        StationInfo &si = m_infoByBand[band][hearer.toUpper()];
        si.freqHz = hearerRfHz;       // display copy (newest transmit)
        si.freqWhen = now;
        si.freqFromRadio = true;
        noteFreq(band, hearer, hearerRfHz, now, /*radio=*/true,
                 /*tx=*/true);        // the verdict's evidence
    }
    auto const resolve = [&](QString const &grid, float *az, float *dist) {
        *az = 0.0f;
        *dist = -1.0f;
        if (grid.size() < 4 || m_myGrid.size() < 4)
            return;
        if (auto const v = Geodesic::vector(m_myGrid, grid);
            v.azimuth().isValid() && v.distance().isValid()) {
            *az = v.azimuth();
            *dist = v.distance();
        }
    };
    bool const isNew = !m_hearingByBand[band].contains(hearer.toUpper());
    // [gridtrim] Frame grid fields arrive space-prefixed (" EM38",
    // field 2026-08-15 W0MQD) — trim before resolving.
    // [gridfine] Feed then refine: the authority holds the most
    // precise grid seen from ANY source (also serves as the
    // empty-grid fallback).
    rememberGrid(hearer, hearerGrid.trimmed());
    QString const hearerGridT =
        refinedGrid(hearer, hearerGrid.trimmed());
    HearingEntry &e = m_hearingByBand[band][hearer.toUpper()];
    // [maptruth #4] The presence clock takes the OBSERVATION time,
    // not the arrival time -- a 38-minute-old PSKReporter report was
    // stamping LAST_SEEN as now. Forward-only.
    {
        QDateTime const obs = heardWhen.isValid() ? heardWhen : now;
        if (!e.lastSeen.isValid() || obs > e.lastSeen)
            e.lastSeen = obs;
    }
    // [placelog] Attribute every FIRST placement of an on-air station
    // (field 2026-08-15: W0MQD appeared at a grid no logged message
    // supplied — source unproven; this line convicts the next one).
    if (isNew && !hearerGridT.isEmpty())
        qCWarning(mqttclient_js8)
            << "[SPOTMAP] on-air station placed:" << hearer
            << "grid=" << hearerGridT << "band=" << band;
    // [movers 2026-08-15] Update on ANY grid change, not only when
    // unplaced — a /P station that moves squares must move on the
    // map (audit item 1: entries froze at their first position).
    // refinedGrid already returns the canonical best form, so
    // same-square precision can only improve here, never thrash.
    if (!hearerGridT.isEmpty())
        e.grid = hearerGridT;   // [#170(f)] grid only; position derived
    if (reportedToMeSnr > -99) {
        // [maptruth ROOT FIX] value and clock are ONE record.
        QDateTime const obs = heardWhen.isValid() ? heardWhen : now;
        if (!e.snrWhen.isValid() || obs >= e.snrWhen) {
            e.snr = reportedToMeSnr;
            e.snrWhen = obs;
        }
    }
    // [audit 2026-08-21] PRESENCE provenance. RF evidence is STICKY:
    // once a station has been heard on the air, later internet
    // reports must not demote it to "internet-only" and make it
    // vanish when the PSKR toggle goes off.
    {
        QString const evid =
            !source.isEmpty()
                ? source
                : (heardWhen.isValid() ? QStringLiteral("hearing")
                                       : QStringLiteral("radio"));
        if (e.source.isEmpty() ||
            (e.source == QStringLiteral("mqtt") &&
             evid != QStringLiteral("mqtt")))
            e.source = evid;
    }
    for (int i = 0; i < heardCalls.size(); ++i) {
        QString const call = heardCalls.at(i).toUpper();
        if (call.isEmpty() || call == hearer.toUpper())
            continue;
        HeardEdge &edge = e.heard[call];
        // [meshprobe 2026-08-22] What actually happens to an edge that
        // names US: does it move forward, or is it refused?
        if (!m_myCall.isEmpty() &&
            call == m_myCall.toUpper()) {
            static qint64 lastEdgeProbe = 0;
            qint64 const nowMs2 = now.toMSecsSinceEpoch();
            if (nowMs2 - lastEdgeProbe > 10000) {
                lastEdgeProbe = nowMs2;
                qCWarning(mqttclient_js8)
                    << "[MESHPROBE] edge ->me hearer=" << hearer
                    << "existing age_s="
                    << (edge.when.isValid() ? edge.when.secsTo(now) : -1)
                    << "incoming age_s="
                    << (heardWhen.isValid() ? heardWhen.secsTo(now) : -1)
                    << "src=" << source;
            }
        }
        // [#161 querycall] Backdated sightings never REGRESS a
        // fresher edge; ordinary feeds keep their refresh-to-now.
        QDateTime const sighting =
            heardWhen.isValid() ? heardWhen : now;
        if (!edge.when.isValid() || sighting > edge.when)
            edge.when = sighting;
        if (heardSnr > -99)
            edge.snr = heardSnr;
        // [tribblenet] Explicit source beats inference: the caller
        // knows. Default keeps the old behaviour for on-air paths.
        // [audit 2026-08-21] STICKY toward FIRST-HAND, never a
        // downgrade. Assigning unconditionally let an internet report
        // of a pair overwrite our own radio observation of it -- and
        // PSKR echoes our OWN spots back to us constantly, so a
        // radio edge could be demoted to "mqtt" seconds after we
        // earned it, hiding it from the blue mesh and from any
        // offline (#159) route.
        {
            QString const evid =
                !source.isEmpty()
                    ? source
                    : (heardWhen.isValid() ? QStringLiteral("hearing")
                                           : QStringLiteral("radio"));
            auto const rank = [](QString const &s) {
                return s == QStringLiteral("radio")     ? 0
                       : s == QStringLiteral("hearing") ? 1
                                                        : 2;
            };
            if (edge.source.isEmpty() ||
                rank(evid) <= rank(edge.source))
                edge.source = evid;
        }
        QString const gRaw =
            (i < heardGrids.size() ? heardGrids.at(i) : QString())
                .trimmed();
        rememberGrid(call, gRaw); // [gridfine] feed then refine
        QString const g = refinedGrid(call, gRaw);
        if (!g.isEmpty() || edge.grid.isEmpty())
            edge.grid = g;      // [#170(f)] grid only; position derived
        // [#168 part 3] Journal the edge the authority just accepted.
        // RAW GRIDS only -- az/dist are my-grid-relative and become
        // nonsense after a move (#154/#164 trap); bearings are
        // recomputed on load. A BACKDATED sighting is a QUERY CALL /
        // HEARING reply (#161), which is the ONLY way a station that
        // does not publish to PSKReporter ever reveals its ears --
        // worth distinguishing, per the operator.
        GridDb::EdgeRow row;
        row.band = band;
        row.hearer = hearer;
        row.heard = call;
        row.hearerGrid = e.grid;
        row.heardGrid = edge.grid;
        row.snr = edge.snr;
        row.when = edge.when.isValid()
                       ? edge.when.toSecsSinceEpoch()
                       : now.toSecsSinceEpoch();
        row.source = edge.source;
        m_gridDb.queueEdge(row);
    }
    // [#168 part 1] Presence, on every sighting (throttled inside).
    // [audit] -99, not reportedToMeSnr: that value is THEIR copy of
    // OUR signal, not ours of theirs.
    m_gridDb.noteActivity(hearer, -99);
    if (band == m_currentBand && isVisible() && m_showConnections)
        requestReplot();
}

// [oneobs 2026-08-22] Journal STATION FACTS for one call. The
// observations themselves are journalled as edges by
// addHearingReport(); this records only what is true of the station.
void SpotMapWindow::journalStation(QString const &band,
                                   QString const &call) {
    if (band.isEmpty() || call.isEmpty())
        return;
    QString const c = call.toUpper();
    auto const &info = m_infoByBand.value(band).value(c);
    auto const &hz = m_hearingByBand.value(band);
    GridDb::StationRow r;
    r.band = band;
    r.call = c;
    r.grid = m_gridByCall.value(c);
    r.country = info.country;
    r.freqHz = info.freqHz;
    // [freqclock] the frequency's own clock and source ride with it
    r.freqWhen = info.freqWhen.isValid() ? info.freqWhen.toSecsSinceEpoch()
                                         : 0;
    r.freqRadio = info.freqFromRadio;
    r.rxOnly = !info.sawAsSender;
    auto const now = DriftingDateTime::currentDateTimeUtc();
    r.anyWhen = now.toSecsSinceEpoch();
    // Their report of MY signal, if this station has one.
    if (auto const it = hz.constFind(c); it != hz.constEnd()) {
        r.snrToMe = it->snr;
        r.snrToMeWhen = it->snrWhen.isValid()
                            ? it->snrWhen.toSecsSinceEpoch() : 0;
        r.reportsMe = it->heard.contains(m_myCall.toUpper());
        if (it->source != QStringLiteral("mqtt"))
            r.radioWhen = r.anyWhen;
    }
    m_gridDb.queueStation(r);
    m_gridDb.noteActivity(c, -99);
}

void SpotMapWindow::restoreStationsFromDisk() {
    // [oneobs] Only STATION FACTS are restored here -- country, their
    // transmit frequency, whether they have been seen sending. The
    // observations themselves come back as edges in
    // restoreMeshFromDisk(), and every dot is derived from those.
    auto const rows = m_gridDb.loadStations(WINDOW_SECS);
    int restored = 0;
    for (GridDb::StationRow const &r : rows) {
        if (r.band.isEmpty() || r.call.isEmpty())
            continue;
        StationInfo &info = m_infoByBand[r.band][r.call.toUpper()];
        if (!r.country.isEmpty())
            info.country = r.country;
        // [passband #218 freqclock 2026-09-11] A restored frequency
        // now carries its own clock and source, so it is real
        // evidence again after a restart: the display copy is
        // restored, and ONE transmit entry is seeded into the
        // evidence set (noteFreq prunes anything past 60 min
        // itself). Before this the row had the value but no date,
        // the verdict could only say Unknown, and a station we
        // decoded five minutes before restarting came back drawn at a
        // dial it could not be heard on (KK6WVY at 7.0855, field).
        // Listening evidence (reporters) is not persisted; PSKR
        // refills it within a batch or two.
        if (r.freqHz > 0) {
            info.freqHz = r.freqHz;
            if (r.freqWhen > 0) {
                info.freqWhen =
                    QDateTime::fromSecsSinceEpoch(r.freqWhen, Qt::UTC);
                info.freqFromRadio = r.freqRadio;
                noteFreq(r.band, r.call, r.freqHz, info.freqWhen,
                         r.freqRadio, /*tx=*/true);
            }
        }
        if (!r.rxOnly)
            info.sawAsSender = true;
        // THEIR REPORT OF OUR SIGNAL. This was written to disk and
        // never read back -- a write-only column. It is the one value
        // that colours a dot ([snrwho]: a dB figure exists only as a
        // report of MY signal), so after every restart the whole map
        // painted in the no-report colour until fresh PSKR reports of
        // us trickled in. 131 rows held a real value while the live
        // store had none (operator, 2026-08-22: "we lost all color
        // coding for signal strength").
        //
        // Applied ONLY to a station the mesh restore already created.
        // Creating an entry here would put a dot on the map with no
        // observation behind it -- the hollow-circle defect again.
        // [snrpersist] value AND its date together, or neither --
        // an undated dB is the exact lie the display audit killed
        // ("age unknown" on every restored report).
        if (r.snrToMe > -99 && r.snrToMeWhen > 0) {
            auto &band = m_hearingByBand[r.band];
            auto const it = band.find(r.call.toUpper());
            if (it != band.end() && it->snr <= -99) {
                it->snr = r.snrToMe;
                it->snrWhen = QDateTime::fromSecsSinceEpoch(
                    r.snrToMeWhen, Qt::UTC);
            }
        }
        ++restored;
    }
    if (restored)
        qCWarning(mqttclient_js8)
            << "[SPOTMAP] restored" << restored << "station records";
}

// [#168 part 3] Rebuild the mesh banked by earlier sessions.
void SpotMapWindow::restoreMeshFromDisk() {
    // WINDOW_SECS, not RETAIN_SECS: the RAM store ages at
    // WINDOW_SECS and pruneBand() runs on every incoming report, so
    // anything older is deleted within seconds of startup. Loading a
    // day's worth only made spots appear and then visibly vanish
    // (operator, 2026-08-21: "PSKR spots started disappearing... in
    // the first 10 seconds"). The extra history stays ON DISK for
    // tooling that queries it directly.
    auto const rows = m_gridDb.loadEdges(WINDOW_SECS);
    if (rows.isEmpty())
        return;
    int restored = 0;
    for (GridDb::EdgeRow const &r : rows) {
        if (r.band.isEmpty() || r.hearer.isEmpty() || r.heard.isEmpty())
            continue;
        HearingEntry &e = m_hearingByBand[r.band][r.hearer];
        QDateTime const when =
            QDateTime::fromSecsSinceEpoch(r.when, Qt::UTC);
        if (!e.lastSeen.isValid() || when > e.lastSeen)
            e.lastSeen = when;
        if (e.grid.isEmpty() && !r.hearerGrid.isEmpty())
            e.grid = r.hearerGrid;
        // [audit] Same stickiness rule as the live path.
        if (e.source.isEmpty() ||
            (e.source == QStringLiteral("mqtt") &&
             !r.source.isEmpty() && r.source != QStringLiteral("mqtt")))
            e.source = r.source.isEmpty() ? QStringLiteral("radio")
                                          : r.source;
        HeardEdge &edge = e.heard[r.heard];
        // Freshest wins, exactly as the live path does -- a replay
        // must never drag an edge backwards.
        if (!edge.when.isValid() || when > edge.when) {
            edge.when = when;
            if (r.snr > -99)
                edge.snr = r.snr;
            // [tribblenet] PROVENANCE MUST SURVIVE THE ROUND TRIP.
            // Dropping it here made every restored edge draw BLUE --
            // an untagged edge fails the "mqtt" display filter, so a
            // map rebuilt from disk showed PSKR links as on-air ones
            // while live spots (correctly tagged) drew yellow. That is
            // exactly what the operator saw: wrong colours on the
            // initial draw, correct ones appearing afterwards.
            // It is banked in `edges.source` -- restore it.
            edge.source = r.source.isEmpty()
                              ? QStringLiteral("radio")
                              : r.source;
        }
        if (edge.grid.isEmpty() && !r.heardGrid.isEmpty())
            edge.grid = r.heardGrid;
        ++restored;
    }
    // [#170(f)] NO BEARING RESOLUTION HERE ANY MORE.
    //
    // This block existed because restored edges carried a grid but no
    // az/dist, so they drew nothing -- "faithfully restored and
    // completely invisible" (audit, 2026-08-21). With position derived
    // from m_gridByCall at paint, a restored edge is placeable the
    // moment the grid bank has its grid, and the bank is seeded from
    // the grids table before this runs.
    //
    // Checked against the live database before deleting: of 617
    // callsigns carrying a grid on an edge, ZERO were missing from the
    // grids table -- the bank is a strict superset (656 rows). So
    // nothing loses its position by trusting the authority instead.
    qCWarning(mqttclient_js8)
        << "[SPOTMAP] restored" << restored << "mesh edges from disk"
        << "across" << m_hearingByBand.size() << "band(s)";
}

// [#168 mapdump] See header. Everything here is already in RAM; we
// only serialize it. Read-only: no aging, no pruning, no side effects
// on what the map displays.
namespace {

// One place decides how this dump writes a time that was never set.
//
// A default-constructed QDateTime still answers toMSecsSinceEpoch(),
// and the number it gives back looks entirely real: on this machine
// 25200000, which is just the UTC-7 offset. Every raw timestamp here
// used to call it unguarded, so an absent time went out as a plausible
// 1970 date that no consumer could tell from a measurement. Reading
// the map over the API showed all 150 stations reporting the same
// "radio time", which is what put us onto it.
//
// The age field keeps its -1 convention rather than going null: a
// negative age is impossible, so -1 is already unambiguous, and
// tools/js8reach/live.py reads it with float(), which a null would
// break. The raw time has no such spare value -- every number is a
// valid instant -- so absence there has to be null.
//
// Emitting the time and its age together is the point: there is no way
// to write one of these without resolving whether the value is set.
void putTime(QVariantMap &m, QString const &key, QString const &ageKey,
             QDateTime const &t, QDateTime const &now) {
    m[key] = t.isValid() ? QVariant{t.toMSecsSinceEpoch()} : QVariant{};
    if (!ageKey.isEmpty())
        m[ageKey] = t.isValid() ? qint64(t.secsTo(now)) : qint64(-1);
}

} // namespace

// [reachport] Typed reads for the in-app executor; see header.
QVector<SpotMapWindow::HearerView>
SpotMapWindow::hearersOf(QString const &band, QString const &heard) const {
    QVector<HearerView> out;
    QString const H = heard.toUpper();
    auto const &hb = m_hearingByBand.value(band);
    for (auto it = hb.constBegin(); it != hb.constEnd(); ++it) {
        auto const he = it.value().heard.constFind(H);
        if (he == it.value().heard.constEnd())
            continue;
        HearerView v;
        v.hearer = it.key();
        v.grid = he.value().grid.isEmpty()
                     ? m_gridByCall.value(it.key())
                     : he.value().grid;
        v.whenMs = he.value().when.isValid()
                       ? he.value().when.toMSecsSinceEpoch() : 0;
        v.snr = he.value().snr;
        v.source = he.value().source;
        out.append(v);
    }
    return out;
}

QVector<SpotMapWindow::StationView>
SpotMapWindow::activeStations(QString const &band) const {
    QVector<StationView> out;
    QString const me = m_myCall.toUpper();
    auto const &hb = m_hearingByBand.value(band);
    auto const &info = m_infoByBand.value(band);
    // [heardproof] heard-in-any-edge = transmit evidence.
    QSet<QString> heardSet;
    for (auto h0 = hb.constBegin(); h0 != hb.constEnd(); ++h0)
        for (auto e0 = h0.value().heard.constBegin();
             e0 != h0.value().heard.constEnd(); ++e0)
            heardSet.insert(e0.key());
    for (auto it = hb.constBegin(); it != hb.constEnd(); ++it) {
        StationView v;
        v.call = it.key();
        v.grid = m_gridByCall.value(it.key());
        v.lastSeenMs = it.value().lastSeen.isValid()
                           ? it.value().lastSeen.toMSecsSinceEpoch() : 0;
        v.snrToMe = it.value().snr;
        v.hearsMe = !me.isEmpty() && it.value().heard.contains(me);
        v.txAlive = info.value(it.key()).sawAsSender ||
                    heardSet.contains(it.key()) ||
                    it.value().source == QStringLiteral("radio");
        v.source = it.value().source;
        out.append(v);
    }
    return out;
}

QVector<SpotMapWindow::EdgeView>
SpotMapWindow::allEdges(QString const &band) const {
    QVector<EdgeView> out;
    auto const &hb = m_hearingByBand.value(band);
    for (auto it = hb.constBegin(); it != hb.constEnd(); ++it) {
        for (auto he = it.value().heard.constBegin();
             he != it.value().heard.constEnd(); ++he) {
            EdgeView v;
            v.hearer = it.key();
            v.heard = he.key();
            v.whenMs = he.value().when.isValid()
                           ? he.value().when.toMSecsSinceEpoch() : 0;
            v.snr = he.value().snr;
            v.source = he.value().source;
            out.append(v);
        }
    }
    return out;
}

QVector<GridDb::EdgeRow> SpotMapWindow::edges24h() const {
    return m_gridDb.loadEdges(24 * 3600);
}

QVariantMap SpotMapWindow::dumpState(QString const &band) const {
    QString const b = band.isEmpty() ? m_currentBand : band;
    auto const now = DriftingDateTime::currentDateTimeUtc();

    // Who is hearing whom, with the age of each observation. This is
    // the mesh an offline planner needs and cannot otherwise get.
    QVariantList hearing;
    for (auto it = m_hearingByBand.value(b).constBegin();
         it != m_hearingByBand.value(b).constEnd(); ++it) {
        HearingEntry const &e = it.value();
        QVariantMap h;
        h["CALL"] = it.key();
        h["GRID"] = e.grid;
        putTime(h, QStringLiteral("LAST_SEEN"), QStringLiteral("AGE_S"),
                e.lastSeen, now);
        h["SNR_TO_ME"] = e.snr;      // their report of OUR signal
        QVariantList heard;
        for (auto he = e.heard.constBegin(); he != e.heard.constEnd();
             ++he) {
            QVariantMap edge;
            edge["CALL"] = he.key();
            edge["GRID"] = he.value().grid;
            putTime(edge, QStringLiteral("WHEN"), QStringLiteral("AGE_S"),
                    he.value().when, now);
            edge["SNR"] = he.value().snr;   // third-party report
            // [tribblenet] PROVENANCE, so a consumer can tell an
            // over-the-air edge from an internet one -- the
            // difference between a relay we can actually reach
            // and one we only know about because PSKReporter
            // said so (operator audit, 2026-08-21).
            edge["SOURCE"] = he.value().source.isEmpty()
                                 ? QStringLiteral("radio")
                                 : he.value().source;
            heard.append(edge);
        }
        h["HEARS"] = heard;
        hearing.append(h);
    }

    // Spots, including internet-sourced ones: one entry per STATION,
    // saying what is on the air and where. Who hears whom is not here
    // -- that is HEARING above, which holds every pair rather than one
    // per station.
    auto const packSpots = [&now](QVector<Spot> const &src) {
        QVariantList out;
        for (Spot const &sp : src) {
            QVariantMap m;
            m["CALL"] = sp.receiverCall;
            m["GRID"] = sp.receiverGrid;
            putTime(m, QStringLiteral("WHEN"), QStringLiteral("AGE_S"),
                    sp.when, now);
            m["SNR"] = sp.snr;
            // [maptruth #5] the report's OWN age rides beside it.
            putTime(m, QStringLiteral("SNR_WHEN"),
                    QStringLiteral("SNR_AGE_S"), sp.reportsMeWhen, now);
            // No HEARD_BY here. A spot names ONE station; who heard it
            // is a relationship, and HEARING above carries all of them
            // rather than one. Since the map became observation-based
            // these lists are DERIVED from that same store, so the
            // field could only restate a subset of it -- and in
            // practice it went out empty on every spot. Consumers ask
            // HEARING.
            // [dumphonest] No PSKR / RADIO_WHEN here: this list does
            // not compute them, and emitting the defaults told readers
            // the opposite of the truth. See mk() above.
            m["RX_ONLY"] = sp.rxOnly;
            m["MONITOR_ONLY"] = sp.monitorOnly;
            m["REPORTS_ME"] = sp.reportsMe;
            out.append(m);
        }
        return out;
    };

    // [attemptviz] What the map currently believes we are trying, so
    // the state behind the red/green lines can be READ instead of
    // inferred from a log line someone pastes back at me (operator,
    // 2026-08-22: "you should see that already in your diagnostics,
    // right?" -- I could not).
    QVariantList attempts;
    for (Attempt const &a : m_attempts) {
        QVariantMap m;
        m["PATH"] = a.path;
        m["WAIT_S"] = a.waitSecs;
        m["REPLIED"] = a.replied;
        putTime(m, QStringLiteral("STARTED"),
                QStringLiteral("AGE_S"), a.started, now);
        putTime(m, QStringLiteral("REPLIED_AT"),
                QStringLiteral("REPLIED_AGE_S"), a.repliedAt, now);
        attempts.append(m);
    }

    QVariantMap out;
    out["ATTEMPTS"] = attempts;
    out["BAND"] = b;
    out["MY_CALL"] = m_myCall;
    out["MY_GRID"] = m_myGrid;
    // `now` cannot be invalid, but routing it through putTime too means
    // there is no line here that anyone can copy as a template for
    // emitting a time without the validity check.
    putTime(out, QStringLiteral("UTC"), QString{}, now, now);
    out["HEARING"] = hearing;
    // [oneobs] Derived, not stored: SPOTS_MINE is every station whose
    // observation names ME as the heard party; SPOTS_ALL is every
    // station the observations mention at all. Consumers keep the same
    // two lists; nothing keeps a second copy to disagree with.
    {
        QVector<Spot> mine, all;
        QSet<QString> seenAll;
        QString const myUpD = m_myCall.toUpper();
        auto const &hzD = m_hearingByBand.value(b);
        // [#170(f)] Position from the ONE authority, like the renderer.
        auto const &infoD = m_infoByBand.value(b);
        auto const mk = [&](QString const &call, QString const &grid,
                            int snr, QDateTime const &snrWh,
                            QDateTime const &wh, bool reportsMe) {
            Spot sp;
            sp.receiverCall = call;
            // [maptruth #12] grid from the ONE authority, exactly as
            // the renderer plots it; the edge copy could disagree.
            QString const auth = m_gridByCall.value(call);
            sp.receiverGrid = auth.isEmpty() ? grid : auth;
            sp.snr = snr;
            sp.reportsMeWhen = snrWh;   // the report's OWN clock
            sp.when = wh;
            sp.reportsMe = reportsMe;
            // [maptruth #6] RX_ONLY computed like the renderer, not
            // shipped as the struct default (the RADIO_WHEN lesson,
            // same class): a sender by info OR by radio presence.
            bool sender = infoD.value(call).sawAsSender;
            if (auto const hh = hzD.constFind(call);
                hh != hzD.constEnd() &&
                hh.value().source == QStringLiteral("radio"))
                sender = true;
            // [heardproof] heard by anyone = it transmitted.
            if (!sender)
                for (auto h2 = hzD.constBegin();
                     h2 != hzD.constEnd() && !sender; ++h2)
                    if (h2.value().heard.contains(call))
                        sender = true;
            sp.rxOnly = !sender;
            sp.monitorOnly = (snr <= -99);
            // [dumphonest] radioWhen and pskr are NOT set here, and a
            // reader cannot tell that from the output: every entry came
            // back RADIO_WHEN=null, PSKR=false, which reads as "every
            // station is internet-only AND none of them is a PSKR spot"
            // -- two contradictory claims, both artefacts of the
            // default. It cost two wrong diagnoses on 2026-08-24. The
            // classification lives in the render pass, so rather than
            // duplicate it here (and risk the two disagreeing), the
            // fields are omitted from the dump entirely.
            return sp;
        };
        // [maptruth #7] DETERMINISTIC: all hearers first (they carry
        // the richer facts), heard-only endpoints after -- QHash
        // iteration order can no longer decide which version of a
        // station the dump ships.
        for (auto h = hzD.constBegin(); h != hzD.constEnd(); ++h) {
            bool const hearsMe = h.value().heard.contains(myUpD);
            Spot const sp = mk(h.key(), h.value().grid, h.value().snr,
                               h.value().snrWhen, h.value().lastSeen,
                               hearsMe);
            if (hearsMe)
                mine.append(sp);
            if (!seenAll.contains(h.key())) {
                seenAll.insert(h.key());
                all.append(sp);
            }
        }
        for (auto h = hzD.constBegin(); h != hzD.constEnd(); ++h) {
            for (auto ed = h.value().heard.constBegin();
                 ed != h.value().heard.constEnd(); ++ed) {
                if (seenAll.contains(ed.key()) || ed.key() == myUpD)
                    continue;
                seenAll.insert(ed.key());
                all.append(mk(ed.key(), ed.value().grid, -99,
                              QDateTime{}, ed.value().when, false));
            }
        }
        out["SPOTS_MINE"] = packSpots(mine);
        out["SPOTS_ALL"] = packSpots(all);
    }
    // The grid authority: every locator we know, from any source.
    QVariantMap grids;
    for (auto it = m_gridByCall.constBegin();
         it != m_gridByCall.constEnd(); ++it)
        grids[it.key()] = it.value();
    out["GRIDS"] = grids;
    return out;
}

QString SpotMapWindow::refinedGrid(QString const &call,
                                   QString const &grid) const {
    // [gridfine] Precision cure for stations jumping between views:
    // a 4-char square from call-list/on-air text can sit ~35 km
    // from the 6-char PSKR locator of the SAME square. If the MQTT
    // cache holds a longer grid in the same square, use it — every
    // source then projects to identical coordinates. Different
    // square = station genuinely moved; keep the supplied grid.
    QString const cached = m_gridByCall.value(call.toUpper());
    if (cached.size() > grid.size() &&
        (grid.isEmpty() ||
         cached.left(4).compare(grid.left(4),
                                Qt::CaseInsensitive) == 0))
        return cached;
    return grid;
}

void SpotMapWindow::rememberGrid(QString const &call,
                                 QString const &gridIn,
                                 QString const &source) {
    // [gridcase 2026-08-20] CANONICAL CASE AT THE ONE DOOR (operator):
    // MQTT sends subsquare-lowercase ("DN61ok") while wire text is
    // uppercased — uppercase here so the authority, the persistent
    // store, and the == early-out below all see one form. Geodesic
    // normalizes internally, so nothing downstream cares.
    QString const grid = gridIn.trimmed().toUpper();
    // [mqttgrid] Harvest a locator from the MQTT feed and backfill
    // any hearing-store entry/edge stored without a position — the
    // feed runs constantly, so a HEARING-list station we've never
    // copied gets placed the moment ANY reporter mentions it
    // (operator, 2026-08-16).
    if (call.isEmpty() || grid.size() < 4)
        return;
    // [gridvalid 2026-08-27] The authority admits only real locators
    // -- AK6OI's corrupt "DM14GDDM33" sat in the authority; the
    // renderer refused to plot it but knownGrid()/the dump/the
    // executor all served it as a grid.
    if (!Maidenhead::valid(grid.left(6)))
        return;
    QString const key = call.toUpper();
    QString const known = m_gridByCall.value(key);
    if (known == grid)
        return; // already known — nothing to backfill
    // [gridfine] Never downgrade precision: a shorter grid in the
    // same square keeps the cached long form.
    if (known.size() > grid.size() &&
        known.left(4).compare(grid.left(4), Qt::CaseInsensitive) == 0)
        return;
    m_gridByCall.insert(key, grid);
    // [#164] Write-through: this is the authority's ONE accept point,
    // so the persistent tier records exactly its decisions.
    m_gridDb.upsert(key, grid, source);
    // [#170(k)] A grid sighting IS a sighting. upsert() no longer
    // stamps activity itself -- that belongs to noteActivity(), which
    // owns the per-call throttle -- so the fact has to be recorded
    // here, through its owner. Without this, grid-only sightings would
    // stop counting as activity entirely: the [mqttgrid] path harvests
    // a locator from spots it then discards, and nothing else on that
    // route calls noteActivity().
    m_gridDb.noteActivity(key, -99);
    // [#170(f) 2026-08-22] THE BACKFILL WALK IS GONE.
    //
    // This used to walk every band x every hearer x every edge to copy
    // az/dist onto each record whose grid had just changed -- roughly
    // 1500 iterations per accepted grid change, arriving at PSKR rate.
    //
    // It existed only to keep a SECOND COPY of a derived value in sync.
    // Azimuth and distance are functions of (my grid, their grid), and
    // m_gridByCall above is the one authority for their grid, so the
    // renderer computes position at paint from that instead. Nothing
    // is stored, so nothing can go stale -- which also retires the
    // "entry frozen at its first position" defect class outright, and
    // the first-placement-wins and thrash guards along with it: both
    // were protecting against instability that this accept point
    // already prevents.
    //
    // Verified against the live database before removing (666
    // edge/authority grid pairs): 641 identical, 18 where the
    // authority is MORE precise, 7 genuine disagreements that resolve
    // to whatever passed the precision/mover rules here -- and zero
    // callsigns whose grid existed only on an edge, so no station
    // becomes unplaceable.
    if (isVisible())
        requestReplot();
}

void SpotMapWindow::onMqttState(QString const &state) {
    m_stateText = state;
    requestReplot();
}

void SpotMapWindow::onMqttMessage(QString const &topic,
                                  QByteArray const &payload) {
    // [pskrbusy 2026-09-01] Every message on our band-filtered topic
    // is one PSKR spot on the current band: stamp it and prune the
    // trailing window here (arrival side), so the paint-side index
    // read stays const.
    {
        qint64 const nowMs = QDateTime::currentMSecsSinceEpoch();
        m_pskrArrivals.append(nowMs);
        while (!m_pskrArrivals.isEmpty() &&
               m_pskrArrivals.first() < nowMs - PSKR_BUSY_WINDOW_MS)
            m_pskrArrivals.removeFirst();
    }
    // Runtime schema verification: dump the first N messages after
    // each (re)subscribe so topic-level ordering and field names can
    // be confirmed live before trusting them.
    if (m_debugDumpsLeft > 0) {
        --m_debugDumpsLeft;
        qCDebug(mqttclient_js8) << "spot" << topic << payload;
    }

    QJsonObject const o = QJsonDocument::fromJson(payload).object();
    if (o.isEmpty()) {
        ++m_skippedSpots;
        return;
    }

    // Verify the sender is EXACTLY us. The topic subscribe uses the
    // base callsign level (topic levels can't contain '/'), so the
    // stream is a superset when running with a /P /M suffix — but
    // WM8Q and WM8Q/P are distinct stations operationally, and this
    // map must show only the EXACT configured call's spots (full-
    // callsign-compare rule; no base-call fallback here).
    // NOTE: how the broker encodes a suffixed call at the topic
    // level is unverified — when operating /P, watch the first-N
    // debug dumps (mqttclient.js8) to confirm suffixed spots arrive
    // on the base-level subscription at all.
    QString const sender = o.value(QStringLiteral("sc")).toString();
    // [viewall] The subscription now carries ALL JS8 senders. Spots
    // of MY signal feed the my-view dataset exactly as before (the
    // exact-sc rule above still governs it); every OTHER sender's
    // spot feeds the All-view dataset further below.
    bool const senderIsMe =
        !sender.isEmpty() &&
        sender.compare(m_myCall, Qt::CaseInsensitive) == 0;

    QString const receiverCall = o.value(QStringLiteral("rc")).toString();
    QString const receiverGrid = o.value(QStringLiteral("rl")).toString();
    int const snr = o.value(QStringLiteral("rp")).toInt(-99);
    // [mqttgrid] Harvest locators BEFORE any validity bail-out — a
    // spot we skip as a spot is still a grid sighting.
    rememberGrid(receiverCall, receiverGrid,
                 QStringLiteral("mqtt"));
    rememberGrid(sender, o.value(QStringLiteral("sl")).toString(),
                 QStringLiteral("mqtt"));
    if (receiverCall.isEmpty() || receiverGrid.size() < 4 || snr == -99) {
        ++m_skippedSpots;
        return;
    }
    // [BUILD 340] Exact RF Hz the spotter logged us at (payload f) —
    // hover shows the audio offset (f − dial) and double-click QSYs
    // to it.
    qint64 const spotFreqHz =
        static_cast<qint64>(o.value(QStringLiteral("f")).toDouble(0));
    // [BUILD 340] Country name for hover, only when the spotter's
    // DXCC differs from OURS — both codes ride the topic
    // (…/{sDXCC}/{rDXCC}); the NAME comes from the injected
    // LogBook/cty.dat lookup by callsign.
    QString country;
    if (QStringList const lv = topic.split(QLatin1Char('/'));
        lv.size() > 10 && m_countryLookup && lv.at(9) != lv.at(10)) {
        country = m_countryLookup(receiverCall);
    }

    // Band: prefer the topic level (pskr/filter/v2/{band}/...), fall
    // back to mapping the reported frequency.
    QString band;
    if (QStringList const levels = topic.split(QLatin1Char('/'));
        levels.size() > 3)
        band = levels.at(3);
    if (band.isEmpty() || m_config->bands()->find(band) < 0) {
        auto const f = static_cast<Radio::Frequency>(
            o.value(QStringLiteral("f")).toDouble(0));
        if (f > 0)
            band = m_config->bands()->find(f);
    }
    if (band.isEmpty()) {
        ++m_skippedSpots;
        return;
    }

    // [viewall] Which station gets PLOTTED depends on the dataset:
    // my-view plots the spotter (who hears me); the All view plots
    // the heard SENDER (payload sl = sender locator), with the
    // reporting station kept for hover detail.
    QString plottedCall = receiverCall.toUpper();
    // [gridfine] Raw payload locators go through the same refinement
    // as every on-air source — a station whose PSKR-REGISTERED
    // locator is 4-char must not re-degrade a precise position on
    // every report it files (field 2026-08-15: KJ7VWV DN52 vs
    // DN52QT, my-view spot re-clobbered per report).
    QString plottedGrid = refinedGrid(plottedCall, receiverGrid);
    if (!senderIsMe) {
        QString const senderGrid = o.value(QStringLiteral("sl")).toString();
        if (senderGrid.size() < 4) {
            ++m_skippedSpots;
            return;
        }
        plottedCall = sender.toUpper();
        plottedGrid = refinedGrid(plottedCall, senderGrid); // [gridfine]
        // Country of the plotted (sender) station for hover.
        country = m_countryLookup ? m_countryLookup(plottedCall)
                                  : QString();
    }
    // [maptruth #9, operator: "drop from hover if even implemented,
    // but don't lose the underlying data"] The heardBy hover line and
    // its per-message Geodesic projections died in the oneobs
    // refactor and could never render; deleted. The underlying fact
    // (reporter heard sender) IS the hearing-store edge recorded
    // below -- nothing lost.

    auto const vec = Geodesic::vector(m_myGrid, plottedGrid);
    if (!vec.azimuth().isValid() || !vec.distance().isValid()) {
        ++m_skippedSpots;
        return;
    }

    // [oneobs 2026-08-22] NO SPOT OBJECT. A PSKR report IS an
    // observation ("reporter heard sender"); recording it once below
    // gives both circles and the line between them. Station-level
    // facts the observation cannot carry go to m_infoByBand.
    auto const now = DriftingDateTime::currentDateTimeUtc();
    QDateTime when;
    if (qint64 const t =
            static_cast<qint64>(o.value(QStringLiteral("t")).toDouble(0));
        t > 0) {
        when = QDateTime::fromSecsSinceEpoch(t, Qt::UTC);
        if (when > now)
            when = now;
    } else {
        when = now;
        // [tstamp] Arrival-stamped (payload carried no 't') -- a broker
        // reconnect delivering retained messages would look fresh here.
        qCWarning(mqttclient_js8)
            << "[SPOTMAP] spot WITHOUT timestamp, stamped now:"
            << "sender=" << sender << "reporter=" << receiverCall
            << "band=" << band << "state=" << m_stateText;
    }
    {
        StationInfo &info = m_infoByBand[band][plottedCall];
        if (!country.isEmpty())
            info.country = country;
        // THEIRS only: on a report of MY signal `f` is the frequency
        // they heard US on, which is not a fact about them.
        // [freqclock 2026-09-08] Braces were missing, so freqWhen was
        // stamped UNCONDITIONALLY -- every reports-me or freq-less
        // spot refreshed the clock while the value stayed stale, the
        // exact value+clock split [maptruth #13] exists to prevent.
        // [passband #218] NEWEST OBSERVATION WINS; ties go to radio.
        // Compared on the two OBSERVATION times, never on "now" and
        // never through a window. The first cut of this rule let a
        // radio reading block PSKR for a full hour regardless of
        // which was newer -- so a station we decoded 40 minutes ago
        // at an out-of-passband offset stayed "Out" while a PSKR spot
        // from a minute ago said it had moved in (audit 2026-09-11).
        // A PSKR spot that is strictly newer than what we hold is
        // better evidence about where the station is NOW, first-hand
        // or not; one that is older (out-of-order batch, or older
        // than our own decode) changes nothing. Radio writes, in
        // addHearingReport(), are stamped at decode time and so are
        // always the newest possible -- they always overwrite.
        if (!senderIsMe && spotFreqHz > 0) {
            if (!info.freqWhen.isValid() || when > info.freqWhen) {
                info.freqHz = spotFreqHz;
                info.freqWhen = when;   // [maptruth #13] value + clock
                info.freqFromRadio = false;
            }
        }
        // [passband #218 evidence, operator ruling 2026-09-11] ONE
        // spot, TWO facts. The sender was TRANSMITTING at f (theirs
        // only -- when the sender is me, f is where they heard ME,
        // not a fact about anyone's transmitter). The REPORTER was
        // LISTENING at f -- always a fact about the reporter, me as
        // sender included. The first cut recorded only the first and
        // left every reporter -- most of the map -- with no frequency
        // at all, hence visible from one band edge to the other.
        if (spotFreqHz > 0) {
            if (!senderIsMe)
                noteFreq(band, sender, spotFreqHz, when,
                         /*radio=*/false, /*tx=*/true);
            noteFreq(band, receiverCall, spotFreqHz, when,
                     /*radio=*/false, /*tx=*/false);
        }
        if (!senderIsMe)
            info.sawAsSender = true;   // it was the SENDER of a spot
    }
    // [maptruth #11] journal AFTER the store update -- journalling
    // first persisted the PREVIOUS report's SNR with the current
    // clock, and restores grafted that one-behind value onto fresh
    // edges after every restart.
    // (moved below; see the addHearingReport calls)

    // [tribblenet 2026-08-21] FEED THE MESH, not just the dot.
    // Operator: "KB2UUL keeps changing who hears it every few seconds
    // ... but the total is always 1". CAUSE: the dot store keeps ONE
    // row per plotted station (the erase below), and the PSKR path
    // never wrote to the hearing store at all -- so who-hears-whom
    // fell back to the dots and could hold a single reporter.
    //
    // Placed HERE, after spot.when is resolved, and the spot's OWN
    // timestamp is passed as heardWhen: stamping edges "now" made
    // every PSKR re-report refresh them, so nothing ever aged and the
    // time-window buttons did nothing (operator, same session).
    if (!band.isEmpty()) {
        QString const reporter = receiverCall.toUpper();
        QString const rGrid = refinedGrid(reporter, receiverGrid);
        if (senderIsMe) {
            // [meshprobe 2026-08-22] Reports-of-me dots refresh while
            // their edges freeze; the code reads correct, so measure
            // instead of re-reading it. One line per reports-of-me
            // spot, rate-limited to 1/10 s.
            static qint64 lastProbe = 0;
            qint64 const nowMs = now.toMSecsSinceEpoch();
            if (nowMs - lastProbe > 10000) {
                lastProbe = nowMs;
                qCWarning(mqttclient_js8)
                    << "[MESHPROBE] reports-me from" << reporter
                    << "band=" << band << "myCall=" << m_myCall
                    << "obs age_s="
                    << when.secsTo(now)
                    << "will feed edge:" << reporter << "->"
                    << m_myCall.toUpper();
            }
            if (!m_myCall.isEmpty())
                // [audit 2026-08-21] heardSnr ALONGSIDE
                // reportedToMeSnr. Both describe the same measurement
                // -- how well this station hears US -- but they land in
                // different places: reportedToMeSnr on the STATION
                // (colours its dot), heardSnr on the EDGE (ranks it as
                // a first hop). Passing only the first left every
                // reports-of-me edge at -99, so TribbleNet knew THAT a
                // station hears us but never HOW WELL, and could not
                // rank first hops at all.
                addHearingReport(band, reporter, rGrid,
                                 {m_myCall.toUpper()}, {m_myGrid}, snr,
                                 when, /*heardSnr=*/snr,
                                 QStringLiteral("mqtt"));
        } else {
            QString const sndr = sender.toUpper();
            QString const sGrid = o.value(QStringLiteral("sl")).toString();
            // [audit 2026-08-21] WHEN THE REPORTER IS US, this is OUR
            // OWN receiver's evidence making a round trip through
            // PSKReporter -- radio-derived, not internet-derived. The
            // transport is not the provenance. Tagging it "mqtt" hid
            // our own decodes from the blue mesh and would have
            // deleted them from an offline (#159) route, which is
            // precisely backwards.
            QString const evid =
                reporter.compare(m_myCall, Qt::CaseInsensitive) == 0
                    ? QStringLiteral("radio")
                    : QStringLiteral("mqtt");
            if (!sndr.isEmpty() && sndr != reporter)
                addHearingReport(band, reporter, rGrid, {sndr}, {sGrid},
                                 /*reportedToMeSnr=*/-99, when,
                                 /*heardSnr=*/snr, evid);
        }
    }
    journalStation(band, plottedCall);   // [maptruth #11] AFTER
    pruneBand(band);

    // Repaint when the on-screen view is affected: reports of me
    // feed BOTH views since the All-superset change (audit item 2 —
    // the old senderIsMe != m_viewAll gate predated it); other
    // senders' spots feed the All view only.
    if (band == m_currentBand && isVisible() &&
        (senderIsMe || m_viewAll))
        requestReplot();
}

// [snrwho] THE reported-to-me SNR rule (audit item 8 — paint and
// hover each encoded a copy): a dB value exists ONLY as a report of
// MY signal. My view: the spot's snr IS that report (sentinel -99 on
// position-only spots). All view: the hearing store is the
// authority; reporter-of-me spots qualify directly (their snr is my
// signal at their QTH). Everything else: no report.
SpotMapWindow::ReportOfMe
SpotMapWindow::reportedToMe(Spot const &s) const {
    // [maptruth] value + observation time from the SAME record; a
    // dated pair or nothing. My view: the spot carries the report
    // (snr + reportsMeWhen). All view: the hearing store's dated
    // pair (snr + snrWhen).
    ReportOfMe r;
    if (!m_viewAll) {
        r.snr = s.snr;
        r.when = s.reportsMeWhen;
        return r;
    }
    auto const &ca = m_hearingByBand.value(m_currentBand);
    if (auto const it = ca.constFind(s.receiverCall);
        it != ca.constEnd() && it->snr > -99) {
        r.snr = it->snr;
        r.when = it->snrWhen;
        return r;
    }
    if (s.reportsMe) {
        r.snr = s.snr;
        r.when = s.reportsMeWhen;
    }
    return r;
}

// [radioage] THE presence/age clock (audit items 3+7 — one
// definition; the fill, superset append, me-lines, fade, and hover
// all read this): with PSKR display ON, freshness is freshness; with
// it OFF the view is as-received only, so a station ages by its last
// RADIO evidence even while internet reports keep refreshing `when`.
QDateTime SpotMapWindow::effectiveWhen(Spot const &s) const {
    return m_showPskr ? s.when
                      : (s.radioWhen.isValid() ? s.radioWhen : s.when);
}

void SpotMapWindow::pruneBand(QString const &band) {
    auto const cutoff =
        DriftingDateTime::currentDateTimeUtc().addSecs(-WINDOW_SECS);
    // [audit 2026-08-21] Prune on effectiveWhen(), THE presence clock
    // the header declares every consumer reads. This one did not: it
    // used the raw `when`, which internet reports keep refreshing, so
    // with PSKR display OFF a station could sit in the store long
    // after its last radio evidence. Conservative either way, but the
    // "one definition" contract has to actually hold.
    auto const age = [&](QVector<Spot> &spots) {
        spots.erase(std::remove_if(spots.begin(), spots.end(),
                                   [&](Spot const &s) {
                                       return effectiveWhen(s) < cutoff;
                                   }),
                    spots.end());
    };
    // [audit 2026-08-21] find(), not operator[]: pruning must never
    // CREATE a band. onPruneTick() walks one map and prunes by key,
    // so operator[] quietly inserted empty entries into the other two
    // on every 30 s tick, for every band ever seen.
    auto const hb = m_hearingByBand.find(band);
    if (hb == m_hearingByBand.end())
        return;
    // [hearlines] Per-edge aging; empty hearers drop out.
    auto &hearers = hb.value();
    for (auto h = hearers.begin(); h != hearers.end();) {
        auto &heard = h.value().heard;
        for (auto ed = heard.begin(); ed != heard.end();) {
            if (ed.value().when < cutoff)
                ed = heard.erase(ed);
            else
                ++ed;
        }
        if (heard.isEmpty() && h.value().lastSeen < cutoff)
            h = hearers.erase(h);
        else
            ++h;
    }
}

void SpotMapWindow::onPruneTick() {
    // [oneobs] One store to walk.
    for (auto it = m_hearingByBand.begin(); it != m_hearingByBand.end();
         ++it)
        pruneBand(it.key());
    // [maptick] Repaint on every 30 s tick while visible (was: only
    // when pruning removed something) — keeps the "last X of Y min"
    // counter, the age fade, and the view-window filter current on a
    // quiet band instead of freezing until the next spot arrives.
    if (isVisible())
        requestReplot();
}

// -------------------------------------------------------------------------
// Geographic background — Natural Earth outlines (SpotMapGeoData),
// projected azimuthal-equidistant around my grid, cached until the
// center, scale, or chart geometry changes.
// -------------------------------------------------------------------------

namespace {
constexpr double EARTH_RADIUS_KM = 6371.0;

// Maidenhead grid (4/6 chars) to lat/lon, center-of-square — matches
// the convention Geodesic uses internally (which isn't exported).
bool gridToLatLon(QString const &grid, double &lat, double &lon) {
    QString const g = grid.toUpper();
    if (g.size() < 4 || !g.at(0).isLetter() || !g.at(1).isLetter() ||
        !g.at(2).isDigit() || !g.at(3).isDigit())
        return false;
    lon = (g.at(0).toLatin1() - 'A') * 20.0 - 180.0 +
          (g.at(2).toLatin1() - '0') * 2.0;
    lat = (g.at(1).toLatin1() - 'A') * 10.0 - 90.0 +
          (g.at(3).toLatin1() - '0') * 1.0;
    if (g.size() >= 6 && g.at(4).isLetter() && g.at(5).isLetter()) {
        lon += (g.at(4).toLatin1() - 'A') * (2.0 / 24.0) + (1.0 / 24.0);
        lat += (g.at(5).toLatin1() - 'A') * (1.0 / 24.0) + (0.5 / 24.0);
    } else {
        lon += 1.0;
        lat += 0.5;
    }
    return true;
}
} // namespace

void SpotMapWindow::rebuildMapCache(float const R, float const scaleKm,
                                    float const cutKm) {
    if (m_mapCacheGrid == m_myGrid && m_mapCacheScale == scaleKm &&
        m_mapCacheR == R && m_mapCacheCut == cutKm)
        return;
    m_mapCache.clear();
    m_mapCacheGrid = m_myGrid;
    m_mapCacheScale = scaleKm;
    m_mapCacheR = R;
    m_mapCacheCut = cutKm;

    double lat0 = 0.0, lon0 = 0.0;
    if (!gridToLatLon(m_myGrid, lat0, lon0))
        return;
    double const p1 = lat0 * DEG2RAD;
    double const l1 = lon0 * DEG2RAD;
    double const sinP1 = std::sin(p1), cosP1 = std::cos(p1);

    // Segments whose endpoints are beyond the visible extent are
    // dropped — the caller derives cutKm from the panned viewport;
    // the cap in redraw() sidesteps the antipodal blow-up inherent
    // to the azimuthal projection.

    for (int i = 0; i < kGeoPolylineCount; ++i) {
        QPolygonF run;
        for (int j = kGeoPolylineStart[i]; j < kGeoPolylineStart[i + 1];
             ++j) {
            double const p2 = (kGeoPoints[j][0] / 100.0) * DEG2RAD;
            double const l2 = (kGeoPoints[j][1] / 100.0) * DEG2RAD;
            double const dl = l2 - l1;
            double const cosC = std::clamp(
                sinP1 * std::sin(p2) +
                    cosP1 * std::cos(p2) * std::cos(dl),
                -1.0, 1.0);
            double const distKm = EARTH_RADIUS_KM * std::acos(cosC);
            if (distKm > cutKm) {
                if (run.size() > 1)
                    m_mapCache.append(run);
                run.clear();
                continue;
            }
            double const az = std::atan2(
                std::sin(dl) * std::cos(p2),
                cosP1 * std::sin(p2) - sinP1 * std::cos(p2) * std::cos(dl));
            double const r = R * distKm / scaleKm;
            run.append(QPointF{std::sin(az) * r, -std::cos(az) * r});
        }
        if (run.size() > 1)
            m_mapCache.append(run);
    }
}

// -------------------------------------------------------------------------
// Rendering — CPlotter pattern: draw into m_pixmap off the paint path,
// then update(); paintEvent only composites.
// -------------------------------------------------------------------------

void SpotMapWindow::requestReplot() {
    // [paintperf 2026-08-23] THE DEBOUNCE MUST RESPECT WHAT A FRAME
    // COSTS. A fixed 150 ms let the PSKR feed ask for ~7 repaints a
    // second while each one took 615 ms: the GUI thread never finished
    // before the next request arrived, so it was permanently painting
    // and the window stalled for half a second at a time.
    //
    // Spend at most about half the thread on painting -- wait at least
    // as long as the last frame took. On a fast machine or a quiet
    // band this stays at the old 150 ms and nothing changes; on a
    // dense map it backs off exactly as much as it must.
    //
    // Quality is NOT the thing to trade here: drawing the mesh aliased
    // was visibly worse (operator, 2026-08-23), and this is a
    // SCHEDULING problem, not a rendering one.
    int const interval =
        static_cast<int>(qBound(qint64{150}, m_lastPaintMs, qint64{1000}));
    if (m_replotTimer.interval() != interval)
        m_replotTimer.setInterval(interval);
    if (!m_replotTimer.isActive())
        m_replotTimer.start();
}

QColor SpotMapWindow::snrColor(int const snr, float const alphaScale) {
    // Fixed range (operator choice): -25 dB deep blue (hue 240) through
    // yellow (~60) at 0 dB to red (hue 0) at +10 dB, clamped.
    float const t =
        std::clamp(static_cast<float>(snr - SNR_COLD) / (SNR_HOT - SNR_COLD),
                   0.0f, 1.0f);
    int const hue = static_cast<int>(240.0f * (1.0f - t));
    QColor c = QColor::fromHsv(hue, 255, 255);
    c.setAlphaF(std::clamp(alphaScale, 0.0f, 1.0f));
    return c;
}

float SpotMapWindow::niceCeil(float const value) {
    // Fine-grained auto-zoom steps (operator feedback 2026-07-14:
    // 1/2/5 rounding left the whole US at half-width — a 2600 km
    // spread jumped to a 5000 km scale). [ladder] Shared table.
    if (value <= 0.0f)
        return DEFAULT_SCALE_KM;
    float const mag = std::pow(10.0f, std::floor(std::log10(value)));
    for (float const mult : kLadderMults)
        if (value <= mag * mult)
            return mag * mult;
    return mag * 10.0f;
}

float SpotMapWindow::stepScale(float const scale, int const dir) {
    // One step up (+1) or down (−1) the same ladder niceCeil uses, so
    // manual zoom lands on the same scale values auto-zoom produces.
    // [ladder] Shared table.
    auto const &mults = kLadderMults;
    constexpr int n = static_cast<int>(std::size(kLadderMults));
    float mag = std::pow(10.0f, std::floor(std::log10(scale)));
    float const ratio = scale / mag; // [1, 10)
    int idx = 0;
    float bestErr = 1e9f;
    for (int i = 0; i < n; ++i) {
        if (float const e = std::fabs(mults[i] - ratio); e < bestErr) {
            bestErr = e;
            idx = i;
        }
    }
    idx += dir;
    if (idx < 0) {
        idx = n - 1;
        mag /= 10.0f;
    } else if (idx >= n) {
        idx = 0;
        mag *= 10.0f;
    }
    return std::clamp(mults[idx] * mag, FLOOR_SCALE_KM, MAX_SCALE_KM);
}

// [zoomkeepcenter] Manual zoom steps keep the CURRENT center point
// (operator 2026-08-15): leaving Auto bakes the auto-fit pan into
// the user pan, and every step rescales the pan so the map point at
// the window center stays put — home is NOT re-centered.
void SpotMapWindow::stepZoom(int const dir) {
    // [zoomunify 2026-08-15] The current scale must come from ONE
    // place with immediate update: m_manualScaleKm once manual;
    // m_lastScaleKm (redraw-computed) only while in Auto. Reading
    // the redraw-lagged value for manual steps let wheel spins
    // outpace the coalesced 150 ms replot — the ladder stalled while
    // the pan kept rescaling, hopping the center asymmetrically.
    float const oldScale =
        m_manualScaleKm > 0.0f
            ? m_manualScaleKm
            : (m_lastScaleKm > 0.0f ? m_lastScaleKm : DEFAULT_SCALE_KM);
    if (m_manualScaleKm <= 0.0f)
        m_panPx += m_autoPanPx; // leaving Auto: keep its framing
    m_manualScaleKm = stepScale(oldScale, dir);
    m_panPx *= oldScale / m_manualScaleKm;
    m_zoomAutoBtn->setChecked(false); // [autochk] manual now
    requestReplot();
}

void SpotMapWindow::zoomIn() { stepZoom(-1); }

void SpotMapWindow::zoomOut() { stepZoom(+1); }

void SpotMapWindow::zoomAuto() {
    m_manualScaleKm = 0.0f;
    m_panPx = QPointF{}; // recenter — Auto = fit all, centered
    m_lastAutoScaleKm = 0.0f; // [fitdamp] explicit press = fresh fit
    m_zoomAutoBtn->setChecked(true); // [autochk] auto in effect
    requestReplot();
}

void SpotMapWindow::redraw() {
    // [perf 2026-08-21] The MQTT client runs from app launch whether or
    // not the map is open, so a hidden window was still painting a full
    // chart on every state change. Nothing can see it; showEvent()
    // requests a replot, so reopening is still immediate.
    if (!isVisible())
        return;
    // [paintlog 2026-08-23] MEASURE, do not guess. The operator reports
    // the redraw being slow to START and 32% CPU while dragging; those
    // are different costs and only numbers separate them. Reports the
    // GAP since the previous paint (how long we made him wait) and the
    // paint's own duration, whenever either is bad enough to feel.
    QElapsedTimer paintTimer;
    paintTimer.start();
    m_lastNoteCount = 0;      // per FRAME, not cumulative
    m_lastGeoMs = 0;
    static qint64 lastPaintEndMs = 0;
    qint64 const gapMs =
        lastPaintEndMs ? QDateTime::currentMSecsSinceEpoch() - lastPaintEndMs
                       : 0;
    QSize const sz = size() * devicePixelRatio();
    if (sz.isEmpty())
        return;
    // [perf] Reuse the buffer: a full-window QPixmap was allocated on
    // EVERY repaint (up to ~7/s on the 150 ms debounce). Only the
    // size changing needs a new one.
    if (m_pixmap.size() != sz) {
        m_pixmap = QPixmap{sz};
        m_pixmap.setDevicePixelRatio(devicePixelRatio());
    }
    m_pixmap.fill(QColor(16, 16, 24)); // near-black chart background

    QPainter p{&m_pixmap};
    // [paintperf 2026-08-23] MEASURED: build 1 ms, geo 0 ms, draw
    // 127-627 ms -- the frame IS the rasterisation of ~4590 lines.
    // Antialiasing multiplies that cost, and while the chart is
    // sliding under the cursor nobody is inspecting edge quality. It
    // comes back the instant the drag ends (mouseReleaseEvent forces
    // that frame).
    p.setRenderHint(QPainter::Antialiasing, !m_dragging);
    p.setRenderHint(QPainter::TextAntialiasing);

    int const w = width();
    int const h = height();
    QPointF center =
        QPointF{w / 2.0,
                TITLE_STRIP_PX +
                    (h - TITLE_STRIP_PX - LEGEND_STRIP_PX) / 2.0} +
        m_panPx;
    float const R =
        std::min(w / 2.0, (h - TITLE_STRIP_PX - LEGEND_STRIP_PX) / 2.0) -
        MARGIN_PX;
    if (R < 40.0f)
        return;

    // [units] Distance units follow the Settings selection (was:
    // miles-if-grid-in-North-America). configRefresh() repaints on
    // settings accept so a units change shows immediately.
    bool const miles = m_config->miles();
    QString const unitLabel = miles ? tr("mi") : tr("km");

    auto const now = DriftingDateTime::currentDateTimeUtc();
    // [spotwin] Storage holds the full hour; the 15/30/60 buttons
    // pick how much of it renders. Everything below (auto-scale,
    // counts, dots, hover) operates on the filtered view.
    // ===================================================================
    // [renderset 2026-08-21] THE render-set authority. ONE pass, ONE
    // visibility rule, ONE position rule.
    //
    // WHY THIS EXISTS. The drawn set used to be assembled by FOUR
    // independent passes -- spot filter, [allsuper] append, [mondots]
    // synthesis, and the hearing-store anchor walk -- each with its own
    // PSKR gate, its own age gate and its own "already present" set.
    // Feeding the PSKR stream into the mesh (needed for routing) then
    // put the same station in reach of several passes at once, and
    // every fix in one pass surfaced as a defect in another: blue lines
    // that followed the PSKR toggle, then orphan circles with the
    // toggle on, then orphan circles with it off. Operator: "we're
    // chasing circularly... do one thing in one place."
    //
    // THE MODEL. Each station is registered ONCE with the freshest
    // timestamp per EVIDENCE CLASS:
    //     radioWhen  our own receiver, or a station answering us
    //     when       freshest of ANY class (radio or internet)
    // Visibility is then a single expression -- effectiveWhen(), which
    // already encodes [radioage]: with PSKR shown, freshness is
    // freshness; with it hidden, a station ages by its RADIO clock and
    // an internet-only station has no radio clock, so it disappears.
    // No pass needs its own toggle check, and none can disagree.
    //
    // POSITION IS SEPARATE AND UNCONDITIONAL (operator: "you can always
    // use PSKR data for the grid"). allPos records where every station
    // is, drawn or not, so a line never loses an endpoint because a
    // spot is hidden. That is the whole point of the grid bank (#164).
    QVector<Spot> render;
    // [dotlog] WHY A DOT IS NOT THERE. Operator, 2026-08-24: "not only
    // the line from PSKR data didn't show, the dots from PSKR data
    // didn't show either." The dots are upstream of the lines, so any
    // explanation that starts at the line layer is already downstream
    // of the fault. Every gate a station passes through on its way to
    // being drawn is counted here, split by evidence class, so the log
    // names the one that emptied the map instead of leaving it to be
    // inferred.
    int dotSeen = 0, dotPskr = 0, dotHidden = 0, dotNoClock = 0,
        dotOldPskr = 0, dotOldRadio = 0, dotDrawnPskr = 0,
        dotOutOfBand = 0,   // [passband #218]
        dotFreqUnknown = 0; // [passband #218] drawn with NO frequency
                            // evidence at all -- the exempt class;
                            // if this is large, the map is not being
                            // filtered much, and that is why
    // [passband #218] Stations the passband filter dropped THIS
    // paint. The line layer must skip every edge touching one: the
    // ruling is that an out-of-passband station is not on the map at
    // all, dot OR lines. This set is the only way to say so, because
    // line endpoints come from allPos -- every station with a known
    // position, drawn or not -- by design ([audit2]: a radio edge to
    // a PSKR-hidden station is real evidence and must render). That
    // design is right for the PSKR toggle and wrong for this filter,
    // so the filter carries its own exclusion. Field 2026-09-11:
    // dots vanished on a dial change and their lines stayed.
    QSet<QString> passbandDropped;
    QElapsedTimer buildTimer;   // [paintlog] render-set cost
    buildTimer.start();
    QHash<QString, QPointF> allPos;   // call -> (azimuth, distance)
    {
        QString const myUp = m_myCall.toUpper();
        auto const cutoff = now.addSecs(-m_viewWindowSecs);
        QHash<QString, Spot> reg;
        QSet<QString> sawAsSender;    // [mondots] rxOnly = never a sender
        // [heardproof 2026-08-27, operator: "should have been this
        // way already"] Appearing as HEARD in any edge proves the
        // station transmitted, whatever the transport. Collect the
        // newest such time per station in one pass.
        QHash<QString, QDateTime> lastTx;
        auto const &hzTx = m_hearingByBand.value(m_currentBand);
        for (auto h0 = hzTx.constBegin(); h0 != hzTx.constEnd();
             ++h0) {
            if (h0.value().source == QStringLiteral("radio")) {
                auto &t = lastTx[h0.key()];
                if (h0.value().lastSeen > t)
                    t = h0.value().lastSeen;
            }
            for (auto e0 = h0.value().heard.constBegin();
                 e0 != h0.value().heard.constEnd(); ++e0) {
                auto &t = lastTx[e0.key()];
                if (e0.value().when > t)
                    t = e0.value().when;
            }
        }
        for (auto it0 = lastTx.constBegin(); it0 != lastTx.constEnd();
             ++it0)
            sawAsSender.insert(it0.key());

        // Register one observation. `pskrEv` says which clock it feeds;
        // `posAuth` forces the position (the hearing store is [posauth],
        // the ONE position source when it has placed a station).
        auto const note = [&](QString const &callRaw, bool pskrEv,
                              QDateTime const &when, int snrToMe,
                              QDateTime const &snrWhen =
                                  QDateTime{}) -> Spot * {
            // [paintperf 2026-08-23] NO toUpper() HERE. Both callers
            // pass keys straight out of m_hearingByBand, which are
            // canonical uppercase already ([#164] "UPPERCASE at the
            // door"), so this allocated a fresh QString 5513 times a
            // frame to produce a string it had been handed. Measured on
            // the HP650: the whole 95-170 ms frame was this pass.
            QString const &call = callRaw;
            // [selfhop] I am the triangle, never a dot -- from ANY
            // source, so nothing clickable exists at my position.
            if (call.isEmpty() ||
                call.compare(myUp, Qt::CaseInsensitive) == 0)
                return nullptr;
            ++m_lastNoteCount;
            Spot &r = reg[call];
            if (r.receiverCall.isEmpty()) {
                r.receiverCall = call;
                r.distance = -1.0f;   // unplaced until a grid resolves
            }
            // [#170(f)] POSITION COMES FROM THE AUTHORITY, always.
            // Callers no longer pass az/dist and no record stores
            // them: m_gridByCall owns the grid, so the position is a
            // function of it and is computed here, fresh, every paint.
            // That removes the old precedence rule (first placement
            // wins unless posAuth) because with a single source there
            // are no competing placements to rank.
            // ONE RESOLUTION PER STATION PER PAINT. note() runs per
            // EDGE, so a station with ten edges used to pay ten times.
            // Geodesic::vector is cached, but the cache is behind a
            // static QMutex and keyed by a constructed QString, so at
            // ~1700 edges that was 1700 mutex-locked lookups every
            // repaint -- and dragging repaints continuously by design.
            // Measured effect: the map went janky at 32% CPU while
            // panning (operator, 2026-08-23). allPos already holds the
            // answer, so consult it before computing.
            if (auto const seen = allPos.constFind(call);
                seen != allPos.constEnd()) {
                r.azimuth = static_cast<float>(seen->x());
                r.distance = static_cast<float>(seen->y());
                if (r.receiverGrid.isEmpty())
                    r.receiverGrid = m_gridByCall.value(call);
            } else if (m_myGrid.size() >= 4) {
                QString const authGrid = m_gridByCall.value(call);
                if (!authGrid.isEmpty()) {
                    auto const v = Geodesic::vector(m_myGrid, authGrid);
                    if (v.azimuth().isValid() && v.distance().isValid()) {
                        r.receiverGrid = authGrid;
                        r.azimuth = static_cast<float>(v.azimuth());
                        r.distance = static_cast<float>(v.distance());
                        allPos.insert(call,
                                      QPointF{static_cast<qreal>(r.azimuth),
                                              static_cast<qreal>(r.distance)});
                    }
                }
            }
            if (when.isValid() && (!r.when.isValid() || when > r.when))
                r.when = when;
            if (!pskrEv && when.isValid() &&
                (!r.radioWhen.isValid() || when > r.radioWhen))
                r.radioWhen = when;
            // [snrwho] A dB value exists ONLY as a report of MY
            // signal, and it is only as fresh as the edge it rode in
            // on -- so the two travel together.
            if (snrToMe > -99) {
                // [maptruth] the report and ITS OWN observation time,
                // one record (snrWhen from the store); reportsMe was
                // a dead binding (#8) -- it lives now.
                QDateTime const rw = snrWhen.isValid() ? snrWhen : when;
                if (rw.isValid() &&
                    (!r.reportsMeWhen.isValid() ||
                     rw >= r.reportsMeWhen)) {
                    r.snr = snrToMe;
                    r.reportsMeWhen = rw;
                    r.reportsMe = true;
                }
            }
            return &r;
        };

        // ---- 1. (no separate spot pass) ------------------------------
        // [oneobs 2026-08-22] There is nothing to feed here any more.
        // Circles come from the same observations the lines do, below.
        // ---- 2. THE OBSERVATIONS -------------------------------------
        // [hearlines] Stations that never report to PSKReporter exist
        // ONLY here. [posauth] and the position rule both live in note().
        auto const &hz = m_hearingByBand.value(m_currentBand);
        for (auto h = hz.constBegin(); h != hz.constEnd(); ++h) {
            bool const hearsMe =
                h.value().snr > -99 || h.value().heard.contains(myUp);
            if (!m_viewAll && !hearsMe)
                continue;    // [viewedges] MY view: hearers of me only
            // [renderset] A DOT REQUIRES AN EDGE, never bare
            // presence. Registering a station from lastSeen alone gave
            // a dot to anything with radio presence whose edges were
            // all internet-sourced -- visible with PSKR off, every one
            // of its lines filtered out, i.e. a hollow circle connected
            // to nothing (operator, 2026-08-21). Presence still
            // REFRESHES a station the edges below register; it just
            // cannot conjure one on its own.
            QDateTime hearerAny, hearerRadio;
            int hearerSnr = -99;
            // [maptruth] the store's dated report pair (snr +
            // snrWhen, one record) is the report's value and clock.
            // [oneclass 454] Its CLASS comes from the X->ME edge --
            // the SAME record the line pass classifies by -- so the
            // dot and its line can never disagree. Classifying by the
            // entry's sticky presence label here (maptruth, 08-27)
            // let an internet report feed radioWhen: dot fresh, edge
            // hidden with PSKR off -- dots without lines.
            QDateTime hearerSnrWhen = h.value().snrWhen;
            bool meEdgePskr = true;
            for (auto ed = h.value().heard.constBegin();
                 ed != h.value().heard.constEnd(); ++ed) {
                bool const edgePskr =
                    ed.value().source == QStringLiteral("mqtt");
                // [renderset] AGE A STATION BY THE EVIDENCE THIS VIEW
                // IS ABOUT. The MY view shows stations HEARING ME and
                // draws one line each, from the X->ME edge; so only
                // that edge may qualify X. Refreshing X's clock from
                // its edges to OTHER stations let a dot pass the time
                // filter while the line it needs had already aged out
                // -- coloured spots with no lines (operator,
                // 2026-08-21, 15 min window: the six radio "X hears
                // WM8Q" edges were 22.6 min old while the dots stayed).
                // The All view is about every edge, so there every
                // edge qualifies -- and every one of them can draw.
                if (!m_viewAll && ed.key() != myUp)
                    continue;
                // The hearer is evidenced by this edge too.
                //
                // [snrwho] THE dB FIGURE BELONGS TO THE X->ME EDGE,
                // not to the station. HearingEntry::snr is a
                // station-level value that never ages, so passing it
                // on every edge made the map assert a relationship it
                // would not draw: W3NIC showed a dot, "hears me at
                // -10" and "4 min ago" while its WM8Q edge was 20 min
                // old and correctly filtered out of a 15-min view
                // (operator, 2026-08-23). The dot was fresh on OTHER
                // evidence; only the report-of-me was stale.
                //
                // So the report only travels on the edge that carries
                // it. Every other edge contributes presence and age,
                // never an SNR -- which is the same rule the MY view
                // already applies to ageing.
                // [paintperf 2026-08-23] THE HEARER IS NOTED ONCE,
                // AFTER THIS LOOP. It is the same station on every
                // edge, so calling note() per edge did the whole
                // register-and-place dance 2589 times to advance one
                // age -- 5513 note() calls for 2772 segments, and on
                // the HP650 that pass WAS the frame (95-170 ms, with
                // the geography at 0 ms). Accumulate here, apply once.
                //
                // Two clocks, because they mean different things: the
                // freshest evidence of ANY kind sets `when`, and the
                // freshest RADIO evidence sets radioWhen ([radioage]).
                // Collapsing them would let an internet refresh keep a
                // station alive with the PSKR toggle off.
                if (!hearerAny.isValid() || ed.value().when > hearerAny)
                    hearerAny = ed.value().when;
                if (!edgePskr &&
                    (!hearerRadio.isValid() || ed.value().when > hearerRadio))
                    hearerRadio = ed.value().when;
                if (ed.key() == myUp) {
                    hearerSnr = h.value().snr;
                    meEdgePskr = edgePskr;   // [oneclass 454]
                }
                if (m_viewAll) // heard-endpoints: All view only
                    note(ed.key(), edgePskr, ed.value().when, -99);
            }
            // Flush the hearer: at most two calls instead of one per
            // edge. The radio pass runs FIRST so `when` still ends up
            // at the freshest of either, while radioWhen only ever
            // sees on-air evidence.
            if (hearerRadio.isValid())
                note(h.key(), /*pskrEv=*/false, hearerRadio, -99);
            if (hearerAny.isValid() && hearerAny != hearerRadio)
                note(h.key(), /*pskrEv=*/true, hearerAny, -99);
            if (hearerSnrWhen.isValid() && hearerSnr > -99)
                note(h.key(), meEdgePskr,
                     hearerSnrWhen, hearerSnr, hearerSnrWhen);
            // [maptruth #10] a station whose PRESENCE is radio was
            // decoded transmitting -- it is a sender, monitor label
            // and dB suppression must not apply (offline, this is
            // every station on the map).
            if (h.value().source == QStringLiteral("radio"))
                sawAsSender.insert(h.key());
        }

        m_lastBuildMs = buildTimer.elapsed();

        // ---- 3. ONE visibility rule ----------------------------------
        auto const &infoBand = m_infoByBand.value(m_currentBand);
        for (auto it = reg.begin(); it != reg.end(); ++it) {
            Spot &r = it.value();
            if (r.distance < 0.0f)
                continue;               // nowhere to draw it
            // [oneobs] Station facts an observation cannot carry.
            if (auto const in = infoBand.constFind(it.key());
                in != infoBand.constEnd()) {
                r.country = in->country;
                r.freqHz = in->freqHz;
                if (in->sawAsSender)
                    sawAsSender.insert(it.key());
            }
            // [mondots] rxOnly = never observed as a SENDER; the only
            // case hover labels "monitor".
            r.rxOnly = !sawAsSender.contains(it.key());
            r.lastTxWhen = lastTx.value(it.key());
            // [pskrtoggle] Internet-only when no radio clock exists --
            // derived, never set by a pass.
            r.pskr = !r.radioWhen.isValid();
            // [snrwho][allhollow] Hollow unless a report of MY signal.
            r.monitorOnly = (r.snr <= -99);
            ++dotSeen;                                   // [dotlog]
            if (r.pskr)
                ++dotPskr;                               // [dotlog]
            // [pskrtoggle] EVIDENCE CLASS FIRST. An internet-only
            // station is not shown at all while PSKR spots are hidden
            // -- and therefore draws no yellow line, which is the
            // whole contract of the button.
            //
            // This must be explicit. effectiveWhen() does NOT encode
            // the toggle on its own: with no radio clock it FALLS BACK
            // to `when`, so relying on it alone kept every
            // internet-only station visible with the toggle off and
            // gave the MY view a full set of yellow lines (operator:
            // "can't have ANY yellow lines with add PSKR unchecked").
            // The old code caught this in a separate spot filter that
            // the unified pass replaced; the rule survives, now stated
            // once, here.
            if (!m_showPskr && r.pskr) {
                ++dotHidden;                             // [dotlog]
                continue;
            }
            // [radioage] THE clock, for what remains.
            QDateTime const eff = effectiveWhen(r);
            if (!eff.isValid()) {
                ++dotNoClock;                            // [dotlog]
                continue;
            }
            if (eff < cutoff) {
                (r.pskr ? dotOldPskr : dotOldRadio)++;   // [dotlog]
                continue;
            }
            // [passband #218] ALWAYS in effect, no toggle (operator
            // ruling): a station KNOWN to be transmitting outside the
            // passband we are listening to is not drawn. Only the
            // Out verdict drops -- Unknown (no frequency, stale
            // frequency, RX-only station, or our own dial unknown)
            // is drawn as before. Evaluated here at paint time, every
            // paint, so a QSY re-judges every dot on the next frame.
            // Dropping the dot also drops its lines, since the
            // connections layer only draws edges whose endpoints
            // were plotted (endpointMissing in LINELOG).
            //
            // HONOURS THE PSKR DISPLAY TOGGLE, like every other
            // consumer on this map (effectiveWhen() is the contract:
            // with internet evidence hidden, the map judges on radio
            // evidence only). A PSKR-sourced frequency therefore
            // cannot hide a radio-heard dot while the toggle is off
            // -- the operator saw exactly that happen at startup on
            // 2026-09-11 before this guard existed. Routing does not
            // consult the toggle and is untouched: its book always
            // uses PSKR edges.
            // (The PSKR-toggle rule now lives INSIDE the verdict as
            // pskrAllowed, so a PSKR-only frequency cannot hide a dot
            // while internet evidence is hidden.)
            switch (passbandVerdict(m_currentBand, it.key(),
                                    JS8_FREQ_STALE_SECS,
                                    /*includeRx=*/true,
                                    /*pskrAllowed=*/m_showPskr)) {
            case Passband::Out:
                ++dotOutOfBand;                          // [dotlog]
                passbandDropped.insert(it.key());        // lines too
                continue;
            case Passband::Unknown:
                ++dotFreqUnknown;                        // [dotlog]
                break;
            case Passband::In:
                break;
            }
            if (r.pskr)
                ++dotDrawnPskr;                          // [dotlog]
            render.append(r);
        }

        // [relaykeep] Selected relay hops stay pinned while relay-select
        // is active, even if their evidence aged out of the window.
        if (m_relaySelect) {
            QSet<QString> present;
            for (Spot const &s : render)
                present.insert(s.receiverCall);
            for (Spot const &snap : m_relayPathSpots)
                if (!present.contains(snap.receiverCall)) {
                    render.append(snap);
                    present.insert(snap.receiverCall);
                    if (snap.distance >= 0.0f)
                        allPos.insert(
                            snap.receiverCall,
                            QPointF{static_cast<qreal>(snap.azimuth),
                                    static_cast<qreal>(snap.distance)});
                }
        }
    }

    // [bboxfit 2026-08-15] Auto-fit uses the WHOLE window: the
    // bounding box of every station (home included at the origin) in
    // km-space is fitted to the usable rect and CENTERED — the home
    // station is not kept centered (operator: spots filled only half
    // the width). Pan-independent by construction, so dragging never
    // changes the auto scale; manual zoom overrides; resize replots
    // and re-fits.
    float minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f;
    bool anyFit = false;
    auto const grow = [&](float az, float dist) {
        double const rad = az * DEG2RAD;
        float const x = static_cast<float>(std::sin(rad) * dist);
        float const y = static_cast<float>(-std::cos(rad) * dist);
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
        anyFit = true;
    };
    // [fitset] The box grows over EXACTLY the rendered set -- now a
    // single vector, so divergence between "what was fit" and "what was
    // drawn" is impossible by construction.
    for (Spot const &s : render)
        grow(s.azimuth, s.distance);
    // [autofit] The box covers the COMPLETE render set — spots and
    // store anchors alike; Auto must fit ALL of them.
    double const availW = std::max(40.0, w - 2.0 * MARGIN_PX);
    // [#208, operator] Auto-fit's lower bound is the TOP of the
    // "Show stations..." button stack, not the legend strip: the
    // full-height fit kept hiding southern stations behind the
    // buttons.
    double bottomReserved = LEGEND_STRIP_PX;
    if (m_viewMineBtn && m_viewMineBtn->isVisible())
        bottomReserved = std::max<double>(
            bottomReserved, h - m_viewMineBtn->y());
    double const availH = std::max(
        40.0, h - TITLE_STRIP_PX - bottomReserved - 2.0 * MARGIN_PX);
    float const needKm = static_cast<float>(
        R * std::max((maxX - minX) / availW, (maxY - minY) / availH));
    // [ladder] Auto obeys the same range manual can reach (audit
    // item 6: auto could exceed MAX_SCALE_KM; the ladder is one
    // authority, its bounds included).
    float autoScaleKm =
        (!anyFit || needKm <= 0.0f)
            ? DEFAULT_SCALE_KM
            : std::clamp(niceCeil(needKm), FLOOR_SCALE_KM,
                         MAX_SCALE_KM);
    // [fitdamp 2026-08-16] Shrink hysteresis: GROW immediately (a
    // station must never render off-window), but shrink only when
    // the need falls well under the current scale — a far station
    // blinking at the view-window age boundary was flapping the
    // auto zoom 5000↔4000 every few seconds (viewlog, 02:06Z).
    // Damps ONLY against the last AUTO scale: comparing against
    // m_lastScaleKm let MANUAL scales pollute the damper (viewlog
    // 03:13Z — Auto after "−" refused to re-fit from 6000 to 5000).
    if (m_manualScaleKm <= 0.0f) {
        if (m_lastAutoScaleKm > 0.0f &&
            autoScaleKm < m_lastAutoScaleKm &&
            needKm > 0.7f * m_lastAutoScaleKm)
            autoScaleKm = m_lastAutoScaleKm;
        m_lastAutoScaleKm = autoScaleKm;
    }
    float const scaleKm =
        m_manualScaleKm > 0.0f ? m_manualScaleKm : autoScaleKm;
    m_lastScaleKm = scaleKm;
    // [bboxfit] Auto centers the station bounding box, not home.
    // [zoomkeepcenter] Remember the applied auto-pan so a switch to
    // manual zoom can bake it in and keep the same center point.
    if (m_manualScaleKm <= 0.0f) {
        m_autoPanPx =
            anyFit ? QPointF{-(minX + maxX) / 2.0 * R / scaleKm,
                             -(minY + maxY) / 2.0 * R / scaleKm}
                   : QPointF{};
        center += m_autoPanPx;
    } else {
        m_autoPanPx = QPointF{};
    }

    // [viewlog 2026-08-16] One line per VIEW/MODE/SCALE transition
    // (field: "failed to re-scale, many calls off-screen" once,
    // unreproducible — this names pan vs manual-mode vs fit next
    // time). Rate-limited to changes; view switches are rare.
    // NOT WHILE DRAGGING. Pan is part of the key, so every drag frame
    // differed and logged -- 680 warning-level lines with an
    // 11-argument QString built EVERY frame whether or not it was
    // logged, plus the disk write, all on the GUI thread during
    // exactly the repaints we just spent an hour making fast. The
    // frame after release still records the final pan, which is the
    // state actually worth having (2026-08-23).
    if (!m_dragging) {
        static QString lastViewLog;
        QString const cur =
            QStringLiteral("view=%1 mode=%2 scale=%3 pan=%4,%5 "
                           "box=%6x%7 fit=%8 R=%9 avail=%10x%11")
                .arg(m_viewAll ? "all" : "mine")
                .arg(m_manualScaleKm > 0.0f ? "manual" : "auto")
                .arg(qRound(scaleKm))
                .arg(qRound(m_panPx.x()))
                .arg(qRound(m_panPx.y()))
                .arg(qRound(maxX - minX))
                .arg(qRound(maxY - minY))
                .arg(anyFit)
                .arg(qRound(R))
                .arg(qRound(availW))
                .arg(qRound(availH));
        if (cur != lastViewLog) {
            lastViewLog = cur;
            qCWarning(mqttclient_js8) << "[SPOTMAP][viewlog]" << cur;
        }
    }

    QFont small = p.font();
    small.setPointSize(8);
    p.setFont(small);

    // Geographic background: country/coast outlines across the whole
    // visible viewport (pan can carry any part of the chart into
    // view, so the old clip-to-circle is gone — the boundary ring
    // overlays as the scale reference). The cut extends to the
    // farthest visible corner, quantized to the nice ladder so drags
    // only rebuild the cache when crossing a step; capped short of
    // the antipode where the projection blows up.
    double maxCornerPx = 0.0;
    for (QPointF const &corner :
         {QPointF{0, 0}, QPointF{static_cast<qreal>(w), 0},
          QPointF{0, static_cast<qreal>(h)},
          QPointF{static_cast<qreal>(w), static_cast<qreal>(h)}}) {
        maxCornerPx = std::max(maxCornerPx,
                               std::hypot(corner.x() - center.x(),
                                          corner.y() - center.y()));
    }
    float const cutKm = std::min(
        niceCeil(scaleKm * static_cast<float>(maxCornerPx) / R * 1.05f),
        19000.0f);
    rebuildMapCache(R, scaleKm, cutKm);
    // [paintlog] Attribute the frame. The coastline walk is the one
    // cost that does NOT scale with traffic, so it is invisible in any
    // "too many lines" theory -- and culling lines barely moved a slow
    // box, which is what pointed here.
    QElapsedTimer geoTimer;
    geoTimer.start();
    p.save();
    p.translate(center);
    p.setPen(QPen{QColor(70, 100, 80), 1}); // muted land outline
    int geoPts = 0;
    for (QPolygonF const &poly : m_mapCache) {
        geoPts += poly.size();
        p.drawPolyline(poly);
    }
    p.restore();
    m_lastGeoMs = geoTimer.elapsed();
    m_lastGeoPts = geoPts;
    m_lastGeoPolys = static_cast<int>(m_mapCache.size());

    // [scalebar 2026-08-15] Outer boundary circle, azimuth ticks and
    // compass dropped (operator) — scale now reads from the bar above
    // the SNR legend.

    // [scalebar] The scale legend above the SNR bar is the map's
    // scale cue (rings feature removed, audit item 10 — no UI ever
    // shipped and quarter-radius circles stopped meaning anything
    // under border-fit).
    // [units] In miles, label the km zoom ladder with the operator's
    // nice-number equivalents (operator table, 2026-08-14) instead of
    // the raw conversion (500 km -> "300 mi", not "311 mi"). The
    // ladder steps themselves are unchanged. Falls back to the raw
    // conversion for any off-ladder scale.
    auto const scaleLabelValue = [&]() -> int {
        if (!miles)
            return qRound(scaleKm);
        struct KmMi { int km; int mi; };
        static constexpr KmMi table[] = {
            // [units] NEW close rungs, rounded to the nearest 25 mi
            // (operator: "show scale to nearest 25 miles then... for
            // the two new scales only") — the >=500 km entries below
            // keep the original operator table untouched.
            {150, 100},    {200, 125},    {250, 150},   {300, 175},
            {400, 250},
            {500, 300},    {600, 375},    {800, 500},   {1000, 625},
            {1250, 775},   {1500, 925},   {2000, 1250}, {2500, 1550},
            {3000, 1850},  {4000, 2500},  {5000, 3100}, {6000, 3700},
            {8000, 5000},  {10000, 6200}, {12500, 7700},
            {15000, 9300}};
        for (auto const &e : table)
            if (std::fabs(scaleKm - e.km) <= e.km * 0.01f)
                return e.mi;
        return qRound(scaleKm * 0.621371f);
    };

    // [hometri] My station is drawn LAST, at the end of this function
    // -- see the triangle block down there. It used to paint here,
    // before the dots and lines, so a busy centre buried it.

    // Dots render oldest first so the newest draw on top (the sort
    // happens AFTER the anchor pass below — audit item 12: anchors
    // appended post-sort always painted on top regardless of age).
    // Age fades alpha 1.0 -> 0.5 across the selected view window.
    QVector<Spot> ordered = render;

    auto const project = [&](float az, float dist) {
        double const rad = az * DEG2RAD;
        return center + QPointF{std::sin(rad), -std::cos(rad)} *
                            (R * (dist / scaleKm));
    };
    // Screen position of every plotted station this frame, by call —
    // shared by the Connections overlay (lines must END on a visible
    // dot; operator 2026-08-14: no lines to nowhere) and the relay
    // path.
    QHash<QString, QPointF> posByCall;
    // [audit2] Positions the mesh knows but is not DRAWING (a PSKR-only
    // station with the spot toggle off) still anchor lines: a radio
    // edge to such a station is real evidence and must render.
    for (auto it = allPos.constBegin(); it != allPos.constEnd();
         ++it)
        posByCall.insert(it.key(),
                         project(static_cast<float>(it.value().x()),
                                 static_cast<float>(it.value().y())));
    for (Spot const &s : ordered)
        posByCall.insert(s.receiverCall, project(s.azimuth, s.distance));
    // [selfhop] My own position is always the triangle at center —
    // registered here (not as a spot) so edges from/to me anchor
    // even in a pure-MQTT session with no hearing-store entry.
    if (!m_myCall.isEmpty())
        posByCall.insert(m_myCall.toUpper(), center); // upper: edge
                                                      // keys are upper
    m_centerPx = center;   // [selfhover #204] for hover hit-testing

    // [connlines] Connections overlay, drawn UNDER the dots.
    // Dark-yellow = PSKR-sourced ([linecolor]); blue = on-air mesh,
    // drawn after (over) the yellow. My view: yellow center lines for
    // PSKR reporters only (radio stations carry blue edges). All
    // view: sender-to-reporter lines require both dots visible.
    // Stays visible during relay-select — the red path reads on top.
    // [hearlines] On-air heard-mesh stations: plotted in the All
    // view ALWAYS (operator 2026-08-14 — not gated on Connections;
    // stations that never report to PSK Reporter appear only here).
    // Heat-colored solid dot when WE have decoded them (our own
    // last-heard SNR); hollow gray only when position is relay-
    // learned and we've never copied them ourselves.
    // Oldest first, newest on top — now that anchors are in.
    std::sort(ordered.begin(), ordered.end(),
              [](Spot const &a, Spot const &b) { return a.when < b.when; });

    static qint64 s_dotLogMs = 0;
    if (qint64 const nowLog = QDateTime::currentMSecsSinceEpoch();
        nowLog - s_dotLogMs > 10000) {
        s_dotLogMs = nowLog;
    qCWarning(mqttclient_js8).nospace()
        << "[DOTLOG] stations=" << dotSeen << " ofWhichPskr=" << dotPskr
        << " | dropped: toggleHid=" << dotHidden
        << " noClock=" << dotNoClock
        << " tooOldPskr=" << dotOldPskr
        << " tooOldRadio=" << dotOldRadio
        << " outOfBand=" << dotOutOfBand              // [passband #218]
        << " | drawn=" << render.size()
        << " ofWhichFreqUnknown=" << dotFreqUnknown  // [passband #218]
        << " ofWhichPskr=" << dotDrawnPskr
        << " | window=" << m_viewWindowSecs << "s showPskr=" << m_showPskr;
    }

    // [inkdensity 2026-08-21] ADAPTIVE PSKR LINE COLOUR.
    // At greyline the internet mesh is ~1400 segments and solid yellow
    // reads as a hairball. PSKReporter displays this same density
    // legibly (operator), and the way it does it is to let the busy
    // case go quiet rather than to discard data. So the colour is a
    // function of how much ink is actually on the chart:
    //
    //   coverage = (total yellow length x pen width) / chart area
    //
    // Length matters as much as count -- a dozen transatlantic lines
    // fill more map than a hundred local ones -- and the ratio is
    // resolution-independent, so it self-corrects on zoom, on resize,
    // and when the time window is shortened (which is the operator's
    // primary density control and must keep working).
    //
    // Below CLEAR the colour is untouched; at SATURATED it is fully
    // muted toward a dark warm grey; linear between. The LEGEND swatch
    // uses this same value, so what the legend teaches is always what
    // the map is drawing.
    QColor pskrLineColor{165, 138, 18, 190};
    double pskrCoverage = 0.0;
    // [hovertrace 2026-08-29, operator] The hover tracer works even
    // with the Connections overlay OFF: hovering a station lifts
    // ITS lines (and paints our callsign when one reaches us) while
    // the base mesh stays hidden -- same rationale as the relay hop
    // pills ignoring the callsign toggle: the moment you ask is the
    // moment the toggle is off.
    m_tracerHitsMe = false; // reset even when the block is skipped:
                            // the label must erase the moment the
                            // hover ends (operator, 2026-08-29)
    if (m_showConnections || !m_hoverCall.isEmpty()) {
        bool const overlayOn = m_showConnections;
        auto const cutoffH = now.addSecs(-m_viewWindowSecs);
        // [linelog] WHY A LINE IS NOT THERE. Operator, 2026-08-24: with
        // Show-all and Add-PSKR both on, no yellow or grey lines at any
        // window -- then minutes later they were all back. The layer is
        // gated THREE times over and none of it was visible: the edge
        // must be fresher than the window, and BOTH endpoints must be
        // plotted, which needs each station's own spot fresher than the
        // same window. Internet edges arrive in bursts (2 in one minute
        // against 324 in another), so in a lull at a short window all
        // three rarely hold at once and only the radio mesh survives --
        // we decode those stations every cycle, so their spots never go
        // stale. One line per paint says which gate did it.
        int seenPskr = 0, seenRadio = 0, oldEdge = 0, noEnd = 0,
            hidPskr = 0, drewPskr = 0, drewRadio = 0,
            outOfBandEdge = 0;   // [passband #218]
        QPen const penRadio{QColor(90, 160, 255, 210), 1};   // on-air
        auto const &hearers = m_hearingByBand.value(m_currentBand);
        QString const myUp = m_myCall.toUpper();
        // [lineperf 2026-08-21] COLLECT, THEN DRAW PER COLOUR. At
        // greyline this loop went from ~9 lines to ~1400, and the old
        // shape paid for it twice: a setPen() for every edge, and a
        // save()/setBrush()/restore() around every arrowhead. Grouping
        // by colour makes that four state changes instead of several
        // thousand, with identical output. The arrowhead geometry is
        // unchanged.
        struct Seg { QPointF hearer, heard; bool head; bool hover; };
        QVector<Seg> segRadio, segPskr;
        // [hoverlift] Whether a line touches the hovered station is
        // known here, where both endpoint callsigns are in hand. The
        // decision to USE it is made at draw time, because it depends
        // on the density, which is not computed until the loop ends.
        QString const hoverUp = m_hoverCall.toUpper();
        m_tracerHitsMe = false;
        for (auto h = hearers.constBegin(); h != hearers.constEnd(); ++h) {
            for (auto ed = h.value().heard.constBegin();
                 ed != h.value().heard.constEnd(); ++ed) {
                bool const isPskr =
                    ed.value().source == QStringLiteral("mqtt");
                (isPskr ? seenPskr : seenRadio)++;   // [linelog]
                if (ed.value().when < cutoffH) {
                    ++oldEdge;                       // [linelog]
                    continue;
                }
                // [pskrtoggle] Internet lines are exactly what the
                // button hides. Radio lines are never affected by it.
                if (isPskr && !m_showPskr) {
                    ++hidPskr;                       // [linelog]
                    continue;
                }
                // [passband #218] No line to or from a station the
                // filter dropped this paint -- it is not on the map.
                if (passbandDropped.contains(h.key()) ||
                    passbandDropped.contains(ed.key())) {
                    ++outOfBandEdge;                 // [linelog]
                    continue;
                }
                QPointF from, to;
                bool head = false;
                if (!m_viewAll) {
                    // [viewedges] MY view: only "X heard ME", drawn
                    // from X to my triangle; no head, every line
                    // connects to me by definition.
                    if (ed.key() != myUp ||
                        !posByCall.contains(h.key()))
                        continue;
                    from = posByCall.value(h.key());
                    to = center;
                } else {
                    if (!posByCall.contains(h.key()) ||
                        !posByCall.contains(ed.key())) {
                        ++noEnd;                     // [linelog]
                        continue;
                    }
                    (isPskr ? drewPskr : drewRadio)++;   // [linelog]
                    // [heararrow] h.key() HEARS ed.key(), so the head
                    // points at h.key(): an edge A->B says traffic
                    // flows B to A, and routing reads that direction.
                    from = posByCall.value(h.key());
                    to = posByCall.value(ed.key());
                    head = true;
                }
                bool const touchesHover =
                    !hoverUp.isEmpty() &&
                    (h.key().toUpper() == hoverUp ||
                     ed.key().toUpper() == hoverUp);
                (isPskr ? segPskr : segRadio)
                    .append(Seg{from, to, head, touchesHover});
            }
        }
        // [linelog] One line per paint saying which gate emptied the
        // connections layer. Read it as: of the edges in the store, how
        // many were too old for the window, how many the PSKR button
        // hid, and how many had an endpoint that was not plotted --
        // that last one being the surprise, since it makes the line
        // layer depend on the DOT layer's freshness as well as its own.
        static qint64 s_lineLogMs = 0;
        if (qint64 const nowLog = QDateTime::currentMSecsSinceEpoch();
            nowLog - s_lineLogMs > 10000) {
            s_lineLogMs = nowLog;
        qCWarning(mqttclient_js8).nospace()
            << "[LINELOG] edges pskr=" << seenPskr << " radio=" << seenRadio
            << " | dropped: old=" << oldEdge << " pskrHidden=" << hidPskr
            << " endpointMissing=" << noEnd
            << " outOfBand=" << outOfBandEdge     // [passband #218]
            << " | collected pskr=" << drewPskr << " radio="
            << drewRadio
            << " | window=" << m_viewWindowSecs << "s showPskr="
            << m_showPskr << " showConn=" << m_showConnections
            << " viewAll=" << m_viewAll
            << " hover=" << m_hoverCall
            << " plotted=" << posByCall.size();
        }
        // [inkdensity 2026-08-22] METRIC: THE NUMBER OF YELLOW LINES.
        // Operator's call, after trying ink-coverage and peak-cell
        // density: "just use the number of lines".
        //
        // It is the metric you can predict in your head while tuning by
        // eye, and the fancier ones each failed in practice. Ink
        // coverage averaged the solid United States against the empty
        // Atlantic and reported "medium". Peak-cell density was worse:
        // every line in the MY view terminates on my triangle, so the
        // centre cell pinned the peak at maximum and the bright end
        // became unreachable no matter how sparse the map was.
        //
        // A plain count has neither failure mode. It does ignore line
        // LENGTH and off-screen segments -- accepted deliberately; the
        // time-window buttons remain the real density control and this
        // only decides how loudly the layer speaks.
        constexpr int FULL_MUTE_LINES = 500;  // count at full grey
        double const u =
            std::clamp(static_cast<double>(segPskr.size()) /
                           FULL_MUTE_LINES,
                       0.0, 1.0);
        // Weighted late so modest crowding keeps its colour; continuous
        // so nothing pops as lines come and go.
        //   50 lines ->  1% muted     250 lines -> 25%
        //  150 lines ->  9%           500 lines -> full grey
        pskrCoverage = u * u;
        {
            // Reverted to the previous maximum brightness (operator,
            // 2026-08-22: "i was wrong about increasing the max
            // brightness of the yellow").
            QColor const sparse{225, 190, 30, 225};  // bright yellow
            // Muted must mean RECEDED, never ERASED: the chart
            // background is (16,16,24), so the dark end stays clearly
            // above it. NEUTRAL channels -- an R>G>B "grey" reads as
            // brown, i.e. residual yellow rather than muting.
            QColor const dense{82, 82, 86, 170};     // neutral grey
            auto const mix = [t = pskrCoverage](int a1, int b1) {
                return static_cast<int>(a1 + (b1 - a1) * t);
            };
            pskrLineColor = QColor(mix(sparse.red(), dense.red()),
                                   mix(sparse.green(), dense.green()),
                                   mix(sparse.blue(), dense.blue()),
                                   mix(sparse.alpha(), dense.alpha()));
        }

        // [paintperf 2026-08-23] MEASURED: 5785 segments per frame and
        // 126-791 ms to paint one, with a 3-30 ms gap before it -- so
        // the frame itself is the whole cost, not any delay reaching
        // it. Two things follow.
        //
        // CULL. Most of those segments are wholly outside the widget
        // at any real zoom, and Qt still transforms and clips every
        // one. A rectangle test is far cheaper than the clip.
        QRectF const visible{0, 0, static_cast<qreal>(w),
                             static_cast<qreal>(h)};
        auto const onScreen = [&visible](Seg const &sg) {
            return QRectF(sg.heard, sg.hearer)
                .normalized()
                .adjusted(-24, -24, 24, 24)
                .intersects(visible);
        };
        // NO ARROWHEADS WHILE DRAGGING. Each head is a QPainterPath
        // built and filled per segment -- thousands of them per frame,
        // for a direction cue nobody reads mid-drag. They come back
        // the moment the drag ends.
        bool const heads = !m_dragging;
        auto const drawGroup = [&](QVector<Seg> const &segs,
                                   QPen const &pen) {
            if (segs.isEmpty())
                return;
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            // ONE drawLines() FOR THE WHOLE GROUP. Thousands of
            // individual drawLine() calls each re-enter the paint
            // engine; the batched form lets it set up once.
            QVector<QLineF> batch;
            batch.reserve(segs.size());
            for (Seg const &sg : segs)
                if (onScreen(sg))
                    batch.append(QLineF{sg.heard, sg.hearer});
            if (!batch.isEmpty())
                p.drawLines(batch);
            if (!heads)
                return;
            // Heads share one brush/pen state for the whole group.
            p.setBrush(pen.color());
            p.setPen(Qt::NoPen);
            for (Seg const &sg : segs) {
                if (!sg.head || !onScreen(sg))
                    continue;
                double const len = QLineF(sg.heard, sg.hearer).length();
                if (len < 24.0)
                    continue; // too short for a legible head
                double const ang =
                    std::atan2(sg.hearer.y() - sg.heard.y(),
                               sg.hearer.x() - sg.heard.x());
                QPointF const dir{std::cos(ang), std::sin(ang)};
                double const sz = 8.0;
                double const off = len >= 2.0 * (3.0 * sz) + 6.0
                                       ? 3.0 * sz
                                       : sz * 0.4;
                QPointF const tip =
                    (sg.hearer + sg.heard) / 2.0 + dir * off;
                QPainterPath head(tip);
                head.lineTo(tip - QPointF(std::cos(ang - 0.45),
                                          std::sin(ang - 0.45)) * sz);
                head.lineTo(tip - QPointF(std::cos(ang + 0.45),
                                          std::sin(ang + 0.45)) * sz);
                head.closeSubpath();
                p.drawPath(head);
            }
            p.setBrush(Qt::NoBrush);
        };
        // [hoverlift 2026-08-22, operator] Muting is what makes a dense
        // field readable, and it is also what buries the one station
        // being looked at. So while the cursor is on a station AND the
        // yellow has actually backed off, that station's lines are put
        // back to full brightness and drawn last, above everything.
        // Only when pskrCoverage > 0: with no muting there is nothing
        // to restore, and lifting lines then would be a change with no
        // cause.
        // [tracer 2026-08-27, TODO #183 + operator revision] The
        // tracer works at EVERY density -- on a sparse map the old
        // gate (muting active) made hover change nothing, and even
        // the unmuted yellow was hard to follow. Lifted lines draw
        // in the max yellow TRENDED TO WHITE; when a lifted line
        // ends at our own station, the callsign paints beside the
        // center triangle in the same colour for the duration of
        // the hover.
        bool const lift = !hoverUp.isEmpty();
        if (!lift) {
            drawGroup(segPskr, QPen{pskrLineColor, 1}); // internet under
            m_lastSegCount = segPskr.size() + segRadio.size();
        drawGroup(segRadio, penRadio); // on-air over ([hearlines])
        } else {
            // Internet lines ONLY lift (operator: "that's the
            // design, keep it that way"); lifted lines draw LAST, on
            // top of the blue. [tracer3] The lift is PROPORTIONAL:
            // "noticeably (and proportionally) brighter -- for gray,
            // that's toward yellow. when yellow, that's toward
            // white." The base runs sparse-yellow -> gray with
            // muting (pskrCoverage); the lift advances the SAME ramp
            // one stage: fully muted gray lifts to the sparse
            // yellow, unmuted yellow lifts toward white, mid-mute
            // lands in between.
            QVector<Seg> dim, lit;
            for (Seg const &s : segPskr)
                (s.hover ? lit : dim).append(s);
            if (overlayOn) {
                drawGroup(dim, QPen{pskrLineColor, 1});
                drawGroup(segRadio, penRadio);
            }
            QColor const yellow{225, 190, 30, 255};
            QColor const white{255, 250, 225, 255};
            double const t = 1.0 - pskrCoverage;  // 1 = unmuted base
            auto const mix = [&](int a, int b) {
                return int(a + (b - a) * t);
            };
            QColor const tracerColor{mix(yellow.red(), white.red()),
                                     mix(yellow.green(), white.green()),
                                     mix(yellow.blue(), white.blue()),
                                     255};
            // [operator fix 2026-08-29] With the overlay OFF, hover
            // shows OUR CALLSIGN only -- no lifted yellow/gray
            // lines; the lit set is still computed for the
            // reaches-us test below.
            if (overlayOn)
                drawGroup(lit, QPen{tracerColor, 1});
            m_lastSegCount = segPskr.size() + segRadio.size();
            auto const nearCenter = [&](QPointF const &pt) {
                return (pt - center).manhattanLength() < 1.0;
            };
            for (Seg const &s : lit)
                if (nearCenter(s.hearer) || nearCenter(s.heard))
                    m_tracerHitsMe = true;
            // [operator 2026-08-29] The hovered station hearing us
            // over RADIO (blue line) shows our callsign too -- not
            // just the PSKR-reported (yellow/gray) case.
            for (Seg const &s : segRadio)
                if (s.hover &&
                    (nearCenter(s.hearer) || nearCenter(s.heard)))
                    m_tracerHitsMe = true;
        }
    }

    m_screenSpots.clear();
    if (m_nonRelayersDirty)
        refreshNonRelayers(); // [nonrelayer] lazy, event-driven
    for (Spot const &s : ordered) {
        // [radioage] Fade on the SAME clock that governs presence —
        // with PSKR off, internet refreshes must not keep a dot
        // looking fresh (audit item 3).
        float const age =
            std::clamp(static_cast<float>(
                           effectiveWhen(s).secsTo(now)) /
                           static_cast<float>(m_viewWindowSecs),
                       0.0f, 1.0f);
        float const alpha = 1.0f - 0.5f * age;
        // TRUE radial position — no outer-ring clamp (pan model:
        // zoomed-in views push far spots off-window, and dragging
        // reaches them at full radial resolution). Painter clips
        // off-pixmap dots; auto scale still covers every spot, so
        // nothing is beyond the ring in Auto anyway.
        float const r = R * (s.distance / scaleKm);
        double const rad = s.azimuth * DEG2RAD;
        QPointF const pos =
            center + QPointF{std::sin(rad), -std::cos(rad)} * r;

        // Tiny solid circle (operator choice), SNR heat color, age
        // fade in the alpha; thin dark outline for contrast on land.
        // [mondots] Receive-only reporters: hollow gray — no SNR of
        // their own to heat-color.
        // [allhollow][snrwho] ONE rule for both views: solid heat
        // only from a report of MY signal (audit item 8).
        // [maptruth #3] colour from the DATED report; one older than
        // the store's own retention paints as no-report, so the
        // colour, the hollow rule and the hover can no longer
        // contradict each other.
        auto const rep = reportedToMe(s);
        int const paintSnr =
            (rep.when.isValid() &&
             rep.when.secsTo(DriftingDateTime::currentDateTimeUtc())
                 <= WINDOW_SECS)
                ? rep.snr : -99;
        bool const hollow = s.monitorOnly || paintSnr <= -99;
        if (hollow) {
            p.setPen(QPen{QColor(170, 170, 185,
                                 static_cast<int>(220 * alpha)), 1});
            p.setBrush(Qt::NoBrush);
        } else {
            p.setPen(QPen{QColor(0, 0, 0, 180), 1});
            p.setBrush(snrColor(paintSnr, alpha));
        }
        p.drawEllipse(pos, DOT_RADIUS_PX, DOT_RADIUS_PX);

        // [presencelog 453, temporary] PSKR display is OFF yet this
        // dot is on screen: name its clocks. The field mystery
        // (2026-08-29: KS1DMD "seen 334 s ago" with zero decodes in
        // 95 min) hinges on whether radioWhen is absent (the
        // effectiveWhen fallback hole) or somehow fresh.
        if (!m_showPskr) {
            qint64 const whenAge =
                s.when.isValid() ? s.when.secsTo(now) : -1;
            qint64 const radioAge =
                s.radioWhen.isValid() ? s.radioWhen.secsTo(now) : -1;
            if (radioAge < 0 || radioAge > m_viewWindowSecs) {
                static qint64 s_presLogMs = 0;
                qint64 const nowMs =
                    QDateTime::currentMSecsSinceEpoch();
                if (nowMs - s_presLogMs > 10000) {
                    s_presLogMs = nowMs;
                    qCWarning(mqttclient_js8)
                        << "[PRESENCELOG] pskrOff dot w/ stale-or-no"
                        << "radio evidence:" << s.receiverCall
                        << "whenAge=" << whenAge
                        << "radioAge=" << radioAge
                        << "(-1 = radioWhen UNSET)";
                }
            }
        }

        // [nonrelayer + hbrelay, operator 2026-08-29] Relay status as
        // a dashed ring, same precedence as the hover lines: RED =
        // transmits (within the hour) but declined two asks today;
        // GREEN = relay-enabled (announced via the HB flag, or a
        // successful forward on the 90-day record). The fresh
        // negative outranks the positive.
        QString const callU = s.receiverCall.toUpper();
        bool const ringRed =
            m_nonRelayers.contains(callU) && s.lastTxWhen.isValid() &&
            s.lastTxWhen.secsTo(
                DriftingDateTime::currentDateTimeUtc()) <= WINDOW_SECS;
        bool const ringGreen =
            !ringRed && (m_flagRelayers.contains(callU) ||
                         m_knownRelayers.contains(callU));
        if (ringRed || ringGreen) {
            // red = 2 px (operator 2026-08-29); green stays 1 px
            QPen ring{ringRed
                          ? QColor(225, 60, 60,
                                   static_cast<int>(200 * alpha))
                          : QColor(70, 200, 90,
                                   static_cast<int>(200 * alpha)),
                      ringRed ? 2.0 : 1.0};
            ring.setStyle(Qt::DashLine);
            p.setPen(ring);
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(pos, DOT_RADIUS_PX + 2.5,
                          DOT_RADIUS_PX + 2.5);
        }

        m_screenSpots.append({pos, s});
    }

    // Callsign labels: on-map option, default off (hover tooltip
    // provides identity; toggle UI to follow).
    if (m_showCallsigns) {
        // [callsbtn] White on transparent, to the RIGHT of the dot,
        // clear of it (operator 2026-08-15; both views).
        p.setPen(Qt::white);
        p.setBrush(Qt::NoBrush);
        for (auto const &ss : m_screenSpots) {
            p.drawText(QRectF{ss.pos.x() + DOT_RADIUS_PX + 5.0,
                              ss.pos.y() - 9.0, 140.0, 18.0},
                       Qt::AlignLeft | Qt::AlignVCenter,
                       ss.spot.receiverCall);
        }
    }

    // [relaysel] Relay path on TOP of the dots: my triangle -> hop 1
    // -> hop 2 -> ... Bright green so it reads over the dimmer
    // Connections style; selected stations get a ring. Hops whose
    // station has scrolled out of the visible dataset keep their slot
    // in the path (the template still includes them) but draw no
    // segment.
    if (m_relaySelect && !m_relayPath.isEmpty()) {
        // 2 px red (operator revision 2026-08-14) — distinct from the
        // 1 px green Connections mesh underneath.
        p.setPen(QPen{QColor(235, 60, 60, 230), 2});
        p.setBrush(Qt::NoBrush);
        QPointF prev = center;
        bool prevKnown = true;
        for (QString const &call : m_relayPath) {
            if (!posByCall.contains(call)) {
                prevKnown = false;
                continue;
            }
            QPointF const pos = posByCall.value(call);
            if (prevKnown)
                p.drawLine(prev, pos);
            p.drawEllipse(pos, DOT_RADIUS_PX + 3.0, DOT_RADIUS_PX + 3.0);
            prev = pos;
            prevKnown = true;
        }
    }

    // Title strip (backfilled — pan can slide outlines under it).
    p.fillRect(QRectF{0, 0, static_cast<qreal>(w),
                      static_cast<qreal>(TITLE_STRIP_PX)},
               QColor(16, 16, 24));
    p.setPen(QColor(220, 220, 235));
    QFont title = p.font();
    title.setPointSize(9);
    p.setFont(title);
    QString const bandText =
        m_currentBand.isEmpty() ? tr("no band") : m_currentBand;
    // [spotfmt] "last X of Y min" while accumulation is younger than
    // the selected window; plain "last Y min" once it's full.
    int const viewMin = m_viewWindowSecs / 60;
    qint64 const accumMin =
        m_accumStart.isValid()
            ? std::max<qint64>(1, m_accumStart.secsTo(now) / 60)
            : viewMin;
    QString const windowText =
        accumMin < viewMin
            ? tr("last %1 of %2 min").arg(accumMin).arg(viewMin)
            : tr("last %1 min").arg(viewMin);
    QString headline =
        m_viewAll
            // [viewall] All view counts heard stations, not
            // spotters of me.
            ? tr("All stations — %1 — %2 spots / %3")
                  .arg(bandText)
                  .arg(ordered.size())
                  .arg(windowText)
            : tr("%1 @ %2 — %3 — %4 spotters / %5")
                  .arg(m_myCall, m_myGrid, bandText)
                  .arg(ordered.size()) // [posauth] anchors count too
                  .arg(windowText);
    // [pskrtoggle] The MQTT connection state is only relevant while
    // internet-sourced spots are shown.
    if (m_showPskr && !m_stateText.isEmpty())
        headline += QStringLiteral(" — ") + m_stateText;
    p.drawText(QRectF{0, 0, static_cast<qreal>(w), TITLE_STRIP_PX},
               Qt::AlignCenter, headline);

    if (ordered.isEmpty()) { // [onairscale] anchors count as spots
        p.setPen(QColor(150, 150, 170));
        // One line height below the WINDOW center (not the panned
        // chart center) — dead-center overlapped the own-station
        // marker graphic.
        qreal const drop = p.fontMetrics().height();
        p.drawText(QRectF{0, (center.y() - m_panPx.y()) - 30 + drop,
                          static_cast<qreal>(w), 60},
                   Qt::AlignCenter, tr("No new spots yet"));
    }

    // Legend: SNR gradient bar (strip backfilled, same reason as the
    // title strip).
    {
        p.fillRect(QRectF{0, static_cast<qreal>(h - LEGEND_STRIP_PX),
                          static_cast<qreal>(w),
                          static_cast<qreal>(LEGEND_STRIP_PX)},
                   QColor(16, 16, 24));
        // [scalebar] Bar width = the chart radius in pixels, so the
        // scale legend above it IS the radius length (operator
        // 2026-08-15); SNR bar matches it.
        qreal const barW = static_cast<qreal>(R);
        // [operator 2026-09-01] The center info display (congestion,
        // distance, SNR bar) centers between the FACING edges of the
        // two button stacks, not the window -- same rule as the
        // overlay panels.
        qreal const cx =
            (m_leftColW > 0 && m_btnColW > 0)
                ? (6.0 + m_leftColW + (w - 6.0 - m_btnColW)) / 2.0
                : w / 2.0;
        QRectF bar{cx - barW / 2.0,
                   static_cast<qreal>(h - LEGEND_STRIP_PX + 8), barW, 10};
        {
            // [operator 2026-08-28] Bar and its distance legend sit
            // 2/3 of the legend font height lower than before.
            qreal const yLine = h - LEGEND_STRIP_PX - 10.0
                + QFontMetricsF{p.font()}.height() * 2.0 / 3.0;
            p.setPen(QPen{QColor(190, 190, 210), 1});
            p.drawLine(QPointF{bar.left(), yLine},
                       QPointF{bar.right(), yLine});
            p.drawLine(QPointF{bar.left(), yLine - 4},
                       QPointF{bar.left(), yLine + 4}); // end ticks
            p.drawLine(QPointF{bar.right(), yLine - 4},
                       QPointF{bar.right(), yLine + 4});
            p.setPen(QColor(190, 190, 210));
            p.drawText(QRectF{bar.left(), yLine - 20.0, barW, 16.0},
                       Qt::AlignHCenter | Qt::AlignBottom,
                       QStringLiteral("%1 %2")
                           .arg(scaleLabelValue())
                           .arg(unitLabel));
            // [congestion, operator 2026-08-31] Band congestion
            // index just above the distance legend: 1-10 =
            // occupied-slot fraction of the trailing 20 slots.
            if (m_congestionProbe) {
                int const ci = m_congestionProbe();
                int const pi = pskrCongestionIndex();
                // [operator 2026-09-01] Color-code by which metric
                // is IN EFFECT for the executor: the ruling number
                // gets the green/amber/red scale, the other stays
                // neutral. PSKR selected without data = on-air is
                // ruling (the fallback).
                int mode = -1;
                if (m_waitProbe)
                    mode = m_waitProbe().mode;
                bool const pskrRules =
                    mode == 3 && pskrDataAvailable();
                bool const onAirRules = mode == 1 || (mode == 3 &&
                                                      !pskrRules);
                auto const scale = [](int v) {
                    return v >= 7   ? QColor(235, 120, 110)
                           : v >= 4 ? QColor(230, 200, 120)
                                    : QColor(150, 210, 150);
                };
                QColor const neutral{170, 170, 185};
                QString const head = tr("Band congestion: ");
                QString const sOn = tr("%1/10 (on-air)").arg(ci);
                QString const sSep = QStringLiteral(", ");
                QString const sPk = tr("%1/10 (PSKR)").arg(pi);
                QFontMetricsF const fm{p.font()};
                qreal x = bar.center().x() -
                          fm.horizontalAdvance(head + sOn + sSep +
                                               sPk) / 2.0;
                qreal const ty = yLine - 38.0 + 16.0 - fm.descent();
                struct Seg { QString t; QColor c; };
                for (Seg const &s : {
                         Seg{head, neutral},
                         Seg{sOn, onAirRules ? scale(ci) : neutral},
                         Seg{sSep, neutral},
                         Seg{sPk, pskrRules ? scale(pi) : neutral}}) {
                    p.setPen(s.c);
                    p.drawText(QPointF{x, ty}, s.t);
                    x += fm.horizontalAdvance(s.t);
                }
            }
        }
        QLinearGradient lg{bar.topLeft(), bar.topRight()};
        for (int i = 0; i <= 10; ++i) {
            float const t = i / 10.0f;
            lg.setColorAt(t, snrColor(SNR_COLD +
                                          static_cast<int>(
                                              t * (SNR_HOT - SNR_COLD)),
                                      1.0f));
        }
        p.fillRect(bar, lg);
        p.setPen(QColor(190, 190, 210));
        p.setFont(small);
        p.drawText(QPointF{bar.left(), bar.bottom() + 14},
                   QStringLiteral("%1 dB").arg(SNR_COLD));
        p.drawText(QPointF{bar.center().x() - 24, bar.bottom() + 14},
                   tr("SNR (dB)"));
        p.drawText(QPointF{bar.right() - 24, bar.bottom() + 14},
                   QStringLiteral("+%1").arg(SNR_HOT));
    }

    // [connlegend] Line-type legend under the Connections button
    // (operator 2026-08-14): blue = on-air heard-mesh, dark yellow =
    // PSKReporter MQTT. Solid 1 px swatches, right-aligned; subtle
    // backdrop so the labels read over busy map areas. Shown only
    // while the Connections overlay is on (operator 2026-08-15).
    // [pskrtoggle] Legend shown only with Connections on AND PSKR
    // spots added in (operator 2026-08-15: hide BOTH lines otherwise).
    if (m_showConnections && m_showPskr) {
        QFont lf = p.font();
        lf.setPointSize(8);
        p.setFont(lf);
        struct Row { QColor c; QString label; };
        Row const rows[2] = {
            {QColor(90, 160, 255),
             tr("Heard by %1").arg(m_myCall.isEmpty() ? tr("me")
                                                      : m_myCall)},
            // [inkdensity] THE SAME value the lines were drawn with --
            // read from the variable, never re-specified, so the legend
            // cannot teach a colour the map is not using (it did
            // exactly that once when the line was darkened alone).
            {pskrLineColor, pskrCoverage > 0.25
                                ? tr("Heard by PSKR")
                                : tr("Heard by PSKR")}};
        int const rowH = 11;
        qreal const ascent = p.fontMetrics().ascent();
        // [operator 2026-08-29] ONE text x-position for both legend
        // variants: width over ALL four labels, so swapping legends
        // never shifts the text column.
        qreal maxW = 0;
        for (Row const &r : rows)
            maxW = std::max(
                maxW, static_cast<qreal>(
                          p.fontMetrics().horizontalAdvance(r.label)));
        for (QString const &l :
             {tr("Relay enabled"), tr("Relay disabled?")})
            maxW = std::max(
                maxW, static_cast<qreal>(
                          p.fontMetrics().horizontalAdvance(l)));
        // [operator 2026-09-01, rev3] Bottom-right corner, under
        // the Show connections button (the column stops above us).
        qreal const xRight = w - 6.0;
        // [operator 2026-08-29] top row lifted 1/3 font height, same
        // as the relay legend's top row.
        qreal const extra = p.fontMetrics().height() / 3.0;
        // [operator 2026-09-01, rev4] Rows dropped so the box
        // bottom is 6 px above the WINDOW edge (was ~a button
        // height higher); the button column's floor tracks this.
        qreal const y0 = h - 6.0 - 2.0 * rowH - 2.0;
        p.fillRect(QRectF{xRight - maxW - 28, y0 - 2 - extra,
                          maxW + 28 + 4, 2.0 * rowH + 4 + extra},
                   QColor(16, 16, 24, 170));
        for (int i = 0; i < 2; ++i) {
            qreal const y = y0 + i * rowH - (i == 0 ? extra : 0.0);
            qreal const xText = xRight - maxW;
            p.setPen(QColor(205, 205, 220));
            p.drawText(QPointF{xText, y + ascent - 1}, rows[i].label);
            p.setPen(QPen{rows[i].c, 1});
            qreal const ymid = y + rowH / 2.0 - 1;
            p.drawLine(QPointF{xText - 24, ymid},
                       QPointF{xText - 6, ymid});
        }
    } else {
        // [relaylegend, operator 2026-08-29] Same position, shown
        // when the PSKR legend is NOT: the relay-status rings.
        QFont lf = p.font();
        lf.setPointSize(8);
        p.setFont(lf);
        struct Row { QColor c; qreal penW; QString label; };
        Row const rows[2] = {
            {QColor(70, 200, 90), 1.0, tr("Relay enabled")},
            {QColor(225, 60, 60), 2.0, tr("Relay disabled?")}};
        int const rowH = 11;
        qreal const ascent = p.fontMetrics().ascent();
        // [operator 2026-08-29] shared text column with the PSKR
        // legend: width over ALL four labels.
        qreal maxW = 0;
        for (Row const &r : rows)
            maxW = std::max(
                maxW, static_cast<qreal>(
                          p.fontMetrics().horizontalAdvance(r.label)));
        for (QString const &l :
             {tr("Heard by %1").arg(m_myCall.isEmpty() ? tr("me")
                                                       : m_myCall),
              tr("Heard by PSKR")})
            maxW = std::max(
                maxW, static_cast<qreal>(
                          p.fontMetrics().horizontalAdvance(l)));
        // [operator 2026-09-01, rev3] Bottom-right corner, under
        // the Show connections button (the column stops above us).
        qreal const xRight = w - 6.0;
        // [operator 2026-08-29 rev2] Only the TOP row lifts 1/3 of
        // the font height -- the two rings touched at the 11 px
        // pitch; the bottom row stays aligned with the PSKR legend's
        // position.
        qreal const extra = p.fontMetrics().height() / 3.0;
        // [operator 2026-09-01, rev4] Rows dropped so the box
        // bottom is 6 px above the WINDOW edge (was ~a button
        // height higher); the button column's floor tracks this.
        qreal const y0 = h - 6.0 - 2.0 * rowH - 2.0;
        p.fillRect(QRectF{xRight - maxW - 28, y0 - 2 - extra,
                          maxW + 28 + 4, 2.0 * rowH + 4 + extra},
                   QColor(16, 16, 24, 170));
        for (int i = 0; i < 2; ++i) {
            qreal const y = y0 + i * rowH - (i == 0 ? extra : 0.0);
            qreal const xText = xRight - maxW;
            p.setPen(QColor(205, 205, 220));
            p.drawText(QPointF{xText, y + ascent - 1}, rows[i].label);
            QPointF const symAt{xText - 15, y + rowH / 2.0 - 1};
            // [operator 2026-08-29] empty station dot inside the
            // ring, matching how rings read on the map.
            p.setPen(QPen{QColor(170, 170, 185, 220), 1});
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(symAt, qreal(DOT_RADIUS_PX),
                          qreal(DOT_RADIUS_PX));
            QPen ring{rows[i].c, rows[i].penW};
            ring.setStyle(Qt::DashLine);
            p.setPen(ring);
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(symAt, DOT_RADIUS_PX + 2.5,
                          DOT_RADIUS_PX + 2.5);
        }
    }

    // [attemptviz 2026-08-22, operator] What we are trying, RIGHT NOW,
    // drawn LAST so it sits above every other layer including the
    // hover lift -- an attempt is transient and about this moment, so
    // nothing should ever obscure it.
    //
    // The dash GAP widens as the reply window runs down and the whole
    // line fades with it, so the path visibly "runs out" instead of
    // just blinking off. A reply turns the chain solid green for a few
    // seconds ([attemptviz] GREEN_SECS).
    if (!m_attempts.isEmpty()) {
        QHash<QString, QPointF> at;
        for (ScreenSpot const &ss : m_screenSpots)
            at.insert(ss.spot.receiverCall.toUpper(), ss.pos);
        // An attempt is about RIGHT NOW and must not be filtered by the
        // view. In "hearing <me>" only stations that hear us are
        // rendered, so a relay hop outside that set had no entry above
        // and the line silently truncated when the view was switched
        // (operator, 2026-08-22 -- same requirement as the relay
        // builder). Fall back to the grid authority, which knows where
        // a station is regardless of what is currently drawn.
        auto const placeFromGrid = [&](QString const &call) -> QPointF {
            QString const g = m_gridByCall.value(call);
            if (g.isEmpty())
                return QPointF{};
            auto const v = Geodesic::vector(m_myGrid, g);
            if (!v.azimuth().isValid() || !v.distance().isValid())
                return QPointF{};
            float const rr =
                R * (static_cast<float>(v.distance()) / scaleKm);
            double const rad = static_cast<float>(v.azimuth()) * DEG2RAD;
            return center + QPointF{std::sin(rad), -std::cos(rad)} * rr;
        };
        auto const now = DriftingDateTime::currentDateTimeUtc();
        p.setBrush(Qt::NoBrush);
        for (Attempt const &a : m_attempts) {
            // Chain starts at OUR station; the path holds only the
            // far end(s), so the first leg is always center -> hop 1.
            QVector<QPointF> pts{center};
            for (QString const &c : a.path) {
                auto it = at.constFind(c);
                QPointF const q =
                    it != at.constEnd() ? it.value() : placeFromGrid(c);
                if (q.isNull())
                    break;   // no grid anywhere: cannot place this hop
                pts << q;
            }
            // [attemptpaint 2026-08-26] Say what was PLACED. The
            // state side provably registers attempts (live check,
            // ATTEMPTS populated) yet the operator saw no red line --
            // so if it recurs, the gap must be here, and this line
            // names it: how many of the chain's points found a
            // position. Low volume: only while an attempt is alive.
            static qint64 s_attLogMs = 0;
            if (qint64 const nowLog =
                    QDateTime::currentMSecsSinceEpoch();
                nowLog - s_attLogMs > 10000) {
                s_attLogMs = nowLog;
                qCWarning(mqttclient_js8).nospace()
                    << "[ATTEMPT-PAINT] path=" << a.path.join(">")
                    << " placed=" << pts.size() << "/"
                    << (a.path.size() + 1)
                    << (a.replied ? " GREEN" : " red");
            }
            // Draw AS FAR AS WE CAN. Requiring the whole chain to be
            // placeable meant one hop with no grid erased the entire
            // line, which is how a three-hop attempt drew nothing at
            // all (operator, 2026-08-22: "no red line"). A partial
            // path still says what is being tried and in which
            // direction; silence says nothing.
            if (pts.size() < 2)
                continue;

            QPen pen;
            pen.setWidthF(3.0);
            // FULLY OPAQUE, both colours. The countdown is carried by
            // the widening gap alone -- an alpha ramp on top of it just
            // made the line look washed out (operator, 2026-08-22).
            // It ends by being removed, not by fading away.
            if (a.replied) {
                pen.setColor(QColor(60, 230, 110));
                pen.setStyle(Qt::SolidLine);
            } else {
                double const t =
                    std::clamp(double(a.started.secsTo(now)) /
                                   double(qMax(1, a.waitSecs)),
                               0.0, 1.0);
                // Dash stays put; the GAP opens from tight to wide.
                // Units are multiples of the pen width.
                pen.setColor(QColor(230, 60, 60));
                pen.setStyle(Qt::CustomDashLine);
                pen.setDashPattern({2.0, 1.0 + 11.0 * t});
            }
            p.setPen(pen);
            for (int i = 1; i < pts.size(); ++i)
                p.drawLine(pts.at(i - 1), pts.at(i));

            // [attemptviz] NAME EVERY HOP, always -- the same rule the
            // relay builder follows. An attempt is about specific
            // stations, so a path you cannot read the stations off is
            // half a picture; and it must not depend on the callsign
            // toggle, because the operator asking "who is that hop?"
            // is exactly when the toggle is off (operator,
            // 2026-08-22). Drawn in the path's own colour on a dark
            // pill so it stays legible over dots and lines.
            {
                QFont f = p.font();
                f.setBold(true);
                p.setFont(f);
                QFontMetricsF const fm{f};
                for (int i = 1; i < pts.size() && i <= a.path.size(); ++i) {
                    QString const &name = a.path.at(i - 1);
                    QPointF const at = pts.at(i);
                    qreal const tw = fm.horizontalAdvance(name);
                    QRectF const pill{at.x() + 7.0, at.y() - 9.0,
                                      tw + 8.0, 18.0};
                    p.setPen(Qt::NoPen);
                    p.setBrush(QColor(16, 16, 24, 205));
                    p.drawRoundedRect(pill, 3.0, 3.0);
                    p.setBrush(Qt::NoBrush);
                    p.setPen(pen.color());
                    p.drawText(pill, Qt::AlignCenter, name);
                }
                p.setPen(pen);
            }
        }
    }

    // [hometri] MY STATION, LAST. Everything else has been painted, so
    // nothing can bury it -- at greyline the centre is the densest part
    // of the chart and the triangle was disappearing under dots and
    // lines (operator, 2026-08-23). It also sits over the origin of
    // every attempt path, which is where those paths start anyway.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(230, 230, 240));
    {
        QPolygonF tri; // smaller (operator, 2026-08-14)
        tri << center + QPointF{0.0, -4.5} << center + QPointF{3.8, 3.0}
            << center + QPointF{-3.8, 3.0};
        p.drawPolygon(tri);
        // [#183] a traced line ends here: our callsign in the tracer
        // colour, placed EXACTLY like every other callsign label --
        // strictly to the right, vertically centered (operator: it
        // had drawn north of a station north of me), on the same dark
        // pill as the relay-in-progress hop labels (operator,
        // 2026-08-27) so it stays legible over the dense centre.
        if (m_tracerHitsMe && !m_myCall.isEmpty()) {
            QFontMetricsF const fm{p.font()};
            qreal const tw = fm.horizontalAdvance(m_myCall);
            QRectF const pill{center.x() + DOT_RADIUS_PX + 5.0,
                              center.y() - 9.0, tw + 8.0, 18.0};
            p.setBrush(QColor(16, 16, 24, 205));
            p.drawRoundedRect(pill, 3.0, 3.0);
            p.setBrush(Qt::NoBrush);
            p.setPen(QColor{255, 250, 225, 255});
            p.drawText(pill, Qt::AlignCenter, m_myCall);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(230, 230, 240));
        }
    }

    {
        qint64 const tookMs = paintTimer.elapsed();
        m_lastPaintMs = tookMs;   // feeds the adaptive debounce
        lastPaintEndMs = QDateTime::currentMSecsSinceEpoch();
        // Only a slow FRAME is interesting. The gap is reported for
        // context but must not TRIGGER, or an idle map logs forever --
        // long gaps are just nothing happening (2026-08-23: every line
        // in the first run fired on an idle gap while frames were 3 ms).
        // 150 ms, not 40. After the adaptive-debounce fix a normal
        // frame is 55-102 ms on the slow box, so a 40 ms threshold
        // logged every single one -- turning the watchdog back into
        // the hot-path noise it was meant to diagnose. This now fires
        // only for a frame bad enough to be felt.
        if (tookMs > 150)
            qCWarning(mqttclient_js8)
                << "[PAINT] took" << tookMs << "ms (geo" << m_lastGeoMs
                << "ms /" << m_lastGeoPts << "pts /" << m_lastGeoPolys
                << "polys, build" << m_lastBuildMs << "ms, draw"
                << (tookMs - m_lastGeoMs - m_lastBuildMs)
                << "ms), gap"
                << gapMs << "ms, notes" << m_lastNoteCount
                << "segs" << m_lastSegCount;
    }

    update();
}

void SpotMapWindow::paintEvent(QPaintEvent *) {
    QPainter p{this};
    p.drawPixmap(0, 0, m_pixmap);
}

void SpotMapWindow::positionWindowButtons() {
    if (!m_win15Btn) return;
    int const x = width() - 36 - 6; // right-aligned, mirrors zoom stack
    int y = TITLE_STRIP_PX + 6;
    for (QToolButton *b :
         {m_win5Btn, m_win15Btn, m_win30Btn, m_win60Btn}) {
        b->move(x, y);
        y += 22;
    }
    // [viewall] View-selector stack, lower-left: my call over "All",
    // bottom edge 8 px clear of the legend strip (which also carries
    // the status message). Width grows to fit the callsign (e.g.
    // "WM8Q/P" outgrows the 36 px zoom-button width); both buttons
    // share the wider size so the stack stays a clean column.
    if (m_viewMineBtn) {
        QString const callText = m_myCall.isEmpty()
            ? tr("Show stations hearing me")
            : tr("Show stations hearing %1").arg(m_myCall);
        QString const allText = tr("Show all stations");
        m_viewMineBtn->setText(callText);
        m_viewAllBtn->setText(allText);
        // Width from the buttons' OWN sizeHint — the style knows
        // its true padding; hand-added fontMetrics constants don't.
        int const wBtn = std::max(
            {36, m_viewMineBtn->sizeHint().width(),
             m_viewAllBtn->sizeHint().width(),
             m_pskrBtn ? m_pskrBtn->sizeHint().width() : 0});
        m_viewMineBtn->setFixedSize(wBtn, 20);
        m_viewAllBtn->setFixedSize(wBtn, 20);
        m_leftColW = wBtn; // [autoroute] panel centering reference
        // [pskrtoggle] Third row: one button height (20 px) of air
        // below the view pair — separate function group.
        // [operator 2026-09-01] Whole group dropped so its lowest
        // button sits a little above the WINDOW bottom edge (the
        // legend strip's left corner is empty; the SNR bar and its
        // labels are centered). Group height = 84 px (see offsets
        // below). Auto-fit's lower bound follows m_viewMineBtn->y()
        // and so enlarges with this move automatically.
        int vy = height() - 6 - 84;
        for (QToolButton *b : {m_viewMineBtn, m_viewAllBtn}) {
            b->move(6, vy);
            vy += 22;
        }
        if (m_pskrBtn) {
            m_pskrBtn->setFixedSize(wBtn, 20); // same length as pair
            m_pskrBtn->move(6, vy + 20);
        }
    }
    // [connlines] Lower right, bottom-aligned with the view stack;
    // [relaysel] the relay rows stack directly above it.
    if (m_connBtn) {
        // One shared width for the right-hand column (operator
        // 2026-08-15): Show connections, Show call signs,
        // Select relay(s) — widest sizeHint wins.
        int wConn = std::max(36, m_connBtn->sizeHint().width());
        if (m_callsBtn)
            wConn = std::max(wConn, m_callsBtn->sizeHint().width());
        if (m_relaySelBtn)
            wConn = std::max(wConn, m_relaySelBtn->sizeHint().width());
        if (m_autoRouteBtn)
            wConn = std::max(wConn, m_autoRouteBtn->sizeHint().width());
        if (m_optionsBtn)
            wConn = std::max(wConn, m_optionsBtn->sizeHint().width());
        m_btnColW = wConn; // [autoroute] panel centering reference
        // [operator 2026-09-01, rev3] The ring/PSKR legend rows
        // keep the bottom-right corner, UNDER the Show connections
        // button; the column's floor is the top of that legend box
        // (rowH 11, box pads 2+2, top row lifted fm/3).
        // rev4: mirrors the paint side -- box bottom 6 px above the
        // window edge, y0 = h - 6 - 2*rowH - 2, top = y0 - 2 - fm/3.
        int legendRowsTop;
        {
            QFont lf = font();
            lf.setPointSize(8);
            legendRowsTop = height() - 6 - 2 * 11 - 4 -
                            QFontMetrics{lf}.height() / 3;
        }
        int const yConn = legendRowsTop - 4 - 20;
        m_connBtn->setFixedSize(wConn, 20);
        m_connBtn->move(width() - wConn - 6, yConn);
        // [callsbtn] Directly above Show connections.
        if (m_callsBtn) {
            m_callsBtn->setFixedSize(wConn, 20);
            m_callsBtn->move(width() - wConn - 6, yConn - 22);
        }
        if (m_relaySelBtn) {
            // Row 2 (Done | Undo) shares the column width.
            int const half = (wConn - 4) / 2;
            m_relayDoneBtn->setFixedSize(half, 20);
            m_relayUndoBtn->setFixedSize(wConn - 4 - half, 20);
            // Relay rows lifted one button height above the calls
            // button (operator 2026-08-15) — separate function group.
            int const yRow2 = yConn - 66;
            m_relayDoneBtn->move(width() - wConn - 6, yRow2);
            m_relayUndoBtn->move(width() - wConn - 6 + half + 4, yRow2);
            m_relaySelBtn->setFixedSize(wConn, 20);
            m_relaySelBtn->move(width() - wConn - 6, yRow2 - 22);
            // [autoroute] One button height of AIR above the Select
            // button (rows are 22 apart; skip one row), per the
            // operator's layout spec. [#207 waitopts, operator
            // 2026-09-01] Auto-route lifted one further button
            // height; Options takes its old slot directly below.
            if (m_autoRouteBtn) {
                m_autoRouteBtn->setFixedSize(wConn, 20);
                m_autoRouteBtn->move(width() - wConn - 6, yRow2 - 88);
            }
            if (m_optionsBtn) {
                m_optionsBtn->setFixedSize(wConn, 20);
                m_optionsBtn->move(width() - wConn - 6, yRow2 - 66);
            }
            positionAutoRoutePanel();
            if (m_optionsPanel && m_optionsPanel->isVisible())
                positionOverlayPanel(m_optionsPanel);
            // Status line follows the legend on resize.
            if (m_statusLine && m_statusLine->isVisible())
                setStickyToast(m_stickyToast);
        }
    }
}

void SpotMapWindow::updateRelayButtons() {
    if (!m_relayDoneBtn) return;
    // A relay path needs at least one hop AND a destination — Done
    // stays disabled until two stations are clicked.
    m_relayDoneBtn->setEnabled(m_relaySelect && m_relayPath.size() >= 2);
    m_relayUndoBtn->setEnabled(m_relaySelect && !m_relayPath.isEmpty());
}

void SpotMapWindow::resizeEvent(QResizeEvent *) {
    positionWindowButtons();
    requestReplot();
}

SpotMapWindow::ScreenSpot const *
SpotMapWindow::hitTest(QPointF const &pos) const {
    ScreenSpot const *best = nullptr;
    double bestD2 = 12.0 * 12.0;
    for (auto const &ss : m_screenSpots) {
        double const dx = ss.pos.x() - pos.x();
        double const dy = ss.pos.y() - pos.y();
        if (double const d2 = dx * dx + dy * dy; d2 < bestD2) {
            bestD2 = d2;
            best = &ss;
        }
    }
    return best;
}

// [attemptviz] An outgoing directed call or relay. `path` is the chain
// WITHOUT us: {target} for a direct call, {relay..., target} otherwise.
void SpotMapWindow::noteAttempt(QStringList const &path, int waitSecs) {
    // [autoroute, operator spec 2026-08-28] The dashed-red / solid-
    // green layer draws ONLY in auto-route mode.
    // [firstline 2026-08-30] The mode gate moved to the CALLER (the
    // owner's flag): this widget's derived copy is still false when
    // move 1 transmits inside reachStart, and gating here dropped
    // the first red line.
    if (path.isEmpty())
        return;
    Attempt a;
    for (QString const &c : path)
        a.path << c.trimmed().toUpper();
    a.started = DriftingDateTime::currentDateTimeUtc();
    a.waitSecs = qMax(15, waitSecs);
    // A new call SUPERSEDES any outstanding one. The channel is
    // half-duplex, so we cannot be trying two paths at once, and
    // leaving the old dashes up would claim we were (operator,
    // 2026-08-22). Replied attempts survive: those are results, not
    // attempts, and they run out their own green timeout.
    for (int i = m_attempts.size() - 1; i >= 0; --i) {
        if (!m_attempts.at(i).replied)
            m_attempts.remove(i);
    }
    m_attempts.append(a);
    if (m_attemptTimer && !m_attemptTimer->isActive())
        m_attemptTimer->start();
    redraw();
}

// [attemptviz] Somebody answered. Any live attempt naming that station
// anywhere in its chain goes green -- a relay answering on the target's
// behalf is still our path working.
void SpotMapWindow::noteReply(QString const &from) {
    // [autoroute] Same layer as noteAttempt; [firstline] gate lives
    // at the caller, on the owner's flag.
    QString const f = from.trimmed().toUpper();
    if (f.isEmpty())
        return;
    bool hit = false;
    for (Attempt &a : m_attempts) {
        if (a.replied)
            continue;
        for (QString const &c : a.path) {
            if (Radio::same_station(c, f)) {
                a.replied = true;
                a.repliedAt = DriftingDateTime::currentDateTimeUtc();
                hit = true;
                break;
            }
        }
    }
    // Somebody answered US without matching a chain we drew -- the
    // usual case is a broadcast sweep, which has no path to draw but
    // very much has responders. The operator's rule is "any called
    // station replies to us, even HB replies", so give it its own
    // green line rather than dropping it because we never drew a red
    // one (operator, 2026-08-22: "the incoming responses to the first
    // call *are* directed (to us) so should show green").
    if (!hit) {
        Attempt a;
        a.path << f;
        a.started = DriftingDateTime::currentDateTimeUtc();
        a.replied = true;
        a.repliedAt = a.started;
        m_attempts.append(a);
        hit = true;
    }
    if (hit) {
        if (m_attemptTimer && !m_attemptTimer->isActive())
            m_attemptTimer->start();
        redraw();
    }
}

// [attemptviz] See the header: the caller declares failure.
void SpotMapWindow::clearAttempts() {
    int const before = m_attempts.size();
    for (int i = m_attempts.size() - 1; i >= 0; --i)
        if (!m_attempts.at(i).replied)
            m_attempts.remove(i);
    if (m_attempts.size() != before)
        redraw();
}

// [attemptviz] Expire finished attempts and keep the dashes moving.
// The timer stops itself when nothing is live, so an idle map costs
// nothing.
void SpotMapWindow::tickAttempts() {
    auto const now = DriftingDateTime::currentDateTimeUtc();
    constexpr int GREEN_SECS = 12;   // how long a success stays up
    for (int i = m_attempts.size() - 1; i >= 0; --i) {
        Attempt const &a = m_attempts.at(i);
        bool const done =
            a.replied ? a.repliedAt.secsTo(now) > GREEN_SECS
                      : a.started.secsTo(now) > a.waitSecs;
        if (done)
            m_attempts.remove(i);
    }
    if (m_attempts.isEmpty() && m_attemptTimer)
        m_attemptTimer->stop();
    redraw();
}

// [hoverlift] The cursor can leave the chart without a final move event
// inside it, which would strand the lift on whatever was last hovered.
void SpotMapWindow::leaveEvent(QEvent *event) {
    if (!m_hoverCall.isEmpty()) {
        m_hoverCall.clear();
        redraw();
    }
    QWidget::leaveEvent(event);
}

void SpotMapWindow::mouseMoveEvent(QMouseEvent *event) {
    // Drag-to-pan: once the press moves past the drag threshold it's
    // a pan, not a click. Direct redraw() (not the debounced
    // requestReplot) so the chart tracks the cursor smoothly; the
    // outline cache is center-relative so panning never re-projects.
    if (m_maybeDrag && (event->buttons() & Qt::LeftButton)) {
        QPointF const d = event->position() - m_pressPos;
        if (!m_dragging &&
            d.manhattanLength() >= QApplication::startDragDistance()) {
            m_dragging = true;
            setCursor(Qt::ClosedHandCursor);
            // [dragmanual 2026-08-16] Dragging EXITS Auto (operator
            // model): freeze the current fit as manual zoom and bake
            // the auto-centering into the pan, so the scale can't
            // keep re-fitting under the operator's hand — and the
            // Auto button reads as an honest mode indicator again
            // (enabled = manual).
            if (m_manualScaleKm <= 0.0f) {
                m_manualScaleKm = m_lastScaleKm > 0.0f ? m_lastScaleKm
                                                       : DEFAULT_SCALE_KM;
                m_panAtPress += m_autoPanPx;
            }
        }
        if (m_dragging) {
            m_panPx = m_panAtPress + d;
            m_zoomAutoBtn->setChecked(false); // [autochk] drag = manual
            redraw();
            return;
        }
    }
    // Hover identity: nearest spot within a comfortable radius.
    // [hoverlift] Track which station that is, and repaint ONLY when it
    // changes -- mouseMoveEvent fires continuously, and redrawing the
    // whole chart per pixel of cursor travel would cost far more than
    // the effect is worth.
    {
        ScreenSpot const *hov = hitTest(event->position());
        QString now =
            hov ? hov->spot.receiverCall.toUpper() : QString{};
        // [selfhover #204, operator] The triangle is not a
        // ScreenSpot; test it directly so hovering my own station
        // shows tracers like any other. touchesHover and posByCall
        // already handle my call -- only the hover identity was
        // missing.
        if (now.isEmpty() && !m_myCall.isEmpty()) {
            QPointF const d = event->position() - m_centerPx;
            if (d.x() * d.x() + d.y() * d.y() <= 12.0 * 12.0)
                now = m_myCall.trimmed().toUpper();
        }
        if (now != m_hoverCall) {
            if (!m_hoverCall.isEmpty())
                m_prevHoverCall = m_hoverCall; // [hoverabove v2]
            m_hoverCall = now;
            redraw();
        }
    }
    if (ScreenSpot const *best = hitTest(event->position())) {
        bool const miles = m_config->miles(); // [units] per Settings
        double const dist = best->spot.distance * (miles ? 0.621371 : 1.0);
        // [radioage] Hover age on the presence clock (audit item 3).
        qint64 const ageSecs = effectiveWhen(best->spot).secsTo(
            DriftingDateTime::currentDateTimeUtc());
        QString tip;
        // [heardproof, operator: on EVERY hover, before the
        // distance] the dated transmit claim.
        // [heardproof, operator: absence of "hears me" and "last
        // heard" says it -- no filler text]
        // [operator 2026-08-27, compact hover] one fact per line,
        // short labels: "RX:" = their copy of my signal, "TX:" = the
        // dated transmit claim; grid capped at 6 chars; distance and
        // country/continent share the last line.
        QString txPart;
        if (best->spot.lastTxWhen.isValid()) {
            qint64 const txAge = best->spot.lastTxWhen.secsTo(
                DriftingDateTime::currentDateTimeUtc());
            txPart = (txAge >= 3600
                ? tr("TX: %1 hr ago").arg(txAge / 3600)
                : tr("TX: %1 min ago").arg(txAge / 60))
                + QStringLiteral("\n");
        }
        // [snrwho] A dB value is shown ONLY when it is a report
        // of MY signal; -99 is the no-report sentinel, a real
        // 0 dB report still shows (operator 2026-08-15). All
        // view: third-party PSKR SNRs never display — only the
        // hearing store's reported-to-me value qualifies. My
        // view: the spot's snr IS their copy of me (sentinel
        // possible on position-only on-air spots). rxOnly spots
        // have no report by construction.
        QString snrPart;
        if (!best->spot.rxOnly) {
            auto const rep = reportedToMe(best->spot);
            if (rep.snr > -99) {
                snrPart = tr("RX: %1 dB").arg(rep.snr);
                if (rep.when.isValid()) {
                    qint64 const rAge = rep.when.secsTo(
                        DriftingDateTime::currentDateTimeUtc());
                    snrPart += tr(" (%1 min ago)").arg(rAge / 60);
                } else {
                    snrPart += tr(" (age unknown)");
                }
                snrPart += QStringLiteral("\n");
            }
        }
        tip = tr("%1 (%2)\n%3%4%5 %6")
                  .arg(best->spot.receiverCall,
                       best->spot.receiverGrid.left(6), snrPart, txPart)
                  .arg(qRound(dist))
                  .arg(miles ? tr("mi") : tr("km"));
        // [BUILD 340] Country, when not our own (topic DXCC compare);
        // on the distance line since 2026-08-27 (compact hover).
        // The string form here is "Name; XX" (prefix stripped at the
        // lookup). NA is the home continent -- stating it adds
        // nothing, so "Canada; NA" shows as just "Canada" (operator,
        // 2026-08-27); other continents keep the suffix.
        if (!best->spot.country.isEmpty()) {
            QString country = best->spot.country;
            if (country.endsWith(QStringLiteral("; NA"))) {
                country.chop(4);
            }
            tip += QStringLiteral(" · ") + country;
        }
        // [nonrelayer, operator 2026-08-29] Last hover line, only
        // while the ring is showing (same predicate as the paint).
        if (m_nonRelayers.contains(
                best->spot.receiverCall.toUpper()) &&
            best->spot.lastTxWhen.isValid() &&
            best->spot.lastTxWhen.secsTo(
                DriftingDateTime::currentDateTimeUtc()) <=
                WINDOW_SECS) {
            tip += QStringLiteral("\n") + tr("Relay disabled?");
        } else if (m_knownRelayers.contains(
                       best->spot.receiverCall.toUpper()) ||
                   m_flagRelayers.contains(
                       best->spot.receiverCall.toUpper())) {
            // [knownrelayer, operator 2026-08-29] The positive
            // counterpart: a successful forward on the 90-day
            // record. The fresh negative above outranks it (a known
            // relayer currently declining reads "Relay disabled?").
            tip += QStringLiteral("\n") + tr("Relay enabled");
        }
        // [passband #218] The "QSY: xx.xxx" hover line that lived
        // here (qsyhover, Build 476) is GONE, by operator ruling and
        // in the ruled ORDER: it named the dial that would reach a
        // station transmitting outside our passband, and now that
        // the passband filter drops such stations from the map there
        // is nothing left to hover over. It was removed only AFTER
        // the filter landed, on the same branch, so no build ever
        // showed out-of-passband stations without the line.
        // [hovertime 2026-08-22, operator: "cut the hover info timeout
        // to 50%"] Qt's default when no time is given is
        //     10000 + 40 * max(0, len - 100) ms
        // (qtooltip.cpp), so half of it has to be computed the same way
        // rather than hard-coded -- the default grows with text length
        // and these tips vary a lot in size.
        int const qtDefaultMs =
            10000 + 40 * qMax(0, tip.size() - 100);
        // [hoverabove v2, operator 2026-08-29] The SECOND hover of
        // the same station shows the text ABOVE the dot from its
        // first instant -- no delay: the first visit reads at the
        // cursor, and returning to the station means the operator
        // now wants the lines under it visible. m_prevHoverCall is
        // the station of the PREVIOUS hover session (set when a
        // hover ends).
        QString const tipCall = best->spot.receiverCall.toUpper();
        QPoint at = event->globalPosition().toPoint();
        if (tipCall == m_prevHoverCall) {
            int const lines = tip.count(QLatin1Char('\n')) + 1;
            int const estH =
                lines * (fontMetrics().height() + 2) + 14;
            at = mapToGlobal(QPoint(
                int(best->pos.x()),
                int(best->pos.y()) - estH - DOT_RADIUS_PX - 4));
        }
        QToolTip::showText(at, tip, this, QRect{}, qtDefaultMs / 2);
    } else {
        QToolTip::hideText();
    }
    QWidget::mouseMoveEvent(event);
}

// [BUILD 340] Double-click a spot → QSY to the DX station's audio
// offset — only above 1000 Hz (same convention as the waterfall
// double-click gate: keep the HB sub-band / low region safe).
void SpotMapWindow::mouseDoubleClickEvent(QMouseEvent *event) {
    // [BUILD 340] Double-click a spot: QSY to its audio offset.
    // Suppressed ONLY while relay-select is active (clicks there are
    // hop selection; operator clarification 2026-08-14 — the earlier
    // global disable was over-applied).
    // [autoroute] Double-clicks are disabled for the whole mode --
    // armed (a double-click is two picks) and active (map input is
    // dead until exit).
    if (m_autoRouteArmed || m_autoRouteActive)
        return;
    if (event->button() == Qt::LeftButton && !m_relaySelect) {
        if (ScreenSpot const *best = hitTest(event->position())) {
            // [audit 2026-08-21] WHOSE FREQUENCY? The payload `f` is
            // the frequency of the SPOTTED TRANSMISSION -- which is
            // THEIRS in the All view (the sender is plotted) but OURS
            // in the MY view (the SPOTTER is plotted, and the
            // transmission they logged was our own). QSYing on a
            // reports-me spot therefore moved us to our OWN offset
            // while announcing it as someone else's. Hover still shows
            // the offset for both, because "what offset did they copy
            // me on" is genuinely useful -- only the QSY is wrong.
            if (!best->spot.reportsMe && best->spot.freqHz > 0 &&
                m_dialHz > 0) {
                qint64 const audio = best->spot.freqHz - m_dialHz;
                // QSY window: above 1000 Hz (HB sub-band convention --
                // a SEPARATE rule that stays as it is), and inside the
                // passband at the top.
                // [passband #218] The upper bound was a local 2500;
                // it is now JS8_PASSBAND_WIDTH_HZ, the one authority.
                // Inclusive at the top, per the boundary ruling.
                if (audio > 1000 && audio <= JS8_PASSBAND_WIDTH_HZ) {
                    showToast(tr("Moved to %1's frequency (%2 Hz)")
                                  .arg(best->spot.receiverCall)
                                  .arg(audio));
                    Q_EMIT qsyToOffset(static_cast<int>(audio));
                }
            }
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void SpotMapWindow::showToast(QString const &text) {
    if (!m_toast) {
        m_toast = new QLabel(this);
        m_toast->setStyleSheet(
            QStringLiteral("QLabel { background-color: rgba(30,30,30,220);"
                           " color: white; border-radius: 6px;"
                           " padding: 6px 14px; font-weight: bold; }"));
        m_toast->setAlignment(Qt::AlignCenter);
        m_toast->hide();
        m_toastTimer.setSingleShot(true);
        connect(&m_toastTimer, &QTimer::timeout, m_toast, &QLabel::hide);
    }
    m_toast->setText(text);
    m_toast->adjustSize();
    m_toast->move((width() - m_toast->width()) / 2,
                  height() - m_toast->height() - 24);
    m_toast->show();
    m_toast->raise();
    m_toastTimer.start(2500);
}

// [autoroute] The mode's status line: its own label just above the
// distance scale legend, shown until cleared with an empty string.
// Transient toasts are a separate control and keep their position
// and 2.5 s timing (operator, 2026-08-28).
void SpotMapWindow::setStickyToast(QString const &text) {
    m_stickyToast = text;
    if (text.isEmpty()) {
        if (m_statusLine)
            m_statusLine->hide();
        return;
    }
    if (!m_statusLine) {
        m_statusLine = new QLabel(this);
        m_statusLine->setStyleSheet(
            QStringLiteral("QLabel { background-color: rgba(30,30,30,220);"
                           " color: white; border-radius: 6px;"
                           " padding: 4px 12px; font-weight: bold; }"));
        m_statusLine->setAlignment(Qt::AlignCenter);
    }
    m_statusLine->setText(text);
    m_statusLine->adjustSize();
    // [operator 2026-08-31] Just ABOVE the congestion index (which
    // sits ~40-56 px above the legend strip), centered.
    m_statusLine->move((width() - m_statusLine->width()) / 2,
                       height() - LEGEND_STRIP_PX - 58
                           - m_statusLine->height());
    m_statusLine->show();
    m_statusLine->raise();
}

// [autoroute] The target prompt: a small panel low on the map with a
// line edit; Enter validates, or the operator clicks a station.
void SpotMapWindow::autoRouteShowPanel() {
    if (!m_autoRoutePanel) {
        m_autoRoutePanel = new QFrame(this);
        // [operator 2026-08-28] Same dark gray as the unchecked map
        // buttons; same font family, normal weight (the button font
        // IS that, so adopt it wholesale).
        m_autoRoutePanel->setStyleSheet(QStringLiteral(
            "QFrame { background-color: rgba(40,40,55,200);"
            " border-radius: 6px; }"
            "QLabel { color: rgb(210,210,225);"
            " background: transparent; }"));
        m_autoRoutePanel->setFont(m_autoRouteBtn->font());
        auto *lay = new QVBoxLayout(m_autoRoutePanel);
        lay->setContentsMargins(14, 10, 14, 10);
        auto *lbl = new QLabel(
            tr("Enter station or grid to automatically find a route, "
               "or click on a station on the map"),
            m_autoRoutePanel);
        lbl->setWordWrap(true);
        lay->addWidget(lbl);
        m_autoRouteEdit = new QLineEdit(m_autoRoutePanel);
        lay->addWidget(m_autoRouteEdit);
        // Adopt the constructor-made map-style buttons (addWidget
        // reparents); give the text room beyond the 36 px default.
        auto *btnRow = new QHBoxLayout;
        for (QToolButton *b : {m_autoRouteStartBtn,
                               m_autoRouteCancelBtn}) {
            b->setFixedSize(
                std::max(56, b->sizeHint().width() + 10), 20);
            b->show();
            btnRow->addWidget(b);
        }
        lay->addLayout(btnRow);
        // Valid = a real amateur callsign (no SWL/freebander fakes,
        // no hyphen nodes) or a grid; Start enables only then, and
        // Enter is a synonym for Start. Our OWN callsign is not a
        // valid target -- IDENTICAL full-call match, not base call
        // (operator, 2026-08-28: WM8Q/P stays targetable when we are
        // WM8Q).
        auto const isValidTarget = [this](QString const &t) {
            if (t.compare(m_myCall.trimmed(),
                          Qt::CaseInsensitive) == 0)
                return false;
            // [gridintent, operator 2026-08-30] An entry that OPENS
            // like a grid square is a grid, full stop: only a
            // complete 4- or 6-char grid enables Start. Without this
            // the half-typed grid EN45D read as a valid Ukrainian
            // callsign shape and the search ran for a station of
            // that name. Cost, accepted: a special-event callsign
            // that begins exactly like a grid square cannot be typed
            // as a target.
            static QRegularExpression const gridOpen(
                QStringLiteral("^[A-R]{2}[0-9]{2}"));
            if (gridOpen.match(t).hasMatch())
                return Maidenhead::valid(t);
            return Radio::is_routable_callsign(t);
        };
        connect(m_autoRouteEdit, &QLineEdit::textChanged, this,
                [this, isValidTarget](QString const &s) {
                    // [operator 2026-08-30] Grid targets clamp to 6
                    // characters -- the position math computes at 6
                    // everywhere (the Build 364 standard), so extra
                    // precision only creates format mismatches. A
                    // 7th character onto a valid 6-char grid is
                    // dropped as typed or pasted; callsigns are
                    // unaffected.
                    QString const up = s.trimmed().toUpper();
                    static QRegularExpression const grid6(
                        QStringLiteral("^[A-R]{2}[0-9]{2}[A-X]{2}$"));
                    // [operator 2026-08-30, second ruling] If the
                    // first 6 characters are a valid grid, adding
                    // MORE is simply disabled -- the earlier
                    // digit-only clamp let "EN45OOOO" grow with
                    // Start dark and no hint why. No callsign
                    // exception: [gridintent] already rules a
                    // grid-shaped opening to BE a grid.
                    if (up.size() > 6 &&
                        grid6.match(up.left(6)).hasMatch()) {
                        m_autoRouteEdit->setText(up.left(6));
                        return; // re-fires with the clamped text
                    }
                    m_autoRouteStartBtn->setEnabled(
                        isValidTarget(up));
                });
        connect(m_autoRouteStartBtn, &QToolButton::clicked, this,
                [this]() {
                    autoRouteChooseTarget(
                        m_autoRouteEdit->text().trimmed().toUpper());
                });
        connect(m_autoRouteEdit, &QLineEdit::returnPressed, this,
                [this]() {
                    if (m_autoRouteStartBtn->isEnabled())
                        m_autoRouteStartBtn->click();
                });
        connect(m_autoRouteCancelBtn, &QToolButton::clicked, this,
                [this]() {
                    // Same teardown as unchecking while armed.
                    m_autoRouteBtn->setChecked(false);
                });
    }
    m_autoRouteEdit->clear();
    positionAutoRoutePanel();
    m_autoRoutePanel->show();
    m_autoRoutePanel->raise();
    m_autoRouteEdit->setFocus();
}

void SpotMapWindow::positionAutoRoutePanel() {
    if (!m_autoRoutePanel)
        return;
    positionOverlayPanel(m_autoRoutePanel);
}

// [#207 waitopts] The response-wait dialog: three radio choices,
// styled and placed like the auto-route prompt. A change applies
// and persists immediately (sink -> mainwindow -> .ini); Close just
// dismisses. Right-clicking "Adaptive" reveals a small threshold
// spin box (hidden test feature) that hides itself after 3 s
// without a change.
void SpotMapWindow::optionsShowPanel() {
    if (!m_optionsPanel) {
        m_optionsPanel = new QFrame(this);
        m_optionsPanel->setStyleSheet(QStringLiteral(
            "QFrame { background-color: rgba(40,40,55,200);"
            " border-radius: 6px; }"
            "QLabel { color: rgb(210,210,225);"
            " background: transparent; }"
            "QRadioButton { color: rgb(210,210,225);"
            " background: transparent; }"
            "QSpinBox { background-color: rgba(40,40,55,230);"
            " color: rgb(210,210,225);"
            " border: 1px solid rgb(120,140,190); }"));
        m_optionsPanel->setFont(m_optionsBtn->font());
        auto *lay = new QVBoxLayout(m_optionsPanel);
        lay->setContentsMargins(14, 10, 14, 10);
        lay->addWidget(
            new QLabel(tr("Response wait time:"), m_optionsPanel));
        m_waitShort =
            new QRadioButton(tr("Short / quiet band"), m_optionsPanel);
        m_waitLong =
            new QRadioButton(tr("Long / busy band"), m_optionsPanel);
        m_waitAdaptive =
            new QRadioButton(tr("Adaptive (on-air)"), m_optionsPanel);
        m_waitAdaptivePskr =
            new QRadioButton(tr("Adaptive (PSKR)"), m_optionsPanel);
        for (QRadioButton *r : {m_waitShort, m_waitLong,
                                m_waitAdaptive, m_waitAdaptivePskr})
            lay->addWidget(r);
        auto *btnRow = new QHBoxLayout;
        btnRow->addStretch(1);
        m_optionsCloseBtn->setFixedSize(
            std::max(56, m_optionsCloseBtn->sizeHint().width() + 10),
            20);
        m_optionsCloseBtn->show();
        btnRow->addWidget(m_optionsCloseBtn);
        lay->addLayout(btnRow);
        connect(m_optionsCloseBtn, &QToolButton::clicked,
                m_optionsPanel, &QFrame::hide);
        // Threshold spin: parented to the panel, shown on demand
        // beside the Adaptive radio.
        m_threshSpin = new QSpinBox(m_optionsPanel);
        m_threshSpin->setRange(1, 10);
        m_threshSpin->setFixedSize(48, 22);
        m_threshSpin->hide();
        m_threshTimer = new QTimer(this);
        m_threshTimer->setSingleShot(true);
        m_threshTimer->setInterval(3000);
        connect(m_threshTimer, &QTimer::timeout, m_threshSpin,
                &QWidget::hide);
        auto const push = [this]() {
            if (!m_waitSink)
                return;
            int const mode = m_waitShort->isChecked()  ? 0
                             : m_waitLong->isChecked() ? 2
                             : m_waitAdaptivePskr->isChecked() ? 3
                                                               : 1;
            m_waitSink(mode, m_curThreshOnAir, m_curThreshPskr);
        };
        for (QRadioButton *r : {m_waitShort, m_waitLong,
                                m_waitAdaptive, m_waitAdaptivePskr})
            connect(r, &QRadioButton::toggled, this,
                    [push](bool on) {
                        if (on)
                            push();
                    });
        connect(m_threshSpin, qOverload<int>(&QSpinBox::valueChanged),
                this, [this, push](int v) {
                    (m_threshSpinPskr ? m_curThreshPskr
                                      : m_curThreshOnAir) = v;
                    push();
                    m_threshTimer->start();
                });
        // [operator 2026-09-01] Right-click on EITHER adaptive row
        // edits THAT metric's threshold; the spin appears over the
        // row's text (past the radio circle).
        for (QRadioButton *r : {m_waitAdaptive, m_waitAdaptivePskr}) {
            r->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(r, &QWidget::customContextMenuRequested, this,
                    [this, r](QPoint const &) {
                        m_threshSpinPskr = (r == m_waitAdaptivePskr);
                        QSignalBlocker const b{m_threshSpin};
                        m_threshSpin->setValue(m_threshSpinPskr
                                                   ? m_curThreshPskr
                                                   : m_curThreshOnAir);
                        m_threshSpin->move(r->x() + 24, r->y() - 1);
                        m_threshSpin->show();
                        m_threshSpin->raise();
                        m_threshSpin->setFocus();
                        m_threshTimer->start();
                    });
        }
    }
    // Reflect the owner's current values every time it opens.
    if (m_waitProbe) {
        WaitConfig const cfg = m_waitProbe();
        QSignalBlocker const b1{m_waitShort}, b2{m_waitLong},
            b3{m_waitAdaptive}, b4{m_threshSpin},
            b5{m_waitAdaptivePskr};
        m_curThreshOnAir = cfg.threshOnAir;
        m_curThreshPskr = cfg.threshPskr;
        (cfg.mode == 0   ? m_waitShort
         : cfg.mode == 2 ? m_waitLong
         : cfg.mode == 3 ? m_waitAdaptivePskr
                         : m_waitAdaptive)
            ->setChecked(true);
        // [operator 2026-09-01] No MQTT data -> the PSKR choice is
        // not selectable (it stays visibly checked if it IS the
        // stored mode -- the executor is falling back to on-air
        // meanwhile and resumes PSKR when data returns).
        m_waitAdaptivePskr->setEnabled(pskrDataAvailable());
    }
    m_threshSpin->hide();
    positionOverlayPanel(m_optionsPanel);
    m_optionsPanel->show();
    m_optionsPanel->raise();
}

// [#207 waitopts] Shared placement for the low centered overlay
// panels (auto-route prompt, Options dialog).
void SpotMapWindow::positionOverlayPanel(QFrame *panel) {
    panel->adjustSize();
    // [operator 2026-08-28, corrected] Equal air on BOTH sides:
    // (right edge of the LEFT button stack) .. panel .. (left edge
    // of the RIGHT button stack).
    int const leftEdge = 6 + m_leftColW;
    int const rightEdge = width() - m_btnColW - 6;
    int const avail = rightEdge - leftEdge;
    int const w = qMin(panel->sizeHint().width() + 20, avail - 12);
    panel->resize(w, panel->sizeHint().height());
    panel->move(leftEdge + qMax(6, (avail - w) / 2),
                height() - panel->height() - 70);
}

// [modeowner 2026-08-30] A validated target is a REQUEST, nothing
// more. The map changes NO state of its own here: the mainwindow
// owns the mode, and this widget's active-mode UI is set only by
// its autoRouteStarted()/autoRouteEnded() notifications. A refused
// request (busy radio -> toast) therefore leaves the prompt open
// with the target still typed, ready to retry -- the 2026-08-30
// field failure was exactly this function declaring the mode active
// optimistically while the owner refused, after which every copy of
// the state disagreed (status line up, Halt dead, prompt stuck).
void SpotMapWindow::autoRouteChooseTarget(QString const &target) {
    Q_EMIT autoRouteStart(target);
}

// [modeowner] Owner says the mode actually STARTED -- only now does
// the map's UI flip: prompt away, Halt label, sticky status.
void SpotMapWindow::autoRouteStarted(QString const &target) {
    m_autoRouteArmed = false;
    m_autoRouteActive = true;
    m_autoRouteTarget = target;
    if (m_autoRoutePanel)
        m_autoRoutePanel->hide();
    {
        // The mode can start with the button unchecked (target sent
        // while a prior teardown unchecked it); the label and check
        // state are DERIVED from the owner's notification.
        QSignalBlocker const b(m_autoRouteBtn);
        m_autoRouteBtn->setChecked(true);
    }
    m_autoRouteBtn->setText(tr("Halt auto-route"));
    setStickyToast(tr("Auto-route to %1 in progress...").arg(target));
}

// [autoroute] Mode over (success, failure, or cancel) -- the
// mainwindow already showed any success/failure dialog; a cancel
// gets its toast here. Idempotent: the button's toggled(false)
// fires after the flags are cleared and does nothing.
void SpotMapWindow::autoRouteEnded(bool canceled) {
    QString const t = m_autoRouteTarget;
    m_autoRouteActive = false;
    m_autoRouteArmed = false;
    m_autoRouteTarget.clear();
    if (m_autoRoutePanel)
        m_autoRoutePanel->hide();
    setStickyToast(QString{});
    {
        QSignalBlocker const b(m_autoRouteBtn);
        m_autoRouteBtn->setChecked(false);
    }
    m_autoRouteBtn->setText(tr("Auto-route"));
    m_relaySelBtn->setEnabled(true);
    if (canceled && !t.isEmpty())
        showToast(tr("Auto-route to %1 canceled").arg(t));
    requestReplot();
}

// [hbrelay TODO #191] A heartbeat announced relay-enabled. RAM for
// the instant hover, PLUS a durable hbflag event (existing store,
// new kind value, no schema change) throttled to one row per
// station per hour -- so the flag survives restarts and ages out
// via the same 24 h window the refresh applies (operator: "confirm
// it passes through to the DB" -- it did not, now it does).
void SpotMapWindow::noteRelayFlag(QString const &call) {
    QString const c = call.trimmed().toUpper();
    m_flagRelayers.insert(c);
    qint64 const nowMs =
        QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    if (nowMs - m_flagJournaledMs.value(c, 0) >
        qint64(WINDOW_SECS) * 1000) {
        m_flagJournaledMs.insert(c, nowMs);
        queueReachEvent({m_currentBand, c, QStringLiteral("hbflag"),
                         QString{}, nowMs / 1000, true});
    }
}

// [nonrelayer] See header. Derived, no stored state: for each call,
// count failed relay asks (fwd ok=0) that are BOTH newer than its
// most recent successful forward on any band AND within the trailing
// 24 h. Two or more = candidate; the paint site adds the
// transmitted-within-the-hour test from the spot itself. ~200 rows,
// sub-ms; recomputed only when the event store changes.
void SpotMapWindow::refreshNonRelayers() {
    m_nonRelayersDirty = false;
    m_nonRelayers.clear();
    m_knownRelayers.clear();
    m_flagRelayers.clear(); // rebuilt from durable hbflag events
    qint64 const now =
        QDateTime::currentDateTimeUtc().toSecsSinceEpoch();
    auto const rows = reachEvents(); // one load, two passes
    QHash<QString, qint64> lastOk;
    for (auto const &r : rows) {
        if (r.kind == QLatin1String("fwd") && r.ok) {
            lastOk[r.station] = qMax(lastOk.value(r.station), r.when);
            m_knownRelayers.insert(r.station); // [knownrelayer]
        }
        // [hbrelay] announcements count while fresh: the flag rides
        // every heartbeat, so a live relayer re-announces hourly and
        // a turned-off one ages out within a day.
        if (r.kind == QLatin1String("hbflag") &&
            now - r.when <= 24 * 3600)
            m_flagRelayers.insert(r.station);
    }
    QHash<QString, int> fails;
    for (auto const &r : rows) {
        if (r.kind != QLatin1String("fwd") || r.ok)
            continue;
        if (now - r.when > 24 * 3600)
            continue;
        if (r.when <= lastOk.value(r.station, 0))
            continue;
        if (++fails[r.station] >= 2)
            m_nonRelayers.insert(r.station);
    }
    // [relayprior] The executor demotes by the SAME criterion the
    // red ring displays; keep the counts so the demotion can feed
    // the actual failures into the prior instead of a constant.
    m_nonRelayerFails = fails;
}

// [#187 intelminer] See header.
int SpotMapWindow::seedLogGrids(QVector<GridDb::LogSeed> const &rows) {
    int inserted = 0;
    for (auto const &s : rows) {
        if (m_gridDb.seedLogGrid(s))
            ++inserted;
        if (!m_gridByCall.contains(s.call))
            m_gridByCall.insert(s.call, s.grid);
    }
    if (inserted)
        qCWarning(mqttclient_js8)
            << "[SPOTMAP] seeded" << inserted
            << "log-mined grids into the bank";
    return inserted;
}

void SpotMapWindow::showRelayPathToast() {
    // [relaysel] Running summary while building: origin first, then
    // the hops in outbound order.
    QStringList parts{m_myCall};
    parts.append(m_relayPath);
    showToast(tr("Relay path: %1")
                  .arg(parts.join(QStringLiteral(" → "))));
}

void SpotMapWindow::mousePressEvent(QMouseEvent *event) {
    // Arm a potential drag; the click action moved to release so a
    // pan gesture doesn't also fire spotClicked.
    if (event->button() == Qt::LeftButton) {
        m_maybeDrag = true;
        m_dragging = false;
        m_pressPos = event->position();
        m_panAtPress = m_panPx;
    } else if (event->button() == Qt::MiddleButton) {
        // [wheelzoom] Push the scroll wheel to zoom in (operator,
        // 2026-08-16).
        zoomIn();
    } else if (event->button() == Qt::RightButton) {
        // [stamon] Right-click on any plotted station (on-air,
        // PSKR, whatever ring it wears) -> offer the monitor.
        if (auto const *s = hitTest(event->position());
            s && !s->spot.receiverCall.isEmpty())
            Q_EMIT stationRightClicked(
                s->spot.receiverCall,
                event->globalPosition().toPoint());
    }
    QWidget::mousePressEvent(event);
}

void SpotMapWindow::wheelEvent(QWheelEvent *event) {
    // [wheelzoom] Scroll to zoom (operator, 2026-08-16). One ladder
    // step per notch; accumulate fine trackpad deltas to a notch.
    m_wheelAccum += event->angleDelta().y();
    while (m_wheelAccum >= 120) {
        m_wheelAccum -= 120;
        zoomIn();
    }
    while (m_wheelAccum <= -120) {
        m_wheelAccum += 120;
        zoomOut();
    }
    event->accept();
}

void SpotMapWindow::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        if (m_dragging) {
            m_dragging = false;
            unsetCursor();
            // Arrowheads are suppressed DURING a drag for speed, so the
            // frame that restores them has to be asked for explicitly.
            // Without this they returned only on the next natural
            // repaint -- which on a quiet band is seconds away, and
            // reads as the map being stuck (operator, 2026-08-23:
            // "excessive delay before arrows redrawn"). An
            // optimisation that defers work owes the frame that
            // finishes it.
            redraw();
        } else if (m_maybeDrag) {
            // [BUILD 336 TODO #96 first slice] Left-click a spot dot
            // → emit its callsign. Same nearest-within-radius
            // hit-test as hover. (Release-time since mapzoom drag.)
            if (ScreenSpot const *best = hitTest(event->position())) {
                // [autoroute] Active mode: map clicks are disabled
                // until exit. Armed mode: this click IS the target
                // pick, same validity screen as the typed entry.
                if (m_autoRouteActive) {
                    m_maybeDrag = false;
                    return;
                }
                if (m_autoRouteArmed) {
                    QString const call =
                        best->spot.receiverCall.trimmed().toUpper();
                    // Own full callsign excluded, identical match
                    // (operator, 2026-08-28).
                    if (Radio::is_routable_callsign(call) &&
                        call.compare(m_myCall.trimmed(),
                                     Qt::CaseInsensitive) != 0)
                        autoRouteChooseTarget(call);
                    else
                        showToast(tr("%1 is not a valid call sign")
                                      .arg(call));
                    m_maybeDrag = false;
                    return;
                }
                // [relaysel] Selecting: clicks append hops instead of
                // seeding the outgoing box.
                if (m_relaySelect) {
                    QString const call = best->spot.receiverCall;
                    // [relayscreen 2026-08-27, operator ruling] A
                    // non-amateur or hyphenated receive node cannot
                    // carry traffic: refused. A station without
                    // recent transmit evidence is ALLOWED (downgrade
                    // philosophy, never exclude) with the fact
                    // stated.
                    bool const hyphen = call.contains(QLatin1Char('-'));
                    if (hyphen || !Radio::is_callsign(call)) {
                        showToast(tr("%1 cannot relay (receive-only "
                                     "node or not an amateur call)")
                                      .arg(call));
                    } else if (!m_relayPath.contains(call)) {
                        m_relayPath.append(call);
                        m_relayPathSpots.append(best->spot); // [relaykeep]
                        if (best->spot.rxOnly)
                            showToast(tr("%1 added -- no transmission "
                                         "observed recently")
                                          .arg(call));
                        else
                            showRelayPathToast();
                        updateRelayButtons();
                        requestReplot();
                    }
                } else {
                    Q_EMIT spotClicked(best->spot.receiverCall);
                }
            }
        }
        m_maybeDrag = false;
    }
    QWidget::mouseReleaseEvent(event);
}

void SpotMapWindow::changeEvent(QEvent *event) {
    // [btnpos] Activation and font settling both arrive AFTER
    // showEvent (screen mapping can rescale fonts) — recompute the
    // overlay-button geometry at the moment the operator actually
    // looks at it (field: width wrong at activate through THREE
    // show-time fixes).
    if (event->type() == QEvent::ActivationChange ||
        event->type() == QEvent::FontChange)
        positionWindowButtons();
    // Hint toast whenever the map window gains focus (Andy
    // 2026-07-16) — teaches the click-to-copy affordance in place.
    if (event->type() == QEvent::ActivationChange && isActiveWindow() &&
        // [autoroute] No hint toast during the mode -- the status
        // line and prompt already say what matters (operator,
        // 2026-08-28).
        !m_autoRouteActive && !m_autoRouteArmed &&
        // [operator 2026-08-30] ...and none while a standard message
        // or ARQ transfer is under way: the hint advertises
        // click-to-compose, which must not be used mid-send.
        (!m_txBusyProbe || m_txBusyProbe().isEmpty())) {
        // [relaysel] While selecting relays, the activation hint
        // teaches THAT gesture, not click-to-compose (operator,
        // 2026-08-14).
        showToast(m_relaySelect
                      ? tr("Click to select relay stations, in "
                           "outbound order")
                      : tr("Click on a call sign to create an outgoing "
                           "message. Double-click to QSY. Drag to pan."));
    }
    QWidget::changeEvent(event);
}

void SpotMapWindow::showEvent(QShowEvent *) {
    // [showfix] Session-only resets fire ONLY on a genuine open
    // (first show / show-after-close). A minimize-restore or
    // desktop-switch re-expose also lands here and must preserve the
    // operator's view/zoom/pan/mode (field 2026-08-15: map
    // "spontaneously" reverted to the default view on activation).
    // [btnpos] Position immediately AND once more after the event
    // loop settles: at showEvent the window may still carry its
    // pre-restore default size (geometry restore / WM sizing land
    // after show), which left buttons misplaced until a manual
    // resize (operator 2026-08-15, again 2026-08-16).
    positionWindowButtons();
    QTimer::singleShot(0, this, [this]() { positionWindowButtons(); });
    m_restoreVisible = true; // [visrace] open = restore at startup
    if (m_resetOnNextShow) {
        m_resetOnNextShow = false;
        // [persistui 2026-08-15] Zoom level and time range persist
        // (operator revision — supersedes the 2026-08-02 every-open-
        // starts-in-Auto directive); view type, Connections, and PSKR
        // toggle PERSIST across reopen and sessions — only the
        // relay builder resets (its path is a per-use gesture).
        if (m_relaySelBtn && m_relaySelBtn->isChecked())
            m_relaySelBtn->setChecked(false);
    }
    requestReplot();
}

void SpotMapWindow::userClose() {
    m_restoreVisible = false; // [visrace] menu toggle = user intent
    close();
}

void SpotMapWindow::closeEvent(QCloseEvent *event) {
    // Hide only — the MQTT client and caches keep running so the map
    // is current the moment it's reopened. Client stops at app exit.
    // FULL state (incl. the tracked WindowVisible flag) saves here:
    // a spontaneous close is the user's X (clears the restore flag);
    // a programmatic close is shutdown or the menu toggle, which
    // manages the flag itself ([visrace]).
    // [visrace2 2026-08-21] ...but ONLY while the app is running. At
    // shutdown the window manager can deliver a SPONTANEOUS close to
    // this top-level window as part of session teardown, which read
    // as "user clicked X", cleared the restore flag, and saved false
    // over the correct value written moments earlier by
    // writeSettings() -- so the map sometimes failed to reopen, with
    // the outcome depending on which close arrived first (operator:
    // "starts up without the spots map sometimes. a race?").
    if (event->spontaneous() && !m_shuttingDown)
        m_restoreVisible = false; // [visrace] user clicked the X
    // [modeowner, operator 2026-08-30] Closing the map clears the
    // target prompt (pure local UI); a RUNNING auto-route is the
    // owner's state and continues untouched -- reopening the map
    // redraws Halt/status from that fact.
    if (m_autoRouteArmed && !m_autoRouteActive) {
        m_autoRouteArmed = false;
        if (m_autoRoutePanel)
            m_autoRoutePanel->hide();
        {
            QSignalBlocker const b(m_autoRouteBtn);
            m_autoRouteBtn->setChecked(false);
        }
        m_relaySelBtn->setEnabled(true);
    }
    saveSettings(); // full state, incl. geometry — crash-safe too
    m_resetOnNextShow = true; // [showfix] genuine close -> next show resets
    Q_EMIT closed();
    QWidget::closeEvent(event);
}
