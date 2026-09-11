#include "net/HttpServer.h"

#include "core/LogBus.h"
#include "core/VirtualCamera.h"
#include "net/HttpExchange_p.h"
#include "net/NetUtil.h"

#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QLocale>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QVector>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <algorithm>
#include <functional>

namespace onvifsim {

class HttpConnection;

namespace {

constexpr qint64 kDefaultMaxBodySize = 1024 * 1024;   // plan §4.1：body 上限 1 MB
constexpr int kDefaultKeepAliveSeconds = 30;
constexpr qsizetype kMaxHeaderBytes = 32 * 1024;      // 头部块上限，挡住只发头不发完的空占连接
// 流式响应的写缓冲上限：客户端读不动时真机就是直接踹掉，这里也踹，别把内存吃光。
constexpr qint64 kStreamBacklogLimit = 8 * 1024 * 1024;

// 双栈 socket 上的 IPv4 连接会显示成 ::ffff:1.2.3.4。日志、
// 「按 peer 网段挑源地址」都按 IPv4 处理更省事，收下来先归一化。
QHostAddress normalizedAddress(const QHostAddress &address)
{
    bool ok = false;
    const quint32 v4 = address.toIPv4Address(&ok);
    return ok ? QHostAddress(v4) : address;
}

// RFC 7231 的 IMF-fixdate：固定英文月份名 + GMT，不能跟随系统 locale。
bool headerEquals(const QByteArray &a, const char *b)
{
    return a.compare(b, Qt::CaseInsensitive) == 0;
}

// 响应头保留调用方写的大小写，取值时只能逐个比。
QByteArray headerValue(const QMap<QByteArray, QByteArray> &headers, const char *name)
{
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        if (headerEquals(it.key(), name))
            return it.value();
    }
    return QByteArray();
}

LogBus *busFor(VirtualCamera *camera)
{
    // camera 允许为空（单测里会这么用），日志退回全局总线。
    // 注意 REST 控制面**不**走这个类：ControlApi 自己裸用 QTcpServer。
    if (camera) {
        if (LogBus *bus = camera->logBus())
            return bus;
    }
    return LogBus::global();
}

QString cameraIdOf(VirtualCamera *camera)
{
    return camera ? camera->id() : QString();
}

} // namespace

// 一条路由。method 为空表示不限方法；prefix 路由存在 HttpServerContext 的另一张表里。
struct HttpServerRoute {
    QByteArray method;
    QString path;
    HttpHandler handler;
};

// HttpServer::Private 是私有嵌套类型，HttpConnection 够不着，
// 所以把连接真正要用的东西抽成 HttpServerContext，让 Private 继承它。
struct HttpServerContext {
    VirtualCamera *camera = nullptr;
    QVector<HttpServerRoute> exactRoutes;
    QVector<HttpServerRoute> prefixRoutes;
    HttpHandler fallback;
    qint64 maxBodySize = kDefaultMaxBodySize;
    int keepAliveSeconds = kDefaultKeepAliveSeconds;
    std::function<void(const HttpRequest &)> requestNotifier;
    std::function<void(HttpConnection *)> closeNotifier;

    const HttpServerRoute *findRoute(const QByteArray &method, const QString &path,
                               bool *pathExists) const;
    void log(LogLevel level, const QString &summary, const QString &peer = QString(),
             const QString &detail = QString(), qint64 durationUs = -1, bool ok = true) const;
};

const HttpServerRoute *HttpServerContext::findRoute(const QByteArray &method, const QString &path,
                                          bool *pathExists) const
{
    const QByteArray wanted = method.toUpper();
    bool seen = false;
    for (const HttpServerRoute &route : exactRoutes) {
        if (route.path != path)
            continue;
        seen = true;
        if (route.method.isEmpty() || route.method.toUpper() == wanted)
            return &route;
    }
    // 前缀路由取最长匹配：订阅管理器的 /event-1024 与厂商 API 的 /api 可能同时挂着，
    // 谁的前缀更长谁优先，避免注册顺序决定行为。
    const HttpServerRoute *best = nullptr;
    for (const HttpServerRoute &route : prefixRoutes) {
        if (!path.startsWith(route.path))
            continue;
        seen = true;
        if (!route.method.isEmpty() && route.method.toUpper() != wanted)
            continue;
        if (!best || route.path.size() > best->path.size())
            best = &route;
    }
    if (pathExists)
        *pathExists = seen;
    return best;
}

void HttpServerContext::log(LogLevel level, const QString &summary, const QString &peer,
                        const QString &detail, qint64 durationUs, bool ok) const
{
    LogBus *bus = busFor(camera);
    if (!bus)
        return;
    LogRecord record;
    record.level = level;
    record.category = QString::fromLatin1(logcat::Http);
    record.cameraId = cameraIdOf(camera);
    record.peer = peer;
    record.summary = summary;
    record.detail = detail;
    record.durationUs = durationUs;
    record.ok = ok;
    bus->post(record);
}

// 一条 keep-alive 连接的状态机：收头 → 收 body → 建 HttpExchange 交给 handler →
// 等回包（可能是几十秒后的长轮询）→ 写回 → 复位收下一条。
// 一次只处理一个请求：pipeline 上的后续字节先留在缓冲里，等这条回完再解析。
class HttpConnection : public QObject
{
    Q_OBJECT
public:
    HttpConnection(QTcpSocket *socket, HttpServerContext *context, QObject *parent);
    ~HttpConnection() override;

    // 服务器要收摊时用：先掐断所有回调再排队析构，避免正在 handler 栈里被删掉。
    void shutdown();

private:
    void onReadyRead();
    void onDisconnected();
    void processBuffer();
    bool parseHeaderBlock(const QByteArray &block);
    void dispatch();
    void failAndClose(int status, const QString &message);
    void deliver(const HttpResponse &response);
    void sendNow(const HttpResponse &response);
    QByteArray buildHead(const HttpResponse &response, bool keepAlive, bool stream) const;
    void sendChunk();
    void finishResponse();
    void resetForNextRequest();
    void armIdleTimer();
    void abortExchange();
    void releaseExchangeLater();
    void dropNow(const QString &why);
    // ---- 长连接流式响应（海康 alertStream / 大华 attach）----
    void beginStream(const HttpResponse &response);
    void flushStreamHead();
    bool writeChunk(const QByteArray &data);
    void endStream();

    QTcpSocket *m_socket = nullptr;
    HttpServerContext *m_ctx = nullptr;
    QTimer *m_idleTimer = nullptr;
    QTimer *m_slowTimer = nullptr;

    QByteArray m_buffer;
    HttpRequest m_request;
    bool m_haveHeaders = false;
    qint64 m_expectedBody = 0;
    bool m_awaitingResponse = false;
    bool m_closing = false;
    HttpExchangePtr m_exchange;
    QElapsedTimer m_elapsed;

    QByteArray m_pending;        // 待写出的整包（慢发送时按块切）
    qsizetype m_pendingOffset = 0;
    int m_chunkSize = 0;
    bool m_closeAfterSend = false;

    bool m_streaming = false;
    bool m_streamHeadSent = false;
    QByteArray m_streamHead;     // delayMs 期间压着的响应头
    QByteArray m_streamQueued;   // 头还没出去就先来的段，攒着保持顺序
    qint64 m_streamBytes = 0;
    int m_streamChunks = 0;
};

HttpConnection::HttpConnection(QTcpSocket *socket, HttpServerContext *context, QObject *parent)
    : QObject(parent), m_socket(socket), m_ctx(context)
{
    m_socket->setParent(this);
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);   // 关 Nagle，小响应别攒着
    connect(m_socket, &QTcpSocket::readyRead, this, &HttpConnection::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &HttpConnection::onDisconnected);

    m_idleTimer = new QTimer(this);
    m_idleTimer->setSingleShot(true);
    connect(m_idleTimer, &QTimer::timeout, this, [this] {
        // 只在空闲时收割：长轮询挂着的连接不算空闲。
        if (m_awaitingResponse)
            return;
        m_closing = true;
        m_socket->disconnectFromHost();
    });
    armIdleTimer();

    m_request.localAddress = normalizedAddress(m_socket->localAddress());
    m_request.localPort = m_socket->localPort();
    m_request.peerAddress = normalizedAddress(m_socket->peerAddress());
    m_request.peerPort = m_socket->peerPort();
}

HttpConnection::~HttpConnection()
{
    abortExchange();
}

void HttpConnection::shutdown()
{
    m_closing = true;
    disconnect(m_socket, nullptr, this, nullptr);
    m_idleTimer->stop();
    if (m_slowTimer)
        m_slowTimer->stop();
    abortExchange();
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();
}

void HttpConnection::armIdleTimer()
{
    // 正在等 handler 回包（长轮询）或正在推流的连接不算空闲，一律不计时。
    if (m_awaitingResponse || m_streaming) {
        m_idleTimer->stop();
        return;
    }
    const int seconds = m_ctx->keepAliveSeconds;
    if (seconds <= 0) {
        m_idleTimer->stop();
        return;
    }
    m_idleTimer->start(seconds * 1000);
}

void HttpConnection::abortExchange()
{
    if (!m_exchange)
        return;
    const HttpExchangePtr exchange = m_exchange;
    m_exchange.clear();
    exchange->d->connected = false;
    exchange->d->detach();    // 先断开全部回调再发信号，防止 handler 在槽里回包 / 推流
    // 流式响应也走这条路：推送方就靠 aborted() 停掉自己的定时器。
    if (!exchange->d->responded)
        emit exchange->aborted();
}

void HttpConnection::onReadyRead()
{
    if (m_streaming) {
        // 流是单向的，客户端这时发什么都不该攒进解析缓冲里。
        m_socket->readAll();
        return;
    }
    m_buffer += m_socket->readAll();
    armIdleTimer();
    processBuffer();
}

void HttpConnection::onDisconnected()
{
    m_closing = true;
    abortExchange();
    if (m_ctx->closeNotifier)
        m_ctx->closeNotifier(this);
}

void HttpConnection::processBuffer()
{
    while (!m_closing && !m_awaitingResponse) {
        if (!m_haveHeaders) {
            // 有的客户端在两条请求之间多发一对 CRLF，RFC 7230 要求忽略。
            while (m_buffer.startsWith("\r\n"))
                m_buffer.remove(0, 2);
            const qsizetype end = m_buffer.indexOf("\r\n\r\n");
            if (end < 0) {
                if (m_buffer.size() > kMaxHeaderBytes)
                    failAndClose(431, QStringLiteral("header block too large"));
                return;
            }
            const QByteArray block = m_buffer.left(end);
            m_buffer.remove(0, end + 4);
            if (!parseHeaderBlock(block))
                return;                       // parseHeaderBlock 里已经回错并关连接
            m_haveHeaders = true;
            if (m_expectedBody > 0 && m_request.header("expect").toLower().contains("100-continue"))
                m_socket->write("HTTP/1.1 100 Continue\r\n\r\n");
        }
        if (m_buffer.size() < m_expectedBody)
            return;                           // body 还没收全，等下一次 readyRead
        m_request.body = m_buffer.left(qsizetype(m_expectedBody));
        m_buffer.remove(0, qsizetype(m_expectedBody));
        dispatch();
    }
}

bool HttpConnection::parseHeaderBlock(const QByteArray &block)
{
    const QList<QByteArray> lines = block.split('\n');
    if (lines.isEmpty()) {
        failAndClose(400, QStringLiteral("empty request"));
        return false;
    }

    QByteArray requestLine = lines.first();
    if (requestLine.endsWith('\r'))
        requestLine.chop(1);
    const QList<QByteArray> parts = requestLine.simplified().split(' ');
    if (parts.size() != 3) {
        failAndClose(400, QStringLiteral("malformed request line"));
        return false;
    }

    m_request.method = parts.at(0);
    m_request.rawTarget = QString::fromLatin1(parts.at(1));
    m_request.version = parts.at(2);
    if (!m_request.version.startsWith("HTTP/")) {
        failAndClose(400, QStringLiteral("malformed request line"));
        return false;
    }
    if (!m_request.version.startsWith("HTTP/1.")) {
        failAndClose(505, QStringLiteral("only HTTP/1.x is supported"));
        return false;
    }

    // 代理风格的 absolute-form（GET http://host/path HTTP/1.1）也要收：
    // 有的客户端把 XAddr 整个塞进请求行。
    QString target = m_request.rawTarget;
    if (target.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
        || target.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
        const qsizetype slash =
            target.indexOf(QLatin1Char('/'), target.indexOf(QLatin1String("//")) + 2);
        target = slash < 0 ? QStringLiteral("/") : target.mid(slash);
    }
    const qsizetype qmark = target.indexOf(QLatin1Char('?'));
    const QString rawPath = qmark < 0 ? target : target.left(qmark);
    m_request.query = QUrlQuery(qmark < 0 ? QString() : target.mid(qmark + 1));
    m_request.path = QUrl::fromPercentEncoding(rawPath.toUtf8());
    if (m_request.path.isEmpty())
        m_request.path = QStringLiteral("/");

    m_request.headers.clear();
    QByteArray lastKey;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        QByteArray line = lines.at(i);
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.isEmpty())
            continue;
        if (line.startsWith(' ') || line.startsWith('\t')) {
            // obs-fold 续行：并进上一条头的值。
            if (lastKey.isEmpty()) {
                failAndClose(400, QStringLiteral("malformed header continuation"));
                return false;
            }
            m_request.headers[lastKey] += ' ' + line.trimmed();
            continue;
        }
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) {
            failAndClose(400, QStringLiteral("malformed header line"));
            return false;
        }
        const QByteArray key = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (key.isEmpty()) {
            failAndClose(400, QStringLiteral("malformed header line"));
            return false;
        }
        // 同名头按 RFC 7230 合成逗号分隔的一条。
        if (m_request.headers.contains(key))
            m_request.headers[key] += ", " + value;
        else
            m_request.headers.insert(key, value);
        lastKey = key;
    }

    const QByteArray transferEncoding = m_request.header("transfer-encoding").toLower();
    if (!transferEncoding.isEmpty()) {
        // 本项目不做 chunked 请求体（plan §4.1）。411 是给客户端最明确的提示：
        // 你得先算出长度再发。
        if (transferEncoding.contains("chunked")) {
            failAndClose(411, QStringLiteral("chunked request body is not supported"));
            return false;
        }
        failAndClose(501, QStringLiteral("unsupported transfer-encoding"));
        return false;
    }

    m_expectedBody = 0;
    if (m_request.hasHeader("content-length")) {
        const QByteArray raw = m_request.header("content-length");
        bool ok = false;
        // 合成过的重复头会变成 "12, 12"，一律当非法。
        const qint64 length = raw.toLongLong(&ok);
        if (!ok || length < 0) {
            failAndClose(400, QStringLiteral("invalid Content-Length"));
            return false;
        }
        if (length > m_ctx->maxBodySize) {
            failAndClose(413, QStringLiteral("request body exceeds the configured limit"));
            return false;
        }
        m_expectedBody = length;
    }
    return true;
}

void HttpConnection::dispatch()
{
    m_elapsed.start();
    m_request.peerAddress = normalizedAddress(m_socket->peerAddress());
    m_request.peerPort = m_socket->peerPort();
    m_request.localAddress = normalizedAddress(m_socket->localAddress());
    m_request.localPort = m_socket->localPort();

    m_awaitingResponse = true;
    m_idleTimer->stop();   // 处理期间不收割：PullMessages 可能挂 60 秒

    // 交给 handler 的句柄。不设 parent，靠 QSharedPointer 管生命周期；
    // 连接自己也留一份强引用，handler 提前撒手也不会把它析构掉。
    HttpExchangePtr exchange(new HttpExchange());
    exchange->d->request = m_request;
    exchange->d->deliver = [this](const HttpResponse &response) { deliver(response); };
    exchange->d->beginStream = [this](const HttpResponse &head) { beginStream(head); };
    exchange->d->writeChunk = [this](const QByteArray &data) { return writeChunk(data); };
    exchange->d->endStream = [this] { endStream(); };
    m_exchange = exchange;

    if (m_ctx->requestNotifier)
        m_ctx->requestNotifier(m_request);

    bool pathExists = false;
    const HttpServerRoute *route = m_ctx->findRoute(m_request.method, m_request.path, &pathExists);
    const HttpHandler handler = route ? route->handler : m_ctx->fallback;
    if (handler) {
        handler(m_request, exchange);
    } else if (pathExists) {
        // 路径注册过但方法对不上：405 比 404 更好排查。
        HttpResponse response = HttpResponse::text(405, QStringLiteral("Method Not Allowed"));
        response.setHeader("Allow", "GET, POST, PUT, DELETE");
        exchange->respond(response);
    } else {
        exchange->respond(HttpResponse::text(404, QStringLiteral("Not Found")));
    }
}

void HttpConnection::failAndClose(int status, const QString &message)
{
    m_ctx->log(LogLevel::Warning,
               QStringLiteral("HTTP %1 %2").arg(status).arg(message),
               m_request.peerString(), QString(), -1, false);
    HttpResponse response = HttpResponse::text(status, message);
    response.forceClose = true;
    m_awaitingResponse = false;
    abortExchange();
    sendNow(response);
}

void HttpConnection::dropNow(const QString &why)
{
    // 不回包直接断，客户端只会看到 connection reset。
    m_ctx->log(LogLevel::Warning,
               QStringLiteral("%1 %2 → connection dropped (%3)")
                   .arg(QString::fromLatin1(m_request.method), m_request.path, why),
               m_request.peerString(), QString(),
               m_elapsed.isValid() ? m_elapsed.nsecsElapsed() / 1000 : -1, false);
    m_closing = true;
    m_streaming = false;
    abortExchange();
    m_socket->abort();
    if (m_ctx->closeNotifier)
        m_ctx->closeNotifier(this);
}

void HttpConnection::releaseExchangeLater()
{
    if (!m_exchange)
        return;
    // 极小概率但要防：调用方可能把最后一份引用交到了我们手上，而此刻
    // 我们正站在 HttpExchange 自己的成员函数栈上。绕一圈事件循环再放手，
    // 别把脚下的对象拆了。连接先没了的话 lambda 连同引用一起销毁，同样安全。
    const HttpExchangePtr keep = m_exchange;
    m_exchange.clear();
    QTimer::singleShot(0, this, [keep] { Q_UNUSED(keep) });
}

void HttpConnection::deliver(const HttpResponse &response)
{
    if (m_closing)
        return;
    if (response.dropConnection) {
        dropNow(QStringLiteral("故障注入"));
        return;
    }
    if (response.delayMs > 0) {
        // 用定时器而不是 sleep：整个进程只有一个事件循环，睡下去全体都停。
        // 上下文给 this，连接先没了定时器自动取消。
        QTimer::singleShot(response.delayMs, this, [this, response] {
            if (!m_closing)
                sendNow(response);
        });
        return;
    }
    sendNow(response);
}

// 拼状态行 + 头 + 空行。普通响应与流式响应共用，差别只在结尾那几行：
// 流式的长度天生未知，绝不能自作主张补 Content-Length（客户端会照它截断），
// keep-alive 也由调用方在头里说了算。
QByteArray HttpConnection::buildHead(const HttpResponse &response, bool keepAlive,
                                     bool stream) const
{
    const QByteArray reason = response.reason.isEmpty()
        ? QByteArray(HttpResponse::defaultReason(response.status))
        : response.reason;
    QByteArray out = "HTTP/1.1 " + QByteArray::number(response.status) + ' ' + reason + "\r\n";

    bool hasContentLength = false;
    bool hasDate = false;
    bool hasServer = false;
    bool hasConnection = false;
    for (auto it = response.headers.constBegin(); it != response.headers.constEnd(); ++it) {
        if (headerEquals(it.key(), "content-length"))
            hasContentLength = true;
        else if (headerEquals(it.key(), "date"))
            hasDate = true;
        else if (headerEquals(it.key(), "server"))
            hasServer = true;
        else if (headerEquals(it.key(), "connection")) {
            if (!stream)
                continue;         // 普通响应的 keep-alive 由服务器统一决定
            hasConnection = true;
        }
        // 一个 key 想发多条（E8 的 Basic + Digest 双挑战）时，
        // 调用方把它们用 '\n' 串起来，这里再拆成多行。
        const QList<QByteArray> values = it.value().split('\n');
        for (const QByteArray &value : values) {
            const QByteArray trimmed = value.trimmed();
            if (!trimmed.isEmpty())
                out += it.key() + ": " + trimmed + "\r\n";
        }
    }
    if (!hasDate)
        out += "Date: " + netutil::httpDate() + "\r\n";
    if (!hasServer)
        out += "Server: onvifsim\r\n";
    if (stream) {
        if (!hasConnection)
            out += "Connection: close\r\n";
    } else {
        // 204 / 304 按 RFC 7230 不带 Content-Length，别的一律补上（空体也要写 0，
        // 否则客户端会一直等到连接关闭才认为收完）。
        const bool bodyless = response.status == 204 || response.status == 304
            || (response.status >= 100 && response.status < 200);
        if (!hasContentLength && !bodyless)
            out += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
        out += keepAlive ? QByteArray("Connection: keep-alive\r\n")
                         : QByteArray("Connection: close\r\n");
    }
    out += "\r\n";
    return out;
}

void HttpConnection::sendNow(const HttpResponse &response)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) {
        m_closing = true;
        return;
    }

    const bool raw = !response.rawOverride.isEmpty();
    // rawOverride 之后的字节流我们说了不算（F1 故意畸形），没法接着复用连接。
    const bool keepAlive = m_request.wantsKeepAlive() && !response.forceClose && !raw;

    QByteArray out;
    if (raw) {
        out = response.rawOverride;
    } else {
        out = buildHead(response, keepAlive, false);
        if (m_request.method.toUpper() != "HEAD")
            out += response.body;
    }

    // detail 只放文本类响应体（GUI 里要展开看 SOAP 原文）；
    // 快照那种二进制只记大小，别把 JPEG 塞进日志。
    QString detail;
    const QByteArray contentType = headerValue(response.headers, "content-type");
    const bool textual = contentType.isEmpty() || contentType.contains("xml")
        || contentType.contains("json") || contentType.startsWith("text/");
    if (raw)
        detail = QString::fromUtf8(response.rawOverride.left(4096));
    else if (textual)
        detail = QString::fromUtf8(response.body.left(64 * 1024));
    else if (!response.body.isEmpty())
        detail = QStringLiteral("<%1 字节 %2>")
                     .arg(response.body.size())
                     .arg(QString::fromLatin1(contentType));

    m_ctx->log(response.status >= 500 ? LogLevel::Warning : LogLevel::Info,
               QStringLiteral("%1 %2 → %3")
                   .arg(QString::fromLatin1(m_request.method),
                        m_request.rawTarget.isEmpty() ? m_request.path : m_request.rawTarget)
                   .arg(response.status),
               m_request.peerString(), detail,
               m_elapsed.isValid() ? m_elapsed.nsecsElapsed() / 1000 : -1,
               response.status < 400);

    m_pending = out;
    m_pendingOffset = 0;
    m_closeAfterSend = !keepAlive;

    if (response.slowSendChunk > 0) {
        // Slowloris：整包切小块、块间等一会儿，测客户端的读超时。
        m_chunkSize = response.slowSendChunk;
        if (!m_slowTimer) {
            m_slowTimer = new QTimer(this);
            connect(m_slowTimer, &QTimer::timeout, this, &HttpConnection::sendChunk);
        }
        m_slowTimer->start(qMax(1, response.slowSendIntervalMs));
        sendChunk();
        return;
    }

    m_socket->write(m_pending);
    m_pendingOffset = m_pending.size();
    finishResponse();
}

void HttpConnection::sendChunk()
{
    if (m_closing || !m_socket || m_socket->state() != QAbstractSocket::ConnectedState) {
        if (m_slowTimer)
            m_slowTimer->stop();
        return;
    }
    const qsizetype remaining = m_pending.size() - m_pendingOffset;
    const qsizetype n = qMin<qsizetype>(m_chunkSize, remaining);
    if (n > 0) {
        m_socket->write(m_pending.constData() + m_pendingOffset, n);
        m_socket->flush();
        m_pendingOffset += n;
    }
    if (m_pendingOffset >= m_pending.size()) {
        m_slowTimer->stop();
        finishResponse();
    }
}

// ---- 长连接流式响应 -------------------------------------------------------
// 海康 /ISAPI/Event/notification/alertStream 与大华 eventManager.cgi?action=attach
// 都是「连上就一直挂着、有事件推一段」的接口，真机上一条连接能开几个小时。
// 这里只负责把字节原样送出去：multipart 边界还是 chunked 编码，由调用方在头里定。

void HttpConnection::beginStream(const HttpResponse &response)
{
    if (m_closing || !m_socket || m_socket->state() != QAbstractSocket::ConnectedState) {
        m_closing = true;
        return;
    }
    if (response.dropConnection) {
        dropNow(QStringLiteral("故障注入"));
        return;
    }

    m_streaming = true;
    m_streamHeadSent = false;
    m_streamBytes = 0;
    m_streamChunks = 0;
    m_awaitingResponse = true;   // 这条连接从此只属于这个流，不再解析后续请求
    m_idleTimer->stop();         // 推流期间永不按空闲收割

    m_streamHead = !response.rawOverride.isEmpty() ? response.rawOverride
                                                   : buildHead(response, false, true);

    m_ctx->log(LogLevel::Info,
               QStringLiteral("%1 %2 → %3 (streaming response started)")
                   .arg(QString::fromLatin1(m_request.method),
                        m_request.rawTarget.isEmpty() ? m_request.path : m_request.rawTarget)
                   .arg(response.status),
               m_request.peerString(),
               QString::fromUtf8(headerValue(response.headers, "content-type")),
               m_elapsed.isValid() ? m_elapsed.nsecsElapsed() / 1000 : -1, true);

    if (response.delayMs > 0) {
        // 头先压住，期间来的段攒进 m_streamQueued，保证字节顺序不乱。
        QTimer::singleShot(response.delayMs, this, [this] { flushStreamHead(); });
        return;
    }
    flushStreamHead();
}

void HttpConnection::flushStreamHead()
{
    if (m_closing || m_streamHeadSent || !m_streaming)
        return;
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return;
    m_socket->write(m_streamHead);
    m_streamHead.clear();
    m_streamHeadSent = true;
    if (!m_streamQueued.isEmpty()) {
        m_socket->write(m_streamQueued);
        m_streamQueued.clear();
    }
    m_socket->flush();
}

bool HttpConnection::writeChunk(const QByteArray &data)
{
    if (m_closing || !m_streaming || !m_socket
        || m_socket->state() != QAbstractSocket::ConnectedState)
        return false;
    // 客户端读不动时 socket 写缓冲会一直涨。真机对这种消费者就是直接踹掉，
    // 这里也踹：既保住内存，也让推送方立刻收到 false 而不是无限积压。
    if (m_socket->bytesToWrite() > kStreamBacklogLimit) {
        m_ctx->log(LogLevel::Warning,
                   QStringLiteral("Streaming backlog over %1 KB, dropping this client that cannot keep up")
                       .arg(kStreamBacklogLimit / 1024),
                   m_request.peerString(), QString(), -1, false);
        dropNow(QStringLiteral("streaming backlog"));
        return false;
    }

    ++m_streamChunks;
    m_streamBytes += data.size();
    if (!m_streamHeadSent) {
        m_streamQueued += data;   // 头还压在 delayMs 定时器里
        return true;
    }
    // HttpTypes.h 白纸黑字承诺「连接已断返回 false，调用方据此停止推送」。
    // 老实现恒返回 true，于是海康的 alertStream / 大华的 attach 推送方
    // 在对端早就走了之后还会一直空推。这里按承诺办：写失败就是 false。
    const qint64 written = m_socket->write(data);
    if (written < 0) {
        dropNow(QStringLiteral("流式写失败"));
        return false;
    }
    m_socket->flush();
    return m_socket->state() == QAbstractSocket::ConnectedState;
}

void HttpConnection::endStream()
{
    if (!m_streaming)
        return;
    flushStreamHead();            // 一段都没推就收尾时，头也得发出去
    m_streaming = false;
    m_awaitingResponse = false;
    m_streamHead.clear();
    m_streamQueued.clear();
    releaseExchangeLater();

    m_ctx->log(LogLevel::Info,
               QStringLiteral("Streaming response finished: %1 chunk(s) / %2 byte(s)")
                   .arg(m_streamChunks).arg(m_streamBytes),
               m_request.peerString(), QString(),
               m_elapsed.isValid() ? m_elapsed.nsecsElapsed() / 1000 : -1, true);

    m_closing = true;
    m_socket->disconnectFromHost();   // 会先把排队的字节冲出去
}

void HttpConnection::finishResponse()
{
    m_awaitingResponse = false;
    releaseExchangeLater();
    m_pending.clear();
    m_pendingOffset = 0;

    if (m_closeAfterSend) {
        m_closing = true;
        m_socket->disconnectFromHost();   // 会先把已排队的字节冲出去
        return;
    }
    resetForNextRequest();
    armIdleTimer();
    // 缓冲里可能已经躺着下一条（pipeline）。绕一圈事件循环再解析，
    // 免得「同步回包」的 handler 把栈越叠越深。
    QTimer::singleShot(0, this, [this] {
        if (!m_closing)
            processBuffer();
    });
}

void HttpConnection::resetForNextRequest()
{
    const QHostAddress peer = m_request.peerAddress;
    const quint16 peerPort = m_request.peerPort;
    const QHostAddress local = m_request.localAddress;
    const quint16 localPort = m_request.localPort;
    m_request = HttpRequest();
    m_request.peerAddress = peer;
    m_request.peerPort = peerPort;
    m_request.localAddress = local;
    m_request.localPort = localPort;
    m_haveHeaders = false;
    m_expectedBody = 0;
}

struct HttpServer::Private : HttpServerContext
{
    QTcpServer *tcp = nullptr;
    QList<HttpConnection *> connections;
    bool suspended = false;
    QHostAddress boundAddress = QHostAddress(QHostAddress::AnyIPv4);
    quint16 boundPort = 0;
};

HttpServer::HttpServer(VirtualCamera *camera, QObject *parent)
    : QObject(parent), d(new Private)
{
    d->camera = camera;
    d->tcp = new QTcpServer(this);

    d->requestNotifier = [this](const HttpRequest &request) { emit requestReceived(request); };
    d->closeNotifier = [this](HttpConnection *connection) {
        if (d->connections.removeOne(connection))
            emit connectionCountChanged(int(d->connections.size()));
        connection->deleteLater();
    };

    connect(d->tcp, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket *socket = d->tcp->nextPendingConnection()) {
            auto *connection = new HttpConnection(socket, d, this);
            d->connections.append(connection);
            emit connectionCountChanged(int(d->connections.size()));
        }
    });
}

HttpServer::~HttpServer()
{
    d->tcp->close();
    d->requestNotifier = nullptr;
    d->closeNotifier = nullptr;
    const QList<HttpConnection *> connections = d->connections;
    d->connections.clear();
    // 这里必须真删干净：连接对象持有 HttpServerContext 指针，
    // 而 d 马上就要没了，不能留给 QObject 的子对象析构去碰。
    for (HttpConnection *connection : connections) {
        connection->shutdown();
        delete connection;
    }
    delete d;
}

bool HttpServer::listen(const QHostAddress &address, quint16 port, QString *errorOut)
{
    if (d->tcp->isListening())
        d->tcp->close();
    if (!d->tcp->listen(address, port)) {
        const QString message = d->tcp->errorString();
        if (errorOut)
            *errorOut = message;
        d->log(LogLevel::Error,
               QStringLiteral("Cannot listen on HTTP %1:%2: %3").arg(address.toString()).arg(port).arg(message),
               QString(), QString(), -1, false);
        return false;
    }
    d->suspended = false;
    d->boundAddress = address;
    // port 传 0 时记住内核给的端口，resume() 才能回到同一个地址。
    d->boundPort = d->tcp->serverPort();
    d->log(LogLevel::Info,
           QStringLiteral("HTTP listening on %1:%2").arg(address.toString()).arg(d->boundPort));
    return true;
}

void HttpServer::close()
{
    d->tcp->close();
    const QList<HttpConnection *> connections = d->connections;
    d->connections.clear();
    // 可能是某个 handler 在自己的调用栈里把相机打到离线（SystemReboot quirk），
    // 那条连接就在栈上，只能先掐断再 deleteLater。
    for (HttpConnection *connection : connections) {
        connection->shutdown();
        connection->deleteLater();
    }
    if (!connections.isEmpty())
        emit connectionCountChanged(0);
}

bool HttpServer::isListening() const
{
    return d->tcp->isListening();
}

QHostAddress HttpServer::serverAddress() const
{
    return d->tcp->isListening() ? d->tcp->serverAddress() : d->boundAddress;
}

quint16 HttpServer::serverPort() const
{
    return d->tcp->isListening() ? d->tcp->serverPort() : d->boundPort;
}

void HttpServer::addRoute(const QByteArray &method, const QString &path, HttpHandler handler)
{
    HttpServerRoute route;
    route.method = method;
    route.path = path;
    route.handler = std::move(handler);
    d->exactRoutes.append(route);
}

void HttpServer::addPrefixRoute(const QByteArray &method, const QString &prefix,
                                HttpHandler handler)
{
    HttpServerRoute route;
    route.method = method;
    route.path = prefix;
    route.handler = std::move(handler);
    d->prefixRoutes.append(route);
}

void HttpServer::clearRoutes()
{
    d->exactRoutes.clear();
    d->prefixRoutes.clear();
}

void HttpServer::setFallbackHandler(HttpHandler handler)
{
    d->fallback = std::move(handler);
}

int HttpServer::connectionCount() const
{
    return int(d->connections.size());
}

void HttpServer::setMaxBodySize(qint64 bytes)
{
    d->maxBodySize = bytes > 0 ? bytes : kDefaultMaxBodySize;
}

void HttpServer::suspend()
{
    // 离线模拟：只收监听与连接，路由留着，resume 时不必重新注册。
    if (d->suspended)
        return;
    d->suspended = true;
    close();
    d->log(LogLevel::Info, QStringLiteral("HTTP stopped listening on %1:%2")
                               .arg(d->boundAddress.toString()).arg(d->boundPort));
}

bool HttpServer::resume(QString *errorOut)
{
    if (!d->suspended)
        return true;
    return listen(d->boundAddress, d->boundPort, errorOut);
}

bool HttpServer::isSuspended() const
{
    return d->suspended;
}

} // namespace onvifsim

#include "HttpServer.moc"
