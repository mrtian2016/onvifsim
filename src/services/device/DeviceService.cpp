#include "services/device/DeviceService.h"

#include "core/LogBus.h"
#include "core/VirtualCamera.h"
#include "events/EventEngine.h"
#include "soap/Dispatcher.h"
#include "soap/Namespaces.h"
#include "soap/XmlNode.h"

#include <QtCore/QDateTime>
#include <QtCore/QStringList>
#include <QtCore/QTimer>

namespace onvifsim {
namespace {

// 对外宣称的用户数上限，CreateUsers 按它拒绝。
constexpr int kMaxUsers = 16;

// 继电器 token。事件层的 tns1:Device/Trigger/Relay 用的是同一串
// （见 events/EventTypes.cpp），DeviceIO 服务也必须一致，
// 否则客户端拿 GetRelayOutputs 的 token 去对事件的 Source 就对不上。
const char *kRelayToken = iotoken::Relay;

QHostAddress peerOf(const SoapContext &ctx)
{
    // 单测里没有 HttpRequest；空地址会让 advertisedHost() 退回绑定地址，正好。
    return ctx.http ? ctx.http->peerAddress : QHostAddress();
}

// tt:DateTime。顺序是 schema 定死的：先 Time 后 Date，写反了 zeep 直接报错。
void writeDateTime(XmlWriter &w, const QString &qname, const QDateTime &utc)
{
    XmlWriter::Scope scope(w, qname);
    {
        XmlWriter::Scope t(w, QStringLiteral("tt:Time"));
        w.element(QStringLiteral("tt:Hour"), utc.time().hour());
        w.element(QStringLiteral("tt:Minute"), utc.time().minute());
        w.element(QStringLiteral("tt:Second"), utc.time().second());
    }
    {
        XmlWriter::Scope d(w, QStringLiteral("tt:Date"));
        w.element(QStringLiteral("tt:Year"), utc.date().year());
        w.element(QStringLiteral("tt:Month"), utc.date().month());
        w.element(QStringLiteral("tt:Day"), utc.date().day());
    }
}

// 某个服务是否真的装上了。按 dispatcher 里实际注册的服务判断，
// 而不是照 CameraModel 的开关猜 —— persona 也能否掉 Media2。
bool hasService(const SoapContext &ctx, const char *name)
{
    SoapDispatcher *d = ctx.camera->dispatcher();
    return d && d->serviceByName(QString::fromLatin1(name)) != nullptr;
}

QString xaddrOf(const SoapContext &ctx, const char *name)
{
    return ctx.camera->serviceXAddr(QString::fromLatin1(name), peerOf(ctx));
}

// scope 是不是出厂固定项。规范里 SetScopes / RemoveScopes 只能动 Configurable 的，
// type / hardware / Profile 三类是设备身份的一部分，改了客户端就认不出这是台相机。
bool isFixedScope(const QString &scope)
{
    return scope.contains(QLatin1String("/type/"))
           || scope.contains(QLatin1String("/hardware/"))
           || scope.contains(QLatin1String("/Profile/"));
}

// 请求里重复出现的同名参数（Scopes / ScopeItem / Username / User …）一次取全。
QStringList repeatedText(const SoapContext &ctx, const QString &name)
{
    QStringList out;
    if (!ctx.body)
        return out;
    for (const XmlNode *n : ctx.body->childrenNamed(name)) {
        const QString t = n->text.trimmed();
        if (!t.isEmpty())
            out.append(t);
    }
    return out;
}

QString macAddressOf(const CameraModel &model)
{
    if (!model.macAddress.isEmpty())
        return model.macAddress;
    // 没配就按相机 id 生成一个稳定的本地管理地址（首字节 0x02 = locally administered）。
    // 多台相机报同一个 MAC 会让按 MAC 去重的客户端把它们并成一台。
    const quint32 h = static_cast<quint32>(qHash(model.id, 0));
    return QStringLiteral("02:00:%1:%2:%3:%4")
        .arg((h >> 24) & 0xFFu, 2, 16, QLatin1Char('0'))
        .arg((h >> 16) & 0xFFu, 2, 16, QLatin1Char('0'))
        .arg((h >> 8) & 0xFFu, 2, 16, QLatin1Char('0'))
        .arg(h & 0xFFu, 2, 16, QLatin1Char('0'))
        .toUpper();
}

// ---- GetCapabilities 的各大类 ---------------------------------------------
// 元素顺序按 tt:Capabilities 的 sequence：Analytics / Device / Events /
// Imaging / Media / PTZ / Extension。顺序错了严格解析的客户端会整包丢弃。

void writeAnalyticsCategory(SoapContext &ctx)
{
    XmlWriter &w = *ctx.out;
    XmlWriter::Scope s(w, QStringLiteral("tt:Analytics"));
    w.element(QStringLiteral("tt:XAddr"), xaddrOf(ctx, "analytics"));
    w.element(QStringLiteral("tt:RuleSupport"), true);
    w.element(QStringLiteral("tt:AnalyticsModuleSupport"), true);
}

void writeDeviceCategory(SoapContext &ctx)
{
    XmlWriter &w = *ctx.out;
    const CameraModel &model = ctx.camera->model();

    XmlWriter::Scope s(w, QStringLiteral("tt:Device"));
    w.element(QStringLiteral("tt:XAddr"), ctx.camera->deviceServiceXAddr(peerOf(ctx)));
    {
        XmlWriter::Scope net(w, QStringLiteral("tt:Network"));
        w.element(QStringLiteral("tt:IPFilter"), false);
        w.element(QStringLiteral("tt:ZeroConfiguration"), false);
        w.element(QStringLiteral("tt:IPVersion6"), false);
        w.element(QStringLiteral("tt:DynDNS"), false);
    }
    {
        XmlWriter::Scope sys(w, QStringLiteral("tt:System"));
        w.element(QStringLiteral("tt:DiscoveryResolve"), true);
        w.element(QStringLiteral("tt:DiscoveryBye"), true);
        w.element(QStringLiteral("tt:RemoteDiscovery"), false);
        w.element(QStringLiteral("tt:SystemBackup"), false);
        w.element(QStringLiteral("tt:SystemLogging"), true);
        w.element(QStringLiteral("tt:FirmwareUpgrade"), false);
        XmlWriter::Scope ver(w, QStringLiteral("tt:SupportedVersions"));
        w.element(QStringLiteral("tt:Major"), 2);
        w.element(QStringLiteral("tt:Minor"), 6);
    }
    {
        XmlWriter::Scope io(w, QStringLiteral("tt:IO"));
        w.element(QStringLiteral("tt:InputConnectors"), model.hasDigitalInputs ? 1 : 0);
        w.element(QStringLiteral("tt:RelayOutputs"), model.hasRelayOutputs ? 1 : 0);
    }
    {
        XmlWriter::Scope sec(w, QStringLiteral("tt:Security"));
        // 元素名里带点是 ONVIF schema 的原样定义（TLS1.1 / X.509Token），不是笔误。
        w.element(QStringLiteral("tt:TLS1.1"), false);
        w.element(QStringLiteral("tt:TLS1.2"), false);
        w.element(QStringLiteral("tt:OnboardKeyGeneration"), false);
        w.element(QStringLiteral("tt:AccessPolicyConfig"), false);
        w.element(QStringLiteral("tt:X.509Token"), false);
        w.element(QStringLiteral("tt:SAMLToken"), false);
        w.element(QStringLiteral("tt:KerberosToken"), false);
        w.element(QStringLiteral("tt:RELToken"), false);
    }
}

void writeEventsCategory(SoapContext &ctx)
{
    XmlWriter &w = *ctx.out;
    XmlWriter::Scope s(w, QStringLiteral("tt:Events"));
    w.element(QStringLiteral("tt:XAddr"), xaddrOf(ctx, "events"));
    w.element(QStringLiteral("tt:WSSubscriptionPolicySupport"), false);
    // 参照客户端只走 PullPoint，这一位为 false 它会直接判定「不支持事件」。
    w.element(QStringLiteral("tt:WSPullPointSupport"), true);
    w.element(QStringLiteral("tt:WSPausableSubscriptionManagerInterfaceSupport"), false);
}

void writeImagingCategory(SoapContext &ctx)
{
    XmlWriter &w = *ctx.out;
    XmlWriter::Scope s(w, QStringLiteral("tt:Imaging"));
    w.element(QStringLiteral("tt:XAddr"), xaddrOf(ctx, "imaging"));
}

void writeMediaCategory(SoapContext &ctx)
{
    XmlWriter &w = *ctx.out;
    XmlWriter::Scope s(w, QStringLiteral("tt:Media"));
    w.element(QStringLiteral("tt:XAddr"), xaddrOf(ctx, "media"));
    {
        XmlWriter::Scope st(w, QStringLiteral("tt:StreamingCapabilities"));
        w.element(QStringLiteral("tt:RTPMulticast"), false);
        w.element(QStringLiteral("tt:RTP_TCP"), true);
        w.element(QStringLiteral("tt:RTP_RTSP_TCP"), true);
    }
    {
        XmlWriter::Scope ext(w, QStringLiteral("tt:Extension"));
        XmlWriter::Scope prof(w, QStringLiteral("tt:ProfileCapabilities"));
        const int profileCount = static_cast<int>(ctx.camera->model().profiles.size());
        w.element(QStringLiteral("tt:MaximumNumberOfProfiles"), qMax(profileCount, 3));
    }
}

void writePtzCategory(SoapContext &ctx)
{
    XmlWriter &w = *ctx.out;
    XmlWriter::Scope s(w, QStringLiteral("tt:PTZ"));
    w.element(QStringLiteral("tt:XAddr"), xaddrOf(ctx, "ptz"));
}

void writeDeviceIoExtension(SoapContext &ctx)
{
    XmlWriter &w = *ctx.out;
    const CameraModel &model = ctx.camera->model();
    XmlWriter::Scope ext(w, QStringLiteral("tt:Extension"));
    XmlWriter::Scope dio(w, QStringLiteral("tt:DeviceIO"));
    w.element(QStringLiteral("tt:XAddr"), xaddrOf(ctx, "deviceio"));
    w.element(QStringLiteral("tt:VideoSources"), 1);
    w.element(QStringLiteral("tt:VideoOutputs"), 0);
    w.element(QStringLiteral("tt:AudioSources"), 1);
    w.element(QStringLiteral("tt:AudioOutputs"), model.hasAudioBackchannel ? 1 : 0);
    w.element(QStringLiteral("tt:RelayOutputs"), model.hasRelayOutputs ? 1 : 0);
}

// GetSystemLog 的正文。真机吐的是一坨文本，这里直接把 LogBus 里跟本机相关的
// 记录拉出来 —— 比编造几行假日志有用，客户端拿去展示也像回事。
QString buildSystemLog(VirtualCamera *camera, const QString &logType)
{
    LogBus *bus = camera->logBus();
    if (!bus)
        return QStringLiteral("(no log)");

    const bool accessOnly = logType.compare(QLatin1String("Access"), Qt::CaseInsensitive) == 0;
    const QVector<LogRecord> history = bus->history();
    QStringList lines;
    for (int i = history.size() - 1; i >= 0 && lines.size() < 200; --i) {
        const LogRecord &r = history.at(i);
        if (!r.cameraId.isEmpty() && r.cameraId != camera->id())
            continue;
        if (accessOnly && r.category != QLatin1String(logcat::Soap)
            && r.category != QLatin1String(logcat::Http))
            continue;
        lines.prepend(QStringLiteral("%1 %2 %3 %4 %5")
                          .arg(r.timestamp.toString(Qt::ISODate), r.levelName(), r.category,
                               r.peer.isEmpty() ? QStringLiteral("-") : r.peer, r.summary));
    }
    if (lines.isEmpty())
        lines.append(QStringLiteral("%1 Info core - system log is empty")
                         .arg(camera->deviceTimeUtc().toString(Qt::ISODate)));
    return lines.join(QLatin1Char('\n'));
}

} // namespace

// ---------------------------------------------------------------------------

const char *DeviceService::serviceNamespace() const
{
    return ns::Device;
}

const char *DeviceService::serviceName() const
{
    return "device";
}

QString DeviceService::defaultPath() const
{
    // 客户端硬编码这个路径，任何 persona / quirk 都不该改它（A5 只改 host:port）。
    return QStringLiteral("/onvif/device_service");
}

// tds:DeviceServiceCapabilities。GetServiceCapabilities 与 GetServices 的
// IncludeCapability 共用这一段，属性顺序无所谓，元素顺序（Network / Security /
// System / Misc）是 schema 定死的。
void DeviceService::writeServiceCapabilities(SoapContext &ctx) const
{
    XmlWriter &w = *ctx.out;
    XmlWriter::Scope caps(w, QStringLiteral("tds:Capabilities"));
    {
        XmlWriter::Scope net(w, QStringLiteral("tds:Network"));
        w.attr(QStringLiteral("IPFilter"), QStringLiteral("false"));
        w.attr(QStringLiteral("ZeroConfiguration"), QStringLiteral("false"));
        w.attr(QStringLiteral("IPVersion6"), QStringLiteral("false"));
        w.attr(QStringLiteral("DynDNS"), QStringLiteral("false"));
        w.attr(QStringLiteral("Dot11Configuration"), QStringLiteral("false"));
        w.attr(QStringLiteral("HostnameFromDHCP"), QStringLiteral("false"));
        w.attr(QStringLiteral("NTP"), QStringLiteral("1"));
        w.attr(QStringLiteral("DHCPv6"), QStringLiteral("false"));
    }
    {
        XmlWriter::Scope sec(w, QStringLiteral("tds:Security"));
        w.attr(QStringLiteral("TLS1.0"), QStringLiteral("false"));
        w.attr(QStringLiteral("TLS1.1"), QStringLiteral("false"));
        w.attr(QStringLiteral("TLS1.2"), QStringLiteral("false"));
        w.attr(QStringLiteral("OnboardKeyGeneration"), QStringLiteral("false"));
        w.attr(QStringLiteral("AccessPolicyConfig"), QStringLiteral("false"));
        w.attr(QStringLiteral("DefaultAccessPolicy"), QStringLiteral("false"));
        w.attr(QStringLiteral("Dot1X"), QStringLiteral("false"));
        w.attr(QStringLiteral("RemoteUserHandling"), QStringLiteral("false"));
        w.attr(QStringLiteral("X.509Token"), QStringLiteral("false"));
        w.attr(QStringLiteral("SAMLToken"), QStringLiteral("false"));
        w.attr(QStringLiteral("KerberosToken"), QStringLiteral("false"));
        w.attr(QStringLiteral("UsernameToken"), QStringLiteral("true"));
        w.attr(QStringLiteral("HttpDigest"), QStringLiteral("true"));
        w.attr(QStringLiteral("RELToken"), QStringLiteral("false"));
        w.attr(QStringLiteral("MaxUsers"), QString::number(kMaxUsers));
        w.attr(QStringLiteral("MaxUserNameLength"), QStringLiteral("32"));
        w.attr(QStringLiteral("MaxPasswordLength"), QStringLiteral("64"));
    }
    {
        XmlWriter::Scope sys(w, QStringLiteral("tds:System"));
        w.attr(QStringLiteral("DiscoveryResolve"), QStringLiteral("true"));
        w.attr(QStringLiteral("DiscoveryBye"), QStringLiteral("true"));
        w.attr(QStringLiteral("RemoteDiscovery"), QStringLiteral("false"));
        w.attr(QStringLiteral("SystemBackup"), QStringLiteral("false"));
        w.attr(QStringLiteral("SystemLogging"), QStringLiteral("true"));
        w.attr(QStringLiteral("FirmwareUpgrade"), QStringLiteral("false"));
        w.attr(QStringLiteral("HttpFirmwareUpgrade"), QStringLiteral("false"));
        w.attr(QStringLiteral("HttpSystemBackup"), QStringLiteral("false"));
        w.attr(QStringLiteral("HttpSystemLogging"), QStringLiteral("false"));
        w.attr(QStringLiteral("HttpSupportInformation"), QStringLiteral("false"));
    }
}

DeviceService::DeviceService()
    : m_discoveryMode(QStringLiteral("Discoverable"))
{
    m_relayStates.insert(QString::fromLatin1(kRelayToken), false);

    registerIdentityOps();
    registerTimeOps();
    registerCapabilityOps();
    registerScopeOps();
    registerNetworkOps();
    registerUserOps();
    registerSystemOps();
}

// ---- 身份 ----
void DeviceService::registerIdentityOps()
{
    cameraOp("GetDeviceInformation", AuthLevel::User, [](SoapContext &ctx) {
        const CameraModel &m = ctx.camera->model();

        // quirk A10：真机常缺 HardwareId 或 SerialNumber，按这些字串挑厂商适配器的
        // 客户端会整条链路落空。参数 fields 是逗号分隔的字段名。
        QStringList omitted;
        if (ctx.quirks().isEnabled(QuirkId::DeviceInfoMissingFields)) {
            const QString spec = ctx.quirks().paramString(QuirkId::DeviceInfoMissingFields,
                                                          QStringLiteral("fields"));
            for (const QString &f : spec.split(QLatin1Char(','), Qt::SkipEmptyParts))
                omitted.append(f.trimmed().toLower());
        }
        auto keep = [&omitted](const char *field) {
            return !omitted.contains(QString::fromLatin1(field).toLower());
        };

        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tds:GetDeviceInformationResponse"));
        if (keep("Manufacturer"))
            ctx.out->element(QStringLiteral("tds:Manufacturer"), m.manufacturer);
        if (keep("Model"))
            ctx.out->element(QStringLiteral("tds:Model"), m.model);
        if (keep("FirmwareVersion"))
            ctx.out->element(QStringLiteral("tds:FirmwareVersion"), m.firmwareVersion);
        if (keep("SerialNumber"))
            ctx.out->element(QStringLiteral("tds:SerialNumber"), m.serialNumber);
        if (keep("HardwareId"))
            ctx.out->element(QStringLiteral("tds:HardwareId"), m.hardwareId);
    });

    cameraOp("GetEndpointReference", AuthLevel::User, [](SoapContext &ctx) {
        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tds:GetEndpointReferenceResponse"));
        // 与 WS-Discovery 的 EndpointReference 必须是同一串，客户端拿它去重。
        ctx.out->element(QStringLiteral("tds:GUID"), ctx.camera->model().endpointReference);
    });

    // WSDL 从客户端本地磁盘读，没人真会来拉；但这是 PRE_AUTH 操作，要能匿名答。
    cameraOp("GetWsdlUrl", AuthLevel::PreAuth, [](SoapContext &ctx) {
        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tds:GetWsdlUrlResponse"));
        ctx.out->element(QStringLiteral("tds:WsdlUrl"),
                         ctx.serviceUri(QStringLiteral("/onvif/wsdl")));
    });

}


void DeviceService::registerTimeOps()
{
    // ---- 时间 ------------------------------------------------------------

    cameraOp("GetSystemDateAndTime", AuthLevel::PreAuth, [](SoapContext &ctx) {
        // 一律 UTC：deviceTimeUtc() 已经叠了 quirk A8 的 clock_skew，
        // 客户端不做时钟补偿时正好被这里拨偏的时间打成 401。
        const QDateTime now = ctx.camera->deviceTimeUtc();

        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tds:GetSystemDateAndTimeResponse"));
        XmlWriter::Scope sdt(*ctx.out, QStringLiteral("tds:SystemDateAndTime"));
        ctx.out->element(QStringLiteral("tt:DateTimeType"), QStringLiteral("NTP"));
        ctx.out->element(QStringLiteral("tt:DaylightSavings"), false);
        {
            XmlWriter::Scope tz(*ctx.out, QStringLiteral("tt:TimeZone"));
            ctx.out->element(QStringLiteral("tt:TZ"), QStringLiteral("UTC0"));
        }
        writeDateTime(*ctx.out, QStringLiteral("tt:UTCDateTime"), now);
        // 设备时区就是 UTC，本地时间与 UTC 相同 —— 全项目只用 UTC。
        writeDateTime(*ctx.out, QStringLiteral("tt:LocalDateTime"), now);
    });

    cameraOp("SetSystemDateAndTime", AuthLevel::Administrator, [](SoapContext &ctx) {
        const QString type = ctx.arg(QStringLiteral("DateTimeType"), QStringLiteral("NTP"));
        if (type == QLatin1String("Manual") && !ctx.hasArg(QStringLiteral("UTCDateTime"))) {
            ctx.fault(QString::fromLatin1(ter::InvalidArgVal),
                      QStringLiteral("Manual 模式必须带 UTCDateTime"));
            return;
        }
        // 设备时钟由 quirk 的 clock_skew 掌控（那是故障注入的一部分），
        // 这里只接受请求并回 ACK，不把客户端设的时间写进模型 —— 否则一次
        // SetSystemDateAndTime 就能把注入的时钟偏移抹掉。
        ctx.out->emptyElement(QStringLiteral("tds:SetSystemDateAndTimeResponse"));
    });

}

void DeviceService::registerCapabilityOps()
{
    // ---- 能力发现（最关键的两个操作）--------------------------------------

    cameraOp("GetCapabilities", AuthLevel::PreAuth, [](SoapContext &ctx) {
        // 参照客户端只调这一个操作拿全部 XAddr，从不调 GetServices。
        // 少写一个 XAddr，对应的服务在客户端那边就等于不存在。
        QStringList categories;
        if (ctx.body) {
            for (const XmlNode *n : ctx.body->childrenNamed(QStringLiteral("Category")))
                categories.append(n->text.trimmed());
        }
        const bool all = categories.isEmpty()
                         || categories.contains(QStringLiteral("All"), Qt::CaseInsensitive);
        auto wanted = [&](const char *name) {
            return all || categories.contains(QString::fromLatin1(name), Qt::CaseInsensitive);
        };

        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tds:GetCapabilitiesResponse"));
        XmlWriter::Scope caps(*ctx.out, QStringLiteral("tds:Capabilities"));

        if (wanted("Analytics") && hasService(ctx, "analytics"))
            writeAnalyticsCategory(ctx);
        if (wanted("Device"))
            writeDeviceCategory(ctx);
        if (wanted("Events") && hasService(ctx, "events"))
            writeEventsCategory(ctx);
        if (wanted("Imaging") && hasService(ctx, "imaging"))
            writeImagingCategory(ctx);
        if (wanted("Media") && hasService(ctx, "media"))
            writeMediaCategory(ctx);
        if (wanted("PTZ") && hasService(ctx, "ptz"))
            writePtzCategory(ctx);
        // DeviceIO 在 tt:Capabilities 里没有自己的大类，只能挂在 Extension 下。
        if (wanted("Device") && hasService(ctx, "deviceio"))
            writeDeviceIoExtension(ctx);
    });

    cameraOp("GetServices", AuthLevel::PreAuth, [](SoapContext &ctx) {
        const bool includeCapability = ctx.argBool(QStringLiteral("IncludeCapability"), false);
        // 顺序走 dispatcher：quirk A6（Media2 排到 Media 前面）在那里实现。
        const QList<SoapService *> services = ctx.camera->dispatcher()->servicesForAdvertisement();

        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tds:GetServicesResponse"));
        for (SoapService *svc : services) {
            XmlWriter::Scope one(*ctx.out, QStringLiteral("tds:Service"));
            ctx.out->element(QStringLiteral("tds:Namespace"),
                             QString::fromLatin1(svc->serviceNamespace()));
            ctx.out->element(QStringLiteral("tds:XAddr"), xaddrOf(ctx, svc->serviceName()));
            if (includeCapability) {
                // 外层 tds:Capabilities 是 GetServices 的壳，里层由各服务自己写
                // （device 写 tds:Capabilities，media 写 trt:Capabilities …）。
                XmlWriter::Scope wrap(*ctx.out, QStringLiteral("tds:Capabilities"));
                svc->writeServiceCapabilities(ctx);
            }
            XmlWriter::Scope ver(*ctx.out, QStringLiteral("tds:Version"));
            ctx.out->element(QStringLiteral("tt:Major"), svc->versionMajor());
            ctx.out->element(QStringLiteral("tt:Minor"), svc->versionMinor());
        }
    });

    cameraOp("GetServiceCapabilities", AuthLevel::PreAuth, [this](SoapContext &ctx) {
        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tds:GetServiceCapabilitiesResponse"));
        writeServiceCapabilities(ctx);
    });

}

void DeviceService::registerScopeOps()
{
    // ---- Scopes ----------------------------------------------------------

    cameraOp("GetScopes", AuthLevel::User, [](SoapContext &ctx) {
        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tds:GetScopesResponse"));
        for (const QString &scope : ctx.camera->model().scopes) {
            XmlWriter::Scope one(*ctx.out, QStringLiteral("tds:Scopes"));
            ctx.out->element(QStringLiteral("tt:ScopeDef"),
                             isFixedScope(scope) ? QStringLiteral("Fixed")
                                                 : QStringLiteral("Configurable"));
            ctx.out->element(QStringLiteral("tt:ScopeItem"), scope);
        }
    });

    cameraOp("SetScopes", AuthLevel::Administrator, [](SoapContext &ctx) {
        // 规范：SetScopes 替换掉全部 Configurable scope，Fixed 的原样保留。
        // 参数元素名是 Scopes（Add/RemoveScopes 用的才是 ScopeItem），两个都收。
        QStringList requested = repeatedText(ctx, QStringLiteral("Scopes"));
        requested.append(repeatedText(ctx, QStringLiteral("ScopeItem")));

        CameraModel &model = ctx.camera->mutableModel();
        QStringList next;
        for (const QString &scope : model.scopes) {
            if (isFixedScope(scope))
                next.append(scope);
        }
        for (const QString &scope : requested) {
            if (!next.contains(scope))
                next.append(scope);
        }
        model.scopes = next;
        ctx.camera->applyModelChanges();
        ctx.out->emptyElement(QStringLiteral("tds:SetScopesResponse"));
    });

    cameraOp("AddScopes", AuthLevel::Administrator, [](SoapContext &ctx) {
        QStringList requested = repeatedText(ctx, QStringLiteral("ScopeItem"));
        requested.append(repeatedText(ctx, QStringLiteral("Scopes")));
        if (requested.isEmpty()) {
            ctx.fault(QString::fromLatin1(ter::InvalidArgVal),
                      QStringLiteral("AddScopes 至少要给一条 ScopeItem"));
            return;
        }
        CameraModel &model = ctx.camera->mutableModel();
        for (const QString &scope : requested) {
            if (!model.scopes.contains(scope))
                model.scopes.append(scope);
        }
        ctx.camera->applyModelChanges();
        ctx.out->emptyElement(QStringLiteral("tds:AddScopesResponse"));
    });

    cameraOp("RemoveScopes", AuthLevel::Administrator, [](SoapContext &ctx) {
        const QStringList requested = repeatedText(ctx, QStringLiteral("ScopeItem"));
        CameraModel &model = ctx.camera->mutableModel();

        QStringList removed;
        for (const QString &scope : requested) {
            if (isFixedScope(scope)) {
                // 出厂固定项删不掉，规范要求报 ter:FixedScope。
                ctx.fault(QString::fromLatin1(ter::FixedScope),
                          QStringLiteral("Scope %1 是固定项，不可删除").arg(scope));
                return;
            }
            if (model.scopes.removeAll(scope) > 0)
                removed.append(scope);
        }
        ctx.camera->applyModelChanges();

        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tds:RemoveScopesResponse"));
        for (const QString &scope : removed)
            ctx.out->element(QStringLiteral("tds:ScopeItem"), scope);
    });

}

void DeviceService::registerNetworkOps()
{
    // ---- 网络 ------------------------------------------------------------

    cameraOp("GetHostname", AuthLevel::User, [](SoapContext &ctx) {
        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tds:GetHostnameResponse"));
        XmlWriter::Scope info(*ctx.out, QStringLiteral("tds:HostnameInformation"));
        ctx.out->element(QStringLiteral("tt:FromDHCP"), false);
        ctx.out->element(QStringLiteral("tt:Name"), ctx.camera->model().hostname);
    });

    cameraOp("SetHostname", AuthLevel::Administrator, [](SoapContext &ctx) {
        const QString name = ctx.arg(QStringLiteral("Name")).trimmed();
        if (name.isEmpty()) {
            ctx.fault(QString::fromLatin1(ter::InvalidArgVal), QStringLiteral("Name 不能为空"));
            return;
        }
        ctx.camera->mutableModel().hostname = name;
        ctx.camera->applyModelChanges();
        ctx.out->emptyElement(QStringLiteral("tds:SetHostnameResponse"));
    });

    cameraOp("GetNetworkInterfaces", AuthLevel::User, [](SoapContext &ctx) {
        const CameraModel &model = ctx.camera->model();

        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tds:GetNetworkInterfacesResponse"));
        XmlWriter::Scope iface(*ctx.out, QStringLiteral("tds:NetworkInterfaces"));
        ctx.out->attr(QStringLiteral("token"), QStringLiteral("eth0"));
        ctx.out->element(QStringLiteral("tt:Enabled"), true);
        {
            XmlWriter::Scope info(*ctx.out, QStringLiteral("tt:Info"));
            ctx.out->element(QStringLiteral("tt:Name"), QStringLiteral("eth0"));
            ctx.out->element(QStringLiteral("tt:HwAddress"), macAddressOf(model));
            ctx.out->element(QStringLiteral("tt:MTU"), 1500);
        }
        {
            XmlWriter::Scope v4(*ctx.out, QStringLiteral("tt:IPv4"));
            ctx.out->element(QStringLiteral("tt:Enabled"), true);
            XmlWriter::Scope cfg(*ctx.out, QStringLiteral("tt:Config"));
            {
                XmlWriter::Scope manual(*ctx.out, QStringLiteral("tt:Manual"));
                // 报的是对外可达的那个地址，跟 XAddr 保持一致：
                // 两边不一致的相机在客户端那儿会被当成「多网口设备」另走一套逻辑。
                ctx.out->element(QStringLiteral("tt:Address"),
                                 ctx.camera->advertisedHost(peerOf(ctx)));
                ctx.out->element(QStringLiteral("tt:PrefixLength"), 24);
            }
            ctx.out->element(QStringLiteral("tt:DHCP"), false);
        }
    });

    cameraOp("GetNetworkProtocols", AuthLevel::User, [](SoapContext &ctx) {
        // 端口用对外宣称值：quirk A5 打开时这里也要跟着撒谎，才自洽。
        const quint16 httpPort = ctx.camera->advertisedHttpPort();
        const quint16 rtspPort = ctx.camera->advertisedRtspPort();

        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tds:GetNetworkProtocolsResponse"));
        {
            XmlWriter::Scope p(*ctx.out, QStringLiteral("tds:NetworkProtocols"));
            ctx.out->element(QStringLiteral("tt:Name"), QStringLiteral("HTTP"));
            ctx.out->element(QStringLiteral("tt:Enabled"), true);
            ctx.out->element(QStringLiteral("tt:Port"), static_cast<int>(httpPort));
        }
        {
            XmlWriter::Scope p(*ctx.out, QStringLiteral("tds:NetworkProtocols"));
            ctx.out->element(QStringLiteral("tt:Name"), QStringLiteral("HTTPS"));
            ctx.out->element(QStringLiteral("tt:Enabled"), false);
            ctx.out->element(QStringLiteral("tt:Port"), 443);
        }
        {
            XmlWriter::Scope p(*ctx.out, QStringLiteral("tds:NetworkProtocols"));
            ctx.out->element(QStringLiteral("tt:Name"), QStringLiteral("RTSP"));
            ctx.out->element(QStringLiteral("tt:Enabled"), true);
            ctx.out->element(QStringLiteral("tt:Port"), static_cast<int>(rtspPort));
        }
    });

    cameraOp("GetDNS", AuthLevel::User, [](SoapContext &ctx) {
        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tds:GetDNSResponse"));
        XmlWriter::Scope info(*ctx.out, QStringLiteral("tds:DNSInformation"));
        ctx.out->element(QStringLiteral("tt:FromDHCP"), false);
        XmlWriter::Scope manual(*ctx.out, QStringLiteral("tt:DNSManual"));
        ctx.out->element(QStringLiteral("tt:Type"), QStringLiteral("IPv4"));
        ctx.out->element(QStringLiteral("tt:IPv4Address"), QStringLiteral("8.8.8.8"));
    });

    cameraOp("GetNTP", AuthLevel::User, [](SoapContext &ctx) {
        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tds:GetNTPResponse"));
        XmlWriter::Scope info(*ctx.out, QStringLiteral("tds:NTPInformation"));
        ctx.out->element(QStringLiteral("tt:FromDHCP"), false);
        XmlWriter::Scope manual(*ctx.out, QStringLiteral("tt:NTPManual"));
        ctx.out->element(QStringLiteral("tt:Type"), QStringLiteral("DNS"));
        ctx.out->element(QStringLiteral("tt:DNSname"), QStringLiteral("pool.ntp.org"));
    });

    cameraOp("GetDiscoveryMode", AuthLevel::User, [this](SoapContext &ctx) {
        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tds:GetDiscoveryModeResponse"));
        ctx.out->element(QStringLiteral("tds:DiscoveryMode"), m_discoveryMode);
    });

    cameraOp("SetDiscoveryMode", AuthLevel::Administrator, [this](SoapContext &ctx) {
        const QString mode = ctx.arg(QStringLiteral("DiscoveryMode")).trimmed();
        if (mode != QLatin1String("Discoverable") && mode != QLatin1String("NonDiscoverable")) {
            ctx.fault(QString::fromLatin1(ter::InvalidArgVal),
                      QStringLiteral("DiscoveryMode 只能是 Discoverable / NonDiscoverable"));
            return;
        }
        m_discoveryMode = mode;
        ctx.out->emptyElement(QStringLiteral("tds:SetDiscoveryModeResponse"));
    });

}

void DeviceService::registerUserOps()
{
    // ---- 用户表 ----------------------------------------------------------

    cameraOp("GetUsers", AuthLevel::User, [](SoapContext &ctx) {
        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tds:GetUsersResponse"));
        for (const User &u : ctx.camera->model().users) {
            XmlWriter::Scope one(*ctx.out, QStringLiteral("tds:User"));
            ctx.out->element(QStringLiteral("tt:Username"), u.username);
            // 口令绝不回吐：规范明确要求 GetUsers 的 Password 留空。
            ctx.out->element(QStringLiteral("tt:UserLevel"), CameraModel::userLevelName(u.level));
        }
    });

    cameraOp("CreateUsers", AuthLevel::Administrator, [](SoapContext &ctx) {
        if (!ctx.body) {
            ctx.fault(QString::fromLatin1(ter::InvalidArgs), QStringLiteral("缺少 User"));
            return;
        }
        const QVector<const XmlNode *> nodes = ctx.body->childrenNamed(QStringLiteral("User"));
        if (nodes.isEmpty()) {
            ctx.fault(QString::fromLatin1(ter::InvalidArgs), QStringLiteral("缺少 User"));
            return;
        }

        CameraModel &model = ctx.camera->mutableModel();
        for (const XmlNode *node : nodes) {
            // GetServiceCapabilities 对外宣称 MaxUsers=16，那就得真的卡住 ——
            // 宣称一个上限却无限接受，客户端照着上限做的容量规划全是空的。
            if (model.users.size() >= kMaxUsers) {
                ctx.fault(QString::fromLatin1(ter::TooManyUsers),
                          QStringLiteral("用户数已达上限 %1").arg(kMaxUsers));
                return;
            }
            const QString username = node->childText(QStringLiteral("Username")).trimmed();
            if (username.isEmpty()) {
                ctx.fault(QString::fromLatin1(ter::InvalidArgVal),
                          QStringLiteral("Username 不能为空"));
                return;
            }
            if (model.findUser(username)) {
                ctx.fault(QString::fromLatin1(ter::UsernameClash),
                          QStringLiteral("用户 %1 已存在").arg(username));
                return;
            }
            bool levelOk = false;
            const QString levelName = node->childText(QStringLiteral("UserLevel"),
                                                      QStringLiteral("User"));
            const UserLevel level = CameraModel::userLevelFromName(levelName, &levelOk);
            if (!levelOk) {
                ctx.fault(QString::fromLatin1(ter::InvalidArgVal),
                          QStringLiteral("未知的 UserLevel：%1").arg(levelName));
                return;
            }
            model.users.append(User{ username, node->childText(QStringLiteral("Password")),
                                     level });
        }
        ctx.camera->applyModelChanges();
        ctx.out->emptyElement(QStringLiteral("tds:CreateUsersResponse"));
    });

    cameraOp("DeleteUsers", AuthLevel::Administrator, [](SoapContext &ctx) {
        const QStringList names = repeatedText(ctx, QStringLiteral("Username"));
        if (names.isEmpty()) {
            ctx.fault(QString::fromLatin1(ter::InvalidArgs), QStringLiteral("缺少 Username"));
            return;
        }

        CameraModel &model = ctx.camera->mutableModel();
        for (const QString &name : names) {
            const User *u = model.findUser(name);
            if (!u) {
                ctx.fault(QString::fromLatin1(ter::UsernameMissing),
                          QStringLiteral("用户 %1 不存在").arg(name));
                return;
            }
            // 删光管理员就再也改不回来了，真机一律拒绝这一步。
            if (u->level == UserLevel::Administrator) {
                int admins = 0;
                for (const User &other : model.users) {
                    if (other.level == UserLevel::Administrator)
                        ++admins;
                }
                if (admins <= 1) {
                    ctx.fault(QString::fromLatin1(ter::OperationProhibited),
                              QStringLiteral("不能删除最后一个管理员"));
                    return;
                }
            }
            for (int i = 0; i < model.users.size(); ++i) {
                if (model.users.at(i).username == name) {
                    model.users.removeAt(i);
                    break;
                }
            }
        }
        ctx.camera->applyModelChanges();
        ctx.out->emptyElement(QStringLiteral("tds:DeleteUsersResponse"));
    });

    cameraOp("SetUser", AuthLevel::Administrator, [](SoapContext &ctx) {
        if (!ctx.body) {
            ctx.fault(QString::fromLatin1(ter::InvalidArgs), QStringLiteral("缺少 User"));
            return;
        }
        const QVector<const XmlNode *> nodes = ctx.body->childrenNamed(QStringLiteral("User"));
        if (nodes.isEmpty()) {
            ctx.fault(QString::fromLatin1(ter::InvalidArgs), QStringLiteral("缺少 User"));
            return;
        }

        CameraModel &model = ctx.camera->mutableModel();
        for (const XmlNode *node : nodes) {
            const QString username = node->childText(QStringLiteral("Username")).trimmed();
            User *target = nullptr;
            for (User &u : model.users) {
                if (u.username == username) {
                    target = &u;
                    break;
                }
            }
            if (!target) {
                ctx.fault(QString::fromLatin1(ter::UsernameMissing),
                          QStringLiteral("用户 %1 不存在").arg(username));
                return;
            }
            // Password / UserLevel 都是可选的：只给了 UserLevel 就只改级别，
            // 别把口令顺手清空 —— 客户端会立刻全线 401。
            if (node->hasChild(QStringLiteral("Password")))
                target->password = node->childText(QStringLiteral("Password"));
            if (node->hasChild(QStringLiteral("UserLevel"))) {
                bool levelOk = false;
                const QString levelName = node->childText(QStringLiteral("UserLevel"));
                const UserLevel level = CameraModel::userLevelFromName(levelName, &levelOk);
                if (!levelOk) {
                    ctx.fault(QString::fromLatin1(ter::InvalidArgVal),
                              QStringLiteral("未知的 UserLevel：%1").arg(levelName));
                    return;
                }
                target->level = level;
            }
        }
        ctx.camera->applyModelChanges();
        ctx.out->emptyElement(QStringLiteral("tds:SetUserResponse"));
    });

}

void DeviceService::registerSystemOps()
{
    // ---- 继电器 / 系统 ----------------------------------------------------

    cameraOp("GetRelayOutputs", AuthLevel::User, [this](SoapContext &ctx) {
        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tds:GetRelayOutputsResponse"));
        if (!ctx.camera->model().hasRelayOutputs)
            return;   // 空响应即「没有继电器」，比报错更像真机
        for (auto it = m_relayStates.constBegin(); it != m_relayStates.constEnd(); ++it) {
            XmlWriter::Scope one(*ctx.out, QStringLiteral("tds:RelayOutputs"));
            ctx.out->attr(QStringLiteral("token"), it.key());
            XmlWriter::Scope props(*ctx.out, QStringLiteral("tt:Properties"));
            ctx.out->element(QStringLiteral("tt:Mode"), QStringLiteral("Bistable"));
            ctx.out->element(QStringLiteral("tt:DelayTime"), QStringLiteral("PT1S"));
            ctx.out->element(QStringLiteral("tt:IdleState"), QStringLiteral("closed"));
        }
    });

    cameraOp("SetRelayOutputState", AuthLevel::Operator, [this](SoapContext &ctx) {
        const QString token = ctx.arg(QStringLiteral("RelayOutputToken"));
        const QString state = ctx.arg(QStringLiteral("LogicalState"));
        if (!m_relayStates.contains(token)) {
            ctx.fault(QString::fromLatin1(ter::NoToken),
                      QStringLiteral("未知的 RelayOutputToken：%1").arg(token));
            return;
        }
        if (state != QLatin1String("active") && state != QLatin1String("inactive")) {
            ctx.fault(QString::fromLatin1(ter::InvalidArgVal),
                      QStringLiteral("LogicalState 只能是 active / inactive"));
            return;
        }
        const bool active = state == QLatin1String("active");
        m_relayStates.insert(token, active);
        // 真的改状态还不够 —— 真机会同时抛一条 tns1:Device/Trigger/Relay，
        // 客户端就是靠它确认继电器动作生效的。topic 串按当前命名风格取（quirk D9）。
        if (EventEngine *events = ctx.camera->events()) {
            const QVector<TopicDef> topics = events->topics();
            if (const TopicDef *def = TopicCatalog::findByKind(topics, EventKind::RelayOutput))
                events->trigger(def->topic, active);
        }
        ctx.out->emptyElement(QStringLiteral("tds:SetRelayOutputStateResponse"));
    });

    cameraOp("GetSystemLog", AuthLevel::Administrator, [](SoapContext &ctx) {
        const QString type = ctx.arg(QStringLiteral("LogType"), QStringLiteral("System"));
        if (type != QLatin1String("System") && type != QLatin1String("Access")) {
            ctx.fault(QString::fromLatin1(ter::InvalidArgVal),
                      QStringLiteral("LogType 只能是 System / Access"));
            return;
        }
        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tds:GetSystemLogResponse"));
        XmlWriter::Scope log(*ctx.out, QStringLiteral("tds:SystemLog"));
        ctx.out->element(QStringLiteral("tt:String"), buildSystemLog(ctx.camera, type));
    });

    cameraOp("SystemReboot", AuthLevel::Administrator, [](SoapContext &ctx) {
        VirtualCamera *camera = ctx.camera;
        const int seconds = ctx.quirks().isEnabled(QuirkId::SystemRebootReal)
                                ? ctx.quirks().paramInt(QuirkId::SystemRebootReal,
                                                        QStringLiteral("seconds"))
                                : 0;

        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tds:SystemRebootResponse"));
        ctx.out->element(QStringLiteral("tds:Message"),
                         seconds > 0 ? QStringLiteral("Rebooting in 1 second")
                                     : QStringLiteral("Reboot acknowledged"));

        if (seconds > 0) {
            // 必须先让这条响应发出去再关端口：goOffline() 立刻 suspend HTTP，
            // 在 handler 里直接调用的话客户端连这句 OK 都收不到，
            // 那就成了「连接被重置」而不是「设备重启」，测不出重连逻辑。
            QTimer::singleShot(500, camera, [camera, seconds] { camera->goOffline(seconds); });
        }
    });
}


namespace services {

SoapService *createDevice()
{
    return new DeviceService;
}

} // namespace services
} // namespace onvifsim
