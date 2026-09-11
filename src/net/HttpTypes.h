#pragma once

// HTTP 请求 / 响应的共享数据结构。net、soap、services、vendor、control 都用这一套。
//
// 响应支持异步交付（HttpExchange），PullMessages 的长轮询就靠它：
// handler 拿住 HttpExchangePtr 注册一个 QTimer，超时或有消息时再 respond，
// 全程不阻塞事件循环。

#include <QtCore/QByteArray>
#include <QtCore/QMap>
#include <QtCore/QObject>
#include <QtCore/QSharedPointer>
#include <QtCore/QString>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QHostAddress>

#include <functional>

namespace onvifsim {

struct HttpRequest {
    QByteArray method;                     // GET / POST / PUT / DELETE
    QString path;                          // 已解码，不含 query
    QString rawTarget;                     // 原始请求行里的 target，含 query
    QUrlQuery query;
    QByteArray version = "HTTP/1.1";
    QMap<QByteArray, QByteArray> headers;  // key 一律小写
    QByteArray body;

    QHostAddress peerAddress;
    quint16 peerPort = 0;
    QHostAddress localAddress;             // 请求打到哪个绑定地址上（独立 IP 模式要用）
    quint16 localPort = 0;
    bool secure = false;                   // 走的是 TLS（F2 自签场景）

    QByteArray header(const char *name) const;
    bool hasHeader(const char *name) const;
    QString contentType() const;
    // 客户端要求的 Host，用来回填 XAddr / StreamUri 的 host。
    QString hostHeader() const;
    QString peerString() const;            // "1.2.3.4:5678"
    bool wantsKeepAlive() const;
};

struct HttpResponse {
    int status = 200;
    QByteArray reason;                     // 空则按 status 取标准短语
    // headers 是 QMap，一个名字只能有一条。需要发多条同名头（quirk E8 的
    // 双 WWW-Authenticate 就是）时，把多个值用 '\n' 串起来放同一个 key，
    // HttpServer 写响应时会拆成多行。
    QMap<QByteArray, QByteArray> headers;
    QByteArray body;

    // ---- 故障注入钩子（由 quirk 驱动，普通 handler 不必碰）----
    int delayMs = 0;              // 回包前先等
    int slowSendChunk = 0;        // >0：分块慢发（F Slowloris）
    int slowSendIntervalMs = 0;
    bool dropConnection = false;  // 不回包，直接断
    bool forceClose = false;      // 回完就断，不 keep-alive
    QByteArray rawOverride;       // 非空则原样发这段字节，不再拼状态行与头（F1 畸形响应）

    void setBody(const QByteArray &data, const QByteArray &contentType);
    void setHeader(const QByteArray &name, const QByteArray &value);

    static HttpResponse text(int status, const QString &message);
    static HttpResponse soap(const QByteArray &xml, int status = 200);
    static HttpResponse jpeg(const QByteArray &data);
    static HttpResponse json(const QByteArray &data, int status = 200);
    static const char *defaultReason(int status);
};

// 一次请求 - 响应的句柄。handler 可以立刻 respond，也可以持有它稍后 respond。
// 客户端中途断开时发 aborted()，长轮询的 handler 应据此清理。
class HttpExchange : public QObject
{
    Q_OBJECT
public:
    ~HttpExchange() override;

    const HttpRequest &request() const;
    bool isResponded() const;
    bool isConnected() const;

    // 幂等：重复调用只有第一次生效。
    void respond(const HttpResponse &response);
    void respondText(int status, const QString &message);

    // ---- 长连接流式响应 ----
    // 海康的 /ISAPI/Event/notification/alertStream 与大华的
    // eventManager.cgi?action=attach 都是「连上就一直挂着、有事件推一段」的接口，
    // 真机上这条连接可以开几个小时。用 respond() 模拟只能周期性断开重连，
    // 客户端看到的重连风暴不是真机行为。
    //
    // beginStream() 之后连接保持打开、不再计 keep-alive 空闲超时；
    // writeChunk() 直接写 socket（连接已断返回 false，调用方据此停止推送）；
    // endStream() 收尾关连接。响应头里通常要写 multipart 的 boundary 或
    // Transfer-Encoding: chunked —— 用哪种由调用方在 headers 里决定，
    // 本类只负责把字节原样送出去。
    void beginStream(const HttpResponse &headers);
    bool writeChunk(const QByteArray &data);
    void endStream();
    bool isStreaming() const;

signals:
    void aborted();

protected:
    explicit HttpExchange(QObject *parent = nullptr);

private:
    friend class HttpServer;
    friend class HttpConnection;
    struct Private;
    Private *d;
};

using HttpExchangePtr = QSharedPointer<HttpExchange>;
using HttpHandler = std::function<void(const HttpRequest &, const HttpExchangePtr &)>;

} // namespace onvifsim

// 让 requestReceived 能走队列连接与 QSignalSpy。
Q_DECLARE_METATYPE(onvifsim::HttpRequest)
