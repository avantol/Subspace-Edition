#ifndef REMOTESERVER_H
#define REMOTESERVER_H

/**
 * @file RemoteServer.h
 * @brief Phone remote: the app's own small web server.
 *
 * [phone remote, operator 2026-10-21] A phone browser runs the station
 * the way POTACAT's ECHOCAT does, without the audio: JS8 and JTTY are
 * text. The phone loads one page from here and speaks the EXISTING JSON
 * API -- the same messages, the same dispatcher (networkMessage), the
 * same replies and events as a TCP API client:
 *
 *   GET  /, /remote.js, /remote.css  the page (Qt resources :/remote/)
 *   POST /login                       {"passcode":"..."} -> session cookie
 *   POST /api                         one API message in, its reply out
 *   GET  /events                      Server-Sent Events: every broadcast
 *
 * Reach is the home network or Tailscale; nothing is meant to face the
 * internet. A pass code is ALWAYS required; five wrong tries lock the
 * login for 60 s. One phone at a time: a NEW authenticated login ends
 * the previous session -- an unauthenticated connection never can.
 * Lives on the network thread, like MessageServer.
 */

#include "JS8_Main/Message.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

class RemoteServer : public QTcpServer {
    Q_OBJECT
  public:
    explicit RemoteServer(QObject *parent = nullptr);
    ~RemoteServer() override;

    // [phone remote] the defaults; the settings live in the profile's
    // SsRemote group (UI_Constructor)
    static constexpr quint16 kDefaultPort = 2443;

  signals:
    // An API message from the phone, for UI_Constructor::networkMessage.
    void message(Message const &message);
    // A login succeeded / the phone's event stream opened or closed.
    void phoneConnected(bool connected);

  public slots:
    // enabled = listen on every interface at `port`; disabled = nothing
    // listens and every session ends.
    void configure(bool enabled, quint16 port, QString passcode);
    void send(Message const &message);

  protected:
    void incomingConnection(qintptr handle) override;

  private:
    struct Request {
        QByteArray method, path, body;
        QHash<QByteArray, QByteArray> headers; // lower-case names
        QHash<QByteArray, QByteArray> query;   // ?name=value
    };
    void onReadyRead(QTcpSocket *socket);
    void handle(QTcpSocket *socket, Request const &req);
    bool authorized(Request const &req) const;
    void login(QTcpSocket *socket, Request const &req);
    void serveFile(QTcpSocket *socket, QString const &resource,
                   QByteArray const &contentType);
    void openEvents(QTcpSocket *socket);
    void endSession();
    static void reply(QTcpSocket *socket, int status, QByteArray const &reason,
                      QByteArray const &contentType, QByteArray const &body,
                      QList<QByteArray> const &extraHeaders = {});

    bool m_enabled = false;
    quint16 m_port = kDefaultPort;
    QString m_passcode;
    QByteArray m_session;                      // cookie value, empty = none
    int m_failures = 0;                        // wrong pass codes in a row
    QDateTime m_lockedUntil;                   // login refused until then
    QHash<QTcpSocket *, QByteArray> m_buffers; // partial requests
    QList<QPointer<QTcpSocket>> m_events;      // open event streams
    QHash<qint64, QPointer<QTcpSocket>> m_pending; // API _ID -> waiting POST
    QTimer m_keepalive;
};

#endif // REMOTESERVER_H
