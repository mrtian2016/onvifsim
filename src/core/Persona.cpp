#include "core/Persona.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace onvifsim {
namespace {

// 内置预设表。设备信息三件套照抄真机上报的字串 —— 客户端就是按这些字串
// 选厂商适配器的，写得不像就命中不了对应分支。
QVector<Persona> buildPersonas()
{
    QVector<Persona> v;

    {
        Persona p;
        p.key = QStringLiteral("generic");
        p.displayName = QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::persona", "标准 ONVIF Generic"));
        p.manufacturer = QStringLiteral("ONVIFSim");
        p.model = QStringLiteral("Virtual Camera");
        p.firmwareVersion = QStringLiteral("1.0.0");
        p.hardwareId = QStringLiteral("ONVIFSIM-HW");
        p.serialPrefix = QStringLiteral("SIM");
        p.rtspMainPath = QStringLiteral("/profile1");
        p.rtspSubPath = QStringLiteral("/profile2");
        p.rtspThirdPath = QStringLiteral("/profile3");
        p.snapshotPath = QStringLiteral("/onvif/snapshot");
        p.topicStyle = QStringLiteral("onvif");
        v.append(p);
    }

    {
        // 海康。Annke / LaView 是贴牌，改 Manufacturer 字串即可复用本预设。
        Persona p;
        p.key = QStringLiteral("hikvision");
        p.displayName = QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::persona", "海康威视 Hikvision"));
        p.manufacturer = QStringLiteral("HIKVISION");
        p.model = QStringLiteral("DS-2CD2143G0-I");
        p.firmwareVersion = QStringLiteral("V5.6.3 build 190923");
        p.hardwareId = QStringLiteral("88");
        p.serialPrefix = QStringLiteral("DS-2CD2143G0-I20190923AAWR");
        p.rtspMainPath = QStringLiteral("/Streaming/Channels/101");
        p.rtspSubPath = QStringLiteral("/Streaming/Channels/102");
        p.rtspThirdPath = QStringLiteral("/Streaming/Channels/103");
        p.snapshotPath = QStringLiteral("/ISAPI/Streaming/channels/101/picture");
        p.topicStyle = QStringLiteral("onvif");
        p.vendorApi = VendorApi::Isapi;
        // 海康 / Axis 双栈固件的真实布局：GetServices 里 Media2 排在 Media 前面。
        p.defaultQuirks = { QuirkId::ServicesMedia2First, QuirkId::TalkbackBusySlot };
        v.append(p);
    }

    {
        // 大华。Amcrest / Lorex / Imou 是贴牌。
        Persona p;
        p.key = QStringLiteral("dahua");
        p.displayName = QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::persona", "大华 Dahua"));
        p.manufacturer = QStringLiteral("Dahua");
        p.model = QStringLiteral("IPC-HDW4433C-A");
        p.firmwareVersion = QStringLiteral("2.800.0000000.16.R");
        p.hardwareId = QStringLiteral("IPC-HDW4433C-A");
        p.serialPrefix = QStringLiteral("4H03D2CPAG");
        p.rtspMainPath = QStringLiteral("/cam/realmonitor?channel=1&subtype=0");
        p.rtspSubPath = QStringLiteral("/cam/realmonitor?channel=1&subtype=1");
        p.rtspThirdPath = QStringLiteral("/cam/realmonitor?channel=1&subtype=2");
        p.snapshotPath = QStringLiteral("/cgi-bin/snapshot.cgi");
        p.topicStyle = QStringLiteral("onvif");
        p.vendorApi = VendorApi::DahuaCgi;
        p.talkbackDualTrack = true;   // 麦克风 recvonly 在前 + 对讲 sendonly 在后
        p.defaultQuirks = { QuirkId::TalkbackDualTrack };
        v.append(p);
    }

    {
        Persona p;
        p.key = QStringLiteral("reolink");
        p.displayName = QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::persona", "Reolink"));
        p.manufacturer = QStringLiteral("Reolink");
        p.model = QStringLiteral("RLC-810A");
        p.firmwareVersion = QStringLiteral("v3.1.0.956_22041215");
        p.hardwareId = QStringLiteral("IPC_523128M8MP");
        p.serialPrefix = QStringLiteral("00000000");
        p.rtspMainPath = QStringLiteral("/h264Preview_01_main");
        p.rtspSubPath = QStringLiteral("/h264Preview_01_sub");
        p.rtspThirdPath = QStringLiteral("/h264Preview_01_ext");
        p.snapshotPath = QStringLiteral("/cgi-bin/api.cgi?cmd=Snap&channel=0");
        // Reolink 自定义的人车宠物检测 topic。
        p.topicStyle = QStringLiteral("reolink");
        p.vendorApi = VendorApi::ReolinkJson;
        v.append(p);
    }

    {
        Persona p;
        p.key = QStringLiteral("vigi");
        p.displayName = QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::persona", "TP-Link VIGI"));
        p.manufacturer = QStringLiteral("TP-LINK");
        p.model = QStringLiteral("VIGI C400HP");
        p.firmwareVersion = QStringLiteral("1.0.11 Build 220526 Rel.55621n");
        p.hardwareId = QStringLiteral("VIGI C400HP 1.0");
        p.serialPrefix = QStringLiteral("22463A");
        p.rtspMainPath = QStringLiteral("/stream1");
        p.rtspSubPath = QStringLiteral("/stream2");
        p.rtspThirdPath = QStringLiteral("/stream3");
        p.snapshotPath = QStringLiteral("/onvif/snapshot");
        p.topicStyle = QStringLiteral("tplink");
        p.vendorApi = VendorApi::VigiJsonRpc;
        p.vendorApiPort = 20443;      // 自签 HTTPS，ONVIF 仍在 80
        p.defaultQuirks = { QuirkId::SelfSignedTls };
        v.append(p);
    }

    {
        // TP-Link TL-IPC 系列。这台机器是本项目里怪癖最多的参照物：
        // XAddr 报 :2020 而实连 80、GetEventProperties 返回非法 XML、
        // PTZ Spaces 为空、订阅端口递增、预置位名百分号编码。
        Persona p;
        p.key = QStringLiteral("tplink");
        p.displayName = QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::persona", "TP-Link TL-IPC"));
        p.manufacturer = QStringLiteral("TP-LINK");
        p.model = QStringLiteral("TL-IPC652P-A4");
        p.firmwareVersion = QStringLiteral("1.0.9 Build 210716 Rel.63037n");
        p.hardwareId = QStringLiteral("TL-IPC652P-A4 1.0");
        p.serialPrefix = QStringLiteral("21A8C7");
        p.devicePath = QStringLiteral("/onvif/device_service");
        p.servicePathPattern = QStringLiteral("/onvif/service");
        p.rtspMainPath = QStringLiteral("/stream1");
        p.rtspSubPath = QStringLiteral("/stream2");
        p.rtspThirdPath = QStringLiteral("/stream3");
        p.snapshotPath = QStringLiteral("/onvif/snapshot");
        p.topicStyle = QStringLiteral("tplink");
        p.hasMedia2 = false;
        p.subscriptionOwnPort = true;
        p.vendorApi = VendorApi::TplinkDs;
        p.defaultQuirks = { QuirkId::XAddrOddPort, QuirkId::PtzSpacesEmpty,
                            QuirkId::EventPropertiesBadXml, QuirkId::SubscriptionPortIncrement,
                            QuirkId::PtzPresetPercentEncoded };
        v.append(p);
    }

    {
        Persona p;
        p.key = QStringLiteral("axis");
        p.displayName = QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::persona", "Axis"));
        p.manufacturer = QStringLiteral("AXIS");
        p.model = QStringLiteral("AXIS M3045-V");
        p.firmwareVersion = QStringLiteral("9.80.3.2");
        p.hardwareId = QStringLiteral("762");
        p.serialPrefix = QStringLiteral("ACCC8E");
        p.rtspMainPath = QStringLiteral("/axis-media/media.amp?videocodec=h264&resolution=1920x1080");
        p.rtspSubPath = QStringLiteral("/axis-media/media.amp?videocodec=h264&resolution=1280x720");
        p.rtspThirdPath = QStringLiteral("/axis-media/media.amp?videocodec=h264&resolution=640x360");
        p.snapshotPath = QStringLiteral("/axis-cgi/jpg/image.cgi");
        p.topicStyle = QStringLiteral("axis");
        p.defaultQuirks = { QuirkId::ServicesMedia2First };
        v.append(p);
    }

    {
        Persona p;
        p.key = QStringLiteral("uniview");
        p.displayName = QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::persona", "宇视 Uniview"));
        p.manufacturer = QStringLiteral("Uniview");
        p.model = QStringLiteral("IPC2124SR3-DPF28");
        p.firmwareVersion = QStringLiteral("IPC_G6103-B0004P22D1904");
        p.hardwareId = QStringLiteral("IPC2124SR3-DPF28");
        p.serialPrefix = QStringLiteral("210235C3");
        p.rtspMainPath = QStringLiteral("/media/video1");
        p.rtspSubPath = QStringLiteral("/media/video2");
        p.rtspThirdPath = QStringLiteral("/media/video3");
        p.snapshotPath = QStringLiteral("/images/snapshot.jpg");
        p.topicStyle = QStringLiteral("onvif");
        v.append(p);
    }

    return v;
}

QVector<Persona> &mutablePersonas()
{
    static QVector<Persona> table = buildPersonas();
    return table;
}

VendorApi vendorApiFromName(const QString &name)
{
    if (name.compare(QLatin1String("isapi"), Qt::CaseInsensitive) == 0)
        return VendorApi::Isapi;
    if (name.compare(QLatin1String("dahua_cgi"), Qt::CaseInsensitive) == 0)
        return VendorApi::DahuaCgi;
    if (name.compare(QLatin1String("reolink_json"), Qt::CaseInsensitive) == 0)
        return VendorApi::ReolinkJson;
    if (name.compare(QLatin1String("vigi"), Qt::CaseInsensitive) == 0)
        return VendorApi::VigiJsonRpc;
    if (name.compare(QLatin1String("tplink_ds"), Qt::CaseInsensitive) == 0)
        return VendorApi::TplinkDs;
    return VendorApi::None;
}

QString vendorApiName(VendorApi api)
{
    switch (api) {
    case VendorApi::Isapi:       return QStringLiteral("isapi");
    case VendorApi::DahuaCgi:    return QStringLiteral("dahua_cgi");
    case VendorApi::ReolinkJson: return QStringLiteral("reolink_json");
    case VendorApi::VigiJsonRpc: return QStringLiteral("vigi");
    case VendorApi::TplinkDs:    return QStringLiteral("tplink_ds");
    case VendorApi::None:        break;
    }
    return QString();
}

// 用 JSON 覆盖预设的某几个字段，未出现的字段保持内置值。
void applyOverride(Persona &p, const QJsonObject &obj)
{
    const auto str = [&obj](const char *key, QString &target) {
        const QJsonValue v = obj.value(QLatin1String(key));
        if (v.isString())
            target = v.toString();
    };
    const auto flag = [&obj](const char *key, bool &target) {
        const QJsonValue v = obj.value(QLatin1String(key));
        if (v.isBool())
            target = v.toBool();
    };

    str("displayName", p.displayName);
    str("manufacturer", p.manufacturer);
    str("model", p.model);
    str("firmwareVersion", p.firmwareVersion);
    str("hardwareId", p.hardwareId);
    str("serialPrefix", p.serialPrefix);
    str("devicePath", p.devicePath);
    str("servicePathPattern", p.servicePathPattern);
    str("rtspMainPath", p.rtspMainPath);
    str("rtspSubPath", p.rtspSubPath);
    str("rtspThirdPath", p.rtspThirdPath);
    str("snapshotPath", p.snapshotPath);
    str("profileNamingStyle", p.profileNamingStyle);
    str("topicStyle", p.topicStyle);
    flag("hasMedia2", p.hasMedia2);
    flag("talkbackDualTrack", p.talkbackDualTrack);
    flag("subscriptionOwnPort", p.subscriptionOwnPort);

    if (obj.contains(QLatin1String("vendorApi")))
        p.vendorApi = vendorApiFromName(obj.value(QLatin1String("vendorApi")).toString());
    if (obj.contains(QLatin1String("vendorApiPort")))
        p.vendorApiPort = static_cast<quint16>(obj.value(QLatin1String("vendorApiPort")).toInt());

    if (obj.value(QLatin1String("extraScopes")).isArray()) {
        p.extraScopes.clear();
        for (const QJsonValue &v : obj.value(QLatin1String("extraScopes")).toArray())
            p.extraScopes.append(v.toString());
    }
    if (obj.value(QLatin1String("defaultQuirks")).isArray()) {
        p.defaultQuirks.clear();
        for (const QJsonValue &v : obj.value(QLatin1String("defaultQuirks")).toArray()) {
            if (const QuirkDef *d = QuirkRegistry::findByKey(v.toString()))
                p.defaultQuirks.append(d->id);
        }
    }
}

} // namespace

QJsonObject Persona::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("key"), key);
    o.insert(QStringLiteral("displayName"), displayName);
    o.insert(QStringLiteral("manufacturer"), manufacturer);
    o.insert(QStringLiteral("model"), model);
    o.insert(QStringLiteral("firmwareVersion"), firmwareVersion);
    o.insert(QStringLiteral("hardwareId"), hardwareId);
    o.insert(QStringLiteral("serialPrefix"), serialPrefix);
    o.insert(QStringLiteral("devicePath"), devicePath);
    o.insert(QStringLiteral("servicePathPattern"), servicePathPattern);
    o.insert(QStringLiteral("rtspMainPath"), rtspMainPath);
    o.insert(QStringLiteral("rtspSubPath"), rtspSubPath);
    o.insert(QStringLiteral("rtspThirdPath"), rtspThirdPath);
    o.insert(QStringLiteral("snapshotPath"), snapshotPath);
    o.insert(QStringLiteral("profileNamingStyle"), profileNamingStyle);
    o.insert(QStringLiteral("topicStyle"), topicStyle);
    o.insert(QStringLiteral("hasMedia2"), hasMedia2);
    o.insert(QStringLiteral("talkbackDualTrack"), talkbackDualTrack);
    o.insert(QStringLiteral("subscriptionOwnPort"), subscriptionOwnPort);
    o.insert(QStringLiteral("vendorApi"), vendorApiName(vendorApi));
    if (vendorApiPort)
        o.insert(QStringLiteral("vendorApiPort"), vendorApiPort);
    if (!extraScopes.isEmpty())
        o.insert(QStringLiteral("extraScopes"), QJsonArray::fromStringList(extraScopes));
    if (!defaultQuirks.isEmpty()) {
        QJsonArray arr;
        for (QuirkId id : defaultQuirks)
            arr.append(QuirkRegistry::def(id).key);
        o.insert(QStringLiteral("defaultQuirks"), arr);
    }
    return o;
}

const QVector<Persona> &PersonaRegistry::all()
{
    return mutablePersonas();
}

const Persona *PersonaRegistry::find(const QString &key)
{
    for (const Persona &p : all()) {
        if (p.key == key)
            return &p;
    }
    return nullptr;
}

const Persona &PersonaRegistry::generic()
{
    return all().first();
}

QStringList PersonaRegistry::keys()
{
    QStringList out;
    for (const Persona &p : all())
        out.append(p.key);
    return out;
}

bool PersonaRegistry::loadOverrides(const QString &directory, QStringList *errors)
{
    QDir dir(directory);
    if (!dir.exists()) {
        if (errors)
            errors->append(QCoreApplication::translate("onvifsim::core", "预设目录不存在：%1").arg(directory));
        return false;
    }

    bool allOk = true;
    const QStringList files = dir.entryList(QStringList() << QStringLiteral("*.json"), QDir::Files);
    for (const QString &fileName : files) {
        QFile f(dir.filePath(fileName));
        if (!f.open(QIODevice::ReadOnly)) {
            if (errors)
                errors->append(QCoreApplication::translate("onvifsim::core", "无法读取 %1：%2").arg(fileName, f.errorString()));
            allOk = false;
            continue;
        }
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            if (errors)
                errors->append(QCoreApplication::translate("onvifsim::core", "%1 不是合法 JSON：%2")
                                   .arg(fileName, parseError.errorString()));
            allOk = false;
            continue;
        }

        const QJsonObject obj = doc.object();
        const QString key = obj.value(QStringLiteral("key")).toString(
            QFileInfo(fileName).completeBaseName());

        QVector<Persona> &table = mutablePersonas();
        bool found = false;
        for (Persona &p : table) {
            if (p.key == key) {
                applyOverride(p, obj);
                found = true;
                break;
            }
        }
        if (!found) {
            // 新预设：从 generic 起手再覆盖，这样没写全的字段有合理默认值。
            Persona p = generic();
            p.key = key;
            p.defaultQuirks.clear();
            applyOverride(p, obj);
            table.append(p);
        }
    }
    return allOk;
}

} // namespace onvifsim
