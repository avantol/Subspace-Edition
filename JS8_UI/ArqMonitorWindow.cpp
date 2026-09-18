/**
 * @file ArqMonitorWindow.cpp
 * @brief See header.
 */

#include "ArqMonitorWindow.h"
#include "JS8_Widgets/ScreenRescue.h" // [#246]

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>

#include "JS8_Include/SettingsGroup.h"
#include "JS8_Main/ArqMonitor.h"
#include "JS8_Main/FileTransfer.h"

namespace {
QString typeName(ArqMonitor::Type const t) {
    switch (t) {
    case ArqMonitor::Type::FileV1: return QStringLiteral("File V1");
    case ArqMonitor::Type::FileV2: return QStringLiteral("File V2");
    case ArqMonitor::Type::FileV3: return QStringLiteral("File V3");
    case ArqMonitor::Type::Link: return QStringLiteral("Link");
    }
    return {};
}
QString statusName(ArqMonitor::Session const &s) {
    switch (s.status) {
    case ArqMonitor::Status::Monitoring:
        return QStringLiteral("Monitoring");
    case ArqMonitor::Status::Complete:
        return s.saved ? QStringLiteral("Saved") :
                         QStringLiteral("Complete");
    case ArqMonitor::Status::Incomplete:
        return QStringLiteral("Incomplete");
    case ArqMonitor::Status::Failed: return QStringLiteral("Failed");
    }
    return {};
}
} // namespace

ArqMonitorWindow::ArqMonitorWindow(QSettings *settings,
                                   ArqMonitor *monitor, QWidget *parent)
    : QWidget{parent}, m_settings{settings}, m_monitor{monitor} {
    setWindowFlag(Qt::Window);
    setWindowTitle(tr("ARQ Monitor"));
    setMinimumSize(560, 360);

    auto *lay = new QVBoxLayout(this);
    m_headline = new QLabel(
        tr("Monitoring overheard ARQ transfers on current audio "
           "offset (decode-only; never transmits)"),
        this);
    m_headline->setWordWrap(true);
    lay->addWidget(m_headline);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels(
        {tr("Time (UTC)"), tr("From → To"), tr("Type"),
         tr("Name"), tr("Progress"), tr("Status")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    lay->addWidget(m_table, /*stretch=*/2);

    m_content = new QPlainTextEdit(this);
    m_content->setReadOnly(true);
    QFont mono{QStringLiteral("Monospace")};
    mono.setStyleHint(QFont::TypeWriter);
    m_content->setFont(mono);
    m_content->setPlaceholderText(
        tr("Select a completed transfer to view its content."));
    lay->addWidget(m_content, /*stretch=*/3);

    auto *btnRow = new QHBoxLayout;
    auto *clearBtn = new QPushButton(tr("Clear list"), this);
    m_saveBtn = new QPushButton(tr("Save to file storage"), this);
    m_saveBtn->setEnabled(false);
    btnRow->addWidget(clearBtn);
    btnRow->addStretch();
    btnRow->addWidget(m_saveBtn);
    lay->addLayout(btnRow);

    {
        SettingsGroup g{m_settings, "ArqMonitor"};
        restoreGeometry(
            m_settings->value("geometry", saveGeometry()).toByteArray());
        JS8::rescueOffScreen(this); // [#246]
        m_restoreVisible =
            m_settings->value("WindowVisible", false).toBool();
    }

    connect(m_monitor, &ArqMonitor::sessionsUpdated, this,
            &ArqMonitorWindow::refresh);
    connect(m_table, &QTableWidget::itemSelectionChanged, this,
            &ArqMonitorWindow::showSelected);
    connect(m_saveBtn, &QPushButton::clicked, this,
            &ArqMonitorWindow::saveSelected);
    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        // Drop-and-restart via the switch: OFF clears, ON resumes.
        m_monitor->setActive(false);
        m_monitor->setActive(true);
    });
}

void ArqMonitorWindow::showEvent(QShowEvent *event) {
    m_restoreVisible = true;
    m_monitor->setActive(true); // window open = monitoring on
    QWidget::showEvent(event);
}

void ArqMonitorWindow::userClose() {
    m_restoreVisible = false;
    close();
}

void ArqMonitorWindow::closeEvent(QCloseEvent *event) {
    if (event->spontaneous()) // the window X = user intent
        m_restoreVisible = false;
    m_monitor->setActive(false); // sessions dropped
    saveSettings();
    emit closed();
    QWidget::closeEvent(event);
}

void ArqMonitorWindow::saveSettings() {
    SettingsGroup g{m_settings, "ArqMonitor"};
    m_settings->setValue("geometry", saveGeometry());
    // Tracked flag, never isVisible() — the visrace lesson.
    m_settings->setValue("WindowVisible", m_restoreVisible);
}

int ArqMonitorWindow::selectedSessionId() const {
    auto const rows = m_table->selectionModel()
                          ? m_table->selectionModel()->selectedRows()
                          : QModelIndexList{};
    if (rows.isEmpty()) return -1;
    return m_table->item(rows.first().row(), 0)
        ->data(Qt::UserRole)
        .toInt();
}

void ArqMonitorWindow::refresh() {
    int const keepId = selectedSessionId();
    auto const &sessions = m_monitor->sessions();
    m_table->setRowCount(sessions.size());
    int row = 0;
    for (auto const &s : sessions) {
        auto *timeItem = new QTableWidgetItem(
            s.started.toString(QStringLiteral("HH:mm:ss")));
        timeItem->setData(Qt::UserRole, s.id);
        m_table->setItem(row, 0, timeItem);
        m_table->setItem(
            row, 1,
            new QTableWidgetItem(QStringLiteral("%1 → %2")
                                     .arg(s.from, s.to)));
        m_table->setItem(row, 2, new QTableWidgetItem(typeName(s.type)));
        QString name = s.header.name;
        if (name.isEmpty() && s.type == ArqMonitor::Type::Link)
            name = s.linkUrl;
        if (name.isEmpty()) name = QStringLiteral("---");
        m_table->setItem(row, 3, new QTableWidgetItem(name));
        int const have = s.type == ArqMonitor::Type::FileV3
                             ? (s.status ==
                                        ArqMonitor::Status::Monitoring
                                    ? s.binChunks.size()
                                    : s.total)
                             : (s.status ==
                                        ArqMonitor::Status::Monitoring
                                    ? s.textChunks.size()
                                    : s.total);
        m_table->setItem(
            row, 4,
            new QTableWidgetItem(
                QStringLiteral("%1/%2").arg(have).arg(s.total)));
        QString status = statusName(s);
        if (!s.detail.isEmpty())
            status += QStringLiteral(" (%1)").arg(s.detail);
        if (s.saved)
            status += QStringLiteral(" → %1").arg(s.savedPath);
        m_table->setItem(row, 5, new QTableWidgetItem(status));
        ++row;
    }
    if (keepId >= 0) {
        for (int r = 0; r < m_table->rowCount(); ++r) {
            if (m_table->item(r, 0)->data(Qt::UserRole).toInt() ==
                keepId) {
                m_table->selectRow(r);
                break;
            }
        }
    }
    showSelected();
}

void ArqMonitorWindow::showSelected() {
    auto const *s = m_monitor->session(selectedSessionId());
    if (!s || s->status != ArqMonitor::Status::Complete) {
        m_content->clear();
        m_saveBtn->setEnabled(false);
        return;
    }
    if (s->type == ArqMonitor::Type::Link) {
        m_content->setPlainText(
            tr("Web link:\n%1").arg(s->linkUrl));
        m_saveBtn->setEnabled(false); // links are display-only
        return;
    }
    QByteArray const head = s->fileBytes.left(64 * 1024);
    if (head.contains('\0'))
        m_content->setPlainText(
            tr("Binary file — %1, %2 bytes.")
                .arg(s->header.name)
                .arg(s->fileBytes.size()));
    else
        m_content->setPlainText(QString::fromUtf8(s->fileBytes));
    m_saveBtn->setEnabled(!s->saved);
}

void ArqMonitorWindow::saveSelected() {
    auto *s = m_monitor->session(selectedSessionId());
    if (!s || s->status != ArqMonitor::Status::Complete || s->saved)
        return;
    QString err;
    QString const dir = FileTransfer::receiveDirectory();
    // Identical to a real recipient: V1 re-runs the canonical decode+
    // verify+write; V2/V3 write the already-verified bytes.
    QString const path =
        s->type == ArqMonitor::Type::FileV1
            ? FileTransfer::assembleReceivedFile(dir, s->header,
                                                 s->payloadBase32, &err)
            : FileTransfer::writeReceivedFile(dir, s->header,
                                              s->fileBytes, &err);
    if (path.isEmpty()) {
        m_content->setPlainText(
            tr("Save failed: %1").arg(err));
        return;
    }
    s->saved = true;
    s->savedPath = path;
    refresh();
}
