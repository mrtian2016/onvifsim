#include "vendor/hikvision/IsapiStub.h"

#include "core/Quirks.h"
#include "core/VirtualCamera.h"
#include "events/EventEngine.h"
#include "imaging/ImagingState.h"
#include "media/Snapshot.h"
#include "net/HttpServer.h"
#include "vendor/VendorFactories.h"

#include <QtCore/QTimer>
#include <QtCore/QXmlStreamReader>

namespace onvifsim {
namespace {

// ISAPI 的默认命名空间。客户端一般不校验，但真机确实带着它，
// 而且有的适配器会拿 xmlns 当「这确实是海康」的旁证，所以照抄。
const char *const kIsapiNs = "http://www.hikvision.com/ver20/XMLSchema";

// alertStream 的 multipart 分隔串。真机就是这个字面量。
const char *const kAlertBoundary = "boundary";

// 事件长连接的挂起上限：没有事件时到点也要回一包心跳，
// 免得客户端的读超时先于我们动手。
constexpr int kAlertHeartbeatMs = 5000;

QString xmlEscape(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        switch (c.unicode()) {
        case u'&':  out += QLatin1String("&amp;");  break;
        case u'<':  out += QLatin1String("&lt;");   break;
        case u'>':  out += QLatin1String("&gt;");   break;
        case u'"':  out += QLatin1String("&quot;"); break;
        case u'\'': out += QLatin1String("&apos;"); break;
        default:    out += c;                       break;
        }
    }
    return out;
}

QString tag(const QString &name, const QString &value)
{
    return QLatin1Char('<') + name + QLatin1Char('>') + xmlEscape(value)
        + QLatin1String("</") + name + QLatin1Char('>');
}

QString tagNum(const QString &name, qint64 value)
{
    return tag(name, QString::number(value));
}

QByteArray xmlDocument(const QString &body)
{
    return QByteArray("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n") + body.toUtf8();
}

HttpResponse xmlResponse(const QByteArray &xml, int status = 200)
{
    HttpResponse r;
    r.status = status;
    // 真机吐的是 application/xml；有的客户端拿 content-type 当「是不是 ISAPI」的判据。
    r.setBody(xml, "application/xml; charset=\"UTF-8\"");
    return r;
}

// 路径按 '/' 切开并丢掉空段。ISAPI 的路径层级固定，按下标取比正则好读。
QStringList splitPath(const QString &path)
{
    return path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

// 从 <Root><name>value</name></Root> 里取第一个 name 的文本。
// ISAPI 的请求体都很浅，用 QXmlStreamReader 顺一遍即可，不必建树。
QString firstElementText(const QByteArray &xml, const QString &name)
{
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        if (reader.readNext() == QXmlStreamReader::StartElement
            && reader.name().toString().compare(name, Qt::CaseInsensitive) == 0) {
            return reader.readElementText();
        }
    }
    return QString();
}

// 海康的 IrcutFilterType 与 ONVIF 的 IrCutFilter 是反的直觉：
// day = 滤光片切进来（白天彩色）= ONVIF 的 ON，night = 切走（夜视黑白）= OFF。
IrCutFilterMode ircutFromIsapi(const QString &value, bool *ok)
{
    if (ok)
        *ok = true;
    if (value.compare(QLatin1String("day"), Qt::CaseInsensitive) == 0)
        return IrCutFilterMode::On;
    if (value.compare(QLatin1String("night"), Qt::CaseInsensitive) == 0)
        return IrCutFilterMode::Off;
    if (value.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0
        || value.compare(QLatin1String("schedule"), Qt::CaseInsensitive) == 0)
        return IrCutFilterMode::Auto;
    if (ok)
        *ok = false;
    return IrCutFilterMode::Auto;
}

QString ircutToIsapi(IrCutFilterMode mode)
{
    switch (mode) {
    case IrCutFilterMode::On:   return QStringLiteral("day");
    case IrCutFilterMode::Off:  return QStringLiteral("night");
    case IrCutFilterMode::Auto: break;
    }
    return QStringLiteral("auto");
}

// EventKind → 海康 eventType / targetType。映射方向与
// reference-client-facts.md §7.2 里客户端那张反向表对齐，
// 保证「桩发什么，客户端就能还原成什么语义」。
struct HikEventName {
    QString eventType;
    QString targetType;
    bool hasPlate = false;
};

HikEventName hikNameFor(EventKind kind)
{
    HikEventName n;
    switch (kind) {
    case EventKind::Motion:
    case EventKind::MotionAlarm:
        n.eventType = QStringLiteral("VMD");
        break;
    case EventKind::LineCrossing:
        n.eventType = QStringLiteral("linedetection");
        n.targetType = QStringLiteral("human");
        break;
    case EventKind::FieldIntrusion:
        n.eventType = QStringLiteral("fielddetection");
        n.targetType = QStringLiteral("human");
        break;
    case EventKind::Tamper:
        n.eventType = QStringLiteral("tamperdetection");
        break;
    case EventKind::SceneChange:
        n.eventType = QStringLiteral("scenechangedetection");
        break;
    case EventKind::AudioDetected:
        n.eventType = QStringLiteral("audioexception");
        break;
    case EventKind::ImageTooDark:
        n.eventType = QStringLiteral("videoloss");
        break;
    case EventKind::DigitalInput:
        n.eventType = QStringLiteral("IO");
        break;
    case EventKind::RelayOutput:
        n.eventType = QStringLiteral("IO");
        break;
    case EventKind::ProcessorUsage:
        n.eventType = QStringLiteral("diskfull");
        break;
    case EventKind::PeopleDetect:
        n.eventType = QStringLiteral("fielddetection");
        n.targetType = QStringLiteral("human");
        break;
    case EventKind::VehicleDetect:
        // 车辆走 ANPR：客户端就是靠这条拿车牌的。
        n.eventType = QStringLiteral("vehicledetection");
        n.targetType = QStringLiteral("vehicle");
        n.hasPlate = true;
        break;
    case EventKind::AnimalDetect:
        n.eventType = QStringLiteral("fielddetection");
        n.targetType = QStringLiteral("animal");
        break;
    case EventKind::FaceDetect:
        n.eventType = QStringLiteral("facedetection");
        n.targetType = QStringLiteral("human");
        break;
    case EventKind::Count:
        break;
    }
    if (n.eventType.isEmpty())
        n.eventType = QStringLiteral("VMD");
    return n;
}

// 属性型事件的 true/false 在 dataItems 里；瞬时事件没有状态，一律 active。
QString alertStateOf(const EventMessage &message)
{
    if (!message.isProperty)
        return QStringLiteral("active");
    for (auto it = message.dataItems.constBegin(); it != message.dataItems.constEnd(); ++it) {
        const QString v = it.value().trimmed();
        if (v.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0
            || v == QLatin1String("0"))
            return QStringLiteral("inactive");
        if (v.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0
            || v == QLatin1String("1"))
            return QStringLiteral("active");
    }
    return QStringLiteral("active");
}

} // namespace

// 一条挂着的 alertStream 连接。
//
// 走 HttpExchange 的流式接口：beginStream() 写完 multipart 的响应头之后，
// 连接就一直开着（net 层不再计 keep-alive 空闲超时），每条事件用 writeChunk()
// 推一段。真机上这条连接能开几个小时，客户端不必反复重连。
// writeChunk() 返回 false 就是对端走了 / 读不动被踹了，据此清理。
struct IsapiStub::AlertClient
{
    HttpExchangePtr exchange;
    QTimer *heartbeat = nullptr;   // 没有事件时也要周期性吐一条，客户端靠它判活
};

IsapiStub::IsapiStub(VirtualCamera *camera, QObject *parent)
    : VendorApiStub(camera, parent), m_auth(QStringLiteral("DS-2CD2143G0-I"))
{
    // 事件引擎与 ONVIF 的订阅管理器共用同一条 eventProduced 信号：
    // 从 GUI / REST 触发一次，ONVIF 订阅方和 ISAPI alertStream 都能收到。
    if (m_camera && m_camera->events()) {
        connect(m_camera->events(), &EventEngine::eventProduced,
                this, &IsapiStub::onEventProduced);
    }
}

IsapiStub::~IsapiStub()
{
    // 相机被销毁时把还挂着的流收干净，别把 HttpExchange 的强引用留到最后。
    const QVector<QSharedPointer<AlertClient>> clients = m_alertClients;
    for (const QSharedPointer<AlertClient> &client : clients)
        drop(client);
}

VendorApi IsapiStub::kind() const
{
    return VendorApi::Isapi;
}

int IsapiStub::streamingClients() const
{
    return int(m_alertClients.size());
}

const Persona &IsapiStub::personaOrDefault() const
{
    return m_camera ? m_camera->persona() : PersonaRegistry::generic();
}

void IsapiStub::registerRoutes(HttpServer *server)
{
    if (!server || m_routesRegistered)
        return;   // VirtualCamera::start() 每次都会调，重启一台相机不能把路由挂两遍
    m_routesRegistered = true;

    // alertStream 单独挂精确路由：它要拿住 exchange，走不了统一的同步分支。
    server->addRoute("GET", QStringLiteral("/ISAPI/Event/notification/alertStream"),
                     [this](const HttpRequest &request, const HttpExchangePtr &exchange) {
                         beginAlertStream(request, exchange);
                     });

    // 其余全部走一条前缀路由：ISAPI 的路径里带通道号与预置位号，
    // 逐条注册反而要拼一堆字符串，不如在 handle() 里按段分派。
    server->addPrefixRoute(QByteArray(), QStringLiteral("/ISAPI/"),
                           [this](const HttpRequest &request, const HttpExchangePtr &exchange) {
                               exchange->respond(handle(request));
                           });
}

bool IsapiStub::authorize(const HttpRequest &request, HttpResponse *response)
{
    // 单测里 camera 为空（没有用户表也没有 quirks），这时不做鉴权，
    // 让端点本身的响应体成为唯一被验的东西。
    if (!m_camera)
        return true;

    const QByteArray uri = request.rawTarget.isEmpty()
        ? request.path.toUtf8()
        : request.rawTarget.toUtf8();
    const AuthResult result = m_auth.verify(request.header("authorization"), request.method,
                                            uri, m_camera->model(), m_camera->quirks());
    if (result.authenticated)
        return true;

    if (response) {
        *response = xmlResponse(xmlDocument(QStringLiteral("<ResponseStatus version=\"2.0\" "
                                                           "xmlns=\"%1\">%2%3%4</ResponseStatus>")
                                                .arg(QLatin1String(kIsapiNs),
                                                     tag(QStringLiteral("requestURL"), request.path),
                                                     tagNum(QStringLiteral("statusCode"), 4),
                                                     tag(QStringLiteral("statusString"),
                                                         QStringLiteral("Invalid Operation")))),
                                401);
        // challenges() 可能返回两条（E8 双挑战），HttpServer 按 '\n' 拆成多行头。
        const QList<QByteArray> challenges = m_auth.challenges(m_camera->quirks(), result.stale);
        QByteArray joined;
        for (const QByteArray &c : challenges) {
            if (!joined.isEmpty())
                joined += '\n';
            joined += c;
        }
        response->setHeader("WWW-Authenticate", joined);
    }
    return false;
}

// ---------------------------------------------------------------------------
// 分派
// ---------------------------------------------------------------------------

HttpResponse IsapiStub::handle(const HttpRequest &request)
{
    HttpResponse challenge;
    if (!authorize(request, &challenge))
        return challenge;

    const QStringList seg = splitPath(request.path);
    // seg[0] == "ISAPI"
    if (seg.size() < 2)
        return xmlResponse(responseStatusXml(request.path, 4, QStringLiteral("Invalid Operation")),
                           400);

    const QString &group = seg.at(1);
    if (group == QLatin1String("System")) {
        if (seg.size() == 3 && seg.at(2) == QLatin1String("deviceInfo"))
            return handleDeviceInfo(request);
        if (seg.size() >= 3 && seg.at(2) == QLatin1String("IO"))
            return handleIo(request, seg);
    } else if (group == QLatin1String("PTZCtrl")) {
        return handlePtz(request, seg);
    } else if (group == QLatin1String("Streaming")) {
        return handleStreaming(request, seg);
    } else if (group == QLatin1String("Image")) {
        return handleImage(request, seg);
    }

    return xmlResponse(responseStatusXml(request.path, 4, QStringLiteral("Invalid Operation"),
                                         QStringLiteral("invalidOperation")),
                       404);
}

// ---------------------------------------------------------------------------
// /ISAPI/System/deviceInfo —— 厂商识别就靠这一条
// ---------------------------------------------------------------------------

QByteArray IsapiStub::deviceInfoXml(const CameraModel &model, const Persona &persona)
{
    // 固件版本在真机上是 "V5.6.3 build 190923" 这种拼串，
    // 而 ISAPI 把它拆成 firmwareVersion + firmwareReleasedDate 两个字段。
    QString version = model.firmwareVersion;
    QString released;
    const int buildAt = version.indexOf(QLatin1String("build"), 0, Qt::CaseInsensitive);
    if (buildAt > 0) {
        released = version.mid(buildAt + 5).trimmed();
        version = version.left(buildAt).trimmed();
    }

    QString body = QStringLiteral("<DeviceInfo version=\"2.0\" xmlns=\"%1\">")
                       .arg(QLatin1String(kIsapiNs));
    body += tag(QStringLiteral("deviceName"), model.displayName);
    body += tagNum(QStringLiteral("deviceID"), 88);
    body += tag(QStringLiteral("deviceDescription"), QStringLiteral("IPCamera"));
    body += tag(QStringLiteral("deviceLocation"), model.location);
    body += tag(QStringLiteral("systemContact"), QStringLiteral("Hikvision"));
    body += tag(QStringLiteral("model"), model.model);
    body += tag(QStringLiteral("serialNumber"), model.serialNumber.isEmpty()
                                                    ? persona.serialPrefix
                                                    : model.serialNumber);
    body += tag(QStringLiteral("macAddress"), model.macAddress.isEmpty()
                                                  ? QStringLiteral("00:11:22:33:44:55")
                                                  : model.macAddress);
    body += tag(QStringLiteral("firmwareVersion"), version);
    body += tag(QStringLiteral("firmwareReleasedDate"),
                released.isEmpty() ? QStringLiteral("build 000000") : released);
    body += tag(QStringLiteral("encoderVersion"), QStringLiteral("V7.3"));
    body += tag(QStringLiteral("encoderReleasedDate"), QStringLiteral("build 190910"));
    body += tag(QStringLiteral("bootVersion"), QStringLiteral("V1.3.4"));
    body += tag(QStringLiteral("bootReleasedDate"), QStringLiteral("100316"));
    body += tag(QStringLiteral("hardwareVersion"), QStringLiteral("0x0"));
    body += tag(QStringLiteral("deviceType"), QStringLiteral("IPCamera"));
    body += tag(QStringLiteral("telecontrolID"), model.hardwareId);
    body += tag(QStringLiteral("supportBeep"), QStringLiteral("false"));
    body += tag(QStringLiteral("supportVideoLoss"), QStringLiteral("false"));
    // manufacturer 不是 ISAPI 的标准字段，但识别逻辑要拿它做子串匹配，
    // 多带一条比让客户端猜安全。
    body += tag(QStringLiteral("manufacturer"), model.manufacturer);
    body += QLatin1String("</DeviceInfo>");
    return xmlDocument(body);
}

HttpResponse IsapiStub::handleDeviceInfo(const HttpRequest &request)
{
    if (request.method.toUpper() != "GET")
        return xmlResponse(responseStatusXml(request.path, 4, QStringLiteral("Invalid Operation")),
                           405);
    const CameraModel model = m_camera ? m_camera->model() : CameraModel();
    return xmlResponse(deviceInfoXml(model, personaOrDefault()));
}

QByteArray IsapiStub::responseStatusXml(const QString &requestUrl, int statusCode,
                                        const QString &statusString, const QString &subStatus)
{
    QString body = QStringLiteral("<ResponseStatus version=\"2.0\" xmlns=\"%1\">")
                       .arg(QLatin1String(kIsapiNs));
    body += tag(QStringLiteral("requestURL"), requestUrl);
    body += tagNum(QStringLiteral("statusCode"), statusCode);
    body += tag(QStringLiteral("statusString"), statusString);
    body += tag(QStringLiteral("subStatusCode"),
                subStatus.isEmpty() ? QStringLiteral("ok") : subStatus);
    body += QLatin1String("</ResponseStatus>");
    return xmlDocument(body);
}

// ---------------------------------------------------------------------------
// PTZ：-100..100 ↔ [-1, 1]
// ---------------------------------------------------------------------------

double IsapiStub::speedFromIsapi(int value)
{
    return qBound(-1.0, double(qBound(-100, value, 100)) / 100.0, 1.0);
}

int IsapiStub::speedToIsapi(double value)
{
    // qRound 而不是截断：往返换算要能原样回来（50 → 0.5 → 50）。
    return qBound(-100, qRound(qBound(-1.0, value, 1.0) * 100.0), 100);
}

PtzVector IsapiStub::parsePtzData(const QByteArray &xml)
{
    PtzVector v;
    v.hasPanTilt = false;
    v.hasZoom = false;

    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        if (reader.readNext() != QXmlStreamReader::StartElement)
            continue;
        const QString name = reader.name().toString();
        const bool isPan = name.compare(QLatin1String("pan"), Qt::CaseInsensitive) == 0;
        const bool isTilt = name.compare(QLatin1String("tilt"), Qt::CaseInsensitive) == 0;
        const bool isZoom = name.compare(QLatin1String("zoom"), Qt::CaseInsensitive) == 0;
        // <Momentary> 里也是同一组 pan/tilt/zoom，读到哪个算哪个。
        // readElementText() 会把当前元素整棵子树读完，所以只能在确认是叶子
        // 元素之后再调 —— 在外面统一调的话，<PTZData> / <Momentary> 会被整个
        // 吞掉，里面的 pan/tilt/zoom 一个都读不到。
        if (!isPan && !isTilt && !isZoom)
            continue;
        // 查 ok：速度全零在下游会被判成「停」，解析失败静默归零等于把一条
        // 写错的移动指令变成一次成功的停止指令。
        bool ok = false;
        const int raw = reader.readElementText().toInt(&ok);
        if (!ok)
            continue;
        if (isPan) {
            v.pan = speedFromIsapi(raw);
            v.hasPanTilt = true;
        } else if (isTilt) {
            v.tilt = speedFromIsapi(raw);
            v.hasPanTilt = true;
        } else {
            v.zoom = speedFromIsapi(raw);
            v.hasZoom = true;
        }
    }
    return v;
}

QByteArray IsapiStub::buildPtzData(const PtzVector &velocity)
{
    QString body = QStringLiteral("<PTZData version=\"2.0\" xmlns=\"%1\">")
                       .arg(QLatin1String(kIsapiNs));
    if (velocity.hasPanTilt) {
        body += tagNum(QStringLiteral("pan"), speedToIsapi(velocity.pan));
        body += tagNum(QStringLiteral("tilt"), speedToIsapi(velocity.tilt));
    }
    if (velocity.hasZoom)
        body += tagNum(QStringLiteral("zoom"), speedToIsapi(velocity.zoom));
    body += QLatin1String("</PTZData>");
    return xmlDocument(body);
}

HttpResponse IsapiStub::handlePtz(const HttpRequest &request, const QStringList &seg)
{
    // /ISAPI/PTZCtrl/channels/{ch}/...
    if (seg.size() < 4 || seg.at(2) != QLatin1String("channels"))
        return xmlResponse(responseStatusXml(request.path, 4, QStringLiteral("Invalid Operation")),
                           400);

    PtzState *ptz = m_camera ? m_camera->ptz() : nullptr;
    const QString what = seg.size() > 4 ? seg.at(4) : QString();
    const QByteArray method = request.method.toUpper();

    if (what == QLatin1String("capabilities")) {
        const int maxPresets = m_camera ? m_camera->model().ptzNode.maxPresets : 300;
        QString body = QStringLiteral("<PTZChanelCap version=\"2.0\" xmlns=\"%1\">")
                           .arg(QLatin1String(kIsapiNs));
        body += QLatin1String("<AbsolutePTZ><elevation min=\"-900\" max=\"2700\"/>"
                              "<azimuth min=\"0\" max=\"3600\"/>"
                              "<absoluteZoom min=\"10\" max=\"400\"/></AbsolutePTZ>");
        body += QLatin1String("<RelativePTZ><positionX min=\"-100\" max=\"100\"/>"
                              "<positionY min=\"-100\" max=\"100\"/>"
                              "<relativeZoom min=\"-100\" max=\"100\"/></RelativePTZ>");
        // 这三行是本端点的重点：客户端读到的速度域就是 -100..100。
        body += QLatin1String("<ContinuousPTZ><pan min=\"-100\" max=\"100\"/>"
                              "<tilt min=\"-100\" max=\"100\"/>"
                              "<zoom min=\"-100\" max=\"100\"/></ContinuousPTZ>");
        body += tagNum(QStringLiteral("maxPresetNum"), maxPresets);
        body += tagNum(QStringLiteral("maxPatrolNum"), 8);
        body += tagNum(QStringLiteral("maxPatternNum"), 4);
        body += tag(QStringLiteral("isSupportPresetName"), QStringLiteral("true"));
        body += QLatin1String("</PTZChanelCap>");
        return xmlResponse(xmlDocument(body));
    }

    if (what == QLatin1String("continuous")) {
        if (method != "PUT" && method != "POST") {
            return xmlResponse(responseStatusXml(request.path, 4,
                                                 QStringLiteral("Invalid Operation")), 405);
        }
        const PtzVector velocity = parsePtzData(request.body);
        if (ptz) {
            // 全零等价于停：真机也是靠这条让云台刹车的。
            const bool zero = (!velocity.hasPanTilt || (qFuzzyIsNull(velocity.pan)
                                                        && qFuzzyIsNull(velocity.tilt)))
                && (!velocity.hasZoom || qFuzzyIsNull(velocity.zoom));
            if (zero)
                ptz->stop(velocity.hasPanTilt, velocity.hasZoom);
            else
                ptz->continuousMove(velocity);
        }
        return xmlResponse(responseStatusXml(request.path, 1, QStringLiteral("OK")));
    }

    if (what == QLatin1String("presets")) {
        // GET  .../presets              列表
        // PUT  .../presets/{n}          写入
        // PUT  .../presets/{n}/goto     调用
        if (seg.size() == 5) {
            if (method != "GET") {
                return xmlResponse(responseStatusXml(request.path, 4,
                                                     QStringLiteral("Invalid Operation")), 405);
            }
            QString body = QStringLiteral("<PTZPresetList version=\"2.0\" xmlns=\"%1\">")
                               .arg(QLatin1String(kIsapiNs));
            if (ptz) {
                int index = 1;
                const QList<PtzPreset> presets = ptz->presets();
                for (const PtzPreset &preset : presets) {
                    bool numeric = false;
                    const int id = preset.token.toInt(&numeric);
                    body += QLatin1String("<PTZPreset version=\"2.0\">");
                    body += tag(QStringLiteral("enabled"), QStringLiteral("true"));
                    body += tagNum(QStringLiteral("id"), numeric ? id : index);
                    body += tag(QStringLiteral("presetName"), preset.name);
                    body += QLatin1String("</PTZPreset>");
                    ++index;
                }
            }
            body += QLatin1String("</PTZPresetList>");
            return xmlResponse(xmlDocument(body));
        }

        const QString token = seg.at(5);
        const bool isGoto = seg.size() > 6 && seg.at(6) == QLatin1String("goto");
        if (method != "PUT" && method != "POST") {
            return xmlResponse(responseStatusXml(request.path, 4,
                                                 QStringLiteral("Invalid Operation")), 405);
        }
        if (!ptz)
            return xmlResponse(responseStatusXml(request.path, 1, QStringLiteral("OK")));

        if (isGoto) {
            PtzVector speed;
            speed.pan = speed.tilt = speed.zoom = 1.0;
            if (!ptz->gotoPreset(token, speed)) {
                return xmlResponse(responseStatusXml(request.path, 4,
                                                     QStringLiteral("Invalid Operation"),
                                                     QStringLiteral("notSupport")), 400);
            }
            return xmlResponse(responseStatusXml(request.path, 1, QStringLiteral("OK")));
        }

        // 写入：body 里带 <presetName>，没带就用海康默认的 "Preset{n}"。
        QString name = firstElementText(request.body, QStringLiteral("presetName"));
        if (name.isEmpty())
            name = QStringLiteral("Preset%1").arg(token);
        QString error;
        if (ptz->setPreset(name, token, &error).isEmpty()) {
            return xmlResponse(responseStatusXml(request.path, 4, QStringLiteral("Invalid Operation"),
                                                 error.isEmpty() ? QStringLiteral("notSupport")
                                                                 : error),
                               400);
        }
        return xmlResponse(responseStatusXml(request.path, 1, QStringLiteral("OK")));
    }

    return xmlResponse(responseStatusXml(request.path, 4, QStringLiteral("Invalid Operation")), 404);
}

// ---------------------------------------------------------------------------
// Streaming：通道信息与快照
// ---------------------------------------------------------------------------

const MediaProfile *IsapiStub::profileForChannel(int channelId) const
{
    if (!m_camera || m_camera->model().profiles.isEmpty())
        return nullptr;
    // 101 / 102 / 103 → 主 / 子 / 三码流。取个位当序号，越界回落到主码流。
    const int stream = channelId % 10;
    const QList<MediaProfile> &profiles = m_camera->model().profiles;
    const int index = qBound(0, stream - 1, int(profiles.size()) - 1);
    return &profiles.at(index);
}

HttpResponse IsapiStub::handleStreaming(const HttpRequest &request, const QStringList &seg)
{
    // /ISAPI/Streaming/channels/{ch}01[/picture]
    if (seg.size() < 4 || seg.at(2) != QLatin1String("channels"))
        return xmlResponse(responseStatusXml(request.path, 4, QStringLiteral("Invalid Operation")),
                           400);

    const int channelId = seg.at(3).toInt();
    const MediaProfile *profile = profileForChannel(channelId);

    if (seg.size() > 4 && seg.at(4) == QLatin1String("picture")) {
        // 快照与 ONVIF 侧走同一个渲染器，两边拿到的图是同一台相机的同一帧风格。
        const QString token = profile ? profile->token : QString();
        return HttpResponse::jpeg(snapshot::renderFor(m_camera, token));
    }

    if (seg.size() != 4)
        return xmlResponse(responseStatusXml(request.path, 4, QStringLiteral("Invalid Operation")),
                           404);

    VideoEncoderConfig video;
    AudioEncoderConfig audio;
    bool hasAudio = true;
    QString channelName = QStringLiteral("Camera 01");
    if (profile) {
        video = profile->videoEncoder;
        audio = profile->audioEncoder;
        hasAudio = profile->hasAudio;
        channelName = profile->name;
    }

    // 海康的编码名带点（H.264 / H.265），ONVIF 侧不带，转换在这里做。
    QString codec = video.encoding;
    if (codec.compare(QLatin1String("H264"), Qt::CaseInsensitive) == 0)
        codec = QStringLiteral("H.264");
    else if (codec.compare(QLatin1String("H265"), Qt::CaseInsensitive) == 0)
        codec = QStringLiteral("H.265");
    else if (codec.compare(QLatin1String("JPEG"), Qt::CaseInsensitive) == 0)
        codec = QStringLiteral("MJPEG");

    QString audioCodec = QStringLiteral("G.711ulaw");
    if (audio.encoding.compare(QLatin1String("G726"), Qt::CaseInsensitive) == 0)
        audioCodec = QStringLiteral("G.726");
    else if (audio.encoding.compare(QLatin1String("AAC"), Qt::CaseInsensitive) == 0)
        audioCodec = QStringLiteral("AAC");

    QString body = QStringLiteral("<StreamingChannel version=\"2.0\" xmlns=\"%1\">")
                       .arg(QLatin1String(kIsapiNs));
    body += tagNum(QStringLiteral("id"), channelId);
    body += tag(QStringLiteral("channelName"), channelName);
    body += tag(QStringLiteral("enabled"), QStringLiteral("true"));
    body += QLatin1String("<Transport>");
    body += tagNum(QStringLiteral("rtspPortNo"), m_camera ? m_camera->advertisedRtspPort() : 554);
    body += tagNum(QStringLiteral("maxPacketSize"), 1000);
    body += QLatin1String("<ControlProtocolList><ControlProtocol>"
                          "<streamingTransport>RTSP</streamingTransport>"
                          "</ControlProtocol></ControlProtocolList>");
    body += QLatin1String("</Transport>");
    body += QLatin1String("<Video>");
    body += tag(QStringLiteral("enabled"), QStringLiteral("true"));
    body += tagNum(QStringLiteral("videoInputChannelID"), qMax(1, channelId / 100));
    body += tag(QStringLiteral("videoCodecType"), codec);
    body += tag(QStringLiteral("videoScanType"), QStringLiteral("progressive"));
    body += tagNum(QStringLiteral("videoResolutionWidth"), video.width);
    body += tagNum(QStringLiteral("videoResolutionHeight"), video.height);
    body += tag(QStringLiteral("videoQualityControlType"), QStringLiteral("VBR"));
    body += tagNum(QStringLiteral("constantBitRate"), video.bitrateKbps);
    body += tagNum(QStringLiteral("vbrUpperCap"), video.bitrateKbps);
    // ISAPI 的 maxFrameRate 是「帧率 × 100」，写 1500 表示 15 fps。
    body += tagNum(QStringLiteral("maxFrameRate"), qRound(video.frameRate * 100.0));
    body += tagNum(QStringLiteral("GovLength"), video.govLength);
    body += tag(QStringLiteral("H264Profile"), video.h264Profile);
    body += QLatin1String("</Video>");
    body += QLatin1String("<Audio>");
    body += tag(QStringLiteral("enabled"), hasAudio ? QStringLiteral("true")
                                                    : QStringLiteral("false"));
    body += tag(QStringLiteral("audioCompressionType"), audioCodec);
    body += QLatin1String("</Audio>");
    body += QLatin1String("</StreamingChannel>");
    return xmlResponse(xmlDocument(body));
}

// ---------------------------------------------------------------------------
// Image：补光灯与 IR-cut —— 改的都是 ImagingState
// ---------------------------------------------------------------------------

HttpResponse IsapiStub::handleImage(const HttpRequest &request, const QStringList &seg)
{
    // /ISAPI/Image/channels/{ch}/supplementLight[/capabilities] | ircutFilter
    if (seg.size() < 5 || seg.at(2) != QLatin1String("channels"))
        return xmlResponse(responseStatusXml(request.path, 4, QStringLiteral("Invalid Operation")),
                           400);

    ImagingState *imaging = m_camera ? m_camera->imaging() : nullptr;
    const QString what = seg.at(4);
    const QByteArray method = request.method.toUpper();
    const bool writing = (method == "PUT" || method == "POST");

    if (what == QLatin1String("supplementLight")) {
        if (seg.size() > 5 && seg.at(5) == QLatin1String("capabilities")) {
            QString body = QStringLiteral("<SupplementLight version=\"2.0\" xmlns=\"%1\">")
                               .arg(QLatin1String(kIsapiNs));
            body += QLatin1String("<supplementLightMode opt=\"irLight,colorVuWhiteLight,"
                                  "mixedLight,eventIntelligence,close\">close</supplementLightMode>");
            body += QLatin1String("<mixedLightBrightnessRegulatMode opt=\"auto,manual\">"
                                  "manual</mixedLightBrightnessRegulatMode>");
            body += QLatin1String("<whiteLightBrightness min=\"0\" max=\"100\">"
                                  "100</whiteLightBrightness>");
            body += QLatin1String("<irLightBrightness min=\"0\" max=\"100\">"
                                  "100</irLightBrightness>");
            body += QLatin1String("</SupplementLight>");
            return xmlResponse(xmlDocument(body));
        }

        if (writing) {
            const QString mode = firstElementText(request.body,
                                                  QStringLiteral("supplementLightMode"));
            const QString brightness = firstElementText(request.body,
                                                        QStringLiteral("whiteLightBrightness"));
            if (mode.isEmpty()) {
                return xmlResponse(responseStatusXml(request.path, 6,
                                                     QStringLiteral("Invalid Content"),
                                                     QStringLiteral("badXmlContent")), 400);
            }
            // colorVuWhiteLight / mixedLight / eventIntelligence 都算白光亮着；
            // irLight 与 close 算灭。红外补光不在 ImagingState 里单独建模。
            const bool on = mode.compare(QLatin1String("irLight"), Qt::CaseInsensitive) != 0
                && mode.compare(QLatin1String("close"), Qt::CaseInsensitive) != 0;
            if (imaging) {
                // 查 ok：亮度这一路的 0 意味着关灯，解析失败当成「没给」（-1）
                // 才是对的，静默归零等于替客户端做了它没要求的事。
                bool brightnessOk = false;
                const int parsed = brightness.toInt(&brightnessOk);
                imaging->setWhiteLight(on, brightnessOk ? parsed : -1);
            }
            return xmlResponse(responseStatusXml(request.path, 1, QStringLiteral("OK")));
        }

        const bool on = imaging && imaging->settings().whiteLightOn;
        const int brightness = imaging ? imaging->settings().whiteLightBrightness : 100;
        QString body = QStringLiteral("<SupplementLight version=\"2.0\" xmlns=\"%1\">")
                           .arg(QLatin1String(kIsapiNs));
        body += tag(QStringLiteral("supplementLightMode"),
                    on ? QStringLiteral("colorVuWhiteLight") : QStringLiteral("close"));
        body += tag(QStringLiteral("mixedLightBrightnessRegulatMode"), QStringLiteral("manual"));
        body += tagNum(QStringLiteral("whiteLightBrightness"), brightness);
        body += tagNum(QStringLiteral("irLightBrightness"), 100);
        body += QLatin1String("</SupplementLight>");
        return xmlResponse(xmlDocument(body));
    }

    if (what == QLatin1String("ircutFilter")) {
        if (writing) {
            const QString type = firstElementText(request.body,
                                                  QStringLiteral("IrcutFilterType"));
            bool ok = false;
            const IrCutFilterMode mode = ircutFromIsapi(type, &ok);
            if (!ok) {
                return xmlResponse(responseStatusXml(request.path, 6,
                                                     QStringLiteral("Invalid Content"),
                                                     QStringLiteral("badXmlContent")), 400);
            }
            if (imaging)
                imaging->setIrCutFilter(mode);
            return xmlResponse(responseStatusXml(request.path, 1, QStringLiteral("OK")));
        }

        const IrCutFilterMode mode = imaging ? imaging->settings().irCutFilter
                                             : IrCutFilterMode::Auto;
        QString body = QStringLiteral("<IrcutFilter version=\"2.0\" xmlns=\"%1\">")
                           .arg(QLatin1String(kIsapiNs));
        body += tag(QStringLiteral("IrcutFilterType"), ircutToIsapi(mode));
        body += tagNum(QStringLiteral("nightToDayFilterLevel"), 4);
        body += tagNum(QStringLiteral("nightToDayFilterTime"), 5);
        body += QLatin1String("</IrcutFilter>");
        return xmlResponse(xmlDocument(body));
    }

    return xmlResponse(responseStatusXml(request.path, 4, QStringLiteral("Invalid Operation")), 404);
}

// ---------------------------------------------------------------------------
// System/IO：报警输出
// ---------------------------------------------------------------------------

HttpResponse IsapiStub::handleIo(const HttpRequest &request, const QStringList &seg)
{
    // /ISAPI/System/IO/outputs[/{id}/trigger]
    if (seg.size() < 4 || seg.at(3) != QLatin1String("outputs"))
        return xmlResponse(responseStatusXml(request.path, 4, QStringLiteral("Invalid Operation")),
                           404);

    if (seg.size() == 4) {
        QString body = QStringLiteral("<IOOutputPortList version=\"2.0\" xmlns=\"%1\">")
                           .arg(QLatin1String(kIsapiNs));
        body += QLatin1String("<IOOutputPort version=\"2.0\">");
        body += tagNum(QStringLiteral("id"), 1);
        body += tag(QStringLiteral("powerOnState"), QStringLiteral("low"));
        body += tag(QStringLiteral("outputState"), QStringLiteral("low"));
        body += QLatin1String("</IOOutputPort></IOOutputPortList>");
        return xmlResponse(xmlDocument(body));
    }

    if (seg.size() >= 6 && seg.at(5) == QLatin1String("trigger")) {
        if (request.method.toUpper() != "PUT" && request.method.toUpper() != "POST") {
            return xmlResponse(responseStatusXml(request.path, 4,
                                                 QStringLiteral("Invalid Operation")), 405);
        }
        // 触发报警输出：走事件引擎，这样 ONVIF 订阅方也能看见同一次动作。
        const QString state = firstElementText(request.body, QStringLiteral("outputState"));
        const bool high = state.compare(QLatin1String("high"), Qt::CaseInsensitive) == 0;
        if (m_camera && m_camera->events())
            m_camera->events()->trigger(EventKind::RelayOutput, high ? 2000 : 0);
        return xmlResponse(responseStatusXml(request.path, 1, QStringLiteral("OK")));
    }

    return xmlResponse(responseStatusXml(request.path, 4, QStringLiteral("Invalid Operation")), 404);
}

// ---------------------------------------------------------------------------
// alertStream
// ---------------------------------------------------------------------------

QByteArray IsapiStub::eventAlertXml(const QString &ipAddress, int channel,
                                    const QDateTime &utcTime, const QString &eventType,
                                    const QString &eventState, const QString &targetType,
                                    const QString &plate)
{
    QString body = QStringLiteral("<EventNotificationAlert version=\"2.0\" xmlns=\"%1\">")
                       .arg(QLatin1String(kIsapiNs));
    body += tag(QStringLiteral("ipAddress"), ipAddress);
    body += tagNum(QStringLiteral("portNo"), 80);
    body += tag(QStringLiteral("protocol"), QStringLiteral("HTTP"));
    body += tag(QStringLiteral("macAddress"), QStringLiteral("00:11:22:33:44:55"));
    body += tagNum(QStringLiteral("channelID"), channel);
    body += tag(QStringLiteral("dateTime"), utcTime.toUTC().toString(Qt::ISODate));
    body += tagNum(QStringLiteral("activePostCount"), 1);
    body += tag(QStringLiteral("eventType"), eventType);
    body += tag(QStringLiteral("eventState"), eventState);
    body += tag(QStringLiteral("eventDescription"),
                eventType + QLatin1String(" alarm"));
    if (!targetType.isEmpty()) {
        // 客户端读的是这一层的 targetType；DetectionRegionList 里再重复一次，
        // 是为了贴住真机报文的形状（不同固件放的位置不一样）。
        body += tag(QStringLiteral("targetType"), targetType);
        body += QLatin1String("<DetectionRegionList><DetectionRegionEntry>");
        body += tagNum(QStringLiteral("regionID"), 1);
        body += tagNum(QStringLiteral("sensitivityLevel"), 4);
        body += tag(QStringLiteral("detectionTarget"), targetType);
        body += QLatin1String("</DetectionRegionEntry></DetectionRegionList>");
    }
    if (!plate.isEmpty()) {
        body += QLatin1String("<ANPR>");
        body += tag(QStringLiteral("licensePlate"), plate);
        body += tag(QStringLiteral("country"), QStringLiteral("CN"));
        body += QLatin1String("</ANPR>");
    }
    body += QLatin1String("</EventNotificationAlert>");
    return xmlDocument(body);
}

QByteArray IsapiStub::multipartPart(const QByteArray &xml)
{
    QByteArray part = "--" + QByteArray(kAlertBoundary) + "\r\n";
    part += "Content-Type: application/xml; charset=\"UTF-8\"\r\n";
    part += "Content-Length: " + QByteArray::number(xml.size()) + "\r\n\r\n";
    part += xml;
    part += "\r\n\r\n";
    return part;
}

void IsapiStub::beginAlertStream(const HttpRequest &request, const HttpExchangePtr &exchange)
{
    HttpResponse challenge;
    if (!authorize(request, &challenge)) {
        exchange->respond(challenge);
        return;
    }

    // 只写头，body 留空 —— 流式响应不补 Content-Length，补了客户端会照它截断。
    HttpResponse head;
    head.status = 200;
    head.setHeader("Content-Type", "multipart/mixed; boundary=" + QByteArray(kAlertBoundary));
    exchange->beginStream(head);

    QSharedPointer<AlertClient> client(new AlertClient);
    client->exchange = exchange;

    // 心跳：真机在没有事件时也会周期性吐一条 videoloss/inactive。
    // 顺带当探活用 —— 对端悄悄走掉时 writeChunk() 会返回 false。
    client->heartbeat = new QTimer(this);
    client->heartbeat->setInterval(kAlertHeartbeatMs);
    connect(client->heartbeat, &QTimer::timeout, this, [this, client] {
        const QString ip = m_camera ? m_camera->advertisedHost() : QStringLiteral("127.0.0.1");
        const QDateTime now = m_camera ? m_camera->deviceTimeUtc()
                                       : QDateTime::currentDateTimeUtc();
        push(client, multipartPart(eventAlertXml(ip, 1, now, QStringLiteral("videoloss"),
                                                 QStringLiteral("inactive"), QString(),
                                                 QString())));
    });
    client->heartbeat->start();

    // 客户端提前断开时把它摘掉，别让心跳定时器空转。
    connect(exchange.data(), &HttpExchange::aborted, this, [this, client] { drop(client); });

    m_alertClients.append(client);
}

// 推一段。对端已经走了就顺手清理，返回值告诉调用方这条连接还在不在。
bool IsapiStub::push(const QSharedPointer<AlertClient> &client, const QByteArray &chunk)
{
    if (!client || !client->exchange)
        return false;
    if (client->exchange->writeChunk(chunk))
        return true;
    drop(client);
    return false;
}

void IsapiStub::onEventProduced(const EventMessage &message)
{
    if (m_alertClients.isEmpty())
        return;

    // topic → EventKind：先查当前命名风格下的 topic 表，查不到再退回按尾段猜。
    EventKind kind = EventKind::Motion;
    bool found = false;
    if (m_camera && m_camera->events()) {
        const QVector<TopicDef> topics = m_camera->events()->topics();
        for (const TopicDef &def : topics) {
            if (def.topic == message.topic) {
                kind = def.kind;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        const QString leaf = message.topic.section(QLatin1Char('/'), -1);
        if (leaf.contains(QLatin1String("Line"), Qt::CaseInsensitive))
            kind = EventKind::LineCrossing;
        else if (leaf.contains(QLatin1String("Face"), Qt::CaseInsensitive))
            kind = EventKind::FaceDetect;
        else if (leaf.contains(QLatin1String("Vehicle"), Qt::CaseInsensitive))
            kind = EventKind::VehicleDetect;
        else if (leaf.contains(QLatin1String("People"), Qt::CaseInsensitive))
            kind = EventKind::PeopleDetect;
        else if (leaf.contains(QLatin1String("Tamper"), Qt::CaseInsensitive))
            kind = EventKind::Tamper;
    }

    const HikEventName name = hikNameFor(kind);
    const QString ip = m_camera ? m_camera->advertisedHost() : QStringLiteral("127.0.0.1");
    const QByteArray xml = eventAlertXml(ip, 1, message.utcTime, name.eventType,
                                         alertStateOf(message), name.targetType,
                                         name.hasPlate ? QStringLiteral("京A·12345") : QString());
    const QByteArray part = multipartPart(xml);

    // 复制一份再遍历：push() 里的 drop() 会从 m_alertClients 摘元素。
    const QVector<QSharedPointer<AlertClient>> clients = m_alertClients;
    for (const QSharedPointer<AlertClient> &client : clients)
        push(client, part);
}

void IsapiStub::drop(const QSharedPointer<AlertClient> &client)
{
    if (!client)
        return;
    if (client->heartbeat) {
        client->heartbeat->stop();
        client->heartbeat->deleteLater();
        client->heartbeat = nullptr;
    }
    if (client->exchange) {
        // 先断信号再收尾：endStream() 会让 net 层关连接，
        // 那条路径上还会回调 aborted()，不断开就会递归进来第二次。
        disconnect(client->exchange.data(), nullptr, this, nullptr);
        if (client->exchange->isStreaming())
            client->exchange->endStream();
        client->exchange.clear();
    }
    m_alertClients.removeAll(client);
}

namespace vendorapi {

VendorApiStub *createIsapi(VirtualCamera *camera)
{
    return new IsapiStub(camera, camera);
}

} // namespace vendorapi
} // namespace onvifsim
