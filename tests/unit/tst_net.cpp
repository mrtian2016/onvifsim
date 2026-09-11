// net/ 层单测：HTTP 解析与路由、HttpExchange 的异步回包、Digest / Basic 鉴权
// （含 E8 双挑战与 E9 严格参数）、按 peer 挑源地址、IP 别名命令串。
//
// 客户端与服务器跑在同一个事件循环里，所以一律不能用 waitForReadyRead 这类
// 阻塞等待（它只驱动自己那个 socket，服务器那边根本转不起来），
// 统一用 QTest::qWait 轮询。

#include <QtTest/QtTest>

#include "core/CameraModel.h"
#include "core/Quirks.h"
#include "net/HttpAuth.h"
#include "net/HttpServer.h"
#include "net/HttpTypes.h"
#include "net/IpAlias.h"
#include "net/NetUtil.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QElapsedTimer>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <functional>

using namespace onvifsim;

namespace {

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs)
        QTest::qWait(5);
    return predicate();
}

// 收到的字节是不是一条完整响应（头 + Content-Length 指定的 body）。
bool responseComplete(const QByteArray &data)
{
    const qsizetype end = data.indexOf("\r\n\r\n");
    if (end < 0)
        return false;
    const QByteArray head = data.left(end).toLower();
    const qsizetype at = head.indexOf("content-length:");
    if (at < 0)
        return true;
    const qsizetype lineEnd = head.indexOf("\r\n", at);
    const QByteArray value = head.mid(at + 15, (lineEnd < 0 ? head.size() : lineEnd) - at - 15);
    return data.size() >= end + 4 + value.trimmed().toLongLong();
}

// 裸 HTTP 客户端：只管发字节、攒响应，不做任何解析上的善意补齐。
class RawClient : public QObject
{
public:
    explicit RawClient(quint16 port)
    {
        connect(&m_socket, &QTcpSocket::readyRead, this, [this] { m_data += m_socket.readAll(); });
        connect(&m_socket, &QTcpSocket::disconnected, this, [this] { m_closed = true; });
        // 服务器 abort() 过来的是 RST，未必走 disconnected，errorOccurred 也要收。
        connect(&m_socket, &QTcpSocket::errorOccurred, this,
                [this](QAbstractSocket::SocketError) { m_closed = true; });
        m_socket.connectToHost(QHostAddress::LocalHost, port);
    }

    bool waitConnected()
    {
        return waitUntil([this] { return m_socket.state() == QAbstractSocket::ConnectedState; });
    }

    void send(const QByteArray &bytes)
    {
        m_socket.write(bytes);
        m_socket.flush();
    }

    bool waitForResponse()
    {
        return waitUntil([this] { return responseComplete(m_data) || m_closed; });
    }

    bool waitForClose()
    {
        return waitUntil([this] {
            return m_closed || m_socket.state() == QAbstractSocket::UnconnectedState;
        });
    }
    void abort() { m_socket.abort(); }
    void clear() { m_data.clear(); }
    const QByteArray &data() const { return m_data; }
    bool isClosed() const { return m_closed; }

    int statusCode() const
    {
        const QList<QByteArray> parts = m_data.left(m_data.indexOf("\r\n")).split(' ');
        return parts.size() > 1 ? parts.at(1).toInt() : 0;
    }

    QByteArray body() const
    {
        const qsizetype end = m_data.indexOf("\r\n\r\n");
        return end < 0 ? QByteArray() : m_data.mid(end + 4);
    }

    // 同名头可能有多条（E8 的双挑战），全都要拿到。
    QList<QByteArray> headerValues(const QByteArray &name) const
    {
        QList<QByteArray> values;
        const qsizetype end = m_data.indexOf("\r\n\r\n");
        const QList<QByteArray> lines = m_data.left(end < 0 ? m_data.size() : end).split('\n');
        for (const QByteArray &line : lines) {
            const QByteArray trimmed = line.trimmed();
            if (trimmed.toLower().startsWith(name.toLower() + ':'))
                values.append(trimmed.mid(name.size() + 1).trimmed());
        }
        return values;
    }

private:
    QTcpSocket m_socket;
    QByteArray m_data;
    bool m_closed = false;
};

CameraModel modelWithUser()
{
    CameraModel model;
    model.users = { User{ QStringLiteral("admin"), QStringLiteral("admin123"),
                          UserLevel::Administrator } };
    return model;
}

QByteArray nonceOf(const QByteArray &challenge)
{
    return HttpAuth::parseAuthParams(challenge).value("nonce");
}

} // namespace

class TstNet : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void requestLineAndHeaders();
    void percentDecodedPath();
    void keepAliveReusesConnection();
    void keepAliveHeaderVariants_data();
    void keepAliveHeaderVariants();
    void chunkedBodyReturns411();
    void oversizedBodyReturns413();
    void malformedRequestLineReturns400();
    void exactRouteWins();
    void prefixRouteLongestMatch();
    void unknownPathReturns404();
    void methodMismatchReturns405();
    void fallbackHandler();
    void deferredResponseIsIdempotent();
    void abortedOnClientDisconnect();
    void rawOverrideBypassesHeaders();
    void delayAndSlowSend();
    void dropConnectionSendsNothing();
    void streamKeepsConnectionOpen();
    void streamStopsWhenClientDisconnects();
    void suspendAndResume();

    void digestRfc2617Vector();
    void parseAuthParamsHandlesQuotes();
    void challengeShapeAndDualOrder();
    void digestRoundTrip();
    void strictDigestParamsRejectsAlgorithm();
    void staleNonce();
    void basicAuth();
    void dualChallengeOverHttp();

    void sameSubnetAndLinkLocal();
    void preferredLocalAddressBound();
    void preferredLocalAddressLoopbackPeer();
    void preferredLocalAddressLanPeer();
    void interfaceLookupAndFreePort();

    void ipAliasCommandStrings();
    void ipAliasRejectsBadInput();

private:
    HttpServer *m_server = nullptr;
    quint16 m_port = 0;
};

void TstNet::init()
{
    m_server = new HttpServer(nullptr);
    QString error;
    QVERIFY2(m_server->listen(QHostAddress::LocalHost, 0, &error), qPrintable(error));
    m_port = m_server->serverPort();
    QVERIFY(m_port != 0);
}

void TstNet::cleanup()
{
    delete m_server;
    m_server = nullptr;
}

void TstNet::requestLineAndHeaders()
{
    HttpRequest captured;
    bool got = false;
    m_server->addRoute("POST", QStringLiteral("/onvif/device_service"),
                       [&](const HttpRequest &request, const HttpExchangePtr &exchange) {
                           captured = request;
                           got = true;
                           exchange->respond(HttpResponse::soap("<ok/>"));
                       });

    RawClient client(m_port);
    QVERIFY(client.waitConnected());
    client.send("POST /onvif/device_service?a=1&b=x%20y HTTP/1.1\r\n"
                "Host: 1.2.3.4:8000\r\n"
                "CoNtEnT-TyPe: application/soap+xml; charset=utf-8\r\n"
                "X-Multi: one\r\n"
                "X-Multi: two\r\n"
                "Content-Length: 5\r\n"
                "\r\n"
                "hello");
    QVERIFY(waitUntil([&] { return got; }));

    QCOMPARE(captured.method, QByteArray("POST"));
    QCOMPARE(captured.path, QStringLiteral("/onvif/device_service"));
    QCOMPARE(captured.rawTarget, QStringLiteral("/onvif/device_service?a=1&b=x%20y"));
    QCOMPARE(captured.query.queryItemValue(QStringLiteral("b"), QUrl::FullyDecoded),
             QStringLiteral("x y"));
    QCOMPARE(captured.version, QByteArray("HTTP/1.1"));
    QCOMPARE(captured.body, QByteArray("hello"));

    // key 一律小写存，查询时大小写随便写。
    QVERIFY(captured.headers.contains("content-type"));
    QVERIFY(!captured.headers.contains("CoNtEnT-TyPe"));
    QCOMPARE(captured.header("CONTENT-TYPE"), QByteArray("application/soap+xml; charset=utf-8"));
    QVERIFY(captured.hasHeader("Content-Length"));
    QCOMPARE(captured.contentType(), QStringLiteral("application/soap+xml; charset=utf-8"));
    QCOMPARE(captured.hostHeader(), QStringLiteral("1.2.3.4:8000"));
    // 同名头按 RFC 7230 合成一条。
    QCOMPARE(captured.header("x-multi"), QByteArray("one, two"));
    QVERIFY(captured.wantsKeepAlive());
    QVERIFY(captured.peerString().startsWith(QStringLiteral("127.0.0.1:")));
    QCOMPARE(captured.localPort, m_port);

    QVERIFY(client.waitForResponse());
    QCOMPARE(client.statusCode(), 200);
    QCOMPARE(client.body(), QByteArray("<ok/>"));
    QCOMPARE(client.headerValues("Connection"), QList<QByteArray>{ QByteArray("keep-alive") });
}

void TstNet::percentDecodedPath()
{
    bool hit = false;
    m_server->addRoute(QByteArray(), QStringLiteral("/onvif/device_service"),
                       [&](const HttpRequest &, const HttpExchangePtr &exchange) {
                           hit = true;
                           exchange->respondText(200, QStringLiteral("ok"));
                       });
    RawClient client(m_port);
    QVERIFY(client.waitConnected());
    client.send("GET /onvif/device%5Fservice HTTP/1.1\r\nHost: h\r\n\r\n");
    QVERIFY(client.waitForResponse());
    QVERIFY(hit);
    QCOMPARE(client.statusCode(), 200);
}

void TstNet::keepAliveReusesConnection()
{
    int hits = 0;
    m_server->addRoute("GET", QStringLiteral("/ping"),
                       [&](const HttpRequest &, const HttpExchangePtr &exchange) {
                           ++hits;
                           exchange->respondText(200, QStringLiteral("pong"));
                       });
    RawClient client(m_port);
    QVERIFY(client.waitConnected());
    client.send("GET /ping HTTP/1.1\r\nHost: h\r\n\r\n");
    QVERIFY(client.waitForResponse());
    QCOMPARE(client.statusCode(), 200);
    QVERIFY(!client.isClosed());

    client.clear();
    client.send("GET /ping HTTP/1.1\r\nHost: h\r\n\r\n");
    QVERIFY(client.waitForResponse());
    QCOMPARE(hits, 2);
    QVERIFY(!client.isClosed());
}

void TstNet::keepAliveHeaderVariants_data()
{
    QTest::addColumn<QByteArray>("version");
    QTest::addColumn<QByteArray>("connection");
    QTest::addColumn<bool>("expected");

    QTest::newRow("1.1 默认长连接") << QByteArray("HTTP/1.1") << QByteArray() << true;
    QTest::newRow("1.1 显式 close") << QByteArray("HTTP/1.1") << QByteArray("close") << false;
    QTest::newRow("1.1 大小写混写") << QByteArray("HTTP/1.1") << QByteArray("Close") << false;
    QTest::newRow("1.1 token 列表") << QByteArray("HTTP/1.1") << QByteArray("TE, close") << false;
    QTest::newRow("1.0 默认短连接") << QByteArray("HTTP/1.0") << QByteArray() << false;
    QTest::newRow("1.0 keep-alive") << QByteArray("HTTP/1.0") << QByteArray("Keep-Alive") << true;

    QTest::newRow("1.1 keep-alive") << QByteArray("HTTP/1.1") << QByteArray("keep-alive") << true;
}

void TstNet::keepAliveHeaderVariants()
{
    QFETCH(QByteArray, version);
    QFETCH(QByteArray, connection);
    QFETCH(bool, expected);

    HttpRequest request;
    request.version = version;
    if (!connection.isEmpty())
        request.headers.insert("connection", connection);
    QCOMPARE(request.wantsKeepAlive(), expected);
}

void TstNet::chunkedBodyReturns411()
{
    m_server->addRoute("POST", QStringLiteral("/x"),
                       [](const HttpRequest &, const HttpExchangePtr &exchange) {
                           exchange->respondText(200, QStringLiteral("should not happen"));
                       });
    RawClient client(m_port);
    QVERIFY(client.waitConnected());
    client.send("POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
                "5\r\nhello\r\n0\r\n\r\n");
    QVERIFY(client.waitForResponse());
    QCOMPARE(client.statusCode(), 411);
    QVERIFY(client.waitForClose());
}

void TstNet::oversizedBodyReturns413()
{
    m_server->setMaxBodySize(64);
    m_server->addRoute("POST", QStringLiteral("/x"),
                       [](const HttpRequest &, const HttpExchangePtr &exchange) {
                           exchange->respondText(200, QStringLiteral("should not happen"));
                       });
    RawClient client(m_port);
    QVERIFY(client.waitConnected());
    client.send("POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 100\r\n\r\n");
    QVERIFY(client.waitForResponse());
    QCOMPARE(client.statusCode(), 413);
}

void TstNet::malformedRequestLineReturns400()
{
    RawClient client(m_port);
    QVERIFY(client.waitConnected());
    client.send("NOT-A-REQUEST\r\nHost: h\r\n\r\n");
    QVERIFY(client.waitForResponse());
    QCOMPARE(client.statusCode(), 400);
}

void TstNet::exactRouteWins()
{
    QString hit;
    m_server->addPrefixRoute(QByteArray(), QStringLiteral("/onvif"),
                             [&](const HttpRequest &, const HttpExchangePtr &exchange) {
                                 hit = QStringLiteral("prefix");
                                 exchange->respondText(200, QStringLiteral("prefix"));
                             });
    m_server->addRoute(QByteArray(), QStringLiteral("/onvif/device_service"),
                       [&](const HttpRequest &, const HttpExchangePtr &exchange) {
                           hit = QStringLiteral("exact");
                           exchange->respondText(200, QStringLiteral("exact"));
                       });

    RawClient client(m_port);
    QVERIFY(client.waitConnected());
    client.send("GET /onvif/device_service HTTP/1.1\r\nHost: h\r\n\r\n");
    QVERIFY(client.waitForResponse());
    QCOMPARE(hit, QStringLiteral("exact"));
}

void TstNet::prefixRouteLongestMatch()
{
    QString hit;
    QString seenPath;
    m_server->addPrefixRoute(QByteArray(), QStringLiteral("/event"),
                             [&](const HttpRequest &, const HttpExchangePtr &exchange) {
                                 hit = QStringLiteral("short");
                                 exchange->respondText(200, QStringLiteral("short"));
                             });
    m_server->addPrefixRoute("POST", QStringLiteral("/event-1024_1024"),
                             [&](const HttpRequest &request, const HttpExchangePtr &exchange) {
                                 hit = QStringLiteral("long");
                                 seenPath = request.path;
                                 exchange->respondText(200, QStringLiteral("long"));
                             });

    RawClient client(m_port);
    QVERIFY(client.waitConnected());
    client.send("POST /event-1024_1024/sub7 HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n");
    QVERIFY(client.waitForResponse());
    QCOMPARE(client.statusCode(), 200);
    QCOMPARE(hit, QStringLiteral("long"));
    QCOMPARE(seenPath, QStringLiteral("/event-1024_1024/sub7"));

    // 方法对不上时退回到不限方法的短前缀，而不是 405。
    client.clear();
    client.send("GET /event-1024_1024/sub7 HTTP/1.1\r\nHost: h\r\n\r\n");
    QVERIFY(client.waitForResponse());
    QCOMPARE(hit, QStringLiteral("short"));
}

void TstNet::unknownPathReturns404()
{
    RawClient client(m_port);
    QVERIFY(client.waitConnected());
    client.send("GET /nope HTTP/1.1\r\nHost: h\r\n\r\n");
    QVERIFY(client.waitForResponse());
    QCOMPARE(client.statusCode(), 404);
}

void TstNet::methodMismatchReturns405()
{
    m_server->addRoute("GET", QStringLiteral("/only-get"),
                       [](const HttpRequest &, const HttpExchangePtr &exchange) {
                           exchange->respondText(200, QStringLiteral("ok"));
                       });
    RawClient client(m_port);
    QVERIFY(client.waitConnected());
    client.send("DELETE /only-get HTTP/1.1\r\nHost: h\r\n\r\n");
    QVERIFY(client.waitForResponse());
    QCOMPARE(client.statusCode(), 405);
}

void TstNet::fallbackHandler()
{
    m_server->setFallbackHandler([](const HttpRequest &request, const HttpExchangePtr &exchange) {
        exchange->respondText(418, request.path);
    });
    RawClient client(m_port);
    QVERIFY(client.waitConnected());
    client.send("GET /whatever HTTP/1.1\r\nHost: h\r\n\r\n");
    QVERIFY(client.waitForResponse());
    QCOMPARE(client.statusCode(), 418);
    QCOMPARE(client.body(), QByteArray("/whatever"));
}

void TstNet::deferredResponseIsIdempotent()
{
    // PullMessages 的长轮询就是这个形状：handler 拿住句柄先不回，稍后再 respond。
    HttpExchangePtr held;
    m_server->addRoute("POST", QStringLiteral("/pull"),
                       [&](const HttpRequest &, const HttpExchangePtr &exchange) {
                           held = exchange;
                       });

    RawClient client(m_port);
    QVERIFY(client.waitConnected());
    client.send("POST /pull HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n");
    QVERIFY(waitUntil([&] { return !held.isNull(); }));

    QTest::qWait(60);
    QVERIFY2(client.data().isEmpty(), "handler 还没 respond，不该有任何字节回去");
    QVERIFY(!held->isResponded());
    QVERIFY(held->isConnected());
    QCOMPARE(held->request().path, QStringLiteral("/pull"));

    held->respond(HttpResponse::text(200, QStringLiteral("late")));
    QVERIFY(held->isResponded());
    // 幂等：超时定时器和「新消息到了」可能同时触发，第二次必须没有任何效果。
    held->respondText(500, QStringLiteral("ignored"));

    QVERIFY(client.waitForResponse());
    QCOMPARE(client.statusCode(), 200);
    QCOMPARE(client.body(), QByteArray("late"));

    const qsizetype size = client.data().size();
    QTest::qWait(60);
    QCOMPARE(client.data().size(), size);   // 第二次 respond 一个字节都没发出去
}

void TstNet::abortedOnClientDisconnect()
{
    HttpExchangePtr held;
    m_server->addRoute("POST", QStringLiteral("/pull"),
                       [&](const HttpRequest &, const HttpExchangePtr &exchange) {
                           held = exchange;
                       });

    auto *client = new RawClient(m_port);
    QVERIFY(client->waitConnected());
    client->send("POST /pull HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n");
    QVERIFY(waitUntil([&] { return !held.isNull(); }));

    QSignalSpy spy(held.data(), &HttpExchange::aborted);
    client->abort();
    delete client;

    QVERIFY(waitUntil([&] { return spy.count() == 1; }));
    QVERIFY(!held->isConnected());
    // 客户端走了以后再回包只是空转，不能崩。
    held->respondText(200, QStringLiteral("nobody is listening"));
    QVERIFY(held->isResponded());
}

void TstNet::rawOverrideBypassesHeaders()
{
    // F1：固件回一坨畸形字节，客户端只能把原文塞进异常里。
    m_server->addRoute("POST", QStringLiteral("/x"),
                       [](const HttpRequest &request, const HttpExchangePtr &exchange) {
                           HttpResponse response;
                           response.rawOverride = "HTTP/1.1 500 Internal Server Error\r\n\r\n"
                                                  + request.body;
                           exchange->respond(response);
                       });
    RawClient client(m_port);
    QVERIFY(client.waitConnected());
    client.send("POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\n\r\nEcHo");
    QVERIFY(client.waitForClose());
    QCOMPARE(client.data(), QByteArray("HTTP/1.1 500 Internal Server Error\r\n\r\nEcHo"));
    QVERIFY(!client.data().contains("Server: onvifsim"));   // 头一个字节都没被加工
}

void TstNet::delayAndSlowSend()
{
    m_server->addRoute("GET", QStringLiteral("/slow"),
                       [](const HttpRequest &, const HttpExchangePtr &exchange) {
                           HttpResponse response = HttpResponse::text(200,
                               QStringLiteral("0123456789abcdef"));
                           response.delayMs = 40;
                           response.slowSendChunk = 8;
                           response.slowSendIntervalMs = 5;
                           response.forceClose = true;
                           exchange->respond(response);
                       });
    RawClient client(m_port);
    QVERIFY(client.waitConnected());
    QElapsedTimer timer;
    timer.start();
    client.send("GET /slow HTTP/1.1\r\nHost: h\r\n\r\n");
    QVERIFY(client.waitForResponse());
    QVERIFY2(timer.elapsed() >= 40, "delayMs 应该真的把回包推后");
    QCOMPARE(client.statusCode(), 200);
    QCOMPARE(client.body(), QByteArray("0123456789abcdef"));
    QVERIFY(client.waitForClose());   // forceClose
}

void TstNet::dropConnectionSendsNothing()
{
    m_server->addRoute("GET", QStringLiteral("/drop"),
                       [](const HttpRequest &, const HttpExchangePtr &exchange) {
                           HttpResponse response;
                           response.dropConnection = true;
                           exchange->respond(response);
                       });
    RawClient client(m_port);
    QVERIFY(client.waitConnected());
    client.send("GET /drop HTTP/1.1\r\nHost: h\r\n\r\n");
    QVERIFY(client.waitForClose());
    QVERIFY(client.data().isEmpty());
}


void TstNet::streamKeepsConnectionOpen()
{
    // 海康 alertStream / 大华 attach 的形状：头发完连接不关，有事件推一段。
    HttpExchangePtr held;
    m_server->addRoute("GET", QStringLiteral("/ISAPI/Event/notification/alertStream"),
                       [&](const HttpRequest &, const HttpExchangePtr &exchange) {
                           HttpResponse head;
                           head.setHeader("Content-Type",
                                          "multipart/mixed; boundary=onvifsimboundary");
                           exchange->beginStream(head);
                           held = exchange;
                       });

    RawClient client(m_port);
    QVERIFY(client.waitConnected());
    client.send("GET /ISAPI/Event/notification/alertStream HTTP/1.1\r\nHost: h\r\n\r\n");
    QVERIFY(waitUntil([&] { return !held.isNull() && client.data().contains("\r\n\r\n"); }));

    QCOMPARE(client.statusCode(), 200);
    QVERIFY(client.headerValues("Content-Type").first().startsWith("multipart/mixed"));
    // 长度天生未知，绝不能补 Content-Length，否则客户端会照它截断。
    QVERIFY(client.headerValues("Content-Length").isEmpty());
    QVERIFY(held->isStreaming());
    QVERIFY(!held->isResponded());

    for (int i = 1; i <= 3; ++i) {
        const QByteArray chunk = "--onvifsimboundary\r\nContent-Type: application/xml\r\n\r\n"
                                 "<EventNotificationAlert seq=\"" + QByteArray::number(i)
            + "\"/>\r\n";
        QVERIFY(held->writeChunk(chunk));
        QVERIFY(waitUntil([&] {
            return client.data().contains("seq=\"" + QByteArray::number(i) + "\"");
        }));
    }
    QVERIFY(!client.isClosed());   // 推了三段还挂着

    // 流式期间 respond() 必须被忽略，不能往流里插一个完整响应。
    const qsizetype before = client.data().size();
    held->respond(HttpResponse::text(500, QStringLiteral("nope")));
    QTest::qWait(60);
    QCOMPARE(client.data().size(), before);
    QVERIFY(!client.data().contains("nope"));

    held->endStream();
    QVERIFY(client.waitForClose());
    QVERIFY(!held->isStreaming());
    QVERIFY(held->isResponded());
}

void TstNet::streamStopsWhenClientDisconnects()
{
    HttpExchangePtr held;
    m_server->addRoute("GET", QStringLiteral("/cgi-bin/eventManager.cgi"),
                       [&](const HttpRequest &, const HttpExchangePtr &exchange) {
                           HttpResponse head;
                           head.setHeader("Content-Type", "text/plain");
                           exchange->beginStream(head);
                           held = exchange;
                       });

    auto *client = new RawClient(m_port);
    QVERIFY(client->waitConnected());
    client->send("GET /cgi-bin/eventManager.cgi?action=attach&codes=[All] HTTP/1.1\r\n"
                 "Host: h\r\n\r\n");
    QVERIFY(waitUntil([&] { return !held.isNull() && client->data().contains("\r\n\r\n"); }));
    QVERIFY(held->writeChunk("Code=VideoMotion;action=Start;index=0\r\n"));

    // 客户端走人 → aborted()，推送方据此停掉自己的定时器。
    QSignalSpy spy(held.data(), &HttpExchange::aborted);
    client->abort();
    delete client;
    QVERIFY(waitUntil([&] { return spy.count() == 1; }));

    QVERIFY(!held->isConnected());
    // 连接已经没了：继续推只会拿到 false，绝不能碰悬垂的 socket。
    QVERIFY(!held->writeChunk("Code=VideoMotion;action=Stop;index=0\r\n"));
    held->endStream();             // 收尾同样是安全的空转
}

void TstNet::suspendAndResume()
{
    m_server->addRoute("GET", QStringLiteral("/ping"),
                       [](const HttpRequest &, const HttpExchangePtr &exchange) {
                           exchange->respondText(200, QStringLiteral("pong"));
                       });
    m_server->suspend();
    QVERIFY(m_server->isSuspended());
    QVERIFY(!m_server->isListening());

    QString error;
    m_server->resume(&error);
    QVERIFY2(m_server->isListening(), qPrintable(error));
    QVERIFY(!m_server->isSuspended());
    QCOMPARE(m_server->serverPort(), m_port);   // 端口不变，路由也还在

    RawClient client(m_port);
    QVERIFY(client.waitConnected());
    client.send("GET /ping HTTP/1.1\r\nHost: h\r\n\r\n");
    QVERIFY(client.waitForResponse());
    QCOMPARE(client.statusCode(), 200);
}

void TstNet::digestRfc2617Vector()
{
    // RFC 2617 §3.5 的示例，一个字节都不能差。
    const QByteArray response = HttpAuth::digestResponse(
        "Mufasa", "testrealm@host.com", "Circle Of Life", "GET", "/dir/index.html",
        "dcd98b7102dd2f0e8b11d0f600bfb0c093", "00000001", "0a4f113b", "auth");
    QCOMPARE(response, QByteArray("6629fae49393a05397450978507c4ef1"));

    // qop 缺省时走 RFC 2069 的两段式。
    const QByteArray legacy = HttpAuth::digestResponse(
        "Mufasa", "testrealm@host.com", "Circle Of Life", "GET", "/dir/index.html",
        "dcd98b7102dd2f0e8b11d0f600bfb0c093", QByteArray(), QByteArray(), QByteArray());
    QCOMPARE(legacy, QByteArray("670fd8c2df070c60b045671b8b24ff02"));
}

void TstNet::parseAuthParamsHandlesQuotes()
{
    const QHash<QByteArray, QByteArray> params = HttpAuth::parseAuthParams(
        "Digest username=\"Mufasa\", realm=\"testrealm@host.com\", "
        "nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\", uri=\"/dir/index.html?a=1,2\", "
        "qop=auth, nc=00000001, cnonce=\"0a4f113b\", "
        "response=\"6629fae49393a05397450978507c4ef1\", algorithm=MD5");
    QCOMPARE(params.value("username"), QByteArray("Mufasa"));
    QCOMPARE(params.value("realm"), QByteArray("testrealm@host.com"));
    QCOMPARE(params.value("uri"), QByteArray("/dir/index.html?a=1,2"));   // 引号里的逗号不算分隔
    QCOMPARE(params.value("qop"), QByteArray("auth"));
    QCOMPARE(params.value("nc"), QByteArray("00000001"));
    QCOMPARE(params.value("algorithm"), QByteArray("MD5"));
    QCOMPARE(int(params.size()), 9);
}

void TstNet::challengeShapeAndDualOrder()
{
    HttpAuth auth(QStringLiteral("onvifsim"));
    Quirks quirks;

    const QList<QByteArray> plain = auth.challenges(quirks);
    QCOMPARE(plain.size(), 1);
    QVERIFY(plain.first().startsWith("Digest "));
    QVERIFY(plain.first().contains("realm=\"onvifsim\""));
    QVERIFY(plain.first().contains("qop=\"auth\""));
    QVERIFY(!plain.first().contains("stale"));
    // 挑战里故意不写 algorithm：E9 判的就是「挑战没给过的参数」。
    QVERIFY(!plain.first().contains("algorithm"));
    QVERIFY(!nonceOf(plain.first()).isEmpty());

    QVERIFY(auth.challenges(quirks, true).first().contains("stale=true"));

    // E8：两条挑战，顺序可配。
    quirks.setEnabled(QuirkId::AuthDualChallenge, true);
    const QList<QByteArray> digestFirst = auth.challenges(quirks);
    QCOMPARE(digestFirst.size(), 2);
    QVERIFY(digestFirst.at(0).startsWith("Digest "));
    QVERIFY(digestFirst.at(1).startsWith("Basic realm=\"onvifsim\""));

    quirks.setParam(QuirkId::AuthDualChallenge, QStringLiteral("order"),
                    QStringLiteral("basic_first"));
    const QList<QByteArray> basicFirst = auth.challenges(quirks);
    QCOMPARE(basicFirst.size(), 2);
    QVERIFY(basicFirst.at(0).startsWith("Basic "));
    QVERIFY(basicFirst.at(1).startsWith("Digest "));
}

void TstNet::digestRoundTrip()
{
    HttpAuth auth(QStringLiteral("onvifsim"));
    Quirks quirks;
    const CameraModel model = modelWithUser();

    const QByteArray challenge = auth.challenges(quirks).first();
    const QByteArray nonce = nonceOf(challenge);
    const QByteArray uri = "/onvif/device_service";
    const QByteArray expected = HttpAuth::digestResponse("admin", "onvifsim", "admin123", "POST",
                                                         uri, nonce, "00000001", "abc", "auth");
    const QByteArray header = "Digest username=\"admin\", realm=\"onvifsim\", nonce=\"" + nonce
        + "\", uri=\"" + uri + "\", qop=auth, nc=00000001, cnonce=\"abc\", response=\"" + expected
        + "\"";

    const AuthResult ok = auth.verify(header, "POST", uri, model, quirks);
    QVERIFY2(ok.authenticated, qPrintable(ok.failureReason));
    QCOMPARE(ok.username, QStringLiteral("admin"));
    QCOMPARE(ok.scheme, AuthScheme::Digest);
    QVERIFY(ok.level == UserLevel::Administrator);

    // 口令不对 → 摘要对不上，且不是 stale。
    const QByteArray wrong = "Digest username=\"admin\", realm=\"onvifsim\", nonce=\"" + nonce
        + "\", uri=\"" + uri + "\", qop=auth, nc=00000001, cnonce=\"abc\", "
          "response=\"00000000000000000000000000000000\"";
    const AuthResult bad = auth.verify(wrong, "POST", uri, model, quirks);
    QVERIFY(!bad.authenticated);
    QVERIFY(!bad.stale);

    // 没有 Authorization 头。
    const AuthResult none = auth.verify(QByteArray(), "POST", uri, model, quirks);
    QVERIFY(!none.authenticated);
    QCOMPARE(none.scheme, AuthScheme::None);
}

void TstNet::strictDigestParamsRejectsAlgorithm()
{
    HttpAuth auth(QStringLiteral("onvifsim"));
    Quirks quirks;
    const CameraModel model = modelWithUser();

    const QByteArray nonce = nonceOf(auth.challenges(quirks).first());
    const QByteArray uri = "/onvif/device_service";
    const QByteArray response = HttpAuth::digestResponse("admin", "onvifsim", "admin123", "POST",
                                                         uri, nonce, "00000001", "abc", "auth");
    // 客户端习惯性多带一个挑战里没出现过的 algorithm=MD5。
    const QByteArray header = "Digest username=\"admin\", realm=\"onvifsim\", nonce=\"" + nonce
        + "\", uri=\"" + uri + "\", algorithm=MD5, qop=auth, nc=00000001, cnonce=\"abc\", "
          "response=\"" + response + "\"";

    // 默认宽容：摘要对就放行。
    QVERIFY(auth.verify(header, "POST", uri, model, quirks).authenticated);

    // E9 打开后同一条请求直接 401，这是真机上很多客户端连不上的原因。
    quirks.setEnabled(QuirkId::AuthStrictDigestParams, true);
    const AuthResult strict = auth.verify(header, "POST", uri, model, quirks);
    QVERIFY(!strict.authenticated);
    QVERIFY(strict.failureReason.contains(QStringLiteral("algorithm")));

    // 不带多余参数的规矩请求在严格模式下照样能过。
    const QByteArray clean = "Digest username=\"admin\", realm=\"onvifsim\", nonce=\"" + nonce
        + "\", uri=\"" + uri + "\", qop=auth, nc=00000002, cnonce=\"abc\", response=\""
        + HttpAuth::digestResponse("admin", "onvifsim", "admin123", "POST", uri, nonce,
                                   "00000002", "abc", "auth")
        + "\"";
    QVERIFY(auth.verify(clean, "POST", uri, model, quirks).authenticated);
}

void TstNet::staleNonce()
{
    HttpAuth auth(QStringLiteral("onvifsim"));
    Quirks quirks;
    const CameraModel model = modelWithUser();
    const QByteArray uri = "/onvif/device_service";

    // 没见过的 nonce（进程重启后客户端还拿着旧的）→ stale，让客户端静默重试。
    const QByteArray unknown = "Digest username=\"admin\", realm=\"onvifsim\", "
                               "nonce=\"deadbeef\", uri=\"" + uri
        + "\", qop=auth, nc=00000001, cnonce=\"abc\", response=\""
        + HttpAuth::digestResponse("admin", "onvifsim", "admin123", "POST", uri, "deadbeef",
                                   "00000001", "abc", "auth")
        + "\"";
    const AuthResult unknownResult = auth.verify(unknown, "POST", uri, model, quirks);
    QVERIFY(!unknownResult.authenticated);
    QVERIFY(unknownResult.stale);

    // 过期的 nonce 同理。
    auth.setNonceLifetime(-1);
    const QByteArray nonce = nonceOf(auth.challenges(quirks).first());
    const QByteArray header = "Digest username=\"admin\", realm=\"onvifsim\", nonce=\"" + nonce
        + "\", uri=\"" + uri + "\", qop=auth, nc=00000001, cnonce=\"abc\", response=\""
        + HttpAuth::digestResponse("admin", "onvifsim", "admin123", "POST", uri, nonce,
                                   "00000001", "abc", "auth")
        + "\"";
    const AuthResult expired = auth.verify(header, "POST", uri, model, quirks);
    QVERIFY(!expired.authenticated);
    QVERIFY(expired.stale);
}

void TstNet::basicAuth()
{
    HttpAuth auth(QStringLiteral("onvifsim"));
    Quirks quirks;
    const CameraModel model = modelWithUser();

    const QByteArray good = "Basic " + QByteArray("admin:admin123").toBase64();
    const AuthResult ok = auth.verify(good, "GET", "/onvif/snapshot", model, quirks);
    QVERIFY(ok.authenticated);
    QCOMPARE(ok.scheme, AuthScheme::Basic);
    QCOMPARE(ok.username, QStringLiteral("admin"));

    const QByteArray bad = "Basic " + QByteArray("admin:nope").toBase64();
    QVERIFY(!auth.verify(bad, "GET", "/onvif/snapshot", model, quirks).authenticated);
}

void TstNet::dualChallengeOverHttp()
{
    // E8 的两条挑战要真的变成两行 WWW-Authenticate：
    // HttpResponse::headers 是 QMap，一个 key 只能存一个值，
    // 约定用 '\n' 把多条串起来，由 HttpServer 拆成多行。
    HttpAuth auth(QStringLiteral("onvifsim"));
    Quirks quirks;
    quirks.setEnabled(QuirkId::AuthDualChallenge, true);
    const QList<QByteArray> challenges = auth.challenges(quirks);

    m_server->addRoute("GET", QStringLiteral("/snapshot"),
                       [&](const HttpRequest &, const HttpExchangePtr &exchange) {
                           HttpResponse response = HttpResponse::text(401,
                               QStringLiteral("Unauthorized"));
                           QByteArray joined;
                           for (const QByteArray &challenge : challenges) {
                               if (!joined.isEmpty())
                                   joined += '\n';
                               joined += challenge;
                           }
                           response.setHeader("WWW-Authenticate", joined);
                           exchange->respond(response);
                       });

    RawClient client(m_port);
    QVERIFY(client.waitConnected());
    client.send("GET /snapshot HTTP/1.1\r\nHost: h\r\n\r\n");
    QVERIFY(client.waitForResponse());
    QCOMPARE(client.statusCode(), 401);
    const QList<QByteArray> values = client.headerValues("WWW-Authenticate");
    QCOMPARE(values.size(), 2);
    QVERIFY(values.at(0).startsWith("Digest "));
    QVERIFY(values.at(1).startsWith("Basic "));
}

void TstNet::sameSubnetAndLinkLocal()
{
    const QHostAddress mask(QStringLiteral("255.255.255.0"));
    QVERIFY(netutil::isSameSubnet(QHostAddress(QStringLiteral("192.168.1.10")),
                                  QHostAddress(QStringLiteral("192.168.1.201")), mask));
    QVERIFY(!netutil::isSameSubnet(QHostAddress(QStringLiteral("192.168.1.10")),
                                   QHostAddress(QStringLiteral("192.168.2.201")), mask));
    // 掩码没拿到时不能瞎认，否则「同网段」会退化成「全世界」。
    QVERIFY(!netutil::isSameSubnet(QHostAddress(QStringLiteral("192.168.1.10")),
                                   QHostAddress(QStringLiteral("10.0.0.1")), QHostAddress()));

    QVERIFY(netutil::isLinkLocal(QHostAddress(QStringLiteral("169.254.3.4"))));
    QVERIFY(!netutil::isLinkLocal(QHostAddress(QStringLiteral("192.168.1.4"))));
    QVERIFY(!netutil::isLinkLocal(QHostAddress()));
}

void TstNet::preferredLocalAddressBound()
{
    // 独立 IP 模式：绑在别名上时只能报那个地址，
    // 否则客户端看到的来源 IP 与 XAddrs 对不上。
    const QHostAddress alias(QStringLiteral("192.168.1.201"));
    QCOMPARE(netutil::preferredLocalAddress(QHostAddress(QStringLiteral("192.168.1.10")), alias),
             alias);
    QCOMPARE(netutil::preferredLocalAddress(QHostAddress(QHostAddress::LocalHost),
                                            QHostAddress(QHostAddress::LocalHost)),
             QHostAddress(QHostAddress::LocalHost));
}

void TstNet::preferredLocalAddressLoopbackPeer()
{
    const QHostAddress picked =
        netutil::preferredLocalAddress(QHostAddress(QHostAddress::LocalHost));
    QVERIFY(!picked.isNull());
    QCOMPARE(picked.protocol(), QAbstractSocket::IPv4Protocol);

    bool hasLan = false;
    const QList<InterfaceInfo> ifaces = netutil::interfaces(false);
    for (const InterfaceInfo &info : ifaces) {
        for (const QHostAddress &address : info.addresses) {
            if (!address.isLoopback() && !netutil::isLinkLocal(address))
                hasLan = true;
        }
    }
    if (hasLan) {
        // 同机客户端也要拿 LAN IP：XAddr 会被存下来甚至转给别的进程，
        // 127.0.0.1 一出这台机器就废了。
        QVERIFY(!picked.isLoopback());
    } else {
        QVERIFY(picked.isLoopback());
    }
}

void TstNet::preferredLocalAddressLanPeer()
{
    QHostAddress local;
    QHostAddress mask;
    const QList<InterfaceInfo> ifaces = netutil::interfaces(false);
    for (const InterfaceInfo &info : ifaces) {
        for (int i = 0; i < info.addresses.size() && local.isNull(); ++i) {
            const QHostAddress candidate = info.addresses.at(i);
            const QHostAddress candidateMask = i < info.netmasks.size() ? info.netmasks.at(i)
                                                                       : QHostAddress();
            if (candidate.isLoopback() || netutil::isLinkLocal(candidate))
                continue;
            if (candidateMask.isNull() || candidateMask.toIPv4Address() == 0xFFFFFFFFu)
                continue;
            local = candidate;
            mask = candidateMask;
        }
    }
    if (local.isNull())
        QSKIP("这台机器没有带掩码的非回环 IPv4 网卡");

    // 构造一个同网段但不同主机位的 peer。
    // 翻掉主机位最低位就还在同一网段（/32 已经被上面挑掉了）。
    const quint32 peer = local.toIPv4Address() ^ 1u;
    QCOMPARE(netutil::preferredLocalAddress(QHostAddress(peer)), local);
    QVERIFY(netutil::isSameSubnet(QHostAddress(peer), local, mask));

    const InterfaceInfo owner = netutil::interfaceForAddress(local);
    QVERIFY(!owner.name.isEmpty());
    QVERIFY(owner.addresses.contains(local));
}

void TstNet::interfaceLookupAndFreePort()
{
    const QList<InterfaceInfo> all = netutil::interfaces(true);
    QVERIFY(!all.isEmpty());
    bool loopbackSeen = false;
    for (const InterfaceInfo &info : all)
        loopbackSeen = loopbackSeen || info.isLoopback;
    QVERIFY(loopbackSeen);
    for (const InterfaceInfo &info : netutil::interfaces(false))
        QVERIFY(!info.isLoopback);

    QCOMPARE(netutil::interfaceByName(all.first().name).name, all.first().name);
    QVERIFY(netutil::interfaceByName(QStringLiteral("no-such-nic")).name.isEmpty());

    const quint16 free = netutil::findFreePort(QHostAddress::LocalHost, 45123, 50);
    QVERIFY(free != 0);
    QTcpServer blocker;
    QVERIFY(blocker.listen(QHostAddress::LocalHost, free));
    // 占掉之后必须换一个。
    const quint16 next = netutil::findFreePort(QHostAddress::LocalHost, free, 50);
    QVERIFY(next != 0);
    QVERIFY(next != free);
}

void TstNet::ipAliasCommandStrings()
{
    IpAliasRequest request;
    request.interfaceName = QStringLiteral("eth0");
    request.address = QHostAddress(QStringLiteral("192.168.1.201"));
    request.netmask = QHostAddress(QStringLiteral("255.255.255.0"));

    const QStringList add = IpAlias::commandsFor(request, true);
    const QStringList remove = IpAlias::commandsFor(request, false);
    QCOMPARE(add.size(), 1);
    QCOMPARE(remove.size(), 1);

#if defined(Q_OS_LINUX)
    QCOMPARE(add.first(), QStringLiteral("ip addr add 192.168.1.201/24 dev eth0"));
    QCOMPARE(remove.first(), QStringLiteral("ip addr del 192.168.1.201/24 dev eth0"));

    IpAliasRequest wide = request;
    wide.netmask = QHostAddress(QStringLiteral("255.255.0.0"));
    QCOMPARE(IpAlias::commandsFor(wide, true).first(),
             QStringLiteral("ip addr add 192.168.1.201/16 dev eth0"));
#elif defined(Q_OS_MACOS)
    QCOMPARE(add.first(), QStringLiteral("ifconfig eth0 alias 192.168.1.201 255.255.255.0"));
    QCOMPARE(remove.first(), QStringLiteral("ifconfig eth0 -alias 192.168.1.201"));
#elif defined(Q_OS_WIN)
    QCOMPARE(add.first(),
             QStringLiteral("netsh interface ip add address \"eth0\" 192.168.1.201 255.255.255.0"));
    QCOMPARE(remove.first(),
             QStringLiteral("netsh interface ip delete address \"eth0\" 192.168.1.201"));
#endif

    QVERIFY(!IpAlias::elevationHint().isEmpty());
    QVERIFY(!IpAlias::aliasExists(QStringLiteral("no-such-nic"),
                                  QHostAddress(QStringLiteral("192.168.1.201"))));
}

void TstNet::ipAliasRejectsBadInput()
{
    // 参数不合法时立刻失败，绝不去弹提权框。
    IpAlias alias;
    const IpAliasResult empty = alias.add(IpAliasRequest());
    QVERIFY(!empty.ok);
    QVERIFY(!empty.errorMessage.isEmpty());

    const IpAliasResult range = alias.addRange(QStringLiteral("eth0"), QHostAddress(), 0,
                                               QHostAddress(QStringLiteral("255.255.255.0")));
    QVERIFY(!range.ok);
    QVERIFY(alias.ownedAliases().isEmpty());
}

QTEST_MAIN(TstNet)
#include "tst_net.moc"
