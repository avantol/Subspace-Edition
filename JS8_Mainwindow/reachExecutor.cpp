/** \file
 * @brief member functions of the UI_Constructor class
 *
 * [reachport2] THE REACHING EXECUTOR — MECHANICAL port of the frozen
 * python (tools/js8reach @ c6e2e7e3), written against the 80-item
 * claims-audit of 2026-08-27 (memory: reference_reachport_audit).
 * The first port was written from a mental model and silently dropped
 * 24 behaviors; this one cites its python twin at every function.
 *
 * ONE OWNER PER FACT: the hearing store + GridDb 24h tier + intel
 * corpus feed a per-attempt route book (snapshot, updated live by YES
 * answers); the assembler feeds the watchers; stopTx's completion
 * branch is the TX-end anchor.
 *
 * DECLARED DEFERRED (and only these): grid targets (#180),
 * multi-hop chains (best_routes Dijkstra; every live success was
 * 1-hop and replay showed hop count changed nothing).
 *
 * DELIBERATE IMPROVEMENTS over the python (kept from build 383):
 * dead-stays-dead watchers; attempt lines cleared at stop AND
 * verdict; [MESSAGE] placeholder selection; REACHED requires the
 * answer addressed to us (via same_station, not prefix).
 */

#include "JS8_UI/mainwindow.h"
#include "JS8_UI/SpotMapWindow.h"   // also provides Geodesic.h
#include "JS8_Main/SelfDestructMessageBox.h" // [modegate]
                                    // (which has no include guard)
#include "JS8_Main/DriftingDateTime.h"
#include "JS8_Mode/JS8Submode.h"

#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTimer>
#include <QMessageBox>
#include <cmath>
#include <limits>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

// ---- measured constants, python twins cited --------------------------

// decide.py:89-93 (actions.rtt at Normal)
constexpr double T_SNR = 68.0, T_GRID = 82.0, T_SHOUT = 98.0,
                 T_HEARING = 112.0;
// actions.rtt(2,2,h): cost per chain length (decide.py:93)
constexpr double kTRelay[5] = {97.5, 217.5, 337.5, 457.5, 577.5};
constexpr double T_RELAY1 = 217.5;
// decide.py:162-169
constexpr int kFramesSnr = 1, kFramesGrid = 1, kFramesShout = 4,
              kFramesHearing = 4;
// attempt.py slot_end margin (reference_slot_timing)
constexpr qint64 kEnqueueMarginMs = 700;
// sim.py:53 / livemodel priors
constexpr double kUnseenLink = 0.12;
constexpr double kFwdPrior = 0.43;   // livemodel.py:329
constexpr double kAnsPrior = 0.45;   // livemodel.py:315 formula prior
// decide.py:333
constexpr double FRESH_LINK = 0.28;
// decide.py:628 — session length for the _stale model
constexpr double SESSION_S = 2700.0;
// attempt.py:320-321 — the per-move hard cap
constexpr qint64 kMoveCapMs = (90 + 16 * 15) * 1000;
// livemodel.py:355 geo pool radius; :354 pool limit
constexpr float kGeoKm = 1200.0f;
constexpr int kPoolLimit = 40;
// sim.py:85-88 — measured reciprocity by out-degree ratio
struct RecipRow { double edge, p; };
constexpr RecipRow kReciprocity[] = {
    {0.1, 0.055}, {0.5, 0.199}, {2.0, 0.311}, {10.0, 0.362},
    {1e18, 0.423}};

QString fmtClock(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, Qt::UTC)
        .toString(QStringLiteral("HH:mm:ss.zzz"));
}

// callsign.py:88 — amateur-call shape, at least one letter in prefix
// Promoted to Radio:: (one authority; the Spots Map needs the same
// screen for auto-route target entry). These wrappers keep the
// python-port names local call sites use.
bool isAmateurCall(QString const &call) {
    return Radio::is_amateur_callsign(call);
}
// callsign.py:105 — hyphen suffix marks a receive-only node
bool isReceiveOnly(QString const &call) {
    return call.contains(QLatin1Char('-'));
}
// callsign.py:116 is_routable
bool isRoutable(QString const &call) {
    return Radio::is_routable_callsign(call);
}
// gridtarget.py:44 GRID_RE
bool isGridSquare(QString const &s) {
    // 4, 6, or 8 characters -- MUST accept everything the map
    // panel's Maidenhead::valid enables Start for (operator
    // 2026-08-30: an 8-char grid passed the panel and was then
    // rejected here with the generic failure dialog).
    static QRegularExpression const re(
        QStringLiteral("^[A-R]{2}[0-9]{2}([A-X]{2}([0-9]{2})?)?$"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(s.trimmed()).hasMatch();
}

// ---- the per-attempt route book --------------------------------------
// Snapshot at reachStart from THREE sources, freshest edge wins:
// RAM hearing store (this hour), GridDb persistent tier (24 h — the
// python's GRAPH_SECS horizon, livemodel.py:55), intel corpus
// (~/.config/js8reach-intel.db, livemodel.py:150-157). Live YES
// answers update it during the attempt (told_us — decide.py:565-571).
struct BookEdge {
    qint64 whenMs = 0;
    int snr = -99;
    QString source;
};
struct BookStation {
    QString grid;
    qint64 lastSeenMs = 0;
    int snrToMe = -99;
    bool hearsMe = false;
    bool txAlive = false;
    QString source;
    double ansHabit = -1.0;   // -1 = not looked up yet
    double fwdHabit = -1.0;
};
struct LearnedEdge {
    QString hearer, heard, source;
    qint64 whenMs;
    int snr;
};
struct Book {
    QHash<QString, QHash<QString, BookEdge>> edges;  // hearer -> heard
    QHash<QString, BookStation> stations;
    QSet<QString> transmitters;   // seen keying (phantom-edge filter)
    QStringList pool;             // ordered, limited
    QStringList walk;             // pool + hears-us extras (Dijkstra)
    QSet<QString> firstHops;      // eligible chain anchors
    QVector<LearnedEdge> learned; // live YES overlays, re-applied on
                                  // every refresh
    QString intelPath;
    bool intelOpen = false;
};
Book g_book;

// [gatetrust 2026-08-27] BAND FORWARDING TRUST -- Andy: "teach the
// gate that repeated booked-relay silence devalues the book" and it
// must NOT require a restart. This is not a cached negative about any
// station (that invariant stands): it is a measured SESSION RATE --
// booked-relay asks vs observed forwards on this band, each event
// recency-weighted (half-life SESSION_S), shaped like the p_fwd habit
// formula with the 0.43 prior counting as two asks. Every relay-silent
// verdict drags it down, every forward restores it, time heals it.
// Applied as a multiplier on every relay's forward factor, so ranking,
// expected time, and the shout gate all inherit the skepticism at the
// NEXT move -- mid-attempt, and across retries in the session.
struct RelayOutcome { qint64 ms; bool forwarded; QString station; };
QHash<QString, QVector<RelayOutcome>> g_relayOutcomes;  // per band
// [habitstore] answered-when-called observations, per band
struct AnsOutcome { qint64 ms; bool answered; QString station; };
QHash<QString, QVector<AnsOutcome>> g_ansOutcomes;
// [relayprior 2026-08-30] The map's ring criteria, consumed by the
// ranking (operator: announced relay-on should outrank unknown;
// "demote 'relay disabled?' stations -- just now, it tried one").
// Refreshed per move in reachRefreshBook; the MAP stays the one
// authority for both criteria.
// g_relayGreen = the FULL green-ring criterion (announced within
// 24h OR proven forwarder in the journal) -- operator 2026-08-30:
// "the point was to use that green-ring status". Boost consuming
// only the announced half was a half-port; map and ranking must
// read the same union.
QSet<QString> g_relayGreen;
QHash<QString, int> g_relayFails24;
bool g_eventsLoaded = false;

double bandTrust(QString const &band, qint64 nowMs) {
    double n = 0.0, a = 0.0;
    for (auto const &o : g_relayOutcomes.value(band)) {
        double const w = std::pow(
            0.5, (nowMs - o.ms) / (SESSION_S * 1000.0));
        n += w;
        a += w * (o.forwarded ? 1.0 : 0.0);
    }
    return qMin(0.95, qMax(0.05,
        (kFwdPrior * 2.0 + a) / (2.0 + n)));
}

void bookAddEdge(QString const &hearer, QString const &heard,
                 qint64 whenMs, int snr, QString const &source) {
    auto &e = g_book.edges[hearer][heard];
    if (whenMs > e.whenMs) {       // freshest wins (livemodel.py:156)
        e.whenMs = whenMs;
        e.snr = snr;
        e.source = source;
    } else if (whenMs == e.whenMs && snr > e.snr) {
        e.snr = snr;
    }
}

BookEdge bookEdge(QString const &hearer, QString const &heard) {
    return g_book.edges.value(hearer).value(heard);
}

int bookOutDegree(QString const &call) {          // livemodel.py:194
    return g_book.edges.value(call).size();
}

// sim.py:90-102 — order-sensitive: sender then receiver
double reciprocityFor(int degSender, int degReceiver) {
    double const a = qMax(degSender, 1);
    double const b = qMax(degReceiver, 1);
    double const ratio = b / a;
    for (auto const &r : kReciprocity)
        if (ratio <= r.edge)
            return r.p;
    return kReciprocity[4].p;
}

double pLinkRaw(qint64 whenMs, int snr, qint64 nowMs);

} // namespace

void UI_Constructor::reachLog(QString const &line) {
    qCWarning(mainwindow_js8).noquote()
        << QStringLiteral("[REACH]")
        << fmtClock(DriftingDateTime::currentMSecsSinceEpoch())
        << line;
}

// [exactreply 2026-08-27, operator directive: "be sure to use the
// correct expected reply message when calculating the ultimate wait
// time"] Frame counts come from the PACKER fed the composed expected
// reply -- one owner for "how many frames does this message take",
// per query kind, checksum and callsign packing included. SNR rides
// as a fixed-width 3-char literal, so any value gives the same count.
int UI_Constructor::reachReplyFrames(QString const &from,
                                     QString const &text) const {
    Varicode::MessageInfo info;
    auto const frames = Varicode::buildMessageFrames(
        from, QString{}, QString{}, text,
        /*forceIdentify=*/false, /*forceData=*/false, m_nSubMode,
        &info);
    return qMax(1, int(frames.size()));
}

// [#207 waitopts 2026-09-01] The busy-band predicate for the TWO
// busy-conditional points (second start slot; return slide).
// Short = never the extra wait, Long = always, Adaptive = the
// selected congestion metric at/over ITS operator threshold
// (defaults 7, persisted; right-click either Adaptive row in the
// map Options dialog). Adaptive (PSKR) falls back to the on-air
// metric whenever MQTT data is absent from the 5-minute window,
// and resumes by itself when spots flow again.
bool UI_Constructor::reachBandBusy() const {
    // [operator 2026-09-01] Log the EFFECTIVE inputs at every
    // decision point, so each armed wait is attributable to the
    // metric, value, and threshold that ruled it.
    bool busy;
    QString basis;
    if (m_reachWaitMode == 0) {
        busy = false;
        basis = QStringLiteral("mode short");
    } else if (m_reachWaitMode == 2) {
        busy = true;
        basis = QStringLiteral("mode long");
    } else if (m_reachWaitMode == 3 && m_spotMapWindow &&
               m_spotMapWindow->pskrDataAvailable()) {
        int const pi = m_spotMapWindow->pskrCongestionIndex();
        busy = pi >= m_reachBusyThresholdPskr;
        basis = QStringLiteral("pskr %1 thr %2")
                    .arg(pi)
                    .arg(m_reachBusyThresholdPskr);
    } else {
        int const ci = bandCongestionIndex();
        busy = ci >= m_reachBusyThreshold;
        basis = QStringLiteral("on-air %1 thr %2%3")
                    .arg(ci)
                    .arg(m_reachBusyThreshold)
                    .arg(m_reachWaitMode == 3
                             ? QStringLiteral(" (pskr fallback)")
                             : QString());
    }
    qCWarning(mainwindow_js8)
        << "[REACH] wait decision:" << basis << "->"
        << (busy ? "busy" : "quiet");
    return busy;
}

// [structured 2026-08-31] Expected frame counts for a relay move,
// all from the packer (one owner; compound calls native; SNR is a
// fixed-width literal so any value packs the same). Standard calls
// give fwd=3, ans=3, ret=3 -- the accounting doc's numbers.
void UI_Constructor::reachComputeExpected() {
    QString const me = m_config.my_callsign().trimmed().toUpper();
    QString const T = m_reach.target;
    // The relay's rewrite, exactly (field-verified 1-hop form
    // "AC7WY> SNR? + *DE* WM8Q"; code: remainder with the FIRST
    // token '>'-ified): 1-hop -> "T> SNR? *DE* ME"; 2-relay ->
    // "B>T SNR? *DE* ME" (intermediate keeps '>', final dest bare).
    QStringList rest = m_reach.chain.mid(1);
    rest << T;
    QString const fwdText =
        rest.join(QLatin1Char('>')) +
        (rest.size() == 1 ? QStringLiteral(">") : QString()) +
        QStringLiteral(" SNR? *DE* ") + me;
    m_reach.fwdExpFrames = reachReplyFrames(m_reach.via, fwdText);
    // The target's real answer to a relayed ask is the RELAY-FORM
    // reply back along the path (accounting doc slots 7-9:
    // "K1TAR: A1VIA> WM8Q SNR +00 .XX"), checksummed -- not a
    // simple directed answer. Field-caught on N3CHX/P1 2026-09-01:
    // the simple shape under-counted the silence by a slot.
    m_reach.ansExpFrames = reachReplyFrames(
        T, m_reach.chain.last() + QStringLiteral("> ") + me +
               QStringLiteral(" SNR -15"));
    m_reach.retExpFrames = reachReplyFrames(
        m_reach.via,
        me + QStringLiteral("> SNR -15 *DE* ") + T);
    // [retlen 2026-09-03] KI6WDY case: return computed 3 frames,
    // W4CAT aired 4 (frame 4 = checksum tail, stomped by our next
    // ask). Same packer + same-length text must agree, so log the
    // exact templates and counts to catch the diverging input on
    // the next live run.
    qCWarning(mainwindow_js8)
        << "[REACH] expected frames: fwd" << m_reach.fwdExpFrames
        << "ans" << m_reach.ansExpFrames << "ret"
        << m_reach.retExpFrames << "retTemplate"
        << (me + QStringLiteral("> SNR -15 *DE* ") + T);
}

// attempt.py:291-294
qint64 UI_Constructor::reachSlotEndMs(qint64 tMs, int n) const {
    qint64 const p = qMax(1u, JS8::Submode::periodMS(m_nSubMode));
    qint64 const b = ((tMs - 1000) / p + 1) * p;
    return b + qint64(n) * p - kEnqueueMarginMs;
}

// livemodel.py:175-192 p_link, VERBATIM including the -99 sentinel:
// an edge with no measured SNR gets margin 0.25, never 1.0 (audit
// item 26 — the first port had this inverted). A frame-1 YES pin
// (source "yes-frame1") returns FRESH_LINK flat (decide.py:333).
double UI_Constructor::reachPLink(qint64 whenMs, int snr) const {
    return pLinkRaw(whenMs, snr,
                    DriftingDateTime::currentMSecsSinceEpoch());
}

namespace {

double pLinkRaw(qint64 whenMs, int snr, qint64 nowMs) {
    if (whenMs <= 0)
        return kUnseenLink;
    double const ageH = qMax<qint64>(0, nowMs - whenMs) / 3600000.0;
    double const di = qMax(0.0, std::cos(2.0 * M_PI * ageH / 24.0));
    double const live = 0.120 + 0.100 * std::exp(-ageH / 5.0)
                      + 0.080 * std::exp(-ageH / 192.0) * di;
    double const margin =
        (snr > -99) ? qMin(1.0, qMax(0.25, (snr + 24.0) / 18.0))
                    : 0.25;
    return qMin(0.95, live * margin);
}

double pLinkEdge(UI_Constructor const *, QString const &hearer,
                 QString const &heard) {
    auto const e = bookEdge(hearer, heard);
    if (e.source == QLatin1String("yes-frame1"))
        return FRESH_LINK;
    return pLinkRaw(e.whenMs, e.snr,
                    DriftingDateTime::currentMSecsSinceEpoch());
}

// livemodel.py:248-266 p_copy: 0.95 plateau to 900 s, then decay
// with SESSION_S, floor 0.05; never heard at all -> 0.02.
double pCopy(QString const &call, qint64 nowMs) {
    auto const s = g_book.stations.value(call);
    if (s.lastSeenMs <= 0)
        return 0.02;
    double const age = (nowMs - s.lastSeenMs) / 1000.0;
    return age <= 900.0
               ? 0.95
               : qMax(0.05, 0.95 * std::exp(-(age - 900.0) / SESSION_S));
}

// livemodel.py:268-276 p_hears_us: fresh report of us (<= 1 h) is a
// flat 0.92; otherwise the standing edge through the decay curve.
double pHearsUs(UI_Constructor const *w, QString const &call,
                QString const &me, qint64 nowMs) {
    auto const s = g_book.stations.value(call);
    if (s.hearsMe) {
        auto const e = bookEdge(call, me);
        if (e.whenMs > 0 && nowMs - e.whenMs <= 3600000)
            return 0.92;
    }
    return pLinkEdge(w, call, me);
}

// livemodel.py:214-244 p_reverse: does b hear a, GIVEN a hears b.
double pReverse(QString const &a, QString const &b) {
    double const base = reciprocityFor(bookOutDegree(a),
                                       bookOutDegree(b));
    auto const e = bookEdge(a, b);
    if (e.whenMs > 0 && e.snr > -99) {
        double const margin =
            qMin(1.0, qMax(0.25, (e.snr + 24.0) / 18.0));
        return qMin(0.95, base * (0.45 + 0.85 * margin));
    }
    return base * 0.7;   // known link, unknown quality
}

// decide.py:491-545 delivers_to: chance DEST hears STATION. Forward
// edge where reported; reciprocity from the reverse where only that
// exists; the HIGHER wins. (Audit item 2: the first port used the
// reverse edge directly AS delivery — the exact defect this function
// was written to fix on 2026-08-24, reintroduced. Not this time.)
double deliversTo(UI_Constructor const *w, QString const &station,
                  QString const &dest) {
    double const fwd = pLinkEdge(w, dest, station);
    auto const revEdge = bookEdge(station, dest);
    double const rev =
        revEdge.whenMs > 0 ? pReverse(station, dest) : 0.0;
    return qMax(fwd, rev);
}

// livemodel.py:294-317 p_ans — recency-weighted habit from the intel
// probes table; weight halves every 30 days; prior counts as three
// fresh probes. livemodel.py:319-340 p_fwd — asked/done tiers, else
// seen-forwarding, else the 0.43 prior. Both keyed on BASE call
// (the corpus's own keying).
double intelAns(QString const &call, qint64 nowMs) {
    if (!g_book.intelOpen)
        return kAnsPrior;
    QSqlQuery q(QSqlDatabase::database(QStringLiteral("reach_intel")));
    q.prepare(QStringLiteral(
        "SELECT ts, answered FROM probes WHERE target=?"));
    q.addBindValue(Radio::base_callsign(call).toUpper());
    if (!q.exec())
        return kAnsPrior;
    double n = 0.0, a = 0.0;
    while (q.next()) {
        double const w = std::pow(
            0.5, (nowMs / 1000.0 - q.value(0).toDouble())
                     / (30.0 * 86400.0));
        n += w;
        a += w * q.value(1).toDouble();
    }
    return qMin(0.95, qMax(0.05, (kAnsPrior * 3.0 + a) / (3.0 + n)));
}

double intelFwd(QString const &call) {
    if (!g_book.intelOpen)
        return kFwdPrior;
    QSqlQuery q(QSqlDatabase::database(QStringLiteral("reach_intel")));
    q.prepare(QStringLiteral(
        "SELECT relay_asked, relay_done, relay_seen FROM stations "
        "WHERE call=?"));
    q.addBindValue(Radio::base_callsign(call).toUpper());
    if (!q.exec() || !q.next())
        return kFwdPrior;
    int const asked = q.value(0).toInt();
    int const done = q.value(1).toInt();
    int const seen = q.value(2).toInt();
    if (asked)
        return qMin(0.95, (kFwdPrior * 2.0 + done) / (2.0 + asked));
    if (seen)
        return qMin(0.85, 0.55 + 0.05 * std::log1p(double(seen)));
    return kFwdPrior;
}

double stAns(QString const &call, qint64 nowMs) {
    auto &s = g_book.stations[call];
    if (s.ansHabit < 0) {
        double const base = intelAns(call, nowMs);
        // live observations, same 30-day half-life as the corpus
        double n = 0.0, a = 0.0;
        for (auto it = g_ansOutcomes.constBegin();
             it != g_ansOutcomes.constEnd(); ++it)
            for (auto const &o : it.value()) {
                if (o.station != call)
                    continue;
                double const w = std::pow(
                    0.5, (nowMs - o.ms) / (30.0 * 86400.0 * 1000.0));
                n += w;
                a += w * (o.answered ? 1.0 : 0.0);
            }
        s.ansHabit = qMin(0.95, qMax(0.05,
            (base * 3.0 + a) / (3.0 + n)));
    }
    return s.ansHabit;
}
// [stationhabit 2026-08-27, operator: "when we see that NZ1ON won't
// answer, doesn't that de-weight it? same for K9IMM, we keep
// calling"] The corpus p_fwd formula fed the SESSION's own events
// live: each ask and each observed forward records per station, and
// the habit blends corpus base with recency-weighted session
// outcomes -- (base*2 + done)/(2 + asked). NOT a cached negative
// (that invariant covers link-silence ambiguity): asked-vs-done is
// the same measured rate the corpus itself mines, arriving sooner.
// K9IMM after four unanswered asks: 0.43 -> 0.14; one real forward
// restores it instantly. Cache lives in the book, which rebuilds
// every move.
double stFwd(QString const &call, QString const &band, qint64 nowMs) {
    auto &s = g_book.stations[call];
    if (s.fwdHabit < 0) {
        double base = intelFwd(call);
        // [relayprior] Announced relay-on within 24h (green-ring
        // criterion) floors the prior at the fleet average: direct
        // willingness evidence, not an unknown. Applied BEFORE the
        // demotion so failed asks still beat the announcement.
        if (g_relayGreen.contains(call))
            base = qMax(base, kFwdPrior);
        // [relayprior] Red-ring criterion (>=2 failed asks in 24h
        // since the last success): blend those REAL failures into
        // the prior un-decayed -- base*2/(2+f) (f=4 reproduces the
        // K9IMM 0.14). The 45-min session decay had forgotten them
        // by evening and the ranking asked a ringed station again.
        if (int const f = g_relayFails24.value(call); f >= 2)
            base = (base * 2.0) / (2.0 + f);
        double n = 0.0, a = 0.0;
        for (auto const &o : g_relayOutcomes.value(band)) {
            if (o.station != call)
                continue;
            double const w = std::pow(
                0.5, (nowMs - o.ms) / (SESSION_S * 1000.0));
            n += w;
            a += w * (o.forwarded ? 1.0 : 0.0);
        }
        s.fwdHabit = qMin(0.95, qMax(0.02,
            (base * 2.0 + a) / (2.0 + n)));
    }
    return s.fwdHabit;
}

// decide.py:295-306 _bearing + :307-324 toward — cosine preference,
// floor 0.45, and 1.0 (with "?" display) on any missing grid.
double bearingDeg(QString const &a, QString const &b) {
    auto const v = Geodesic::vector(a, b);
    return v.azimuth().isValid() ? double(v.azimuth()) : -1.0;
}
double toward(QString const &myGrid, QString const &candGrid,
              QString const &tGrid) {
    if (myGrid.size() < 4 || candGrid.size() < 4 || tGrid.size() < 4)
        return 1.0;
    double const want = bearingDeg(myGrid, tGrid);
    double const got = bearingDeg(myGrid, candGrid);
    if (want < 0 || got < 0)
        return 1.0;
    double off = std::fabs(std::fmod(want - got + 180.0, 360.0) - 180.0);
    return 0.45 + 0.55 * (1.0 + std::cos(off * M_PI / 180.0)) / 2.0;
}

// decide.py:629-632 _stale
double staleWorth(double sinceS) {
    return 1.0 - std::exp(-qMax(0.0, sinceS) / SESSION_S);
}

// decide.py:184-214 waits_for (display-only; control flow is
// slot-anchored) and the COST line of explain().
struct MoveCand {
    QString kind;        // snr | shout | relay | hearing | grid
    QString via;         // relay/hearing/grid subject (chain[0])
    QStringList chain;   // full relay chain, excl. target
    double cost = 0.0;
    double score = 0.0;  // p/cost for attempts, net gain for probes
    double p = 0.0;      // route probability (relay) or direct p
    QStringList factors; // pre-rendered factor lines
};

QString bar(double score, bool unknown) {
    if (unknown)
        return QStringLiteral("??????????");
    if (score >= 9.95)
        return QStringLiteral("**********+");
    return QString(int(std::round(score)), QLatin1Char('*'));
}
QString factorLine(QString const &name, QString const &raw,
                   double score, int width, bool unknown = false) {
    return QStringLiteral("  %1  %2  %3  %4")
        .arg(name, -width)
        .arg(score, 4, 'f', 1)
        .arg(bar(score, unknown), -11)
        .arg(raw);
}

} // namespace

// [livebook 2026-08-27] The book is NOT a per-attempt snapshot: the
// operator watched KI4RXJ hear NO1ZE at -12 DURING a run while the
// model ranked it on aged edges -- the observation reached the store
// at +2 min and the book, built at attempt start, never saw it
// ("a backwards search would reveal that" -- it did exist; it ran on
// stale data). Rebuilt from the store's layers before EVERY move
// decision; the live-learned YES overlays are re-applied on top.
void UI_Constructor::reachRefreshBook() {
    if (m_spotMapWindow) {   // [relayprior] ring criteria, per move
        g_relayGreen = m_spotMapWindow->announcedRelayers();
        g_relayGreen.unite(m_spotMapWindow->knownRelayers());
        g_relayFails24 = m_spotMapWindow->nonRelayerFails();
    }
    qint64 const now = DriftingDateTime::currentMSecsSinceEpoch();
    bool const intelWasOpen = g_book.intelOpen;
    g_book.edges.clear();
    g_book.stations.clear();
    g_book.transmitters.clear();
    g_book.intelOpen = intelWasOpen;
    for (auto const &s : m_spotMapWindow->activeStations(m_reach.band)) {
        BookStation b;
        b.grid = s.grid;
        b.lastSeenMs = s.lastSeenMs;
        b.snrToMe = s.snrToMe;
        b.hearsMe = s.hearsMe;
        b.txAlive = s.txAlive;
        b.source = s.source;
        g_book.stations.insert(s.call, b);
        if (s.txAlive)
            g_book.transmitters.insert(s.call);
    }
    for (auto const &e : m_spotMapWindow->allEdges(m_reach.band))
        bookAddEdge(e.hearer, e.heard, e.whenMs, e.snr, e.source);
    // Persistent tier at the 24 h horizon (livemodel.py:55 GRAPH_SECS
    // — the RAM store's 1 h pruning halves the graph, audit item 25).
    for (auto const &r : m_spotMapWindow->edges24h()) {
        if (r.band != m_reach.band)
            continue;
        bookAddEdge(r.hearer.toUpper(), r.heard.toUpper(),
                    r.when * 1000, r.snr, r.source);
        if (!g_book.stations.contains(r.hearer.toUpper())) {
            BookStation b;
            b.grid = r.hearerGrid;
            g_book.stations.insert(r.hearer.toUpper(), b);
        }
    }
    // Corpus edges (livemodel.py:150-157, :373-376): second source,
    // fresher wins; 8 of 12 candidates once lived only here.
    if (!g_book.intelOpen) {
        QString const path =
            QDir::home().filePath(QStringLiteral(
                ".config/js8reach-intel.db"));
        if (QFile::exists(path)) {
            auto db = QSqlDatabase::addDatabase(
                QStringLiteral("QSQLITE"),
                QStringLiteral("reach_intel"));
            db.setDatabaseName(path);
            db.setConnectOptions(
                QStringLiteral("QSQLITE_OPEN_READONLY"));
            g_book.intelOpen = db.open();
        }
    }
    if (g_book.intelOpen) {
        QSqlQuery q(QSqlDatabase::database(
            QStringLiteral("reach_intel")));
        q.prepare(QStringLiteral(
            "SELECT hearer, heard, last_when, snr, source FROM edges "
            "WHERE last_when > ?"));
        q.addBindValue(now / 1000 - 24 * 3600);
        if (q.exec())
            while (q.next())
                bookAddEdge(q.value(0).toString().toUpper(),
                            q.value(1).toString().toUpper(),
                            q.value(2).toLongLong() * 1000,
                            q.value(3).isNull() ? -99
                                                : q.value(3).toInt(),
                            q.value(4).toString());
    }

    // [oom 2026-08-29] g_book.learned is the durable in-session list
    // and is NEVER cleared above; re-derive its EDGES only. The old
    // copy-then-append-back DOUBLED the list every move -- 3 entries
    // became millions by move 20, each move first copying the whole
    // list (the lengthening GUI freezes) until the kernel OOM-killed
    // the app at 62 GB (WA4ZL attempt; W2NYX survived 55 moves only
    // because its empty shout left the list empty).
    for (auto const &l : g_book.learned)
        bookAddEdge(l.hearer, l.heard, l.whenMs, l.snr, l.source);
}

void UI_Constructor::reachStart(QString const &target, int maxMoves,
                                QString const &via) {
    if (m_reach.active) {
        reachLog(QStringLiteral("already reaching %1 -- stop it first")
                     .arg(m_reach.target));
        return;
    }
    if (!m_reachTimer) {
        m_reachTimer = new QTimer(this);
        m_reachTimer->setSingleShot(true);
        m_reachTimer->setTimerType(Qt::PreciseTimer);
        connect(m_reachTimer, &QTimer::timeout,
                this, &UI_Constructor::reachTick);
    }
    QString T = target.toUpper().trimmed();        // LITERAL, never base
    if (!Radio::is_callsign(T) && !isGridSquare(T)) {
        reachLog(QStringLiteral("%1 is neither a callsign nor a grid "
                                "square").arg(T));
        return;
    }
    m_reach = ReachState{};
    m_reach.forcedVia = via.toUpper().trimmed();
    m_reach.active = true;
    m_reach.target = T;
    m_reach.maxMoves = qBound(1, maxMoves, 12);
    m_reach.startMs = DriftingDateTime::currentMSecsSinceEpoch();
    m_reach.band = m_config.bands()->find(dialFrequency());
    m_reach.learnedAt0 = g_book.learned.size();   // [#196]

    // Speed pin (attempt.py:102-128): Normal for the whole attempt,
    // refusal aborts, restored on every exit incl. app quit.
    if (m_nSubMode != Varicode::JS8CallNormal) {
        if (!txIdleNow()) {
            reachLog(QStringLiteral("cannot pin Normal speed (tx busy)"
                                    " -- refusing to start"));
            m_reach = ReachState{};
            return;
        }
        m_reach.savedSubmode = m_nSubMode;
        setSubmode(Varicode::JS8CallNormal);
        reachLog(QStringLiteral("speed pinned to Normal (app was at %1;"
                                " restored on exit)")
                     .arg(m_reach.savedSubmode));
    }

    // [habitstore] resurrect the durable habit record once per
    // process -- asked-vs-forwarded and called-vs-answered follow
    // stations across restarts now ("we're throwing away good data").
    if (!g_eventsLoaded) {
        g_eventsLoaded = true;
        int n = 0;
        for (auto const &r : m_spotMapWindow->reachEvents()) {
            if (r.kind == QLatin1String("fwd"))
                g_relayOutcomes[r.band].append(
                    {r.when * 1000, r.ok, r.station});
            else if (r.kind == QLatin1String("ans"))
                g_ansOutcomes[r.band].append(
                    {r.when * 1000, r.ok, r.station});
            ++n;
        }
        if (n)
            reachLog(QStringLiteral("habit record loaded: %1 events "
                                    "(90-day retention)").arg(n));
    }

    reachRefreshBook();

    // [#180 gridtarget, ported verbatim from gridtarget.py] A GRID
    // is a first-class target: resolve it to the best reachable
    // occupant, screened (receive-only warns and sinks, never
    // refused), then run the normal loop. A square whose only
    // occupants are monitors that report us is a PARTIAL SUCCESS:
    // delivery-in provable, two-way not.
    if (isGridSquare(m_reach.target)) {
        // Resolution runs on the 6-char square: precision beyond
        // that is far below the 120-250 km candidate radius, and
        // Geodesic takes 4/6-char grids.
        QString const chosen =
            reachResolveGrid(m_reach.target.left(6));
        if (chosen.isEmpty()) {
            reachStop(QStringLiteral("no reachable station in %1")
                          .arg(m_reach.target));
            return;
        }
        m_reach.target = chosen;
        T = chosen;
    }

    // ---- pool: the python's two crude facts, screened -------------
    // (livemodel.py:343-413 LiveBoard: hearers of the target ordered
    // by freshness, then geography near the target; is_routable on
    // BOTH branches; phantom edges — no snr AND never seen keying —
    // are evidence of nothing, live.py:286-302; limit 40.)
    QString const me = m_config.my_callsign().trimmed().toUpper();
    QString const tGrid = m_spotMapWindow->knownGrid(T);
    QVector<QPair<qint64, QString>> hearers;
    for (auto it = g_book.edges.constBegin();
         it != g_book.edges.constEnd(); ++it) {
        auto const he = it.value().constFind(T);
        if (he == it.value().constEnd())
            continue;
        if (he.value().snr <= -99 &&
            !g_book.transmitters.contains(it.key()))
            continue;                       // phantom edge
        hearers.append({he.value().whenMs, it.key()});
    }
    std::sort(hearers.begin(), hearers.end(),
              [](auto const &a, auto const &b) {
                  return a.first > b.first;
              });
    QSet<QString> seen;
    for (auto const &[whenMs, h] : hearers) {
        if (h == me || Radio::same_station(h, me) || h == T ||
            seen.contains(h) || !isRoutable(h))
            continue;
        seen.insert(h);
        g_book.pool.append(h);
    }
    if (!tGrid.isEmpty()) {
        for (auto it = g_book.stations.constBegin();
             it != g_book.stations.constEnd(); ++it) {
            QString const c = it.key();
            if (c == me || Radio::same_station(c, me) || c == T ||
                seen.contains(c) || !isRoutable(c))
                continue;
            if (it.value().grid.size() < 4)
                continue;
            auto const v = Geodesic::vector(tGrid, it.value().grid);
            if (!v.distance().isValid() || float(v.distance()) > kGeoKm)
                continue;
            seen.insert(c);
            g_book.pool.append(c);
        }
    }
    while (g_book.pool.size() > kPoolLimit)
        g_book.pool.removeLast();

    // [firsthop 2026-08-27, operator: "it would augment other
    // knowledge very nicely"] First-hop eligibility = HEARS US
    // RELIABLY, regardless of where it sits. The target-centric pool
    // stays the ranking prior, but stations with hears-us evidence
    // (routable, transmit-alive) join the walk and may anchor chains
    // -- the KS1DMD case: near us, hears us at -13, invisible to a
    // target-centric pool 1500 km away.
    // [operator's trick, 2026-08-28 -- NOT WIRED UP YET] If we ever
    // need a LIVE survey of who currently hears us (this set is
    // built from stored evidence only), transmit
    //     "WM8Q: @ALLCALL QUERY CALL WM8Q?"
    // -- the self-query. Every station that hears us answers
    // "YES <snr> (NOW)", and unlike an HB request it works on
    // stations that have heartbeats disabled. One 2-frame broadcast
    // refreshes the whole hears-us picture; the existing QUERY CALL
    // harvest (#161/#178) already binds the answers into the hearing
    // store, so wiring this up is only a matter of choosing when the
    // executor spends the airtime.
    g_book.walk = g_book.pool;
    g_book.firstHops = QSet<QString>(g_book.pool.begin(),
                                     g_book.pool.end());
    for (auto it = g_book.stations.constBegin();
         it != g_book.stations.constEnd(); ++it) {
        QString const c = it.key();
        if (g_book.firstHops.contains(c) || c == me ||
            Radio::same_station(c, me) || c == T)
            continue;
        if (!it.value().hearsMe || !it.value().txAlive ||
            !isRoutable(c))
            continue;
        g_book.walk.append(c);
        g_book.firstHops.insert(c);
    }

    // #173 named-target screen (attempt.py:195-210): warn, never
    // refuse -- BOTH halves this time (audit item 37).
    auto const ts = g_book.stations.value(T);
    if (!ts.txAlive && ts.lastSeenMs > 0) {
        QString extra;
        if (ts.hearsMe && ts.snrToMe > -99)
            extra = QStringLiteral(" (it hears us at %1 -- delivery "
                                   "provable)")
                        .arg(Varicode::formatSNR(ts.snrToMe));
        reachLog(QStringLiteral("WARNING: %1 has never been heard "
                                "transmitting -- receive-only monitor;"
                                " expect no reply%2").arg(T, extra));
    } else if (ts.lastSeenMs <= 0) {
        reachLog(QStringLiteral("WARNING: %1 never heard on radio or "
                                "internet this session").arg(T));
    } else if (ts.source == QLatin1String("mqtt")) {
        reachLog(QStringLiteral("WARNING: %1 never heard on radio this"
                                " session -- internet-only evidence")
                     .arg(T));
    }
    // [freshgate 2026-08-27, operator: "it's crowding out other
    // possibilities"] Two delivered-and-silent verdicts on record
    // with NO transmission from the target since the latest one =
    // the record says unattended. The attempt then spends ONE direct
    // call (keeps the evidence current) and refuses relays, shouts
    // and probes -- two proven deliveries unanswered is the same
    // once-is-enough doctrine applied to the delivered ask. The gate
    // clears itself: it compares recorded times, so the target being
    // heard transmitting again reopens everything.
    {
        qint64 latestDeliveredSilent = 0;
        int deliveredSilent = 0;
        for (auto it = g_ansOutcomes.constBegin();
             it != g_ansOutcomes.constEnd(); ++it)
            for (auto const &o : it.value())
                if (o.station == T && !o.answered) {
                    ++deliveredSilent;
                    latestDeliveredSilent =
                        qMax(latestDeliveredSilent, o.ms);
                }
        // Transmission evidence from ANY witness clears the gate --
        // a PSKR spot, a YES answer's age, a HEARING list, our own
        // decode: every edge naming the target as HEARD proves it
        // transmitted, and all but PSKR work with no internet
        // (operator: "suppose no-internet, how would we hear that
        // the station had transmitted?").
        qint64 lastTxEvidence = 0;
        auto const ts = g_book.stations.value(T);
        if (ts.source == QLatin1String("radio"))
            lastTxEvidence = ts.lastSeenMs;
        for (auto it = g_book.edges.constBegin();
             it != g_book.edges.constEnd(); ++it) {
            auto const he = it.value().constFind(T);
            if (he != it.value().constEnd())
                lastTxEvidence = qMax(lastTxEvidence,
                                      he.value().whenMs);
        }
        if (deliveredSilent >= 2 &&
            lastTxEvidence < latestDeliveredSilent) {
            m_reach.relaysBlocked = true;
            m_reach.gateSilenceMs = latestDeliveredSilent;
            reachLog(QStringLiteral("WARNING: %1 has %2 unanswered "
                                    "asks on record and no station "
                                    "has heard it since -- relays "
                                    "restricted to routes with "
                                    "evidence newer than that "
                                    "silence; the shout stays "
                                    "available to ask the band")
                         .arg(T).arg(deliveredSilent));
        }
    }
    reachLog(QStringLiteral("target %1 on %2; %3 candidates")
                 .arg(T, m_reach.band)
                 .arg(g_book.pool.size()));
    reachNextMove();
}

// [#180] gridtarget.py collect/rank/resolve, ported verbatim onto
// the native stores: the book's stations (grid, presence age, radio
// evidence = source "radio", rx_only = never seen as sender,
// reports_me = hears-us with a report) plus the intel corpus for
// stations the live stores aged out. Distance via Geodesic, whose
// 120 km floor on 4-char squares applies naturally. Rank class:
// radio <1h/<6h/<48h/older = 0/1/2/3; internet-only +10; receive-
// only +20 (sinks hardest, never refused). Ledger mirrors the python.
QString UI_Constructor::reachResolveGrid(QString const &square) {
    qint64 const now = DriftingDateTime::currentMSecsSinceEpoch();
    struct Cand {
        QString call, grid, warn;
        double km = 0;
        qint64 radioAgeS = -1, anyAgeS = -1;
        bool rxOnly = false, reportsMe = false;
        int snrToMe = -99;
        int cls = 4;
    };
    QHash<QString, Cand> seen;
    for (auto it = g_book.stations.constBegin();
         it != g_book.stations.constEnd(); ++it) {
        auto const &b = it.value();
        if (b.grid.size() < 4)
            continue;
        auto const v = Geodesic::vector(square, b.grid.left(6));
        if (!v.distance().isValid() || float(v.distance()) > 250.0f)
            continue;
        Cand c;
        c.call = it.key();
        c.grid = b.grid;
        c.km = double(v.distance());
        if (b.lastSeenMs > 0) {
            c.anyAgeS = (now - b.lastSeenMs) / 1000;
            if (b.source == QLatin1String("radio"))
                c.radioAgeS = c.anyAgeS;
        }
        c.rxOnly = !b.txAlive;
        c.reportsMe = b.hearsMe && b.snrToMe > -99;
        c.snrToMe = b.snrToMe;
        c.warn = c.rxOnly ? QStringLiteral("receive-only monitor")
                 : c.radioAgeS < 0
                     ? QStringLiteral("never heard on radio")
                     : QString{};
        seen.insert(c.call, c);
    }
    if (g_book.intelOpen) {
        QSqlQuery q(QSqlDatabase::database(
            QStringLiteral("reach_intel")));
        q.prepare(QStringLiteral(
            "SELECT call, grid, last_heard FROM stations WHERE grid "
            "IS NOT NULL AND grid<>''"));
        if (q.exec())
            while (q.next()) {
                QString const call = q.value(0).toString().toUpper();
                if (seen.contains(call))
                    continue;
                QString const gr = q.value(1).toString();
                if (gr.size() < 4)
                    continue;
                auto const v = Geodesic::vector(square, gr.left(6));
                if (!v.distance().isValid() ||
                    float(v.distance()) > 250.0f)
                    continue;
                Cand c;
                c.call = call;
                c.grid = gr;
                c.km = double(v.distance());
                qint64 const lh = q.value(2).toLongLong();
                if (lh > 0)
                    c.radioAgeS = now / 1000 - lh;
                c.warn = QStringLiteral(
                    "corpus only, not seen this session");
                seen.insert(call, c);
            }
    }
    QVector<Cand> cands;
    for (auto &c : seen) {
        int cls = 4;
        if (c.radioAgeS >= 0)
            cls = c.radioAgeS < 3600 ? 0
                  : c.radioAgeS < 6 * 3600 ? 1
                  : c.radioAgeS < 48 * 3600 ? 2 : 3;
        else if (c.anyAgeS >= 0 && c.anyAgeS < 3600)
            cls = 2;
        if (c.radioAgeS < 0 && c.anyAgeS >= 0)
            cls += 10;
        if (c.rxOnly)
            cls += 20;   // cannot answer by construction: sinks hardest
        c.cls = cls;
        cands.append(c);
    }
    std::sort(cands.begin(), cands.end(),
              [](Cand const &a, Cand const &b) {
                  if (a.cls != b.cls)
                      return a.cls < b.cls;
                  return a.km < b.km;
              });
    if (cands.isEmpty()) {
        reachLog(QStringLiteral("  no station known within 250 km of "
                                "%1 on %2")
                     .arg(square, m_reach.band));
        return {};
    }
    reachLog(QStringLiteral("  %1: %2 candidates (120 km floor "
                            "applies to 4-char squares)")
                 .arg(square).arg(cands.size()));
    for (int i = 0; i < qMin(8, int(cands.size())); ++i) {
        auto const &c = cands[i];
        QString age = c.radioAgeS < 0 ? QStringLiteral("radio never")
                      : c.radioAgeS < 7200
                          ? QStringLiteral("radio %1m")
                                .arg(c.radioAgeS / 60)
                          : QStringLiteral("radio %1h")
                                .arg(c.radioAgeS / 3600.0, 0, 'f', 1);
        reachLog(QStringLiteral("    %1 %2 %3 km  %4%5")
                     .arg(c.call, -9)
                     .arg(c.grid, -8)
                     .arg(c.km, 4, 'f', 0)
                     .arg(age)
                     .arg(c.warn.isEmpty()
                              ? QString{}
                              : QStringLiteral("   !! ") + c.warn));
    }
    Cand const *pick = nullptr;
    for (auto const &c : cands)
        if (!c.rxOnly) {
            pick = &c;
            break;
        }
    for (auto const &c : cands)
        if (c.rxOnly && c.reportsMe)
            reachLog(QStringLiteral("  note: %1 (monitor, %2 km) "
                                    "reports us at %3 dB -- delivery "
                                    "into %4 provable even without a "
                                    "contact")
                         .arg(c.call)
                         .arg(c.km, 0, 'f', 0)
                         .arg(Varicode::formatSNR(c.snrToMe))
                         .arg(square));
    if (!pick) {
        reachLog(QStringLiteral("  every known station in %1 is a "
                                "receive-only monitor: nothing can "
                                "answer; partial success at best")
                     .arg(square));
        return {};
    }
    reachLog(QStringLiteral("  -> %1 (%2, %3 km) enters the normal "
                            "loop as the target%4")
                 .arg(pick->call, pick->grid)
                 .arg(pick->km, 0, 'f', 0)
                 .arg(pick->warn.isEmpty()
                          ? QString{}
                          : QStringLiteral("   (WARNED: ") + pick->warn
                                + QStringLiteral(")")));
    return pick->call;
}

void UI_Constructor::reachStop(QString const &reason) {
    if (!m_reach.active)
        return;
    reachLog(QStringLiteral("STOP: %1 (%2 transmissions, %3s total)")
                 .arg(reason)
                 .arg(m_reach.sent)
                 .arg((DriftingDateTime::currentMSecsSinceEpoch()
                       - m_reach.startMs) / 1000));
    if (m_reachTimer)
        m_reachTimer->stop();
    if (m_spotMapWindow)
        m_spotMapWindow->clearAttempts();
    m_reach.active = false;
    reachRestoreSpeed();
    // [autoroute] The mode ends with the attempt, whatever ended it.
    if (m_autoRouteActive)
        autoRouteReachStopped(reason);
}

// [autoroute 2026-08-28] Mode entry: lock the main screen (the flag
// is folded into the ARQ gate predicates, polled by guiUpdate), then
// run the executor. reachStart can REFUSE (already active is handled
// here; TX-busy speed pin) -- a refusal ends the mode immediately as
// a failure, never a silent hang.
void UI_Constructor::autoRouteBegin(QString const &target) {
    // [operator 2026-08-30] A busy radio is a WAIT, not a failure:
    // say which kind in a toast and leave everything untouched --
    // no mode entry, no failure dialog, a running API attempt keeps
    // its slot. When the radio is idle, the start below pins Normal
    // speed automatically whatever speed the app was at.
    if (QString const busy = txBusyToastText(); !busy.isEmpty()) {
        if (m_spotMapWindow)
            m_spotMapWindow->showToast(busy);
        return;
    }
    if (m_reach.active) {
        // An API-driven attempt is already running; the operator's
        // auto-route takes over.
        reachStop(QStringLiteral("superseded by auto-route"));
    }
    m_autoRouteActive = true;
    m_autoRouteCancel = false;
    m_autoRouteTarget = target;
    // [ssdetect] An auto-route run must be fully decodable: force
    // the Subspace-decode switch on for the duration (and beyond --
    // operator turns it back off if wanted).
    if (!m_l2Enabled)
        setSubspaceDecodeEnabled(true,
                                 QStringLiteral("auto-route begin"));
    // [autoroute] Take the Last Tx status label over for the mode --
    // SAME cache/valid mechanism as the ARQ progress override (build
    // 252): the valid flag silences the per-frame writer, and ARQ
    // and auto-route can never run concurrently. "Auto-route" until
    // the first path goes out; reachSend then numbers each path.
    if (!m_lastTxLabelCacheValid) {
        m_lastTxLabelCache = last_tx_label.text();
        m_lastTxLabelCacheValid = true;
    }
    last_tx_label.setText(QStringLiteral("Auto-route"));
    // A leftover draft would block the executor's queue (the TX
    // queue only drains into an EMPTY box) and the banner belongs
    // there anyway (operator, 2026-08-28: clear at start).
    ui->extFreeTextMsgEdit->clear();
    // [queueclear, operator 2026-08-31] The mode is the only
    // speaker AND the only listener: anything already in the
    // OUTGOING message queue would drain into the first free slot
    // -- exactly the listening window -- and defeat the wait
    // (there is no room in the auto-route protocol to pause a
    // drain). Anything in the INBOUND command queue would churn
    // mid-mode (its replies are gated at [REPLY-GATE] anyway).
    // Both cleared at mode entry; new replies stay suppressed for
    // the mode's duration by the existing gate.
    m_txMessageQueue = {};
    m_rxCommandQueue.clear();
    // [modegate 2026-09-04, special-case item 3] An autoreply-
    // confirmation dialog opened BEFORE the mode could still enqueue
    // its transmission mid-run via Yes or its self-destruct default.
    // Same rule, same door: reject them all at entry (No path --
    // discards without enqueueing).
    for (auto *box : findChildren<SelfDestructMessageBox *>()) {
        qCWarning(mainwindow_js8)
            << "[REACH] open autoreply-confirmation dialog rejected"
            << "at mode entry";
        box->reject();
    }
    refreshOutgoingPlaceholder();
    reachStart(target);
    if (!m_reach.active) {
        // Refused (speed pin while transmitting, bad grid resolve
        // path, etc.) -- reachStop never fired, so close out here.
        autoRouteReachStopped(QStringLiteral("refused"));
        return;
    }
    // [modeowner 2026-08-30] The mode ACTUALLY started -- only now
    // does the map hear about it and flip its UI. The map no longer
    // arms itself at click time, so a refusal above leaves it
    // untouched (prompt open, target still typed).
    if (m_spotMapWindow)
        m_spotMapWindow->autoRouteStarted(m_autoRouteTarget);
}

// [autoroute] Operator cancel -- the map button or the main Halt.
void UI_Constructor::autoRouteCancel() {
    if (!m_autoRouteActive)
        return;
    // Kill any in-flight transmission exactly as the main Halt does
    // (operator, 2026-08-28: the map's Halt left the TX running).
    // Idempotent when the main-Halt path already called it.
    stopTxMechanical();
    m_autoRouteCancel = true;
    if (m_reach.active)
        reachStop(QStringLiteral("stopped by operator"));
    else
        autoRouteReachStopped(QStringLiteral("stopped by operator"));
}

// [autoroute] Single exit point: success dialog on REACHED, failure
// dialog otherwise, cancel toast when the operator halted. The map
// tears down its button/status; the main-screen locks release on the
// next gate poll once the flag is false.
void UI_Constructor::autoRouteReachStopped(QString const &reason) {
    // Idempotent: a grid that resolves to nothing makes reachStart
    // refuse via reachStop, whose tail already ran this teardown --
    // autoRouteBegin's refusal check then called it AGAIN with a
    // cleared target, producing a second (empty-named) failure
    // dialog (operator, 2026-08-29).
    if (!m_autoRouteActive)
        return;
    bool const canceled = m_autoRouteCancel;
    bool const success = (reason == QLatin1String("REACHED"));
    QString const target = m_autoRouteTarget;
    m_autoRouteActive = false;
    m_autoRouteCancel = false;
    m_autoRouteTarget.clear();
    // Give the Last Tx label back (mirrors the ARQ progressEnd
    // restore).
    if (m_lastTxLabelCacheValid) {
        last_tx_label.setText(m_lastTxLabelCache);
        m_lastTxLabelCache.clear();
        m_lastTxLabelCacheValid = false;
    }
    if (m_spotMapWindow)
        m_spotMapWindow->autoRouteEnded(canceled);
    refreshOutgoingPlaceholder();
    if (canceled)
        return; // the map showed the "canceled" toast
    // [exitghost 2026-08-30] Exiting with a route still running ends
    // the attempt through this same path -- and the failure box it
    // built flashed behind the closing main window (operator: "an
    // extra dialog hidden behind the main window... at exit"). A
    // teardown is not a verdict; no dialog.
    if (reason == QLatin1String("app shutting down"))
        return;
    // [operator 2026-08-29] The result box was getting lost behind
    // the main window (auto-route is driven from the map). Parent it
    // to whichever of OUR windows is active and grab focus with the
    // ARQ result dialog's exact recipe (show + raise +
    // activateWindow, chunkedArqHooks.cpp).
    QWidget *host = this;
    if (m_spotMapWindow && m_spotMapWindow->isActiveWindow())
        host = m_spotMapWindow.data();
    auto *box = new QMessageBox(host);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::NonModal);
    // [operator 2026-08-29, second report] raise+activateWindow was
    // not enough -- the window manager may refuse focus theft for a
    // non-modal box, and the "active window" parent guess loses when
    // neither of ours is active. Stays-on-top guarantees it fronts
    // our windows until dismissed.
    box->setWindowFlag(Qt::WindowStaysOnTopHint, true);
    box->setStandardButtons(QMessageBox::Ok);
    if (success) {
        box->setIcon(QMessageBox::Information);
        box->setWindowTitle(tr("Auto-route"));
        box->setText(tr("Auto-route to %1 successful, enter your "
                        "message to that station in the outgoing "
                        "text box.").arg(target));
    } else {
        box->setIcon(QMessageBox::Warning);
        box->setWindowTitle(tr("Auto-route"));
        // [operator 2026-08-30] Say what actually stopped it: the
        // one-size text claimed a failed path search for stops that
        // never searched (the KQ4ODY speed-pin refusal).
        QString text =
            reason.startsWith(QLatin1String("no reachable station"))
                ? tr("No reachable station near that grid")
                : tr("Auto-route failed to find a path to %1")
                      .arg(target);
        // [#196] Second line when the attempt left something behind.
        // "will": an ask/answer outcome was journaled this attempt --
        // those re-weight the SAME candidates' factors on a retry
        // (45-min half-life), so the next search is guaranteed to
        // rank differently. "can": no outcomes, but the book learned
        // new stations/edges while we listened -- new information
        // that MAY open a route. Neither: no line (e.g. halted
        // before any transmission). startMs == 0 means the attempt
        // was refused at the door and nothing ran.
        qint64 const t0 = m_reach.startMs;
        bool will = false;
        if (t0 > 0) {
            for (auto const &o : g_relayOutcomes.value(m_reach.band))
                if (o.ms >= t0) { will = true; break; }
            if (!will)
                for (auto const &o : g_ansOutcomes.value(m_reach.band))
                    if (o.ms >= t0) { will = true; break; }
        }
        bool const can = !will && t0 > 0 &&
                         g_book.learned.size() > m_reach.learnedAt0;
        if (will || can) {
            text += tr("\n\nIf you want to try again, the "
                       "information from this attempt %1 help the "
                       "next search.")
                        .arg(will ? tr("will") : tr("can"));
            // [retrybtn, operator 2026-08-30] When the attempt left
            // usable information behind, offer the retry right in
            // the verdict. Retry re-enters through the normal front
            // door (autoRouteBegin), so the busy gate, mode locks
            // and map notification all apply as if Start were
            // clicked.
            box->setStandardButtons(QMessageBox::Retry |
                                    QMessageBox::Cancel);
            box->setDefaultButton(QMessageBox::Retry);
            QString const retryTarget = target;
            connect(box, &QMessageBox::buttonClicked, this,
                    [this, box, retryTarget](QAbstractButton *b) {
                        if (box->standardButton(b) ==
                            QMessageBox::Retry)
                            autoRouteBegin(retryTarget);
                    });
        }
        box->setText(text);
    }
    box->show();
    box->raise();
    box->activateWindow();
}

// Speed restore with retry (audit item 14: the python's atexit fired
// on every exit; the first port's "restore manually" branch leaked).
// Called from reachStop and from the destructor path; retries on a
// timer until the TX queue frees.
void UI_Constructor::reachRestoreSpeed() {
    if (m_reach.savedSubmode < 0)
        return;
    if (txIdleNow()) {
        setSubmode(m_reach.savedSubmode);
        reachLog(QStringLiteral("speed restored to %1")
                     .arg(m_reach.savedSubmode));
        m_reach.savedSubmode = -1;
        return;
    }
    reachLog(QStringLiteral("speed restore pending (tx busy) -- "
                            "retrying in 5s"));
    QTimer::singleShot(5000, this, &UI_Constructor::reachRestoreSpeed);
}

// decide.py:830-901 choose(), :636-712 attempts(), :716-780 probes()
// — the full decision, ported mechanically. 1-hop chains only
// (declared); probes hearing?/grid? included (audit item 10).
void UI_Constructor::reachNextMove() {
    if (!m_reach.active)
        return;
    qint64 const now = DriftingDateTime::currentMSecsSinceEpoch();
    // Late-forward hold (attempt.py:255-265): leave the air clear —
    // a late relay's reply may be inbound. reachOnDirected still
    // catches a REACHED during the hold.
    if (now < m_reach.holdUntilMs) {
        m_reachTimer->start(m_reach.holdUntilMs - now);
        return;
    }
    // [operator ruling 2026-08-30] The move budget is HARD -- no
    // extensions of any kind. Over budget nothing new is started:
    // no direct, no shout, no probes, no routes; the in-flight move
    // finishes its verdict and the attempt stops.
    bool const overBudget = m_reach.moveNo >= m_reach.maxMoves;
    reachRefreshBook();   // [livebook] the store may know more now
    // [relayalive 2026-08-27, operator: "do not exclude is
    // consistent" -- downgrade instead] A relaying station must
    // transmit; its on-the-air factor runs on its newest TRANSMIT
    // evidence (heard by anyone, 24h book + corpus), through the
    // same curve. Internet-reporting presence no longer counts as
    // relay-aliveness (the KC2DAC case: raise 1.00, alive 0.95,
    // never keyed once).
    QHash<QString, qint64> lastTxMs;
    for (auto itE = g_book.edges.constBegin();
         itE != g_book.edges.constEnd(); ++itE)
        for (auto e = itE.value().constBegin();
             e != itE.value().constEnd(); ++e) {
            auto &t = lastTxMs[e.key()];
            if (e.value().whenMs > t)
                t = e.value().whenMs;
        }
    QString const me = m_config.my_callsign().trimmed().toUpper();
    QString const T = m_reach.target;
    QString const myGrid = m_config.my_grid().left(6);
    QString const tGrid = m_spotMapWindow->knownGrid(T);

    m_reach.kind.clear();
    m_reach.via.clear();
    m_reach.txEndMs = 0;
    m_reach.deadlineMs = 0;
    m_reach.moveCapMs = 0;
    // Every per-move observation field resets together, chain
    // included -- a field missed here leaks one move's observation
    // into the next move's verdict (happened 2026-08-27, AB9MK
    // attempt, with the since-deleted return-start timestamp).
    m_reach.fwdStartedMs = m_reach.fwdDoneMs = m_reach.ansStartedMs =
        m_reach.retStartedMs = 0;
    m_reach.retOffset = 0;
    m_reach.fwdExpFrames = m_reach.ansExpFrames =
        m_reach.retExpFrames = m_reach.fwdFrames =
        m_reach.fwdOffset = m_reach.retWindowSlots = 0;
    m_reach.forcedVerdict.clear();
    m_reach.chain.clear();
    m_reach.watchers.clear();

    // ---- forced first move (attempt.py:239-246) --------------------
    if (!m_reach.forcedVia.isEmpty() && m_reach.moveNo == 0) {
        QString const via = m_reach.forcedVia.toUpper();
        reachLog(QStringLiteral("[1] FORCED route via %1").arg(via));
        m_reach.kind = QStringLiteral("relay");
        m_reach.via = via;
        m_reach.chain = QStringList{via};
        reachComputeExpected();   // [structured]
        m_reach.triedAt.insert(QStringLiteral("relay:") + via, now);
        reachSend(QStringLiteral("%1: %2>%3 SNR?").arg(me, via, T));
        return;
    }

    // ---- attempts: direct + every 1-hop route, scored p/cost ------
    struct Ranked { double score; MoveCand mv; };
    QVector<Ranked> ranked;

    double pd = pCopy(T, now) * pHearsUs(this, T, me, now)
                * stAns(T, now);
    qint64 const snrTried =
        m_reach.triedAt.value(QStringLiteral("snr:") + T, -1);
    double staleF = 1.0;
    if (snrTried >= 0) {
        // decide.py:641-650: a repeat only buys the chance they
        // ARRIVED; if they were present and silent, they never left.
        staleF = staleWorth((now - snrTried) / 1000.0);
        pd *= staleF * qMax(0.05, 1.0 - pCopy(T, now));
    }
    {
        MoveCand m;
        m.kind = QStringLiteral("snr");
        m.cost = T_SNR;
        m.p = pd;
        m.score = pd / T_SNR;
        int const w = QStringLiteral("worth calling again yet").size();
        m.factors
            << factorLine(QStringLiteral("they are on the air"),
                          QString::number(pCopy(T, now), 'f', 2),
                          10 * pCopy(T, now), w)
            << factorLine(QStringLiteral("they can hear us"),
                          QString::number(pHearsUs(this, T, me, now),
                                          'f', 2),
                          10 * pHearsUs(this, T, me, now), w)
            << factorLine(QStringLiteral("they answer when called"),
                          QString::number(stAns(T, now), 'f', 2),
                          10 * stAns(T, now), w)
            << factorLine(QStringLiteral("worth calling again yet"),
                          snrTried < 0 ? QStringLiteral("first call")
                                       : QString::number(staleF, 'f', 2),
                          snrTried < 0 ? 10.0 : 10 * staleF, w);
        ranked.append({m.score, m});
    }
    // [dijkstra 2026-08-27] best_routes ported VERBATIM
    // (decide.py:336-445) -- the operator's Parallel Path Tree,
    // restored after being wrongly dropped with the multi-hop scope
    // cut ("the backwards-graph part got dropped too?" -- yes, and
    // here it returns). Backward walk FROM the target over the
    // shortlist, -log(p) weights, chains to 4 hops; the first hop
    // must be a station we can actually raise; intermediate hops are
    // not restricted (we never talk to them directly).
    {
        int const maxHops = 4;
        QHash<QString, double> dist;
        QHash<QString, QStringList> route;
        QSet<QString> seenN;
        dist.insert(T, -std::log(qMax(1e-9, stAns(T, now))));
        route.insert(T, {});
        using QP = QPair<double, QString>;
        QVector<QP> heap;
        heap.append({dist.value(T), T});
        auto heapLess = [](QP const &a, QP const &b) {
            return a.first > b.first;   // min-heap via greater
        };
        std::make_heap(heap.begin(), heap.end(), heapLess);
        while (!heap.isEmpty()) {
            std::pop_heap(heap.begin(), heap.end(), heapLess);
            auto const [cost, node] = heap.takeLast();
            if (seenN.contains(node))
                continue;
            seenN.insert(node);
            if (route.value(node).size() >= maxHops)
                continue;
            for (QString const &cand : g_book.walk) {
                if (seenN.contains(cand) || cand == me ||
                    Radio::same_station(cand, me))
                    continue;
                double const out = deliversTo(this, cand, node);
                double const back = pReverse(node, cand);
                bool const observed =
                    bookEdge(cand, node).whenMs > 0 ||
                    bookEdge(node, cand).whenMs > 0;
                QString const cGrid = g_book.stations.value(cand).grid;
                double const direction =
                    observed ? 1.0 : toward(myGrid, cGrid, tGrid);
                double const trust = bandTrust(m_reach.band, now);
                double const fwdEff = qMin(0.95, qMax(
                    0.02, stFwd(cand, m_reach.band, now) * (trust / kFwdPrior)));
                double const candTx = [&] {
                    BookStation b = g_book.stations.value(cand);
                    b.lastSeenMs = lastTxMs.value(cand, 0);
                    double const age =
                        b.lastSeenMs > 0
                            ? (now - b.lastSeenMs) / 1000.0 : -1.0;
                    if (age < 0)
                        return 0.02;
                    return age <= 900.0
                        ? 0.95
                        : qMax(0.05, 0.95 * std::exp(
                              -(age - 900.0) / SESSION_S));
                }();
                double const live = candTx * fwdEff * direction;
                double const step = out * back * live;
                if (step <= 1e-9)
                    continue;
                double const nd = cost - std::log(step);
                if (nd < dist.value(cand,
                                    std::numeric_limits<double>::max())) {
                    dist.insert(cand, nd);
                    QStringList r;
                    r << cand;
                    r += route.value(node);
                    route.insert(cand, r);
                    heap.append({nd, cand});
                    std::push_heap(heap.begin(), heap.end(), heapLess);
                }
            }
            if (seenN.size() > 960)
                break;
        }
        // [walkviz, operator 2026-08-28: "show 3-hop evaluations"]
        // Everything the backward walk produced beyond one relay --
        // including chains later dropped because their first hop
        // does not hear us -- plus the raw material it had to work
        // with. When no chains appear here, the reason is visible:
        // chains can only follow station-to-station hearing evidence
        // (an edge or its reciprocity), never guesses.
        {
            int interEdges = 0;
            for (auto e = g_book.edges.constBegin();
                 e != g_book.edges.constEnd(); ++e)
                if (!Radio::same_station(e.key(), me))
                    interEdges += e.value().size();
            int chains = 0;
            for (auto it = route.constBegin();
                 it != route.constEnd(); ++it) {
                if (it.key() == T || it.value().size() < 2)
                    continue;
                ++chains;
                reachLog(QStringLiteral(
                             "      walk chain: %1>%2  -log(p)=%3%4")
                             .arg(it.value().join(QLatin1Char('>')),
                                  T)
                             .arg(dist.value(it.key()), 0, 'f', 2)
                             .arg(g_book.firstHops.contains(
                                      it.value().first())
                                      ? QString{}
                                      : QStringLiteral(
                                            "  DROPPED: first hop "
                                            "does not hear us")));
            }
            reachLog(QStringLiteral("    walk: %1 stations, %2 "
                                    "station-to-station edges, %3 "
                                    "multi-relay chains formed")
                         .arg(g_book.walk.size())
                         .arg(interEdges)
                         .arg(chains));
        }
        for (auto it = route.constBegin(); it != route.constEnd();
             ++it) {
            QString const &cand = it.key();
            QStringList const &chain = it.value();
            if (cand == T || chain.isEmpty())
                continue;
            if (!g_book.firstHops.contains(chain.first()))
                continue;   // first hop must hear us reliably
            double p = std::exp(-dist.value(cand))
                       * pHearsUs(this, chain.first(), me, now);
            // [chainid, operator-approved 2026-08-28] A route's
            // tried-identity is the FULL chain: KC6UCN>N6CYB is not
            // a retry of N6CYB (the last-hop key made an untried
            // multi-hop look spent -- the KE8SNO miss).
            qint64 const tried = m_reach.triedAt.value(
                QStringLiteral("relay:") + chain.join(QLatin1Char('>')),
                -1);
            if (tried >= 0)
                p *= staleWorth((now - tried) / 1000.0);
            if (p <= 0.0)
                continue;
            int const hops = chain.size();
            double const cost = kTRelay[qMin(4, hops)];
            QString const first = chain.first();
            QString const lastHop = chain.last();
            double const raise_ = pHearsUs(this, first, me, now);
            double copy_;
            {
                qint64 const t = lastTxMs.value(first, 0);
                double const age =
                    t > 0 ? (now - t) / 1000.0 : -1.0;
                copy_ = age < 0 ? 0.02
                        : age <= 900.0
                            ? 0.95
                            : qMax(0.05, 0.95 * std::exp(
                                  -(age - 900.0) / SESSION_S));
            }
            double const trust = bandTrust(m_reach.band, now);
            double const fwd_ = qMin(0.95, qMax(
                0.02, stFwd(first, m_reach.band, now) * (trust / kFwdPrior)));
            double const deliver = deliversTo(this, lastHop, T);
            double back = 1.0;
            QString prev = T;
            for (int i = chain.size() - 1; i >= 0; --i) {
                back *= pReverse(prev, chain[i]);
                prev = chain[i];
            }
            QString const cGrid = g_book.stations.value(first).grid;
            bool const linkSeen =
                bookEdge(first, T).whenMs > 0 ||
                bookEdge(T, first).whenMs > 0;
            double const dir =
                linkSeen ? 1.0 : toward(myGrid, cGrid, tGrid);
            double const ans_ = stAns(T, now);
            MoveCand m;
            m.kind = QStringLiteral("relay");
            m.via = first;
            m.chain = chain;
            m.cost = cost;
            m.p = p;
            m.score = p / cost;
            int const w =
                (first + QStringLiteral(" forwards when asked")).size();
            bool const dirUnknown = !linkSeen && cGrid.size() < 4;
            m.factors
                << factorLine(QStringLiteral("we can raise ") + first,
                              QString::number(raise_, 'f', 2),
                              10 * raise_, w)
                << factorLine(first + QStringLiteral(" is on the air"),
                              QString::number(copy_, 'f', 2),
                              10 * copy_, w)
                << factorLine(first
                                  + QStringLiteral(" forwards when asked"),
                              QString::number(fwd_, 'f', 2)
                                  // [relayprior] show when a ring
                                  // criterion moved this factor
                                  + (g_relayFails24.value(first) >= 2
                                         ? QStringLiteral(" (relay disabled?)")
                                     : g_relayGreen.contains(first)
                                         ? QStringLiteral(" (relay enabled)")
                                         : QString{}),
                              10 * fwd_, w)
                << factorLine(QStringLiteral("target hears ") + lastHop,
                              QString::number(deliver, 'f', 2),
                              10 * deliver, w)
                << factorLine(QStringLiteral("the reply gets back"),
                              QString::number(back, 'f', 2), 10 * back,
                              w)
                << factorLine(QStringLiteral("target answers at all"),
                              QString::number(ans_, 'f', 2), 10 * ans_,
                              w)
                << factorLine(QStringLiteral("toward the target"),
                              dirUnknown ? QStringLiteral("unknown")
                                         : QString::number(dir, 'f', 2),
                              10 * dir, w, dirUnknown)
                << factorLine(QStringLiteral("route length"),
                              QStringLiteral("%1 hop").arg(hops),
                              10.0 / hops, w);
            ranked.append({m.score, m});
        }
    }

    std::sort(ranked.begin(), ranked.end(),
              [](auto const &a, auto const &b) {
                  return a.score > b.score;
              });

    // [fulltable, operator 2026-08-28: "we need to print everything
    // so you can analyze"] EVERY scored candidate, one line each,
    // rank order -- a station that qualifies but never cracks the
    // printed top three (KT5DC, WB5BNV attempt) is otherwise
    // invisible, and a missed opportunity cannot be seen.
    reachLog(QStringLiteral("    ranking, all %1 candidates:")
                 .arg(ranked.size()));
    for (int i = 0; i < ranked.size(); ++i) {
        auto const &x = ranked.at(i);
        QString route = x.mv.kind;
        if (x.mv.kind == QLatin1String("relay"))
            route = (x.mv.chain.isEmpty()
                         ? x.mv.via
                         : x.mv.chain.join(QLatin1Char('>')))
                    + QLatin1Char('>') + T;
        else if (!x.mv.via.isEmpty())
            route += QLatin1Char(' ') + x.mv.via;
        reachLog(QStringLiteral("      %1. %2  %3/1000s  p=%4 cost=%5s")
                     .arg(i + 1, 2)
                     .arg(route)
                     .arg(x.score * 1000.0, 0, 'f', 4)
                     .arg(x.mv.p, 0, 'f', 3)
                     .arg(x.mv.cost, 0, 'f', 0));
        // Full factor table for the top 8 (operator, 2026-08-28).
        if (i < 8)
            for (QString const &f : x.mv.factors)
                reachLog(QStringLiteral("      ") + f);
    }

    // expected_time (decide.py:705-712)
    auto const expectedTime = [](QVector<Ranked> const &r) {
        double total = 0.0, alive = 1.0;
        for (auto const &x : r) {
            total += alive * x.mv.cost;
            alive *= 1.0 - qMin(0.95, x.score * x.mv.cost);
        }
        return total + alive * 600.0;
    };
    double const nowET = expectedTime(ranked);

    {
        double const trust = bandTrust(m_reach.band, now);
        if (trust < kFwdPrior * 0.9) {
            int n = g_relayOutcomes.value(m_reach.band).size();
            reachLog(QStringLiteral("    band forwarding trust %1 "
                                    "(%2 recent asks) -- booked "
                                    "relays devalued")
                         .arg(trust, 0, 'f', 2)
                         .arg(n));
        }
    }

    // ---- step 1: the direct call, once (decide.py:859-866) --------
    if (!overBudget && snrTried < 0) {
        auto const &m = ranked.isEmpty() || ranked[0].mv.kind
                                != QLatin1String("snr")
            ? [&]() -> MoveCand const & {
                  for (auto const &x : ranked)
                      if (x.mv.kind == QLatin1String("snr"))
                          return x.mv;
                  return ranked[0].mv;
              }()
            : ranked[0].mv;
        m_reach.kind = QStringLiteral("snr");
        m_reach.triedAt.insert(QStringLiteral("snr:") + T, now);
        reachExplain(&m);
        reachSend(QStringLiteral("%1: %2 SNR?").arg(me, T));
        return;
    }

    // ---- step 2: the shout, priced as a COMPOSITE ATTEMPT --------
    // [gateswap 2026-08-27, operator-approved replacement of the
    // python's VOI gate] The ported expected-time-difference formula
    // could never fire at real probability scales (measured: gate
    // score -79 at prior trust, -92 devalued -- the devalue moved it
    // the WRONG way). The shout is an attempt-enabler, so price it in
    // the SAME p/t currency as every attempt: shout (98s) + follow-up
    // ask via whoever answers (218s), with the measured 29% answer
    // rate, the answerer's PROVEN aliveness (it just keyed) and
    // hears-us (it decoded our shout: fresh-report 0.92), delivery
    // pinned at FRESH_LINK, the way home at the comparable-degree
    // reciprocity bucket x unknown-quality (0.311 x 0.7 -- both
    // measured constants from the corpus), and the answerer's forward
    // habit at the BAND TRUST (unknown station, current band). Fires
    // when this composite out-scores the best untried booked relay --
    // so collapsed trust or floor-grade booked routes escalate to
    // asking the band NATURALLY, and a strong fresh booked route
    // keeps the 98 seconds in hand.
    if (!overBudget &&
        !m_reach.triedAt.contains(QStringLiteral("shout"))) {
        double const trust = bandTrust(m_reach.band, now);
        double const qAns = 0.29;              // measured answer rate
        double const backUnknown = 0.311 * 0.7;
        double const pRoute = 0.92 * 0.95 * trust * FRESH_LINK
                              * backUnknown * stAns(T, now);
        double const shoutScore =
            qAns * pRoute / (T_SHOUT + T_RELAY1);
        double bestBooked = 0.0;
        QString bestCall;
        for (auto const &x : ranked) {
            if (x.mv.kind != QLatin1String("relay"))
                continue;
            if (m_reach.triedAt.contains(
                    QStringLiteral("relay:") + x.mv.via))
                continue;
            // [gaterefine2] In restricted mode the baseline must be
            // a SPENDABLE route -- the shout was being outbid by
            // routes the restriction then refused to spend (measured
            // 21:19Z: composite lost to N6CYB 0.0432, which was
            // withheld; the attempt quit without asking the band the
            // warning promised it could).
            if (m_reach.relaysBlocked) {
                QString const lastHop = x.mv.chain.isEmpty()
                                            ? x.mv.via
                                            : x.mv.chain.last();
                bool const fresh =
                    bookEdge(lastHop, T).whenMs >
                        m_reach.gateSilenceMs ||
                    bookEdge(T, lastHop).whenMs >
                        m_reach.gateSilenceMs;
                if (!fresh)
                    continue;
            }
            if (x.score > bestBooked) {
                bestBooked = x.score;
                bestCall = x.mv.via;
            }
        }
        // [shoutfirst, operator-approved 2026-08-28] After a silent
        // direct ask, ask the band BEFORE spending any relay --
        // UNLESS someone in the LIVE hearing store was recently
        // heard hearing the target, ANY source: fresh mqtt evidence
        // is real evidence and skipping the shout then saves the 97 s
        // and the @ALLCALL cooldown. Under JS8_NO_MQTT no mqtt edges
        // exist at all, so the no-internet test gets the radio-only
        // behavior with no source test needed (operator, 2026-08-28).
        // Measured basis: the internet-fed K1BRG run burned 6 relay
        // moves (~1000 s) on STALE book evidence -- its live store
        // held no fresh hearer of the target from any source, so this
        // rule would have shouted there too; the radio-only run
        // shouted on move 2 and nine stations answered in one cycle.
        bool shoutFirst = false;
        if (m_reach.triedAt.contains(QStringLiteral("snr:") + T)) {
            shoutFirst =
                !m_spotMapWindow ||
                m_spotMapWindow->hearersOf(m_reach.band, T).isEmpty();
            if (shoutFirst)
                reachLog(QStringLiteral(
                    "    no recent report of anyone hearing %1 -- "
                    "asking the band before spending relays").arg(T));
        }
        if (shoutFirst || shoutScore > bestBooked) {
            MoveCand m;
            m.kind = QStringLiteral("shout");
            m.cost = T_SHOUT;
            int const w = QStringLiteral(
                "route via an answerer, per 1000s").size();
            m.factors
                << factorLine(QStringLiteral("someone answers this"),
                              QString::number(qAns, 'f', 2),
                              10 * qAns, w)
                << factorLine(
                       QStringLiteral("route via an answerer, "
                                      "per 1000s"),
                       QString::number(shoutScore * 1000.0, 'f', 4),
                       10 * qMin(1.0, shoutScore / qMax(1e-9,
                                                        bestBooked)),
                       w)
                << factorLine(
                       QStringLiteral("best booked relay, per 1000s"),
                       bestCall.isEmpty()
                           ? QStringLiteral("none untried")
                           : QStringLiteral("%1 %2")
                                 .arg(bestCall)
                                 .arg(bestBooked * 1000.0, 0, 'f', 4),
                       10 * qMin(1.0, bestBooked / qMax(1e-9,
                                                        shoutScore)),
                       w)
                << factorLine(QStringLiteral("band forwarding trust"),
                              QString::number(trust, 'f', 2),
                              10 * trust / 0.95, w);
            m_reach.kind = QStringLiteral("shout");
            m_reach.triedAt.insert(QStringLiteral("shout"), now);
            reachExplain(&m);
            reachSend(QStringLiteral("%1: @ALLCALL QUERY CALL %2?")
                          .arg(me, T));
            return;
        }
        reachLog(QStringLiteral("    shout waits: composite %1/1000s "
                                "vs booked %2/1000s (%3, trust %4)")
                     .arg(shoutScore * 1000.0, 0, 'f', 4)
                     .arg(bestBooked * 1000.0, 0, 'f', 4)
                     .arg(bestCall.isEmpty()
                              ? QStringLiteral("none")
                              : bestCall)
                     .arg(trust, 0, 'f', 2));
    }

    // ---- step 3: routes, best first (decide.py:884-887) -----------
    for (auto const &x : ranked) {
        if (x.mv.kind != QLatin1String("relay"))
            continue;
        if (m_reach.relaysBlocked) {
            // restricted: only routes resting on evidence NEWER than
            // the last delivered-silence (down-weighting to zero for
            // the stale book only -- operator: "i thought i
            // suggested down-weighting")
            QString const lastHop = x.mv.chain.isEmpty()
                                        ? x.mv.via
                                        : x.mv.chain.last();
            bool const fresh =
                bookEdge(lastHop, T).whenMs > m_reach.gateSilenceMs ||
                bookEdge(T, lastHop).whenMs > m_reach.gateSilenceMs;
            if (!fresh)
                continue;
            reachLog(QStringLiteral("    restricted target, but %1's "
                                    "route evidence postdates the "
                                    "silence -- spendable")
                         .arg(x.mv.via));
        }
        // [chainid] Full-chain identity, matching the pricing key.
        QString const relKey = x.mv.chain.isEmpty()
                                   ? x.mv.via
                                   : x.mv.chain.join(QLatin1Char('>'));
        qint64 const tried = m_reach.triedAt.value(
            QStringLiteral("relay:") + relKey, -1);
        if (tried >= 0)
            continue;   // stale-scored above; fresh candidates first
        // [shoutspend, operator 2026-08-30: "an early shout can
        // easily return 9 calls, seen it happen, we should try them
        // all"] The budget is hard for everything EXCEPT stations
        // that answered THIS attempt's QUERY CALL shout. Unlike the
        // cut fresh-learned rule (IW2GOB: 36 tx, 28 extensions --
        // ANY edge touching the target qualified, and a busy band
        // replenishes those every slot), the responder set is CLOSED
        // at shout time: YES replies are parsed only while the shout
        // move is live (yesRe gate on m_reach.kind), so the overrun
        // is bounded by the responder count, full stop. A responder
        // self-certified BOTH links a relay needs (it decoded our
        // shout; it claims the target with SNR+age) -- the highest-
        // value evidence in the attempt, paid for with the most
        // expensive move (~97 s); quitting with it untried wastes
        // the purchase. triedAt dedup gives each responder ONE try.
        if (overBudget) {
            QString const lastHop = x.mv.chain.isEmpty()
                                        ? x.mv.via
                                        : x.mv.chain.last();
            bool responder = false;
            for (int i = m_reach.learnedAt0;
                 i < g_book.learned.size(); ++i) {
                auto const &l = g_book.learned.at(i);
                if (l.hearer == lastHop && l.heard == T &&
                    (l.source == QLatin1String("yes-frame1") ||
                     l.source ==
                         QLatin1String("querycall-live"))) {
                    responder = true;
                    break;
                }
            }
            if (!responder)
                continue;
            reachLog(QStringLiteral("    budget spent, but %1 "
                                    "answered the shout -- it gets "
                                    "its try")
                         .arg(lastHop));
        }
        m_reach.kind = QStringLiteral("relay");
        m_reach.via = x.mv.via;
        m_reach.chain = x.mv.chain.isEmpty()
                            ? QStringList{x.mv.via} : x.mv.chain;
        reachComputeExpected();   // [structured]
        m_reach.triedAt.insert(QStringLiteral("relay:") + relKey,
                               now);
        m_reach.pastVias.append(x.mv.via);
        reachExplain(&x.mv);
        // [top3 2026-08-27] Runners-up, compact -- and the best
        // multi-hop candidate even when it loses, so the backward
        // walk's chain-finding is inspectable in every decision
        // (operator: "has the backward-path been evaluated?").
        {
            int shown = 0;
            for (auto const &r : ranked) {
                if (r.mv.kind != QLatin1String("relay") ||
                    r.mv.via == x.mv.via)
                    continue;
                if (shown++ >= 2)
                    break;
                reachLog(QStringLiteral("    also: %1>%2  %3/1000s")
                             .arg(r.mv.chain.join(QLatin1Char('>')),
                                  T)
                             .arg(r.score * 1000.0, 0, 'f', 4));
            }
            double bestMh = 0.0;
            QString bestMhChain;
            for (auto const &r : ranked)
                if (r.mv.chain.size() > 1 && r.score > bestMh) {
                    bestMh = r.score;
                    bestMhChain = r.mv.chain.join(QLatin1Char('>'));
                }
            reachLog(bestMhChain.isEmpty()
                         ? QStringLiteral("    best multi-hop: none "
                                          "found by the walk")
                         : QStringLiteral("    best multi-hop: %1>%2 "
                                          " %3/1000s (not chosen)")
                               .arg(bestMhChain, T)
                               .arg(bestMh * 1000.0, 0, 'f', 4));
        }
        reachSend(QStringLiteral("%1: %2>%3 SNR?")
                      .arg(me, m_reach.chain.join(QLatin1Char('>')),
                           T));
        return;
    }

    // ---- step 4: probes (decide.py:762-777, :889-895) -------------
    if (!overBudget && !m_reach.relaysBlocked) {
        MoveCand bestProbe;
        double bestScore = 0.0;
        // HEARING? at the least-known raisable station
        QString least;
        double lp = 1e9;
        for (QString const &c : g_book.pool) {
            if (m_reach.askedHearing.contains(c))
                continue;
            double const p = pLinkEdge(this, c, T);
            if (p < lp) {
                least = c;
                lp = p;
            }
        }
        if (!least.isEmpty()) {
            QVector<Ranked> boosted = ranked;
            for (auto &x : boosted)
                if (x.mv.via == least)
                    x.score *= 2.0;
            std::sort(boosted.begin(), boosted.end(),
                      [](auto const &a, auto const &b) {
                          return a.score > b.score;
                      });
            double const score =
                0.35 * (nowET - expectedTime(boosted)) - T_HEARING;
            if (score > bestScore) {
                bestScore = score;
                bestProbe.kind = QStringLiteral("hearing");
                bestProbe.via = least;
                bestProbe.cost = T_HEARING;
            }
        }
        // GRID? at the first unlocated of the top 12
        QString unloc;
        for (int i = 0; i < qMin(12, int(g_book.pool.size())); ++i) {
            QString const c = g_book.pool[i];
            if (!m_reach.askedGrid.contains(c) &&
                g_book.stations.value(c).grid.size() < 4) {
                unloc = c;
                break;
            }
        }
        if (!unloc.isEmpty()) {
            QVector<Ranked> boosted = ranked;
            for (auto &x : boosted)
                if (x.mv.via == unloc)
                    x.score *= 1.4;
            std::sort(boosted.begin(), boosted.end(),
                      [](auto const &a, auto const &b) {
                          return a.score > b.score;
                      });
            double const score =
                0.5 * (nowET - expectedTime(boosted)) - T_GRID;
            if (score > bestScore) {
                bestScore = score;
                bestProbe.kind = QStringLiteral("grid");
                bestProbe.via = unloc;
                bestProbe.cost = T_GRID;
            }
        }
        if (bestScore > 0) {
            m_reach.kind = bestProbe.kind;
            m_reach.via = bestProbe.via;
            if (bestProbe.kind == QLatin1String("hearing")) {
                m_reach.askedHearing.append(bestProbe.via);
                reachSend(QStringLiteral("%1: %2 HEARING?")
                              .arg(me, bestProbe.via));
            } else {
                m_reach.askedGrid.append(bestProbe.via);
                reachSend(QStringLiteral("%1: %2 GRID?")
                              .arg(me, bestProbe.via));
            }
            return;
        }
    }

    // ---- nothing left (decide.py:896-901) -------------------------
    if (m_reach.relaysBlocked) {
        // [withheldviz] The board you cannot spend is still a board
        // you can read (operator: "wasn't high enough ranking, or
        // not considered?" -- it was ranked and withheld, invisibly).
        double wBest = 0.0;
        QString wCall;
        qint64 wWhen = 0;
        for (auto const &x : ranked) {
            if (x.mv.kind != QLatin1String("relay") ||
                x.score <= wBest)
                continue;
            QString const lastHop = x.mv.chain.isEmpty()
                                        ? x.mv.via
                                        : x.mv.chain.last();
            wBest = x.score;
            wCall = x.mv.via;
            wWhen = qMax(bookEdge(lastHop, m_reach.target).whenMs,
                         bookEdge(m_reach.target, lastHop).whenMs);
        }
        if (!wCall.isEmpty())
            // [zerotime] wWhen == 0 is a PAIR THE BOOK NEVER SAW
            // (bookEdge returns a default edge), not an old one --
            // the subtraction printed "496697.2h older" (the age of
            // the Unix epoch, field 2026-08-30). Scoring was never
            // affected: pLinkRaw guards whenMs<=0 with kUnseenLink,
            // and every other consumer tests whenMs>0 first.
            reachLog(
                wWhen > 0
                    ? QStringLiteral("    withheld best: %1  %2/1000s "
                                     "(target-side evidence %3h "
                                     "older than the silence)")
                          .arg(wCall)
                          .arg(wBest * 1000.0, 0, 'f', 4)
                          .arg((m_reach.gateSilenceMs - wWhen)
                                   / 3600000.0, 0, 'f', 1)
                    : QStringLiteral("    withheld best: %1  %2/1000s "
                                     "(no target-side evidence in "
                                     "the book at all)")
                          .arg(wCall)
                          .arg(wBest * 1000.0, 0, 'f', 4));
        reachStop(QStringLiteral("direct call unanswered; delivery to "
                                 "%1 was already proven, no station "
                                 "has heard it since, and no route "
                                 "carries newer evidence -- retry "
                                 "when it is heard transmitting")
                      .arg(m_reach.target));
        return;
    }
    reachStop(overBudget && m_reach.silentDeliveries >= 2
                  ? QStringLiteral("NOT REACHED -- delivery proven "
                                   "%1 times with no answer; the "
                                   "target is not answering, further "
                                   "relays add nothing")
                        .arg(m_reach.silentDeliveries)
              : overBudget
                  ? QStringLiteral("NOT REACHED -- move budget "
                                   "spent; busy or disabled, retry "
                                   "from the top later")
                  : QStringLiteral("nothing left to try -- every "
                                   "option is spent; busy or "
                                   "disabled, retry from the top "
                                   "later"));
}

// explain() (decide.py:263-291): COST line from waits_for
// (decide.py:184-214, display-only) + the factor table.
void UI_Constructor::reachExplain(void const *cand) {
    auto const &m = *static_cast<MoveCand const *>(cand);
    double const period = JS8::Submode::periodMS(m_nSubMode) / 1000.0;
    double const check = period + 3, escalate = period + 6;
    double const abandon =
        (m.kind == QLatin1String("shout") ? 20 : 16) * period;
    reachLog(QStringLiteral("  COST   %1 s   check +%2s / escalate "
                            "+%3s / abandon +%4s%5")
                 .arg(m.cost, 0, 'f', 0)
                 .arg(check, 0, 'f', 1)
                 .arg(escalate, 0, 'f', 1)
                 .arg(abandon, 0, 'f', 1)
                 .arg(m.kind == QLatin1String("relay")
                          ? QStringLiteral("   (via keying in the "
                                           "first slot is the "
                                           "checkpoint)")
                          : QString{}));
    for (auto const &f : m.factors)
        reachLog(f);
}

void UI_Constructor::reachSend(QString const &wire) {
    m_reach.moveNo += 1;
    m_reach.lastWire = wire;
    // [autoroute] Path counter on the owned Last Tx label.
    if (m_autoRouteActive)
        // [operator 2026-08-30] "... Path #x/6" -- show the budget
        // so the operator can see how much attempt remains. A
        // shout-responder overrun past the budget reads honestly
        // as e.g. #7/6.
        last_tx_label.setText(
            QStringLiteral("Auto-route: Path #%1/%2")
                .arg(m_reach.moveNo)
                .arg(m_reach.maxMoves));
    m_reach.moveCapMs = DriftingDateTime::currentMSecsSinceEpoch()
                        + kMoveCapMs;   // attempt.py:320-321
    reachLog(QStringLiteral("[%1] SEND %2")
                 .arg(m_reach.moveNo).arg(wire));
    enqueueMessage(PriorityHigh, wire, -1, nullptr);
    m_reach.sent += 1;
    processTxQueue();
}

void UI_Constructor::reachOnTxComplete() {
    if (!m_reach.active || m_reach.txEndMs != 0)
        return;
    m_reach.txEndMs = DriftingDateTime::currentMSecsSinceEpoch();
    // [twoslot 2026-08-30, operator: "build the 2-slot wait after
    // the SNR? and we'll see if it works better"] The 1-slot verdict
    // collided with second-opportunity repliers: verdict at
    // slot-end+1s, move 2 keyed at the NEXT boundary -- exactly the
    // boundary a station keys when its decode+enqueue missed the
    // first one. Half-duplex ate the answer AND journaled a false
    // ans=false (self-inflicted habit contamination). Direct pings
    // [structured] Start wait: 2 slots when the band is busy, 1
    // when quiet -- one of the TWO busy-conditional points (operator
    // design 2026-08-31). [#207 waitopts] reachBandBusy() now rules
    // by the operator's Short/Adaptive/Long setting.
    m_reach.deadlineMs =
        reachSlotEndMs(m_reach.txEndMs, reachBandBusy() ? 2 : 1);
    reachLog(QStringLiteral("    TX-END; deadline %1")
                 .arg(fmtClock(m_reach.deadlineMs)));
    reachArmTimer();
}

void UI_Constructor::reachOnFrame(ActivityDetail const &d) {
    if (!m_reach.active || m_reach.txEndMs == 0)
        return;
    qint64 const now = DriftingDateTime::currentMSecsSinceEpoch();
    QString const up = d.text.trimmed().toUpper();
    QString const me = m_config.my_callsign().trimmed().toUpper();
    QString const T = m_reach.target;

    // [operator's rule, restated & confirmed 2026-08-28] A relay move
    // has exactly two cheap checkpoints, both "did the via key" --
    // callsign or "<....>" slash-callsign placeholder in frame 1, any
    // addressee. No addressee matching, no packed-count arithmetic
    // (the 2026-08-27 detectors misread WO7I's heartbeat ACK to a
    // third station as the answer coming back). Identified by
    // CALLSIGN, not offset: WO7I forwarded at 1104 Hz after being
    // heard only at 754/803 Hz (HB sub-band) -- relays answer on
    // their own chosen offset.
    //
    //   1. keying in the first slot (this block): wait for the
    //      forward to finish (5 slots, the measured forward span);
    //      never keys -> the one-slot deadline ends the move. This
    //      handler only runs while the move is alive, so no explicit
    //      slot test is needed.
    //   2. the return, when expected (armed at forward-complete):
    //      must START within 6 slots of the forward's end; a started
    //      return slides one slot per arriving frame on its offset.
    //   Everything bounded by the move cap. A false keying fire costs
    //   wait only -- both habit writes key on the checksum-proven
    //   assembly.
    bool const viaKeyed =
        up.startsWith(m_reach.via + QLatin1String(":")) ||
        up.startsWith(QStringLiteral("<....>:"));
    if (m_reach.kind == QLatin1String("relay") &&
        m_reach.fwdStartedMs == 0 && viaKeyed) {
        m_reach.fwdStartedMs = now;
        m_reach.fwdFrames = 1;
        m_reach.fwdOffset = d.offset;
        // [structured, operator rule: expected length, no pad]
        // Frame 1 in hand; the packed count says how many more.
        int const more = qMax(1, m_reach.fwdExpFrames - 1);
        m_reach.deadlineMs = qMax(m_reach.deadlineMs,
                                  reachSlotEndMs(now, more));
        reachLog(QStringLiteral("    %1 keyed in the first slot +%2s "
                                "-- waiting for the forward to finish"
                                " (%3 frames expected)")
                     .arg(m_reach.via)
                     .arg((now - m_reach.txEndMs) / 1000)
                     .arg(m_reach.fwdExpFrames));
        reachArmTimer();
    } else if (m_reach.kind == QLatin1String("relay") &&
               m_reach.fwdStartedMs != 0 && m_reach.fwdDoneMs == 0 &&
               m_reach.forcedVerdict.isEmpty() &&
               m_reach.fwdOffset != 0 &&
               qAbs(d.offset - m_reach.fwdOffset) <= 5) {
        // [flagcheck, operator-ruled item 1, 2026-08-31] Message
        // endings are announced in-band (bit 73). Our forward's
        // final frame MUST carry the Last flag; a Last flag early,
        // or missing at the expected final frame, or a frame past
        // the count, proves this is not our message -- at that
        // frame's decode. Reads one protocol bit, never content.
        m_reach.fwdFrames += 1;
        bool const lastFlag =
            (d.bits & Varicode::JS8CallLast) != 0;
        int const expN = m_reach.fwdExpFrames;
        if (expN > 0 &&
            (m_reach.fwdFrames > expN ||
             (m_reach.fwdFrames == expN && !lastFlag) ||
             (m_reach.fwdFrames < expN && lastFlag))) {
            reachLog(QStringLiteral("    frame %1 of expected %2 "
                                    "%3 -- not our forward")
                         .arg(m_reach.fwdFrames).arg(expN)
                         .arg(m_reach.fwdFrames > expN
                                  ? QStringLiteral("exceeds it")
                              : lastFlag
                                  ? QStringLiteral("ends early")
                                  : QStringLiteral("lacks the "
                                        "last-frame flag")));
            m_reach.forcedVerdict =
                QStringLiteral("the relaying station keyed, but no "
                               "forward of ours assembled");
            m_reach.deadlineMs = now;
        }
        reachArmTimer();
    }
    // Checkpoint 2 fires here: the via keying again after its proven
    // forward = the return starting. LEDGER NAMES THE CASE (operator,
    // 2026-08-28): started-and-slid or the verdict says no-return /
    // never-assembled explicitly.
    if (m_reach.kind == QLatin1String("relay") &&
        m_reach.fwdDoneMs != 0 && m_reach.retStartedMs == 0 &&
        viaKeyed) {
        // [structured] During the answer silence the return cannot
        // physically start (the target is still transmitting to the
        // via, two hops away) -- a via keying there is doing
        // something else; note it and keep waiting. Single-relay
        // only; multi-hop keeps the old accept-any-time behavior.
        if (m_reach.chain.size() <= 1 && m_reach.ansExpFrames > 0 &&
            now < reachSlotEndMs(m_reach.fwdDoneMs,
                                 m_reach.ansExpFrames) -
                      qMax(1u, JS8::Submode::periodMS(m_nSubMode)) / 2) {
            reachLog(QStringLiteral("    %1 keyed during the answer "
                                    "silence -- not the return, "
                                    "still waiting")
                         .arg(m_reach.via));
            return;
        }
        m_reach.retStartedMs = now;
        m_reach.retOffset = d.offset;
        // [structured] completion: computed length when quiet; the
        // per-frame slide is the busy accommodation (one of the two
        // busy-conditional points). Stub is TRUE today.
        m_reach.deadlineMs = qMax(
            m_reach.deadlineMs,
            reachSlotEndMs(now,
                           reachBandBusy()
                               ? 1
                               : qMax(1, m_reach.retExpFrames - 1)));
        reachLog(QStringLiteral("    return started +%1s at %2 Hz -- "
                                "waiting for end of message "
                                "(%3 frames expected)")
                     .arg((now - m_reach.txEndMs) / 1000)
                     .arg(d.offset)
                     .arg(m_reach.retExpFrames));
        reachArmTimer();
    } else if (m_reach.kind == QLatin1String("relay") &&
               m_reach.retStartedMs != 0 &&
               qAbs(d.offset - m_reach.retOffset) <= 5) {
        // continuation frames of the return carry no callsign prefix;
        // ride the offset. [structured] The per-frame SLIDE is the
        // busy-band accommodation only. [retflag 2026-09-03,
        // operator-approved] Quiet completion no longer trusts the
        // computed count alone: the count is a PROXY (placeholder
        // SNR + template checksum through variable-width coding =
        // +/-1 frame at packing boundaries -- the KI6WDY case,
        // where our next ask transmitted over W4CAT's 4th frame).
        // Bit 73 is the in-band truth: a frame decoding WITHOUT the
        // Last flag announces at least one more frame, so extend
        // one slot per flagless frame -- evidence, not a pad. A
        // frame WITH the flag extends nothing (assembly happens in
        // its own decode pass).
        if (reachBandBusy()) {
            m_reach.deadlineMs = qMax(m_reach.deadlineMs,
                                      reachSlotEndMs(now, 1));
        } else if (!(d.bits & Varicode::JS8CallLast)) {
            qint64 const ext = reachSlotEndMs(now, 1);
            if (ext > m_reach.deadlineMs) {
                m_reach.deadlineMs = ext;
                reachLog(QStringLiteral(
                    "    return frame without the Last flag -- at "
                    "least one more coming, extending 1 slot"));
            }
        }
        reachArmTimer();
    }

    bool addressed = false;
    if (m_reach.kind == QLatin1String("shout")) {
        static QRegularExpression const re(
            QStringLiteral("^[A-Z0-9/]+:\\s*%1\\b")
                .arg(QRegularExpression::escape(me)));
        addressed = re.match(up).hasMatch();
    } else if (m_reach.kind == QLatin1String("hearing") ||
               m_reach.kind == QLatin1String("grid")) {
        addressed = up.startsWith(m_reach.via + QLatin1String(":"));
    } else {
        addressed = up.startsWith(T + QLatin1String(":"));
    }

    auto keyFor = [this](int off) {
        for (auto it = m_reach.watchers.begin();
             it != m_reach.watchers.end(); ++it)
            if (qAbs(it.key() - off) <= 5)
                return it.key();
        return off;
    };
    int const k = keyFor(d.offset);
    if (m_reach.watchers.contains(k) && m_reach.watchers[k].dead)
        return;   // dead stays dead
    if (addressed) {
        if (!m_reach.watchers.contains(k)) {
            reachLog(QStringLiteral("    reply started at %1 Hz: %2")
                         .arg(k).arg(up.left(40)));
            // Frame-1 YES learning (attempt.py:381-384, the N6GRG
            // lesson): the claim rides in frame 1; a died reply still
            // teaches. Pinned at FRESH_LINK via a marker edge.
            static QRegularExpression const yes(
                QStringLiteral(":\\s*%1\\s+YES\\b")
                    .arg(QRegularExpression::escape(me)));
            if (m_reach.kind == QLatin1String("shout") &&
                yes.match(up).hasMatch()) {
                QString const who =
                    up.section(QLatin1Char(':'), 0, 0).trimmed();
                bookAddEdge(who, T, now, -99,
                            QStringLiteral("yes-frame1"));
                g_book.learned.append(
                    {who, T, QStringLiteral("yes-frame1"), now, -99});
            }
        }
        auto &w = m_reach.watchers[k];
        w.lastMs = now;
        if (m_reach.deadlineMs)
            m_reach.deadlineMs = qMax(m_reach.deadlineMs,
                                      reachSlotEndMs(now, 1));
        if (m_reach.ansStartedMs == 0) {
            m_reach.ansStartedMs = now;
            int frames;
            if (m_reach.kind == QLatin1String("shout"))
                frames = kFramesShout;   // variable riders; measured max
            else if (m_reach.kind == QLatin1String("hearing"))
                frames = kFramesHearing; // list length varies
            else if (m_reach.kind == QLatin1String("grid"))
                frames = reachReplyFrames(
                    m_reach.via,
                    me + QStringLiteral(" GRID FN20"));
            else if (m_reach.kind == QLatin1String("relay"))
                frames = reachReplyFrames(
                    T, m_reach.via + QStringLiteral("> ") + me
                           + QStringLiteral(" SNR -15"));
            else
                frames = reachReplyFrames(
                    T, me + QStringLiteral(" SNR -15"));
            m_reach.deadlineMs = qMax(m_reach.deadlineMs,
                                      reachSlotEndMs(now, frames - 1));
            reachLog(QStringLiteral("    answer STARTED +%1s -- "
                                    "deadline extended")
                         .arg((now - m_reach.txEndMs) / 1000));
        }
        reachArmTimer();
    } else if (m_reach.watchers.contains(k) &&
               !m_reach.watchers[k].done) {
        m_reach.watchers[k].lastMs = now;
        if (m_reach.deadlineMs)
            m_reach.deadlineMs = qMax(m_reach.deadlineMs,
                                      reachSlotEndMs(now, 1));
        reachArmTimer();
    }
}

// Assembled directed messages. NO tx-end guard (audit item 12): an
// answer decoded before TX.COMPLETE still counts, python verbatim.
void UI_Constructor::reachOnDirected(CommandDetail const &d,
                                     QString const &line) {
    if (!m_reach.active)
        return;
    qint64 const now = DriftingDateTime::currentMSecsSinceEpoch();
    QString const from = d.from.trimmed().toUpper();
    QString const T = m_reach.target;
    QString const me = m_config.my_callsign().trimmed().toUpper();
    QString const off =
        m_reach.txEndMs
            ? QStringLiteral("+%1s")
                  .arg((now - m_reach.txEndMs) / 1000)
            : QString{};

    for (auto it = m_reach.watchers.begin();
         it != m_reach.watchers.end(); ++it) {
        if (qAbs(it.key() - d.offset) <= 5)
            it.value().done = true;
    }

    // Late forward from a PAST via (attempt.py:422-428): hold TX two
    // slots for its reply -- the K4GMX +104 s lesson.
    if (m_reach.pastVias.contains(from) &&
        from != m_reach.via &&
        line.toUpper().contains(QStringLiteral("*DE* ") + me)) {
        m_reach.holdUntilMs = now + 30000;
        reachLog(QStringLiteral("    LATE FORWARD from %1: holding TX "
                                "two slots for its reply").arg(from));
    }

    // The relay-returned answer: "VIA: WM8Q> SNR -x *DE* TARGET" --
    // from the via (or a past via), carrying *DE* target, addressed
    // to us. This IS the target's answer arriving; the from==target
    // gate alone missed it (the python had the same gap -- every
    // prior success was a directly-audible target).
    bool const viaReturn =
        (from == m_reach.via || m_reach.pastVias.contains(from)) &&
        line.toUpper().contains(QStringLiteral("*DE* ") + T) &&
        Radio::same_station(d.to.trimmed().toUpper()
                                .section(QLatin1Char('>'), 0, 0), me);
    if (viaReturn ||
        (Radio::same_station(from, T) &&
         Radio::same_station(d.to.trimmed().toUpper()
                                 .section(QLatin1Char('>'), 0, 0),
                             me))) {
        qint64 const total = (now - m_reach.startMs) / 1000;
        reachLog(QStringLiteral("ANSWER %1: %2")
                     .arg(off, line.left(70)));
        reachLog(QStringLiteral("REACHED %1 on move %2, %3s total, "
                                "%4 transmissions")
                     .arg(T).arg(m_reach.moveNo).arg(total)
                     .arg(m_reach.sent));
        QStringList path;
        if (m_reach.kind == QLatin1String("relay"))
            path = m_reach.chain;
        else if (viaReturn)
            path << from;
        path << T;
        // [habitstore + TODO #182] answered = the strongest habit
        // fact; the route that WORKED, saved with it.
        g_ansOutcomes[m_reach.band].append({now, true, T});
        m_spotMapWindow->queueReachEvent(
            {m_reach.band, T, QStringLiteral("ans"), QString{}, 0,
             true});
        // [chaincredit 2026-08-30] The reply arriving through a
        // relay chain is checksum-grade PROOF that every hop
        // forwarded -- exact evidence, previously discarded for
        // hops beyond our decode range (the *DE* observation only
        // credits relays we HEAR). Journal a forward outcome for
        // each chain member: fwd habit stays honest on proven
        // routes, and far hops earn the known-forwarder green ring
        // and its kFwdPrior ranking floor.
        if (m_reach.kind == QLatin1String("relay"))
            for (QString const &hop : m_reach.chain) {
                g_relayOutcomes[m_reach.band].append(
                    {now, true, hop});
                m_spotMapWindow->queueReachEvent(
                    {m_reach.band, hop, QStringLiteral("fwd"),
                     QString{}, 0, true});
            }
        m_spotMapWindow->queueReachEvent(
            {m_reach.band, T, QStringLiteral("reached"),
             path.join(QLatin1Char('>')), 0, true});
        // [TODO #177, operator 2026-08-24 "nobody cares"] The reply
        // arriving IS the acknowledgement; mark this exact message so
        // the relay-ACK composer later in processCommandActivity
        // skips it. One-shot, exact from+text identity -- no timers.
        m_reachAnsweredFrom = d.from;
        m_reachAnsweredText = d.text;
        reachPlaceTemplate(path);
        reachStop(QStringLiteral("REACHED"));
        return;
    }
    if (m_reach.kind == QLatin1String("relay") &&
        from == m_reach.via &&
        line.toUpper().contains(QStringLiteral("*DE* ") + me)) {
        m_reach.fwdDoneMs = now;
        // Checksum-proven forward: THE fact both habit writes key on
        // (the delivered-silent negative at verdict time uses this
        // same timestamp). The DURABLE forwarded-ok journal is NOT
        // written here: the *DE* observation in processCommandActivity
        // journals every forward of our traffic, executor or manual --
        // one authority, no duplicate rows.
        g_relayOutcomes[m_reach.band].append({now, true, m_reach.via});
        // [operator's return rule, restated & confirmed 2026-08-28]
        // The return must START within 6 slots PER RELAY in the
        // chain: for the nearest relay that is 3 frames of answer
        // plus the 3-frame start window; every relay beyond it adds
        // one 3-frame forward and one 3-frame return, all beyond our
        // hearing (only hop 1's transmissions are guaranteed
        // audible). Constants, not packed counts -- the SNR ask's
        // reply shape is fixed. Nothing by then = abandon there, not
        // at the cap; the cap is only the outer bound.
        // [structured, operator design 2026-08-31] Single relay:
        // the window has structure -- the target's answer occupies
        // ansExpFrames slots of planned silence (nothing can happen
        // there), then the return must START within the start
        // allowance. [operator 2026-09-01, KO4JJW run] That
        // allowance is the THIRD busy-conditional point: 1 slot
        // when quiet (the field REACHEDs all started there), 2
        // when busy. Multi-hop chains keep the flat 6 slots per
        // relay until that decomposition is designed.
        int const retWindow =
            m_reach.chain.size() <= 1
                ? qMax(1, m_reach.ansExpFrames) +
                      (reachBandBusy() ? 2 : 1)
                : 6 * static_cast<int>(m_reach.chain.size());
        m_reach.retWindowSlots = retWindow;
        m_reach.deadlineMs = qMax(m_reach.deadlineMs,
                                  reachSlotEndMs(now, retWindow));
        reachLog(QStringLiteral("    forward complete %1 "
                                "(checksum-proven); %2 slots of "
                                "answer silence, return must start "
                                "within %3 slots total")
                     .arg(off)
                     .arg(m_reach.chain.size() <= 1
                              ? m_reach.ansExpFrames : -1)
                     .arg(retWindow));
        reachArmTimer();
        return;
    }
    // YES learned: update the ROUTE BOOK live (the python's told_us;
    // the store gets it via bindCallQueryReply on the same pass).
    static QRegularExpression const yesRe(
        QStringLiteral(":\\s*%1\\s+YES(?:\\s+([+-]\\d+)\\s*\\((\\d+)"
                       "([SMHD]))?")
            .arg(QRegularExpression::escape(
                m_config.my_callsign().trimmed().toUpper())));
    auto const ym = yesRe.match(line.toUpper());
    if (m_reach.kind == QLatin1String("shout") && ym.hasMatch()) {
        qint64 whenMs = now;
        int snr = -99;
        if (!ym.captured(1).isEmpty()) {
            snr = ym.captured(1).toInt();
            qint64 const n = ym.captured(2).toLongLong();
            QChar const u = ym.captured(3).isEmpty()
                                ? QLatin1Char('S')
                                : ym.captured(3).at(0);
            qint64 const secs = u == QLatin1Char('D') ? n * 86400
                                : u == QLatin1Char('H') ? n * 3600
                                : u == QLatin1Char('M') ? n * 60 : n;
            whenMs = now - secs * 1000;
        }
        bookAddEdge(from, T, whenMs, snr,
                    QStringLiteral("querycall-live"));
        g_book.learned.append(
            {from, T, QStringLiteral("querycall-live"), whenMs, snr});
        reachLog(QStringLiteral("    learned %1: %2")
                     .arg(off, line.left(60)));
    }
}

void UI_Constructor::reachArmTimer() {
    if (!m_reach.active || !m_reachTimer)
        return;
    qint64 const now = DriftingDateTime::currentMSecsSinceEpoch();
    if (m_reach.deadlineMs == 0) {
        // waiting for TX-END; the hard cap still guards the move
        if (m_reach.moveCapMs)
            m_reachTimer->start(
                qMax<qint64>(5, m_reach.moveCapMs - now));
        return;
    }
    qint64 next = qMin(m_reach.deadlineMs,
                       m_reach.moveCapMs ? m_reach.moveCapMs
                                         : m_reach.deadlineMs);
    for (auto const &w : m_reach.watchers) {
        if (w.done || w.dead)
            continue;
        qint64 const death = reachSlotEndMs(w.lastMs, 1);
        if (death > now)
            next = qMin(next, death);
    }
    m_reachTimer->start(qMax<qint64>(5, next - now));
}

void UI_Constructor::reachTick() {
    if (!m_reach.active)
        return;
    qint64 const now = DriftingDateTime::currentMSecsSinceEpoch();

    // hold expiry re-enters the move chooser
    if (m_reach.holdUntilMs && now >= m_reach.holdUntilMs &&
        m_reach.txEndMs == 0 && m_reach.kind.isEmpty()) {
        m_reach.holdUntilMs = 0;
        reachNextMove();
        return;
    }

    // per-move hard cap (attempt.py:320-321): the ONLY absolute bound
    bool const capped =
        m_reach.moveCapMs && now >= m_reach.moveCapMs;

    if (!capped) {
        if (m_reach.txEndMs == 0) {
            reachArmTimer();
            return;
        }
        if (m_reach.ansStartedMs && !m_reach.watchers.isEmpty() &&
            now < m_reach.deadlineMs) {
            bool allSettled = true;
            int done = 0;
            for (auto it = m_reach.watchers.begin();
                 it != m_reach.watchers.end(); ++it) {
                auto &w = it.value();
                if (w.done) { ++done; continue; }
                if (!w.dead && now >= reachSlotEndMs(w.lastMs, 1)) {
                    w.dead = true;
                    reachLog(QStringLiteral("    reply at %1 Hz died "
                                            "(missing frame)")
                                 .arg(it.key()));
                }
                if (!w.dead) {
                    allSettled = false;
                    break;
                }
            }
            if (allSettled) {
                reachLog(QStringLiteral("    all %1 started replies "
                                        "finished (%2 assembled, %3 "
                                        "died) -- shortcutting %4s")
                             .arg(m_reach.watchers.size()).arg(done)
                             .arg(m_reach.watchers.size() - done)
                             .arg((m_reach.deadlineMs - now) / 1000));
                m_reach.deadlineMs = now;
            }
        }
        if (now < m_reach.deadlineMs) {
            reachArmTimer();
            return;
        }
    }

    // pre-verdict tally (attempt.py:501-505)
    if (!m_reach.watchers.isEmpty()) {
        int done = 0;
        for (auto const &w : m_reach.watchers)
            if (w.done)
                ++done;
        reachLog(QStringLiteral("    replies: %1 started, %2 "
                                "assembled, %3 died")
                     .arg(m_reach.watchers.size()).arg(done)
                     .arg(m_reach.watchers.size() - done));
    }
    QString state;
    if (!m_reach.forcedVerdict.isEmpty())
        state = m_reach.forcedVerdict;
    // [operator-caught session 2026-08-27, found during the detector
    // audit] The durable write below ran on EVERY verdict -- missing
    // braces -- journaling a false did-not-forward mark for vias that
    // HAD forwarded, and empty-station rows for direct moves.
    if (m_reach.kind == QLatin1String("relay") &&
        m_reach.fwdStartedMs == 0) {
        g_relayOutcomes[m_reach.band].append(
            {DriftingDateTime::currentMSecsSinceEpoch(), false,
             m_reach.via});
        m_spotMapWindow->queueReachEvent(
            {m_reach.band, m_reach.via, QStringLiteral("fwd"),
             QString{}, 0, false});
    }
    if (!state.isEmpty())
        ;   // forced verdict already named the case
    else if (capped)
        state = QStringLiteral("move hard cap (330s) reached");
    else if (m_reach.kind == QLatin1String("relay") &&
             m_reach.fwdStartedMs == 0)
        state = QStringLiteral("relay silent -- never keyed");
    else if (m_reach.fwdDoneMs != 0 && m_reach.retStartedMs != 0)
        state = QStringLiteral("return started but never assembled "
                               "-- frames lost");
    else if (m_reach.fwdDoneMs != 0)
        state = QStringLiteral("forward delivered; no return started "
                               "within the %1-slot window -- target "
                               "silent")
                    .arg(m_reach.retWindowSlots > 0
                             ? m_reach.retWindowSlots
                             : 6 * static_cast<int>(
                                   m_reach.chain.size()));
    else if (m_reach.ansStartedMs != 0) {
        bool allDone = !m_reach.watchers.isEmpty();
        for (auto const &w : m_reach.watchers)
            if (!w.done)
                allDone = false;
        state = allDone
            ? QStringLiteral("every started reply assembled -- the "
                             "target itself never answered")
            : QStringLiteral("answer started but never assembled -- "
                             "frames lost");
    } else if (m_reach.kind == QLatin1String("relay"))
        // past "relay silent" (it keyed) and past fwdDone (nothing
        // assembled): the one-checkpoint wait ran dry.
        state = QStringLiteral("the relaying station keyed, but no "
                               "forward of ours assembled");
    else if (m_reach.kind == QLatin1String("shout"))
        state = QStringLiteral("the whole group is busy or disabled");
    else
        state = (m_reach.kind == QLatin1String("relay")
                     ? m_reach.via
                 : m_reach.kind == QLatin1String("hearing") ||
                         m_reach.kind == QLatin1String("grid")
                     ? m_reach.via
                     : m_reach.target)
                + QStringLiteral(" is busy or disabled");
    // [habitstore] the target's answer habit, observed: a direct
    // call that went unanswered; a relayed ask whose delivery was
    // PROVEN (forward complete) with no answer -- the strong form.
    if (m_reach.kind == QLatin1String("snr")) {
        g_ansOutcomes[m_reach.band].append(
            {now, false, m_reach.target});
        m_spotMapWindow->queueReachEvent(
            {m_reach.band, m_reach.target, QStringLiteral("ans"),
             QString{}, 0, false});
    } else if (m_reach.kind == QLatin1String("relay") &&
               m_reach.fwdDoneMs != 0) {
        g_ansOutcomes[m_reach.band].append(
            {now, false, m_reach.target});
        m_spotMapWindow->queueReachEvent(
            {m_reach.band, m_reach.target, QStringLiteral("ans"),
             QStringLiteral("delivered"), 0, false});
        // [silentcut] attempt-scope tally; see the extension gate.
        m_reach.silentDeliveries += 1;
    }
    reachLog(QStringLiteral("    VERDICT +%1s: %2 -- next move")
                 .arg(m_reach.txEndMs
                          ? (now - m_reach.txEndMs) / 1000
                          : 0)
                 .arg(state));
    if (m_spotMapWindow)
        m_spotMapWindow->clearAttempts();
    reachNextMove();
}

void UI_Constructor::reachPlaceTemplate(QStringList const &path) {
    clearCallsignSelected();
    QString tpl = path.join(QLatin1Char('>'))
                  + QStringLiteral(" [MESSAGE]");
    // [operator 2026-08-30] Teach the reached station the return
    // path, single-relay successes only (deeper chains unlikely to
    // carry a reply; direct contacts need no instruction). Left
    // unhighlighted on purpose: the operator DELETES it when no
    // reply is expected, so its presence is a meaningful request.
    if (path.size() == 2)
        tpl += QStringLiteral(" REPLY TO \"%1>%2\"")
                   .arg(path.first(), m_config.my_callsign());
    ui->extFreeTextMsgEdit->setPlainText(tpl);
    QTextCursor c = ui->extFreeTextMsgEdit->textCursor();
    if (int const at = tpl.indexOf(QStringLiteral("[MESSAGE]"));
        at >= 0) {
        c.setPosition(at);
        c.setPosition(at + 9, QTextCursor::KeepAnchor);
    } else {
        c.movePosition(QTextCursor::End);
    }
    ui->extFreeTextMsgEdit->setTextCursor(c);
    reachLog(QStringLiteral("template ready in the outgoing box: %1")
                 .arg(tpl));
}
