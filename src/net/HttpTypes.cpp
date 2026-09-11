#include "net/HttpTypes.h"

#include "net/HttpExchange_p.h"

namespace onvifsim {
namespace {

// 请求头统一以小写 key 存，查询时把 name 也压成小写：
// 调用方不必记住报文里到底写的是 Content-Type 还是 CONTENT-TYPE。
QByteArray lowerKey(const char *name)
{
    return QByteArray(name).toLower();
}

// 响应头保留调用方写的大小写（真机吐出来的是 Content-Type 这种规范写法），
// 所以覆盖时只能逐个 key 做不区分大小写的比较。
void replaceHeader(QMap<QByteArray, QByteArray> &headers, const QByteArray &name,
                   const QByteArray &value)
{
    const QByteArray lower = name.toLower();
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        if (it.key().toLower() == lower) {
            it.value() = value;
            return;
        }
    }
    headers.insert(name, value);
}

} // namespace

QByteArray HttpRequest::header(const char *name) const
{
    return headers.value(lowerKey(name));
}

bool HttpRequest::hasHeader(const char *name) const
{
    return headers.contains(lowerKey(name));
}

QString HttpRequest::contentType() const
{
    return QString::fromUtf8(header("content-type"));
}

QString HttpRequest::hostHeader() const
{
    return QString::fromUtf8(header("host"));
}

QString HttpRequest::peerString() const
{
    if (peerAddress.isNull())
        return QString();
    return peerAddress.toString() + QLatin1Char(':') + QString::number(peerPort);
}

bool HttpRequest::wantsKeepAlive() const
{
    // Connection 是逗号分隔的 token 列表，close 与 keep-alive 可能和别的 token 混在一起。
    const QByteArray connection = header("connection").toLower();
    if (connection.contains("close"))
        return false;
    if (version.compare("HTTP/1.0", Qt::CaseInsensitive) == 0)
        return connection.contains("keep-alive");   // 1.0 默认短连接
    return true;                                    // 1.1 默认长连接
}

void HttpResponse::setBody(const QByteArray &data, const QByteArray &contentType)
{
    body = data;
    if (!contentType.isEmpty())
        setHeader("Content-Type", contentType);
}

void HttpResponse::setHeader(const QByteArray &name, const QByteArray &value)
{
    replaceHeader(headers, name, value);
}

HttpResponse HttpResponse::text(int status, const QString &message)
{
    HttpResponse r;
    r.status = status;
    r.setBody(message.toUtf8(), "text/plain; charset=utf-8");
    return r;
}

HttpResponse HttpResponse::soap(const QByteArray &xml, int status)
{
    HttpResponse r;
    r.status = status;
    // SOAP 1.2 的 content-type。收到 SOAP 1.1 请求时由 dispatcher 覆盖成 text/xml。
    r.setBody(xml, "application/soap+xml; charset=utf-8");
    return r;
}

HttpResponse HttpResponse::jpeg(const QByteArray &data)
{
    HttpResponse r;
    r.status = 200;
    // 空体也要带 Content-Type：quirk B4 就是 200 + image/jpeg + 零字节。
    r.setBody(data, "image/jpeg");
    return r;
}

HttpResponse HttpResponse::json(const QByteArray &data, int status)
{
    HttpResponse r;
    r.status = status;
    r.setBody(data, "application/json; charset=utf-8");
    return r;
}

const char *HttpResponse::defaultReason(int status)
{
    switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: break;
    }
    return "Unknown";
}

// HttpExchange 不设 QObject parent：生命周期由 QSharedPointer 管，
// 连接对象只在自己活着的时候多持一份强引用（见 HttpServer.cpp 的 HttpConnection）。
HttpExchange::HttpExchange(QObject *parent)
    : QObject(parent), d(new Private)
{
}

HttpExchange::~HttpExchange()
{
    delete d;
}

const HttpRequest &HttpExchange::request() const
{
    return d->request;
}

bool HttpExchange::isResponded() const
{
    return d->responded;
}

bool HttpExchange::isConnected() const
{
    return d->connected;
}

void HttpExchange::respond(const HttpResponse &response)
{
    // 幂等：长轮询里超时定时器与新消息可能同时到。
    // 与流式互斥：已经 beginStream() 的连接上再拼一个完整响应只会把流搅乱。
    if (d->responded || d->streaming)
        return;
    d->responded = true;
    if (!d->connected || !d->deliver)
        return;                     // 客户端已经走了，回包直接丢
    // 先取出再清空：deliver 内部会写 socket，可能反过来销毁连接对象。
    const std::function<void(const HttpResponse &)> deliver = d->deliver;
    d->detach();
    deliver(response);
}

void HttpExchange::respondText(int status, const QString &message)
{
    respond(HttpResponse::text(status, message));
}

void HttpExchange::beginStream(const HttpResponse &headers)
{
    if (d->responded || d->streaming)
        return;
    // 先置位再写头：即使客户端已经走了（deliver 早被清空），
    // 后面的 writeChunk() 也要一路返回 false，而不是退化成普通响应。
    d->streaming = true;
    if (!d->connected || !d->beginStream)
        return;
    d->beginStream(headers);
}

bool HttpExchange::writeChunk(const QByteArray &data)
{
    if (!d->streaming || !d->connected || !d->writeChunk)
        return false;               // 连接没了 —— 调用方据此停掉推送定时器
    return d->writeChunk(data);
}

void HttpExchange::endStream()
{
    if (!d->streaming)
        return;
    d->streaming = false;
    d->responded = true;            // 这次交互到此结束，后来的 respond() 一律忽略
    if (!d->connected || !d->endStream)
        return;
    const std::function<void()> endStream = d->endStream;
    d->detach();                    // 同 respond()：收尾会关连接，可能反噬调用栈
    endStream();
}

bool HttpExchange::isStreaming() const
{
    return d->streaming;
}

} // namespace onvifsim
