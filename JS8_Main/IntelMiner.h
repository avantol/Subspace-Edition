/**
 * @file IntelMiner.h
 * @brief In-app port of tools/js8reach/mine.py + intel.py (TODO #187).
 *
 * Builds ~/.config/js8reach-intel.db from the user's own DIRECTED.TXT
 * + ALL.TXT + grid bank, exactly as the bench miner does -- same
 * schema, same evidence rules, same path -- so the reaching executor's
 * existing read side needs no changes and every user's operating
 * history becomes their routing intelligence with no tools installed.
 *
 * DESIGN (operator-approved 2026-08-28, TODO_notes.md#item-187):
 *  - Full re-mine per run. mine.py's flush() deletes and rebuilds
 *    every table, and the relay-ask weights decay relative to NOW, so
 *    incremental appends are wrong by construction; the python does
 *    the whole log in ~5 s and this runs on a worker thread.
 *  - Bookmark (meta: source sizes+mtimes) only SKIPS the mine when
 *    neither log changed since the last run.
 *  - Mines into a temp file, atomic-renames over the real one.
 *  - Also seeds the app's grid bank from ALL.TXT broadcast grids
 *    (heartbeat/CQ fields, the operator's reliable-kinds ruling) --
 *    INSERT OR IGNORE, source='log', corroboration >= 2 sightings.
 *
 * ACCEPTANCE: identical table counts vs mine.py over the same logs.
 */
#ifndef INTEL_MINER_H
#define INTEL_MINER_H

#include "GridDb.h"
#include <QObject>
#include <QString>
#include <QVector>

class IntelMiner : public QObject {
    Q_OBJECT
public:
    struct Result {
        bool skipped = false; // logs unchanged since last mine
        bool resumed = false; // [#287] read only the appended bytes
        bool ok = false;
        int directedLines = 0;
        int probes = 0;
        int stations = 0;
        int edges = 0;
        int sightings = 0;
        int events = 0;
        qint64 elapsedMs = 0;
        // Grid-bank seeding rows -- ONE struct, GridDb's (the
        // consumer defines the shape).
        QVector<GridDb::LogSeed> logGrids;
    };

    explicit IntelMiner(QObject *parent = nullptr);

    // Runs on the CALLING thread (blocks): callers wrap it in
    // QThread::create. force=true ignores the unchanged-logs skip
    // (the "Rebuild routing knowledge" menu action).
    Result mine(QString const &myCall, QString const &myGrid,
                bool force);

    // [#287] Idle I/O class + lowest CPU priority for the CALLING
    // thread; the worker calls it before mine().
    static void lowerThreadToBackground();

    // Paths overridable for the bench acceptance run.
    QString directedPath;
    QString allTxtPath;
    QString gridsDbPath;
    QString intelDbPath;
};

#endif // INTEL_MINER_H
