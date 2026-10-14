#ifndef STATION_MONITOR_WINDOW_HPP__
#define STATION_MONITOR_WINDOW_HPP__

/**
 * @file StationMonitorWindow.h
 * @brief [stamon, TODO #210] Debug window following ONE station's
 * conversations: every assembled directed message whose parties
 * connect to the station -- directly, through a relay path, by
 * callsign mention, or by transmitting at/near a member's offset.
 * The member set GROWS as the conversation graph is discovered.
 *
 * Standard feature since Build 465 (env gate removed): entries on
 * the call activity list, waterfall, Spots Map, and Window menu.
 * One window per station; size persists, position does not.
 */

#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QWidget>

#include <functional>

class QLabel;
class QTextBrowser;

class StationMonitorWindow final : public QWidget {
    Q_OBJECT

  signals:
    // [operator 2026-09-03] Callsigns in a line act as buttons:
    // single click selects that call in the main window; double
    // click also QSYs to its offset (>1000 Hz rule).
    void callClicked(QString const &call);
    void callDoubleClicked(QString const &call);
    // [TODO #214] Right-click on a linked call -> "Open station
    // monitor" for it.
    void openMonitorRequested(QString const &call);

  public:
    // Backfill sources: DIRECTED.TXT (RX, assembled) and ALL.TXT
    // (our own per-frame "Transmitting" lines, stitched into
    // messages and credited to myCall).
    StationMonitorWindow(class QSettings *settings,
                         QString const &station,
                         QString const &directedTxtPath,
                         QString const &allTxtPath,
                         QString const &myCall,
                         QWidget *parent = nullptr);

    QString station() const { return m_station; }

    // [operator 2026-09-01] Honor the main window's View-menu
    // "Show Band Heartbeats and ACKs" toggle: when it says hide,
    // heartbeat-related lines are skipped (checked per line, so
    // the toggle applies from that moment on). Membership math
    // still runs on skipped lines.
    void setShowHbProbe(std::function<bool()> fn) {
        m_showHb = std::move(fn);
    }

    // One assembled directed message (the composed "FROM: TO ..."
    // line). historical = a backfilled DIRECTED.TXT line: its mode
    // letter is unknown ("?") but membership math runs identically.
    void feed(QString const &from, QString const &to,
              QString const &relayPath, QString const &text,
              int offset, QDateTime const &utc, int submode,
              bool historical = false);

    // Called by the owner AFTER the probes are installed (backfill
    // must see the HB filter).
    void runBackfill();
    // [JTTY, operator 2026-10-13] Historical lines supplied by the
    // caller (JTTY.TXT in JTTY mode), parties already resolved; fed as
    // backfill in time order, then scrolled to the end.
    struct Historic {
        QDateTime utc;
        QString from;
        QString to;
        QString text;
        int offset;
        int submode;
    };
    void runBackfill(QList<Historic> const &lines);

  protected:
    void resizeEvent(class QResizeEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

  private:
    void updateHeadline();
    void refreshTitle();
    class QSettings *m_settings;

    QString m_station;
    QColor m_tint; // per-window pastel for the station's own lines
    QString m_myCall; // our TX lines always display
    QString m_directedTxtPath; // backfill sources, held until
    QString m_allTxtPath;      // runBackfill()
    QSet<QString> m_members;
    // offset -> last-seen ms; an unattributed sender keying within
    // the submode's rx threshold of a member offset joins the set.
    QHash<int, qint64> m_memberOffsets;
    // Most recent transmit BY the target, for the title bar.
    QDateTime m_lastSeedUtc;
    int m_lastSeedOffset = 0;
    int m_lastSeedSubmode = -1;
    std::function<bool()> m_showHb;
    QTextBrowser *m_log;
};

#endif
