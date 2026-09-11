#include "vendor/dahua/DahuaCgiStub.h"

#include "core/Quirks.h"
#include "core/VirtualCamera.h"
#include "events/EventEngine.h"
#include "imaging/ImagingState.h"
#include "media/Snapshot.h"
#include "net/HttpServer.h"
#include "ptz/PtzState.h"
#include "vendor/VendorFactories.h"

#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

namespace onvifsim {
namespace {

// attach 长连接的 multipart 分隔串。真机就是这个字面量。
const char *const kAttachBoundary = "myboundary";

// 大华的云台速度是 1..8 档。
constexpr int kPtzSpeedMax = 8;

// text/plain 响应。大华的行分隔是 CRLF，客户端按 splitlines 解析，
// 但有的实现会 strip('\r')，这里照真机用 CRLF。
HttpResponse plainResponse(const QByteArray &body, int status = 200)
{
    HttpResponse r;
    r.status = status;
    r.setBody(body, "text/plain;charset=UTF-8");
    return r;
}

HttpResponse okResponse()
{
    return plainResponse("OK\r\n");
}

HttpResponse errorResponse(const QString &detail)
{
    return plainResponse(QByteArray("Error\r\n") + detail.toUtf8() + "\r\n", 400);
}

void appendLine(QByteArray &out, const QString &key, const QString &value)
{
    out += key.toUtf8();
    out += '=';
    out += value.toUtf8();
    out += "\r\n";
}

void appendLine(QByteArray &out, const QString &key, int value)
{
    appendLine(out, key, QString::number(value));
}

// 大华的编码名带点，ONVIF 侧不带。
QString dahuaCompression(const QString &encoding)
{
    if (encoding.compare(QLatin1String("H264"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("H.264");
    if (encoding.compare(QLatin1String("H265"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("H.265");
    if (encoding.compare(QLatin1String("JPEG"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("MJPG");
    return encoding;
}

// 一档速度 → [0, 1]。1 档最慢、8 档最快，映射成 0.125 .. 1.0。
double speedFromDahua(int step)
{
    return qBound(0.125, double(qBound(1, step, kPtzSpeedMax)) / double(kPtzSpeedMax), 1.0);
}

// 属性型事件的 true/false 决定 Start / Stop；瞬时事件一律 Pulse。
QString actionOf(const EventMessage &message)
{
    if (!message.isProperty)
        return QStringLiteral("Pulse");
    for (auto it = message.dataItems.constBegin(); it != message.dataItems.constEnd(); ++it) {
        const QString v = it.value().trimmed();
        if (v.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0 || v == QLatin1String("0"))
            return QStringLiteral("Stop");
        if (v.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0 || v == QLatin1String("1"))
            return QStringLiteral("Start");
    }
    return QStringLiteral("Start");
}

} // namespace

// 一条挂着的 eventManager attach 连接。
//
// 与海康 alertStream 同一套做法：beginStream() 写完 multipart 响应头之后连接一直开着，
// 事件与心跳都用 writeChunk() 往里推。心跳周期取 URL 上的 heartbeat 参数（真机默认 5 秒），
// 客户端在这个周期内收不到任何一行就会判死，所以这条定时器是协议要求的一部分。
struct DahuaCgiStub::AttachClient
{
    HttpExchangePtr exchange;
    QStringList codes;             // 空 = [All]
    QTimer *heartbeat = nullptr;
};

DahuaCgiStub::DahuaCgiStub(VirtualCamera *camera, QObject *parent)
    : VendorApiStub(camera, parent), m_auth(QStringLiteral("Login to 3E00000"))
{
    if (m_camera && m_camera->events()) {
        connect(m_camera->events(), &EventEngine::eventProduced,
                this, &DahuaCgiStub::onEventProduced);
    }
}

DahuaCgiStub::~DahuaCgiStub()
{
    // 相机被销毁时把还挂着的流收干净，别把 HttpExchange 的强引用留到最后。
    const QVector<QSharedPointer<AttachClient>> clients = m_attachClients;
    for (const QSharedPointer<AttachClient> &client : clients)
        drop(client);
}

VendorApi DahuaCgiStub::kind() const
{
    return VendorApi::DahuaCgi;
}

int DahuaCgiStub::streamingClients() const
{
    return int(m_attachClients.size());
}

const Persona &DahuaCgiStub::personaOrDefault() const
{
    return m_camera ? m_camera->persona() : PersonaRegistry::generic();
}

void DahuaCgiStub::registerRoutes(HttpServer *server)
{
    if (!server || m_routesRegistered)
        return;
    m_routesRegistered = true;

    // eventManager 单独一条：action=attach 要拿住 exchange 挂着。
    server->addRoute(QByteArray(), QStringLiteral("/cgi-bin/eventManager.cgi"),
                     [this](const HttpRequest &request, const HttpExchangePtr &exchange) {
                         if (request.query.queryItemValue(QStringLiteral("action"))
                             == QLatin1String("attach")) {
                             beginAttach(request, exchange);
                             return;
                         }
                         exchange->respond(handle(request));
                     });

    server->addPrefixRoute(QByteArray(), QStringLiteral("/cgi-bin/"),
                           [this](const HttpRequest &request, const HttpExchangePtr &exchange) {
                               exchange->respond(handle(request));
                           });
}

bool DahuaCgiStub::authorize(const HttpRequest &request, HttpResponse *response)
{
    if (!m_camera)
        return true;   // 单测路径：没有用户表就不鉴权

    const QByteArray uri = request.rawTarget.isEmpty() ? request.path.toUtf8()
                                                       : request.rawTarget.toUtf8();
    const AuthResult result = m_auth.verify(request.header("authorization"), request.method,
                                            uri, m_camera->model(), m_camera->quirks());
    if (result.authenticated)
        return true;

    if (response) {
        *response = plainResponse("Error\r\n", 401);
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

HttpResponse DahuaCgiStub::handle(const HttpRequest &request)
{
    HttpResponse challenge;
    if (!authorize(request, &challenge))
        return challenge;

    const QString script = request.path.section(QLatin1Char('/'), -1);
    if (script == QLatin1String("magicBox.cgi"))
        return handleMagicBox(request);
    if (script == QLatin1String("ptz.cgi"))
        return handlePtz(request);
    if (script == QLatin1String("configManager.cgi"))
        return handleConfigManager(request);
    if (script == QLatin1String("coaxialControlIO.cgi"))
        return handleCoaxialControl(request);
    if (script == QLatin1String("snapshot.cgi"))
        return handleSnapshot(request);
    if (script == QLatin1String("eventManager.cgi"))
        return handleEventManager(request);

    return plainResponse("Error\r\n", 404);
}

// ---------------------------------------------------------------------------
// magicBox.cgi —— 厂商识别的探针
// ---------------------------------------------------------------------------

QByteArray DahuaCgiStub::systemInfoText(const CameraModel &model, const Persona &persona)
{
    // 真机的 getSystemInfo 就是这几行；客户端拿 deviceType 做子串匹配认厂商。
    const QString serial = model.serialNumber.isEmpty() ? persona.serialPrefix
                                                        : model.serialNumber;
    QByteArray out;
    appendLine(out, QStringLiteral("deviceType"), model.model);
    appendLine(out, QStringLiteral("hardwareVersion"), QStringLiteral("1.00"));
    appendLine(out, QStringLiteral("processor"), QStringLiteral("SSC335"));
    appendLine(out, QStringLiteral("serialNumber"), serial);
    appendLine(out, QStringLiteral("updateSerial"), model.model);
    appendLine(out, QStringLiteral("updateSerialCloudUpgrade"),
               model.model + QLatin1String(":07:01:04:47:33:00:00:00:01:00:00:00:00"));
    // vendor 不在真机的 getSystemInfo 里，但识别逻辑要拿 manufacturer 做匹配，
    // 多带一条比让客户端只能靠 deviceType 猜稳。
    appendLine(out, QStringLiteral("vendor"), model.manufacturer);
    return out;
}

HttpResponse DahuaCgiStub::handleMagicBox(const HttpRequest &request)
{
    const QString action = request.query.queryItemValue(QStringLiteral("action"));
    const CameraModel model = m_camera ? m_camera->model() : CameraModel();
    const Persona &persona = personaOrDefault();

    if (action == QLatin1String("getSystemInfo"))
        return plainResponse(systemInfoText(model, persona));

    QByteArray out;
    if (action == QLatin1String("getDeviceType")) {
        appendLine(out, QStringLiteral("type"), model.model);
    } else if (action == QLatin1String("getSoftwareVersion")) {
        appendLine(out, QStringLiteral("version"), model.firmwareVersion);
    } else if (action == QLatin1String("getSerialNo")) {
        appendLine(out, QStringLiteral("sn"), model.serialNumber.isEmpty() ? persona.serialPrefix
                                                                          : model.serialNumber);
    } else if (action == QLatin1String("getMachineName")) {
        appendLine(out, QStringLiteral("name"), model.displayName);
    } else if (action == QLatin1String("getVendor")) {
        appendLine(out, QStringLiteral("vendor"), model.manufacturer);
    } else if (action == QLatin1String("getHardwareVersion")) {
        appendLine(out, QStringLiteral("version"), QStringLiteral("1.00"));
    } else {
        return errorResponse(QStringLiteral("unknown action"));
    }
    return plainResponse(out);
}

// ---------------------------------------------------------------------------
// ptz.cgi
// ---------------------------------------------------------------------------

HttpResponse DahuaCgiStub::handlePtz(const HttpRequest &request)
{
    const QString action = request.query.queryItemValue(QStringLiteral("action"));
    PtzState *ptz = m_camera ? m_camera->ptz() : nullptr;

    if (action == QLatin1String("getCurrentProtocolCaps")) {
        const int maxPresets = m_camera ? m_camera->model().ptzNode.maxPresets : 300;
        QByteArray out;
        appendLine(out, QStringLiteral("caps.AlarmNum"), 0);
        appendLine(out, QStringLiteral("caps.AuxNum"), 8);
        appendLine(out, QStringLiteral("caps.Focus"), QStringLiteral("true"));
        appendLine(out, QStringLiteral("caps.Iris"), QStringLiteral("false"));
        appendLine(out, QStringLiteral("caps.Menu"), QStringLiteral("false"));
        appendLine(out, QStringLiteral("caps.MonitorTour"), QStringLiteral("true"));
        appendLine(out, QStringLiteral("caps.Pan"), QStringLiteral("true"));
        appendLine(out, QStringLiteral("caps.PanSpeedMax"), kPtzSpeedMax);
        appendLine(out, QStringLiteral("caps.PanSpeedMin"), 1);
        appendLine(out, QStringLiteral("caps.Preset"), QStringLiteral("true"));
        appendLine(out, QStringLiteral("caps.PresetNum"), maxPresets);
        // 「Tile」是真机的拼写错误（本该是 Tilt），客户端跟着错，这里必须照错。
        appendLine(out, QStringLiteral("caps.Tile"), QStringLiteral("true"));
        appendLine(out, QStringLiteral("caps.TileSpeedMax"), kPtzSpeedMax);
        appendLine(out, QStringLiteral("caps.TileSpeedMin"), 1);
        appendLine(out, QStringLiteral("caps.Zoom"), QStringLiteral("true"));
        appendLine(out, QStringLiteral("caps.ZoomSpeedMax"), kPtzSpeedMax);
        appendLine(out, QStringLiteral("caps.ZoomSpeedMin"), 1);
        return plainResponse(out);
    }

    if (action == QLatin1String("getPresets")) {
        QByteArray out;
        if (ptz) {
            int i = 0;
            const QList<PtzPreset> presets = ptz->presets();
            for (const PtzPreset &preset : presets) {
                bool numeric = false;
                const int id = preset.token.toInt(&numeric);
                appendLine(out, QStringLiteral("presets[%1].Index").arg(i),
                           numeric ? id : i + 1);
                appendLine(out, QStringLiteral("presets[%1].Name").arg(i), preset.name);
                ++i;
            }
        }
        return plainResponse(out);
    }

    if (action == QLatin1String("stop")) {
        if (ptz)
            ptz->stop();
        return okResponse();
    }

    if (action == QLatin1String("start")) {
        const QString code = request.query.queryItemValue(QStringLiteral("code"));
        const int arg1 = request.query.queryItemValue(QStringLiteral("arg1")).toInt();
        const int arg2 = request.query.queryItemValue(QStringLiteral("arg2")).toInt();
        if (!ptz)
            return okResponse();

        if (code == QLatin1String("GotoPreset")) {
            PtzVector speed;
            speed.pan = speed.tilt = speed.zoom = speedFromDahua(qMax(arg1, 1));
            // 大华的预置位号放在 arg2。
            if (!ptz->gotoPreset(QString::number(arg2), speed))
                return errorResponse(QStringLiteral("no such preset"));
            return okResponse();
        }
        if (code == QLatin1String("SetPreset")) {
            QString error;
            ptz->setPreset(QStringLiteral("Preset%1").arg(arg2), QString::number(arg2), &error);
            return error.isEmpty() ? okResponse() : errorResponse(error);
        }
        if (code == QLatin1String("ClearPreset")) {
            ptz->removePreset(QString::number(arg2));
            return okResponse();
        }

        // 方向码。arg1 是垂直档位、arg2 是水平档位，斜向两个都给，
        // 单向的那个另一维传 0 —— 所以取两者较大的当统一速度。
        const double s = speedFromDahua(qMax(qMax(arg1, arg2), 1));
        PtzVector v;
        v.hasPanTilt = true;
        v.hasZoom = false;
        if (code == QLatin1String("Left")) {
            v.pan = -s;
        } else if (code == QLatin1String("Right")) {
            v.pan = s;
        } else if (code == QLatin1String("Up")) {
            v.tilt = s;
        } else if (code == QLatin1String("Down")) {
            v.tilt = -s;
        } else if (code == QLatin1String("LeftUp")) {
            v.pan = -s; v.tilt = s;
        } else if (code == QLatin1String("RightUp")) {
            v.pan = s;  v.tilt = s;
        } else if (code == QLatin1String("LeftDown")) {
            v.pan = -s; v.tilt = -s;
        } else if (code == QLatin1String("RightDown")) {
            v.pan = s;  v.tilt = -s;
        } else if (code == QLatin1String("ZoomTele")) {
            v.hasPanTilt = false; v.hasZoom = true; v.zoom = s;
        } else if (code == QLatin1String("ZoomWide")) {
            v.hasPanTilt = false; v.hasZoom = true; v.zoom = -s;
        } else {
            return errorResponse(QStringLiteral("unsupported code"));
        }
        ptz->continuousMove(v);
        return okResponse();
    }

    return errorResponse(QStringLiteral("unknown action"));
}

// ---------------------------------------------------------------------------
// configManager.cgi —— Encode 与 Lighting_V2
// ---------------------------------------------------------------------------

HttpResponse DahuaCgiStub::handleConfigManager(const HttpRequest &request)
{
    const QString action = request.query.queryItemValue(QStringLiteral("action"));
    ImagingState *imaging = m_camera ? m_camera->imaging() : nullptr;

    if (action == QLatin1String("getConfig")) {
        const QString name = request.query.queryItemValue(QStringLiteral("name"));

        if (name == QLatin1String("Encode")) {
            QByteArray out;
            const QList<MediaProfile> profiles = m_camera ? m_camera->model().profiles
                                                          : QList<MediaProfile>();
            // 主码流是 MainFormat[0]，子码流与三码流是 ExtraFormat[0] / [1]。
            for (int i = 0; i < profiles.size(); ++i) {
                const MediaProfile &profile = profiles.at(i);
                const VideoEncoderConfig &video = profile.videoEncoder;
                const QString base = i == 0
                    ? QStringLiteral("table.Encode[0].MainFormat[0]")
                    : QStringLiteral("table.Encode[0].ExtraFormat[%1]").arg(i - 1);
                appendLine(out, base + QLatin1String(".AudioEnable"),
                           profile.hasAudio ? QStringLiteral("true") : QStringLiteral("false"));
                appendLine(out, base + QLatin1String(".VideoEnable"), QStringLiteral("true"));
                appendLine(out, base + QLatin1String(".Video.Compression"),
                           dahuaCompression(video.encoding));
                appendLine(out, base + QLatin1String(".Video.Width"), video.width);
                appendLine(out, base + QLatin1String(".Video.Height"), video.height);
                appendLine(out, base + QLatin1String(".Video.FPS"), qRound(video.frameRate));
                appendLine(out, base + QLatin1String(".Video.BitRate"), video.bitrateKbps);
                appendLine(out, base + QLatin1String(".Video.BitRateControl"),
                           QStringLiteral("VBR"));
                appendLine(out, base + QLatin1String(".Video.GOP"), video.govLength);
                appendLine(out, base + QLatin1String(".Video.Profile"), video.h264Profile);
                appendLine(out, base + QLatin1String(".Video.Quality"), video.quality);
            }
            return plainResponse(out);
        }

        if (name == QLatin1String("Lighting_V2")) {
            const bool on = imaging && imaging->settings().whiteLightOn;
            const int brightness = imaging ? imaging->settings().whiteLightBrightness : 100;
            QByteArray out;
            const QString base = QStringLiteral("table.Lighting_V2[0][0][0]");
            appendLine(out, base + QLatin1String(".Correction"), 50);
            appendLine(out, base + QLatin1String(".LightType"), QStringLiteral("WhiteLight"));
            // Mode=Off 表示灯灭；Manual + Light>0 表示常亮。
            appendLine(out, base + QLatin1String(".Mode"),
                       on ? QStringLiteral("Manual") : QStringLiteral("Off"));
            appendLine(out, base + QLatin1String(".MiddleLight[0].Light"), on ? brightness : 0);
            appendLine(out, base + QLatin1String(".Sensitive"), 3);
            return plainResponse(out);
        }

        return errorResponse(QStringLiteral("unknown config name"));
    }

    if (action == QLatin1String("setConfig")) {
        // setConfig 的参数直接铺在 query 上：Lighting_V2[0][0][0].Mode=Manual&…Light=100
        bool touched = false;
        bool on = imaging && imaging->settings().whiteLightOn;
        int brightness = imaging ? imaging->settings().whiteLightBrightness : 100;

        const QList<QPair<QString, QString>> items = request.query.queryItems();
        for (const QPair<QString, QString> &item : items) {
            if (!item.first.startsWith(QLatin1String("Lighting_V2")))
                continue;
            if (item.first.endsWith(QLatin1String(".Mode"))) {
                on = item.second.compare(QLatin1String("Off"), Qt::CaseInsensitive) != 0;
                touched = true;
            } else if (item.first.endsWith(QLatin1String(".Light"))) {
                // 查 ok：这里的 0 不是中性值而是「关灯」。`Light=abc` 静默变成 0
                // 的话，客户端得到的是一次成功的关灯操作，而不是参数错误。
                bool ok = false;
                const int parsed = item.second.toInt(&ok);
                if (ok) {
                    brightness = parsed;
                    // 亮度调到 0 等价于关灯，真机也是这么表现的。
                    if (brightness <= 0)
                        on = false;
                    touched = true;
                }
            }
        }
        if (touched && imaging)
            imaging->setWhiteLight(on, on ? qMax(1, brightness) : brightness);
        return okResponse();
    }

    return errorResponse(QStringLiteral("unknown action"));
}

// ---------------------------------------------------------------------------
// coaxialControlIO.cgi —— 白光灯与警笛
// ---------------------------------------------------------------------------

HttpResponse DahuaCgiStub::handleCoaxialControl(const HttpRequest &request)
{
    const QString action = request.query.queryItemValue(QStringLiteral("action"));

    if (action == QLatin1String("getCollectAttribute")) {
        QByteArray out;
        appendLine(out, QStringLiteral("status.WhiteLight"), QStringLiteral("true"));
        appendLine(out, QStringLiteral("status.Speaker"), QStringLiteral("true"));
        return plainResponse(out);
    }

    if (action != QLatin1String("control"))
        return errorResponse(QStringLiteral("unknown action"));

    // info[0].Type=WhiteLight&info[0].IO=1 —— Type 决定控什么，IO 是 1 开 / 2 关。
    QString type;
    int io = 0;
    const QList<QPair<QString, QString>> items = request.query.queryItems();
    for (const QPair<QString, QString> &item : items) {
        if (item.first.endsWith(QLatin1String(".Type")))
            type = item.second;
        else if (item.first.endsWith(QLatin1String(".IO"))) {
            bool ok = false;
            const int parsed = item.second.toInt(&ok);
            if (ok)
                io = parsed;
        }
    }
    if (type.isEmpty())
        return errorResponse(QStringLiteral("missing info[0].Type"));

    const bool on = (io == 1);
    if (type.compare(QLatin1String("WhiteLight"), Qt::CaseInsensitive) == 0) {
        if (m_camera && m_camera->imaging())
            m_camera->imaging()->setWhiteLight(on);
    } else if (type.compare(QLatin1String("Speaker"), Qt::CaseInsensitive) == 0) {
        // 警笛没有独立状态位，用一次继电器输出事件代表它响过，
        // 这样 ONVIF 订阅方也能看到同一次动作。
        if (on && m_camera && m_camera->events())
            m_camera->events()->trigger(EventKind::RelayOutput, 2000);
    } else {
        return errorResponse(QStringLiteral("unsupported type"));
    }
    return okResponse();
}

// ---------------------------------------------------------------------------
// snapshot.cgi
// ---------------------------------------------------------------------------

HttpResponse DahuaCgiStub::handleSnapshot(const HttpRequest &request)
{
    // 大华的 channel 从 1 起，subtype 才是主 / 子码流的选择。
    const int subtype = request.query.queryItemValue(QStringLiteral("subtype")).toInt();
    QString token;
    if (m_camera) {
        const QList<MediaProfile> &profiles = m_camera->model().profiles;
        if (!profiles.isEmpty())
            token = profiles.at(qBound(0, subtype, int(profiles.size()) - 1)).token;
    }
    return HttpResponse::jpeg(snapshot::renderFor(m_camera, token));
}

// ---------------------------------------------------------------------------
// eventManager.cgi
// ---------------------------------------------------------------------------

QString DahuaCgiStub::eventCodeFor(EventKind kind)
{
    switch (kind) {
    case EventKind::Motion:
    case EventKind::MotionAlarm:
        return QStringLiteral("VideoMotion");
    case EventKind::LineCrossing:
        return QStringLiteral("CrossLineDetection");
    case EventKind::FieldIntrusion:
        return QStringLiteral("CrossRegionDetection");
    case EventKind::PeopleDetect:
        return QStringLiteral("SmartMotionHuman");
    case EventKind::VehicleDetect:
        return QStringLiteral("SmartMotionVehicle");
    case EventKind::FaceDetect:
        return QStringLiteral("FaceDetection");
    case EventKind::Tamper:
    case EventKind::SceneChange:
    case EventKind::ImageTooDark:
        // 遮挡 / 场景突变 / 画面过暗在大华那边都归到 VideoBlind。
        return QStringLiteral("VideoBlind");
    case EventKind::AnimalDetect:
        // 大华没有独立的动物码，AI 结果统一挂 SmartMotionHuman 的通道，
        // 具体类别放在 data 里的 Object.ObjectType。
        return QStringLiteral("SmartMotionHuman");
    case EventKind::AudioDetected:
    case EventKind::DigitalInput:
    case EventKind::RelayOutput:
    case EventKind::ProcessorUsage:
    case EventKind::Count:
        break;
    }
    return QStringLiteral("VideoMotion");
}

QStringList DahuaCgiStub::parseCodes(const QString &value)
{
    // 先百分号解码：真机的 URL 是 codes=[All]，但客户端普遍会把方括号编码成
    // %5B / %5D，而 QUrlQuery 的 PrettyDecoded 又不会替我们还原它们。
    // 不解码就会把整串当成一个码名，表现是「订阅成功但一条事件都收不到」。
    QString inner = QUrl::fromPercentEncoding(value.toUtf8()).trimmed();
    if (inner.startsWith(QLatin1Char('[')) && inner.endsWith(QLatin1Char(']')))
        inner = inner.mid(1, inner.size() - 2);
    QStringList codes;
    const QStringList raw = inner.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &one : raw) {
        const QString trimmed = one.trimmed();
        if (trimmed.isEmpty())
            continue;
        // [All] 表示全收，用空表表示，省得下游再比一次字符串。
        if (trimmed.compare(QLatin1String("All"), Qt::CaseInsensitive) == 0)
            return QStringList();
        codes.append(trimmed);
    }
    return codes;
}

QByteArray DahuaCgiStub::eventLine(const QString &code, const QString &action, int index,
                                   const QByteArray &dataJson)
{
    QByteArray line = "Code=" + code.toUtf8()
        + ";action=" + action.toUtf8()
        + ";index=" + QByteArray::number(index);
    if (!dataJson.isEmpty())
        line += ";data=" + dataJson;
    return line;
}

QByteArray DahuaCgiStub::multipartPart(const QByteArray &line)
{
    QByteArray part = "--" + QByteArray(kAttachBoundary) + "\r\n";
    part += "Content-Type: text/plain\r\n";
    part += "Content-Length: " + QByteArray::number(line.size()) + "\r\n\r\n";
    part += line;
    part += "\r\n\r\n";
    return part;
}

HttpResponse DahuaCgiStub::handleEventManager(const HttpRequest &request)
{
    const QString action = request.query.queryItemValue(QStringLiteral("action"));
    if (action == QLatin1String("getCaps")) {
        QByteArray out;
        appendLine(out, QStringLiteral("caps.PageSupport"), QStringLiteral("true"));
        appendLine(out, QStringLiteral("caps.Support"),
                   QStringLiteral("VideoMotion,CrossLineDetection,CrossRegionDetection,"
                                  "SmartMotionHuman,SmartMotionVehicle,FaceDetection,"
                                  "TrafficJunction,VideoBlind"));
        return plainResponse(out);
    }
    if (action == QLatin1String("getEventIndexes")) {
        QByteArray out;
        appendLine(out, QStringLiteral("channels[0]"), 0);
        return plainResponse(out);
    }
    return errorResponse(QStringLiteral("unknown action"));
}

void DahuaCgiStub::beginAttach(const HttpRequest &request, const HttpExchangePtr &exchange)
{
    HttpResponse challenge;
    if (!authorize(request, &challenge)) {
        exchange->respond(challenge);
        return;
    }

    // 只写头。真机用的是 x-mixed-replace，不是 mixed；流式响应不补 Content-Length。
    HttpResponse head;
    head.status = 200;
    head.setHeader("Content-Type",
                   "multipart/x-mixed-replace;boundary=" + QByteArray(kAttachBoundary));
    exchange->beginStream(head);

    QSharedPointer<AttachClient> client(new AttachClient);
    client->exchange = exchange;
    client->codes = parseCodes(request.query.queryItemValue(QStringLiteral("codes")));

    // heartbeat 的单位是秒，真机允许 1..60；客户端默认传 5。
    int heartbeat = request.query.queryItemValue(QStringLiteral("heartbeat")).toInt();
    if (heartbeat <= 0)
        heartbeat = 5;
    heartbeat = qBound(1, heartbeat, 60);

    client->heartbeat = new QTimer(this);
    client->heartbeat->setInterval(heartbeat * 1000);
    connect(client->heartbeat, &QTimer::timeout, this,
            [this, client] { push(client, multipartPart("Heartbeat")); });
    client->heartbeat->start();

    connect(exchange.data(), &HttpExchange::aborted, this, [this, client] { drop(client); });

    m_attachClients.append(client);
}

// 推一段。对端已经走了就顺手清理，返回值告诉调用方这条连接还在不在。
bool DahuaCgiStub::push(const QSharedPointer<AttachClient> &client, const QByteArray &chunk)
{
    if (!client || !client->exchange)
        return false;
    if (client->exchange->writeChunk(chunk))
        return true;
    drop(client);
    return false;
}

void DahuaCgiStub::onEventProduced(const EventMessage &message)
{
    if (m_attachClients.isEmpty())
        return;

    EventKind kind = EventKind::Motion;
    if (m_camera && m_camera->events()) {
        const QVector<TopicDef> topics = m_camera->events()->topics();
        for (const TopicDef &def : topics) {
            if (def.topic == message.topic) {
                kind = def.kind;
                break;
            }
        }
    }

    const QString code = eventCodeFor(kind);
    QByteArray data;
    if (kind == EventKind::PeopleDetect || kind == EventKind::VehicleDetect
        || kind == EventKind::AnimalDetect || kind == EventKind::FaceDetect) {
        const char *objectType = kind == EventKind::VehicleDetect ? "Vehicle"
            : kind == EventKind::AnimalDetect                     ? "Animal"
            : kind == EventKind::FaceDetect                       ? "Face"
                                                                  : "Human";
        data = QByteArray("{\"Object\":{\"ObjectType\":\"") + objectType + "\"}}";
    }
    const QByteArray part = multipartPart(eventLine(code, actionOf(message), 0, data));

    // 复制一份再遍历：push() 里的 drop() 会从 m_attachClients 摘元素。
    const QVector<QSharedPointer<AttachClient>> clients = m_attachClients;
    for (const QSharedPointer<AttachClient> &client : clients) {
        // codes 为空表示 [All]。
        if (!client->codes.isEmpty() && !client->codes.contains(code, Qt::CaseInsensitive))
            continue;
        push(client, part);
    }
}

void DahuaCgiStub::drop(const QSharedPointer<AttachClient> &client)
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
    m_attachClients.removeAll(client);
}

namespace vendorapi {

VendorApiStub *createDahuaCgi(VirtualCamera *camera)
{
    return new DahuaCgiStub(camera, camera);
}

} // namespace vendorapi
} // namespace onvifsim
