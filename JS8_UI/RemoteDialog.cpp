/**
 * @file RemoteDialog.cpp
 * @brief File > Phone remote... (see header).
 */

#include "RemoteDialog.h"

#include "JS8_Network/RemoteServer.h"
#include "vendor/qrcodegen/qrcodegen.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkInterface>
#include <QPixmap>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {
// Tailscale hands out addresses in the carrier-grade NAT block 100.64/10.
bool isTailscale(QHostAddress const &a) {
    return a.isInSubnet(QHostAddress(QStringLiteral("100.64.0.0")), 10);
}

QPixmap qrPixmap(QString const &text, int scale) {
    using qrcodegen::QrCode;
    QrCode const qr = QrCode::encodeText(text.toUtf8().constData(), QrCode::Ecc::MEDIUM);
    int const border = 2, n = qr.getSize();
    QImage img(n + 2 * border, n + 2 * border, QImage::Format_Mono);
    img.fill(1);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            if (qr.getModule(x, y)) img.setPixel(x + border, y + border, 0);
    return QPixmap::fromImage(img.scaled(img.width() * scale, img.height() * scale));
}
} // namespace

QString RemoteDialog::newPasscode() {
    static QString const alphabet = QStringLiteral("ABCDEFGHJKMNPQRSTUVWXYZ23456789");
    QString code;
    auto *gen = QRandomGenerator::system();
    for (int i = 0; i < 8; ++i)
        code += alphabet.at(gen->bounded(alphabet.size()));
    return code;
}

RemoteDialog::RemoteDialog(QSettings *settings, QWidget *parent)
    : QDialog(parent), m_settings(settings) {
    setWindowTitle(tr("Phone remote"));

    m_settings->beginGroup(QStringLiteral("SsRemote"));
    bool const enabled = m_settings->value(QStringLiteral("Enabled"), false).toBool();
    int const port = m_settings->value(QStringLiteral("Port"), RemoteServer::kDefaultPort).toInt();
    QString passcode = m_settings->value(QStringLiteral("Passcode")).toString();
    m_settings->endGroup();
    if (passcode.isEmpty()) passcode = newPasscode();

    m_enabled = new QCheckBox(tr("Allow a phone to operate this station"), this);
    m_enabled->setChecked(enabled);
    m_port = new QSpinBox(this);
    m_port->setRange(1024, 65535);
    m_port->setValue(port);
    m_passcode = new QLineEdit(passcode, this);
    m_passcode->setReadOnly(true);
    auto *again = new QPushButton(tr("New code"), this);
    auto *codeRow = new QHBoxLayout;
    codeRow->addWidget(m_passcode);
    codeRow->addWidget(again);

    m_urls = new QLabel(this);
    m_urls->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_urls->setWordWrap(true);
    m_qr = new QLabel(this);
    m_qr->setAlignment(Qt::AlignCenter);

    auto *note = new QLabel(
        tr("On the phone, open one of these addresses (or scan the code). "
           "At home use the home-network address; away from home install "
           "Tailscale (free) on this computer and the phone and use the "
           "Tailscale (100.x) address. Nothing is opened to the internet. "
           "Setting is per profile."),
        this);
    note->setWordWrap(true);

    auto *form = new QFormLayout;
    form->addRow(m_enabled);
    form->addRow(tr("Port:"), m_port);
    form->addRow(tr("Pass code:"), codeRow);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addWidget(m_urls);
    layout->addWidget(m_qr);
    layout->addWidget(buttons);

    connect(again, &QPushButton::clicked, this, [this]() {
        m_passcode->setText(newPasscode());
        refresh();
    });
    connect(m_enabled, &QCheckBox::toggled, this, &RemoteDialog::refresh);
    connect(m_port, &QSpinBox::valueChanged, this, &RemoteDialog::refresh);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        save();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    refresh();
}

void RemoteDialog::refresh() {
    bool const on = m_enabled->isChecked();
    m_port->setEnabled(on);
    m_passcode->setEnabled(on);
    QStringList lines;
    QString first;
    // [operator 2026-10-21] Only adapters a phone can reach: up, running,
    // not loopback, not a virtual-machine or container bridge (libvirt's
    // virbr0 192.168.122.1 was listed as a "home network" address here).
    // Tailscale's own adapter is virtual too, so it is kept by address.
    static QStringList const bridges = {
        QStringLiteral("virbr"), QStringLiteral("docker"), QStringLiteral("veth"),
        QStringLiteral("br-"),   QStringLiteral("vmnet"),  QStringLiteral("vboxnet"),
        QStringLiteral("lxcbr"), QStringLiteral("lxdbr"),  QStringLiteral("cni")};
    QList<QHostAddress> addrs;
    for (auto const &nif : QNetworkInterface::allInterfaces()) {
        auto const f = nif.flags();
        if (!(f & QNetworkInterface::IsUp) || !(f & QNetworkInterface::IsRunning) ||
            (f & QNetworkInterface::IsLoopBack))
            continue;
        bool const bridge = std::any_of(bridges.begin(), bridges.end(), [&](auto const &p) {
            return nif.name().startsWith(p, Qt::CaseInsensitive);
        });
        for (auto const &e : nif.addressEntries()) {
            QHostAddress const a = e.ip();
            if (a.protocol() != QAbstractSocket::IPv4Protocol) continue;
            if (bridge && !isTailscale(a)) continue;
            addrs.append(a);
        }
    }
    // Tailscale first: the one that works away from home
    std::stable_sort(addrs.begin(), addrs.end(), [](auto const &a, auto const &b) {
        return isTailscale(a) && !isTailscale(b);
    });
    for (auto const &a : addrs) {
        // the pass code rides after '#': the browser never sends it to the
        // server in the address, the page reads it and logs in
        QString const url = QStringLiteral("http://%1:%2/#%3")
                                .arg(a.toString())
                                .arg(m_port->value())
                                .arg(m_passcode->text());
        if (first.isEmpty()) first = url;
        lines << QStringLiteral("%1  %2").arg(
                     isTailscale(a) ? tr("Tailscale:") : tr("Home network:"), url);
    }
    m_urls->setText(on ? lines.join('\n') : tr("(off)"));
    m_urls->setVisible(on);
    m_qr->setVisible(on && !first.isEmpty());
    if (on && !first.isEmpty()) m_qr->setPixmap(qrPixmap(first, 5));
    adjustSize();
}

void RemoteDialog::save() {
    m_settings->beginGroup(QStringLiteral("SsRemote"));
    m_settings->setValue(QStringLiteral("Enabled"), m_enabled->isChecked());
    m_settings->setValue(QStringLiteral("Port"), m_port->value());
    m_settings->setValue(QStringLiteral("Passcode"), m_passcode->text());
    m_settings->endGroup();
}
