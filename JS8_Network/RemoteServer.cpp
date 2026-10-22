/**
 * @file RemoteServer.cpp
 * @brief Phone remote: the app's own small web server (see header).
 */

#include "RemoteServer.h"

#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QThread>
#include <QUrlQuery>
#include <QtDebug>

namespace {
// [phone remote] CHOSEN constants (operator plan 2026-10-21), not measured:
constexpr int kMaxFailures = 5;        // wrong pass codes before the lockout
constexpr int kLockoutSecs = 60;       // login refused this long after
constexpr int kReplyTimeoutMs = 10000; // an API request with no reply
constexpr int kKeepaliveMs = 15000;    // event-stream comment, keeps NATs open
constexpr int kMaxRequestBytes = 65536;

QByteArray newSessionToken() {
    QByteArray raw(24, Qt::Uninitialized);
    auto *gen = QRandomGenerator::system();
    for (auto &c : raw) c = static_cast<char>(gen->bounded(256));
    return raw.toHex();
}

// Equal-time compare: the time taken never says how much matched.
bool sameSecret(QByteArray const &a, QByteArray const &b) {
    if (a.isEmpty() || a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a.at(i) ^ b.at(i));
    return diff == 0;
}

QByteArray cookieValue(QByteArray const &header, QByteArray const &name) {
    for (QByteArray part : header.split(';')) {
        part = part.trimmed();
        if (part.startsWith(name + '=')) return part.mid(name.size() + 1);
    }
    return {};
}
} // namespace

RemoteServer::RemoteServer(QObject *parent) : QTcpServer(parent) {
    // [operator 2026-10-21, log "Timers cannot be started from another
    // thread"] a parent, so moveToThread takes the timer along to the
    // network thread; unparented it stayed on the GUI thread and the
    // keep-alive never ran
    m_keepalive.setParent(this);
    m_keepalive.setInterval(kKeepaliveMs);
    connect(&m_keepalive, &QTimer::timeout, this, [this]() {
        for (auto const &s : m_events)
            if (s) s->write(": keepalive\n\n");
    });
}

RemoteServer::~RemoteServer() { configure(false, m_port, m_passcode); }

void RemoteServer::configure(bool enabled, quint16 port, QString passcode) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this, [=, this]() { configure(enabled, port, passcode); },
            Qt::QueuedConnection);
        return;
    }
    bool const changed = enabled != m_enabled || port != m_port ||
                         passcode != m_passcode;
    m_enabled = enabled;
    m_port = port;
    if (passcode != m_passcode) {
        m_passcode = passcode;
        endSession(); // a new pass code logs the phone out
    }
    if (!changed && (isListening() == enabled)) return;
    if (isListening()) close();
    if (!enabled) {
        endSession();
        qWarning() << "[REMOTE] off";
        return;
    }
    bool const ok = listen(QHostAddress::Any, m_port);
    qWarning() << "[REMOTE] listening" << ok << "port" << m_port
               << (ok ? QString() : errorString());
}

void RemoteServer::incomingConnection(qintptr handle) {
    auto *socket = new QTcpSocket(this);
    if (!socket->setSocketDescriptor(handle)) {
        socket->deleteLater();
        return;
    }
    connect(socket, &QTcpSocket::readyRead, this,
            [this, socket]() { onReadyRead(socket); });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
        m_buffers.remove(socket);
        bool wasEvents = false;
        for (int i = m_events.size() - 1; i >= 0; --i)
            if (!m_events.at(i) || m_events.at(i) == socket) {
                m_events.removeAt(i);
                wasEvents = true;
            }
        if (wasEvents)
            qWarning() << "[REMOTE] event stream closed, streams=" << m_events.size();
        if (wasEvents && m_events.isEmpty()) {
            m_keepalive.stop();
            emit phoneConnected(false);
        }
        socket->deleteLater();
    });
}

// One request per connection (the page never pipelines). Headers up to
// the blank line, then Content-Length bytes of body.
void RemoteServer::onReadyRead(QTcpSocket *socket) {
    QByteArray &buf = m_buffers[socket];
    buf += socket->readAll();
    if (buf.size() > kMaxRequestBytes) {
        reply(socket, 413, "Payload Too Large", "text/plain", "too large");
        return;
    }
    int const end = buf.indexOf("\r\n\r\n");
    if (end < 0) return;
    Request req;
    QList<QByteArray> lines = buf.left(end).split('\n');
    QList<QByteArray> const first = lines.takeFirst().trimmed().split(' ');
    if (first.size() < 2) {
        reply(socket, 400, "Bad Request", "text/plain", "bad request");
        return;
    }
    req.method = first.at(0);
    QByteArray const target = first.at(1);
    int const q = target.indexOf('?');
    req.path = q < 0 ? target : target.left(q);
    if (q >= 0)
        for (QByteArray const &kv : target.mid(q + 1).split('&')) {
            int const eq = kv.indexOf('=');
            if (eq > 0) req.query.insert(kv.left(eq), QByteArray::fromPercentEncoding(kv.mid(eq + 1)));
        }
    for (QByteArray const &line : lines) {
        int const colon = line.indexOf(':');
        if (colon > 0)
            req.headers.insert(line.left(colon).trimmed().toLower(),
                               line.mid(colon + 1).trimmed());
    }
    int const length = req.headers.value("content-length", "0").toInt();
    if (buf.size() < end + 4 + length) return; // body still arriving
    req.body = buf.mid(end + 4, length);
    m_buffers.remove(socket);
    handle(socket, req);
}

void RemoteServer::handle(QTcpSocket *socket, Request const &req) {
    if (req.method == "GET" && (req.path == "/" || req.path == "/index.html"))
        return serveFile(socket, QStringLiteral(":/remote/index.html"),
                         "text/html; charset=utf-8");
    if (req.method == "GET" && req.path == "/remote.js")
        return serveFile(socket, QStringLiteral(":/remote/remote.js"),
                         "application/javascript; charset=utf-8");
    if (req.method == "GET" && req.path == "/remote.css")
        return serveFile(socket, QStringLiteral(":/remote/remote.css"),
                         "text/css; charset=utf-8");
    // [operator 2026-10-21] the page icon (the Subspace bolt) and the
    // manifest Android reads for "Add to Home screen"; no login needed,
    // like the page itself
    if (req.method == "GET") {
        static QHash<QByteArray, QByteArray> const assets = {
            {"/favicon-32.png", "image/png"},
            {"/icon-192.png", "image/png"},
            {"/icon-512.png", "image/png"},
            {"/apple-touch-icon.png", "image/png"},
            {"/manifest.webmanifest", "application/manifest+json"},
        };
        if (auto const it = assets.constFind(req.path); it != assets.constEnd())
            return serveFile(socket, QStringLiteral(":/remote") + QString::fromLatin1(req.path),
                             it.value());
        if (req.path == "/favicon.ico")
            return serveFile(socket, QStringLiteral(":/remote/favicon-32.png"), "image/png");
    }
    if (req.method == "POST" && req.path == "/login")
        return login(socket, req);
    if (!authorized(req)) {
        qWarning() << "[REMOTE] 401" << req.method << req.path << "cookie="
                   << !req.headers.value("cookie").isEmpty() << "session="
                   << !m_session.isEmpty();
        return reply(socket, 401, "Unauthorized", "application/json",
                     R"({"error":"login"})");
    }
    if (req.method == "GET" && req.path == "/events")
        return openEvents(socket);
    if (req.method == "POST" && req.path == "/api") {
        Message m;
        try {
            m = Message::fromJson(req.body);
        } catch (...) {
        }
        if (m.type().isEmpty())
            return reply(socket, 400, "Bad Request", "application/json",
                         R"({"error":"not an API message"})");
        // the page numbers its requests; the dispatcher's reply carries
        // the same _ID back, and send() answers this POST with it
        qint64 const id = m.ensureId();
        m_pending.insert(id, socket);
        QTimer::singleShot(kReplyTimeoutMs, socket, [this, id]() {
            if (auto s = m_pending.take(id))
                reply(s, 200, "OK", "application/json",
                      R"({"type":"API.NO_REPLY"})");
        });
        emit message(m);
        return;
    }
    reply(socket, 404, "Not Found", "text/plain", "not found");
}

// [operator 2026-10-21, Android] The phone's browser never sent the login
// cookie back (log: every /api "cookie= false" right after a good login),
// so the session key also travels in the X-Session header (/api) and the
// address (/events?s=..., EventSource cannot set headers). Any one will do.
bool RemoteServer::authorized(Request const &req) const {
    return sameSecret(cookieValue(req.headers.value("cookie"), "js8remote"),
                      m_session) ||
           sameSecret(req.headers.value("x-session"), m_session) ||
           sameSecret(req.query.value("s"), m_session);
}

void RemoteServer::login(QTcpSocket *socket, Request const &req) {
    QDateTime const now = QDateTime::currentDateTimeUtc();
    if (m_lockedUntil.isValid() && now < m_lockedUntil) {
        qWarning() << "[REMOTE] login refused: locked out from"
                   << socket->peerAddress().toString();
        return reply(socket, 429, "Too Many Requests", "application/json",
                     R"({"error":"locked"})");
    }
    QString const given = QJsonDocument::fromJson(req.body)
                              .object()
                              .value(QStringLiteral("passcode"))
                              .toString()
                              .trimmed()
                              .toUpper();
    if (m_passcode.isEmpty() ||
        !sameSecret(given.toUtf8(), m_passcode.toUpper().toUtf8())) {
        if (++m_failures >= kMaxFailures) {
            m_failures = 0;
            m_lockedUntil = now.addSecs(kLockoutSecs);
            qWarning() << "[REMOTE] login LOCKED for" << kLockoutSecs
                       << "s after" << kMaxFailures << "wrong pass codes, last from"
                       << socket->peerAddress().toString();
        } else {
            qWarning() << "[REMOTE] wrong pass code from"
                       << socket->peerAddress().toString();
        }
        return reply(socket, 403, "Forbidden", "application/json",
                     R"({"error":"passcode"})");
    }
    m_failures = 0;
    endSession(); // one phone at a time: the newest LOGIN wins
    m_session = newSessionToken();
    qWarning() << "[REMOTE] login from" << socket->peerAddress().toString();
    reply(socket, 200, "OK", "application/json",
          R"({"ok":true,"session":")" + m_session + R"("})",
          {"Set-Cookie: js8remote=" + m_session +
           "; Path=/; HttpOnly; SameSite=Strict"});
}

void RemoteServer::serveFile(QTcpSocket *socket, QString const &resource,
                             QByteArray const &contentType) {
    QFile f(resource);
    if (!f.open(QIODevice::ReadOnly))
        return reply(socket, 404, "Not Found", "text/plain", "not found");
    reply(socket, 200, "OK", contentType, f.readAll(),
          {"Cache-Control: no-cache"});
}

void RemoteServer::openEvents(QTcpSocket *socket) {
    socket->write("HTTP/1.1 200 OK\r\n"
                  "Content-Type: text/event-stream\r\n"
                  "Cache-Control: no-cache\r\n"
                  "Connection: keep-alive\r\n\r\n"
                  ": open\n\n");
    m_events.append(socket);
    qWarning() << "[REMOTE] event stream open, streams=" << m_events.size();
    if (!m_keepalive.isActive()) m_keepalive.start();
    emit phoneConnected(true);
}

// Ends the session: the cookie stops working and open streams close.
void RemoteServer::endSession() {
    m_session.clear();
    for (auto const &s : m_events)
        if (s) s->disconnectFromHost();
    m_events.clear();
    m_keepalive.stop();
}

void RemoteServer::send(Message const &message) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this, [this, message]() { send(message); }, Qt::QueuedConnection);
        return;
    }
    QByteArray const json = message.toJson();
    if (message.id() > 0) {
        // a reply: to the POST that asked for it, if still waiting
        if (auto s = m_pending.take(message.id()))
            reply(s, 200, "OK", "application/json", json);
        return;
    }
    for (auto const &s : m_events)
        if (s) s->write("data: " + json + "\n\n");
}

void RemoteServer::reply(QTcpSocket *socket, int status, QByteArray const &reason,
                         QByteArray const &contentType, QByteArray const &body,
                         QList<QByteArray> const &extraHeaders) {
    if (!socket) return;
    QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason +
                      "\r\nContent-Type: " + contentType +
                      "\r\nContent-Length: " + QByteArray::number(body.size()) +
                      "\r\nConnection: close\r\n";
    for (QByteArray const &h : extraHeaders) head += h + "\r\n";
    socket->write(head + "\r\n" + body);
    socket->disconnectFromHost();
}
