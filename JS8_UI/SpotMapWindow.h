#ifndef SPOT_MAP_WINDOW_HPP__
#define SPOT_MAP_WINDOW_HPP__

/**
 * @file SpotMapWindow.h
 * @brief "Spots Map" — live polar heat map of where my signal is spotted.
 *
 * Subscribes to the PSK Reporter MQTT feed (mqtt.pskreporter.info) for
 * spots where MY callsign is the sender, and renders each spotter on an
 * azimuthal chart centered on my grid: position from
 * Geodesic::vector(myGrid, spotterGrid), heat color from the SNR the
 * spotter reported. Rolling ONE-HOUR window (WINDOW_SECS),
 * auto-scaling radius,
 * current band displayed with per-band caches retained across band
 * changes.
 *
 * The MQTT client starts with the app (constructor), NOT with the
 * window: history accumulates in the background from launch so opening
 * the map shows the last 15 minutes immediately. The window is purely a
 * viewport; closing it hides it and rendering stops, but the client and
 * caches keep running until app exit.
 */

#include <QColor>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QPixmap>
#include <QString>
#include <QTimer>
#include <QPointF>
#include <QPolygonF>
#include <QVector>
#include <QVariantMap>
#include <QWidget>

#include "JS8_Main/GridDb.h"

#include <functional>

class Configuration;
class MqttClient;
class QSettings;

class SpotMapWindow final : public QWidget {
    Q_OBJECT

  public:
    SpotMapWindow(QSettings *settings, Configuration const *config,
                  QWidget *parent = nullptr);
    ~SpotMapWindow() override;

    void saveSettings();
    // [visrace2] Call before the shutdown save: makes closeEvent stop
    // treating a window-manager close as "the user closed the map".
    void beginShutdown() { m_shuttingDown = true; }
    bool wasVisibleAtShutdown() const; // for startup restore
    // [visrace] User-intent close (menu toggle): clears the
    // restore-at-startup flag, unlike a shutdown-driven close.
    void userClose();
    // Transient toast overlay (bottom-center, auto-hides ~2.5 s) —
    // feedback for click actions, e.g. "Copied K9AVT to outgoing
    // message".
    void showToast(QString const &text);
    // [units] Settings changed (distance units etc.) — repaint with
    // the new configuration. Cheap; safe to call on every accept.
    void configRefresh() { requestReplot(); }
    // [hearlines] On-air heard-mesh feed: a station (hearer) was
    // observed hearing one or more others — from a HEARING reply
    // (incl. relayed "... *DE* CALL" form) or from any directed
    // exchange ("A: B ..." = A hears/works B). Grids may be empty
    // when unknown; edges draw only when both ends resolve. Works
    // with no internet — this is the offline complement to the MQTT
    // mesh (blue lines, drawn over the dark-yellow PSKR ones).
    // [onairspot] A station demonstrably heard MY signal (it sent me
    // a directed frame; an SNR reply even carries their copy's
    // report). The on-air equivalent of a PSK Reporter spot of me —
    // feeds the MY view so the map works identically with no
    // internet. snr -99 = frame carried no report (position-only).
    // [passband #218] callRfHz: that station's transmit frequency as
    // WE measured it (d.dial + d.offset), 0 when unknown. The frame
    // was addressed to us, so we heard this station directly and the
    // measurement is first-hand.
    void addOnAirSpotOfMe(QString const &band, QString const &call,
                          QString const &grid, int snr,
                          qint64 callRfHz = 0);
    // heardCalls may be empty = pure PRESENCE (e.g. a heartbeat with
    // its grid): the station gets a hollow dot in the All view.
    // reportedToMeSnr: an SNR value this station REPORTED TO US (its
    // copy of our signal) — the ONLY thing that colors an on-air
    // All-view dot solid (operator rule 2026-08-14); -99 = no change.
    // [#168 mapdump 2026-08-21] Read-only snapshot of what the map
    // knows RIGHT NOW, for the API dump. The live spot feed and the
    // hearing store exist only in RAM and no tool could reach them,
    // so offline planners were blind to the one fact that matters
    // most: who is on the air this minute (operator: "the spots map
    // is pretty much *the point*"). Debug/test surface — structure
    // may change; it is not a compatibility promise.
    QVariantMap dumpState(QString const &band = QString{}) const;
    // [maptruth] a report of my signal, WITH its observation time --
    // the pair travels together everywhere or not at all.
    struct ReportOfMe { int snr = -99; QDateTime when; };

    // [#164] Authority lookup for outside consumers (hearGridFor's
    // fallback chain) — includes everything the persistent store
    // seeded. Returns empty when unknown.
    QString knownGrid(QString const &call) const {
        return m_gridByCall.value(call.toUpper());
    }

    // [reachport] Typed reads over the RAM store for the in-app
    // reaching executor -- the store stays the ONE authority
    // (GridDb.h: "RAM is the single in-session authority"); these are
    // the knownGrid() precedent applied to edges and presence. NO
    // QVariant round-trip: the executor asks the same questions the
    // frozen python asked of dumpState(), in process.
    struct HearerView {
        QString hearer;      // upper-case callsign
        QString grid;
        qint64  whenMs = 0;  // edge sighting time (ms since epoch)
        int     snr = -99;   // third-party: how well hearer copies
        QString source;      // "radio" | "hearing" | "mqtt"
    };
    QVector<HearerView> hearersOf(QString const &band,
                                  QString const &heard) const;
    struct StationView {
        QString call;        // upper-case
        QString grid;
        qint64  lastSeenMs = 0;
        int     snrToMe = -99;   // their report of OUR signal
        bool    hearsMe = false; // heard-map contains us
        bool    txAlive = false; // seen as a SENDER (it keys up);
                                 // false = reception-only footprint
        QString source;
    };
    QVector<StationView> activeStations(QString const &band) const;

    // [passband #218] THE ONE verdict on whether we can hear a
    // station where it transmits. Three answers, and the difference
    // between Out and Unknown is the whole design:
    //
    //   In      -- its transmit frequency is known, current, and
    //              inside [our dial, our dial + JS8_PASSBAND_WIDTH_HZ]
    //              (inclusive both ends).
    //   Out     -- known, current, and outside that window. This is
    //              the ONLY answer that ever hides a dot or rejects a
    //              relay first hop.
    //   Unknown -- no frequency on record, or the record is older
    //              than JS8_FREQ_STALE_SECS, or OUR OWN DIAL is
    //              unknown (no CAT, not read yet). Exempt everywhere:
    //              stays visible, stays eligible. RX-only and monitor
    //              stations live here permanently by construction --
    //              they never transmit, so nothing can name their
    //              frequency.
    //
    // Evaluated FRESH on every call and never cached: the answer
    // changes the moment we QSY, and a cached verdict would keep
    // hiding a station we just tuned onto. Uses only the station's
    // absolute transmit frequency; their dial is never inferred.
    // In-passband proves we can hear THEM, never that they can hear
    // us -- which is why the relay side treats this as a filter that
    // can only exclude, never qualify.
    enum class Passband { In, Out, Unknown };
    Passband passbandVerdict(QString const &band,
                             QString const &call) const;

    // [reachport2] Whole-band adjacency for the executor's route book
    // (one snapshot per attempt), and the persistent tier at the
    // python's 24 h horizon -- the RAM store prunes at 1 h, which is
    // the exact ratio livemodel's docstring warns halves the graph.
    struct EdgeView {
        QString hearer, heard;
        qint64  whenMs = 0;
        int     snr = -99;
        QString source;
    };
    QVector<EdgeView> allEdges(QString const &band) const;
    QVector<GridDb::EdgeRow> edges24h() const;
    // [habitstore] executor's durable habit observations
    void queueReachEvent(GridDb::ReachEventRow const &r) {
        m_nonRelayersDirty = true; // [nonrelayer] recompute lazily
        m_gridDb.queueReachEvent(r);
    }
    // [relayprior] Ranking consumers of the ring criteria -- the
    // map stays the ONE authority for both: green ring = announced
    // relay-on within 24h; red ring = >=2 failed asks in 24h since
    // the last success (counts included so the executor can blend
    // the real failures, not a constant). Recomputed if stale.
    QSet<QString> announcedRelayers() {
        if (m_nonRelayersDirty) refreshNonRelayers();
        return m_flagRelayers;
    }
    // [relayprior] The other half of the green ring: at least one
    // PROVEN forward in the journal (90-day memory, unwindowed --
    // same rule the ring draws by).
    QSet<QString> knownRelayers() {
        if (m_nonRelayersDirty) refreshNonRelayers();
        return m_knownRelayers;
    }
    QHash<QString, int> nonRelayerFails() {
        if (m_nonRelayersDirty) refreshNonRelayers();
        return m_nonRelayerFails;
    }
    QVector<GridDb::ReachEventRow> reachEvents() const {
        // Disk PLUS the unflushed queue: a fresh forward must be
        // visible to the relay-status computation immediately, not
        // at the next flush (the VE4TIM hover stayed stale until a
        // restart, 2026-08-29).
        auto rows = m_gridDb.loadReachEvents();
        rows += m_gridDb.pendingReachEvents();
        return rows;
    }
    // [nonrelayer 2026-08-29, operator spec] Stations that transmit
    // but do not relay when asked: >= 2 failed asks within the
    // trailing 24 h (both after the most recent successful forward,
    // ANY band -- relaying is an all-bands setting), shown as a
    // subtle dashed ring on the dot IF the station also transmitted
    // within the last hour (WINDOW_SECS). Visual only -- no hover
    // line, no legend (a barely-observable clue for informed
    // pro-relay activists; operator may revisit). Derived from the
    // event store, no new state; a forward clears it by arithmetic,
    // and it auto-expires ~a day after the last failed ask.
    QSet<QString> m_nonRelayers;
    QHash<QString, int> m_nonRelayerFails;   // [relayprior]
    // [knownrelayer] Same pass, zero extra cost: stations with a
    // successful forward anywhere in the 90-day record. Presentation
    // TBD (operator); the set is maintained from here on.
    QSet<QString> m_knownRelayers;
    bool m_nonRelayersDirty = true;
    // [selfhover #204] The triangle's screen position from the last
    // paint, for hover hit-testing (it is not a ScreenSpot). Starts
    // far off-screen so a pre-first-paint move can't false-hit.
    QPointF m_centerPx{-1e9, -1e9};
    void refreshNonRelayers();

    // heardWhen: optional BACKDATED sighting time for the heard
    // edges ([#161] age-bearing replies) — invalid = now; an edge's
    // `when` only ever moves FORWARD. heardSnr: third-party SNR for
    // the heard edges (-99 = none).
    //
    // [passband #218] hearerRfHz: the HEARER's absolute transmit
    // frequency in Hz, when we know it first-hand — that is,
    // d.dial + d.offset from a frame WE decoded. 0 means unknown and
    // records nothing.
    //
    // It describes the HEARER only, never the heard calls: we heard
    // the sender, we did not hear the stations it is telling us
    // about. A caller that is relaying someone else's report must
    // leave this 0 (see the Q-call answer site, which does).
    //
    // Our own decodes already compute this and publish it over the
    // API as "FREQ" (processRxActivity.cpp), then threw it away here
    // — so the stations we are CERTAIN we can hear were the ones with
    // no recorded frequency, while PSKR-sourced ones had it. That was
    // backwards, and it is the prerequisite for the passband filter.
    void addHearingReport(QString const &band, QString const &hearer,
                          QString const &hearerGrid,
                          QStringList const &heardCalls,
                          QStringList const &heardGrids,
                          int reportedToMeSnr = -99,
                          QDateTime const &heardWhen = QDateTime{},
                          int heardSnr = -99,
                          QString const &source = QString{},
                          qint64 hearerRfHz = 0);

  public slots:
    void setBand(QString const &band);
    void setStation(QString const &callsign, QString const &grid);
    // [BUILD 340] Dial frequency (Hz) — lets hover show each spot's
    // AUDIO offset (spot RF f − dial) and gates double-click QSY.
    void setDialFrequency(qint64 hz);

  signals:
    void closed();
    // [BUILD 336 TODO #96 first slice] Left-click on a spot dot.
    // Main window decides what to do with it (currently: seed the
    // outgoing text box when it's empty and no call is selected).
    void spotClicked(QString const &callsign);
    // [BUILD 340] Double-click on a spot: QSY to the DX station's
    // audio offset (only emitted when it's above 1000 Hz).
    void qsyToOffset(int audioHz);
    // [stamon] Right-click landed on a plotted station (any kind).
    void stationRightClicked(QString const &call,
                             QPoint const &globalPos);
    // [relaysel] "Done" pressed in relay-select mode: the composed
    // relay template ("HOP1>HOP2>DEST [MESSAGE]") for the outgoing
    // box; the main window highlights the placeholder for typing.
    // Plain directed text — ARQ is NOT used for any hop (operator
    // directive 2026-08-14).
    void relayTemplateReady(QString const &templateText);
    // [autoroute] A validated target was chosen; the mainwindow
    // starts the executor and locks the main screen.
    void autoRouteStart(QString const &target);
    // [autoroute] Operator clicked "Halt auto-route" on the map.
    void autoRouteHalt();

  protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void closeEvent(QCloseEvent *) override;
    void showEvent(QShowEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;   // [hoverlift] drop the lift
    void mousePressEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void changeEvent(QEvent *) override; // activation → hint toast

  private slots:
    void onMqttMessage(QString const &topic, QByteArray const &payload);
    void onMqttState(QString const &state);
    void onPruneTick();
    void redraw();
    void zoomIn();
    void zoomOut();
    void zoomAuto();

  private:
    // [oneobs 2026-08-22] TRANSIENT RENDER UNIT ONLY -- never stored.
    // Built per paint from the observation store below. The map used to
    // keep two persistent QVector<Spot> stores alongside the mesh, and
    // every display bug of 2026-08-21/22 was those two disagreeing with
    // it: one spot per station capped the lines at one per station, dot
    // age diverged from line age, heardBy provenance leaked onto
    // stations visible for other reasons, and dots refreshed while
    // their edges froze. Operator: "what's wrong with: for each message
    // (or PSKR spot), draw a colour-coded or empty circle, connect the
    // two with a line?" Nothing -- so the observation is now the only
    // record, and circles and lines are drawn from it together.
    struct Spot {
        QDateTime when;         // clamped-to-now report time
        QString receiverCall;   // the PLOTTED station (spotter in my
                                // view; SENDER in the All view)
        QString receiverGrid;   // plotted station's grid
        QString country;        // spotter's country (empty if ours/unknown)
        // [viewall] Non-empty only for All-view spots: the reporting
        // station that heard the plotted sender (hover detail).
        // [connlines] Reporting station's bearing/distance from my
        // grid (All-view spots only; dist < 0 = unknown) so the
        // Connections overlay can draw sender-to-spotter lines
        // without per-paint geodesic work.
        // [mondots] monitorOnly = hollow rendering (no valid SNR for
        // this view). rxOnly = genuinely receive-only station (MQTT
        // reporter never heard transmitting) — the ONLY case hover
        // labels "monitor" (heard-by-me senders are not monitors;
        // operator 2026-08-15).
        bool monitorOnly = false;
        bool rxOnly = false;
        bool pskr = false;      // [pskrtoggle] internet-sourced spot
        // [allsuper] Spot's SNR is a report of MY signal (my-view
        // datasets) — exempt from the All-view color override.
        bool reportsMe = false;
        // [linecolor] Was the sender->reporter relationship supplied by
        // a PSKR spot? Yellow is PSKR STRICTLY, and heardBy only ever
        // comes from the internet feed -- but a station can be VISIBLE
        // for radio reasons while still carrying a PSKR-sourced
        // heardBy, and the line must not draw then.
        // [radioage] Last RADIO evidence for this station; `when`
        // may be refreshed by internet reports, so PSKR-off aging
        // must use this clock (dot-without-line gap, 2026-08-15).
        QDateTime radioWhen;
        int snr = -99;          // dB as reported by the spotter;
                                // -99 = NO REPORT sentinel — never
                                // default to a claimable real value
        qint64 freqHz = 0;      // exact RF Hz the spotter logged us at
        float azimuth = 0.0f;   // degrees true, from my grid
        // ALWAYS km (unit conversion at paint). NEGATIVE = UNPLACED,
        // matching HeardEdge/HearingEntry. It defaulted to 0.0f, which
        // the visibility check reads as a valid position zero km away
        // -- so any station whose grid is unknown was drawn ON TOP OF
        // MY STATION (operator, 2026-08-22: "why is AK6OI on the map?
        // no grid, it's plotted over my station"). Zero is a real
        // distance; it must never double as "no distance".
        float distance = -1.0f;
        // [snrwho] WHEN THIS STATION REPORTED HEARING ME -- the age of
        // the X->ME edge, which is NOT the station's last-seen. Other
        // evidence refreshes `when` constantly, so showing that beside
        // "hears me at -10" claimed a 23-minute-old report was current
        // (W3NIC, 2026-08-23: station 51 s, its report of us 1413 s).
        // One fact, one clock.
        QDateTime reportsMeWhen;
        QDateTime lastTxWhen;   // newest heard-in-any-edge evidence
    };

    // [spotwin] STORAGE horizon: spots are retained in memory per
    // band for a full hour (fleet stations on stock JS8Call re-spot
    // at most hourly per band — the 1-hour reporting cache — so a
    // shorter accumulation misses most of the fleet). The DISPLAY
    // window (15/30/60 buttons, m_viewWindowSecs) filters at paint
    // time; band changes keep each band's accumulated hour.
    static constexpr int WINDOW_SECS = 60 * 60;
    // Default view = the full hour (operator, 2026-08-03: with the
    // fleet's hourly re-spot cache, anything shorter hides most
    // reporting stations by default).
    static constexpr int DEFAULT_VIEW_SECS = 60 * 60;
    static constexpr int SNR_COLD = -25; // dB → blue
    static constexpr int SNR_HOT = 10;   // dB → red

    void rebuildTopics();
    void requestReplot();
    void pruneBand(QString const &band);
    // Outline cache is built CENTER-RELATIVE (origin = chart center)
    // and translated at draw time, so panning never rebuilds it; only
    // grid/R/scale/cut changes do.
    void rebuildMapCache(float R, float scaleKm, float cutKm);
    static float niceCeil(float value); // next 1/2/5 x 10^n
    static float stepScale(float scale, int dir); // ±1 step on the ladder
    static QColor snrColor(int snr, float alphaScale);

    QSettings *m_settings;
    Configuration const *m_config;
    MqttClient *m_mqtt;
    qint64 m_dialHz = 0;
    // Country-name lookup by callsign (LogBook/cty.dat, injected by
    // the main window — this widget has no logbook dependency).
    std::function<QString(QString const &)> m_countryLookup;
    // [operator 2026-08-30] Busy-wait probe installed by the main
    // window: empty string = radio idle, otherwise the toast text
    // ("Wait until outgoing message completed" / "Transfer already
    // in progress"). The relay-builder Done gate asks it.
    std::function<QString()> m_txBusyProbe;
    std::function<int()> m_congestionProbe;   // [congestion]

  public:
    void setTxBusyProbe(std::function<QString()> fn) {
        m_txBusyProbe = std::move(fn);
    }
    // [congestion] band congestion index probe (1-10), installed by
    // the main window; painted above the distance legend.
    void setCongestionProbe(std::function<int()> fn) {
        m_congestionProbe = std::move(fn);
    }
    // [#207 waitopts] Response-wait config, owned by the main
    // window (mode: 0 Short, 1 Adaptive on-air, 2 Long, 3 Adaptive
    // PSKR; per-metric thresholds 1-10). The Options dialog pulls
    // current values through the probe on open and pushes every
    // change through the sink.
    struct WaitConfig {
        int mode;
        int threshOnAir;
        int threshPskr;
    };
    void setWaitConfigProbe(std::function<WaitConfig()> fn) {
        m_waitProbe = std::move(fn);
    }
    void setWaitConfigSink(std::function<void(int, int, int)> fn) {
        m_waitSink = std::move(fn);
    }
    // [pskrbusy] Internet congestion metric + its availability (a
    // spot arrived within the metric window); the executor's PSKR
    // adaptive mode asks these and falls back to on-air when false.
    int pskrCongestionIndex() const;
    bool pskrDataAvailable() const;
    void setCountryLookup(std::function<QString(QString const &)> fn) {
        m_countryLookup = std::move(fn);
    }

  private:

    QString m_currentBand;
    QString m_myCall;
    QString m_myGrid;
    QString m_stateText;
    int m_skippedSpots = 0;
    int m_debugDumpsLeft = 0; // first-N payload dumps after connect

    QPixmap m_pixmap;
    QTimer m_replotTimer; // debounce
    QTimer m_pruneTimer;
    bool m_restoreVisible = false;
    // [visrace2 2026-08-21] Set once app shutdown begins. A close
    // arriving after this is teardown, NEVER user intent -- see
    // closeEvent().
    bool m_shuttingDown = false;

    // Toast overlay (lazy-created by showToast()).
    class QLabel *m_toast = nullptr;
    QTimer m_toastTimer;

    // Zoom controls (upper-left, vertical: + / Auto / −). 0 = auto.
    // PERSISTED since [persistui] 2026-08-15 (operator revision
    // superseding the 2026-08-02 "always start in Auto" directive) --
    // the old comment claiming session-only survived the change and
    // contradicted saveSettings() for six days.
    class QToolButton *m_zoomInBtn = nullptr;
    class QToolButton *m_zoomAutoBtn = nullptr;
    class QToolButton *m_zoomOutBtn = nullptr;
    float m_manualScaleKm = 0.0f;
    float m_lastScaleKm = 0.0f; // effective scale of the last redraw
    // [fitdamp] Last AUTO-computed scale — the shrink hysteresis
    // compares only against this (manual scales polluted the damper:
    // Auto after "−" refused to re-fit). 0 = no damping (fresh fit).
    float m_lastAutoScaleKm = 0.0f;

    // [spotwin] View-window buttons (5/15/30/60 min, upper right).
    // PERSISTED ([persistui]); the value is VALIDATED on load against
    // the button set -- an out-of-set value (e.g. left by an
    // experimental build) checked the 60m button while filtering at
    // the stale number, which reads exactly like "the age selection
    // does nothing".
    class QToolButton *m_win5Btn = nullptr;
    class QToolButton *m_win15Btn = nullptr;
    class QToolButton *m_win30Btn = nullptr;
    class QToolButton *m_win60Btn = nullptr;
    int m_viewWindowSecs = DEFAULT_VIEW_SECS;
    QDateTime m_accumStart; // [spotfmt] when accumulation (re)started
    void positionWindowButtons();

    // [viewall] View-selector buttons (lower-left, vertical stack:
    // my call over "All"). My view = spots of MY signal (spotters
    // plotted, as always). All view = every JS8 spot on the band
    // (the heard SENDER plotted, hover shows who reported it).
    // Persists ([persistui]). NOTE: these two must NOT use
    // autoExclusive — that would join the win5/15/30/60 sibling
    // group (autoExclusive groups by parent) — QButtonGroup instead.
    class QToolButton *m_viewMineBtn = nullptr;
    class QToolButton *m_viewAllBtn = nullptr;
    bool m_viewAll = false;
    // [connlines] "Show connections" toggle (lower right): 1 px
    // lines — dark yellow = PSKR-sourced, blue = on-air mesh.
    // Persists ([persistui]).
    class QToolButton *m_connBtn = nullptr;
    bool m_showConnections = false;
    // [relaysel] Relay-path builder. "Select relay(s)" toggles click-
    // to-append mode: my station -> first click -> next click -> ...;
    // "Undo" pops the last hop, "Done" emits the template. Session-
    // only; disabling the toggle (or reopening) clears the path.
    class QToolButton *m_relaySelBtn = nullptr;
    class QToolButton *m_relayDoneBtn = nullptr;
    class QToolButton *m_relayUndoBtn = nullptr;
    bool m_relaySelect = false;
    QStringList m_relayPath;
    // [relaykeep] Snapshot of each selected hop's Spot at click time:
    // a hop that ages out of the view window (or the window shrinks)
    // stays drawn from its snapshot while relay-select is active —
    // it leaves the map only via Undo or exiting the mode (operator,
    // 2026-08-14). Parallel to m_relayPath.
    QVector<Spot> m_relayPathSpots;
    void updateRelayButtons();
    // [showfix] Genuine-open detection (first show, or show after
    // close): only the relay builder resets there — everything else
    // persists. Minimize-restore and desktop-switch also fire
    // showEvent and must not reset anything (operator, 2026-08-15,
    // twice).
    bool m_resetOnNextShow = true;
    // [autoroute 2026-08-28] Auto-route mode: the operator names a
    // target (typed or clicked) and the reaching executor runs it
    // unattended. Map-side state only -- the mainwindow owns the
    // executor and the main-screen lock.
    //   armed  = button checked, waiting for a target
    //   active = target chosen, executor running
    class QToolButton *m_autoRouteBtn = nullptr;
    bool m_autoRouteArmed = false;
    bool m_autoRouteActive = false;
    QString m_autoRouteTarget;
    class QFrame *m_autoRoutePanel = nullptr;   // the target prompt
    class QLineEdit *m_autoRouteEdit = nullptr;
    class QToolButton *m_autoRouteStartBtn = nullptr;
    class QToolButton *m_autoRouteCancelBtn = nullptr;
    // [#207 waitopts] Options button + response-wait dialog.
    class QToolButton *m_optionsBtn = nullptr;
    class QToolButton *m_optionsCloseBtn = nullptr;
    class QFrame *m_optionsPanel = nullptr;
    class QRadioButton *m_waitShort = nullptr;
    class QRadioButton *m_waitLong = nullptr;
    class QRadioButton *m_waitAdaptive = nullptr;      // on-air
    class QRadioButton *m_waitAdaptivePskr = nullptr;  // PSKR
    class QSpinBox *m_threshSpin = nullptr;   // hidden right-click
    class QTimer *m_threshTimer = nullptr;    // inactivity closer
    bool m_threshSpinPskr = false; // which threshold the spin edits
    int m_curThreshOnAir = 7;      // dialog mirrors of the owner's
    int m_curThreshPskr = 7;       // per-metric thresholds
    std::function<WaitConfig()> m_waitProbe;
    std::function<void(int, int, int)> m_waitSink;
    void optionsShowPanel();
    void positionOverlayPanel(QFrame *panel);
    // [pskrbusy] MQTT spot arrival stamps (ms), trailing 5 min on
    // the current band topic; pruned on arrival, cleared on
    // resubscribe.
    QVector<qint64> m_pskrArrivals;
    int m_btnColW = 0;   // right-hand button column width (layout)
    int m_leftColW = 0;  // left-hand button column width (layout)
    // The mode's status line: its own label just above the distance
    // scale legend (operator, 2026-08-28) -- transient toasts keep
    // their own position and timing, untouched.
    class QLabel *m_statusLine = nullptr;
    // Sticky toast text: while non-empty, the toast label reverts to
    // this after any transient toast expires instead of hiding --
    // the "in progress..." status line rides the SAME control as the
    // toast (operator spec).
    QString m_stickyToast;
    // [hoverabove v2] station of the PREVIOUS hover session; a
    // second hover of the same station shows its text above the dot.
    QString m_prevHoverCall;
    void autoRouteChooseTarget(QString const &target);
    void autoRouteShowPanel();
    void positionAutoRoutePanel();
    void setStickyToast(QString const &text);

    // [hearlines] On-air heard-mesh storage: band → hearer →
    // (position + per-heard-call edges with their own timestamps).
    // Edges prune on the same 1 h horizon as spots; the view-window
    // filter applies at paint.
    struct HeardEdge {
        QDateTime when;
        // [#170(f)] No az/dist here. Position is derived from the
        // grid by the ONE authority (m_gridByCall) at paint time;
        // storing it meant keeping a second copy in sync, which is
        // what the backfill walk existed to do.
        QString grid;
        // [#161 querycall] THIRD-PARTY snr (hearer's copy of the
        // heard station, e.g. a QUERY CALL "YES +08" reply). Feeds
        // this edge ONLY — never the reportedToMeSnr color
        // authority.
        int snr = -99;
        // [tribblenet] PROVENANCE, and the three differ in KIND:
        //   "radio"   FIRST-HAND -- our own receiver decoded it.
        //   "mqtt"    INTERNET -- PSKReporter said so. Vanishes in an
        //             offline session (#159), so never route on it
        //             when planning for no-internet operation.
        //   "hearing" THIRD-PARTY TESTIMONY, RF-derived: a HEARING?
        //             list, or a QUERY CALL reply -- INCLUDING one
        //             that reached us via a RELAY (operator
        //             2026-08-21). Our radio never copied the station
        //             being described; a remote station claims it,
        //             and the SNR/age are THAT station's copy,
        //             backdated by transit. Valid offline, but not an
        //             observation.
        // Without this, radio and internet edges are
        // indistinguishable -- which silently breaks #159 offline
        // routing and hides whether a relay is reachable by RF at all.
        QString source;
    };
    struct HearingEntry {
        QDateTime lastSeen;   // presence freshness (HBs, any frame)
        // [#170(f)] No az/dist here. Position is derived from the
        // grid by the ONE authority (m_gridByCall) at paint time;
        // storing it meant keeping a second copy in sync, which is
        // what the backfill walk existed to do.
        QString grid;
        int snr = -99;        // SNR this station REPORTED TO US only
        // [maptruth 2026-08-27] THE ROOT of the recurring stale-report
        // family: this value had NO timestamp, so every display
        // borrowed a clock from somewhere else (W3NIC 08-23, KA9GAP
        // 08-27, then the audit found 106 live cases). Value and
        // clock are ONE record now; every consumer reads the pair.
        QDateTime snrWhen;    // when that report was OBSERVED
        // [tribblenet audit 2026-08-21] PRESENCE provenance, same
        // vocabulary as HeardEdge::source. Needed because the PSKR
        // feed now populates this store: without it the anchor pass
        // synthesized dots for internet-only stations even with "Add
        // PSKReporter spots" OFF, silently breaking that toggle's
        // contract. "radio" once ANY on-air evidence is seen -- a
        // station heard on the air never reverts to internet-only.
        QString source;
        QHash<QString, HeardEdge> heard;
    };
    // [oneobs] THE observation store: band -> hearer -> (position +
    // per-heard-call edges, each with its own time, SNR and source).
    // ONE record per observation; circles AND lines are drawn from it.
    QHash<QString, QHash<QString, HearingEntry>> m_hearingByBand;
    // [oneobs] Facts that belong to a STATION rather than to any one
    // observation -- country and the frequency THEY transmit on. Small,
    // and kept beside the observations rather than inside them so an
    // observation stays a pure relationship.
    struct StationInfo {
        QString country;
        qint64 freqHz = 0;
        QDateTime freqWhen;   // when freqHz was observed
        bool sawAsSender = false;  // observed transmitting => not rxOnly
        // [passband #218] TRUE when freqHz came from a frame WE
        // decoded, FALSE when it came from a PSKR spot. First-hand
        // evidence outranks second-hand: a station we just decoded is
        // PROVABLY inside our passband at that instant, whereas a
        // PSKR report is an internet fact about someone else's
        // receiver. A radio observation therefore overwrites a PSKR
        // one, and a PSKR one never overwrites a fresh radio one.
        bool freqFromRadio = false;
    };
    QHash<QString, QHash<QString, StationInfo>> m_infoByBand;
    // [mqttgrid] call -> locator harvested from EVERY MQTT message
    // (sender sc/sl and reporter rc/rl) — fallback grid source for
    // on-air stations the sanctioned text sources haven't placed.
    QHash<QString, QString> m_gridByCall;
    // [#164] Persistent tier of the authority above — seeds it at
    // startup, records every accepted refinement. Never read
    // directly by consumers.
    GridDb m_gridDb;
    // [#168 part 3] Write-behind journal timer. RAM stays the query
    // authority; this only drains the queue into ONE transaction.
    QTimer m_dbFlushTimer;
    // Restore the who-hears-whom mesh (and spots) banked by previous
    // sessions. Without this every restart destroyed the mesh, and a
    // route search could only ever see what refilled since — measured
    // three times mid-route on 2026-08-21.
    void restoreMeshFromDisk();
    // [#168 part 3] Journal one accepted observation as STATION FACTS.
    // ONE place, called from BOTH insertion paths (on-air and PSKR) --
    // patching each site separately is how the two drift apart. The
    // RELATIONSHIP it implies is journalled by addHearingReport as an
    // edge; this records only what is true of the station.
    void journalStation(QString const &band, QString const &call);
    void restoreStationsFromDisk();
    int m_wheelAccum = 0; // [wheelzoom] trackpad delta accumulator
    // [zoomkeepcenter] Auto-fit pan applied at the last redraw —
    // baked into m_panPx when the operator leaves Auto so manual
    // zoom keeps the same center point.
    QPointF m_autoPanPx;
    QToolButton *m_pskrBtn = nullptr; // [pskrtoggle]
    QToolButton *m_callsBtn = nullptr; // [callsbtn]
    // [attemptviz 2026-08-22] Live picture of what we are trying RIGHT
    // NOW. A call we put on the air draws a fat dashed red path along
    // the relay chain; the dash GAP widens as the reply window runs
    // down, so the line visibly "runs out" and then vanishes. A reply
    // from a station we called turns its path fat green for a few
    // seconds. Purely presentational: nothing here feeds routing.
    struct Attempt {
        QStringList path;      // [relay..., target], uppercase
        QDateTime started;
        int waitSecs = 70;     // when the dashes finish opening up
        bool replied = false;  // green, and on its own short timer
        QDateTime repliedAt;
    };
    QVector<Attempt> m_attempts;
    class QTimer *m_attemptTimer = nullptr;
    bool m_tracerHitsMe = false;  // [#183] a lifted line ends at us
    void tickAttempts();

public:
    // Called by the app: an outgoing directed call/relay, and a reply
    // from a station we called (HB replies included).
    void noteAttempt(QStringList const &path, int waitSecs);
    void noteReply(QString const &from);
    // We have STOPPED WAITING. The map's countdown is its own estimate
    // of how long an answer might take; the caller knows when it has
    // actually given up, and that is usually sooner. Clearing then
    // rather than letting the dashes run out tells the operator the
    // attempt is over and they can move on (operator, 2026-08-22).
    // Only un-replied attempts go: a green result is a result.
    void clearAttempts();
    // [autoroute] Mainwindow pushes ARQ-session state (gates the
    // Auto-route button) and reports mode end. canceled=true shows
    // the "canceled" toast; success/failure dialogs are the
    // mainwindow's.
    void autoRouteEnded(bool canceled);
    // [modeowner] Owner notification: the mode actually started.
    // The ONLY setter of this widget's active-mode UI state.
    void autoRouteStarted(QString const &target);
    // [hbrelay TODO #191] A heartbeat announced relay-enabled:
    // session-RAM set feeding the "Relay enabled" hover alongside
    // the habit-proven known-relayers.
    void noteRelayFlag(QString const &call);
    QSet<QString> m_flagRelayers;
    // journal throttle: one hbflag row per station per hour
    QHash<QString, qint64> m_flagJournaledMs;
    // [#187 intelminer] Seed log-mined grids: bank (INSERT OR
    // IGNORE, source='log') + the RAM map when absent there. Returns
    // rows actually inserted. GUI thread only (owns the bank).
    int seedLogGrids(QVector<GridDb::LogSeed> const &rows);
    bool autoRouteMode() const { return m_autoRouteActive; }

private:
    bool m_showPskr = true;
    // [hoverlift] Station under the cursor, uppercase, empty when none.
    // When the PSKR lines have been muted for density, this station's
    // lines are drawn at full brightness and on top, so one station can
    // be read out of a crowded field without changing the view.
    QString m_hoverCall;
    // [paintlog] What the last paint actually did, so a slow frame can
    // be attributed instead of guessed at.
    mutable int m_lastNoteCount = 0;
    mutable int m_lastSegCount = 0;
    mutable qint64 m_lastPaintMs = 0;  // drives requestReplot's interval
    mutable qint64 m_lastBuildMs = 0;  // render-set rebuild
    mutable qint64 m_lastGeoMs = 0;    // coastline walk, per frame
    mutable int m_lastGeoPts = 0;      // points in that walk
    mutable int m_lastGeoPolys = 0;
    mutable qint64 m_lastSegMs = 0;    // everything after the lines
    mutable QElapsedTimer m_segTimer;
    void rememberGrid(QString const &call, QString const &grid,
                      QString const &source =
                          QStringLiteral("radio"));
    QString refinedGrid(QString const &call, QString const &grid) const;
    void showRelayPathToast();
    void stepZoom(int dir); // [zoomkeepcenter]
    // [radioage] THE presence/age clock; [snrwho] THE reported-to-me
    // SNR rule — single definitions, every consumer reads these.
    QDateTime effectiveWhen(Spot const &s) const;
    ReportOfMe reportedToMe(Spot const &s) const;

    // Drag-to-pan (persists, [persistui]; the Auto button zeroes it).
    // m_panPx = chart-center offset from the geometric center, in
    // logical px. Spots draw at TRUE radial position (no outer-ring
    // clamp) so a zoomed-in view can pan out to distant clusters.
    QPointF m_panPx;
    bool m_maybeDrag = false;
    bool m_dragging = false;
    QPointF m_pressPos;
    QPointF m_panAtPress;

    bool m_showCallsigns = false; // [callsbtn] persisted toggle

    // Projected world-outline cache (azimuthal equidistant around my
    // grid, clipped to the current scale) — rebuilt only when the
    // center/scale/geometry changes.
    QVector<QPolygonF> m_mapCache;
    QString m_mapCacheGrid;
    float m_mapCacheScale = -1.0f;
    float m_mapCacheR = -1.0f;
    float m_mapCacheCut = -1.0f;

    // Screen positions of the current band's spots as of the last
    // redraw, for hover lookup.
    struct ScreenSpot {
        QPointF pos;
        Spot spot;
    };
    QVector<ScreenSpot> m_screenSpots;
    // Nearest spot within the hit radius, or nullptr.
    ScreenSpot const *hitTest(QPointF const &pos) const;
};

#endif
