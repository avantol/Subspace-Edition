/**
 * @file ICS213Dialog.cpp
 * @brief See header. Design provenance: operator-approved plan
 * 2026-08-18; standard-layout render mirrors the operator's reference
 * template (ICS-213.txt); Winlink render mirrors the RMS_Express_Form
 * XML observed in a real ICS213 exchange (pat-users thread + K4KDR
 * PAT proof-of-concept).
 */

#include "ICS213Dialog.h"
#include "ui_ICS213Dialog.h"

#include "JS8_Main/DriftingDateTime.h"
#include "JS8_Main/StoragePaths.h"   // [#288] per-instance paths

#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>

#include "JS8_Include/SettingsGroup.h"

namespace {
constexpr int kBodyCap = 500;

// [ARQ level 4] ONE authority for the Standard form's reply-section
// text: renderStandard appends it, mergeReply fills/appends it, and
// writeTrimmedWireCopy strips it. Never restate these strings.
QString standardBar() { return QString{50, QLatin1Char('=')}; }
QString standardThin() { return QString{50, QLatin1Char('-')}; }
QString emptyReplyBlock() {
    return QStringLiteral("9. Reply (By Recipient):\n\n"
                          "10. Replied by: \n"
                          "    Name: \n"
                          "    Position/Title: \n"
                          "    Date/Time: \n");
}
QString emptyCompactReplySection() {
    return QStringLiteral("REPLY:\n\n"
                          "REPLIED-BY: \n"
                          "REPLY-DATETIME: \n");
}
QString filledCompactReplySection(QString const &text,
                                  QString const &name,
                                  QString const &pos,
                                  QString const &dt) {
    QString out;
    out += QStringLiteral("REPLY:\n%1\n").arg(text);
    out += QStringLiteral("REPLIED-BY: %1, %2\n").arg(name, pos);
    out += QStringLiteral("REPLY-DATETIME: %1\n").arg(dt);
    return out;
}
QString filledReplyBlock(QString const &text, QString const &name,
                         QString const &pos, QString const &dt) {
    QString out;
    out += QStringLiteral("9. Reply (By Recipient):\n%1\n\n").arg(text);
    out += QStringLiteral("10. Replied by: \n");
    out += QStringLiteral("    Name: %1\n").arg(name);
    out += QStringLiteral("    Position/Title: %1\n").arg(pos);
    out += QStringLiteral("    Date/Time: %1\n").arg(dt);
    return out;
}
QString const kDraftName = QStringLiteral("ics213_draft.json");

QString xmlEscape(QString s) {
    s.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    s.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    s.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    return s;
}

QString xmlUnescape(QString s) {
    s.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    s.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    s.replace(QStringLiteral("&amp;"), QStringLiteral("&")); // LAST
    return s;
}

// First line starting with `key` (after `from`) → the rest of it.
QString lineAfter(QString const &text, QString const &key,
                  int const from = 0) {
    int i = text.indexOf(key, from);
    if (i < 0) return {};
    i += key.size();
    int const e = text.indexOf(QLatin1Char('\n'), i);
    return text.mid(i, (e < 0 ? text.size() : e) - i).trimmed();
}
} // namespace

ICS213Dialog::ICS213Dialog(QSettings *settings, QString myCall,
                           QDir saveDir,
                           std::function<double(int)> airtimeSecs,
                           QWidget *parent)
    : QDialog{parent}, ui{new Ui::ICS213Dialog}, m_settings{settings},
      m_myCall{std::move(myCall)}, m_saveDir{std::move(saveDir)},
      m_airtimeSecs{std::move(airtimeSecs)} {
    ui->setupUi(this);

    auto const now = DriftingDateTime::currentDateTimeUtc();
    ui->dateEdit->setText(now.toString(QStringLiteral("yyyy-MM-dd")));
    ui->timeEdit->setText(now.toString(QStringLiteral("HH:mm")));

    {
        SettingsGroup g{m_settings, "ICS213"};
        ui->formatCombo->setCurrentIndex(
            m_settings->value("Format", 0).toInt());
    }

    // Body cap enforced in the change handler (QPlainTextEdit has no
    // maxLength) — truncation at the cap, never silent growth.
    for (auto *e : {ui->incidentEdit, ui->toEdit, ui->fromEdit,
                    ui->subjectEdit, ui->dateEdit, ui->timeEdit,
                    ui->approvedEdit, ui->positionEdit,
                    ui->repliedByEdit, ui->replyPositionEdit,
                    ui->replyDateTimeEdit})
        connect(e, &QLineEdit::textChanged, this,
                &ICS213Dialog::onFieldChanged);
    connect(ui->messageEdit, &QPlainTextEdit::textChanged, this,
            &ICS213Dialog::onFieldChanged);
    connect(ui->replyEdit, &QPlainTextEdit::textChanged, this,
            &ICS213Dialog::onFieldChanged);
    connect(ui->precedenceCombo, &QComboBox::currentIndexChanged, this,
            &ICS213Dialog::onFieldChanged);
    connect(ui->formatCombo, &QComboBox::currentIndexChanged, this,
            &ICS213Dialog::onFieldChanged);

    connect(ui->sendButton, &QPushButton::clicked, this,
            &ICS213Dialog::onSendClicked);
    connect(ui->saveButton, &QPushButton::clicked, this,
            &ICS213Dialog::onSaveClicked);
    connect(ui->clearButton, &QPushButton::clicked, this,
            &ICS213Dialog::onClearClicked);
    // "Save as draft" = reject(); the override below flushes the
    // draft, and Esc/X share the exact same path.
    connect(ui->cancelButton, &QPushButton::clicked, this,
            &QDialog::reject);

    m_draftTimer.setSingleShot(true);
    m_draftTimer.setInterval(1000);
    connect(&m_draftTimer, &QTimer::timeout, this,
            &ICS213Dialog::saveDraft);

    ui->arqBusyLabel->setVisible(false);
    // Serial in the title: peeked, not assigned — the number is only
    // consumed at Send/Save. Single-instance dialog + no other bumper
    // means the peek can't go stale while the form is open.
    setWindowTitle(tr("Send ICS-213 Form — %1")
                       .arg(peekSerial()));
    loadDraft();
    updateFooter();
}

ICS213Dialog::~ICS213Dialog() { delete ui; }

QDir ICS213Dialog::formDir() const {
    QDir d{m_saveDir};
    d.mkpath(QStringLiteral("ICS213"));
    d.cd(QStringLiteral("ICS213"));
    return d;
}

QString ICS213Dialog::draftPath() const {
    return formDir().filePath(kDraftName);
}

void ICS213Dialog::onFieldChanged() {
    // Enforce the body cap with truncation — on whichever body edit
    // is live in this mode (message in compose, reply text in reply).
    auto *body = m_replyMode ? ui->replyEdit : ui->messageEdit;
    if (auto txt = body->toPlainText(); txt.size() > kBodyCap) {
        auto c = body->textCursor();
        int const pos = std::min(c.position(), kBodyCap);
        body->setPlainText(txt.left(kBodyCap));
        c.setPosition(pos);
        body->setTextCursor(c);
    }
    updateFooter();
    // Reply mode has no draft — the received form is already on disk
    // and the compose draft must not be touched.
    if (!m_replyMode)
        m_draftTimer.start();
}

void ICS213Dialog::updateFooter() {
    auto *body = m_replyMode ? ui->replyEdit : ui->messageEdit;
    // Reply sends only the sparse packet — estimate THAT, not the
    // full form.
    int const chars = m_replyMode ? renderReplySparse().size()
                                  : renderStandard(peekSerial()).size();
    QString est;
    if (m_airtimeSecs) {
        double const s = m_airtimeSecs(chars);
        est = (s >= 90.0)
                  ? tr(" · est. airtime ≈ %1 min").arg(qRound(s / 60.0))
                  : tr(" · est. airtime ≈ %1 s").arg(qRound(s));
    }
    ui->footerLabel->setText(
        tr("%1 / %2 message chars · file %3 chars%4")
            .arg(body->toPlainText().size())
            .arg(kBodyCap)
            .arg(chars)
            .arg(est));
}

bool ICS213Dialog::validateRequired() {
    if (m_replyMode) {
        if (ui->replyEdit->toPlainText().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("ICS-213"),
                                 tr("The reply text is required."));
            ui->replyEdit->setFocus();
            return false;
        }
        if (ui->repliedByEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(
                this, tr("ICS-213"),
                tr("\"Replied by (name)\" is required."));
            ui->repliedByEdit->setFocus();
            return false;
        }
        return true;
    }
    struct Req { QLineEdit *e; char const *name; };
    for (auto const &r : {Req{ui->toEdit, "To"},
                          Req{ui->fromEdit, "From"},
                          Req{ui->subjectEdit, "Subject"}}) {
        if (r.e->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("ICS-213"),
                                 tr("\"%1\" is required.")
                                     .arg(QLatin1String(r.name)));
            r.e->setFocus();
            return false;
        }
    }
    if (ui->messageEdit->toPlainText().trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("ICS-213"),
                             tr("The message body is required."));
        ui->messageEdit->setFocus();
        return false;
    }
    return true;
}

QString ICS213Dialog::nextSerial() {
    SettingsGroup g{m_settings, "ICS213"};
    int const year =
        DriftingDateTime::currentDateTimeUtc().date().year();
    int next = m_settings->value("SerialNext", 1).toInt();
    if (m_settings->value("SerialYear", year).toInt() != year)
        next = 1; // yearly reset (operator decision 2026-08-18)
    m_settings->setValue("SerialYear", year);
    m_settings->setValue("SerialNext", next + 1);
    return QStringLiteral("%1-%2").arg(m_myCall).arg(
        next, 4, 10, QLatin1Char('0'));
}

QString ICS213Dialog::peekSerial() const {
    SettingsGroup g{m_settings, "ICS213"};
    int const year =
        DriftingDateTime::currentDateTimeUtc().date().year();
    int next = m_settings->value("SerialNext", 1).toInt();
    if (m_settings->value("SerialYear", year).toInt() != year)
        next = 1; // mirrors nextSerial()'s yearly reset, sans persist
    return QStringLiteral("%1-%2").arg(m_myCall).arg(
        next, 4, 10, QLatin1Char('0'));
}

// Subject with the precedence suffix (Routine = bare) — used by every
// render so the numbered layout stays structurally standard.
static QString subjectWithPrecedence(QString const &subject,
                                     int const precedenceIdx) {
    switch (precedenceIdx) {
    case 1: return subject + QStringLiteral(" [PRIORITY]");
    case 2: return subject + QStringLiteral(" [IMMEDIATE]");
    default: return subject;
    }
}

QString ICS213Dialog::renderStandard(QString const &serial) const {
    // Mirrors the operator's reference template (ICS-213.txt): same
    // banner, same numbered sections; blocks 9/10 rendered as empty
    // headers so the recipient sees the complete familiar form
    // awaiting reply.
    QString const bar{50, QLatin1Char('=')};
    QString out;
    out += bar + QLatin1Char('\n');
    out += QStringLiteral("GENERAL MESSAGE (ICS-213)\n");
    out += bar + QLatin1Char('\n');
    out += QStringLiteral("Number: %1\n").arg(serial);
    out += QStringLiteral("1. Incident Name (Optional): %1\n")
               .arg(ui->incidentEdit->text());
    out += QStringLiteral("2. To (Name and Position): %1\n")
               .arg(ui->toEdit->text());
    out += QStringLiteral("3. From (Name and Position): %1\n")
               .arg(ui->fromEdit->text());
    out += QStringLiteral("4. Subject: %1\n")
               .arg(subjectWithPrecedence(
                   ui->subjectEdit->text(),
                   ui->precedenceCombo->currentIndex()));
    out += QStringLiteral("5. Date: %1\n").arg(ui->dateEdit->text());
    out += QStringLiteral("6. Time: %1\n\n").arg(ui->timeEdit->text());
    out += QStringLiteral("7. Message:\n%1\n\n")
               .arg(ui->messageEdit->toPlainText());
    out += QStringLiteral("8. Approved by: \n");
    out += QStringLiteral("   Name: %1\n").arg(ui->approvedEdit->text());
    out += QStringLiteral("   Position/Title: %1\n\n")
               .arg(ui->positionEdit->text());
    out += QStringLiteral("Sender:  [%1]\n\n")
               .arg(m_replyMode ? m_replySenderCall : m_myCall);
    // [ARQ level 4] The LOCAL file is always the complete document
    // (printable, hand-fillable — the level-3 interop shape). The
    // level-4 wire trim happens in writeTrimmedWireCopy at dispatch,
    // once the peer's answer is known.
    out += standardThin() + QLatin1Char('\n');
    out += m_replyMode
               ? filledReplyBlock(ui->replyEdit->toPlainText(),
                                  ui->repliedByEdit->text(),
                                  ui->replyPositionEdit->text(),
                                  ui->replyDateTimeEdit->text())
               : emptyReplyBlock();
    out += bar + QLatin1Char('\n');
    return out;
}

QString ICS213Dialog::renderCompact(QString const &serial) const {
    QString out;
    out += QStringLiteral("FORM: ICS-213/1\n");
    out += QStringLiteral("NUMBER: %1\n").arg(serial);
    out += QStringLiteral("INCIDENT: %1\n").arg(ui->incidentEdit->text());
    out += QStringLiteral("TO: %1\n").arg(ui->toEdit->text());
    out += QStringLiteral("FROM: %1\n").arg(ui->fromEdit->text());
    out += QStringLiteral("SUBJECT: %1\n")
               .arg(subjectWithPrecedence(
                   ui->subjectEdit->text(),
                   ui->precedenceCombo->currentIndex()));
    out += QStringLiteral("DATE: %1\n").arg(ui->dateEdit->text());
    out += QStringLiteral("TIME: %1\n").arg(ui->timeEdit->text());
    out += QStringLiteral("APPROVED: %1, %2\n")
               .arg(ui->approvedEdit->text(), ui->positionEdit->text());
    out += QStringLiteral("SENDER: %1\n")
               .arg(m_replyMode ? m_replySenderCall : m_myCall);
    out += QStringLiteral("MSG:\n%1\n")
               .arg(ui->messageEdit->toPlainText());
    // [level3 placeholders] Compose carries EMPTY reply keys so a
    // level-3 recipient can fill them manually in a text editor
    // (operator report 2026-08-18: Compact had no placeholders at
    // all). Level-4 wire strips them (writeTrimmedWireCopy).
    out += m_replyMode
               ? filledCompactReplySection(
                     ui->replyEdit->toPlainText(),
                     ui->repliedByEdit->text(),
                     ui->replyPositionEdit->text(),
                     ui->replyDateTimeEdit->text())
               : emptyCompactReplySection();
    return out;
}

QString ICS213Dialog::renderWinlinkXml(QString const &serial) const {
    // RMS_Express_Form structure per a real ICS213 exchange (tags
    // verified from the pat-users capture, incl. Winlink's own
    // "approved_postitle" spelling). msgseqnum = numeric part of our
    // serial. NOTE: the message-body tag <msg> is best-known but was
    // not visible in the captures — VERIFY against a live Winlink
    // export before relying on far-end rendering (a wrong tag shows
    // an empty body in the Winlink viewer).
    auto const now = DriftingDateTime::currentDateTimeUtc();
    QString const seq = serial.section(QLatin1Char('-'), -1);
    QString out;
    out += QStringLiteral("<?xml version=\"1.0\"?>\n");
    out += QStringLiteral("<RMS_Express_Form>\n");
    out += QStringLiteral("  <form_parameters>\n");
    out += QStringLiteral("    <xml_file_version>1.0</xml_file_version>\n");
    out += QStringLiteral("    <rms_express_version>Subspace</rms_express_version>\n");
    out += QStringLiteral("    <submission_datetime>%1</submission_datetime>\n")
               .arg(now.toString(QStringLiteral("yyyyMMddHHmmss")));
    out += QStringLiteral("    <senders_callsign>%1</senders_callsign>\n")
               .arg(xmlEscape(m_replyMode ? m_replySenderCall
                                          : m_myCall));
    out += QStringLiteral("    <display_form>ICS213_Initial_Viewer.html</display_form>\n");
    out += QStringLiteral("    <reply_template>ICS213_SendReply.0</reply_template>\n");
    out += QStringLiteral("  </form_parameters>\n");
    out += QStringLiteral("  <variables>\n");
    out += QStringLiteral("    <templateversion>ICS 213 v.41.3</templateversion>\n");
    out += QStringLiteral("    <msgsender>%1</msgsender>\n")
               .arg(xmlEscape(m_replyMode ? m_replySenderCall
                                          : m_myCall));
    out += QStringLiteral("    <msgseqnum>%1</msgseqnum>\n").arg(seq);
    out += QStringLiteral("    <inc_name>%1</inc_name>\n")
               .arg(xmlEscape(ui->incidentEdit->text()));
    out += QStringLiteral("    <to_name>%1</to_name>\n")
               .arg(xmlEscape(ui->toEdit->text()));
    out += QStringLiteral("    <fm_name>%1</fm_name>\n")
               .arg(xmlEscape(ui->fromEdit->text()));
    out += QStringLiteral("    <subjectline>%1</subjectline>\n")
               .arg(xmlEscape(subjectWithPrecedence(
                   ui->subjectEdit->text(),
                   ui->precedenceCombo->currentIndex())));
    out += QStringLiteral("    <mdate>%1</mdate>\n")
               .arg(xmlEscape(ui->dateEdit->text()));
    out += QStringLiteral("    <mtime>%1</mtime>\n")
               .arg(xmlEscape(ui->timeEdit->text()));
    out += QStringLiteral("    <msg>%1</msg>\n")
               .arg(xmlEscape(ui->messageEdit->toPlainText()));
    out += QStringLiteral("    <approved_name>%1</approved_name>\n")
               .arg(xmlEscape(ui->approvedEdit->text()));
    out += QStringLiteral("    <approved_postitle>%1</approved_postitle>\n")
               .arg(xmlEscape(ui->positionEdit->text()));
    if (m_replyMode) {
        // Reply tag names UNVERIFIED against a live Winlink reply
        // export (same caveat as <msg> above) — chosen to match the
        // capture's naming style.
        out += QStringLiteral("    <reply>%1</reply>\n")
                   .arg(xmlEscape(ui->replyEdit->toPlainText()));
        out += QStringLiteral("    <replied_by>%1</replied_by>\n")
                   .arg(xmlEscape(ui->repliedByEdit->text()));
        out += QStringLiteral(
                   "    <replied_postitle>%1</replied_postitle>\n")
                   .arg(xmlEscape(ui->replyPositionEdit->text()));
        out += QStringLiteral(
                   "    <replied_datetime>%1</replied_datetime>\n")
                   .arg(xmlEscape(ui->replyDateTimeEdit->text()));
    }
    out += QStringLiteral("  </variables>\n");
    out += QStringLiteral("</RMS_Express_Form>\n");
    return out;
}

bool ICS213Dialog::writeFormFile(QString *outPath) {
    if (!validateRequired())
        return false;
    int const fmt =
        m_replyMode ? m_replyFormat : ui->formatCombo->currentIndex();
    if (!m_replyMode) {
        SettingsGroup g{m_settings, "ICS213"};
        m_settings->setValue("Format", fmt);
    }
    // A reply COMPLETES the original form — it keeps the original's
    // serial and consumes none of ours.
    QString const serial =
        m_replyMode ? (m_replySerial.isEmpty()
                           ? QStringLiteral("REPLY")
                           : m_replySerial)
                    : nextSerial();
    QString content;
    QString ext = QStringLiteral("txt");
    switch (fmt) {
    case 1: content = renderCompact(serial); break;
    case 2:
        content = renderWinlinkXml(serial);
        ext = QStringLiteral("xml");
        break;
    default: content = renderStandard(serial); break;
    }
    // [ARQ level 4] Reply mode writes TWO artifacts: the complete
    // filled form (local archive + the level-3 wire shape) rendered
    // above in the ORIGINAL's format, and the sparse packet (the
    // level-4 wire shape) in the temp wire dir. Dispatch picks by
    // the original sender's answered level.
    m_sparseWirePath.clear();
    if (m_replyMode) {
        // [#288] per-instance scratch, not the shared temp root
        QString const dir = StoragePaths::instanceTempDir() +
                            QStringLiteral("/ics213-wire");
        if (QDir{}.mkpath(dir)) {
            QString const sp =
                dir + QLatin1Char('/') +
                QFileInfo{m_replySourcePath}.completeBaseName() +
                QStringLiteral("_REPLY.txt");
            if (QFile f{sp};
                f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(renderReplySparse().toUtf8());
                f.close();
                m_sparseWirePath = sp;
            }
        }
    }
    int const year =
        DriftingDateTime::currentDateTimeUtc().date().year();
    // [slashfix 2026-08-20] A portable call's '/' is not filename-
    // compatible (send side would treat it as a directory; the
    // receive-side sanitizer truncates at it) — underscore on disk,
    // slash everywhere else (title, body, wire text).
    QString fileSerial = serial;
    fileSerial.replace(QLatin1Char('/'), QLatin1Char('_'));
    // [2026-08-18] No subject slug — serial + year identify the
    // form and the slug doubled the serial digits on short subjects
    // (operator report: "..._0006_2026_6666").
    QString const name =
        m_replyMode
            ? QStringLiteral("%1_REPLY.%2")
                  .arg(QFileInfo{m_replySourcePath}.completeBaseName())
                  .arg(ext)
            : QStringLiteral("ICS213_%1_%2.%3")
                  .arg(fileSerial)
                  .arg(year)
                  .arg(ext);
    QString const path = formDir().filePath(name);
    QFile f{path};
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(
            this, tr("ICS-213"),
            tr("Could not write %1:\n%2").arg(path, f.errorString()));
        return false;
    }
    f.write(content.toUtf8());
    f.close();
    m_writtenPath = path;
    if (outPath)
        *outPath = path;
    return true;
}

void ICS213Dialog::onSendClicked() {
    QString path;
    if (!writeFormFile(&path))
        return;
    if (!m_replyMode)
        clearDraft(); // reply never touches the compose draft
    Q_EMIT sendRequested(path, m_sparseWirePath);
    accept();
}

void ICS213Dialog::onSaveClicked() {
    QString path;
    if (!writeFormFile(&path))
        return;
    clearDraft();
    m_draftTimer.stop();
    QMessageBox::information(this, tr("ICS-213"),
                             tr("Form saved:\n%1").arg(path));
    accept(); // "Save as file" exits after showing the file name
}

void ICS213Dialog::onClearClicked() {
    for (auto *e : {ui->incidentEdit, ui->toEdit, ui->fromEdit,
                    ui->subjectEdit, ui->approvedEdit,
                    ui->positionEdit})
        e->clear();
    ui->messageEdit->clear();
    ui->precedenceCombo->setCurrentIndex(0);
    auto const now = DriftingDateTime::currentDateTimeUtc();
    ui->dateEdit->setText(now.toString(QStringLiteral("yyyy-MM-dd")));
    ui->timeEdit->setText(now.toString(QStringLiteral("HH:mm")));
    clearDraft();
    // The clear() calls above fired textChanged → armed the debounce,
    // which would re-write an EMPTY draft 1 s from now. Disarm it.
    m_draftTimer.stop();
    updateFooter();
}

void ICS213Dialog::reject() {
    m_draftTimer.stop();
    // Reply mode: Cancel discards the reply; the RECEIVED form is
    // already saved on disk and the compose draft is not ours to
    // write.
    if (!m_replyMode)
        saveDraft(); // deletes the draft instead if all-empty
    QDialog::reject();
}

// Keys first, body LAST — everything after "REPLY:\n" is the reply
// text verbatim, so the body can contain anything (even lines that
// look like keys).
QString ICS213Dialog::renderReplySparse() const {
    QString out;
    out += QStringLiteral("FORM: ICS-213-REPLY/1\n");
    out += QStringLiteral("REF: %1\n")
               .arg(QFileInfo{m_replySourcePath}.fileName());
    out += QStringLiteral("REPLIED-BY: %1, %2\n")
               .arg(ui->repliedByEdit->text(),
                    ui->replyPositionEdit->text());
    out += QStringLiteral("REPLY-DATETIME: %1\n")
               .arg(ui->replyDateTimeEdit->text());
    out += QStringLiteral("REPLY:\n%1\n")
               .arg(ui->replyEdit->toPlainText());
    return out;
}

QString ICS213Dialog::writeTrimmedWireCopy(QString const &fullPath) {
    QFile in{fullPath};
    if (!in.open(QIODevice::ReadOnly)) return fullPath;
    QString content = QString::fromUtf8(in.readAll());
    in.close();
    QString const stdSection =
        standardThin() + QLatin1Char('\n') + emptyReplyBlock();
    if (content.count(stdSection) == 1)
        content.remove(stdSection);
    else if (content.count(emptyCompactReplySection()) == 1)
        content.remove(emptyCompactReplySection());
    else
        return fullPath; // nothing to trim (XML / already trimmed)
    QString const dir = StoragePaths::instanceTempDir() +
                        QStringLiteral("/ics213-wire");   // [#288]
    if (!QDir{}.mkpath(dir)) return fullPath;
    // SAME basename — the transfer header (and the reply REF chain)
    // key on the filename.
    QString const outPath =
        dir + QLatin1Char('/') + QFileInfo{fullPath}.fileName();
    QFile out{outPath};
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fullPath;
    out.write(content.toUtf8());
    out.close();
    return outPath;
}

QString ICS213Dialog::serialFromFormName(QString const &name) {
    static QRegularExpression const kSerialRe{
        QStringLiteral(
            R"(^ICS213_(.+)_\d{4}(?:_REPLY)?\.[A-Za-z0-9]+$)"),
        QRegularExpression::CaseInsensitiveOption};
    auto const m = kSerialRe.match(name);
    return m.hasMatch() ? m.captured(1) : QString{};
}

bool ICS213Dialog::isSparseReply(QByteArray const &bytes) {
    return bytes.contains("FORM: ICS-213-REPLY/1");
}

QString ICS213Dialog::mergeReply(QString const &originalContent,
                                 QString const &sparseContent) {
    // Parse the sparse packet.
    QString const by =
        lineAfter(sparseContent, QStringLiteral("REPLIED-BY: "));
    QString const dt =
        lineAfter(sparseContent, QStringLiteral("REPLY-DATETIME: "));
    int const bi = sparseContent.indexOf(QStringLiteral("REPLY:\n"));
    if (bi < 0) return {};
    QString text = sparseContent.mid(bi + 7);
    if (text.endsWith(QLatin1Char('\n'))) text.chop(1);
    QString name = by, pos;
    if (int const comma = by.lastIndexOf(QStringLiteral(", "));
        comma >= 0) {
        name = by.left(comma);
        pos = by.mid(comma + 2);
    }

    QString out = originalContent;
    switch (probeFormat(originalContent.toUtf8())) {
    case 0: {
        QString const filled = filledReplyBlock(text, name, pos, dt);
        // Full-shape original (the normal local archive): empty 9/10
        // block present — fill in place. Block 10's lines are
        // 4-space indented, block 8's are 3-space; no cross-match.
        if (out.count(emptyReplyBlock()) == 1) {
            out.replace(emptyReplyBlock(), filled);
            return out;
        }
        // Trimmed original (a .368.7-era archive, or a received
        // trimmed wire copy): ends at the bar after Sender — append
        // the reply section before a closing bar.
        QString const barLine = standardBar() + QLatin1Char('\n');
        if (!out.endsWith(barLine)) return {}; // shape mismatch
        out.chop(barLine.size());
        out += standardThin() + QLatin1Char('\n');
        out += filled;
        out += barLine;
        return out;
    }
    case 1: {
        QString const filled =
            filledCompactReplySection(text, name, pos, dt);
        // Full-shape original: empty placeholders present — fill.
        if (out.count(emptyCompactReplySection()) == 1) {
            out.replace(emptyCompactReplySection(), filled);
            return out;
        }
        // Legacy/trimmed original: append after the message.
        if (!out.endsWith(QLatin1Char('\n')))
            out += QLatin1Char('\n');
        out += filled;
        return out;
    }
    case 2: {
        // Winlink XML: reply variables slot in before </variables>.
        QString const anchor = QStringLiteral("  </variables>");
        if (out.count(anchor) != 1) return {};
        QString vars;
        vars += QStringLiteral("    <reply>%1</reply>\n")
                    .arg(xmlEscape(text));
        vars += QStringLiteral("    <replied_by>%1</replied_by>\n")
                    .arg(xmlEscape(name));
        vars += QStringLiteral(
                    "    <replied_postitle>%1</replied_postitle>\n")
                    .arg(xmlEscape(pos));
        vars += QStringLiteral(
                    "    <replied_datetime>%1</replied_datetime>\n")
                    .arg(xmlEscape(dt));
        out.replace(anchor, vars + anchor);
        return out;
    }
    default: return {};
    }
}

int ICS213Dialog::probeFormat(QByteArray const &bytes) {
    if (bytes.contains("<RMS_Express_Form>") &&
        bytes.contains("<to_name>") && bytes.contains("<msg>"))
        return 2;
    if (bytes.contains("FORM: ICS-213/1") && bytes.contains("\nTO: ") &&
        bytes.contains("\nMSG:\n"))
        return 1;
    if (bytes.contains("GENERAL MESSAGE (ICS-213)") &&
        bytes.contains("2. To (Name and Position): ") &&
        bytes.contains("7. Message:\n"))
        return 0;
    return -1;
}

bool ICS213Dialog::enterReplyMode(QString const &receivedPath,
                                  QString const &fromCall) {
    QFile f{receivedPath};
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(
            this, tr("ICS-213"),
            tr("Could not read %1:\n%2")
                .arg(receivedPath, f.errorString()));
        return false;
    }
    QByteArray const bytes = f.readAll();
    int const fmt = probeFormat(bytes);
    if (fmt < 0) {
        QMessageBox::warning(this, tr("ICS-213"),
                             tr("Unable to interpret the form."));
        return false;
    }

    // Mode flag FIRST — the setText calls below fire onFieldChanged,
    // which must not arm the compose-draft autosave.
    m_replyMode = true;
    m_replyFormat = fmt;
    m_replySourcePath = receivedPath;
    m_replySenderCall = fromCall; // parse may refine from the form

    m_replySerial =
        serialFromFormName(QFileInfo{receivedPath}.fileName());

    QString const content = QString::fromUtf8(bytes);
    ui->precedenceCombo->setCurrentIndex(0); // suffix stays in subject
    switch (fmt) {
    case 2: parseWinlinkXml(content); break;
    case 1: parseCompact(content); break;
    default: parseStandard(content); break;
    }

    // Originals are evidence now — read-only.
    for (auto *e : {ui->incidentEdit, ui->toEdit, ui->fromEdit,
                    ui->subjectEdit, ui->dateEdit, ui->timeEdit,
                    ui->approvedEdit, ui->positionEdit})
        e->setReadOnly(true);
    ui->messageEdit->setReadOnly(true);
    ui->precedenceCombo->setEnabled(false);
    ui->formatCombo->setEnabled(false); // reply goes back in kind

    // Reply blocks visible, date/time pre-filled.
    for (QWidget *w :
         std::initializer_list<QWidget *>{
             ui->replyLabel, ui->replyEdit, ui->repliedByLabel,
             ui->repliedByEdit, ui->replyPositionLabel,
             ui->replyPositionEdit, ui->replyDateTimeLabel,
             ui->replyDateTimeEdit})
        w->setVisible(true);
    ui->replyDateTimeEdit->setText(
        DriftingDateTime::currentDateTimeUtc().toString(
            QStringLiteral("yyyy-MM-dd HH:mm")));
    // With both message boxes on screen the splitter starved them —
    // give the read-only original and the reply box real height.
    ui->messageEdit->setMinimumHeight(110);
    ui->replyEdit->setMinimumHeight(110);
    setMinimumSize(520, 700);

    // Button set for this phase.
    ui->clearButton->setVisible(false);
    ui->saveButton->setVisible(false);
    ui->sendButton->setToolTip(tr("Send reply immediately"));
    ui->cancelButton->setText(tr("Cancel"));
    ui->cancelButton->setToolTip(tr(
        "Cancel reply, the form is saved for 'Send file' later"));

    setWindowTitle(
        m_replySerial.isEmpty()
            ? tr("Reply to ICS-213 Form")
            : tr("Reply to ICS-213 Form \u2014 %1").arg(m_replySerial));
    ui->replyEdit->setFocus();
    updateFooter();
    return true;
}

void ICS213Dialog::parseStandard(QString const &content) {
    // Only the part above block 9 — block 10's "Name:" line would
    // otherwise shadow block 8's.
    QString const head =
        content.section(QStringLiteral("\n9. Reply"), 0, 0);
    ui->incidentEdit->setText(
        lineAfter(head, QStringLiteral("1. Incident Name (Optional): ")));
    ui->toEdit->setText(
        lineAfter(head, QStringLiteral("2. To (Name and Position): ")));
    ui->fromEdit->setText(
        lineAfter(head, QStringLiteral("3. From (Name and Position): ")));
    ui->subjectEdit->setText(
        lineAfter(head, QStringLiteral("4. Subject: ")));
    ui->dateEdit->setText(lineAfter(head, QStringLiteral("5. Date: ")));
    ui->timeEdit->setText(lineAfter(head, QStringLiteral("6. Time: ")));
    int const m0 = head.indexOf(QStringLiteral("7. Message:\n"));
    int const m1 = head.indexOf(QStringLiteral("\n\n8. Approved by:"));
    if (m0 >= 0 && m1 > m0)
        ui->messageEdit->setPlainText(head.mid(m0 + 12, m1 - (m0 + 12)));
    if (int const a = head.indexOf(QStringLiteral("8. Approved by:"));
        a >= 0) {
        ui->approvedEdit->setText(
            lineAfter(head, QStringLiteral("Name: "), a));
        ui->positionEdit->setText(
            lineAfter(head, QStringLiteral("Position/Title: "), a));
    }
    static QRegularExpression const senderRe{
        QStringLiteral("Sender:\\s*\\[([^\\]]+)\\]")};
    if (auto const m = senderRe.match(head); m.hasMatch())
        m_replySenderCall = m.captured(1);
}

void ICS213Dialog::parseCompact(QString const &content) {
    int const mi = content.indexOf(QStringLiteral("\nMSG:\n"));
    QString const head = mi >= 0 ? content.left(mi + 1) : content;
    ui->incidentEdit->setText(
        lineAfter(head, QStringLiteral("INCIDENT: ")));
    ui->toEdit->setText(lineAfter(head, QStringLiteral("TO: ")));
    ui->fromEdit->setText(lineAfter(head, QStringLiteral("FROM: ")));
    ui->subjectEdit->setText(
        lineAfter(head, QStringLiteral("SUBJECT: ")));
    ui->dateEdit->setText(lineAfter(head, QStringLiteral("DATE: ")));
    ui->timeEdit->setText(lineAfter(head, QStringLiteral("TIME: ")));
    // "APPROVED: name, position" — split on the LAST comma so names
    // containing commas survive better than positions do.
    if (QString const ap = lineAfter(head, QStringLiteral("APPROVED: "));
        !ap.isEmpty()) {
        int const comma = ap.lastIndexOf(QStringLiteral(", "));
        ui->approvedEdit->setText(comma >= 0 ? ap.left(comma) : ap);
        ui->positionEdit->setText(comma >= 0 ? ap.mid(comma + 2)
                                             : QString());
    }
    if (QString const sc = lineAfter(head, QStringLiteral("SENDER: "));
        !sc.isEmpty())
        m_replySenderCall = sc;
    if (mi >= 0) {
        QString msg = content.mid(mi + 6);
        // Full-shape original: the empty reply placeholders trail
        // the message — they are form furniture, not body text.
        if (QString const tail =
                QLatin1Char('\n') + emptyCompactReplySection();
            msg.endsWith(tail))
            msg.chop(tail.size());
        if (msg.endsWith(QLatin1Char('\n')))
            msg.chop(1);
        ui->messageEdit->setPlainText(msg);
    }
}

void ICS213Dialog::parseWinlinkXml(QString const &content) {
    auto const tag = [&content](char const *t) -> QString {
        QRegularExpression const re{
            QStringLiteral("<%1>(.*)</%1>").arg(QLatin1String{t}),
            QRegularExpression::DotMatchesEverythingOption |
                QRegularExpression::InvertedGreedinessOption};
        auto const m = re.match(content);
        return m.hasMatch() ? xmlUnescape(m.captured(1)) : QString();
    };
    ui->incidentEdit->setText(tag("inc_name"));
    ui->toEdit->setText(tag("to_name"));
    ui->fromEdit->setText(tag("fm_name"));
    ui->subjectEdit->setText(tag("subjectline"));
    ui->dateEdit->setText(tag("mdate"));
    ui->timeEdit->setText(tag("mtime"));
    ui->messageEdit->setPlainText(tag("msg"));
    ui->approvedEdit->setText(tag("approved_name"));
    ui->positionEdit->setText(tag("approved_postitle"));
    if (QString const sc = tag("msgsender"); !sc.isEmpty())
        m_replySenderCall = sc;
}

void ICS213Dialog::saveDraft() {
    // An all-empty form has no draft to keep — delete instead of
    // writing an empty file that would "restore" a blank form.
    bool const empty =
        ui->incidentEdit->text().isEmpty() &&
        ui->toEdit->text().isEmpty() && ui->fromEdit->text().isEmpty() &&
        ui->subjectEdit->text().isEmpty() &&
        ui->messageEdit->toPlainText().isEmpty() &&
        ui->approvedEdit->text().isEmpty() &&
        ui->positionEdit->text().isEmpty();
    if (empty) {
        clearDraft();
        return;
    }
    QJsonObject o;
    o[QStringLiteral("incident")] = ui->incidentEdit->text();
    o[QStringLiteral("to")] = ui->toEdit->text();
    o[QStringLiteral("from")] = ui->fromEdit->text();
    o[QStringLiteral("subject")] = ui->subjectEdit->text();
    o[QStringLiteral("precedence")] =
        ui->precedenceCombo->currentIndex();
    o[QStringLiteral("date")] = ui->dateEdit->text();
    o[QStringLiteral("time")] = ui->timeEdit->text();
    o[QStringLiteral("message")] = ui->messageEdit->toPlainText();
    o[QStringLiteral("approved")] = ui->approvedEdit->text();
    o[QStringLiteral("position")] = ui->positionEdit->text();
    QFile f{draftPath()};
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument{o}.toJson(QJsonDocument::Compact));
}

void ICS213Dialog::loadDraft() {
    QFile f{draftPath()};
    if (!f.open(QIODevice::ReadOnly))
        return;
    auto const o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.isEmpty())
        return;
    ui->incidentEdit->setText(o[QStringLiteral("incident")].toString());
    ui->toEdit->setText(o[QStringLiteral("to")].toString());
    ui->fromEdit->setText(o[QStringLiteral("from")].toString());
    ui->subjectEdit->setText(o[QStringLiteral("subject")].toString());
    ui->precedenceCombo->setCurrentIndex(
        o[QStringLiteral("precedence")].toInt());
    if (auto const d = o[QStringLiteral("date")].toString();
        !d.isEmpty())
        ui->dateEdit->setText(d);
    if (auto const t = o[QStringLiteral("time")].toString();
        !t.isEmpty())
        ui->timeEdit->setText(t);
    ui->messageEdit->setPlainText(
        o[QStringLiteral("message")].toString());
    ui->approvedEdit->setText(o[QStringLiteral("approved")].toString());
    ui->positionEdit->setText(o[QStringLiteral("position")].toString());
    ui->footerLabel->setText(tr("Draft restored."));
}

void ICS213Dialog::clearDraft() { QFile::remove(draftPath()); }

void ICS213Dialog::refreshEstimate() { updateFooter(); }

void ICS213Dialog::setArqBusy(bool const busy) {
    ui->sendButton->setDisabled(busy);
    ui->arqBusyLabel->setVisible(busy);
}
