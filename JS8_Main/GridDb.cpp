/**
 * @file GridDb.cpp
 * @brief See header.
 */

#include "GridDb.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

Q_LOGGING_CATEGORY(griddb_js8, "js8.griddb")

namespace {
// One write per call per minute is plenty for a presence signal and
// bounds the queue when the band is busy.
constexpr qint64 kActivityThrottleSecs = 60;
// Pruning walks the whole table; hourly is ample for a day's data.
constexpr qint64 kPruneEverySecs = 60 * 60;

qint64 nowSecs() {
    return QDateTime::currentDateTimeUtc().toSecsSinceEpoch();
}

// [nointernet, operator 2026-08-28] JS8_NO_MQTT simulates no
// internet (MqttClient.cpp:53 disables the connection); with it set,
// the LOADS also exclude everything internet-derived, so the
// simulation is "internet never existed", not "internet just went
// down": grids/edges rows tagged source='mqtt', and station rows
// with no radio evidence at all (radio_when = 0). Writes are
// untouched -- the stored data survives for normal sessions.
bool noInternetSim() {
    static bool const on = qEnvironmentVariableIsSet("JS8_NO_MQTT");
    return on;
}
} // namespace

GridDb::~GridDb() {
    if (m_db.isOpen()) {
        flush(); // do not lose the last window on a clean exit
        m_db.close();
    }
    m_db = QSqlDatabase{}; // release before removeDatabase
    if (!m_connName.isEmpty())
        QSqlDatabase::removeDatabase(m_connName);
}

bool GridDb::open(QString const &path) {
    // Unique connection name — a second instance ([multiinst]) in the
    // same process (tests) must not collide.
    static int serial = 0;
    // [sqlerr 2026-08-30, operator: "wouldn't an error message be
    // more appropriate?"] A missing QSQLITE driver is a BROKEN
    // INSTALLATION (Windows/MSIX: sqldrivers\qsqlite.dll +
    // libsqlite3-0.dll are separately-losable files), and until now
    // it only wrote a diag line while the app ran on with no grid
    // bank, no habit journal, no reach events -- a masked
    // deterministic failure. Record the failure class; the UI owner
    // shows the dialog (this class stays widget-free).
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        m_openFailure = OpenFailure::DriverMissing;
        m_openErrorText = QStringLiteral("QSQLITE driver not found");
        qCWarning(griddb_js8)
            << "[GRIDDB] SQLite DRIVER MISSING -- broken install";
        return false;
    }
    m_connName = QStringLiteral("griddb-%1").arg(++serial);
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                     m_connName);
    m_db.setDatabaseName(path);
    if (!m_db.open()) {
        // [sqlerr2] Windows field log 2026-08-30 18:52Z: QSQLITE was
        // in the AVAILABLE list (registration reads metadata only)
        // yet load failed "Driver not loaded" -- the plugin file
        // shipped but its engine DLL did not. That is the broken-
        // install class, not a permissions problem.
        m_openFailure =
            m_db.lastError().text().contains(
                QStringLiteral("Driver not loaded"))
                ? OpenFailure::DriverMissing
                : OpenFailure::OpenFailed;
        m_openErrorText = m_db.lastError().text();
        qCWarning(griddb_js8)
            << "[GRIDDB] open FAILED:" << path
            << m_db.lastError().text();
        return false;
    }
    m_openFailure = OpenFailure::None;
    // Write-behind wants WAL: readers never block, and NORMAL means we
    // are not fsyncing per transaction. A crash can cost the last
    // flush interval, which is band observations we can re-hear.
    QSqlQuery pragma{m_db};
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    if (!ensureSchema()) {
        m_db.close();
        return false;
    }
    // Normalise grids already on disk. queueEdge() uppercases from now
    // on, but 5194 rows were written before that and would stay mixed
    // for as long as they live -- leaving the trap in place for
    // exactly the data most likely to be compared later. Idempotent
    // and cheap at these row counts, so it runs unconditionally rather
    // than needing a schema bump to carry it.
    {
        QSqlQuery fix{m_db};
        if (fix.exec(QStringLiteral(
                "UPDATE edges SET hearer_grid = upper(hearer_grid),"
                " heard_grid = upper(heard_grid)"
                " WHERE hearer_grid <> upper(hearer_grid)"
                "    OR heard_grid <> upper(heard_grid)"))) {
            if (int const n = fix.numRowsAffected(); n > 0)
                qCWarning(griddb_js8)
                    << "[GRIDDB] normalised" << n << "mixed-case grids";
        } else {
            qCWarning(griddb_js8)
                << "[GRIDDB] grid normalise FAILED:"
                << fix.lastError().text();
        }
    }
    qCWarning(griddb_js8) << "[GRIDDB] open:" << path
                          << "schema v" << SCHEMA_VERSION;
    return true;
}

void GridDb::wipe() {
    QSqlQuery q{m_db};
    for (auto const &t : {QStringLiteral("grids"), QStringLiteral("edges"),
                          QStringLiteral("spots"),
                          QStringLiteral("stations")}) {
        // NEVER ignore this result. It failed silently for three
        // builds (2026-08-21): an active read cursor on the same
        // connection blocks DDL, so every DROP returned "database
        // table is locked" while the caller logged a rebuild that
        // never happened -- grid counts kept GROWING across restarts
        // that claimed to have wiped, and the spots table kept its
        // old columns so loadSpots() could not even prepare.
        if (!q.exec(QStringLiteral("DROP TABLE IF EXISTS %1").arg(t)))
            qCWarning(griddb_js8)
                << "[GRIDDB] DROP" << t << "FAILED:"
                << q.lastError().text();
    }
}

bool GridDb::ensureSchema() {
    QSqlQuery q{m_db};
    if (!q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS meta ("
            " k TEXT PRIMARY KEY, v TEXT)"))) {
        qCWarning(griddb_js8)
            << "[GRIDDB] meta FAILED:" << q.lastError().text();
        return false;
    }
    int have = 0;
    if (q.exec(QStringLiteral(
            "SELECT v FROM meta WHERE k='schema_version'")) &&
        q.next())
        have = q.value(0).toInt();
    // RELEASE THE CURSOR before any DDL. Qt keeps a SELECT active
    // until the query is finished, and SQLite refuses to DROP a
    // table while a read statement is live on the same connection --
    // which is exactly how the wipe below silently did nothing.
    q.finish();
    // Rebuild whenever the version is not EXACTLY ours. The absent
    // `have &&` guard is deliberate: a file with no version row is
    // NOT a fresh one, and the guard I first wrote skipped the wipe
    // for exactly that case -- CREATE TABLE IF NOT EXISTS then kept
    // the old `grids` table, every upsert failed with "Parameter
    // count mismatch" against columns that did not exist, and the
    // version row got stamped to 2 regardless (caught in the log,
    // 2026-08-21). On a genuinely new database the DROPs are
    // harmless no-ops, so unconditional is simpler and right.
    //
    // The wreckage that produces is MIXED-SCHEMA, not "half-migrated"
    // (operator: "what is there to migrate?"): nothing migrates here
    // by design. `grids` kept its old shape because CREATE TABLE IF
    // NOT EXISTS is a no-op on an existing table, while `edges` and
    // `spots` were created fresh at the new shape -- half the tables
    // rebuilt, half did not, and the stamp then claimed the whole
    // file was current.
    //
    // NOT a field-compatibility mechanism, and worth being precise
    // about (operator: "how does legacy fit in?"): GridDb shipped in
    // Build 371, one day before this, on one bench. There are no
    // deployed databases to protect. What this actually guards
    // against is a file THIS CODE wrote badly -- and the stamp alone
    // cannot do that, because the broken file carries the CURRENT
    // version number over the WRONG columns. Hence the shape check
    // below, which is the real mechanism; the stamp is a cheap
    // second signal that starts mattering only once v2 databases
    // exist in the field and a future v3 has to recognise them.
    if (have != SCHEMA_VERSION) {
        // Drop and rebuild BY DESIGN — nothing in the field to
        // migrate, and the old columns' meanings were wrong anyway.
        qCWarning(griddb_js8)
            << "[GRIDDB] schema v" << have << "->" << SCHEMA_VERSION
            << "- rebuilding (drop and recreate by design)";
        wipe();
    }

    bool ok = true;
    ok &= q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS grids ("
        " call TEXT PRIMARY KEY,"
        " grid TEXT NOT NULL,"
        " source TEXT,"
        " first_seen INTEGER,"
        " grid_changed INTEGER,"  // was the misnamed last_seen
        " change_count INTEGER DEFAULT 1,"
        // [#168 part 1] ACTIVITY, updated on every sighting.
        " last_heard INTEGER,"
        " heard_count INTEGER DEFAULT 0,"
        " snr_last INTEGER DEFAULT -99)"));
    ok &= q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS edges ("
        " band TEXT NOT NULL,"
        " hearer TEXT NOT NULL,"
        " heard TEXT NOT NULL,"
        " when_s INTEGER NOT NULL,"
        " snr INTEGER DEFAULT -99,"
        " hearer_grid TEXT,"
        " heard_grid TEXT,"
        " source TEXT,"
        " PRIMARY KEY (band, hearer, heard))"));
    ok &= q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS stations ("
        " band TEXT NOT NULL,"
        " call TEXT NOT NULL,"
        " grid TEXT,"
        " country TEXT,"
        " freq_hz INTEGER DEFAULT 0,"
        " any_when INTEGER NOT NULL,"
        " radio_when INTEGER DEFAULT 0,"
        " snr_to_me INTEGER DEFAULT -99,"
        " reports_me INTEGER DEFAULT 0,"
        " rx_only INTEGER DEFAULT 0,"
        " PRIMARY KEY (band, call))"));
    // One index, on the prune/age column. At a few thousand rows
    // nothing else earns its keep.
    ok &= q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS reach_events ("
        " band TEXT NOT NULL,"
        " station TEXT NOT NULL,"
        " when_s INTEGER NOT NULL,"
        " kind TEXT NOT NULL,"
        " ok INTEGER NOT NULL,"
        " extra TEXT)"));
    ok &= q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS reach_when"
        " ON reach_events(when_s)"));
    ok &= q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS edges_when ON edges(when_s)"));
    ok &= q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS stations_when ON stations(any_when)"));
    if (!ok) {
        qCWarning(griddb_js8)
            << "[GRIDDB] schema FAILED:" << q.lastError().text();
        return false;
    }
    // [snrpersist 2026-08-27] Dated report column, added in place --
    // ALTER instead of a schema bump so the 90-day habit records and
    // the 24h mesh survive. Fails harmlessly when it already exists.
    {
        QSqlQuery a{m_db};
        a.exec(QStringLiteral(
            "ALTER TABLE stations ADD COLUMN snr_when INTEGER"
            " DEFAULT 0"));
    }
    // [passband #218 freqclock 2026-09-11] Same pattern: the
    // frequency's own clock and source, added in place. A frequency
    // without its date is the same lie snr_when was added to kill.
    {
        QSqlQuery a{m_db};
        a.exec(QStringLiteral(
            "ALTER TABLE stations ADD COLUMN freq_when INTEGER"
            " DEFAULT 0"));
        a.exec(QStringLiteral(
            "ALTER TABLE stations ADD COLUMN freq_radio INTEGER"
            " DEFAULT 0"));
    }
    q.prepare(QStringLiteral(
        "INSERT INTO meta (k, v) VALUES ('schema_version', ?)"
        " ON CONFLICT(k) DO UPDATE SET v = excluded.v"));
    q.addBindValue(QString::number(SCHEMA_VERSION));
    q.exec();
    return true;
}

// [#187 intelminer] See header. INSERT OR IGNORE: an existing row of
// ANY source wins over a mined one -- the bank's live paths carry
// fresher provenance.
bool GridDb::seedLogGrid(LogSeed const &s) {
    if (!m_db.isOpen())
        return false;
    QSqlQuery q{m_db};
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO grids (call, grid, source, first_seen,"
        " grid_changed, change_count, last_heard, heard_count,"
        " snr_last) VALUES (?,?,'log',?,?,1,?,?, -99)"));
    q.addBindValue(s.call);
    q.addBindValue(s.grid);
    q.addBindValue(qlonglong(s.whenS));
    q.addBindValue(qlonglong(s.whenS));
    q.addBindValue(qlonglong(s.whenS));
    q.addBindValue(s.count);
    if (!q.exec()) {
        qCWarning(griddb_js8)
            << "[GRIDDB] log seed FAILED:" << q.lastError().text();
        return false;
    }
    return q.numRowsAffected() > 0;
}

QHash<QString, QString> GridDb::loadAll() const {
    QHash<QString, QString> out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q{m_db};
    if (!q.exec(noInternetSim()
                    ? QStringLiteral(
                          "SELECT call, grid FROM grids WHERE"
                          " COALESCE(source,'') <> 'mqtt'")
                    : QStringLiteral("SELECT call, grid FROM grids"))) {
        qCWarning(griddb_js8)
            << "[GRIDDB] load FAILED:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        QString const g = q.value(1).toString().toUpper(); // [gridcase]
        // [audit 2026-08-21] SKIP EMPTY GRIDS. noteActivity() creates
        // rows for stations whose grid is not yet known (grid ''), so
        // loading blindly injected empty strings into the ONE grid
        // authority and inflated every "N grids" count with entries
        // that locate nothing.
        if (g.isEmpty())
            continue;
        out.insert(q.value(0).toString().toUpper(), g);
    }
    qCWarning(griddb_js8) << "[GRIDDB] seeded" << out.size()
                          << "grids from disk";
    return out;
}

void GridDb::upsert(QString const &call, QString const &grid,
                    QString const &source) {
    if (!m_db.isOpen() || call.isEmpty() || grid.isEmpty())
        return;
    qint64 const now = nowSecs();
    QSqlQuery q{m_db};
    q.prepare(QStringLiteral(
        "INSERT INTO grids (call, grid, source, first_seen,"
        " grid_changed, change_count)"
        " VALUES (?, ?, ?, ?, ?, 1)"
        " ON CONFLICT(call) DO UPDATE SET"
        " grid = excluded.grid,"
        " source = excluded.source,"
        " grid_changed = excluded.grid_changed,"
        " change_count = change_count + 1"));
    q.addBindValue(call.toUpper());
    q.addBindValue(grid);
    q.addBindValue(source);
    q.addBindValue(now);
    q.addBindValue(now);
    // [#170(k)] ACTIVITY IS NOT THIS FUNCTION'S JOB. upsert() used to
    // write last_heard/heard_count itself, bypassing the per-call
    // throttle in noteActivity() -- the very separation the #168 split
    // exists to create. Route it through the owner instead; the clock
    // written is identical, so no behaviour changes except that the
    // throttle now actually applies.
    if (!q.exec())
        qCWarning(griddb_js8)
            << "[GRIDDB] upsert FAILED:" << call
            << q.lastError().text();
    else
        qCDebug(griddb_js8) << "[GRIDDB] upsert" << call << grid
                            << source;
}

void GridDb::noteActivity(QString const &call, int snr) {
    if (!m_db.isOpen() || call.isEmpty())
        return;
    QString const c = call.toUpper();
    qint64 const now = nowSecs();
    auto const it = m_lastActivityWrite.constFind(c);
    if (it != m_lastActivityWrite.constEnd() &&
        now - it.value() < kActivityThrottleSecs)
        return;
    // [perf 2026-08-21] Bound the throttle map. One entry per callsign
    // ever seen is small, but it grows without limit across a long
    // session; entries older than the throttle window can never
    // suppress anything again.
    if (m_lastActivityWrite.size() > 4096) {
        for (auto it = m_lastActivityWrite.begin();
             it != m_lastActivityWrite.end();) {
            if (now - it.value() > kActivityThrottleSecs * 4)
                it = m_lastActivityWrite.erase(it);
            else
                ++it;
        }
    }
    m_lastActivityWrite.insert(c, now);
    QSqlQuery q{m_db};
    // A station may be active long before we know its grid, so this
    // must be able to create the row; grid stays empty until a
    // sanctioned source supplies one.
    //
    // [audit 2026-08-21] snr_last IS FIRST-HAND ONLY. Callers were
    // passing whatever SNR was to hand -- which at both sites is a
    // report of MY signal at THEIR receiver, the opposite direction
    // from "how well do we hear this station". Direction conflation
    // is the single most repeated defect in this subsystem, so the
    // column now takes -99 unless a caller has genuinely measured the
    // station itself.
    q.prepare(QStringLiteral(
        "INSERT INTO grids (call, grid, first_seen, last_heard,"
        " heard_count, snr_last, change_count)"
        " VALUES (?, '', ?, ?, 1, ?, 0)"
        " ON CONFLICT(call) DO UPDATE SET"
        " last_heard = excluded.last_heard,"
        " heard_count = heard_count + 1,"
        " snr_last = excluded.snr_last"));
    q.addBindValue(c);
    q.addBindValue(now);
    q.addBindValue(now);
    q.addBindValue(snr);
    if (!q.exec())
        qCDebug(griddb_js8) << "[GRIDDB] activity FAILED:" << c
                            << q.lastError().text();
}

void GridDb::queueEdge(EdgeRow const &e) {
    if (e.hearer.isEmpty() || e.heard.isEmpty())
        return;
    EdgeRow row = e;
    row.hearer = row.hearer.toUpper();
    row.heard = row.heard.toUpper();
    // GRIDS UPPERCASE TOO. Callsigns were normalised here from the
    // start; grids were not, so 5194 of them sat in the edges table as
    // "JN68rn" while the grid authority held "JN68RN" -- the same fact
    // in two cases, in two tables. Nothing compares them for equality
    // today, which is exactly why it survived: it is a trap that costs
    // nothing until something does. The identical shape bit us twice
    // in one session, with "WM8Q>" failing a callsign compare and with
    // the renderer drawing from the un-normalised copy.
    //
    // Maidenhead subsquares are conventionally lowercase, so this is a
    // storage convention, not a display one -- present grids however
    // you like, but store one form.
    row.hearerGrid = row.hearerGrid.toUpper();
    row.heardGrid = row.heardGrid.toUpper();
    if (row.hearer == row.heard)
        return;
    if (!row.when)
        row.when = nowSecs();
    // [perf #170(j)] BOUND THE QUEUE. Edges arrive at PSKR rate with
    // nothing between the 45 s flushes to stop them piling up -- at
    // greyline that is thousands. Flushing early when the queue gets
    // large keeps the memory bounded and the transaction a sane size,
    // and it cannot lose data: flush() is the same call the timer
    // makes.
    constexpr int kMaxPendingEdges = 2000;
    m_pendingEdges.append(row);
    if (m_pendingEdges.size() >= kMaxPendingEdges)
        flush();
}

void GridDb::queueStation(StationRow const &s) {
    if (s.call.isEmpty() || s.band.isEmpty())
        return;
    StationRow row = s;
    row.call = row.call.toUpper();
    if (!row.anyWhen)
        row.anyWhen = nowSecs();
    m_pendingStations.append(row);
}

void GridDb::queueReachEvent(ReachEventRow const &r) {
    if (r.station.isEmpty() || r.band.isEmpty() || r.kind.isEmpty())
        return;
    ReachEventRow row = r;
    row.station = row.station.toUpper();
    if (!row.when)
        row.when = nowSecs();
    m_pendingReach.append(row);
}

QVector<GridDb::ReachEventRow> GridDb::loadReachEvents() const {
    QVector<ReachEventRow> out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q{m_db};
    q.prepare(QStringLiteral(
        "SELECT band, station, when_s, kind, ok, extra FROM"
        " reach_events WHERE when_s > ?"));
    q.addBindValue(nowSecs() - REACH_RETAIN_SECS);
    if (!q.exec()) {
        qCWarning(griddb_js8)
            << "[GRIDDB] loadReachEvents FAILED:"
            << q.lastError().text();
        return out;
    }
    while (q.next()) {
        ReachEventRow r;
        r.band = q.value(0).toString();
        r.station = q.value(1).toString();
        r.when = q.value(2).toLongLong();
        r.kind = q.value(3).toString();
        r.ok = q.value(4).toBool();
        r.extra = q.value(5).toString();
        out.append(r);
    }
    return out;
}

void GridDb::flush() {
    if (!m_db.isOpen())
        return;
    qint64 const now = nowSecs();
    bool const prune = now - m_lastPrune >= kPruneEverySecs;
    if (m_pendingEdges.isEmpty() && m_pendingStations.isEmpty() &&
        m_pendingReach.isEmpty() && !prune)
        return;

    int const edges = m_pendingEdges.size();
    int const spots = m_pendingStations.size();
    // ONE transaction for the whole batch — the entire performance
    // story. Per-row transactions would fsync per row.
    bool const inTxn = m_db.transaction();
    if (!inTxn)
        qCWarning(griddb_js8)
            << "[GRIDDB] transaction FAILED - writing unbatched:"
            << m_db.lastError().text();

    if (!m_pendingReach.isEmpty()) {
        QSqlQuery q{m_db};
        q.prepare(QStringLiteral(
            "INSERT INTO reach_events (band, station, when_s, kind,"
            " ok, extra) VALUES (?, ?, ?, ?, ?, ?)"));
        for (ReachEventRow const &r : m_pendingReach) {
            q.bindValue(0, r.band);
            q.bindValue(1, r.station);
            q.bindValue(2, r.when);
            q.bindValue(3, r.kind);
            q.bindValue(4, r.ok ? 1 : 0);
            q.bindValue(5, r.extra);
            if (!q.exec())
                qCWarning(griddb_js8)
                    << "[GRIDDB] reach_events insert FAILED:"
                    << q.lastError().text();
        }
        m_pendingReach.clear();
    }
    if (!m_pendingEdges.isEmpty()) {
        QSqlQuery q{m_db};
        q.prepare(QStringLiteral(
            "INSERT INTO edges (band, hearer, heard, when_s, snr,"
            " hearer_grid, heard_grid, source)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?)"
            " ON CONFLICT(band, hearer, heard) DO UPDATE SET"
            // Freshest wins; never let a stale replay move an edge
            // backwards in time.
            " when_s = MAX(when_s, excluded.when_s),"
            " snr = CASE WHEN excluded.when_s >= when_s"
            "            THEN excluded.snr ELSE snr END,"
            " hearer_grid = excluded.hearer_grid,"
            " heard_grid = excluded.heard_grid,"
            " source = excluded.source"));
        for (EdgeRow const &e : m_pendingEdges) {
            q.bindValue(0, e.band);
            q.bindValue(1, e.hearer);
            q.bindValue(2, e.heard);
            q.bindValue(3, e.when);
            q.bindValue(4, e.snr);
            q.bindValue(5, e.hearerGrid);
            q.bindValue(6, e.heardGrid);
            q.bindValue(7, e.source);
            if (!q.exec())
                qCDebug(griddb_js8) << "[GRIDDB] edge FAILED:"
                                    << e.hearer << e.heard
                                    << q.lastError().text();
        }
        m_pendingEdges.clear();
    }

    if (!m_pendingStations.isEmpty()) {
        QSqlQuery q{m_db};
        q.prepare(QStringLiteral(
            "INSERT INTO stations (band, call, grid, country, freq_hz,"
            " any_when, radio_when, snr_to_me, snr_when, reports_me,"
            " rx_only, freq_when, freq_radio)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
            " ON CONFLICT(band, call) DO UPDATE SET"
            // [freqclock] the clock and source travel WITH the value:
            // they change exactly when freq_hz does, never alone.
            " freq_when = CASE WHEN excluded.freq_hz > 0"
            "                  THEN excluded.freq_when ELSE freq_when END,"
            " freq_radio = CASE WHEN excluded.freq_hz > 0"
            "                   THEN excluded.freq_radio ELSE freq_radio END,"
            // Freshest wins; a stale replay never moves a clock back.
            " any_when = MAX(any_when, excluded.any_when),"
            " radio_when = MAX(radio_when, excluded.radio_when),"
            // Facts only improve: never blank a known grid/country/freq
            // with an observation that happens not to carry one.
            " grid = CASE WHEN excluded.grid <> '' THEN excluded.grid"
            "             ELSE grid END,"
            " country = CASE WHEN excluded.country <> ''"
            "                THEN excluded.country ELSE country END,"
            " freq_hz = CASE WHEN excluded.freq_hz > 0"
            "                THEN excluded.freq_hz ELSE freq_hz END,"
            " snr_to_me = CASE WHEN excluded.snr_to_me > -99"
            "                  THEN excluded.snr_to_me ELSE snr_to_me END,"
            " snr_when = CASE WHEN excluded.snr_to_me > -99"
            "                 THEN excluded.snr_when ELSE snr_when END,"
            " reports_me = MAX(reports_me, excluded.reports_me),"
            // rx_only is STICKY-FALSE: once seen transmitting, never
            // demoted back to receive-only.
            " rx_only = MIN(rx_only, excluded.rx_only)"));
        for (StationRow const &r : m_pendingStations) {
            q.bindValue(0, r.band);
            q.bindValue(1, r.call);
            q.bindValue(2, r.grid);
            q.bindValue(3, r.country);
            q.bindValue(4, r.freqHz);
            q.bindValue(5, r.anyWhen);
            q.bindValue(6, r.radioWhen);
            q.bindValue(7, r.snrToMe);
            q.bindValue(8, r.snrToMeWhen);
            q.bindValue(9, r.reportsMe ? 1 : 0);
            q.bindValue(10, r.rxOnly ? 1 : 0);
            q.bindValue(11, r.freqWhen);              // [freqclock]
            q.bindValue(12, r.freqRadio ? 1 : 0);     // [freqclock]
            if (!q.exec())
                qCDebug(griddb_js8) << "[GRIDDB] station FAILED:"
                                    << r.call << q.lastError().text();
        }
        m_pendingStations.clear();
    }

    if (prune) {
        {
            QSqlQuery pr{m_db};
            pr.prepare(QStringLiteral(
                "DELETE FROM reach_events WHERE when_s < ?"));
            pr.addBindValue(nowSecs() - REACH_RETAIN_SECS);
            pr.exec();
        }
        m_lastPrune = now;
        QSqlQuery q{m_db};
        qint64 const cutoff = now - RETAIN_SECS;
        q.prepare(QStringLiteral("DELETE FROM edges WHERE when_s < ?"));
        q.addBindValue(cutoff);
        q.exec();
        q.prepare(QStringLiteral(
            "DELETE FROM stations WHERE any_when < ?"));
        q.addBindValue(cutoff);
        q.exec();
    }

    // [audit] Only commit what we actually began; committing without
    // a transaction logs a spurious failure every flush.
    if (inTxn && !m_db.commit())
        qCWarning(griddb_js8)
            << "[GRIDDB] commit FAILED:" << m_db.lastError().text();
    else if (edges || spots)
        qCDebug(griddb_js8) << "[GRIDDB] flushed" << edges << "edges"
                            << spots << "stations";
}

QVector<GridDb::EdgeRow>
GridDb::loadEdges(qint64 notOlderThanSecs) const {
    QVector<EdgeRow> out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q{m_db};
    q.prepare(QStringLiteral(
                  "SELECT band, hearer, heard, when_s, snr,"
                  " hearer_grid, heard_grid, source FROM edges"
                  " WHERE when_s >= ?%1 ORDER BY when_s")
                  .arg(noInternetSim()
                           ? QStringLiteral(
                                 " AND COALESCE(source,'') <> 'mqtt'")
                           : QString{}));
    q.addBindValue(nowSecs() - notOlderThanSecs);
    if (!q.exec()) {
        qCWarning(griddb_js8)
            << "[GRIDDB] loadEdges FAILED:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        EdgeRow e;
        e.band = q.value(0).toString();
        e.hearer = q.value(1).toString();
        e.heard = q.value(2).toString();
        e.when = q.value(3).toLongLong();
        e.snr = q.value(4).toInt();
        e.hearerGrid = q.value(5).toString();
        e.heardGrid = q.value(6).toString();
        e.source = q.value(7).toString();
        out.append(e);
    }
    qCWarning(griddb_js8) << "[GRIDDB] restored" << out.size()
                          << "mesh edges from disk";
    return out;
}

QVector<GridDb::StationRow>
GridDb::loadStations(qint64 notOlderThanSecs) const {
    QVector<StationRow> out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q{m_db};
    q.prepare(QStringLiteral(
                  "SELECT band, call, grid, country, freq_hz,"
                  " any_when, radio_when, snr_to_me, snr_when,"
                  " reports_me, rx_only, freq_when, freq_radio"
                  " FROM stations"
                  " WHERE any_when >= ?%1 ORDER BY any_when")
                  .arg(noInternetSim()
                           ? QStringLiteral(" AND radio_when > 0")
                           : QString{}));
    q.addBindValue(nowSecs() - notOlderThanSecs);
    if (!q.exec()) {
        qCWarning(griddb_js8)
            << "[GRIDDB] loadStations FAILED:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        StationRow r;
        r.band = q.value(0).toString();
        r.call = q.value(1).toString();
        r.grid = q.value(2).toString();
        r.country = q.value(3).toString();
        r.freqHz = q.value(4).toLongLong();
        r.anyWhen = q.value(5).toLongLong();
        r.radioWhen = q.value(6).toLongLong();
        r.snrToMe = q.value(7).toInt();
        r.snrToMeWhen = q.value(8).toLongLong();
        r.reportsMe = q.value(9).toInt() != 0;
        r.rxOnly = q.value(10).toInt() != 0;
        r.freqWhen = q.value(11).toLongLong();        // [freqclock]
        r.freqRadio = q.value(12).toInt() != 0;       // [freqclock]
        out.append(r);
    }
    qCWarning(griddb_js8) << "[GRIDDB] restored" << out.size()
                          << "stations from disk";
    return out;
}
