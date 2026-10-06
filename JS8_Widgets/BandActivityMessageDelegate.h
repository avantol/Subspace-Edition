#ifndef BAND_ACTIVITY_MESSAGE_DELEGATE_H
#define BAND_ACTIVITY_MESSAGE_DELEGATE_H

#include <QStyledItemDelegate>
#include <QVariantList>

/**
 * Custom delegate for the Message(s) column in the band activity table.
 * Renders callsign groups in sub-divided regions with vertical separators.
 * Supports per-sub-region tooltips and click target identification.
 */
class BandActivityMessageDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit BandActivityMessageDelegate(QObject *parent = nullptr);

    void setMyCallsign(const QString &call) { m_myCall = call; }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;

    bool helpEvent(QHelpEvent *event, QAbstractItemView *view,
                   const QStyleOptionViewItem &option,
                   const QModelIndex &index) override;

    /// Given a mouse X position within the cell, return the callsign
    /// for the sub-region at that position. Empty string if no groups.
    static QString callsignAtPosition(const QVariantList &groups,
                                      int cellWidth, int mouseX);

    /// [#240 layer 1b, 2026-10-02] ONE authority for "this `CALL: `
    /// in a group's text is its SENDER" wherever band-activity text is
    /// painted or tooltipped. Shape alone cannot decide it: "JS8MESH"
    /// (a MeshCore tag) passes Varicode::isValidCallsign through the
    /// same compound branch that admits W1AW/100 or VI75G. Position
    /// can: a group's sender is the prefix at the START of its text
    /// (groups are formed from First-frame senders, #240 layer 1), and
    /// the same sender may repeat inside the group when it sent
    /// several messages. So a match is the sender when it opens the
    /// text or names the group's own callsign; anything else is body
    /// text and stays plain. `captured` is the regex match ("JS8MESH: ").
    static bool isSenderPrefix(const QString &captured,
                               const QString &senderCall, bool atStart);

    /// HTML of one group's `text` with its sender prefix(es) in bold,
    /// wrapped for tooltips.
    static QString boldCallsignsHtml(const QString &text,
                                     const QString &senderCall);

    /// HTML for a whole row: one line per group, each group's sender
    /// bold. `joined` is the fallback when there are no groups.
    static QString rowTooltipHtml(const QVariantList &groups,
                                  const QString &joined);

private:
    static QVariantList getGroups(const QModelIndex &index);
    QString m_myCall;
};

#endif // BAND_ACTIVITY_MESSAGE_DELEGATE_H
