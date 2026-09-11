#include "vendor/reolink/ReolinkStub.h"

#include "core/CameraModel.h"
#include "core/Persona.h"
#include "core/VirtualCamera.h"
#include "events/EventEngine.h"
#include "imaging/ImagingState.h"
#include "media/Snapshot.h"
#include "net/HttpServer.h"
#include "ptz/PtzState.h"
#include "vendor/VendorFactories.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QRandomGenerator>

#include <utility>

namespace onvifsim {
namespace {

// token 租期。真机给 3600 秒，客户端按 leaseTime 提前续；
// 我们照给同一个值，让客户端的续期逻辑走到。
constexpr int kTokenLeaseSeconds = 3600;

// Reolink 的云台速度域是 1..64。
constexpr int kPtzSpeedMax = 64;

// 客户端认「token 过期」的唯一判据。
constexpr int kTokenExpiredCode = -6;

double speedFromReolink(int value)
{
    if (value <= 0)
        return 1.0;   // 没给速度就全速，真机也是这个默认
    return qBound(0.05, double(qBound(1, value, kPtzSpeedMax)) / double(kPtzSpeedMax), 1.0);
}

QString reolinkVideoType(const QString &encoding)
{
    if (encoding.compare(QLatin1String("H265"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("h265");
    if (encoding.compare(QLatin1String("JPEG"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("mjpeg");
    return QStringLiteral("h264");
}

// Reolink 的分辨率是 "2560*1920" 这种星号串，不是 WxH。
QString reolinkSize(const VideoEncoderConfig &video)
{
    return QString::number(video.width) + QLatin1Char('*') + QString::number(video.height);
}

QJsonObject encStream(const VideoEncoderConfig &video)
{
    QJsonObject o;
    o.insert(QStringLiteral("bitRate"), video.bitrateKbps);
    o.insert(QStringLiteral("frameRate"), qRound(video.frameRate));
    o.insert(QStringLiteral("gop"), video.govLength);
    o.insert(QStringLiteral("profile"), video.h264Profile);
    o.insert(QStringLiteral("size"), reolinkSize(video));
    o.insert(QStringLiteral("vType"), reolinkVideoType(video.encoding));
    return o;
}

QJsonObject ability(int permit, int ver)
{
    QJsonObject o;
    o.insert(QStringLiteral("permit"), permit);
    o.insert(QStringLiteral("ver"), ver);
    return o;
}

} // namespace

ReolinkStub::ReolinkStub(VirtualCamera *camera, QObject *parent)
    : VendorApiStub(camera, parent)
{
}

ReolinkStub::~ReolinkStub() = default;

VendorApi ReolinkStub::kind() const
{
    return VendorApi::ReolinkJson;
}

int ReolinkStub::tokenExpiredCode()
{
    return kTokenExpiredCode;
}

QString ReolinkStub::currentToken() const
{
    return m_token;
}

void ReolinkStub::expireToken()
{
    // 单测与故障注入都要能立刻把 token 打成过期，好验客户端的重登流程。
    m_tokenExpiry = QDateTime::currentDateTimeUtc().addSecs(-1);
}

void ReolinkStub::registerRoutes(HttpServer *server)
{
    if (!server || m_routesRegistered)
        return;
    m_routesRegistered = true;

    const auto route = [this](const HttpRequest &request, const HttpExchangePtr &exchange) {
        exchange->respond(handle(request));
    };
    // 真机两条路径都通：/api.cgi 是主入口，/cgi-bin/api.cgi 是快照 URL 里用的那条。
    server->addRoute(QByteArray(), QStringLiteral("/api.cgi"), route);
    server->addRoute(QByteArray(), QStringLiteral("/cgi-bin/api.cgi"), route);
}

// ---------------------------------------------------------------------------
// 报文封装
// ---------------------------------------------------------------------------

QByteArray ReolinkStub::encodeResponse(const QJsonArray &items)
{
    return QJsonDocument(items).toJson(QJsonDocument::Compact);
}

QJsonObject ReolinkStub::successItem(const QString &cmd, const QJsonObject &value)
{
    QJsonObject o;
    o.insert(QStringLiteral("cmd"), cmd);
    o.insert(QStringLiteral("code"), 0);
    o.insert(QStringLiteral("value"), value);
    return o;
}

QJsonObject ReolinkStub::rspCodeItem(const QString &cmd, int rspCode)
{
    // 写类命令的成功回执：value.rspCode = 200。
    QJsonObject value;
    value.insert(QStringLiteral("rspCode"), rspCode);
    return successItem(cmd, value);
}

QJsonObject ReolinkStub::errorItem(const QString &cmd, int rspCode, const QString &detail)
{
    QJsonObject error;
    error.insert(QStringLiteral("detail"), detail);
    error.insert(QStringLiteral("rspCode"), rspCode);
    QJsonObject o;
    o.insert(QStringLiteral("cmd"), cmd);
    // 两个位置都写同一个码：客户端有的读顶层 code，有的读 error.rspCode，
    // reference-client-facts.md §7.2 记的就是「两者之一等于 -6 即视为过期」。
    o.insert(QStringLiteral("code"), rspCode);
    o.insert(QStringLiteral("error"), error);
    return o;
}

QString ReolinkStub::issueToken()
{
    const QByteArray seed = QByteArray::number(QDateTime::currentMSecsSinceEpoch())
        + ':' + QByteArray::number(QRandomGenerator::global()->generate64(), 16);
    m_token = QString::fromLatin1(
        QCryptographicHash::hash(seed, QCryptographicHash::Md5).toHex().left(16));
    m_tokenExpiry = QDateTime::currentDateTimeUtc().addSecs(kTokenLeaseSeconds);
    return m_token;
}

bool ReolinkStub::tokenAccepted(const QString &token) const
{
    if (m_token.isEmpty() || token.isEmpty())
        return false;
    if (token != m_token)
        return false;
    return m_tokenExpiry.isValid() && QDateTime::currentDateTimeUtc() < m_tokenExpiry;
}

// ---------------------------------------------------------------------------
// 分派
// ---------------------------------------------------------------------------

HttpResponse ReolinkStub::handle(const HttpRequest &request)
{
    const QString queryCmd = request.query.queryItemValue(QStringLiteral("cmd"));
    const QString token = request.query.queryItemValue(QStringLiteral("token"));

    // 请求体是命令数组；GET 取快照时没有体，命令全在 query 上。
    QJsonArray commands;
    if (!request.body.isEmpty()) {
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(request.body, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            QJsonArray items;
            items.append(errorItem(queryCmd, -1, QStringLiteral("bad json")));
            return HttpResponse::json(encodeResponse(items), 400);
        }
        if (doc.isArray())
            commands = doc.array();
        else if (doc.isObject())
            commands.append(doc.object());
    }
    if (commands.isEmpty()) {
        QJsonObject only;
        only.insert(QStringLiteral("cmd"), queryCmd);
        commands.append(only);
    }

    // Snap 走单独一条：它回的是 JPEG 二进制，不是 JSON 数组。
    const QJsonObject first = commands.at(0).toObject();
    const QString firstCmd = first.value(QStringLiteral("cmd")).toString(queryCmd);
    if (firstCmd == QLatin1String("Snap")) {
        if (!tokenAccepted(token)) {
            QJsonArray items;
            items.append(errorItem(firstCmd, kTokenExpiredCode,
                                   QStringLiteral("please login first")));
            return HttpResponse::json(encodeResponse(items), 200);
        }
        QString profileToken;
        if (m_camera && !m_camera->model().profiles.isEmpty())
            profileToken = m_camera->model().profiles.first().token;
        return HttpResponse::jpeg(snapshot::renderFor(m_camera, profileToken));
    }

    QJsonArray results;
    for (const QJsonValue &value : std::as_const(commands)) {
        const QJsonObject item = value.toObject();
        const QString cmd = item.value(QStringLiteral("cmd")).toString(queryCmd);
        if (cmd.isEmpty()) {
            results.append(errorItem(cmd, -1, QStringLiteral("missing cmd")));
            continue;
        }
        if (cmd == QLatin1String("Login")) {
            results.append(cmdLogin(item));
            continue;
        }
        // 除 Login 外一律验 token；不认就回 -6，客户端据此重登再重试一次。
        if (!tokenAccepted(token)) {
            results.append(errorItem(cmd, kTokenExpiredCode,
                                     QStringLiteral("please login first")));
            continue;
        }
        results.append(dispatch(cmd, item));
    }

    // 真机在业务出错时 HTTP 状态照样是 200，错误只体现在 code 上。
    return HttpResponse::json(encodeResponse(results));
}

QJsonObject ReolinkStub::dispatch(const QString &cmd, const QJsonObject &item)
{
    if (cmd == QLatin1String("GetDevInfo"))
        return cmdGetDevInfo();
    if (cmd == QLatin1String("GetAbility"))
        return cmdGetAbility();
    if (cmd == QLatin1String("GetAiState"))
        return cmdGetAiState();
    if (cmd == QLatin1String("GetEnc"))
        return cmdGetEnc();
    if (cmd == QLatin1String("GetWhiteLed"))
        return cmdGetWhiteLed();
    if (cmd == QLatin1String("SetWhiteLed"))
        return cmdSetWhiteLed(item);
    if (cmd == QLatin1String("GetPtzPreset"))
        return cmdGetPtzPreset();
    if (cmd == QLatin1String("SetPtzPreset"))
        return cmdSetPtzPreset(item);
    if (cmd == QLatin1String("DelPtzPreset"))
        return cmdDelPtzPreset(item);
    if (cmd == QLatin1String("PtzCtrl"))
        return cmdPtzCtrl(item);
    if (cmd == QLatin1String("AudioAlarmPlay"))
        return cmdAudioAlarmPlay(item);
    // 真机对不认识的命令回 rspCode -9（"not support"）。
    return errorItem(cmd, -9, QStringLiteral("not support"));
}

// ---------------------------------------------------------------------------
// 各命令
// ---------------------------------------------------------------------------

QJsonObject ReolinkStub::cmdLogin(const QJsonObject &item)
{
    const QJsonObject param = item.value(QStringLiteral("param")).toObject();
    const QJsonObject user = param.value(QStringLiteral("User")).toObject();
    const QString name = user.value(QStringLiteral("userName")).toString();
    const QString password = user.value(QStringLiteral("password")).toString();

    // camera 为空是单测路径：没有用户表就一律放行，
    // 让测试聚焦在 token 生命周期而不是凭据。
    if (m_camera && !m_camera->model().checkPassword(name, password))
        return errorItem(QStringLiteral("Login"), -7, QStringLiteral("login failed"));

    QJsonObject tokenObj;
    tokenObj.insert(QStringLiteral("leaseTime"), kTokenLeaseSeconds);
    tokenObj.insert(QStringLiteral("name"), issueToken());
    QJsonObject value;
    value.insert(QStringLiteral("Token"), tokenObj);
    return successItem(QStringLiteral("Login"), value);
}

QJsonObject ReolinkStub::cmdGetDevInfo() const
{
    const CameraModel model = m_camera ? m_camera->model() : CameraModel();
    const Persona &persona = m_camera ? m_camera->persona() : PersonaRegistry::generic();

    QJsonObject info;
    info.insert(QStringLiteral("B485"), 0);
    info.insert(QStringLiteral("IOInputNum"), 0);
    info.insert(QStringLiteral("IOOutputNum"), 0);
    info.insert(QStringLiteral("audioNum"), 1);
    info.insert(QStringLiteral("buildDay"), QStringLiteral("build 22041215"));
    info.insert(QStringLiteral("cfgVer"), QStringLiteral("v3.0.0.0"));
    info.insert(QStringLiteral("channelNum"), 1);
    info.insert(QStringLiteral("detail"), model.hardwareId);
    info.insert(QStringLiteral("diskNum"), 1);
    info.insert(QStringLiteral("exactType"), QStringLiteral("IPC"));
    info.insert(QStringLiteral("firmVer"), model.firmwareVersion);
    info.insert(QStringLiteral("frameworkVer"), 1);
    info.insert(QStringLiteral("hardVer"), model.hardwareId);
    info.insert(QStringLiteral("model"), model.model);
    info.insert(QStringLiteral("name"), model.displayName);
    info.insert(QStringLiteral("pakSuffix"), QStringLiteral("pak"));
    info.insert(QStringLiteral("serial"), model.serialNumber.isEmpty() ? persona.serialPrefix
                                                                      : model.serialNumber);
    info.insert(QStringLiteral("type"), QStringLiteral("IPC"));
    info.insert(QStringLiteral("wifi"), 0);
    // 识别用：客户端拿 manufacturer + model + hardware_id 拼串做子串匹配。
    info.insert(QStringLiteral("vendor"), model.manufacturer);

    QJsonObject value;
    value.insert(QStringLiteral("DevInfo"), info);
    return successItem(QStringLiteral("GetDevInfo"), value);
}

QJsonObject ReolinkStub::cmdGetAbility() const
{
    const bool hasPtz = m_camera ? m_camera->model().hasPtzService : true;
    const bool hasLight = m_camera ? m_camera->model().imaging.whiteLight : true;

    QJsonObject chn;
    // permit 是位掩码：4 可读、2 可写，6 = 读写。0 表示不支持。
    chn.insert(QStringLiteral("ptzCtrl"), ability(hasPtz ? 6 : 0, 1));
    chn.insert(QStringLiteral("ptzPreset"), ability(hasPtz ? 6 : 0, 1));
    chn.insert(QStringLiteral("ptzPatrol"), ability(hasPtz ? 6 : 0, 1));
    chn.insert(QStringLiteral("aiTrack"), ability(0, 0));
    chn.insert(QStringLiteral("floodLight"), ability(hasLight ? 6 : 0, 1));
    chn.insert(QStringLiteral("supportAiPeople"), ability(0, 1));
    chn.insert(QStringLiteral("supportAiVehicle"), ability(0, 1));
    chn.insert(QStringLiteral("supportAiDogCat"), ability(0, 1));
    chn.insert(QStringLiteral("supportAiFace"), ability(0, 0));
    chn.insert(QStringLiteral("supportAudioAlarm"), ability(6, 1));
    chn.insert(QStringLiteral("mdWithPir"), ability(0, 0));

    QJsonArray chnArray;
    chnArray.append(chn);
    QJsonObject abilityObj;
    abilityObj.insert(QStringLiteral("abilityChn"), chnArray);
    abilityObj.insert(QStringLiteral("devInfo"), ability(4, 1));
    abilityObj.insert(QStringLiteral("push"), ability(6, 1));

    QJsonObject value;
    value.insert(QStringLiteral("Ability"), abilityObj);
    return successItem(QStringLiteral("GetAbility"), value);
}

QJsonObject ReolinkStub::cmdGetAiState() const
{
    // alarm_state 直接取事件引擎里属性型 topic 的当前状态，
    // 这样 GUI 触发一次人形侦测，Reolink 侧轮询也能立刻看到 1。
    QMap<EventKind, bool> active;
    if (m_camera && m_camera->events()) {
        const QMap<QString, bool> states = m_camera->events()->propertyStates();
        const QVector<TopicDef> topics = m_camera->events()->topics();
        for (const TopicDef &def : topics) {
            const auto it = states.constFind(def.topic);
            if (it != states.constEnd() && it.value())
                active.insert(def.kind, true);
        }
    }
    const auto stateOf = [&active](EventKind kind, bool supported) {
        QJsonObject o;
        o.insert(QStringLiteral("alarm_state"), active.value(kind, false) ? 1 : 0);
        o.insert(QStringLiteral("support"), supported ? 1 : 0);
        return o;
    };

    QJsonObject value;
    value.insert(QStringLiteral("channel"), 0);
    value.insert(QStringLiteral("people"), stateOf(EventKind::PeopleDetect, true));
    value.insert(QStringLiteral("vehicle"), stateOf(EventKind::VehicleDetect, true));
    value.insert(QStringLiteral("dog_cat"), stateOf(EventKind::AnimalDetect, true));
    value.insert(QStringLiteral("face"), stateOf(EventKind::FaceDetect, false));
    return successItem(QStringLiteral("GetAiState"), value);
}

QJsonObject ReolinkStub::cmdGetEnc() const
{
    const QList<MediaProfile> profiles = m_camera ? m_camera->model().profiles
                                                  : QList<MediaProfile>();
    VideoEncoderConfig mainVideo;
    VideoEncoderConfig subVideo;
    subVideo.width = 640;
    subVideo.height = 480;
    bool audio = true;
    if (!profiles.isEmpty()) {
        mainVideo = profiles.at(0).videoEncoder;
        audio = profiles.at(0).hasAudio;
        subVideo = profiles.at(profiles.size() > 1 ? 1 : 0).videoEncoder;
    }

    QJsonObject enc;
    enc.insert(QStringLiteral("audio"), audio ? 1 : 0);
    enc.insert(QStringLiteral("channel"), 0);
    enc.insert(QStringLiteral("mainStream"), encStream(mainVideo));
    enc.insert(QStringLiteral("subStream"), encStream(subVideo));
    QJsonObject value;
    value.insert(QStringLiteral("Enc"), enc);
    return successItem(QStringLiteral("GetEnc"), value);
}

QJsonObject ReolinkStub::cmdGetWhiteLed() const
{
    ImagingState *imaging = m_camera ? m_camera->imaging() : nullptr;
    const bool on = imaging && imaging->settings().whiteLightOn;
    const int bright = imaging ? imaging->settings().whiteLightBrightness : 100;

    QJsonObject schedule;
    schedule.insert(QStringLiteral("StartHour"), 18);
    schedule.insert(QStringLiteral("StartMin"), 0);
    schedule.insert(QStringLiteral("EndHour"), 6);
    schedule.insert(QStringLiteral("EndMin"), 0);

    QJsonObject aiDetect;
    aiDetect.insert(QStringLiteral("people"), 0);
    aiDetect.insert(QStringLiteral("vehicle"), 0);
    aiDetect.insert(QStringLiteral("dog_cat"), 0);
    aiDetect.insert(QStringLiteral("face"), 0);

    QJsonObject led;
    led.insert(QStringLiteral("channel"), 0);
    led.insert(QStringLiteral("bright"), bright);
    // mode 0 = 常关、1 = 夜间自动、3 = 手动常亮。灯亮时报 3 最贴合手动开灯。
    led.insert(QStringLiteral("mode"), on ? 3 : 0);
    led.insert(QStringLiteral("state"), on ? 1 : 0);
    led.insert(QStringLiteral("LightingSchedule"), schedule);
    led.insert(QStringLiteral("wlAiDetectType"), aiDetect);

    QJsonObject value;
    value.insert(QStringLiteral("WhiteLed"), led);
    return successItem(QStringLiteral("GetWhiteLed"), value);
}

QJsonObject ReolinkStub::cmdSetWhiteLed(const QJsonObject &item)
{
    const QJsonObject param = item.value(QStringLiteral("param")).toObject();
    const QJsonObject led = param.value(QStringLiteral("WhiteLed")).toObject();
    if (led.isEmpty())
        return errorItem(QStringLiteral("SetWhiteLed"), -4, QStringLiteral("missing WhiteLed"));

    ImagingState *imaging = m_camera ? m_camera->imaging() : nullptr;
    bool on = imaging ? imaging->settings().whiteLightOn : false;
    if (led.contains(QStringLiteral("state")))
        on = led.value(QStringLiteral("state")).toInt() != 0;
    else if (led.contains(QStringLiteral("mode")))
        on = led.value(QStringLiteral("mode")).toInt() != 0;

    int bright = -1;
    if (led.contains(QStringLiteral("bright")))
        bright = qBound(0, led.value(QStringLiteral("bright")).toInt(), 100);

    if (imaging)
        imaging->setWhiteLight(on, bright);
    return rspCodeItem(QStringLiteral("SetWhiteLed"), 200);
}

QJsonObject ReolinkStub::cmdGetPtzPreset() const
{
    QJsonArray list;
    if (m_camera && m_camera->ptz()) {
        int index = 1;
        const QList<PtzPreset> presets = m_camera->ptz()->presets();
        for (const PtzPreset &preset : presets) {
            bool numeric = false;
            const int id = preset.token.toInt(&numeric);
            QJsonObject o;
            o.insert(QStringLiteral("channel"), 0);
            o.insert(QStringLiteral("enable"), 1);
            o.insert(QStringLiteral("id"), numeric ? id : index);
            o.insert(QStringLiteral("name"), preset.name);
            list.append(o);
            ++index;
        }
    }
    QJsonObject value;
    value.insert(QStringLiteral("PtzPreset"), list);
    return successItem(QStringLiteral("GetPtzPreset"), value);
}

QJsonObject ReolinkStub::cmdSetPtzPreset(const QJsonObject &item)
{
    const QJsonObject param = item.value(QStringLiteral("param")).toObject();
    const QJsonObject preset = param.value(QStringLiteral("PtzPreset")).toObject();
    const int id = preset.value(QStringLiteral("id")).toInt();
    if (id <= 0)
        return errorItem(QStringLiteral("SetPtzPreset"), -4, QStringLiteral("bad preset id"));

    PtzState *ptz = m_camera ? m_camera->ptz() : nullptr;
    if (!ptz)
        return rspCodeItem(QStringLiteral("SetPtzPreset"), 200);

    // Reolink 用 enable=0 表示删除这个槽位，没有单独的删除命令必走。
    if (preset.contains(QStringLiteral("enable"))
        && preset.value(QStringLiteral("enable")).toInt() == 0) {
        ptz->removePreset(QString::number(id));
        return rspCodeItem(QStringLiteral("SetPtzPreset"), 200);
    }

    QString name = preset.value(QStringLiteral("name")).toString();
    if (name.isEmpty())
        name = QStringLiteral("pos%1").arg(id);
    QString error;
    if (ptz->setPreset(name, QString::number(id), &error).isEmpty())
        return errorItem(QStringLiteral("SetPtzPreset"), -4, error);
    return rspCodeItem(QStringLiteral("SetPtzPreset"), 200);
}

QJsonObject ReolinkStub::cmdDelPtzPreset(const QJsonObject &item)
{
    const QJsonObject param = item.value(QStringLiteral("param")).toObject();
    const QJsonObject preset = param.value(QStringLiteral("PtzPreset")).toObject();
    const int id = preset.contains(QStringLiteral("id"))
        ? preset.value(QStringLiteral("id")).toInt()
        : param.value(QStringLiteral("id")).toInt();
    if (id <= 0)
        return errorItem(QStringLiteral("DelPtzPreset"), -4, QStringLiteral("bad preset id"));
    if (m_camera && m_camera->ptz())
        m_camera->ptz()->removePreset(QString::number(id));
    return rspCodeItem(QStringLiteral("DelPtzPreset"), 200);
}

QJsonObject ReolinkStub::cmdPtzCtrl(const QJsonObject &item)
{
    const QJsonObject param = item.value(QStringLiteral("param")).toObject();
    const QString op = param.value(QStringLiteral("op")).toString();
    const double s = speedFromReolink(param.value(QStringLiteral("speed")).toInt());

    PtzState *ptz = m_camera ? m_camera->ptz() : nullptr;
    if (!ptz)
        return rspCodeItem(QStringLiteral("PtzCtrl"), 200);

    if (op == QLatin1String("Stop")) {
        ptz->stop();
        return rspCodeItem(QStringLiteral("PtzCtrl"), 200);
    }
    if (op == QLatin1String("ToPos")) {
        PtzVector speed;
        speed.pan = speed.tilt = speed.zoom = s;
        const int id = param.value(QStringLiteral("id")).toInt();
        if (!ptz->gotoPreset(QString::number(id), speed))
            return errorItem(QStringLiteral("PtzCtrl"), -4, QStringLiteral("no such preset"));
        return rspCodeItem(QStringLiteral("PtzCtrl"), 200);
    }

    PtzVector v;
    v.hasPanTilt = true;
    v.hasZoom = false;
    if (op == QLatin1String("Left")) {
        v.pan = -s;
    } else if (op == QLatin1String("Right")) {
        v.pan = s;
    } else if (op == QLatin1String("Up")) {
        v.tilt = s;
    } else if (op == QLatin1String("Down")) {
        v.tilt = -s;
    } else if (op == QLatin1String("LeftUp")) {
        v.pan = -s; v.tilt = s;
    } else if (op == QLatin1String("RightUp")) {
        v.pan = s;  v.tilt = s;
    } else if (op == QLatin1String("LeftDown")) {
        v.pan = -s; v.tilt = -s;
    } else if (op == QLatin1String("RightDown")) {
        v.pan = s;  v.tilt = -s;
    } else if (op == QLatin1String("ZoomInc")) {
        v.hasPanTilt = false; v.hasZoom = true; v.zoom = s;
    } else if (op == QLatin1String("ZoomDec")) {
        v.hasPanTilt = false; v.hasZoom = true; v.zoom = -s;
    } else {
        return errorItem(QStringLiteral("PtzCtrl"), -9, QStringLiteral("not support"));
    }
    ptz->continuousMove(v);
    return rspCodeItem(QStringLiteral("PtzCtrl"), 200);
}

QJsonObject ReolinkStub::cmdAudioAlarmPlay(const QJsonObject &item)
{
    const QJsonObject param = item.value(QStringLiteral("param")).toObject();
    const bool on = param.value(QStringLiteral("manual_switch")).toInt() != 0;
    // 警笛没有独立状态位，用一次继电器输出事件代表它响过 ——
    // 与大华 coaxialControlIO 的 Speaker 走同一条路，ONVIF 订阅方也能看见。
    if (on && m_camera && m_camera->events())
        m_camera->events()->trigger(EventKind::RelayOutput, 2000);
    return rspCodeItem(QStringLiteral("AudioAlarmPlay"), 200);
}

namespace vendorapi {

VendorApiStub *createReolinkJson(VirtualCamera *camera)
{
    return new ReolinkStub(camera, camera);
}

} // namespace vendorapi
} // namespace onvifsim
