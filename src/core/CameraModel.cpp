#include "core/CameraModel.h"

#include "core/Persona.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QUuid>

namespace onvifsim {
namespace {

QString jsonString(const QJsonObject &o, const char *key, const QString &fallback)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isString() ? v.toString() : fallback;
}

int jsonInt(const QJsonObject &o, const char *key, int fallback)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? v.toInt() : fallback;
}

// 端口单独走这个：直接 static_cast<quint16> 会把 99999 悄悄截成 33903，
// 场景文件写错了端口，表现是「相机起在一个谁也想不到的端口上」而不是报错。
quint16 jsonPort(const QJsonObject &obj, const char *key, quint16 fallback,
                 QStringList *errors)
{
    const int value = jsonInt(obj, key, fallback);
    if (value < 0 || value > 65535) {
        if (errors) {
            errors->append(QCoreApplication::translate("onvifsim::core", "端口 %1 超出 1~65535：%2")
                               .arg(QLatin1String(key)).arg(value));
        }
        return fallback;
    }
    return static_cast<quint16>(value);
}

double jsonDouble(const QJsonObject &o, const char *key, double fallback)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? v.toDouble() : fallback;
}

bool jsonBool(const QJsonObject &o, const char *key, bool fallback)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isBool() ? v.toBool() : fallback;
}

QJsonObject videoEncoderToJson(const VideoEncoderConfig &c)
{
    QJsonObject o;
    o.insert(QStringLiteral("token"), c.token);
    o.insert(QStringLiteral("name"), c.name);
    o.insert(QStringLiteral("encoding"), c.encoding);
    o.insert(QStringLiteral("width"), c.width);
    o.insert(QStringLiteral("height"), c.height);
    o.insert(QStringLiteral("frameRate"), c.frameRate);
    o.insert(QStringLiteral("bitrateKbps"), c.bitrateKbps);
    o.insert(QStringLiteral("govLength"), c.govLength);
    o.insert(QStringLiteral("quality"), c.quality);
    o.insert(QStringLiteral("h264Profile"), c.h264Profile);
    o.insert(QStringLiteral("sessionTimeoutSec"), c.sessionTimeoutSec);
    return o;
}

VideoEncoderConfig videoEncoderFromJson(const QJsonObject &o, const VideoEncoderConfig &base)
{
    VideoEncoderConfig c = base;
    c.token = jsonString(o, "token", c.token);
    c.name = jsonString(o, "name", c.name);
    c.encoding = jsonString(o, "encoding", c.encoding);
    c.width = jsonInt(o, "width", c.width);
    c.height = jsonInt(o, "height", c.height);
    c.frameRate = jsonDouble(o, "frameRate", c.frameRate);
    c.bitrateKbps = jsonInt(o, "bitrateKbps", c.bitrateKbps);
    c.govLength = jsonInt(o, "govLength", c.govLength);
    c.quality = jsonInt(o, "quality", c.quality);
    c.h264Profile = jsonString(o, "h264Profile", c.h264Profile);
    c.sessionTimeoutSec = jsonInt(o, "sessionTimeoutSec", c.sessionTimeoutSec);
    return c;
}

QJsonObject audioEncoderToJson(const AudioEncoderConfig &c)
{
    QJsonObject o;
    o.insert(QStringLiteral("token"), c.token);
    o.insert(QStringLiteral("name"), c.name);
    o.insert(QStringLiteral("encoding"), c.encoding);
    o.insert(QStringLiteral("bitrateKbps"), c.bitrateKbps);
    o.insert(QStringLiteral("sampleRateKhz"), c.sampleRateKhz);
    return o;
}

AudioEncoderConfig audioEncoderFromJson(const QJsonObject &o, const AudioEncoderConfig &base)
{
    AudioEncoderConfig c = base;
    c.token = jsonString(o, "token", c.token);
    c.name = jsonString(o, "name", c.name);
    c.encoding = jsonString(o, "encoding", c.encoding);
    c.bitrateKbps = jsonInt(o, "bitrateKbps", c.bitrateKbps);
    c.sampleRateKhz = jsonInt(o, "sampleRateKhz", c.sampleRateKhz);
    return c;
}

// 下面这几个子配置原来在存盘时被整段丢掉，于是「存场景 → 重新加载」并不等价：
// 一台 4K 相机的 VideoSource Bounds 会悄悄退回 1920×1080，对讲的 sendPrimacy
// 和 outputLevel 也回到默认。而它们不是摆设 —— MediaService 会把它们写进
// GetVideoSourceConfigurations / GetVideoSources / GetStreamUri 的响应。
QJsonObject videoSourceToJson(const VideoSourceConfig &c)
{
    QJsonObject o;
    o.insert(QStringLiteral("token"), c.token);
    o.insert(QStringLiteral("name"), c.name);
    o.insert(QStringLiteral("sourceToken"), c.sourceToken);
    o.insert(QStringLiteral("useCount"), c.useCount);
    o.insert(QStringLiteral("boundsX"), c.boundsX);
    o.insert(QStringLiteral("boundsY"), c.boundsY);
    o.insert(QStringLiteral("boundsWidth"), c.boundsWidth);
    o.insert(QStringLiteral("boundsHeight"), c.boundsHeight);
    return o;
}

VideoSourceConfig videoSourceFromJson(const QJsonObject &o, const VideoSourceConfig &base)
{
    VideoSourceConfig c = base;
    c.token = jsonString(o, "token", c.token);
    c.name = jsonString(o, "name", c.name);
    c.sourceToken = jsonString(o, "sourceToken", c.sourceToken);
    c.useCount = jsonInt(o, "useCount", c.useCount);
    c.boundsX = jsonInt(o, "boundsX", c.boundsX);
    c.boundsY = jsonInt(o, "boundsY", c.boundsY);
    c.boundsWidth = jsonInt(o, "boundsWidth", c.boundsWidth);
    c.boundsHeight = jsonInt(o, "boundsHeight", c.boundsHeight);
    return c;
}

QJsonObject audioSourceToJson(const AudioSourceConfig &c)
{
    QJsonObject o;
    o.insert(QStringLiteral("token"), c.token);
    o.insert(QStringLiteral("name"), c.name);
    o.insert(QStringLiteral("sourceToken"), c.sourceToken);
    return o;
}

AudioSourceConfig audioSourceFromJson(const QJsonObject &o, const AudioSourceConfig &base)
{
    AudioSourceConfig c = base;
    c.token = jsonString(o, "token", c.token);
    c.name = jsonString(o, "name", c.name);
    c.sourceToken = jsonString(o, "sourceToken", c.sourceToken);
    return c;
}

QJsonObject audioOutputToJson(const AudioOutputConfig &c)
{
    QJsonObject o;
    o.insert(QStringLiteral("token"), c.token);
    o.insert(QStringLiteral("name"), c.name);
    o.insert(QStringLiteral("outputToken"), c.outputToken);
    o.insert(QStringLiteral("sendPrimacy"), c.sendPrimacy);
    o.insert(QStringLiteral("outputLevel"), c.outputLevel);
    return o;
}

AudioOutputConfig audioOutputFromJson(const QJsonObject &o, const AudioOutputConfig &base)
{
    AudioOutputConfig c = base;
    c.token = jsonString(o, "token", c.token);
    c.name = jsonString(o, "name", c.name);
    c.outputToken = jsonString(o, "outputToken", c.outputToken);
    c.sendPrimacy = jsonString(o, "sendPrimacy", c.sendPrimacy);
    c.outputLevel = jsonInt(o, "outputLevel", c.outputLevel);
    return c;
}

QJsonObject audioDecoderToJson(const AudioDecoderConfig &c)
{
    QJsonObject o;
    o.insert(QStringLiteral("token"), c.token);
    o.insert(QStringLiteral("name"), c.name);
    return o;
}

AudioDecoderConfig audioDecoderFromJson(const QJsonObject &o, const AudioDecoderConfig &base)
{
    AudioDecoderConfig c = base;
    c.token = jsonString(o, "token", c.token);
    c.name = jsonString(o, "name", c.name);
    return c;
}

QJsonObject metadataToJson(const MetadataConfig &c)
{
    QJsonObject o;
    o.insert(QStringLiteral("token"), c.token);
    o.insert(QStringLiteral("name"), c.name);
    o.insert(QStringLiteral("analytics"), c.analytics);
    o.insert(QStringLiteral("events"), c.events);
    o.insert(QStringLiteral("ptzStatus"), c.ptzStatus);
    return o;
}

MetadataConfig metadataFromJson(const QJsonObject &o, const MetadataConfig &base)
{
    MetadataConfig c = base;
    c.token = jsonString(o, "token", c.token);
    c.name = jsonString(o, "name", c.name);
    c.analytics = jsonBool(o, "analytics", c.analytics);
    c.events = jsonBool(o, "events", c.events);
    c.ptzStatus = jsonBool(o, "ptzStatus", c.ptzStatus);
    return c;
}

QJsonObject profileToJson(const MediaProfile &p)
{
    QJsonObject o;
    o.insert(QStringLiteral("token"), p.token);
    o.insert(QStringLiteral("name"), p.name);
    o.insert(QStringLiteral("fixed"), p.fixed);
    o.insert(QStringLiteral("videoSource"), videoSourceToJson(p.videoSource));
    o.insert(QStringLiteral("videoEncoder"), videoEncoderToJson(p.videoEncoder));
    o.insert(QStringLiteral("hasAudio"), p.hasAudio);
    if (p.hasAudio) {
        o.insert(QStringLiteral("audioSource"), audioSourceToJson(p.audioSource));
        o.insert(QStringLiteral("audioEncoder"), audioEncoderToJson(p.audioEncoder));
    }
    o.insert(QStringLiteral("hasPtz"), p.hasPtz);
    o.insert(QStringLiteral("ptzConfigToken"), p.ptzConfigToken);
    o.insert(QStringLiteral("hasAudioOutput"), p.hasAudioOutput);
    if (p.hasAudioOutput)
        o.insert(QStringLiteral("audioOutput"), audioOutputToJson(p.audioOutput));
    o.insert(QStringLiteral("hasAudioDecoder"), p.hasAudioDecoder);
    if (p.hasAudioDecoder)
        o.insert(QStringLiteral("audioDecoder"), audioDecoderToJson(p.audioDecoder));
    o.insert(QStringLiteral("hasMetadata"), p.hasMetadata);
    if (p.hasMetadata)
        o.insert(QStringLiteral("metadata"), metadataToJson(p.metadata));
    if (!p.streamPath.isEmpty())
        o.insert(QStringLiteral("streamPath"), p.streamPath);
    o.insert(QStringLiteral("mediaAsset"), p.mediaAsset);
    return o;
}

MediaProfile profileFromJson(const QJsonObject &o)
{
    MediaProfile p;
    p.token = jsonString(o, "token", p.token);
    p.name = jsonString(o, "name", p.name);
    p.fixed = jsonBool(o, "fixed", p.fixed);
    p.videoSource = videoSourceFromJson(o.value(QStringLiteral("videoSource")).toObject(),
                                        p.videoSource);
    p.videoEncoder = videoEncoderFromJson(o.value(QStringLiteral("videoEncoder")).toObject(),
                                          p.videoEncoder);
    p.hasAudio = jsonBool(o, "hasAudio", p.hasAudio);
    p.audioSource = audioSourceFromJson(o.value(QStringLiteral("audioSource")).toObject(),
                                        p.audioSource);
    p.audioEncoder = audioEncoderFromJson(o.value(QStringLiteral("audioEncoder")).toObject(),
                                          p.audioEncoder);
    p.hasPtz = jsonBool(o, "hasPtz", p.hasPtz);
    p.ptzConfigToken = jsonString(o, "ptzConfigToken", p.ptzConfigToken);
    p.hasAudioOutput = jsonBool(o, "hasAudioOutput", p.hasAudioOutput);
    p.audioOutput = audioOutputFromJson(o.value(QStringLiteral("audioOutput")).toObject(),
                                        p.audioOutput);
    p.hasAudioDecoder = jsonBool(o, "hasAudioDecoder", p.hasAudioDecoder);
    p.audioDecoder = audioDecoderFromJson(o.value(QStringLiteral("audioDecoder")).toObject(),
                                          p.audioDecoder);
    p.hasMetadata = jsonBool(o, "hasMetadata", p.hasMetadata);
    p.metadata = metadataFromJson(o.value(QStringLiteral("metadata")).toObject(), p.metadata);
    p.streamPath = jsonString(o, "streamPath", p.streamPath);
    p.mediaAsset = jsonString(o, "mediaAsset", p.mediaAsset);
    // 分辨率是 profile 的关键属性，样片档位没写就按分辨率推。
    if (!o.contains(QStringLiteral("mediaAsset"))) {
        if (p.videoEncoder.height >= 1080)
            p.mediaAsset = QStringLiteral("1080p");
        else if (p.videoEncoder.height >= 720)
            p.mediaAsset = QStringLiteral("720p");
        else
            p.mediaAsset = QStringLiteral("360p");
    }
    return p;
}

} // namespace

QString CameraModel::profileNameForStyle(const QString &style, int index)
{
    if (style == QLatin1String("profile_n"))
        return QStringLiteral("Profile_%1").arg(index + 1);
    if (style == QLatin1String("chinese")) {
        static const char *names[] = { "主码流", "子码流", "第三码流" };
        return index < 3 ? QString::fromUtf8(names[index])
                         : QStringLiteral("码流%1").arg(index + 1);
    }
    if (style == QLatin1String("token_only"))
        return QStringLiteral("%1").arg(index + 1);

    static const char *names[] = { "MainStream", "SubStream", "ThirdStream" };
    return index < 3 ? QString::fromLatin1(names[index])
                     : QStringLiteral("Stream%1").arg(index + 1);
}

CameraModel::CameraModel() = default;

const User *CameraModel::findUser(const QString &username) const
{
    for (const User &u : users) {
        if (u.username == username)
            return &u;
    }
    return nullptr;
}

bool CameraModel::checkPassword(const QString &username, const QString &password) const
{
    const User *u = findUser(username);
    return u && u->password == password;
}

UserLevel CameraModel::levelOf(const QString &username) const
{
    const User *u = findUser(username);
    return u ? u->level : UserLevel::Anonymous;
}

MediaProfile *CameraModel::profileByToken(const QString &token)
{
    for (MediaProfile &p : profiles) {
        if (p.token == token)
            return &p;
    }
    return nullptr;
}

const MediaProfile *CameraModel::profileByToken(const QString &token) const
{
    for (const MediaProfile &p : profiles) {
        if (p.token == token)
            return &p;
    }
    return nullptr;
}

QString CameraModel::userLevelName(UserLevel level)
{
    switch (level) {
    case UserLevel::Administrator: return QStringLiteral("Administrator");
    case UserLevel::Operator:      return QStringLiteral("Operator");
    case UserLevel::User:          return QStringLiteral("User");
    case UserLevel::Anonymous:     break;
    }
    return QStringLiteral("Anonymous");
}

UserLevel CameraModel::userLevelFromName(const QString &name, bool *ok)
{
    if (ok)
        *ok = true;
    if (name.compare(QLatin1String("Administrator"), Qt::CaseInsensitive) == 0)
        return UserLevel::Administrator;
    if (name.compare(QLatin1String("Operator"), Qt::CaseInsensitive) == 0)
        return UserLevel::Operator;
    if (name.compare(QLatin1String("User"), Qt::CaseInsensitive) == 0)
        return UserLevel::User;
    if (name.compare(QLatin1String("Anonymous"), Qt::CaseInsensitive) == 0)
        return UserLevel::Anonymous;
    if (ok)
        *ok = false;
    return UserLevel::User;
}

QJsonObject CameraModel::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), id);
    o.insert(QStringLiteral("displayName"), displayName);
    o.insert(QStringLiteral("persona"), personaKey);

    QJsonObject identity;
    identity.insert(QStringLiteral("manufacturer"), manufacturer);
    identity.insert(QStringLiteral("model"), model);
    identity.insert(QStringLiteral("firmwareVersion"), firmwareVersion);
    identity.insert(QStringLiteral("serialNumber"), serialNumber);
    identity.insert(QStringLiteral("hardwareId"), hardwareId);
    identity.insert(QStringLiteral("endpointReference"), endpointReference);
    identity.insert(QStringLiteral("hostname"), hostname);
    identity.insert(QStringLiteral("location"), location);
    identity.insert(QStringLiteral("scopes"), QJsonArray::fromStringList(scopes));
    o.insert(QStringLiteral("identity"), identity);

    QJsonObject network;
    network.insert(QStringLiteral("bindAddress"), bindAddress.toString());
    network.insert(QStringLiteral("httpPort"), httpPort);
    network.insert(QStringLiteral("rtspPort"), rtspPort);
    if (!advertisedHost.isEmpty())
        network.insert(QStringLiteral("advertisedHost"), advertisedHost);
    if (advertisedHttpPort)
        network.insert(QStringLiteral("advertisedHttpPort"), advertisedHttpPort);
    if (advertisedRtspPort)
        network.insert(QStringLiteral("advertisedRtspPort"), advertisedRtspPort);
    if (!macAddress.isEmpty())
        network.insert(QStringLiteral("macAddress"), macAddress);
    o.insert(QStringLiteral("network"), network);

    QJsonArray userArray;
    for (const User &u : users) {
        QJsonObject uo;
        uo.insert(QStringLiteral("username"), u.username);
        uo.insert(QStringLiteral("password"), u.password);
        uo.insert(QStringLiteral("level"), userLevelName(u.level));
        userArray.append(uo);
    }
    o.insert(QStringLiteral("users"), userArray);

    QJsonArray profileArray;
    for (const MediaProfile &p : profiles)
        profileArray.append(profileToJson(p));
    o.insert(QStringLiteral("profiles"), profileArray);

    QJsonObject ptz;
    ptz.insert(QStringLiteral("nodeToken"), ptzNode.nodeToken);
    ptz.insert(QStringLiteral("nodeName"), ptzNode.nodeName);
    ptz.insert(QStringLiteral("configToken"), ptzNode.configToken);
    ptz.insert(QStringLiteral("configName"), ptzNode.configName);
    ptz.insert(QStringLiteral("supportsContinuous"), ptzNode.supportsContinuous);
    ptz.insert(QStringLiteral("supportsAbsolute"), ptzNode.supportsAbsolute);
    ptz.insert(QStringLiteral("supportsRelative"), ptzNode.supportsRelative);
    ptz.insert(QStringLiteral("supportsZoom"), ptzNode.supportsZoom);
    ptz.insert(QStringLiteral("homeSupported"), ptzNode.homeSupported);
    ptz.insert(QStringLiteral("maxPresets"), ptzNode.maxPresets);
    o.insert(QStringLiteral("ptzNode"), ptz);

    QJsonObject caps;
    caps.insert(QStringLiteral("ptz"), hasPtzService);
    caps.insert(QStringLiteral("imaging"), hasImagingService);
    caps.insert(QStringLiteral("events"), hasEventsService);
    caps.insert(QStringLiteral("analytics"), hasAnalyticsService);
    caps.insert(QStringLiteral("deviceIo"), hasDeviceIoService);
    caps.insert(QStringLiteral("media2"), hasMedia2Service);
    caps.insert(QStringLiteral("backchannel"), hasAudioBackchannel);
    caps.insert(QStringLiteral("relayOutputs"), hasRelayOutputs);
    caps.insert(QStringLiteral("digitalInputs"), hasDigitalInputs);
    // imaging 的开关原来只存了 whiteLight 一条，其余五条在存盘时被丢掉。
    // ImagingService 是按它们决定 GetOptions 里出哪些项的，丢了就等于
    //「存一次场景，相机的图像能力悄悄变了一套」。
    caps.insert(QStringLiteral("whiteLight"), imaging.whiteLight);
    caps.insert(QStringLiteral("brightness"), imaging.brightness);
    caps.insert(QStringLiteral("contrast"), imaging.contrast);
    caps.insert(QStringLiteral("saturation"), imaging.saturation);
    caps.insert(QStringLiteral("sharpness"), imaging.sharpness);
    caps.insert(QStringLiteral("irCutFilter"), imaging.irCutFilter);
    caps.insert(QStringLiteral("focus"), imaging.focus);
    o.insert(QStringLiteral("capabilities"), caps);

    o.insert(QStringLiteral("enabled"), enabled);
    return o;
}

CameraModel CameraModel::fromJson(const QJsonObject &obj, QStringList *errors)
{
    const QString id = jsonString(obj, "id", QStringLiteral("cam1"));
    const QString personaKey = jsonString(obj, "persona", QStringLiteral("generic"));
    if (!PersonaRegistry::find(personaKey) && errors)
        errors->append(QCoreApplication::translate("onvifsim::core", "未知的品牌预设：%1").arg(personaKey));

    // 先按预设铺一整台默认相机（三个 profile、用户表、scopes、PTZ 节点都齐），
    // 再让 JSON 里显式写出的字段覆盖它。场景文件因此只需要写差异部分 ——
    // 不这么做的话，没写 profiles 的场景会得到一台没有码流的相机，
    // 没写 users 的会得到一台谁都登不上的相机。
    CameraModel m = makeDefault(id, personaKey, 0);
    m.displayName = jsonString(obj, "displayName", m.displayName);

    const QJsonObject identity = obj.value(QStringLiteral("identity")).toObject();
    m.manufacturer = jsonString(identity, "manufacturer", m.manufacturer);
    m.model = jsonString(identity, "model", m.model);
    m.firmwareVersion = jsonString(identity, "firmwareVersion", m.firmwareVersion);
    m.serialNumber = jsonString(identity, "serialNumber", m.serialNumber);
    m.hardwareId = jsonString(identity, "hardwareId", m.hardwareId);
    m.endpointReference = jsonString(identity, "endpointReference", m.endpointReference);
    m.hostname = jsonString(identity, "hostname", m.hostname);
    m.location = jsonString(identity, "location", m.location);
    if (identity.value(QStringLiteral("scopes")).isArray()) {
        m.scopes.clear();
        for (const QJsonValue &v : identity.value(QStringLiteral("scopes")).toArray())
            m.scopes.append(v.toString());
    }

    const QJsonObject network = obj.value(QStringLiteral("network")).toObject();
    const QString bind = jsonString(network, "bindAddress", QString());
    if (!bind.isEmpty() && !m.bindAddress.setAddress(bind) && errors)
        errors->append(QCoreApplication::translate("onvifsim::core", "非法的绑定地址：%1").arg(bind));
    m.httpPort = jsonPort(network, "httpPort", m.httpPort, errors);
    m.rtspPort = jsonPort(network, "rtspPort", m.rtspPort, errors);
    m.advertisedHost = jsonString(network, "advertisedHost", m.advertisedHost);
    m.advertisedHttpPort = jsonPort(network, "advertisedHttpPort", 0, errors);
    m.advertisedRtspPort = jsonPort(network, "advertisedRtspPort", 0, errors);
    m.macAddress = jsonString(network, "macAddress", m.macAddress);

    if (obj.value(QStringLiteral("users")).isArray()) {
        m.users.clear();
        for (const QJsonValue &v : obj.value(QStringLiteral("users")).toArray()) {
            const QJsonObject uo = v.toObject();
            User u;
            u.username = jsonString(uo, "username", QString());
            u.password = jsonString(uo, "password", QString());
            bool levelOk = true;
            u.level = userLevelFromName(jsonString(uo, "level", QStringLiteral("Administrator")),
                                        &levelOk);
            if (!levelOk && errors)
                errors->append(QCoreApplication::translate("onvifsim::core", "未知的权限级别：%1")
                                   .arg(jsonString(uo, "level", QString())));
            if (u.username.isEmpty()) {
                if (errors)
                    errors->append(QCoreApplication::translate("onvifsim::core", "用户名为空的用户项已忽略"));
                continue;
            }
            m.users.append(u);
        }
    }

    if (obj.value(QStringLiteral("profiles")).isArray()) {
        m.profiles.clear();
        for (const QJsonValue &v : obj.value(QStringLiteral("profiles")).toArray())
            m.profiles.append(profileFromJson(v.toObject()));
    }

    const QJsonObject ptz = obj.value(QStringLiteral("ptzNode")).toObject();
    m.ptzNode.nodeToken = jsonString(ptz, "nodeToken", m.ptzNode.nodeToken);
    m.ptzNode.nodeName = jsonString(ptz, "nodeName", m.ptzNode.nodeName);
    m.ptzNode.configToken = jsonString(ptz, "configToken", m.ptzNode.configToken);
    m.ptzNode.configName = jsonString(ptz, "configName", m.ptzNode.configName);
    m.ptzNode.supportsContinuous = jsonBool(ptz, "supportsContinuous", m.ptzNode.supportsContinuous);
    m.ptzNode.supportsAbsolute = jsonBool(ptz, "supportsAbsolute", m.ptzNode.supportsAbsolute);
    m.ptzNode.supportsRelative = jsonBool(ptz, "supportsRelative", m.ptzNode.supportsRelative);
    m.ptzNode.supportsZoom = jsonBool(ptz, "supportsZoom", m.ptzNode.supportsZoom);
    m.ptzNode.homeSupported = jsonBool(ptz, "homeSupported", m.ptzNode.homeSupported);
    m.ptzNode.maxPresets = jsonInt(ptz, "maxPresets", m.ptzNode.maxPresets);

    const QJsonObject caps = obj.value(QStringLiteral("capabilities")).toObject();
    m.hasPtzService = jsonBool(caps, "ptz", m.hasPtzService);
    m.hasImagingService = jsonBool(caps, "imaging", m.hasImagingService);
    m.hasEventsService = jsonBool(caps, "events", m.hasEventsService);
    m.hasAnalyticsService = jsonBool(caps, "analytics", m.hasAnalyticsService);
    m.hasDeviceIoService = jsonBool(caps, "deviceIo", m.hasDeviceIoService);
    m.hasMedia2Service = jsonBool(caps, "media2", m.hasMedia2Service);
    m.hasAudioBackchannel = jsonBool(caps, "backchannel", m.hasAudioBackchannel);
    m.hasRelayOutputs = jsonBool(caps, "relayOutputs", m.hasRelayOutputs);
    m.hasDigitalInputs = jsonBool(caps, "digitalInputs", m.hasDigitalInputs);
    m.imaging.whiteLight = jsonBool(caps, "whiteLight", m.imaging.whiteLight);
    m.imaging.brightness = jsonBool(caps, "brightness", m.imaging.brightness);
    m.imaging.contrast = jsonBool(caps, "contrast", m.imaging.contrast);
    m.imaging.saturation = jsonBool(caps, "saturation", m.imaging.saturation);
    m.imaging.sharpness = jsonBool(caps, "sharpness", m.imaging.sharpness);
    m.imaging.irCutFilter = jsonBool(caps, "irCutFilter", m.imaging.irCutFilter);
    m.imaging.focus = jsonBool(caps, "focus", m.imaging.focus);

    m.enabled = jsonBool(obj, "enabled", m.enabled);

    if (m.endpointReference.isEmpty())
        m.endpointReference = QStringLiteral("urn:uuid:%1")
                                  .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    return m;
}

CameraModel CameraModel::makeDefault(const QString &id, const QString &personaKey, int index)
{
    const Persona *persona = PersonaRegistry::find(personaKey);
    if (!persona)
        persona = &PersonaRegistry::generic();

    CameraModel m;
    m.id = id;
    m.personaKey = persona->key;
    m.displayName = QStringLiteral("%1 %2").arg(persona->displayName).arg(index + 1);
    m.manufacturer = persona->manufacturer;
    m.model = persona->model;
    m.firmwareVersion = persona->firmwareVersion;
    m.hardwareId = persona->hardwareId;
    m.serialNumber = QStringLiteral("%1%2")
                         .arg(persona->serialPrefix)
                         .arg(index + 1, 4, 10, QLatin1Char('0'));
    // EPR 是客户端的去重键，单次运行内必须固定；跨运行变不变都行。
    m.endpointReference = QStringLiteral("urn:uuid:%1")
                              .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    m.hostname = QStringLiteral("onvifsim-%1").arg(index + 1);

    // scope 里的空格必须百分号编码 —— 线路上 scopes 是空格分隔的一行。
    const QString encodedName = QString(m.displayName).replace(QLatin1Char(' '),
                                                               QStringLiteral("%20"));
    m.scopes = QStringList{
        QStringLiteral("onvif://www.onvif.org/type/video_encoder"),
        QStringLiteral("onvif://www.onvif.org/type/NetworkVideoTransmitter"),
        QStringLiteral("onvif://www.onvif.org/Profile/Streaming"),
        QStringLiteral("onvif://www.onvif.org/Profile/T"),
        QStringLiteral("onvif://www.onvif.org/name/%1").arg(encodedName),
        QStringLiteral("onvif://www.onvif.org/hardware/%1")
            .arg(QString(persona->model).replace(QLatin1Char(' '), QStringLiteral("%20"))),
        QStringLiteral("onvif://www.onvif.org/location/%1").arg(m.location),
    };
    m.scopes.append(persona->extraScopes);

    m.users = { User{ QStringLiteral("admin"), QStringLiteral("admin123"),
                      UserLevel::Administrator },
                User{ QStringLiteral("operator"), QStringLiteral("operator123"),
                      UserLevel::Operator },
                User{ QStringLiteral("viewer"), QStringLiteral("viewer123"), UserLevel::User } };

    // 默认三档：main 1080p / sub 720p / third 360p。
    struct Preset {
        int width;
        int height;
        int bitrate;
        const char *asset;
    };
    static const Preset presets[] = { { 1920, 1080, 4096, "1080p" },
                                      { 1280, 720, 2048, "720p" },
                                      { 640, 360, 512, "360p" } };
    const QString paths[] = { persona->rtspMainPath, persona->rtspSubPath, persona->rtspThirdPath };

    for (int i = 0; i < 3; ++i) {
        MediaProfile p;
        p.token = QStringLiteral("Profile_%1").arg(i + 1);
        p.name = profileNameForStyle(persona->profileNamingStyle, i);
        p.videoEncoder.token = QStringLiteral("VideoEncoder_%1").arg(i + 1);
        p.videoEncoder.name = p.videoEncoder.token;
        p.videoEncoder.width = presets[i].width;
        p.videoEncoder.height = presets[i].height;
        p.videoEncoder.bitrateKbps = presets[i].bitrate;
        p.videoEncoder.frameRate = 15.0;
        p.videoEncoder.govLength = 15;
        p.audioEncoder.token = QStringLiteral("AudioEncoder_%1").arg(i + 1);
        p.audioEncoder.name = p.audioEncoder.token;
        p.mediaAsset = QString::fromLatin1(presets[i].asset);
        p.streamPath = paths[i];
        p.ptzConfigToken = m.ptzNode.configToken;
        // 对讲相关的配置默认不挂 —— 客户端要靠 AddAudioOutputConfiguration 自己绑。
        p.hasAudioOutput = false;
        p.hasAudioDecoder = false;
        m.profiles.append(p);
    }

    // VideoSource 的 bounds 跟主码流分辨率对齐 —— 客户端会拿它算裁剪区域。
    for (MediaProfile &p : m.profiles) {
        p.videoSource.boundsWidth = m.profiles.first().videoEncoder.width;
        p.videoSource.boundsHeight = m.profiles.first().videoEncoder.height;
    }
    return m;
}

} // namespace onvifsim
