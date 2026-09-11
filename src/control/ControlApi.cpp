#include "control/ControlApi.h"

#include "core/Persona.h"
#include "core/Scenario.h"
#include "core/Simulator.h"
#include "core/VirtualCamera.h"
#include "discovery/DiscoveryResponder.h"
#include "events/EventEngine.h"
#include "events/SubscriptionManager.h"
#include "imaging/ImagingState.h"
#include "media/MediaTypes.h"
#include "net/IpAlias.h"
#include "net/NetUtil.h"
#include "ptz/PtzState.h"
#include "rtsp/RtpReceiver.h"
#include "rtsp/RtpSender.h"
#include "rtsp/RtspServer.h"
#include "rtsp/RtspSession.h"
#include "version.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

namespace onvifsim {
namespace {

// 控制面自己实现一份最小 HTTP，而不是复用 VirtualCamera 的 HttpServer：
// 一是它不属于任何一台相机，二是 /api/log 的 SSE 需要一直往同一个 socket 写，
// 而 HttpServer 的 HttpExchange 是「一次请求一次响应」的模型。
struct ParsedRequest {
    QByteArray method;
    QString path;
    QUrlQuery query;
    QMap<QByteArray, QByteArray> headers;
    QByteArray body;
    bool complete = false;
};

bool parseRequest(const QByteArray &buffer, ParsedRequest *out)
{
    const int headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0)
        return false;

    const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
    if (lines.isEmpty())
        return false;

    const QList<QByteArray> parts = lines.first().trimmed().split(' ');
    if (parts.size() < 2)
        return false;
    out->method = parts.at(0);

    const QUrl url = QUrl::fromEncoded(parts.at(1));
    out->path = url.path();
    out->query = QUrlQuery(url.query());

    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const int colon = line.indexOf(':');
        if (colon > 0)
            out->headers.insert(line.left(colon).toLower(), line.mid(colon + 1).trimmed());
    }

    // Content-Length 必须查 ok 并卡下界：负数会让下面的 mid(bodyStart, -1)
    // 退化成「取到缓冲区末尾」，把后续流水线上的数据一并当成本次请求的 body。
    bool lengthOk = false;
    const int declared = out->headers.value("content-length").toInt(&lengthOk);
    if (!lengthOk && out->headers.contains("content-length"))
        return false;   // 头在但不是数字：当成没收全，等它超时断开
    const int contentLength = qMax(0, declared);
    const int bodyStart = headerEnd + 4;
    if (buffer.size() - bodyStart < contentLength)
        return false;   // body 还没收全

    out->body = buffer.mid(bodyStart, contentLength);
    out->complete = true;
    return true;
}

QByteArray statusLine(int status)
{
    switch (status) {
    case 200: return "200 OK";
    case 201: return "201 Created";
    case 204: return "204 No Content";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 409: return "409 Conflict";
    case 500: return "500 Internal Server Error";
    default:  break;
    }
    return QByteArray::number(status) + " Status";
}

QJsonObject jsonError(const QString &message)
{
    QJsonObject o;
    o.insert(QStringLiteral("error"), message);
    return o;
}

QString ptzStatusName(PtzMoveStatus s)
{
    switch (s) {
    case PtzMoveStatus::Moving:  return QStringLiteral("MOVING");
    case PtzMoveStatus::Unknown: return QStringLiteral("UNKNOWN");
    case PtzMoveStatus::Idle:    break;
    }
    return QStringLiteral("IDLE");
}

// Prometheus 的 label 值里反斜杠、双引号、换行都要转义。相机 id 是 REST 与
// 场景 JSON 完全可控的字符串，直接拼进去的话，一个带引号的 id 就能把整份
// metrics 输出弄成语法错误（甚至注入额外的 label）。
QByteArray prometheusLabel(const QString &cameraId)
{
    QByteArray escaped;
    const QByteArray raw = cameraId.toUtf8();
    escaped.reserve(raw.size() + 8);
    for (char c : raw) {
        if (c == '\\' || c == '"')
            escaped += '\\';
        else if (c == '\n') {
            escaped += "\\n";
            continue;
        }
        escaped += c;
    }
    return "{camera=\"" + escaped + "\"}";
}

} // namespace

struct ControlApi::Private {
    Simulator *simulator = nullptr;
    QTcpServer *server = nullptr;
    QString token;
    QList<QTcpSocket *> sseClients;
    IpAlias *ipAlias = nullptr;
    QMetaObject::Connection logConnection;
    int lastStatus = 200;

    // 跨域头**只在设了 token 时才发**。
    //
    // 没有 token 时放 `Allow-Origin: *` 是一条完整的 drive-by 提权链的前半段：
    // 用户开着 onvifsim，随便访问一个网页，页面里的 JS 就能删相机、重载场景、
    // 调 /api/network。绑回环地址挡不住这个 —— 浏览器本来就跑在本机。
    // 不发这些头，浏览器就会拦下跨源读取和带 JSON body 的预检请求；
    // curl / 脚本 / e2e 不受任何影响（它们根本不看 CORS）。
    QByteArray corsHeaders() const
    {
        if (token.isEmpty())
            return QByteArray();
        return "Access-Control-Allow-Origin: *\r\n"
               "Access-Control-Allow-Headers: authorization, content-type\r\n"
               "Access-Control-Allow-Methods: GET, POST, PATCH, DELETE, OPTIONS\r\n";
    }

    void send(QTcpSocket *socket, int status, const QByteArray &contentType,
              const QByteArray &body)
    {
        // 真正发出去的状态码只有这里知道。调用方各自维护的 status 变量会漂移
        // （直接 sendJson(socket, 409, ...) 的分支就不会更新它），所以以这里为准。
        lastStatus = status;
        QByteArray head = "HTTP/1.1 " + statusLine(status) + "\r\n";
        head += "Content-Type: " + contentType + "\r\n";
        head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        head += corsHeaders();
        head += "Connection: close\r\n\r\n";
        socket->write(head);
        socket->write(body);
        socket->flush();
        socket->disconnectFromHost();
    }

    void sendJson(QTcpSocket *socket, int status, const QJsonObject &object)
    {
        send(socket, status, "application/json; charset=utf-8",
             QJsonDocument(object).toJson(QJsonDocument::Compact));
    }

    void sendJson(QTcpSocket *socket, int status, const QJsonArray &array)
    {
        send(socket, status, "application/json; charset=utf-8",
             QJsonDocument(array).toJson(QJsonDocument::Compact));
    }
};

ControlApi::ControlApi(Simulator *simulator, QObject *parent) : QObject(parent), d(new Private)
{
    d->simulator = simulator;
    d->ipAlias = new IpAlias(this);
}

ControlApi::~ControlApi()
{
    stop();
    delete d;
}

bool ControlApi::start(const QHostAddress &address, quint16 port, QString *errorOut)
{
    if (d->server)
        return true;

    d->server = new QTcpServer(this);
    if (!d->server->listen(address, port)) {
        if (errorOut)
            *errorOut = QCoreApplication::translate("onvifsim::core", "控制面监听 %1:%2 失败：%3")
                            .arg(address.toString())
                            .arg(port)
                            .arg(d->server->errorString());
        delete d->server;
        d->server = nullptr;
        return false;
    }

    // 绑到非回环地址却不设 token，等于把「删相机 / 重载场景 / 加 IP 别名」
    // 这组操作对整个网段敞开。容器里确实需要这么跑（--control-bind 0.0.0.0），
    // 所以不拦，但必须喊一声 —— 静默地开着一个无鉴权的控制面是最坏的组合。
    if (d->token.isEmpty() && !address.isLoopback() && d->simulator) {
        if (LogBus *bus = d->simulator->logBus()) {
            bus->warning(logcat::Control, QString(),
                         QStringLiteral("Control API is listening on %1 with **no access token**: "
                                        "anyone on this network can delete cameras, change "
                                        "configuration and add IP aliases. "
                                        "Set --control-token in anything but a lab.")
                             .arg(address.toString()));
        }
    }

    connect(d->server, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket *socket = d->server->nextPendingConnection()) {
            auto *buffer = new QByteArray;
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                buffer->append(socket->readAll());
                ParsedRequest request;
                if (!parseRequest(*buffer, &request) || !request.complete)
                    return;
                buffer->clear();
                handleRequest(socket, request.method, request.path, request.query,
                              request.headers, request.body);
            });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket, buffer] {
                d->sseClients.removeAll(socket);
                delete buffer;
                socket->deleteLater();
            });
        }
    });

    // SSE 日志流：日志总线一有新记录就推给所有订阅者。
    //
    // stop() 不会断开它，而 stop()→start() 是常规操作（GUI 的启停按钮、
    // POST /api/scenario、Simulator::loadScenario 都走这条路）。所以这里必须
    // 防重复连接，否则启停 N 次之后每条日志会往每个订阅者推 N 遍，
    // 而且 lambda 永不释放。Qt::UniqueConnection 对 lambda 无效，只能自己记。
    if (d->logConnection)
        disconnect(d->logConnection);
    d->logConnection = connect(
        d->simulator->logBus(), &LogBus::recordPosted, this, [this](const LogRecord &record) {
        if (d->sseClients.isEmpty())
            return;
        QJsonObject o;
        o.insert(QStringLiteral("time"), record.timestamp.toString(Qt::ISODateWithMs));
        o.insert(QStringLiteral("level"), record.levelName());
        o.insert(QStringLiteral("category"), record.category);
        o.insert(QStringLiteral("camera"), record.cameraId);
        o.insert(QStringLiteral("peer"), record.peer);
        o.insert(QStringLiteral("summary"), record.summary);
        o.insert(QStringLiteral("ok"), record.ok);
        if (!record.quirkKey.isEmpty())
            o.insert(QStringLiteral("quirk"), record.quirkKey);
        if (record.durationUs >= 0)
            o.insert(QStringLiteral("durationUs"), record.durationUs);
        const QByteArray frame = "data: " + QJsonDocument(o).toJson(QJsonDocument::Compact)
                                 + "\n\n";
        // 先取快照再遍历：flush() 写失败会同步走到 disconnected 槽，
        // 那里会 removeAll(socket) —— 直接在 d->sseClients 上 range-for
        // 的话迭代器当场失效，是实打实的未定义行为。
        const QList<QTcpSocket *> clients = d->sseClients;
        for (QTcpSocket *client : clients) {
            if (!d->sseClients.contains(client))
                continue;
            client->write(frame);
            client->flush();
        }
        });

    return true;
}

void ControlApi::stop()
{
    if (!d->server)
        return;
    // 同样先取快照：disconnectFromHost() 在没有待发数据时是**同步**发
    // disconnected 的，槽里会把 socket 从 d->sseClients 里摘掉。
    const QList<QTcpSocket *> clients = d->sseClients;
    d->sseClients.clear();
    for (QTcpSocket *client : clients)
        client->disconnectFromHost();
    d->server->close();
    d->server->deleteLater();
    d->server = nullptr;
}

bool ControlApi::isRunning() const
{
    return d->server && d->server->isListening();
}

quint16 ControlApi::port() const
{
    return d->server ? d->server->serverPort() : 0;
}

void ControlApi::setToken(const QString &token)
{
    d->token = token;
}

QString ControlApi::token() const
{
    return d->token;
}

int ControlApi::sseClientCount() const
{
    return d->sseClients.size();
}

void ControlApi::handleRequest(QTcpSocket *socket, const QByteArray &method, const QString &path,
                               const QUrlQuery &query,
                               const QMap<QByteArray, QByteArray> &headers, const QByteArray &body)
{
    QElapsedTimer timer;
    timer.start();
    d->lastStatus = 200;

    routeRequest(socket, method, path, query, headers, body);

    const int status = d->lastStatus;
    const QString methodText = QString::fromLatin1(method);

    // 控制面能删相机、重载场景、把相机打离线、加 IP 别名 —— 这些改动必须在
    // 日志里留痕，否则事后看「相机怎么没了」无从查起。CLAUDE.md 的约定是
    // 每一步都往 LogBus 发一条记录，在这个模块上之前是完全缺失的：
    // logcat::Control 这个分类在 GUI 的过滤器里列着，却从来没有记录产出。
    if (d->simulator) {
        if (LogBus *bus = d->simulator->logBus()) {
            LogRecord rec;
            rec.category = logcat::Control;
            rec.peer = socket ? QStringLiteral("%1:%2").arg(socket->peerAddress().toString())
                                    .arg(socket->peerPort())
                              : QString();
            rec.summary = QStringLiteral("%1 %2 → %3").arg(methodText, path).arg(status);
            rec.ok = status < 400;
            rec.durationUs = timer.nsecsElapsed() / 1000;
            // 读操作（GET 列表 / metrics / SSE）量大且无副作用，压到 Debug，
            // 免得 GUI 日志被 e2e 和监控轮询刷屏；写操作与错误照常可见。
            const bool mutating = method != "GET" && method != "OPTIONS";
            rec.level = status >= 400 ? LogLevel::Warning
                                      : (mutating ? LogLevel::Info : LogLevel::Debug);
            if (!body.isEmpty() && mutating)
                rec.detail = QString::fromUtf8(body.left(4096));
            bus->post(rec);
        }
    }
}

void ControlApi::sendNotFound(QTcpSocket *socket, const QString &path)
{
    d->sendJson(socket, 404, jsonError(QStringLiteral("没有这个端点：%1").arg(path)));
}

void ControlApi::sendBadRequest(QTcpSocket *socket, const QString &why)
{
    d->sendJson(socket, 400, jsonError(why));
}

bool ControlApi::parseJsonBody(QTcpSocket *socket, const QByteArray &body, QJsonObject *out)
{
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        sendBadRequest(socket,
                       QStringLiteral("请求体不是合法 JSON 对象：%1").arg(error.errorString()));
        return false;
    }
    *out = doc.object();
    return true;
}

void ControlApi::routeRequest(QTcpSocket *socket, const QByteArray &method, const QString &path,
                              const QUrlQuery &query,
                              const QMap<QByteArray, QByteArray> &headers, const QByteArray &body)
{
    // 浏览器里的调试页面会先发 OPTIONS 预检。
    if (method == "OPTIONS") {
        d->send(socket, 204, "text/plain", QByteArray());
        return;
    }

    if (!d->token.isEmpty()) {
        const QByteArray bearer = headers.value("authorization");
        const bool headerOk = bearer == QByteArray("Bearer ") + d->token.toUtf8();
        const bool queryOk = query.queryItemValue(QStringLiteral("token")) == d->token;
        if (!headerOk && !queryOk) {
            d->sendJson(socket, 401, jsonError(QStringLiteral("需要访问令牌")));
            return;
        }
    }

    const QStringList segments = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);

    if (handleMetrics(socket, path))
        return;
    if (handleOpenApi(socket, path))
        return;

    if (segments.isEmpty() || segments.first() != QLatin1String("api")) {
        sendNotFound(socket, path);
        return;
    }

    const QString section = segments.value(1);

    if (handleLogStream(socket, section))
        return;
    if (handleQuirks(socket, section))
        return;
    if (handlePresets(socket, section))
        return;
    if (handleNetwork(socket, method, section, body))
        return;
    if (handleScenario(socket, method, section, body))
        return;
    if (handleCameras(socket, method, path, segments, query, body))
        return;

    sendNotFound(socket, path);
}

// GET /metrics —— Prometheus 文本格式。
bool ControlApi::handleMetrics(QTcpSocket *socket, const QString &path)
{
    // ---- /metrics ----
    if (path == QLatin1String("/metrics")) {
        // 相机状态取一次就够：status() 会遍历该相机的全部 RTSP 会话，
        // 每个 metric family 各取一次等于把这件事做三遍。
        struct Sample {
            QByteArray label;
            CameraStatus status;
        };
        QList<Sample> samples;
        samples.reserve(d->simulator->cameras().size());
        for (const VirtualCamera *cam : d->simulator->cameras())
            samples.append({ prometheusLabel(cam->id()), cam->status() });

        // 同一个 family 的样本必须**连续**输出，这是文本格式的硬要求。
        // 原来是在一个循环里三个 family 交错着写的。
        auto family = [&samples](QByteArray &out, const char *name, const char *help,
                                 const char *type, auto valueOf) {
            out += "# HELP ";
            out += name;
            out += ' ';
            out += help;
            out += "\n# TYPE ";
            out += name;
            out += ' ';
            out += type;
            out += '\n';
            for (const Sample &sample : samples) {
                out += name;
                out += sample.label;
                out += ' ';
                out += QByteArray::number(valueOf(sample.status));
                out += '\n';
            }
        };

        QByteArray text;
        text += "# HELP onvifsim_cameras 相机数量\n# TYPE onvifsim_cameras gauge\n";
        text += "onvifsim_cameras " + QByteArray::number(d->simulator->cameraCount()) + "\n";

        family(text, "onvifsim_rtsp_sessions", "当前 RTSP 会话数", "gauge",
               [](const CameraStatus &s) { return s.rtspSessions; });
        family(text, "onvifsim_subscriptions", "当前 PullPoint 订阅数", "gauge",
               [](const CameraStatus &s) { return s.subscriptions; });
        // 这个 family 原来连 HELP / TYPE 都没有。
        family(text, "onvifsim_camera_online", "相机是否在线（1 = 在线）", "gauge",
               [](const CameraStatus &s) { return s.running && !s.offline ? 1 : 0; });
        family(text, "onvifsim_talkback_bytes_total", "对讲（backchannel）累计收到的字节数",
               "counter", [](const CameraStatus &s) { return s.talkbackBytes; });

        text += "# HELP onvifsim_sse_clients 当前挂着的日志 SSE 订阅者数\n"
                "# TYPE onvifsim_sse_clients gauge\n";
        text += "onvifsim_sse_clients " + QByteArray::number(sseClientCount()) + "\n";

        if (const DiscoveryResponder *discovery = d->simulator->discovery()) {
            text += "# HELP onvifsim_discovery_probes_total 收到的 WS-Discovery Probe 总数\n"
                    "# TYPE onvifsim_discovery_probes_total counter\n";
            text += "onvifsim_discovery_probes_total "
                    + QByteArray::number(discovery->probesReceived()) + "\n";
            text += "# HELP onvifsim_discovery_matches_total 发出的 ProbeMatch 总数\n"
                    "# TYPE onvifsim_discovery_matches_total counter\n";
            text += "onvifsim_discovery_matches_total "
                    + QByteArray::number(discovery->matchesSent()) + "\n";
        }

        d->send(socket, 200, "text/plain; version=0.0.4; charset=utf-8", text);
        return true;
    }

    return false;
}

// GET /openapi.json —— 端点清单从实际路由生成。
bool ControlApi::handleOpenApi(QTcpSocket *socket, const QString &path)
{
    if (path == QLatin1String("/openapi.json")) {
        QJsonObject spec;
        spec.insert(QStringLiteral("openapi"), QStringLiteral("3.0.0"));
        QJsonObject info;
        info.insert(QStringLiteral("title"), QStringLiteral("onvifsim 控制面"));
        info.insert(QStringLiteral("version"), QLatin1String(ONVIFSIM_VERSION));
        spec.insert(QStringLiteral("info"), info);
        // 端点清单从实际路由生成，避免文档与实现漂移。
        QJsonObject paths;
        const char *endpoints[] = {
            "/api/cameras", "/api/cameras/{id}", "/api/cameras/{id}/events",
            "/api/cameras/{id}/ptz", "/api/cameras/{id}/sessions",
            "/api/cameras/{id}/talkback", "/api/cameras/{id}/offline",
            "/api/scenario", "/api/network", "/api/quirks", "/api/presets",
            "/api/log", "/metrics"
        };
        for (const char *e : endpoints)
            paths.insert(QLatin1String(e), QJsonObject());
        spec.insert(QStringLiteral("paths"), paths);
        d->sendJson(socket, 200, spec);
        return true;
    }

    return false;
}

// GET /api/log —— SSE 日志流。
bool ControlApi::handleLogStream(QTcpSocket *socket, const QString &section)
{
    // ---- /api/log（SSE）----
    if (section == QLatin1String("log")) {
        QByteArray head = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/event-stream; charset=utf-8\r\n"
                          "Cache-Control: no-cache\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Connection: keep-alive\r\n\r\n";
        socket->write(head);
        // 先把历史回填一段，新接上的客户端不至于看到空白。
        const QVector<LogRecord> history = d->simulator->logBus()->history();
        const int backlog = qMin(history.size(), 200);
        for (int i = history.size() - backlog; i < history.size(); ++i) {
            QJsonObject o;
            o.insert(QStringLiteral("time"), history.at(i).timestamp.toString(Qt::ISODateWithMs));
            o.insert(QStringLiteral("level"), history.at(i).levelName());
            o.insert(QStringLiteral("category"), history.at(i).category);
            o.insert(QStringLiteral("camera"), history.at(i).cameraId);
            o.insert(QStringLiteral("summary"), history.at(i).summary);
            socket->write("data: " + QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n\n");
        }
        socket->flush();
        d->sseClients.append(socket);
        return true;
    }

    return false;
}

// GET /api/quirks —— 全部故障注入开关的元数据。
bool ControlApi::handleQuirks(QTcpSocket *socket, const QString &section)
{
    // ---- /api/quirks ----
    if (section == QLatin1String("quirks")) {
        QJsonArray array;
        for (const QuirkDef &def : QuirkRegistry::all()) {
            QJsonObject o;
            o.insert(QStringLiteral("key"), def.key);
            o.insert(QStringLiteral("group"), QuirkRegistry::groupKey(def.group));
            o.insert(QStringLiteral("groupTitle"), QuirkRegistry::groupTitle(def.group));
            o.insert(QStringLiteral("sourceId"), def.sourceId);
            o.insert(QStringLiteral("title"), def.title);
            o.insert(QStringLiteral("description"), def.description);
            QJsonArray params;
            for (const QuirkParamDef &p : def.params) {
                QJsonObject po;
                po.insert(QStringLiteral("name"), p.name);
                po.insert(QStringLiteral("default"),
                          QJsonValue::fromVariant(p.defaultValue));
                if (p.minValue.isValid())
                    po.insert(QStringLiteral("min"), QJsonValue::fromVariant(p.minValue));
                if (p.maxValue.isValid())
                    po.insert(QStringLiteral("max"), QJsonValue::fromVariant(p.maxValue));
                if (!p.choices.isEmpty())
                    po.insert(QStringLiteral("choices"), QJsonArray::fromStringList(p.choices));
                po.insert(QStringLiteral("description"), p.description);
                params.append(po);
            }
            if (!params.isEmpty())
                o.insert(QStringLiteral("params"), params);
            array.append(o);
        }
        d->sendJson(socket, 200, array);
        return true;
    }

    return false;
}

// GET /api/presets —— 品牌预设清单。
bool ControlApi::handlePresets(QTcpSocket *socket, const QString &section)
{
    // ---- /api/presets ----
    if (section == QLatin1String("presets")) {
        QJsonArray array;
        for (const Persona &p : PersonaRegistry::all())
            array.append(p.toJson());
        d->sendJson(socket, 200, array);
        return true;
    }

    return false;
}

// GET|POST /api/network —— 网卡、网络模式、IP 别名。
bool ControlApi::handleNetwork(QTcpSocket *socket, const QByteArray &method, const QString &section,
                              const QByteArray &body)
{
    // ---- /api/network ----
    if (section == QLatin1String("network")) {
        if (method == "GET") {
            QJsonObject o;
            QJsonArray ifaces;
            for (const InterfaceInfo &info : netutil::interfaces()) {
                QJsonObject io;
                io.insert(QStringLiteral("name"), info.name);
                io.insert(QStringLiteral("displayName"), info.displayName);
                io.insert(QStringLiteral("mac"), info.hardwareAddress);
                io.insert(QStringLiteral("up"), info.isUp);
                io.insert(QStringLiteral("multicast"), info.supportsMulticast);
                QJsonArray addrs;
                for (const QHostAddress &a : info.addresses)
                    addrs.append(a.toString());
                io.insert(QStringLiteral("addresses"), addrs);
                ifaces.append(io);
            }
            o.insert(QStringLiteral("interfaces"), ifaces);
            const SimulatorConfig config = d->simulator->config();
            o.insert(QStringLiteral("mode"),
                     config.networkMode == NetworkMode::IpAlias ? QStringLiteral("ip_alias")
                     : config.networkMode == NetworkMode::External ? QStringLiteral("external")
                                                                   : QStringLiteral("ports"));
            o.insert(QStringLiteral("needsElevation"), IpAlias::needsElevation());
            o.insert(QStringLiteral("elevationHint"), IpAlias::elevationHint());
            QJsonArray owned;
            for (const IpAliasRequest &r : d->ipAlias->ownedAliases())
                owned.append(r.address.toString());
            o.insert(QStringLiteral("ownedAliases"), owned);
            d->sendJson(socket, 200, o);
        } else if (method == "POST") {
            QJsonObject request;
            if (!parseJsonBody(socket, body, &request)) {
                return true;
            }
            const QString action = request.value(QStringLiteral("action")).toString();
            if (action == QLatin1String("add_range")) {
                QHostAddress start;
                if (!start.setAddress(request.value(QStringLiteral("start")).toString())) {
                    sendBadRequest(socket, QStringLiteral("start 不是合法 IP"));
                    return true;
                }
                QHostAddress mask(QStringLiteral("255.255.255.0"));
                if (request.contains(QStringLiteral("netmask")))
                    mask.setAddress(request.value(QStringLiteral("netmask")).toString());
                const IpAliasResult result = d->ipAlias->addRange(
                    request.value(QStringLiteral("interface")).toString(), start,
                    request.value(QStringLiteral("count")).toInt(1), mask);
                QJsonObject o;
                o.insert(QStringLiteral("ok"), result.ok);
                if (!result.ok) {
                    o.insert(QStringLiteral("error"), result.errorMessage);
                    // 提权失败时把手动命令给用户，不静默降级。
                    o.insert(QStringLiteral("manualCommands"),
                             QJsonArray::fromStringList(result.manualCommands));
                }
                d->sendJson(socket, result.ok ? 200 : 409, o);
            } else if (action == QLatin1String("remove_all")) {
                d->ipAlias->removeAllOwned();
                d->sendJson(socket, 200, QJsonObject{ { QStringLiteral("ok"), true } });
            } else {
                sendBadRequest(socket, QStringLiteral("未知的 action：%1").arg(action));
            }
        } else {
            d->sendJson(socket, 405, jsonError(QStringLiteral("方法不支持")));
        }
        return true;
    }

    return false;
}

// GET|POST /api/scenario —— 导出 / 加载场景。
bool ControlApi::handleScenario(QTcpSocket *socket, const QByteArray &method, const QString &section,
                               const QByteArray &body)
{
    // ---- /api/scenario ----
    if (section == QLatin1String("scenario")) {
        if (method == "GET") {
            Scenario scenario;
            scenario.config = d->simulator->config();
            scenario.globalQuirks = d->simulator->globalQuirks();
            for (const VirtualCamera *cam : d->simulator->cameras()) {
                ScenarioCamera sc;
                sc.model = cam->model();
                sc.quirks = cam->quirks();
                scenario.cameras.append(sc);
            }
            d->sendJson(socket, 200, scenario.toJson());
        } else if (method == "POST") {
            QJsonObject request;
            if (!parseJsonBody(socket, body, &request)) {
                return true;
            }
            QStringList errors;
            // 两种用法：给 path 从磁盘加载，或直接把整份场景 POST 进来。
            // 两条都落到 Simulator::applyScenario()，quirk 的合并语义才不会分叉。
            const QString file = request.value(QStringLiteral("path")).toString();
            bool ok = false;
            if (!file.isEmpty()) {
                ok = d->simulator->loadScenario(file, &errors);
            } else {
                const Scenario scenario = Scenario::fromJson(request, &errors);
                QString startError;
                ok = d->simulator->applyScenario(scenario, &startError);
                if (!ok)
                    errors.append(startError);
            }
            QJsonObject o;
            o.insert(QStringLiteral("ok"), ok);
            o.insert(QStringLiteral("cameras"), d->simulator->cameraCount());
            if (!errors.isEmpty())
                o.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(errors));
            d->sendJson(socket, ok ? 200 : 400, o);
        } else {
            d->sendJson(socket, 405, jsonError(QStringLiteral("方法不支持")));
        }
        return true;
    }

    return false;
}

// /api/cameras 及其子资源。
bool ControlApi::handleCameras(QTcpSocket *socket, const QByteArray &method, const QString &path,
                              const QStringList &segments, const QUrlQuery &query,
                              const QByteArray &body)
{
    // ---- /api/cameras[...] ----
    // 这是最后一个 handler，不归它管的路径就是 404。
    if (segments.value(1) != QLatin1String("cameras")) {
        sendNotFound(socket, path);
        return true;
    }

    const QString cameraId = segments.value(2);
    const QString sub = segments.value(3);

    if (cameraId.isEmpty())
        return handleCameraCollection(socket, method, body);

    VirtualCamera *camera = d->simulator->camera(cameraId);
    if (!camera) {
        sendNotFound(socket, path);
        return true;
    }

    if (sub.isEmpty())
        return handleCameraItem(socket, method, path, camera, body);

    return handleCameraSubResource(socket, method, path, sub, camera, query, body);
}

// GET /api/cameras（列表）与 POST /api/cameras（从预设创建）。
bool ControlApi::handleCameraCollection(QTcpSocket *socket, const QByteArray &method,
                                        const QByteArray &body)
{
    if (method == "GET") {
        QJsonArray array;
        for (const VirtualCamera *cam : d->simulator->cameras()) {
            QJsonObject o = cam->model().toJson();
            const CameraStatus s = cam->status();
            QJsonObject st;
            st.insert(QStringLiteral("running"), s.running);
            st.insert(QStringLiteral("offline"), s.offline);
            st.insert(QStringLiteral("rtspSessions"), s.rtspSessions);
            st.insert(QStringLiteral("subscriptions"), s.subscriptions);
            st.insert(QStringLiteral("httpClients"), s.httpClients);
            st.insert(QStringLiteral("lastEventTopic"), s.lastEventTopic);
            o.insert(QStringLiteral("status"), st);
            o.insert(QStringLiteral("xaddr"), cam->deviceServiceXAddr());
            QJsonArray streams;
            for (const MediaProfile &p : cam->model().profiles)
                streams.append(cam->streamUri(p));
            o.insert(QStringLiteral("streamUris"), streams);
            o.insert(QStringLiteral("quirks"), cam->quirks().toJson());
            array.append(o);
        }
        d->sendJson(socket, 200, array);
    } else if (method == "POST") {
        QJsonObject request;
        if (!parseJsonBody(socket, body, &request)) {
            return true;
        }
        const QString preset = request.value(QStringLiteral("preset"))
                                   .toString(QStringLiteral("generic"));
        VirtualCamera *cam = d->simulator->addCamera(preset);
        if (!cam) {
            d->sendJson(socket, 409, jsonError(QStringLiteral("创建相机失败")));
            return true;
        }
        if (request.value(QStringLiteral("quirks")).isObject()) {
            QStringList errors;
            // 叠加而不是替换：addCamera() 刚把品牌预设和全局 quirk 铺进去，
            // 整体替换会连同它们一起冲掉。想关某条就在请求里显式写 false。
            Quirks merged = cam->quirks();
            merged.merge(Quirks::fromJson(
                request.value(QStringLiteral("quirks")).toObject(), &errors));
            cam->setQuirks(merged);
        }
        QString error;
        if (d->simulator->isRunning() && !cam->start(&error)) {
            d->sendJson(socket, 409, jsonError(error));
            return true;
        }
        QJsonObject o = cam->model().toJson();
        o.insert(QStringLiteral("xaddr"), cam->deviceServiceXAddr());
        d->sendJson(socket, 201, o);
    } else {
        d->sendJson(socket, 405, jsonError(QStringLiteral("方法不支持")));
    }
    return true;
}

// GET / PATCH / DELETE /api/cameras/{id}。
bool ControlApi::handleCameraItem(QTcpSocket *socket, const QByteArray &method,
                                  const QString &path, VirtualCamera *camera,
                                  const QByteArray &body)
{
    if (method == "GET") {
        QJsonObject o = camera->model().toJson();
        o.insert(QStringLiteral("quirks"), camera->quirks().toJson());
        o.insert(QStringLiteral("xaddr"), camera->deviceServiceXAddr());
        d->sendJson(socket, 200, o);
    } else if (method == "PATCH") {
        QJsonObject request;
        if (!parseJsonBody(socket, body, &request)) {
            return true;
        }
        QStringList warnings;
        if (request.value(QStringLiteral("quirks")).isObject()) {
            // PATCH 是部分更新：只覆盖请求里提到的 quirk，其余保持不变。
            // 想整体清空请用 DELETE 重建，或逐条写 false。
            Quirks merged = camera->quirks();
            merged.merge(Quirks::fromJson(
                request.value(QStringLiteral("quirks")).toObject(), &warnings));
            camera->setQuirks(merged);
        }
        if (request.contains(QStringLiteral("preset")))
            camera->setPersona(request.value(QStringLiteral("preset")).toString());
        if (request.contains(QStringLiteral("enabled"))) {
            const bool on = request.value(QStringLiteral("enabled")).toBool();
            if (on) {
                QString error;
                // 启动失败必须让调用方知道：端口被占是最常见的原因，
                // 回 200 的话自动化会以为相机起来了，然后连上去才发现没有。
                if (!camera->start(&error)) {
                    d->sendJson(socket, 409, jsonError(error));
                    return true;
                }
            } else {
                camera->stop();
            }
        }
        // 其余字段按相机模型整体覆盖。
        if (request.value(QStringLiteral("identity")).isObject()
            || request.value(QStringLiteral("network")).isObject()
            || request.value(QStringLiteral("profiles")).isArray()) {
            QJsonObject merged = camera->model().toJson();
            for (auto it = request.constBegin(); it != request.constEnd(); ++it)
                merged.insert(it.key(), it.value());
            camera->setModel(CameraModel::fromJson(merged, &warnings));
        }
        QJsonObject o = camera->model().toJson();
        o.insert(QStringLiteral("quirks"), camera->quirks().toJson());
        if (!warnings.isEmpty())
            o.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(warnings));
        d->sendJson(socket, 200, o);
    } else if (method == "DELETE") {
        // 返回值要看：id 对不上时 removeCamera 返回 false，静默回 200
        // 会让调用方以为删掉了。
        if (!d->simulator->removeCamera(camera->id())) {
            sendNotFound(socket, path);
            return true;
        }
        d->sendJson(socket, 200, QJsonObject{ { QStringLiteral("ok"), true } });
    } else {
        d->sendJson(socket, 405, jsonError(QStringLiteral("方法不支持")));
    }
    return true;
}

// /api/cameras/{id}/{events,ptz,sessions,talkback,offline}。
bool ControlApi::handleCameraSubResource(QTcpSocket *socket, const QByteArray &method,
                                         const QString &path, const QString &sub,
                                         VirtualCamera *camera, const QUrlQuery &query,
                                         const QByteArray &body)
{
    if (sub == QLatin1String("events") && method == "POST") {
        QJsonObject request;
        if (!parseJsonBody(socket, body, &request)) {
            return true;
        }
        const int duration = request.value(QStringLiteral("duration")).toInt(0);
        const QString topic = request.value(QStringLiteral("topic")).toString();
        if (!topic.isEmpty()) {
            camera->events()->trigger(topic, request.value(QStringLiteral("state")).toBool(true),
                                      duration);
        } else {
            bool ok = false;
            const EventKind kind = TopicCatalog::kindFromName(
                request.value(QStringLiteral("kind")).toString(QStringLiteral("Motion")), &ok);
            if (!ok) {
                sendBadRequest(socket, QStringLiteral("未知的事件种类"));
                return true;
            }
            camera->events()->trigger(kind, duration);
        }
        d->sendJson(socket, 200, QJsonObject{ { QStringLiteral("ok"), true } });
        return true;
    }

    if (sub == QLatin1String("ptz")) {
        if (method == "GET") {
            const PtzVector p = camera->ptz()->position();
            QJsonObject o;
            o.insert(QStringLiteral("pan"), p.pan);
            o.insert(QStringLiteral("tilt"), p.tilt);
            o.insert(QStringLiteral("zoom"), p.zoom);
            o.insert(QStringLiteral("panTiltStatus"), ptzStatusName(camera->ptz()->panTiltStatus()));
            o.insert(QStringLiteral("zoomStatus"), ptzStatusName(camera->ptz()->zoomStatus()));
            QJsonArray presets;
            for (const PtzPreset &preset : camera->ptz()->presets()) {
                QJsonObject po;
                po.insert(QStringLiteral("token"), preset.token);
                po.insert(QStringLiteral("name"), preset.name);
                presets.append(po);
            }
            o.insert(QStringLiteral("presets"), presets);
            d->sendJson(socket, 200, o);
        } else if (method == "POST") {
            QJsonObject request;
            if (!parseJsonBody(socket, body, &request)) {
                return true;
            }
            const QString action = request.value(QStringLiteral("action")).toString();
            PtzVector v;
            v.pan = request.value(QStringLiteral("pan")).toDouble(0.0);
            v.tilt = request.value(QStringLiteral("tilt")).toDouble(0.0);
            v.zoom = request.value(QStringLiteral("zoom")).toDouble(0.0);
            v.hasPanTilt = request.contains(QStringLiteral("pan"))
                           || request.contains(QStringLiteral("tilt"));
            v.hasZoom = request.contains(QStringLiteral("zoom"));

            if (action == QLatin1String("continuous")) {
                camera->ptz()->continuousMove(v, request.value(QStringLiteral("timeout")).toInt(0));
            } else if (action == QLatin1String("absolute")) {
                camera->ptz()->absoluteMove(v, PtzVector());
            } else if (action == QLatin1String("relative")) {
                camera->ptz()->relativeMove(v, PtzVector());
            } else if (action == QLatin1String("stop")) {
                camera->ptz()->stop();
            } else if (action == QLatin1String("goto_preset")) {
                camera->ptz()->gotoPreset(request.value(QStringLiteral("token")).toString(),
                                          PtzVector());
            } else if (action == QLatin1String("set_preset")) {
                camera->ptz()->setPreset(request.value(QStringLiteral("name")).toString());
            } else if (action == QLatin1String("generate_factory_presets")) {
                camera->ptz()->generateFactoryPresets(
                    request.value(QStringLiteral("count")).toInt(300));
            } else {
                sendBadRequest(socket, QStringLiteral("未知的 PTZ action：%1").arg(action));
                return true;
            }
            d->sendJson(socket, 200, QJsonObject{ { QStringLiteral("ok"), true } });
        } else {
            d->sendJson(socket, 405, jsonError(QStringLiteral("方法不支持")));
        }
        return true;
    }

    if (sub == QLatin1String("sessions") && method == "GET") {
        QJsonObject o;
        QJsonArray rtsp;
        for (const RtspSession *session : camera->rtspServer()->sessions()) {
            QJsonObject so;
            so.insert(QStringLiteral("id"), QString::fromLatin1(session->sessionId()));
            so.insert(QStringLiteral("peer"), session->peerString());
            so.insert(QStringLiteral("profile"), session->profileToken());
            so.insert(QStringLiteral("playing"), session->isPlaying());
            so.insert(QStringLiteral("backchannel"), session->hasBackchannel());
            so.insert(QStringLiteral("startedAt"), session->startedAt().toString(Qt::ISODate));

            // 发送侧统计。原来一个字都没往外报 —— packetsSent / bytesSent 每个包
            // 都在自增，lossPercentReported / jitter 是整条 RTCP RR 解析链的产出，
            // 全都只写不读。而「客户端报了多少丢包」恰恰是拿这个模拟器压测时
            // 最想看的数字。
            QJsonArray tracks;
            for (const RtspTrack &track : session->tracks()) {
                if (!track.sender)
                    continue;
                const RtpStats stats = track.sender->stats();
                QJsonObject to;
                to.insert(QStringLiteral("kind"),
                          track.kind == RtspTrackKind::Video ? QStringLiteral("video")
                                                             : QStringLiteral("audio"));
                to.insert(QStringLiteral("packetsSent"), stats.packetsSent);
                to.insert(QStringLiteral("bytesSent"), stats.bytesSent);
                to.insert(QStringLiteral("packetsDropped"), stats.packetsDropped);
                to.insert(QStringLiteral("lossPercentReported"), stats.lossPercentReported);
                to.insert(QStringLiteral("jitter"), double(stats.jitter));
                tracks.append(to);
            }
            if (!tracks.isEmpty())
                so.insert(QStringLiteral("tracks"), tracks);

            rtsp.append(so);
        }
        o.insert(QStringLiteral("rtsp"), rtsp);

        QJsonArray subs;
        for (const Subscription *s : camera->subscriptions()->subscriptions()) {
            QJsonObject so;
            so.insert(QStringLiteral("id"), s->id);
            so.insert(QStringLiteral("address"), s->address);
            so.insert(QStringLiteral("queued"), s->queue.size());
            so.insert(QStringLiteral("pulled"), s->pulledCount);
            so.insert(QStringLiteral("dropped"), s->droppedCount);
            so.insert(QStringLiteral("createdAt"), s->createdAt.toString(Qt::ISODate));
            so.insert(QStringLiteral("terminationTime"),
                      s->terminationTime.toString(Qt::ISODate));
            so.insert(QStringLiteral("peer"), s->creatorPeer);
            subs.append(so);
        }
        o.insert(QStringLiteral("subscriptions"), subs);
        o.insert(QStringLiteral("httpClients"), camera->status().httpClients);
        d->sendJson(socket, 200, o);
        return true;
    }

    if (sub == QLatin1String("talkback") && method == "GET") {
        QJsonObject o;
        QJsonArray sessions;
        qint64 totalBytes = 0;
        for (const RtspSession *session : camera->rtspServer()->sessions()) {
            if (!session->hasBackchannel() || !session->backchannelReceiver())
                continue;
            const TalkbackStats stats = session->backchannelReceiver()->stats();
            QJsonObject so;
            so.insert(QStringLiteral("session"), QString::fromLatin1(session->sessionId()));
            so.insert(QStringLiteral("packets"), stats.packetsReceived);
            so.insert(QStringLiteral("bytes"), stats.bytesReceived);
            so.insert(QStringLiteral("dropped"), stats.packetsDropped);
            so.insert(QStringLiteral("markers"), stats.markerCount);
            so.insert(QStringLiteral("markerMissing"), stats.markerMissing);
            so.insert(QStringLiteral("level"), stats.currentLevel);
            so.insert(QStringLiteral("peakLevel"), stats.peakLevel);
            so.insert(QStringLiteral("codec"), codec::audioCodecName(stats.codec));
            totalBytes += stats.bytesReceived;
            sessions.append(so);
        }
        o.insert(QStringLiteral("sessions"), sessions);
        o.insert(QStringLiteral("totalBytes"), totalBytes);
        d->sendJson(socket, 200, o);
        return true;
    }

    if (sub == QLatin1String("offline") && method == "POST") {
        const int seconds = query.queryItemValue(QStringLiteral("seconds")).toInt();
        camera->goOffline(seconds > 0 ? seconds : 10);
        d->sendJson(socket, 200, QJsonObject{ { QStringLiteral("ok"), true } });
        return true;
    }

    sendNotFound(socket, path);
    return true;
}


} // namespace onvifsim
