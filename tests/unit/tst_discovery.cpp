// WS-Discovery 单测：两种方言的 Probe 解析与 ProbeMatch 生成、Scopes 的空格分隔与
// %20 编码、A2/A3/A4 三条 quirk 的输出差异、Resolve → ResolveMatch、Hello / Bye 格式、
// 同 MessageID 去重。
//
// 断言全部对着 docs/reference-client-facts.md §1 记的客户端行为写：
// 少 RelatesTo 客户端索引异常、少 MetadataVersion 整包丢弃、只取 getXAddrs()[0]。
//
// 只依赖 discovery 与 soap 两层的纯函数，不碰 socket，所以不需要事件循环。

#include <QtTest/QtTest>

#include "discovery/WsdMessages.h"
#include "soap/Namespaces.h"
#include "soap/XmlNode.h"

using namespace onvifsim;

namespace {

// 参照客户端（wsdiscovery 2.1.2）真正发出来的那种 Probe：
// WS-Discovery 1.0 命名空间、addressing 是 2004/08、**没有 Types 过滤**。
const char *kProbe2005 = R"XML(<?xml version="1.0" encoding="utf-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope"
               xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing"
               xmlns:wsd="http://schemas.xmlsoap.org/ws/2005/04/discovery">
  <soap:Header>
    <wsa:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To>
    <wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</wsa:Action>
    <wsa:MessageID>urn:uuid:9b1deb4d-3b7d-4bad-9bdd-2b0d7b3dcb6d</wsa:MessageID>
    <wsa:ReplyTo>
      <wsa:Address>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:Address>
    </wsa:ReplyTo>
  </soap:Header>
  <soap:Body>
    <wsd:Probe/>
  </soap:Body>
</soap:Envelope>)XML";

// ONVIF 圈子里另一套：OASIS 2009/01 + addressing 2005/08，并且带 Types 过滤。
const char *kProbe2009 = R"XML(<?xml version="1.0" encoding="utf-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope"
               xmlns:wsa="http://www.w3.org/2005/08/addressing"
               xmlns:wsd="http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01"
               xmlns:dn="http://www.onvif.org/ver10/network/wsdl">
  <soap:Header>
    <wsa:To>urn:docs-oasis-open-org:ws-dd:ns:discovery:2009:01</wsa:To>
    <wsa:Action>http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01/Probe</wsa:Action>
    <wsa:MessageID>urn:uuid:11111111-2222-3333-4444-555555555555</wsa:MessageID>
  </soap:Header>
  <soap:Body>
    <wsd:Probe>
      <wsd:Types>dn:NetworkVideoTransmitter</wsd:Types>
      <wsd:Scopes>onvif://www.onvif.org/type</wsd:Scopes>
    </wsd:Probe>
  </soap:Body>
</soap:Envelope>)XML";

// 客户端在 ProbeMatch 的 XAddrs 为空时补发的 Resolve。
const char *kResolve2005 = R"XML(<?xml version="1.0" encoding="utf-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope"
               xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing"
               xmlns:wsd="http://schemas.xmlsoap.org/ws/2005/04/discovery">
  <soap:Header>
    <wsa:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To>
    <wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Resolve</wsa:Action>
    <wsa:MessageID>urn:uuid:aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee</wsa:MessageID>
  </soap:Header>
  <soap:Body>
    <wsd:Resolve>
      <wsa:EndpointReference>
        <wsa:Address>urn:uuid:12345678-1234-1234-1234-123456789abc</wsa:Address>
      </wsa:EndpointReference>
    </wsd:Resolve>
  </soap:Body>
</soap:Envelope>)XML";

const char *kEpr = "urn:uuid:12345678-1234-1234-1234-123456789abc";

QStringList sampleScopes()
{
    return QStringList{
        QStringLiteral("onvif://www.onvif.org/type/video_encoder"),
        QStringLiteral("onvif://www.onvif.org/type/NetworkVideoTransmitter"),
        QStringLiteral("onvif://www.onvif.org/Profile/Streaming"),
        // 名字里带空格：线路上必须变成 %20，否则会被当成两条 scope。
        QStringLiteral("onvif://www.onvif.org/name/Front%20Door"),
        QStringLiteral("onvif://www.onvif.org/hardware/Virtual%20Camera"),
    };
}

WsdMatch sampleMatch()
{
    WsdMatch m;
    m.endpointReference = QLatin1String(kEpr);
    m.scopes = sampleScopes();
    m.xaddrs = QStringList{ QStringLiteral("http://192.168.1.64:8000/onvif/device_service") };
    m.metadataVersion = 1;
    return m;
}

WsdEnvelopeInfo sampleInfo(WsdDialect dialect, const QString &relatesTo)
{
    WsdEnvelopeInfo info;
    info.dialect = dialect;
    info.messageId = QStringLiteral("urn:uuid:00000000-0000-0000-0000-0000000000ff");
    info.relatesTo = relatesTo;
    info.to = wsd::anonymousAddress(dialect);
    info.instanceId = 1700000000ULL;
    info.messageNumber = 7;
    return info;
}

// 取 ProbeMatches / ResolveMatches / Hello 报文里的那一条 match 节点。
const XmlNode *matchNode(const XmlNode &root, const QString &listName, const QString &itemName)
{
    const XmlNode *body = root.child(QStringLiteral("Body"));
    if (!body)
        return nullptr;
    if (listName.isEmpty())
        return body->child(itemName);
    const XmlNode *list = body->child(listName);
    return list ? list->child(itemName) : nullptr;
}

const XmlNode *headerChild(const XmlNode &root, const QString &name)
{
    const XmlNode *header = root.child(QStringLiteral("Header"));
    return header ? header->child(name) : nullptr;
}

} // namespace

class TstDiscovery : public QObject
{
    Q_OBJECT

private slots:
    void namespaceConstants();
    void parseProbeWithoutTypes();
    void parseProbeOasis2009();
    void parseGarbage();
    void typeMatching();
    void scopeMatching();
    void scopeEncoding();
    void probeMatch2005HasRequiredFields();
    void probeMatch2009UsesOasisNamespaces();
    void quirkNoMetadataVersion();
    void quirkNoXAddrs();
    void quirkBadXAddrFirst();
    void quirkBadXAddrLast();
    void quirkScopesWithoutName();
    void resolveToResolveMatch();
    void helloMessage();
    void byeMessage();
};

// -------------------------------------------------------------- 命名空间常量

void TstDiscovery::namespaceConstants()
{
    // 1.0 方言配 2004/08 addressing —— SOAP 消息里那个 2005/08 是另一回事。
    QCOMPARE(QByteArray(wsd::discoveryNamespace(WsdDialect::Ws2005)),
             QByteArray(ns::Discovery2005));
    QCOMPARE(QByteArray(wsd::addressingNamespace(WsdDialect::Ws2005)),
             QByteArray("http://schemas.xmlsoap.org/ws/2004/08/addressing"));
    QCOMPARE(QByteArray(wsd::discoveryNamespace(WsdDialect::Oasis2009)),
             QByteArray(ns::Discovery2009));
    QCOMPARE(QByteArray(wsd::addressingNamespace(WsdDialect::Oasis2009)),
             QByteArray("http://www.w3.org/2005/08/addressing"));

    QCOMPARE(wsd::actionUri(WsdDialect::Ws2005, QStringLiteral("ProbeMatches")),
             QStringLiteral("http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches"));
    QCOMPARE(wsd::multicastTo(WsdDialect::Ws2005),
             QStringLiteral("urn:schemas-xmlsoap-org:ws:2005:04:discovery"));

    QCOMPARE(int(wsd::dialectFromNamespace(QLatin1String(ns::Discovery2009))),
             int(WsdDialect::Oasis2009));
    // 认不出来时站在参照客户端那一边。
    QCOMPARE(int(wsd::dialectFromNamespace(QStringLiteral("urn:whatever"))),
             int(WsdDialect::Ws2005));

    QVERIFY(wsd::newMessageId().startsWith(QLatin1String("urn:uuid:")));
    QVERIFY(wsd::newMessageId() != wsd::newMessageId());
}

// ------------------------------------------------------------------ 解析

void TstDiscovery::parseProbeWithoutTypes()
{
    const WsdMessage m = parseWsdMessage(QByteArray(kProbe2005));
    QVERIFY2(m.valid, qPrintable(m.error));
    QCOMPARE(int(m.type), int(WsdMessageType::Probe));
    QCOMPARE(int(m.dialect), int(WsdDialect::Ws2005));
    QCOMPARE(m.messageId, QStringLiteral("urn:uuid:9b1deb4d-3b7d-4bad-9bdd-2b0d7b3dcb6d"));
    QCOMPARE(m.action, QStringLiteral("http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe"));
    QCOMPARE(m.replyTo,
             QStringLiteral("http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous"));
    // 参照客户端的 Probe 就是 types=None，不能以 dn:NetworkVideoTransmitter 为应答前提。
    QVERIFY(m.types.isEmpty());
    QVERIFY(m.scopes.isEmpty());
}

void TstDiscovery::parseProbeOasis2009()
{
    const WsdMessage m = parseWsdMessage(QByteArray(kProbe2009));
    QVERIFY2(m.valid, qPrintable(m.error));
    QCOMPARE(int(m.type), int(WsdMessageType::Probe));
    QCOMPARE(int(m.dialect), int(WsdDialect::Oasis2009));
    QCOMPARE(m.messageId, QStringLiteral("urn:uuid:11111111-2222-3333-4444-555555555555"));
    QCOMPARE(m.types, QStringList{ QStringLiteral("dn:NetworkVideoTransmitter") });
    // 前缀绑定在 Envelope 上，XmlNode 不保留命名空间声明，所以匹配只能按 localName。
    QCOMPARE(m.typeLocalNames, QStringList{ QStringLiteral("NetworkVideoTransmitter") });
    QCOMPARE(m.scopes, QStringList{ QStringLiteral("onvif://www.onvif.org/type") });
}

void TstDiscovery::parseGarbage()
{
    QVERIFY(!parseWsdMessage(QByteArray("not xml at all")).valid);
    QVERIFY(!parseWsdMessage(QByteArray("<html><body/></html>")).valid);
    // 结构对但 Body 首元素不是 WS-Discovery 报文 —— 不能当成 Probe 回。
    const WsdMessage m = parseWsdMessage(QByteArray(
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body><Whatever/></s:Body></s:Envelope>"));
    QVERIFY(!m.valid);
    QCOMPARE(int(m.type), int(WsdMessageType::Unknown));
}

// ------------------------------------------------------------------ 匹配

void TstDiscovery::typeMatching()
{
    const QStringList advertised = wsd::advertisedTypes();
    QVERIFY(advertised.contains(QStringLiteral("dn:NetworkVideoTransmitter")));

    // 不带 Types 的 Probe 必须命中，这是参照客户端唯一会发的形态。
    QVERIFY(probeMatchesTypes(parseWsdMessage(QByteArray(kProbe2005)), advertised));
    // 带 Types 且要 NVT 的也命中。
    QVERIFY(probeMatchesTypes(parseWsdMessage(QByteArray(kProbe2009)), advertised));

    // 换个前缀写法照样命中（前缀不参与比较）。
    WsdMessage other;
    other.typeLocalNames = QStringList{ QStringLiteral("Device") };
    QVERIFY(probeMatchesTypes(other, advertised));

    // 要的是显示器不是发射器，不该回。
    WsdMessage display;
    display.typeLocalNames = QStringList{ QStringLiteral("NetworkVideoDisplay") };
    QVERIFY(!probeMatchesTypes(display, advertised));
}

void TstDiscovery::scopeMatching()
{
    const QStringList advertised = sampleScopes();

    WsdMessage none;
    QVERIFY(probeMatchesScopes(none, advertised));

    // 默认 rfc3986：按路径段前缀匹配。
    QVERIFY(probeMatchesScopes(parseWsdMessage(QByteArray(kProbe2009)), advertised));

    WsdMessage exact;
    exact.scopes = QStringList{ QStringLiteral("onvif://www.onvif.org/Profile/Streaming") };
    QVERIFY(probeMatchesScopes(exact, advertised));

    WsdMessage miss;
    miss.scopes = QStringList{ QStringLiteral("onvif://www.onvif.org/location/roof") };
    QVERIFY(!probeMatchesScopes(miss, advertised));

    // 段边界：typeXX 不该被 type 命中。
    WsdMessage partial;
    partial.scopes = QStringList{ QStringLiteral("onvif://www.onvif.org/typ") };
    QVERIFY(!probeMatchesScopes(partial, advertised));

    // Scopes 的语义是「全部命中」，有一条落空就不回。
    WsdMessage both;
    both.scopes = QStringList{ QStringLiteral("onvif://www.onvif.org/type"),
                               QStringLiteral("onvif://www.onvif.org/location/roof") };
    QVERIFY(!probeMatchesScopes(both, advertised));

    // 没实现的 MatchBy 一律放行 —— 宁可多回一份也不要让相机凭空消失。
    WsdMessage weird;
    weird.scopes = QStringList{ QStringLiteral("anything") };
    weird.scopesMatchBy = QStringLiteral("http://example.com/custom-match");
    QVERIFY(probeMatchesScopes(weird, advertised));
}

// ------------------------------------------------------------------ Scopes

void TstDiscovery::scopeEncoding()
{
    QCOMPARE(wsd::encodeScope(QStringLiteral("onvif://www.onvif.org/name/Front Door")),
             QStringLiteral("onvif://www.onvif.org/name/Front%20Door"));
    // 已经编码好的不能再编一遍，否则 %20 会变成 %2520。
    QCOMPARE(wsd::encodeScope(QStringLiteral("onvif://www.onvif.org/name/Front%20Door")),
             QStringLiteral("onvif://www.onvif.org/name/Front%20Door"));

    const QString line = wsd::joinScopes(
        QStringList{ QStringLiteral("onvif://www.onvif.org/type/video_encoder"),
                     QStringLiteral("onvif://www.onvif.org/name/Front Door") });
    // 线路上是空格分隔的一行：恰好一个分隔空格，值里的空格已经编码掉了。
    QCOMPARE(line.count(QLatin1Char(' ')), 1);
    QVERIFY(!line.contains(QLatin1Char('\n')));
    QCOMPARE(wsd::splitScopes(line).size(), 2);
    QCOMPARE(wsd::splitScopes(line).at(1),
             QStringLiteral("onvif://www.onvif.org/name/Front%20Door"));

    // 生成的报文里也必须是一行、值里带 %20。
    const QByteArray xml =
        buildProbeMatches(QVector<WsdMatch>{ sampleMatch() },
                          sampleInfo(WsdDialect::Ws2005, QStringLiteral("urn:uuid:x")));
    const XmlNode root = xml::parse(xml);
    const XmlNode *match = matchNode(root, QStringLiteral("ProbeMatches"),
                                     QStringLiteral("ProbeMatch"));
    QVERIFY(match);
    const QString scopes = match->childText(QStringLiteral("Scopes"));
    QCOMPARE(wsd::splitScopes(scopes).size(), sampleScopes().size());
    QVERIFY(scopes.contains(QStringLiteral("/name/Front%20Door")));
    QVERIFY(scopes.contains(QStringLiteral("/hardware/Virtual%20Camera")));
}

// ------------------------------------------------------------- ProbeMatch

void TstDiscovery::probeMatch2005HasRequiredFields()
{
    const WsdMessage probe = parseWsdMessage(QByteArray(kProbe2005));
    const QByteArray xml = buildProbeMatches(
        QVector<WsdMatch>{ sampleMatch() }, sampleInfo(WsdDialect::Ws2005, probe.messageId));

    // 命名空间必须与 Probe 同款，否则客户端连解析都不会解析。
    QVERIFY(xml.contains("xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""));
    QVERIFY(xml.contains("xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""));

    QString error;
    const XmlNode root = xml::parse(xml, &error);
    QVERIFY2(!root.isNull(), qPrintable(error));

    // 客户端先按 addressing 命名空间找 Action，找不到整包丢弃。
    const XmlNode *action = headerChild(root, QStringLiteral("Action"));
    QVERIFY(action);
    QCOMPARE(action->ns, QStringLiteral("http://schemas.xmlsoap.org/ws/2004/08/addressing"));
    QCOMPARE(action->text,
             QStringLiteral("http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches"));

    // 三个必填头：MessageID、RelatesTo（缺则客户端索引异常）。
    QVERIFY(headerChild(root, QStringLiteral("MessageID")));
    const XmlNode *relatesTo = headerChild(root, QStringLiteral("RelatesTo"));
    QVERIFY(relatesTo);
    QCOMPARE(relatesTo->text, probe.messageId);

    const XmlNode *appSeq = headerChild(root, QStringLiteral("AppSequence"));
    QVERIFY(appSeq);
    QCOMPARE(appSeq->attribute(QStringLiteral("MessageNumber")), QStringLiteral("7"));

    const XmlNode *match = matchNode(root, QStringLiteral("ProbeMatches"),
                                     QStringLiteral("ProbeMatch"));
    QVERIFY(match);
    QCOMPARE(match->ns, QStringLiteral("http://schemas.xmlsoap.org/ws/2005/04/discovery"));

    // EPR 是客户端的去重键，必须在 addressing 命名空间下。
    const XmlNode *address = match->path(QStringLiteral("EndpointReference/Address"));
    QVERIFY(address);
    QCOMPARE(address->text, QLatin1String(kEpr));
    QCOMPARE(address->ns, QStringLiteral("http://schemas.xmlsoap.org/ws/2004/08/addressing"));

    QVERIFY(match->childText(QStringLiteral("Types"))
                .contains(QLatin1String("NetworkVideoTransmitter")));
    QCOMPARE(match->childText(QStringLiteral("XAddrs")),
             QStringLiteral("http://192.168.1.64:8000/onvif/device_service"));
    // 缺 MetadataVersion 客户端会把整包丢掉，默认必须有。
    QCOMPARE(match->childText(QStringLiteral("MetadataVersion")), QStringLiteral("1"));
}

void TstDiscovery::probeMatch2009UsesOasisNamespaces()
{
    const WsdMessage probe = parseWsdMessage(QByteArray(kProbe2009));
    QCOMPARE(int(probe.dialect), int(WsdDialect::Oasis2009));

    const QByteArray xml = buildProbeMatches(QVector<WsdMatch>{ sampleMatch() },
                                             sampleInfo(probe.dialect, probe.messageId));

    QVERIFY(xml.contains("xmlns:d=\"http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01\""));
    QVERIFY(xml.contains("xmlns:wsa=\"http://www.w3.org/2005/08/addressing\""));

    const XmlNode root = xml::parse(xml);
    const XmlNode *action = headerChild(root, QStringLiteral("Action"));
    QVERIFY(action);
    QCOMPARE(action->text,
             QStringLiteral("http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01/ProbeMatches"));

    const XmlNode *match = matchNode(root, QStringLiteral("ProbeMatches"),
                                     QStringLiteral("ProbeMatch"));
    QVERIFY(match);
    QCOMPARE(match->ns, QStringLiteral("http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01"));
    QCOMPARE(match->childText(QStringLiteral("MetadataVersion")), QStringLiteral("1"));
    QCOMPARE(headerChild(root, QStringLiteral("RelatesTo"))->text, probe.messageId);
}

// ------------------------------------------------------------------ quirks

void TstDiscovery::quirkNoMetadataVersion()
{
    WsdMatch m = sampleMatch();
    m.includeMetadataVersion = false;   // A3

    const QByteArray xml = buildProbeMatches(
        QVector<WsdMatch>{ m }, sampleInfo(WsdDialect::Ws2005, QStringLiteral("urn:uuid:x")));
    const XmlNode root = xml::parse(xml);
    const XmlNode *match = matchNode(root, QStringLiteral("ProbeMatches"),
                                     QStringLiteral("ProbeMatch"));
    QVERIFY(match);
    // 别的字段一个不少，只少这一个 —— 客户端却会把整包丢掉，这就是 A3 的杀伤力。
    QVERIFY(!match->hasChild(QStringLiteral("MetadataVersion")));
    QVERIFY(match->hasChild(QStringLiteral("XAddrs")));
    QVERIFY(match->path(QStringLiteral("EndpointReference/Address")));

    // 默认（不开 quirk）时它必须在。
    const QByteArray normal = buildProbeMatches(
        QVector<WsdMatch>{ sampleMatch() },
        sampleInfo(WsdDialect::Ws2005, QStringLiteral("urn:uuid:x")));
    QVERIFY(normal.contains("MetadataVersion"));
    QVERIFY(!xml.contains("MetadataVersion"));
}

void TstDiscovery::quirkNoXAddrs()
{
    WsdMatch m = sampleMatch();
    m.includeXAddrs = false;   // A2

    const QByteArray xml = buildProbeMatches(
        QVector<WsdMatch>{ m }, sampleInfo(WsdDialect::Ws2005, QStringLiteral("urn:uuid:x")));
    const XmlNode root = xml::parse(xml);
    const XmlNode *match = matchNode(root, QStringLiteral("ProbeMatches"),
                                     QStringLiteral("ProbeMatch"));
    QVERIFY(match);
    // 客户端看到空 XAddrs 会补发 Resolve；MetadataVersion 仍在，否则整包就被丢了。
    QVERIFY(!match->hasChild(QStringLiteral("XAddrs")));
    QCOMPARE(match->childText(QStringLiteral("MetadataVersion")), QStringLiteral("1"));
}

void TstDiscovery::quirkBadXAddrFirst()
{
    const QStringList original{ QStringLiteral("http://192.168.1.64:8000/onvif/device_service") };
    const QStringList bad = wsd::applyBadXAddr(original, QStringLiteral("192.168.1.99"), true);

    QCOMPARE(bad.size(), 2);
    // 客户端只取 getXAddrs()[0]，坏地址必须排第一位才测得到点上。
    QCOMPARE(bad.first(), QStringLiteral("http://192.168.1.99:8000/onvif/device_service"));
    QCOMPARE(bad.last(), original.first());

    // 端口与路径必须沿用原来那条 —— 客户端「只接管 host:port、保留 path」。
    const QStringList zero = wsd::applyBadXAddr(original, QStringLiteral("0.0.0.0"), true);
    QCOMPARE(zero.first(), QStringLiteral("http://0.0.0.0:8000/onvif/device_service"));

    // 主机名形式也要能用。
    const QStringList host = wsd::applyBadXAddr(original, QStringLiteral("camera.local"), true);
    QCOMPARE(host.first(), QStringLiteral("http://camera.local:8000/onvif/device_service"));

    // address 自带端口时以它为准。
    const QStringList withPort = wsd::applyBadXAddr(original, QStringLiteral("10.0.0.1:2020"),
                                                    true);
    QCOMPARE(withPort.first(), QStringLiteral("http://10.0.0.1:2020/onvif/device_service"));

    // 空地址 = 不生效。
    QCOMPARE(wsd::applyBadXAddr(original, QString(), true), original);
}

void TstDiscovery::quirkBadXAddrLast()
{
    const QStringList original{ QStringLiteral("http://192.168.1.64:8000/onvif/device_service") };
    const QStringList bad = wsd::applyBadXAddr(original, QStringLiteral("192.168.1.1"), false);
    QCOMPARE(bad.size(), 2);
    QCOMPARE(bad.first(), original.first());
    QCOMPARE(bad.last(), QStringLiteral("http://192.168.1.1:8000/onvif/device_service"));

    // 多条 XAddrs 在线路上是空格分隔的一行。
    WsdMatch m = sampleMatch();
    m.xaddrs = bad;
    const QByteArray xml = buildProbeMatches(
        QVector<WsdMatch>{ m }, sampleInfo(WsdDialect::Ws2005, QStringLiteral("urn:uuid:x")));
    const XmlNode root = xml::parse(xml);
    const XmlNode *match = matchNode(root, QStringLiteral("ProbeMatches"),
                                     QStringLiteral("ProbeMatch"));
    QVERIFY(match);
    const QStringList onWire = match->childText(QStringLiteral("XAddrs"))
                                   .split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QCOMPARE(onWire, bad);
}

void TstDiscovery::quirkScopesWithoutName()
{
    // DiscoveryResponder 抽掉 name 后交过来的样子：上层从 Scopes 只读 name 与 hardware，
    // 少了 name 相机在客户端里就显示为无名。
    WsdMatch m = sampleMatch();
    QStringList kept;
    for (const QString &s : sampleScopes()) {
        if (!s.contains(QLatin1String("/name/")))
            kept.append(s);
    }
    m.scopes = kept;

    const QByteArray xml = buildProbeMatches(
        QVector<WsdMatch>{ m }, sampleInfo(WsdDialect::Ws2005, QStringLiteral("urn:uuid:x")));
    const XmlNode root = xml::parse(xml);
    const XmlNode *match = matchNode(root, QStringLiteral("ProbeMatches"),
                                     QStringLiteral("ProbeMatch"));
    QVERIFY(match);
    const QString scopes = match->childText(QStringLiteral("Scopes"));
    QVERIFY(!scopes.contains(QLatin1String("/name/")));
    QVERIFY(scopes.contains(QLatin1String("/hardware/")));
}

// ------------------------------------------------------------------ Resolve

void TstDiscovery::resolveToResolveMatch()
{
    const WsdMessage resolve = parseWsdMessage(QByteArray(kResolve2005));
    QVERIFY2(resolve.valid, qPrintable(resolve.error));
    QCOMPARE(int(resolve.type), int(WsdMessageType::Resolve));
    QVERIFY(resolve.isResolve());
    // Resolve 点名要某个 EPR 的地址，认错人就别答。
    QCOMPARE(resolve.endpointReference, QLatin1String(kEpr));

    // A2 让 ProbeMatch 不带 XAddrs，客户端才会补发 Resolve；
    // 到了 ResolveMatch 这一步必须给出 XAddrs，否则这条链走不通。
    const WsdMatch m = sampleMatch();
    QVERIFY(m.includeXAddrs);

    const QByteArray xml = buildResolveMatches(m, sampleInfo(resolve.dialect,
                                                             resolve.messageId));
    const XmlNode root = xml::parse(xml);

    const XmlNode *action = headerChild(root, QStringLiteral("Action"));
    QVERIFY(action);
    QCOMPARE(action->text,
             QStringLiteral("http://schemas.xmlsoap.org/ws/2005/04/discovery/ResolveMatches"));
    QCOMPARE(headerChild(root, QStringLiteral("RelatesTo"))->text, resolve.messageId);

    const XmlNode *match = matchNode(root, QStringLiteral("ResolveMatches"),
                                     QStringLiteral("ResolveMatch"));
    QVERIFY(match);
    QCOMPARE(match->path(QStringLiteral("EndpointReference/Address"))->text, QLatin1String(kEpr));
    QCOMPARE(match->childText(QStringLiteral("XAddrs")),
             QStringLiteral("http://192.168.1.64:8000/onvif/device_service"));
    QCOMPARE(match->childText(QStringLiteral("MetadataVersion")), QStringLiteral("1"));

    // 回过头能被自己解析出来（客户端那边也是同一套读法）。
    const WsdMessage back = parseWsdMessage(xml);
    QCOMPARE(int(back.type), int(WsdMessageType::ResolveMatches));
    QCOMPARE(back.relatesTo, resolve.messageId);
}

// --------------------------------------------------------------- Hello / Bye

void TstDiscovery::helloMessage()
{
    WsdEnvelopeInfo info = sampleInfo(WsdDialect::Ws2005, QString());
    info.to = wsd::multicastTo(WsdDialect::Ws2005);

    const QByteArray xml = buildHello(sampleMatch(), info);
    const XmlNode root = xml::parse(xml);

    const XmlNode *action = headerChild(root, QStringLiteral("Action"));
    QVERIFY(action);
    QCOMPARE(action->text, QStringLiteral("http://schemas.xmlsoap.org/ws/2005/04/discovery/Hello"));
    QCOMPARE(headerChild(root, QStringLiteral("To"))->text,
             QStringLiteral("urn:schemas-xmlsoap-org:ws:2005:04:discovery"));
    // Hello 是主动发的，没有 RelatesTo。
    QVERIFY(!headerChild(root, QStringLiteral("RelatesTo")));
    QVERIFY(headerChild(root, QStringLiteral("AppSequence")));

    const XmlNode *hello = matchNode(root, QString(), QStringLiteral("Hello"));
    QVERIFY(hello);
    QCOMPARE(hello->path(QStringLiteral("EndpointReference/Address"))->text, QLatin1String(kEpr));
    QVERIFY(hello->hasChild(QStringLiteral("Types")));
    QVERIFY(hello->hasChild(QStringLiteral("Scopes")));
    QCOMPARE(hello->childText(QStringLiteral("XAddrs")),
             QStringLiteral("http://192.168.1.64:8000/onvif/device_service"));
    QCOMPARE(hello->childText(QStringLiteral("MetadataVersion")), QStringLiteral("1"));

    QCOMPARE(int(parseWsdMessage(xml).type), int(WsdMessageType::Hello));

    // 2009 方言下换的是命名空间，不是结构。
    WsdEnvelopeInfo oasis = sampleInfo(WsdDialect::Oasis2009, QString());
    oasis.to = wsd::multicastTo(WsdDialect::Oasis2009);
    const QByteArray xml2009 = buildHello(sampleMatch(), oasis);
    QVERIFY(xml2009.contains("xmlns:d=\"http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01\""));
    QCOMPARE(int(parseWsdMessage(xml2009).dialect), int(WsdDialect::Oasis2009));
}

void TstDiscovery::byeMessage()
{
    WsdEnvelopeInfo info = sampleInfo(WsdDialect::Ws2005, QString());
    info.to = wsd::multicastTo(WsdDialect::Ws2005);

    const QByteArray xml = buildBye(sampleMatch(), info);
    const XmlNode root = xml::parse(xml);

    QCOMPARE(headerChild(root, QStringLiteral("Action"))->text,
             QStringLiteral("http://schemas.xmlsoap.org/ws/2005/04/discovery/Bye"));

    const XmlNode *bye = matchNode(root, QString(), QStringLiteral("Bye"));
    QVERIFY(bye);
    QCOMPARE(bye->path(QStringLiteral("EndpointReference/Address"))->text, QLatin1String(kEpr));
    // Bye 只要求 EPR；带上 XAddrs 会让某些客户端把下线当成一次地址更新。
    QVERIFY(!bye->hasChild(QStringLiteral("XAddrs")));
    QVERIFY(!bye->hasChild(QStringLiteral("Scopes")));

    QCOMPARE(int(parseWsdMessage(xml).type), int(WsdMessageType::Bye));
}

QTEST_GUILESS_MAIN(TstDiscovery)

#include "tst_discovery.moc"
