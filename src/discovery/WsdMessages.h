#pragma once

// WS-Discovery 报文的生成与解析。
//
// 这一层刻意不碰任何 socket、也不引用 core/net：报文格式是最容易出错、
// 又最需要按「参照客户端到底能不能解析」来钉死的东西，做成纯函数才好单测。
// 收发、组播加入、按相机选源地址那些事全在 DiscoveryResponder 里。
//
// 两套方言并存的原因见 docs/reference-client-facts.md §1：
// 参照客户端用的是 WS-Discovery 1.0（2005/04 discovery + 2004/08 addressing），
// 而 ONVIF 圈子里另一套常见的是 OASIS 2009/01。回错一套 = 客户端直接丢包（quirk A1）。

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

namespace onvifsim {

// 报文层只认具体方言。「照抄 Probe」是 DiscoveryResponder 的策略，
// 到这里必须已经落定成其中一套。
enum class WsdDialect { Ws2005, Oasis2009 };

enum class WsdMessageType {
    Unknown,
    Probe,
    Resolve,
    Hello,
    Bye,
    ProbeMatches,
    ResolveMatches,
};

namespace wsd {

const char *discoveryNamespace(WsdDialect dialect);
const char *addressingNamespace(WsdDialect dialect);

// 完整 Action URI，localAction 传 "Probe" / "ProbeMatches" / "Hello" …
QString actionUri(WsdDialect dialect, const QString &localAction);

// 组播报文的 wsa:To（两套方言各有自己的 urn）。
QString multicastTo(WsdDialect dialect);
QString anonymousAddress(WsdDialect dialect);

// 按命名空间串判方言；认不出来时用 fallback（默认站在参照客户端那一边）。
WsdDialect dialectFromNamespace(const QString &uri, WsdDialect fallback = WsdDialect::Ws2005);

QString newMessageId();

// Scopes 在线路上是空格分隔的一行，所以值里的空格必须 %20。
// 只处理空格：scope 通常已经是编码好的 URI，整体再编一遍会把 %2F 变成 %252F。
QString encodeScope(const QString &scope);
QString joinScopes(const QStringList &scopes);
QStringList splitScopes(const QString &line);

// quirk A4：把一条不可达地址塞进 XAddrs。客户端只取 getXAddrs()[0]，
// 所以 first=true 时插到最前面才有杀伤力。端口与路径沿用原来那条。
QStringList applyBadXAddr(const QStringList &xaddrs, const QString &address, bool first);

// 本设备对外宣称的 Types。Probe 不带 Types 时用不上，但 ProbeMatch 里要写。
QStringList advertisedTypes();

// QName 去前缀。Probe 里的 Types 是 "dn:NetworkVideoTransmitter" 这种，
// 而前缀绑定在 Envelope 上，XmlNode 不保留命名空间声明，只能按 localName 比。
QString localPart(const QString &qname);

} // namespace wsd

// 解析出来的入站报文。
struct WsdMessage {
    bool valid = false;
    QString error;

    WsdMessageType type = WsdMessageType::Unknown;
    WsdDialect dialect = WsdDialect::Ws2005;

    QString messageId;
    QString relatesTo;
    QString action;
    QString to;
    QString replyTo;            // wsa:ReplyTo/wsa:Address

    QStringList types;          // 原样 QName（可能带前缀）
    QStringList typeLocalNames; // 去前缀后的 localName，匹配用
    QStringList scopes;
    QString scopesMatchBy;

    QString endpointReference;  // Resolve / Hello / Bye 里的 wsa:Address

    bool isProbe() const { return type == WsdMessageType::Probe; }
    bool isResolve() const { return type == WsdMessageType::Resolve; }
};

WsdMessage parseWsdMessage(const QByteArray &data);

// 一台相机对外宣告的内容。quirk 由 DiscoveryResponder 先叠加好再交过来，
// 这样报文层不必知道 Quirks 的存在。
struct WsdMatch {
    QString endpointReference;          // urn:uuid:…，客户端的去重键
    QStringList types = wsd::advertisedTypes();
    QStringList scopes;
    QStringList xaddrs;
    int metadataVersion = 1;
    bool includeXAddrs = true;          // false = quirk A2
    bool includeMetadataVersion = true; // false = quirk A3，客户端会整包丢弃
};

// 报文级参数（信封头里的东西）。
struct WsdEnvelopeInfo {
    WsdDialect dialect = WsdDialect::Ws2005;
    QString messageId;          // 空则自动生成
    QString relatesTo;          // ProbeMatches / ResolveMatches 必填，缺则客户端索引异常
    QString to;                 // 空则不写 —— 参照客户端不要求 To
    quint64 instanceId = 1;
    quint32 messageNumber = 1;
    bool includeAppSequence = true;
};

QByteArray buildProbeMatches(const QVector<WsdMatch> &matches, const WsdEnvelopeInfo &info);
QByteArray buildResolveMatches(const WsdMatch &match, const WsdEnvelopeInfo &info);
QByteArray buildHello(const WsdMatch &match, const WsdEnvelopeInfo &info);
QByteArray buildBye(const WsdMatch &match, const WsdEnvelopeInfo &info);

// Probe 的过滤条件是否命中这台相机。
// 参照客户端的 Probe 既不带 Types 也不带 Scopes，两个函数对空过滤都必须返回 true。
bool probeMatchesTypes(const WsdMessage &probe, const QStringList &advertised);
bool probeMatchesScopes(const WsdMessage &probe, const QStringList &advertised);

// 同一个 Probe 会重发 4 次（MessageID 相同，随机间隔 50/250/500ms）。
// 需要「去重回一次」时用它，键是 MessageID + 来源。
} // namespace onvifsim
