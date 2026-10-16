#include "WsjtxImport.h"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QToolButton>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#endif

namespace WsjtxImport {

QStringList importKeys() {
    return {
        // radio
        QStringLiteral("Rig"), QStringLiteral("CATSerialPort"),
        QStringLiteral("CATSerialRate"), QStringLiteral("CATDataBits"),
        QStringLiteral("CATStopBits"), QStringLiteral("CATHandshake"),
        QStringLiteral("CATForceDTR"), QStringLiteral("CATForceRTS"),
        QStringLiteral("CATNetworkPort"), QStringLiteral("CATUSBPort"),
        QStringLiteral("DTR"), QStringLiteral("RTS"), QStringLiteral("Polling"),
        QStringLiteral("SplitMode"), QStringLiteral("DataMode"),
        // transmit
        QStringLiteral("PTTMethod"), QStringLiteral("PTTport"),
        QStringLiteral("TXAudioSource"), QStringLiteral("TxDelay"),
        // audio
        QStringLiteral("SoundInName"), QStringLiteral("SoundOutName"),
        QStringLiteral("AudioInputChannel"), QStringLiteral("AudioOutputChannel"),
        // calibration
        QStringLiteral("CalibrationIntercept"),
        QStringLiteral("CalibrationSlopePPM"),
        // station (used only where ours is empty -- the caller decides)
        QStringLiteral("MyCall"), QStringLiteral("MyGrid"),
    };
}

namespace {

// The text of an enum-valued setting ("PTT_method_CAT" -> "CAT").
QString enumText(QVariant const &v) {
    QString s = v.toString();
    int const us = s.lastIndexOf(QLatin1Char('_'));
    return us >= 0 ? s.mid(us + 1) : s;
}

Source readOne(QSettings &ws, QString const &file, QString const &config,
               QString const &prefix, bool current) {
    Source s;
    s.file = file;
    s.config = config;
    s.current = current;
    for (auto const &key : importKeys()) {
        QVariant const v = ws.value(prefix + QStringLiteral("Configuration/") + key);
        if (v.isValid())
            s.values.insert(key, v);
    }
    QString const rig = s.values.value(QStringLiteral("Rig")).toString();
    s.hasRig = !rig.isEmpty() && rig != QStringLiteral("None");
    QStringList parts;
    parts << (s.hasRig ? rig : QObject::tr("no radio"));
    QString const port = s.values.value(QStringLiteral("CATSerialPort")).toString();
    if (s.hasRig && !port.isEmpty()) parts << port;
    QString const ptt = enumText(s.values.value(QStringLiteral("PTTMethod")));
    if (!ptt.isEmpty()) parts << QObject::tr("PTT %1").arg(ptt);
    s.summary = parts.join(QStringLiteral(", "));
    return s;
}

} // namespace

QList<Source> readFile(QString const &file) {
    QList<Source> out;
    QSettings ws(file, QSettings::IniFormat);
    if (ws.status() != QSettings::NoError) return out;
    // WSJT-X's own layout (its MultiSettings.cpp): the active
    // configuration at the root, named by MultiSettings/CurrentName, the
    // others under MultiSettings/<name>/.
    QString const currentName =
        ws.value(QStringLiteral("MultiSettings/CurrentName"),
                 QStringLiteral("Default")).toString();
    if (ws.childGroups().contains(QStringLiteral("Configuration")))
        out << readOne(ws, file, currentName, QString(), true);
    ws.beginGroup(QStringLiteral("MultiSettings"));
    QStringList const names = ws.childGroups();
    ws.endGroup();
    for (auto const &name : names)
        if (name != currentName)
            out << readOne(ws, file, name,
                           QStringLiteral("MultiSettings/%1/").arg(name), false);
    return out;
}

QList<Source> findConfigurations() {
    QStringList files;
    QStringList const filter{QStringLiteral("WSJT-X*.ini")};
    // Linux / macOS: <ConfigLocation>/WSJT-X*.ini. Windows: Qt puts the
    // application's name under LocalAppData, and WSJT-X sets no
    // organisation, so each instance has <LocalAppData>/<app>/<app>.ini.
    QString const base =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    QDir const dir(base);
    for (auto const &f : dir.entryList(filter, QDir::Files))
        files << dir.absoluteFilePath(f);
    for (auto const &sub : dir.entryList({QStringLiteral("WSJT-X*")},
                                         QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir const d(dir.absoluteFilePath(sub));
        for (auto const &f : d.entryList(filter, QDir::Files))
            files << d.absoluteFilePath(f);
    }
    files.removeDuplicates();
    QList<Source> out;
    for (auto const &f : files)
        out << readFile(f);
    return out;
}

bool wsjtxRunning() {
    QDir const tmp(QStandardPaths::writableLocation(QStandardPaths::TempLocation));
    for (auto const &f : tmp.entryList({QStringLiteral("WSJT-X*.lock")}, QDir::Files)) {
        QLockFile lock(tmp.absoluteFilePath(f));
        qint64 pid = 0;
        QString host, app;
        if (!lock.getLockInfo(&pid, &host, &app) || pid <= 0) continue;
        if (!host.isEmpty() && host != QSysInfo::machineHostName()) continue;
#ifdef Q_OS_WIN
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                               static_cast<DWORD>(pid));
        if (!h) continue;
        DWORD code = 0;
        bool const alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
        CloseHandle(h);
        if (alive) return true;
#else
        if (::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM)
            return true;
#endif
    }
    return false;
}

Dialog::Dialog(QList<Source> sources, bool jttyAvailable, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(tr("Import radio setup from WSJT-X"));
    auto *layout = new QVBoxLayout(this);

    layout->addWidget(new QLabel(tr("WSJT-X configurations found:"), this));
    m_list = new QListWidget(this);
    m_list->setMinimumWidth(520);
    layout->addWidget(m_list);
    auto *browse = new QPushButton(tr("Browse for file..."), this);
    auto *browseRow = new QHBoxLayout;
    browseRow->addStretch();
    browseRow->addWidget(browse);
    layout->addLayout(browseRow);
    connect(browse, &QPushButton::clicked, this, [this]() {
        QString const f = QFileDialog::getOpenFileName(
            this, tr("WSJT-X settings file"), QString(),
            tr("WSJT-X settings (WSJT-X*.ini);;All files (*)"));
        if (!f.isEmpty()) addSources(readFile(f));
    });

    auto *useRow = new QHBoxLayout;
    useRow->addWidget(new QLabel(tr("Use it for:"), this));
    m_use = new QButtonGroup(this);
    auto *js8 = new QRadioButton(tr("JS8"), this);
    m_use->addButton(js8, 0);
    js8->setChecked(true);
#ifdef JS8_ENABLE_JTTY
    if (jttyAvailable) {
        auto *jtty = new QRadioButton(tr("JTTY"), this);
        m_use->addButton(jtty, 1);
        useRow->addWidget(jtty);
        jtty->setChecked(true);
    }
#else
    Q_UNUSED(jttyAvailable);
#endif
    useRow->addWidget(js8);
    useRow->addStretch();
    layout->addLayout(useRow);

    auto *nameRow = new QHBoxLayout;
    nameRow->addWidget(new QLabel(tr("New configuration name:"), this));
    m_name = new QLineEdit(this);
    nameRow->addWidget(m_name, 1);
    layout->addLayout(nameRow);
    connect(m_name, &QLineEdit::textEdited, this, [this]() { m_nameEdited = true; });

    if (wsjtxRunning()) {
        auto *warn = new QLabel(
            tr("WSJT-X appears to be running. Both programs cannot use the "
               "radio and the sound card at the same time: close WSJT-X "
               "before transmitting with the new configuration."),
            this);
        warn->setWordWrap(true);
        warn->setStyleSheet(QStringLiteral("color: #b06000;"));
        layout->addWidget(warn);
    }

    auto *more = new QToolButton(this);
    more->setText(tr("What is copied"));
    more->setCheckable(true);
    more->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    more->setArrowType(Qt::RightArrow);
    auto *details = new QLabel(
        tr("The radio (rig, CAT port and serial settings, DTR/RTS, polling, "
           "split, data mode), transmit (PTT method and port, TX audio "
           "source, TX delay), sound devices and channels, and frequency "
           "calibration. Your call sign and grid only if this program has "
           "none yet. Everything else -- frequencies, groups, macros, "
           "reporting -- is copied from the configuration you are using "
           "now.")
#ifdef JS8_ENABLE_JTTY
            + QLatin1Char(' ') +
            tr("For JTTY the new configuration starts on 14.090 MHz.")
#endif
            ,
        this);
    details->setWordWrap(true);
    details->setVisible(false);
    connect(more, &QToolButton::toggled, this, [more, details](bool on) {
        details->setVisible(on);
        more->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });
    layout->addWidget(more);
    layout->addWidget(details);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto *create = buttons->addButton(tr("Create"), QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_list, &QListWidget::currentRowChanged, this, [this, create](int row) {
        create->setEnabled(row >= 0);
        suggestName();
    });
    connect(m_use, &QButtonGroup::idClicked, this, [this](int) { suggestName(); });
    create->setEnabled(false);
    addSources(sources);
}

void Dialog::addSources(QList<Source> const &more) {
    for (auto const &s : more) {
        m_sources << s;
        auto *item = new QListWidgetItem(
            tr("%1 -- %2    %3")
                .arg(QFileInfo(s.file).fileName(), s.config, s.summary),
            m_list);
        item->setToolTip(s.file);
    }
    if (m_list->currentRow() < 0 && m_list->count() > 0) {
        // the first one with a radio, else the first
        int pick = 0;
        for (int i = 0; i < m_sources.size(); ++i)
            if (m_sources.at(i).hasRig) { pick = i; break; }
        m_list->setCurrentRow(pick);
    }
}

void Dialog::suggestName() {
    if (m_nameEdited) return;
    int const row = m_list->currentRow();
    if (row < 0 || row >= m_sources.size()) return;
    QString suffix = QStringLiteral("JS8");
#ifdef JS8_ENABLE_JTTY
    if (forJtty()) suffix = QStringLiteral("JTTY");
#endif
    m_name->setText(QStringLiteral("%1 %2").arg(m_sources.at(row).config, suffix));
}

Source Dialog::source() const {
    int const row = m_list->currentRow();
    return (row >= 0 && row < m_sources.size()) ? m_sources.at(row) : Source{};
}

bool Dialog::forJtty() const { return m_use->checkedId() == 1; }

QString Dialog::name() const { return m_name->text().trimmed(); }

} // namespace WsjtxImport
