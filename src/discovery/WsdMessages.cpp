#include "discovery/WsdMessages.h"

#include "soap/Namespaces.h"
#include "soap/XmlNode.h"
#include "soap/XmlWriter.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>
#include <QtCore/QUuid>

namespace onvifsim {
namespace {

// 两套方言的 To 常量。写错客户端不会报错，只是安静地不认。
const char *kTo2005 = "urn:schemas-xmlsoap-org:ws:2005:04:discovery";
const char *kTo2009 = "urn:docs-oasis-open-org:ws-dd:ns:discovery:2009:01";

const char *kAnon2005 = "http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous";
const char *kAnon2009 = "http://www.w3.org/2005/08/addressing/anonymous";

// WS-Discovery 的 MatchBy 规则。
enum class ScopeRule {
    Prefix,     // rfc2396 / rfc3986（缺省），按路径段前缀匹配
    Exact,      // uuid / ldap / strcmp0，逐字符比
    Unknown,    // 没实现的自定义规则
};

ScopeRule scopeRuleOf(const QString &matchBy)
{
    if (matchBy.isEmpty() || matchBy.endsWith(QLatin1String("rfc3986"))
        || matchBy.endsWith(QLatin1String("rfc2396"))) {
        return ScopeRule::Prefix;
    }
    if (matchBy.endsWith(QLatin1String("uuid")) || matchBy.endsWith(QLatin1String("ldap"))
        || matchBy.endsWith(QLatin1String("strcmp0"))) {
        return ScopeRule::Exact;
    }
    return ScopeRule::Unknown;
}

bool matchesOneScope(const QString &probeScope, const QString &deviceScope, ScopeRule rule)
{
    if (rule == ScopeRule::Exact)
        return probeScope.compare(deviceScope, Qt::CaseSensitive) == 0;

    // rfc3986：按路径段做前缀匹配，"onvif://x/type" 命中 "onvif://x/type/video_encoder"，
    // 但不命中 "onvif://x/typeXX"。
    QString p = probeScope;
    while (p.endsWith(QLatin1Char('/')))
        p.chop(1);
    if (deviceScope == p)
        return true;
    return deviceScope.startsWith(p + QLatin1Char('/'));
}

// 信封头：MessageID / RelatesTo / To / Action / AppSequence，随后进 Body。
// 不走 XmlWriter::startEnvelope —— 那个直接跳进 Body，写不了 Header。
void startWsdEnvelope(XmlWriter &w, const WsdEnvelopeInfo &info, const QString &actionLocal)
{
    w.startFragment();
    w.start(QStringLiteral("s:Envelope"));
    w.attr(QStringLiteral("xmlns:s"), QLatin1String(ns::Soap12));
    w.attr(QStringLiteral("xmlns:wsa"), QLatin1String(wsd::addressingNamespace(info.dialect)));
    w.attr(QStringLiteral("xmlns:d"), QLatin1String(wsd::discoveryNamespace(info.dialect)));
    // Types 的值是 QName 列表，前缀必须真的绑上，否则严格的客户端解析不出类型。
    w.attr(QStringLiteral("xmlns:dn"), QLatin1String(ns::DiscoveryNetworkVideo));
    w.attr(QStringLiteral("xmlns:tds"), QLatin1String(ns::Device));

    {
        XmlWriter::Scope header(w, QStringLiteral("s:Header"));
        w.element(QStringLiteral("wsa:MessageID"),
                  info.messageId.isEmpty() ? wsd::newMessageId() : info.messageId);
        if (!info.relatesTo.isEmpty())
            w.element(QStringLiteral("wsa:RelatesTo"), info.relatesTo);
        if (!info.to.isEmpty())
            w.element(QStringLiteral("wsa:To"), info.to);
        w.element(QStringLiteral("wsa:Action"), wsd::actionUri(info.dialect, actionLocal));
        if (info.includeAppSequence) {
            w.start(QStringLiteral("d:AppSequence"));
            w.attr(QStringLiteral("InstanceId"), QString::number(info.instanceId));
            w.attr(QStringLiteral("MessageNumber"), QString::number(info.messageNumber));
            w.end();
        }
    }
    w.start(QStringLiteral("s:Body"));
}

// ProbeMatch / ResolveMatch / Hello 的公共体：EPR、Types、Scopes、XAddrs、MetadataVersion。
void writeMatchBody(XmlWriter &w, const QString &qname, const WsdMatch &m)
{
    XmlWriter::Scope match(w, qname);
    {
        XmlWriter::Scope epr(w, QStringLiteral("wsa:EndpointReference"));
        w.element(QStringLiteral("wsa:Address"), m.endpointReference);
    }
    w.element(QStringLiteral("d:Types"), m.types.join(QLatin1Char(' ')));
    w.element(QStringLiteral("d:Scopes"), wsd::joinScopes(m.scopes));
    if (m.includeXAddrs)
        w.element(QStringLiteral("d:XAddrs"), m.xaddrs.join(QLatin1Char(' ')));
    if (m.includeMetadataVersion)
        w.element(QStringLiteral("d:MetadataVersion"), m.metadataVersion);
}

WsdMessageType typeFromLocalName(const QString &name)
{
    if (name == QLatin1String("Probe"))
        return WsdMessageType::Probe;
    if (name == QLatin1String("Resolve"))
        return WsdMessageType::Resolve;
    if (name == QLatin1String("Hello"))
        return WsdMessageType::Hello;
    if (name == QLatin1String("Bye"))
        return WsdMessageType::Bye;
    if (name == QLatin1String("ProbeMatches"))
        return WsdMessageType::ProbeMatches;
    if (name == QLatin1String("ResolveMatches"))
        return WsdMessageType::ResolveMatches;
    return WsdMessageType::Unknown;
}

} // namespace

// ------------------------------------------------------------------ wsd::

namespace wsd {

const char *discoveryNamespace(WsdDialect dialect)
{
    return dialect == WsdDialect::Oasis2009 ? ns::Discovery2009 : ns::Discovery2005;
}

const char *addressingNamespace(WsdDialect dialect)
{
    // 1.0 方言配 2004/08 —— 不是 SOAP 消息里那个 2005/08，这里最容易搞混。
    return dialect == WsdDialect::Oasis2009 ? ns::WsAddressing2005 : ns::WsAddressing2004;
}

QString actionUri(WsdDialect dialect, const QString &localAction)
{
    return QLatin1String(discoveryNamespace(dialect)) + QLatin1Char('/') + localAction;
}

QString multicastTo(WsdDialect dialect)
{
    return QLatin1String(dialect == WsdDialect::Oasis2009 ? kTo2009 : kTo2005);
}

QString anonymousAddress(WsdDialect dialect)
{
    return QLatin1String(dialect == WsdDialect::Oasis2009 ? kAnon2009 : kAnon2005);
}

WsdDialect dialectFromNamespace(const QString &uri, WsdDialect fallback)
{
    if (uri == QLatin1String(ns::Discovery2009))
        return WsdDialect::Oasis2009;
    if (uri == QLatin1String(ns::Discovery2005))
        return WsdDialect::Ws2005;
    return fallback;
}

QString newMessageId()
{
    return QStringLiteral("urn:uuid:%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

QString encodeScope(const QString &scope)
{
    QString s = scope;
    s.replace(QLatin1Char(' '), QStringLiteral("%20"));
    return s;
}

QString joinScopes(const QStringList &scopes)
{
    QStringList encoded;
    encoded.reserve(scopes.size());
    for (const QString &s : scopes) {
        if (!s.isEmpty())
            encoded.append(encodeScope(s));
    }
    return encoded.join(QLatin1Char(' '));
}

// 每条入站 Probe 都会走到这两处，正则必须是 static：就地构造等于每个包
// 重新编译一次 \s+，发现风暴时白烧 CPU。
const QRegularExpression &whitespaceSplitter()
{
    static const QRegularExpression re(QStringLiteral("\\s+"));
    return re;
}

QStringList splitScopes(const QString &line)
{
    // 线路上是空格分隔，但真机也有用换行 / 制表符分隔的，一并当空白处理。
    return line.split(whitespaceSplitter(), Qt::SkipEmptyParts);
}

QStringList applyBadXAddr(const QStringList &xaddrs, const QString &address, bool first)
{
    if (address.isEmpty())
        return xaddrs;

    // 拿第一条真地址当模板，只换 host，保留端口与路径 ——
    // 客户端「只接管 host:port、保留 path」，路径变了这条 quirk 就测不到点上。
    const QString templateUrl =
        xaddrs.isEmpty() ? QStringLiteral("http://0.0.0.0/onvif/device_service") : xaddrs.first();
    const QUrl url(templateUrl);

    QString scheme = url.scheme();
    if (scheme.isEmpty())
        scheme = QStringLiteral("http");
    QString path = url.path();
    if (path.isEmpty())
        path = QStringLiteral("/onvif/device_service");

    // address 允许写成 "192.168.1.1:8080"，此时端口以它为准。
    QString bad = scheme + QStringLiteral("://") + address;
    if (!address.contains(QLatin1Char(':')) && url.port() > 0)
        bad += QLatin1Char(':') + QString::number(url.port());
    bad += path;
    if (url.hasQuery())
        bad += QLatin1Char('?') + url.query();

    QStringList out = xaddrs;
    if (first)
        out.prepend(bad);
    else
        out.append(bad);
    return out;
}

QStringList advertisedTypes()
{
    // 前缀在 Envelope 上绑到 dn / tds，见 startWsdEnvelope()。
    return QStringList{ QStringLiteral("dn:NetworkVideoTransmitter"),
                        QStringLiteral("tds:Device") };
}

QString localPart(const QString &qname)
{
    const int colon = qname.indexOf(QLatin1Char(':'));
    return colon < 0 ? qname : qname.mid(colon + 1);
}

} // namespace wsd

// ----------------------------------------------------------------- 解析

WsdMessage parseWsdMessage(const QByteArray &data)
{
    WsdMessage m;

    QString error;
    const XmlNode root = xml::parse(data, &error);
    if (root.isNull()) {
        m.error = error;
        return m;
    }
    if (root.name != QLatin1String("Envelope")) {
        m.error = QStringLiteral("根元素不是 SOAP Envelope：%1").arg(root.name);
        return m;
    }

    const XmlNode *body = root.child(QStringLiteral("Body"));
    if (!body || body->children.isEmpty()) {
        m.error = QStringLiteral("SOAP Body 为空");
        return m;
    }
    const XmlNode &first = body->children.first();
    m.type = typeFromLocalName(first.name);

    if (const XmlNode *header = root.child(QStringLiteral("Header"))) {
        m.messageId = header->childText(QStringLiteral("MessageID"));
        m.relatesTo = header->childText(QStringLiteral("RelatesTo"));
        m.action = header->childText(QStringLiteral("Action"));
        m.to = header->childText(QStringLiteral("To"));
        if (const XmlNode *reply = header->path(QStringLiteral("ReplyTo/Address")))
            m.replyTo = reply->text;
    }

    // 方言以 Body 首元素的命名空间为准；它没带 ns 时退回看 Action 串。
    if (!first.ns.isEmpty()) {
        m.dialect = wsd::dialectFromNamespace(first.ns);
    } else if (m.action.startsWith(QLatin1String(ns::Discovery2009))) {
        m.dialect = WsdDialect::Oasis2009;
    }

    if (const XmlNode *types = first.child(QStringLiteral("Types"))) {
        m.types = types->text.split(wsd::whitespaceSplitter(), Qt::SkipEmptyParts);
        for (const QString &t : m.types)
            m.typeLocalNames.append(wsd::localPart(t));
    }
    if (const XmlNode *scopes = first.child(QStringLiteral("Scopes"))) {
        m.scopes = wsd::splitScopes(scopes->text);
        m.scopesMatchBy = scopes->attribute(QStringLiteral("MatchBy"));
    }
    if (const XmlNode *addr = first.path(QStringLiteral("EndpointReference/Address")))
        m.endpointReference = addr->text;

    m.valid = m.type != WsdMessageType::Unknown;
    if (!m.valid)
        m.error = QStringLiteral("无法识别的 WS-Discovery 报文：%1").arg(first.name);
    return m;
}

// ----------------------------------------------------------------- 生成

QByteArray buildProbeMatches(const QVector<WsdMatch> &matches, const WsdEnvelopeInfo &info)
{
    XmlWriter w(12);
    startWsdEnvelope(w, info, QStringLiteral("ProbeMatches"));
    {
        XmlWriter::Scope list(w, QStringLiteral("d:ProbeMatches"));
        for (const WsdMatch &m : matches)
            writeMatchBody(w, QStringLiteral("d:ProbeMatch"), m);
    }
    w.endEnvelope();
    return w.take();
}

QByteArray buildResolveMatches(const WsdMatch &match, const WsdEnvelopeInfo &info)
{
    XmlWriter w(12);
    startWsdEnvelope(w, info, QStringLiteral("ResolveMatches"));
    {
        XmlWriter::Scope list(w, QStringLiteral("d:ResolveMatches"));
        writeMatchBody(w, QStringLiteral("d:ResolveMatch"), match);
    }
    w.endEnvelope();
    return w.take();
}

QByteArray buildHello(const WsdMatch &match, const WsdEnvelopeInfo &info)
{
    XmlWriter w(12);
    startWsdEnvelope(w, info, QStringLiteral("Hello"));
    writeMatchBody(w, QStringLiteral("d:Hello"), match);
    w.endEnvelope();
    return w.take();
}

QByteArray buildBye(const WsdMatch &match, const WsdEnvelopeInfo &info)
{
    XmlWriter w(12);
    startWsdEnvelope(w, info, QStringLiteral("Bye"));
    {
        // Bye 只要求 EPR。多写 XAddrs 反而会让某些客户端把「已下线」当成一次地址更新。
        XmlWriter::Scope bye(w, QStringLiteral("d:Bye"));
        XmlWriter::Scope epr(w, QStringLiteral("wsa:EndpointReference"));
        w.element(QStringLiteral("wsa:Address"), match.endpointReference);
    }
    w.endEnvelope();
    return w.take();
}

// ----------------------------------------------------------------- 匹配

bool probeMatchesTypes(const WsdMessage &probe, const QStringList &advertised)
{
    // 参照客户端的 Probe 就是 types=None，绝不能以 dn:NetworkVideoTransmitter 为应答前提。
    if (probe.typeLocalNames.isEmpty())
        return true;

    QStringList advertisedLocals;
    advertisedLocals.reserve(advertised.size());
    for (const QString &t : advertised)
        advertisedLocals.append(wsd::localPart(t));

    // 规范说要「全部命中」，但真机普遍是「命中任一即回」，
    // 而漏回一份的代价（设备搜不到）远大于多回一份。
    for (const QString &want : probe.typeLocalNames) {
        if (advertisedLocals.contains(want))
            return true;
    }
    return false;
}

bool probeMatchesScopes(const WsdMessage &probe, const QStringList &advertised)
{
    if (probe.scopes.isEmpty())
        return true;

    // 没实现的匹配规则一律放行：宁可多回一份，也不要因为一条冷门规则
    // 让相机在客户端里凭空消失。
    const ScopeRule rule = scopeRuleOf(probe.scopesMatchBy);
    if (rule == ScopeRule::Unknown)
        return true;

    for (const QString &want : probe.scopes) {
        bool hit = false;
        for (const QString &have : advertised) {
            if (matchesOneScope(want, have, rule)) {
                hit = true;
                break;
            }
        }
        if (!hit)
            return false;   // Scopes 的语义是「全部命中」，这条与真机一致
    }
    return true;
}

// ----------------------------------------------------------------- 去重


} // namespace onvifsim
