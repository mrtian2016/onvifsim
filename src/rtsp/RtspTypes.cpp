#include "rtsp/RtspTypes.h"

#include "rtsp/RtspInternal_p.h"

#include <QtCore/QList>

namespace onvifsim {
namespace {

// 请求头一律以小写 key 存，查询时把 name 也压成小写：
// 报文里写的是 CSeq 还是 cseq，调用方不必记。
QByteArray lowerKey(const char *name)
{
    return QByteArray(name).toLower();
}

// 响应头保留调用方写的规范大小写（真机吐的就是 CSeq / WWW-Authenticate 这种混写，
// 少数客户端按字面匹配），所以覆盖时只能逐个 key 做不区分大小写的比较。
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

// ONVIF Streaming Spec 定义的 backchannel option-tag。
const char kBackchannelTag[] = "www.onvif.org/ver20/backchannel";

// 各家写法都见过：海康 trackID、部分固件 track、少数写 streamid。
const char *const kTrackSuffixes[] = { "/trackID=", "/track=", "/streamid=" };

// "8000-8001" → (8000, 8001)。只给一个数时按 RFC 2326 惯例补 +1（RTCP 用奇数口）。
void parsePortPair(const QByteArray &value, quint16 *first, quint16 *second)
{
    const qsizetype dash = value.indexOf('-');
    bool ok = false;
    const quint16 a = quint16((dash >= 0 ? value.left(dash) : value).trimmed().toUInt(&ok));
    if (!ok)
        return;
    quint16 b = quint16(a + 1);
    if (dash >= 0) {
        bool ok2 = false;
        const quint16 parsed = quint16(value.mid(dash + 1).trimmed().toUInt(&ok2));
        if (ok2)
            b = parsed;
    }
    if (first)
        *first = a;
    if (second)
        *second = b;
}

} // namespace

QByteArray RtspRequest::header(const char *name) const
{
    return headers.value(lowerKey(name));
}

bool RtspRequest::hasHeader(const char *name) const
{
    return headers.contains(lowerKey(name));
}

int RtspRequest::cseq() const
{
    // 没有 CSeq 的请求是坏请求，回 -1 让调用方能区分「没写」和「写了 0」。
    bool ok = false;
    const int value = header("cseq").trimmed().toInt(&ok);
    return ok ? value : -1;
}

QByteArray RtspRequest::sessionId() const
{
    // Session 头可能带参数："12345678;timeout=60"，只取分号前那截。
    QByteArray value = header("session").trimmed();
    const qsizetype semi = value.indexOf(';');
    if (semi >= 0)
        value.truncate(semi);
    return value.trimmed();
}

bool RtspRequest::wantsBackchannel() const
{
    // Require 是逗号分隔的 option-tag 列表；同名头出现多次时解析侧已经并成一条。
    // 大小写不敏感：真机上 WWW.ONVIF.ORG 这种全大写写法出现过。
    return header("require").toLower().contains(kBackchannelTag);
}

void RtspResponse::setHeader(const QByteArray &name, const QByteArray &value)
{
    replaceHeader(headers, name, value);
}

void RtspResponse::addHeader(const QByteArray &name, const QByteArray &value)
{
    // 多个值用 '\n' 串在同一个 key 上，serialize() 再拆成多行。
    // 与 HttpResponse 同一套约定，两个模块别各写各的（见 net/HttpTypes.h）。
    const QByteArray lower = name.toLower();
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        if (it.key().toLower() == lower) {
            it.value() += '\n' + value;
            return;
        }
    }
    headers.insert(name, value);
}

void RtspResponse::setBody(const QByteArray &data, const QByteArray &contentType)
{
    body = data;
    if (!contentType.isEmpty())
        setHeader("Content-Type", contentType);
}

QByteArray RtspResponse::serialize() const
{
    QByteArray out;
    out.reserve(256 + body.size());
    out += "RTSP/1.0 ";
    out += QByteArray::number(status);
    out += ' ';
    out += reason.isEmpty() ? QByteArray(defaultReason(status)) : reason;
    out += "\r\n";

    QMap<QByteArray, QByteArray> all = headers;
    // 有体必须带 Content-Length（DESCRIBE 的 SDP 靠它定界）；没体时不主动写，
    // 免得给「本来就不该有体」的响应加上多余的头。
    if (!body.isEmpty()) {
        bool hasLength = false;
        for (auto it = all.cbegin(); it != all.cend(); ++it) {
            if (it.key().toLower() == "content-length") {
                hasLength = true;
                break;
            }
        }
        if (!hasLength)
            all.insert("Content-Length", QByteArray::number(body.size()));
    }

    for (auto it = all.cbegin(); it != all.cend(); ++it) {
        // addHeader 把同名头的多个值用 '\n' 串在一起了，这里拆回多行。
        for (const QByteArray &value : it.value().split('\n')) {
            out += it.key();
            out += ": ";
            out += value;
            out += "\r\n";
        }
    }
    out += "\r\n";
    out += body;
    return out;
}

const char *RtspResponse::defaultReason(int status)
{
    switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 250: return "Low on Storage Space";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 415: return "Unsupported Media Type";
    // 下面这段是 RTSP 特有的 45x，客户端的诊断信息全靠它们区分。
    case 451: return "Parameter Not Understood";
    case 452: return "Conference Not Found";
    case 453: return "Not Enough Bandwidth";
    case 454: return "Session Not Found";
    case 455: return "Method Not Valid in This State";
    case 456: return "Header Field Not Valid for Resource";
    case 457: return "Invalid Range";
    case 458: return "Parameter Is Read-Only";
    case 459: return "Aggregate Operation Not Allowed";
    case 460: return "Only Aggregate Operation Allowed";
    case 461: return "Unsupported Transport";
    case 462: return "Destination Unreachable";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "RTSP Version Not Supported";
    case 551: return "Option Not Supported";
    default: break;
    }
    return "Unknown";
}

RtspResponse RtspResponse::make(int status, int cseq)
{
    RtspResponse r;
    r.status = status;
    // CSeq 必须原样回；客户端按它对上请求，缺了就整条会话作废。
    if (cseq >= 0)
        r.setHeader("CSeq", QByteArray::number(cseq));
    return r;
}

RtspTransport RtspTransport::parse(const QByteArray &transportHeader, bool *ok)
{
    if (ok)
        *ok = false;
    RtspTransport result;

    // 客户端可以在一个 Transport 头里用逗号列若干候选（RFC 2326 §12.39），
    // 服务器挑第一条自己支持的。UDP 与 TCP interleaved 我们都支持，所以就是第一条。
    const QList<QByteArray> alternatives = transportHeader.split(',');
    for (const QByteArray &alternative : alternatives) {
        const QList<QByteArray> parts = alternative.split(';');
        if (parts.isEmpty())
            continue;
        const QByteArray spec = parts.at(0).trimmed().toUpper();
        if (!spec.startsWith("RTP/AVP"))
            continue;                       // RTP/SAVP、MP2T 之类一律跳过
        const QByteArray lower = spec.mid(7);
        RtspTransport candidate;
        if (lower == "/TCP")
            candidate.tcpInterleaved = true;
        else if (!lower.isEmpty() && lower != "/UDP")
            continue;                       // RTP/AVP/<别的下层协议>

        for (qsizetype i = 1; i < parts.size(); ++i) {
            const QByteArray param = parts.at(i).trimmed();
            if (param.isEmpty())
                continue;
            const qsizetype eq = param.indexOf('=');
            const QByteArray key = (eq >= 0 ? param.left(eq) : param).trimmed().toLower();
            QByteArray value = eq >= 0 ? param.mid(eq + 1).trimmed() : QByteArray();
            // 有的客户端给 mode 之类的参数加引号。
            if (value.size() >= 2 && value.startsWith('"') && value.endsWith('"'))
                value = value.mid(1, value.size() - 2);

            if (key == "unicast") {
                candidate.multicast = false;
            } else if (key == "multicast") {
                candidate.multicast = true;
            } else if (key == "interleaved") {
                quint16 a = 0;
                quint16 b = 1;
                parsePortPair(value, &a, &b);
                candidate.interleavedRtp = int(a);
                candidate.interleavedRtcp = int(b);
                candidate.tcpInterleaved = true;   // 带 interleaved 就是走控制连接
            } else if (key == "client_port") {
                parsePortPair(value, &candidate.clientRtpPort, &candidate.clientRtcpPort);
            } else if (key == "server_port") {
                parsePortPair(value, &candidate.serverRtpPort, &candidate.serverRtcpPort);
            } else if (key == "port") {
                parsePortPair(value, &candidate.clientRtpPort, &candidate.clientRtcpPort);
            } else if (key == "destination") {
                candidate.destination = QString::fromUtf8(value);
            }
        }

        result = candidate;
        if (ok)
            *ok = true;
        return result;
    }
    return result;
}

QByteArray RtspTransport::toHeader() const
{
    QByteArray out = tcpInterleaved ? QByteArray("RTP/AVP/TCP") : QByteArray("RTP/AVP");
    out += multicast ? ";multicast" : ";unicast";
    if (!destination.isEmpty())
        out += ";destination=" + destination.toUtf8();

    if (tcpInterleaved) {
        out += ";interleaved=" + QByteArray::number(interleavedRtp)
            + '-' + QByteArray::number(interleavedRtcp);
        return out;
    }

    if (clientRtpPort != 0) {
        out += ";client_port=" + QByteArray::number(clientRtpPort)
            + '-' + QByteArray::number(clientRtcpPort);
    }
    // server_port 回给客户端，它据此往回发 RTCP RR。
    if (serverRtpPort != 0) {
        out += ";server_port=" + QByteArray::number(serverRtpPort)
            + '-' + QByteArray::number(serverRtcpPort);
    }
    return out;
}

namespace rtspInternal {

qsizetype parseRequest(const QByteArray &buffer, RtspRequest *out, bool *malformed)
{
    if (malformed)
        *malformed = false;

    qsizetype separator = 4;
    qsizetype end = buffer.indexOf("\r\n\r\n");
    const qsizetype bare = buffer.indexOf("\n\n");
    // 少数固件与手写客户端只发 LF，不发 CRLF。收得下就收。
    if (end < 0 || (bare >= 0 && bare < end)) {
        if (bare < 0)
            return 0;
        end = bare;
        separator = 2;
    }

    QList<QByteArray> lines = buffer.left(end).split('\n');
    for (QByteArray &line : lines) {
        if (line.endsWith('\r'))
            line.chop(1);
    }
    if (lines.isEmpty()) {
        if (malformed)
            *malformed = true;
        return end + separator;
    }

    RtspRequest request;
    const QList<QByteArray> requestLine = lines.first().simplified().split(' ');
    if (requestLine.size() < 3) {
        if (malformed)
            *malformed = true;
        return end + separator;
    }
    request.method = requestLine.at(0).toUpper();
    request.uri = QString::fromUtf8(requestLine.at(1));
    request.version = requestLine.at(2);

    QByteArray lastKey;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray &line = lines.at(i);
        if (line.isEmpty())
            continue;
        if ((line.startsWith(' ') || line.startsWith('\t')) && !lastKey.isEmpty()) {
            request.headers[lastKey] += ' ' + line.trimmed();   // 折行续写
            continue;
        }
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        const QByteArray key = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        // 同名头并成逗号列表：Require 允许出现多次，分开存 wantsBackchannel 就看漏了。
        if (request.headers.contains(key))
            request.headers[key] += ", " + value;
        else
            request.headers.insert(key, value);
        lastKey = key;
    }

    const qint64 contentLength =
        qMax<qint64>(0, request.headers.value("content-length").toLongLong());
    const qsizetype total = end + separator + qsizetype(contentLength);
    if (buffer.size() < total)
        return 0;                       // 体还没收全，等下一批
    request.body = buffer.mid(end + separator, qsizetype(contentLength));
    if (out)
        *out = request;
    return total;
}

QString requestTarget(const QString &uri)
{
    if (uri == QLatin1String("*"))
        return QStringLiteral("/");
    const qsizetype schemeEnd = uri.indexOf(QLatin1String("://"));
    if (schemeEnd < 0)
        return uri.isEmpty() ? QStringLiteral("/") : uri;
    const qsizetype slash = uri.indexOf(QLatin1Char('/'), schemeEnd + 3);
    return slash < 0 ? QStringLiteral("/") : uri.mid(slash);
}

int trackIndexOf(const QString &target)
{
    for (const char *prefix : kTrackSuffixes) {
        const QString needle = QString::fromLatin1(prefix);
        const qsizetype at = target.lastIndexOf(needle, -1, Qt::CaseInsensitive);
        if (at < 0)
            continue;
        bool ok = false;
        const int value = target.mid(at + needle.size()).toInt(&ok);
        if (ok)
            return value;
    }
    return -1;
}

QString normalizeStreamPath(const QString &raw)
{
    QString path = raw.trimmed();
    // 客户端按 a=control 拼出来的 SETUP URL 末尾会多一段 trackID=N。大华的路径带 query
    // （/cam/realmonitor?channel=1&subtype=0/trackID=0），所以必须在「路径 + query」
    // 整串上剥 —— 先用 QUrl 拆掉 query 就找不回来了。
    for (const char *suffix : kTrackSuffixes) {
        const qsizetype at = path.lastIndexOf(QLatin1String(suffix), -1, Qt::CaseInsensitive);
        if (at >= 0) {
            path.truncate(at);
            break;
        }
    }
    while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    if (!path.startsWith(QLatin1Char('/')))
        path.prepend(QLatin1Char('/'));
    return path;
}

} // namespace rtspInternal

} // namespace onvifsim
