// Device / DeviceIO / Analytics 三个 SOAP 服务的单元测试。
//
// 不走网络：直接从 dispatcher 里取出服务，手工拼一个 SoapContext 调 handler，
// 再把生成的信封解析回 XmlNode 断言。这样一条用例几毫秒，也不依赖端口。

#include "core/CameraModel.h"
#include "core/LogBus.h"
#include "core/Quirks.h"
#include "core/VirtualCamera.h"
#include "events/EventEngine.h"
#include "events/EventTypes.h"
#include "services/ServiceBase.h"
#include "soap/Dispatcher.h"
#include "soap/Envelope.h"
#include "soap/Namespaces.h"
#include "soap/XmlNode.h"
#include "soap/XmlWriter.h"

#include <QtCore/QMap>
#include <QtCore/QRegularExpression>
#include <QtCore/QScopedPointer>
#include <QtCore/QSet>
#include <QtTest/QtTest>

using namespace onvifsim;

namespace {

// 一次「不走网络」的操作调用结果。
struct Call {
    bool ok = false;        // handler 没产生 Fault，且确实写出了响应元素
    QString faultSubcode;
    QByteArray xml;
    XmlNode response;       // Body 首元素（= xxxResponse）
};

// XAddr 必须是 http://host:port/path 三段齐全的绝对地址：
// 客户端只接管 host:port、保留 path，少了 path 它就拼不出可用的服务地址。
bool looksLikeXAddr(const QString &value)
{
    static const QRegularExpression re(QStringLiteral("^http://[^/:]+:[0-9]+/\\S+$"));
    return re.match(value).hasMatch();
}

QString childText(const XmlNode *parent, const char *name)
{
    if (!parent)
        return QString();
    const XmlNode *n = parent->child(QString::fromLatin1(name));
    return n ? n->text : QString();
}

// 下钻取文本 / 取属性。缺节点时返回空串而不是解引用空指针，
// 断言失败时才好看出「是缺了哪一层」而不是直接段错误。
QString pathText(const XmlNode &node, const char *slashPath)
{
    const XmlNode *n = node.path(QString::fromLatin1(slashPath));
    return n ? n->text : QString();
}

QString attrOf(const XmlNode *node, const char *name)
{
    return node ? node->attribute(QString::fromLatin1(name)) : QString();
}

} // namespace

class TestDeviceService : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void deviceInformation();
    void deviceInformationMissingFields();
    void systemDateAndTimeIsUtc();
    void capabilitiesCarryEveryXAddr();
    void capabilitiesHonourCategoryFilter();
    void servicesMatchDispatcher();
    void servicesIncludeCapability();
    void authLevels();
    void scopesLifecycle();
    void usersLifecycle();
    void hostnameAndNetwork();
    void discoveryMode();
    void relayOutputs();
    void systemLogAndReboot();
    void endpointReferenceAndWsdlUrl();
    void deviceIoTokens();
    void analyticsRulesMatchEventTopics();

private:
    Call invoke(const char *serviceName, const char *op, const QString &argsXml = QString());
    SoapService *service(const char *name) const;

    QScopedPointer<LogBus> m_log;
    QScopedPointer<VirtualCamera> m_camera;
};

void TestDeviceService::initTestCase()
{
    // GetSystemLog 会去读 LogBus；不设全局实例的话它只能走空日志分支。
    m_log.reset(new LogBus);
    LogBus::setGlobal(m_log.data());
    m_log->info(logcat::Core, QStringLiteral("cam1"), QStringLiteral("测试启动"));

    CameraModel model = CameraModel::makeDefault(QStringLiteral("cam1"),
                                                 QStringLiteral("generic"), 0);
    // 固定对外地址与端口，XAddr 才有确定形态；否则会去探本机网卡，
    // 换台机器跑结果就不一样了。192.0.2.0/24 是 RFC 5737 的文档用网段。
    model.advertisedHost = QStringLiteral("192.0.2.10");
    model.httpPort = 8000;
    model.rtspPort = 8554;
    m_camera.reset(new VirtualCamera(model, nullptr));
}

SoapService *TestDeviceService::service(const char *name) const
{
    return m_camera->dispatcher()->serviceByName(QString::fromLatin1(name));
}

Call TestDeviceService::invoke(const char *serviceName, const char *op, const QString &argsXml)
{
    Call result;
    SoapService *svc = service(serviceName);
    if (!svc)
        return result;
    const SoapOperation *operation = svc->findOperation(QString::fromLatin1(op));
    if (!operation)
        return result;

    // 请求侧是测试脚手架，手写没问题；响应侧一律走 XmlWriter。
    const QString header =
        QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                       "<s:Envelope xmlns:s=\"%1\" xmlns:tds=\"%2\" xmlns:tt=\"%3\">"
                       "<s:Body>")
            .arg(QLatin1String(ns::Soap12), QLatin1String(ns::Device), QLatin1String(ns::Tt));
    const QString name = QString::fromLatin1(op);
    const QString request = header + QStringLiteral("<tds:") + name + QLatin1Char('>') + argsXml
                            + QStringLiteral("</tds:") + name
                            + QStringLiteral("></s:Body></s:Envelope>");

    const SoapRequest soapRequest = soap::parseEnvelope(request.toUtf8());

    XmlWriter writer(12);
    writer.declareServicePrefixes();
    writer.startEnvelope();

    SoapContext ctx;
    ctx.camera = m_camera.data();
    ctx.soap = &soapRequest;
    ctx.body = &soapRequest.body;
    ctx.out = &writer;
    operation->handler(ctx);

    if (ctx.hasFault()) {
        result.faultSubcode = ctx.pendingFault().subcode;
        return result;
    }

    writer.endEnvelope();
    result.xml = writer.take();

    QString error;
    const XmlNode root = xml::parse(result.xml, &error);
    const XmlNode *body = root.child(QStringLiteral("Body"));
    if (body && !body->children.isEmpty()) {
        result.response = body->children.first();
        result.ok = true;
    }
    return result;
}

// ---------------------------------------------------------------------------

void TestDeviceService::deviceInformation()
{
    const Call c = invoke("device", "GetDeviceInformation");
    QVERIFY(c.ok);
    QCOMPARE(c.response.name, QStringLiteral("GetDeviceInformationResponse"));

    const CameraModel &m = m_camera->model();
    QCOMPARE(childText(&c.response, "Manufacturer"), m.manufacturer);
    QCOMPARE(childText(&c.response, "Model"), m.model);
    QCOMPARE(childText(&c.response, "FirmwareVersion"), m.firmwareVersion);
    QCOMPARE(childText(&c.response, "SerialNumber"), m.serialNumber);
    QCOMPARE(childText(&c.response, "HardwareId"), m.hardwareId);
    QVERIFY(!m.serialNumber.isEmpty());
}

void TestDeviceService::deviceInformationMissingFields()
{
    // quirk A10：真机常缺 HardwareId / SerialNumber，按这些字串挑厂商适配器的
    // 客户端会整条链路落空。
    Quirks &q = m_camera->mutableQuirks();
    q.setEnabled(QuirkId::DeviceInfoMissingFields, true);
    q.setParam(QuirkId::DeviceInfoMissingFields, QStringLiteral("fields"),
               QStringLiteral("HardwareId, serialnumber"));   // 大小写与空格都要能吃下

    const Call c = invoke("device", "GetDeviceInformation");
    QVERIFY(c.ok);
    QVERIFY(!c.response.hasChild(QStringLiteral("HardwareId")));
    QVERIFY(!c.response.hasChild(QStringLiteral("SerialNumber")));
    QVERIFY(c.response.hasChild(QStringLiteral("Manufacturer")));
    QVERIFY(c.response.hasChild(QStringLiteral("Model")));
    QVERIFY(c.response.hasChild(QStringLiteral("FirmwareVersion")));

    q.setEnabled(QuirkId::DeviceInfoMissingFields, false);
    QVERIFY(invoke("device", "GetDeviceInformation").response.hasChild(
        QStringLiteral("HardwareId")));
}

void TestDeviceService::systemDateAndTimeIsUtc()
{
    const Call c = invoke("device", "GetSystemDateAndTime");
    QVERIFY(c.ok);
    const XmlNode *sdt = c.response.child(QStringLiteral("SystemDateAndTime"));
    QVERIFY(sdt);
    QCOMPARE(childText(sdt, "DateTimeType"), QStringLiteral("NTP"));

    const XmlNode *utc = sdt->child(QStringLiteral("UTCDateTime"));
    QVERIFY(utc);
    const XmlNode *date = utc->child(QStringLiteral("Date"));
    const XmlNode *time = utc->child(QStringLiteral("Time"));
    QVERIFY(date);
    QVERIFY(time);

    const QDateTime now = m_camera->deviceTimeUtc();
    QCOMPARE(date->childInt(QStringLiteral("Year")), now.date().year());
    QCOMPARE(date->childInt(QStringLiteral("Month")), now.date().month());
    QCOMPARE(date->childInt(QStringLiteral("Day")), now.date().day());
    QCOMPARE(time->childInt(QStringLiteral("Hour")), now.time().hour());

    // 全项目只用 UTC，LocalDateTime 与 UTCDateTime 必须一致。
    const XmlNode *local = sdt->child(QStringLiteral("LocalDateTime"));
    QVERIFY(local);
    QCOMPARE(pathText(*local, "Date/Year"), childText(date, "Year"));
}

void TestDeviceService::capabilitiesCarryEveryXAddr()
{
    // 参照客户端只调这一个操作拿全部 XAddr，从不调 GetServices。
    const Call c = invoke("device", "GetCapabilities",
                          QStringLiteral("<tds:Category>All</tds:Category>"));
    QVERIFY(c.ok);
    const XmlNode *caps = c.response.child(QStringLiteral("Capabilities"));
    QVERIFY(caps);

    const char *categories[] = { "Device", "Media", "PTZ", "Imaging", "Events", "Analytics" };
    for (const char *category : categories) {
        const XmlNode *node = caps->child(QString::fromLatin1(category));
        QVERIFY2(node, category);
        const QString xaddr = childText(node, "XAddr");
        QVERIFY2(looksLikeXAddr(xaddr), qPrintable(QStringLiteral("%1 → %2")
                                                       .arg(QLatin1String(category), xaddr)));
        QVERIFY(xaddr.startsWith(QStringLiteral("http://192.0.2.10:8000/")));
    }

    // device_service 的路径是客户端硬编码的，任何情况下都不能变。
    QCOMPARE(childText(caps->child(QStringLiteral("Device")), "XAddr"),
             QStringLiteral("http://192.0.2.10:8000/onvif/device_service"));
    QCOMPARE(childText(caps->child(QStringLiteral("Media")), "XAddr"),
             QStringLiteral("http://192.0.2.10:8000/onvif/media_service"));

    // 事件那一位为 false 的话，只走 PullPoint 的客户端会直接判定「不支持事件」。
    QCOMPARE(childText(caps->child(QStringLiteral("Events")), "WSPullPointSupport"),
             QStringLiteral("true"));
    QVERIFY(caps->path(QStringLiteral("Media/StreamingCapabilities/RTP_RTSP_TCP")));
    QVERIFY(caps->path(QStringLiteral("Device/System/SystemLogging")));
    QVERIFY(caps->path(QStringLiteral("Analytics/RuleSupport")));

    // DeviceIO 在 tt:Capabilities 里没有自己的大类，只能挂 Extension 下。
    const XmlNode *deviceIo = caps->path(QStringLiteral("Extension/DeviceIO"));
    QVERIFY(deviceIo);
    QVERIFY(looksLikeXAddr(childText(deviceIo, "XAddr")));
}

void TestDeviceService::capabilitiesHonourCategoryFilter()
{
    const Call c = invoke("device", "GetCapabilities",
                          QStringLiteral("<tds:Category>Media</tds:Category>"));
    QVERIFY(c.ok);
    const XmlNode *caps = c.response.child(QStringLiteral("Capabilities"));
    QVERIFY(caps);
    QVERIFY(caps->hasChild(QStringLiteral("Media")));
    QVERIFY(!caps->hasChild(QStringLiteral("Device")));
    QVERIFY(!caps->hasChild(QStringLiteral("PTZ")));
    QVERIFY(!caps->hasChild(QStringLiteral("Extension")));

    // 一个 Category 都不给时按 All 处理（真机与规范都这么做）。
    const Call all = invoke("device", "GetCapabilities");
    QVERIFY(all.ok);
    QVERIFY(all.response.path(QStringLiteral("Capabilities/Device/XAddr")));
}

void TestDeviceService::servicesMatchDispatcher()
{
    const Call c = invoke("device", "GetServices");
    QVERIFY(c.ok);
    const QVector<const XmlNode *> entries =
        c.response.childrenNamed(QStringLiteral("Service"));
    QCOMPARE(entries.size(), m_camera->dispatcher()->services().size());

    QSet<QString> namespaces;
    for (const XmlNode *entry : entries) {
        const QString uri = childText(entry, "Namespace");
        QVERIFY(!uri.isEmpty());
        namespaces.insert(uri);
        QVERIFY(looksLikeXAddr(childText(entry, "XAddr")));
        const XmlNode *version = entry->child(QStringLiteral("Version"));
        QVERIFY(version);
        QVERIFY(version->childInt(QStringLiteral("Major")) >= 2);
    }

    // Media(ver10) 与 Media2(ver20) 的命名空间都含 "/media/"，必须两条都在，
    // 而且是各自完整的带版本串 —— 客户端就是按这个区分的（A6）。
    QVERIFY(namespaces.contains(QLatin1String(ns::Device)));
    QVERIFY(namespaces.contains(QLatin1String(ns::Media)));
    QVERIFY(namespaces.contains(QLatin1String(ns::Media2)));
    QVERIFY(namespaces.contains(QLatin1String(ns::Ptz)));
    QVERIFY(namespaces.contains(QLatin1String(ns::Imaging)));
    QVERIFY(namespaces.contains(QLatin1String(ns::Events)));
    QVERIFY(namespaces.contains(QLatin1String(ns::Analytics)));
    QVERIFY(namespaces.contains(QLatin1String(ns::DeviceIo)));

    // 顺序由 dispatcher 决定（quirk A6 在那里实现），Device 服务照抄即可。
    const QList<SoapService *> expected = m_camera->dispatcher()->servicesForAdvertisement();
    for (int i = 0; i < expected.size(); ++i) {
        QCOMPARE(childText(entries.at(i), "Namespace"),
                 QString::fromLatin1(expected.at(i)->serviceNamespace()));
    }
}

void TestDeviceService::servicesIncludeCapability()
{
    const Call off = invoke("device", "GetServices",
                            QStringLiteral("<tds:IncludeCapability>false"
                                           "</tds:IncludeCapability>"));
    QVERIFY(off.ok);
    for (const XmlNode *entry : off.response.childrenNamed(QStringLiteral("Service")))
        QVERIFY(!entry->hasChild(QStringLiteral("Capabilities")));

    const Call on = invoke("device", "GetServices",
                           QStringLiteral("<tds:IncludeCapability>true</tds:IncludeCapability>"));
    QVERIFY(on.ok);
    const XmlNode *deviceEntry = nullptr;
    for (const XmlNode *entry : on.response.childrenNamed(QStringLiteral("Service"))) {
        QVERIFY(entry->hasChild(QStringLiteral("Capabilities")));
        if (childText(entry, "Namespace") == QLatin1String(ns::Device))
            deviceEntry = entry;
    }
    QVERIFY(deviceEntry);
    // 外层 tds:Capabilities 是 GetServices 的壳，里层是服务自己的能力元素。
    QVERIFY(deviceEntry->path(QStringLiteral("Capabilities/Capabilities/Network")));
    QCOMPARE(attrOf(deviceEntry->path(QStringLiteral("Capabilities/Capabilities/Security")),
                    "UsernameToken"),
             QStringLiteral("true"));

    // GetServiceCapabilities 与 GetServices 共用同一段，不能各写一份。
    const Call caps = invoke("device", "GetServiceCapabilities");
    QVERIFY(caps.ok);
    QVERIFY(caps.response.path(QStringLiteral("Capabilities/System")));
}

void TestDeviceService::authLevels()
{
    struct Expectation {
        const char *service;
        const char *op;
        AuthLevel level;
    };
    // 规范允许匿名调用的四个 PRE_AUTH 操作，以及写操作必须是管理员。
    static const Expectation expectations[] = {
        { "device", "GetSystemDateAndTime", AuthLevel::PreAuth },
        { "device", "GetCapabilities", AuthLevel::PreAuth },
        { "device", "GetServices", AuthLevel::PreAuth },
        { "device", "GetWsdlUrl", AuthLevel::PreAuth },
        { "device", "GetDeviceInformation", AuthLevel::User },
        { "device", "GetUsers", AuthLevel::User },
        { "device", "GetScopes", AuthLevel::User },
        { "device", "SetScopes", AuthLevel::Administrator },
        { "device", "AddScopes", AuthLevel::Administrator },
        { "device", "RemoveScopes", AuthLevel::Administrator },
        { "device", "SetHostname", AuthLevel::Administrator },
        { "device", "SetDiscoveryMode", AuthLevel::Administrator },
        { "device", "CreateUsers", AuthLevel::Administrator },
        { "device", "DeleteUsers", AuthLevel::Administrator },
        { "device", "SetUser", AuthLevel::Administrator },
        { "device", "SetSystemDateAndTime", AuthLevel::Administrator },
        { "device", "GetSystemLog", AuthLevel::Administrator },
        { "device", "SystemReboot", AuthLevel::Administrator },
        { "deviceio", "GetAudioOutputs", AuthLevel::User },
        { "analytics", "GetRules", AuthLevel::User },
    };
    for (const Expectation &e : expectations) {
        SoapService *svc = service(e.service);
        QVERIFY2(svc, e.service);
        const SoapOperation *op = svc->findOperation(QString::fromLatin1(e.op));
        QVERIFY2(op, e.op);
        QVERIFY2(op->auth == e.level, e.op);
    }
}

void TestDeviceService::scopesLifecycle()
{
    const Call initial = invoke("device", "GetScopes");
    QVERIFY(initial.ok);
    const QVector<const XmlNode *> scopes = initial.response.childrenNamed(
        QStringLiteral("Scopes"));
    QCOMPARE(scopes.size(), m_camera->model().scopes.size());

    int fixed = 0;
    int configurable = 0;
    for (const XmlNode *s : scopes) {
        const QString def = childText(s, "ScopeDef");
        QVERIFY(!childText(s, "ScopeItem").isEmpty());
        if (def == QLatin1String("Fixed"))
            ++fixed;
        else if (def == QLatin1String("Configurable"))
            ++configurable;
    }
    QVERIFY(fixed > 0);
    QVERIFY(configurable > 0);

    // AddScopes 要真的改到模型（发现层的 Scopes 直接读它）。
    const QString office = QStringLiteral("onvif://www.onvif.org/location/office");
    QVERIFY(invoke("device", "AddScopes",
                   QStringLiteral("<tds:ScopeItem>%1</tds:ScopeItem>").arg(office)).ok);
    QVERIFY(m_camera->model().scopes.contains(office));

    const Call removed = invoke("device", "RemoveScopes",
                                QStringLiteral("<tds:ScopeItem>%1</tds:ScopeItem>").arg(office));
    QVERIFY(removed.ok);
    QCOMPARE(childText(&removed.response, "ScopeItem"), office);
    QVERIFY(!m_camera->model().scopes.contains(office));

    // 出厂固定项删不掉。
    const QString fixedScope = QStringLiteral("onvif://www.onvif.org/type/video_encoder");
    const Call refused = invoke("device", "RemoveScopes",
                                QStringLiteral("<tds:ScopeItem>%1</tds:ScopeItem>")
                                    .arg(fixedScope));
    QVERIFY(!refused.ok);
    QCOMPARE(refused.faultSubcode, QStringLiteral("ter:FixedScope"));
    QVERIFY(m_camera->model().scopes.contains(fixedScope));

    // SetScopes 替换全部 Configurable，Fixed 原样留着。
    const QString renamed = QStringLiteral("onvif://www.onvif.org/name/Renamed");
    QVERIFY(invoke("device", "SetScopes",
                   QStringLiteral("<tds:Scopes>%1</tds:Scopes>").arg(renamed)).ok);
    const QStringList after = m_camera->model().scopes;
    QVERIFY(after.contains(fixedScope));
    QVERIFY(after.contains(renamed));
    for (const QString &s : after) {
        if (s.contains(QLatin1String("/name/")))
            QCOMPARE(s, renamed);
        QVERIFY(!s.contains(QLatin1String("/location/")));
    }
}

void TestDeviceService::usersLifecycle()
{
    const Call initial = invoke("device", "GetUsers");
    QVERIFY(initial.ok);
    QCOMPARE(int(initial.response.childrenNamed(QStringLiteral("User")).size()), 3);
    for (const XmlNode *u : initial.response.childrenNamed(QStringLiteral("User"))) {
        QVERIFY(!childText(u, "Username").isEmpty());
        QVERIFY(!childText(u, "UserLevel").isEmpty());
        // 口令绝不回吐。
        QVERIFY(childText(u, "Password").isEmpty());
    }

    const QString guest = QStringLiteral("<tds:User><tt:Username>guest</tt:Username>"
                                         "<tt:Password>guest123</tt:Password>"
                                         "<tt:UserLevel>User</tt:UserLevel></tds:User>");
    QVERIFY(invoke("device", "CreateUsers", guest).ok);
    QCOMPARE(int(m_camera->model().users.size()), 4);
    QVERIFY(m_camera->model().checkPassword(QStringLiteral("guest"),
                                            QStringLiteral("guest123")));

    const Call clash = invoke("device", "CreateUsers", guest);
    QVERIFY(!clash.ok);
    QCOMPARE(clash.faultSubcode, QStringLiteral("ter:UsernameClash"));
    QCOMPARE(int(m_camera->model().users.size()), 4);

    // SetUser 只给 UserLevel 时不能把口令顺手清空。
    QVERIFY(invoke("device", "SetUser",
                   QStringLiteral("<tds:User><tt:Username>guest</tt:Username>"
                                  "<tt:UserLevel>Operator</tt:UserLevel></tds:User>")).ok);
    QCOMPARE(m_camera->model().levelOf(QStringLiteral("guest")), UserLevel::Operator);
    QVERIFY(m_camera->model().checkPassword(QStringLiteral("guest"),
                                            QStringLiteral("guest123")));

    const Call missing = invoke("device", "SetUser",
                                QStringLiteral("<tds:User><tt:Username>nobody</tt:Username>"
                                               "<tt:UserLevel>User</tt:UserLevel></tds:User>"));
    QVERIFY(!missing.ok);
    QCOMPARE(missing.faultSubcode, QStringLiteral("ter:UsernameMissing"));

    QVERIFY(invoke("device", "DeleteUsers",
                   QStringLiteral("<tds:Username>guest</tds:Username>")).ok);
    QCOMPARE(int(m_camera->model().users.size()), 3);

    // 删光管理员就再也改不回来了，必须拒绝。
    const Call lastAdmin = invoke("device", "DeleteUsers",
                                  QStringLiteral("<tds:Username>admin</tds:Username>"));
    QVERIFY(!lastAdmin.ok);
    QCOMPARE(lastAdmin.faultSubcode, QString::fromLatin1(ter::OperationProhibited));
    QVERIFY(m_camera->model().findUser(QStringLiteral("admin")) != nullptr);
}

void TestDeviceService::hostnameAndNetwork()
{
    QVERIFY(invoke("device", "SetHostname", QStringLiteral("<tds:Name>lab-cam</tds:Name>")).ok);
    QCOMPARE(m_camera->model().hostname, QStringLiteral("lab-cam"));

    const Call host = invoke("device", "GetHostname");
    QVERIFY(host.ok);
    QCOMPARE(childText(host.response.child(QStringLiteral("HostnameInformation")), "Name"),
             QStringLiteral("lab-cam"));

    QVERIFY(!invoke("device", "SetHostname", QStringLiteral("<tds:Name>  </tds:Name>")).ok);

    const Call ifaces = invoke("device", "GetNetworkInterfaces");
    QVERIFY(ifaces.ok);
    const XmlNode *iface = ifaces.response.child(QStringLiteral("NetworkInterfaces"));
    QVERIFY(iface);
    QCOMPARE(iface->attribute(QStringLiteral("token")), QStringLiteral("eth0"));
    QVERIFY(!childText(iface->child(QStringLiteral("Info")), "HwAddress").isEmpty());
    // 网卡地址与 XAddr 报同一个，否则客户端会当成多网口设备另走一套逻辑。
    QCOMPARE(pathText(*iface, "IPv4/Config/Manual/Address"), QStringLiteral("192.0.2.10"));

    const Call protos = invoke("device", "GetNetworkProtocols");
    QVERIFY(protos.ok);
    QMap<QString, int> ports;
    for (const XmlNode *p : protos.response.childrenNamed(QStringLiteral("NetworkProtocols")))
        ports.insert(childText(p, "Name"), p->childInt(QStringLiteral("Port")));
    QCOMPARE(ports.value(QStringLiteral("HTTP")), 8000);
    QCOMPARE(ports.value(QStringLiteral("RTSP")), 8554);

    QVERIFY(invoke("device", "GetDNS").ok);
    QVERIFY(invoke("device", "GetNTP").ok);
}

void TestDeviceService::discoveryMode()
{
    const Call initial = invoke("device", "GetDiscoveryMode");
    QVERIFY(initial.ok);
    QCOMPARE(childText(&initial.response, "DiscoveryMode"), QStringLiteral("Discoverable"));

    QVERIFY(invoke("device", "SetDiscoveryMode",
                   QStringLiteral("<tds:DiscoveryMode>NonDiscoverable"
                                  "</tds:DiscoveryMode>")).ok);
    const Call changed = invoke("device", "GetDiscoveryMode");
    QVERIFY(changed.ok);
    QCOMPARE(childText(&changed.response, "DiscoveryMode"), QStringLiteral("NonDiscoverable"));

    const Call bad = invoke("device", "SetDiscoveryMode",
                            QStringLiteral("<tds:DiscoveryMode>Maybe</tds:DiscoveryMode>"));
    QVERIFY(!bad.ok);
    QCOMPARE(bad.faultSubcode, QString::fromLatin1(ter::InvalidArgVal));

    QVERIFY(invoke("device", "SetDiscoveryMode",
                   QStringLiteral("<tds:DiscoveryMode>Discoverable</tds:DiscoveryMode>")).ok);
}

void TestDeviceService::relayOutputs()
{
    const Call outputs = invoke("device", "GetRelayOutputs");
    QVERIFY(outputs.ok);
    const XmlNode *relay = outputs.response.child(QStringLiteral("RelayOutputs"));
    QVERIFY(relay);
    QCOMPARE(attrOf(relay, "token"), QStringLiteral("RelayOutput_1"));
    QCOMPARE(childText(relay->child(QStringLiteral("Properties")), "Mode"),
             QStringLiteral("Bistable"));

    QVERIFY(invoke("device", "SetRelayOutputState",
                   QStringLiteral("<tds:RelayOutputToken>RelayOutput_1</tds:RelayOutputToken>"
                                  "<tds:LogicalState>active</tds:LogicalState>")).ok);

    const Call unknown = invoke("device", "SetRelayOutputState",
                                QStringLiteral("<tds:RelayOutputToken>nope</tds:RelayOutputToken>"
                                               "<tds:LogicalState>active</tds:LogicalState>"));
    QVERIFY(!unknown.ok);
    QCOMPARE(unknown.faultSubcode, QString::fromLatin1(ter::NoToken));

    const Call badState = invoke("device", "SetRelayOutputState",
                                 QStringLiteral("<tds:RelayOutputToken>RelayOutput_1"
                                                "</tds:RelayOutputToken>"
                                                "<tds:LogicalState>on</tds:LogicalState>"));
    QVERIFY(!badState.ok);
    QCOMPARE(badState.faultSubcode, QString::fromLatin1(ter::InvalidArgVal));
}

void TestDeviceService::systemLogAndReboot()
{
    const Call log = invoke("device", "GetSystemLog",
                            QStringLiteral("<tds:LogType>System</tds:LogType>"));
    QVERIFY(log.ok);
    QVERIFY(!childText(log.response.child(QStringLiteral("SystemLog")), "String").isEmpty());

    const Call badType = invoke("device", "GetSystemLog",
                                QStringLiteral("<tds:LogType>Kernel</tds:LogType>"));
    QVERIFY(!badType.ok);

    // quirk 关着时只回 OK；开着时也必须先把响应写完（真离线由定时器延后触发，
    // 否则客户端连这句 OK 都收不到）。
    const Call plain = invoke("device", "SystemReboot");
    QVERIFY(plain.ok);
    QVERIFY(!childText(&plain.response, "Message").isEmpty());

    Quirks &q = m_camera->mutableQuirks();
    q.setEnabled(QuirkId::SystemRebootReal, true);
    const Call real = invoke("device", "SystemReboot");
    QVERIFY(real.ok);
    QVERIFY(childText(&real.response, "Message").contains(QStringLiteral("Rebooting")));
    // 相机没启动，goOffline() 会自己短路；这里只确认没有当场把响应吞掉。
    QVERIFY(!m_camera->isOffline());
    q.setEnabled(QuirkId::SystemRebootReal, false);
}

void TestDeviceService::endpointReferenceAndWsdlUrl()
{
    const Call epr = invoke("device", "GetEndpointReference");
    QVERIFY(epr.ok);
    // 与 WS-Discovery 的 EndpointReference 必须是同一串，客户端拿它去重。
    QCOMPARE(childText(&epr.response, "GUID"), m_camera->model().endpointReference);
    QVERIFY(childText(&epr.response, "GUID").startsWith(QStringLiteral("urn:uuid:")));

    const Call wsdl = invoke("device", "GetWsdlUrl");
    QVERIFY(wsdl.ok);
    QVERIFY(looksLikeXAddr(childText(&wsdl.response, "WsdlUrl")));
}

void TestDeviceService::deviceIoTokens()
{
    const Call audio = invoke("deviceio", "GetAudioOutputs");
    QVERIFY(audio.ok);
    QCOMPARE(childText(&audio.response, "Token"), QStringLiteral("AudioOutput_1"));

    // DeviceIO 与 Device 报的继电器 token 必须一致，否则客户端会以为有两个继电器。
    const Call relays = invoke("deviceio", "GetRelayOutputs");
    QVERIFY(relays.ok);
    QCOMPARE(attrOf(relays.response.child(QStringLiteral("RelayOutputs")), "token"),
             QStringLiteral("RelayOutput_1"));

    const Call inputs = invoke("deviceio", "GetDigitalInputs");
    QVERIFY(inputs.ok);
    QCOMPARE(attrOf(inputs.response.child(QStringLiteral("DigitalInputs")), "token"),
             QStringLiteral("DigitalInput_1"));

    const Call caps = invoke("deviceio", "GetServiceCapabilities");
    QVERIFY(caps.ok);
    QCOMPARE(attrOf(caps.response.child(QStringLiteral("Capabilities")), "RelayOutputs"),
             QStringLiteral("1"));
}

void TestDeviceService::analyticsRulesMatchEventTopics()
{
    const QVector<TopicDef> topics = m_camera->events()->topics();
    QVERIFY(!topics.isEmpty());

    const Call supported = invoke("analytics", "GetSupportedRules");
    QVERIFY(supported.ok);
    const XmlNode *set = supported.response.child(QStringLiteral("SupportedRules"));
    QVERIFY(set);

    QSet<QString> parentTopics;
    const QVector<const XmlNode *> descriptions =
        set->childrenNamed(QStringLiteral("RuleDescription"));
    QVERIFY(descriptions.size() >= 3);
    for (const XmlNode *d : descriptions) {
        QVERIFY(!d->attribute(QStringLiteral("Name")).isEmpty());
        // ParentTopic 按 schema 挂在 tt:Messages 下面，不是 RuleDescription 的直接子元素。
        const QString parent = pathText(*d, "Messages/ParentTopic");
        QVERIFY(!parent.isEmpty());
        parentTopics.insert(parent);
        QVERIFY(d->path(QStringLiteral("Messages/Data/SimpleItemDescription")));
    }

    // 规则的 ParentTopic 必须是事件层真会推的 topic，否则客户端订阅了一个
    // 不存在的 topic —— 界面上就是「有规则但永远不报警」。
    const EventKind kinds[] = { EventKind::Motion, EventKind::LineCrossing,
                                EventKind::FieldIntrusion };
    for (EventKind kind : kinds) {
        const TopicDef *def = TopicCatalog::findByKind(topics, kind);
        QVERIFY2(def, qPrintable(TopicCatalog::kindName(kind)));
        QVERIFY2(parentTopics.contains(def->topic), qPrintable(def->topic));
    }

    const Call rules = invoke("analytics", "GetRules");
    QVERIFY(rules.ok);
    QSet<QString> ruleNames;
    for (const XmlNode *r : rules.response.childrenNamed(QStringLiteral("Rule"))) {
        QVERIFY(!r->attribute(QStringLiteral("Type")).isEmpty());
        ruleNames.insert(r->attribute(QStringLiteral("Name")));
        QVERIFY(r->path(QStringLiteral("Parameters/SimpleItem")));
    }
    for (EventKind kind : kinds) {
        const TopicDef *def = TopicCatalog::findByKind(topics, kind);
        QVERIFY(def);
        QVERIFY2(ruleNames.contains(def->ruleItemValue), qPrintable(def->ruleItemValue));
    }

    const Call modules = invoke("analytics", "GetAnalyticsModules");
    QVERIFY(modules.ok);
    QVERIFY(!modules.response.childrenNamed(QStringLiteral("AnalyticsModule")).isEmpty());

    const Call caps = invoke("analytics", "GetServiceCapabilities");
    QVERIFY(caps.ok);
    QCOMPARE(attrOf(caps.response.child(QStringLiteral("Capabilities")), "RuleSupport"),
             QStringLiteral("true"));
}

QTEST_GUILESS_MAIN(TestDeviceService)

#include "tst_device_service.moc"
