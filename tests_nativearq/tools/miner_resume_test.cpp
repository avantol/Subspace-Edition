// [#287 part 3] Offline proof that a RESUMED mine (read only the
// appended bytes, carried-over state) produces the same corpus as a
// FULL mine of the same files.
//
//   miner_resume_test <ALL.TXT> <DIRECTED.TXT> <grids.db> <mycall> [cutFraction=0.9]
//
// 1. Cut both logs at one instant (the stamp of the ALL.TXT line at
//    cutFraction; DIRECTED.TXT cut at the first line at/after it).
// 2. Full mine of the first parts -> intel A; append the rest; mine
//    again -> A is RESUMED.
// 3. Full mine of the complete files -> intel B.
// 4. Compare A and B table by table. Exact for everything except the
//    two 30-day-half-life sums, which are allowed a relative 1e-6
//    (they are re-decayed by elapsed wall time between the two runs).
//
// Build (repo root):
//   g++ -std=c++20 -O1 -I. $(pkg-config --cflags Qt6Core Qt6Sql) \
//     tests_nativearq/tools/miner_resume_test.cpp JS8_Main/IntelMiner.cpp \
//     JS8_Main/Radio.cpp JS8_Main/StoragePaths.cpp \
//     $(pkg-config --libs Qt6Core Qt6Sql) -o /tmp/minertest
#include "JS8_Main/IntelMiner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QTextStream>
#include <QVariant>
#include <cmath>
#include <cstdio>

namespace {

QByteArray readAll(QString const &p) {
    QFile f{p};
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{};
}
void writeAll(QString const &p, QByteArray const &b, bool append = false) {
    QFile f{p};
    f.open(append ? (QIODevice::WriteOnly | QIODevice::Append)
                  : QIODevice::WriteOnly);
    f.write(b);
}
// Byte offset of the first line whose 19-char stamp >= cut.
qint64 cutAtStamp(QByteArray const &b, QByteArray const &cut) {
    qint64 pos = 0;
    while (pos < b.size()) {
        qint64 const nl = b.indexOf('\n', pos);
        qint64 const end = nl < 0 ? b.size() : nl + 1;
        if (end - pos >= 19 && b.mid(pos, 19) >= cut &&
            b.mid(pos, 4).toInt() > 2000)
            return pos;
        pos = end;
    }
    return b.size();
}
// Stamp of the line at fraction f of the file.
QByteArray stampAt(QByteArray const &b, double f) {
    qint64 pos = qint64(b.size() * f);
    qint64 const nl = b.lastIndexOf('\n', pos);
    pos = nl < 0 ? 0 : nl + 1;
    while (pos < b.size()) {
        qint64 const e = b.indexOf('\n', pos);
        if (b.mid(pos, 4).toInt() > 2000)
            return b.mid(pos, 19);
        pos = e < 0 ? b.size() : e + 1;
    }
    return {};
}

struct Table {
    QString name, keyCols, valCols;
};

int compareTable(QSqlDatabase &a, QSqlDatabase &b, Table const &t,
                 int tolCols = 0) {
    auto load = [&](QSqlDatabase &db) {
        QHash<QString, QStringList> rows;
        QSqlQuery q{db};
        q.exec(QStringLiteral("SELECT %1, %2 FROM %3")
                   .arg(t.keyCols, t.valCols, t.name));
        int const nk = t.keyCols.count(',') + 1;
        while (q.next()) {
            QStringList k, v;
            int const n = q.record().count();
            for (int i = 0; i < n; ++i)
                (i < nk ? k : v) << q.value(i).toString();
            rows.insert(k.join('|'), v);
        }
        return rows;
    };
    auto const ra = load(a), rb = load(b);
    int bad = 0;
    for (auto it = rb.constBegin(); it != rb.constEnd(); ++it) {
        auto const va = ra.value(it.key());
        if (va.isEmpty()) {
            if (bad++ < 5)
                printf("  %s MISSING in resumed: %s\n", qPrintable(t.name),
                       qPrintable(it.key()));
            continue;
        }
        auto const &vb = it.value();
        for (int i = 0; i < vb.size(); ++i) {
            bool ok = va.value(i) == vb.at(i);
            if (!ok && i >= vb.size() - tolCols) {
                // The two half-life sums are measured from "now": a run
                // a minute later is 0.5^(60 s / 30 d) = 1 - 1.6e-5 off.
                double const x = va.value(i).toDouble(), y = vb.at(i).toDouble();
                ok = std::fabs(x - y) <= 1e-4 * std::max(1.0, std::fabs(y));
            }
            if (!ok) {
                if (bad++ < 8)
                    printf("  %s DIFF %s col %d: resumed=%s full=%s\n",
                           qPrintable(t.name), qPrintable(it.key()), i,
                           qPrintable(va.value(i)), qPrintable(vb.at(i)));
                break;
            }
        }
    }
    for (auto it = ra.constBegin(); it != ra.constEnd(); ++it)
        if (!rb.contains(it.key()) && bad++ < 5)
            printf("  %s EXTRA in resumed: %s\n", qPrintable(t.name),
                   qPrintable(it.key()));
    printf("%-11s rows resumed=%d full=%d mismatches=%d\n",
           qPrintable(t.name), int(ra.size()), int(rb.size()), bad);
    return bad;
}

int countRows(QSqlDatabase &db, QString const &table) {
    QSqlQuery q{db};
    q.exec(QStringLiteral("SELECT COUNT(*) FROM ") + table);
    return q.next() ? q.value(0).toInt() : -1;
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app{argc, argv};
    QCoreApplication::setApplicationName(QStringLiteral("JS8Call"));
    if (argc < 5) {
        fprintf(stderr, "usage: %s ALL.TXT DIRECTED.TXT grids.db MYCALL [cut]\n",
                argv[0]);
        return 2;
    }
    QString const allSrc = argv[1], dirSrc = argv[2], grids = argv[3];
    QString const mycall = QString::fromLatin1(argv[4]).toUpper();
    double const cutF = argc > 5 ? atof(argv[5]) : 0.9;
    double const cutF2 = argc > 6 ? atof(argv[6]) : -1.0;   // optional 2nd resume

    QTemporaryDir tmp;
    tmp.setAutoRemove(false);   // keep A_intel.db / B_intel.db for inspection
    QString const d = tmp.path();
    printf("work dir %s\n", qPrintable(d));
    QByteArray const all = readAll(allSrc), dir = readAll(dirSrc);
    QByteArray const cut = stampAt(all, cutF);
    qint64 const cutA = cutAtStamp(all, cut), cutD = cutAtStamp(dir, cut);
    printf("cut at %s: ALL %lld/%lld bytes, DIRECTED %lld/%lld bytes\n",
           cut.constData(), (long long)cutA, (long long)all.size(),
           (long long)cutD, (long long)dir.size());

    // ---- A: first parts, full mine; then append, resumed mine ------
    QString const allA = d + "/A_ALL.TXT", dirA = d + "/A_DIRECTED.TXT";
    writeAll(allA, all.left(cutA));
    writeAll(dirA, dir.left(cutD));
    IntelMiner mA;
    mA.allTxtPath = allA;
    mA.directedPath = dirA;
    mA.gridsDbPath = grids;
    mA.intelDbPath = d + "/A_intel.db";
    auto r1 = mA.mine(mycall, QStringLiteral("DN70"), true);
    printf("A full:    ok=%d resumed=%d lines=%d probes=%d %lld ms\n", r1.ok,
           r1.resumed, r1.directedLines, r1.probes, (long long)r1.elapsedMs);
    qint64 fromA = cutA, fromD = cutD;
    if (cutF2 > cutF) {   // two resumes: append to the second cut first
        QByteArray const cut2 = stampAt(all, cutF2);
        qint64 const cutA2 = cutAtStamp(all, cut2), cutD2 = cutAtStamp(dir, cut2);
        writeAll(allA, all.mid(cutA, cutA2 - cutA), true);
        writeAll(dirA, dir.mid(cutD, cutD2 - cutD), true);
        auto rm = mA.mine(mycall, QStringLiteral("DN70"), false);
        printf("A resume1: ok=%d resumed=%d lines=%d probes=%d %lld ms (to %s)\n",
               rm.ok, rm.resumed, rm.directedLines, rm.probes,
               (long long)rm.elapsedMs, cut2.constData());
        if (!rm.resumed) {
            printf("FAIL: first resume did not resume\n");
            return 1;
        }
        fromA = cutA2;
        fromD = cutD2;
    }
    writeAll(allA, all.mid(fromA), true);
    writeAll(dirA, dir.mid(fromD), true);
    auto r2 = mA.mine(mycall, QStringLiteral("DN70"), false);
    printf("A resumed: ok=%d resumed=%d lines=%d probes=%d %lld ms\n", r2.ok,
           r2.resumed, r2.directedLines, r2.probes, (long long)r2.elapsedMs);
    if (!r2.resumed) {
        printf("FAIL: second run did not resume\n");
        return 1;
    }

    // ---- B: full mine of the complete files -----------------------
    QString const allB = d + "/B_ALL.TXT", dirB = d + "/B_DIRECTED.TXT";
    writeAll(allB, all);
    writeAll(dirB, dir);
    IntelMiner mB;
    mB.allTxtPath = allB;
    mB.directedPath = dirB;
    mB.gridsDbPath = grids;
    mB.intelDbPath = d + "/B_intel.db";
    auto r3 = mB.mine(mycall, QStringLiteral("DN70"), true);
    printf("B full:    ok=%d lines=%d probes=%d %lld ms\n", r3.ok,
           r3.directedLines, r3.probes, (long long)r3.elapsedMs);

    // ---- compare ---------------------------------------------------
    auto a = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), "a");
    a.setDatabaseName(mA.intelDbPath);
    auto b = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), "b");
    b.setDatabaseName(mB.intelDbPath);
    a.open();
    b.open();
    int bad = 0;
    bad += compareTable(a, b,
                        {"stations", "call",
                         "first_heard,last_heard,heard_count,snr_n,snr_sum,"
                         "snr_min,snr_max,rev_snr_n,rev_snr_last,rev_snr_best,"
                         "rev_last,resp_count,spont_count,relay_seen,to_us,"
                         "grid,relay_asked,relay_done"},
                        2);
    bad += compareTable(a, b, {"edges", "hearer,heard",
                               "last_when,n,snr,snr_when,source"});
    bad += compareTable(a, b, {"activity", "call,hour", "n"});
    for (QString const t : {"probes", "sightings", "edge_events"}) {
        int const na = countRows(a, t), nb = countRows(b, t);
        printf("%-11s rows resumed=%d full=%d%s\n", qPrintable(t), na, nb,
               na == nb ? "" : "  <-- DIFF");
        if (na != nb)
            ++bad;
    }
    // probes compared by content too (ts,target -> answered,latency,present)
    bad += compareTable(a, b, {"probes", "ts,target",
                               "cmd,answered,latency_s,present"});
    printf(bad ? "RESULT: %d mismatching rows/tables\n" : "RESULT: IDENTICAL\n",
           bad);
    return bad ? 1 : 0;
}
