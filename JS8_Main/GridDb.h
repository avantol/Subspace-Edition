#ifndef GRID_DB_HPP__
#define GRID_DB_HPP__

/**
 * @file GridDb.h
 * @brief [TODO #164 / #168] Persistent SQLite tier of the live band
 * picture: callsign->grid, station ACTIVITY, and TRIBBLENET -- the
 * live who-hears-whom routing mesh (named by the operator
 * 2026-08-21: small in scope, cute in pretension, and the relays
 * keep multiplying every time you look).
 *
 * This is NOT a second authority. SpotMapWindow's in-RAM stores remain
 * the single in-session authority every consumer reads and every query
 * hits; GridDb only (1) SEEDS them at startup and (2) journals what
 * the authority ACCEPTS. No query path touches SQLite, so there is no
 * latency to reason about.
 *
 * WHY TRIBBLENET IS PERSISTED HERE (#168 part 3, the part that
 * matters for routing). The hearing store was RAM-only with a 1 h window, so every
 * restart destroyed it. Measured three times on 2026-08-21 while
 * routing to AL0A: each restart dropped the map to ~0 edges (the
 * grids survived, being already persisted) and each time the app had
 * ALREADY EARNED the exact edge the router needed -- KF0DRT's relay
 * forward proving it could hear us, AE0YH's "*DE* WM8Q" -- and threw
 * it away minutes later. Without this the only way to plan was to
 * re-derive the live window from ALL.TXT by hand, which is precisely
 * what the operator forbids. Edge persistence is what makes "no path
 * found" a real answer instead of an artifact of when someone last
 * restarted.
 *
 * DESIGN (agreed 2026-08-21):
 *   * RAM is the authority; this is a WRITE-BEHIND JOURNAL. Rows are
 *     queued in memory and flushed in ONE transaction on a timer --
 *     a transaction per row would mean an fsync per row.
 *   * WAL + synchronous=NORMAL.
 *   * RETENTION IS NOT THE MAP'S WINDOW. The map RENDERS ~1 h; we
 *     KEEP 24 h, because route search gets dramatically better with
 *     more edges and the volume is trivial (a busy hour is a few
 *     hundred rows).
 *   * RAW GRIDS ONLY -- never the my-grid-relative az/dist carried in
 *     HeardEdge, which are meaningless after a move (#154/#164 trap).
 *     Bearings are recomputed on load.
 *   * Every edge carries its SOURCE (radio / mqtt / hearing), because
 *     PSKReporter uploading is OPTIONAL: a non-publishing station's
 *     ears reach us ONLY via a HEARING? reply, and a router that
 *     cannot tell those apart silently limits itself to publishers.
 *
 * SCHEMA CHANGES ARE DROP-AND-RECREATE, deliberately: GridDb shipped
 * only in Build 371 and only on the bench, so there is nothing in the
 * field to migrate (operator: "nobody else has the grid db, no
 * migration"). SCHEMA_VERSION bump wipes and rebuilds. That also
 * disposes of the old last_seen/count columns, which meant last grid
 * CHANGE rather than activity.
 *
 * File: config dir alongside JS8Call.ini, per-instance suffix rules
 * ([multiinst]): JS8Call<suffix>-grids.db.
 */

#include <QDateTime>
#include <QHash>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

class GridDb final {
  public:
    // Bump to wipe and rebuild. No migration code by design.
    static constexpr int SCHEMA_VERSION = 4;
    // Keep a day on disk; the map still renders only WINDOW_SECS.
    static constexpr qint64 RETAIN_SECS = 24 * 60 * 60;

    // [#187 intelminer] A grid mined from the user's own logs
    // (heartbeat/CQ fields, corroborated). Seeded INSERT OR IGNORE
    // with source='log' -- never overwrites radio or mqtt rows, and
    // survives the JS8_NO_MQTT load filter (it is radio-derived).
    struct LogSeed {
        QString call;
        QString grid;    // 4-char
        qint64 whenS = 0;
        int count = 0;   // corroboration
    };
    bool seedLogGrid(LogSeed const &s);

    struct EdgeRow {
        QString band;
        QString hearer;
        QString heard;
        QString hearerGrid;   // raw, never az/dist
        QString heardGrid;
        QString source;       // radio | mqtt | hearing
        qint64 when = 0;      // secs since epoch, UTC
        int snr = -99;
    };

    // ONE ROW PER STATION -- facts about the station itself.
    //
    // [schema v4 2026-08-21] Replaces the old `spots` table, whose
    // primary key was (band, call, heard_by): that made every row a
    // RELATIONSHIP, storing the same who-heard-whom that `edges`
    // already holds, and repeating the station's own grid, country and
    // frequency on every one. Measured on the bench before the change:
    // 6645 spot rows describing 248 distinct stations, with W0IFM's
    // grid written 156 times. Relationships belong in edges; a
    // station's facts belong here, once.
    //
    // DROPPED with the old table:
    //   heardBy / heardByGrid  -- that IS the relationship (edges).
    //   mine                   -- provably identical to reportsMe at
    //                             every call site, so it was a second
    //                             name for one fact.
    //   monitorOnly            -- derived (no report of my signal).
    //   pskr                   -- derived (no radio clock).
    struct StationRow {
        QString band;
        QString call;
        QString grid;
        QString country;
        // THEIR transmit frequency. Never "the frequency they heard ME
        // on" -- that is a property of the REPORT, not of the station
        // (operator, 2026-08-21: "which freq? theirs or mine?").
        qint64 freqHz = 0;
        // [passband #218 freqclock 2026-09-11] The frequency's OWN
        // clock and source, persisted beside it. Without them a
        // restored frequency had no date, so the passband verdict
        // could only call it Unknown -- and a station we decoded five
        // minutes before a restart came back drawn at a dial it could
        // not possibly be heard on (KK6WVY at 7.0855, field
        // 2026-09-11). radio_when is the STATION's clock and cannot
        // stand in: freq_hz "only improves", so it may be older or
        // newer than the last radio sighting.
        qint64 freqWhen = 0;  // 0 = unknown, verdict treats as Unknown
        bool freqRadio = false; // from a frame WE decoded
        qint64 anyWhen = 0;   // freshest evidence of any source
        qint64 radioWhen = 0; // freshest RADIO evidence, 0 = none
        int snrToMe = -99;    // their report of MY signal
        qint64 snrToMeWhen = 0; // [snrpersist] when that report was
                                // observed -- a dB without its date
                                // is the lie the map audit killed
        bool reportsMe = false;
        bool rxOnly = false;  // never observed transmitting
    };

    GridDb() = default;
    ~GridDb();
    GridDb(GridDb const &) = delete;
    GridDb &operator=(GridDb const &) = delete;

    bool open(QString const &path);
    bool isOpen() const { return m_db.isOpen(); }
    // [sqlerr] Why open() failed, for the UI owner's startup dialog:
    // DriverMissing = broken installation (the SQLite plugin never
    // shipped); OpenFailed = driver fine, file unopenable
    // (permissions / corruption). None after a successful open.
    enum class OpenFailure { None, DriverMissing, OpenFailed };
    OpenFailure openFailure() const { return m_openFailure; }
    QString openErrorText() const { return m_openErrorText; }

    // ---- grids: the original authority tier -----------------------
    QHash<QString, QString> loadAll() const;
    void upsert(QString const &call, QString const &grid,
                QString const &source);
    // [#168 part 1] Presence, on EVERY sighting -- distinct from a
    // grid CHANGE. Throttled per call so a chatty station cannot
    // dominate the write queue.
    void noteActivity(QString const &call, int snr);

    // ---- mesh + spots: queued, flushed as one transaction ---------
    void queueEdge(EdgeRow const &e);
    void queueStation(StationRow const &s);
    // Writes everything queued in ONE transaction and prunes rows
    // older than RETAIN_SECS. Safe to call when nothing is pending.
    void flush();
    int pendingCount() const {
        return m_pendingEdges.size() + m_pendingStations.size();
    }

    QVector<EdgeRow> loadEdges(qint64 notOlderThanSecs) const;
    QVector<StationRow> loadStations(qint64 notOlderThanSecs) const;

    // ---- reach events: months-scale habit observations ------------
    // [habitstore 2026-08-27] The executor's per-attempt observations
    // are habit data (asked-vs-forwarded, called-vs-answered, routes
    // that WORKED) and were dying with the process -- "we're throwing
    // away good data". Observations only, never decayed values; the
    // recency math stays computed. kinds: 'fwd' (asked to forward,
    // ok = keyed), 'ans' (called, ok = answered; extra = 'delivered'
    // when a forward completed first), 'reached' (ok = 1, extra = the
    // chain -- TODO #182 save successful routes). Retained 90 days.
    struct ReachEventRow {
        QString band, station, kind, extra;
        qint64 when = 0;
        bool ok = false;
    };
    static constexpr qint64 REACH_RETAIN_SECS = 90ll * 24 * 3600;
    void queueReachEvent(ReachEventRow const &r);
    // [nonrelayer 2026-08-29] Rows queued but not yet flushed --
    // readers that must see the newest events (the relay-status
    // hover raced the flush and stayed stale until restart) merge
    // these with loadReachEvents().
    QVector<ReachEventRow> const &pendingReachEvents() const {
        return m_pendingReach;
    }
    QVector<ReachEventRow> loadReachEvents() const;

  private:
    bool ensureSchema();
    void wipe();

    QSqlDatabase m_db;
    QString m_connName;
    OpenFailure m_openFailure = OpenFailure::None;   // [sqlerr]
    QString m_openErrorText;
    QVector<EdgeRow> m_pendingEdges;
    QVector<ReachEventRow> m_pendingReach;
    QVector<StationRow> m_pendingStations;
    // call -> last activity write, for the 1/min throttle.
    QHash<QString, qint64> m_lastActivityWrite;
    qint64 m_lastPrune = 0;
};

#endif
