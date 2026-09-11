#include "soap/Envelope.h"

#include "soap/Namespaces.h"

namespace onvifsim {
namespace {

// wsa 在 SOAP 消息里是 2005/08，WS-Discovery 那边是 2004/08。
// 这里按 localName 取值、只用命名空间做粗筛，两种方言都收得下。
bool isAddressingNs(const QString &uri)
{
    return uri == QLatin1String(ns::WsAddressing2005) || uri == QLatin1String(ns::WsAddressing2004);
}

QString addressingText(const XmlNode &header, const QString &localName)
{
    for (const XmlNode &c : header.children) {
        if (c.name == localName && isAddressingNs(c.ns))
            return c.text;
    }
    return QString();
}

// ReplyTo 是个 EndpointReference，真正的地址在 wsa:Address 里。
QString replyToAddress(const XmlNode &header)
{
    for (const XmlNode &c : header.children) {
        if (c.name != QLatin1String("ReplyTo") || !isAddressingNs(c.ns))
            continue;
        if (const XmlNode *addr = c.child(QStringLiteral("Address")))
            return addr->text;
        return c.text;
    }
    return QString();
}

void extractSecurity(const XmlNode &header, SoapRequest &out)
{
    const XmlNode *security = header.child(QStringLiteral("Security"));
    if (!security)
        return;
    // 有 Security 头就不算匿名，哪怕里面是空的 —— 校验（含「空 token 算失败」）
    // 由 WsSecurity 决定，这里只负责如实取字段。
    out.hasSecurity = true;

    const XmlNode *token = security->child(QStringLiteral("UsernameToken"));
    if (!token)
        return;
    out.username = token->childText(QStringLiteral("Username"));
    out.nonceBase64 = token->childText(QStringLiteral("Nonce"));
    out.created = token->childText(QStringLiteral("Created"));

    if (const XmlNode *password = token->child(QStringLiteral("Password"))) {
        out.password = password->text;
        // Type 缺省即 PasswordText（wss UsernameToken profile 的规定）。
        const QString type = password->attribute(QStringLiteral("Type"));
        out.passwordIsDigest = type.endsWith(QLatin1String("#PasswordDigest"));
    }
}

} // namespace

namespace soap {

SoapRequest parseEnvelope(const QByteArray &data)
{
    SoapRequest req;
    req.raw = data;

    QString error;
    const XmlNode root = xml::parse(data, &error);
    if (!error.isEmpty()) {
        req.parseError = error;
        return req;
    }
    if (root.name != QLatin1String("Envelope")) {
        req.parseError = QStringLiteral("root element is <%1>, expected <Envelope>").arg(root.name);
        return req;
    }

    // 版本以信封的实际命名空间为准，Content-Type 只是提示：
    // 真机里 text/xml 配 1.2 信封、application/soap+xml 配 1.1 信封都见过。
    if (root.ns == QLatin1String(ns::Soap11)) {
        req.soapVersion = 11;
    } else if (root.ns == QLatin1String(ns::Soap12)) {
        req.soapVersion = 12;
    } else {
        req.parseError = QStringLiteral("unknown SOAP envelope namespace: %1").arg(root.ns);
        return req;
    }

    if (const XmlNode *header = root.child(root.ns, QStringLiteral("Header"))) {
        req.action = addressingText(*header, QStringLiteral("Action"));
        req.to = addressingText(*header, QStringLiteral("To"));
        req.messageId = addressingText(*header, QStringLiteral("MessageID"));
        req.replyTo = replyToAddress(*header);
        extractSecurity(*header, req);
    }

    const XmlNode *body = root.child(root.ns, QStringLiteral("Body"));
    if (!body) {
        req.parseError = QStringLiteral("missing SOAP Body");
        return req;
    }
    if (body->children.isEmpty()) {
        req.parseError = QStringLiteral("empty SOAP Body");
        return req;
    }

    // Body 首元素就是操作元素：ns + localName 决定分发到哪个服务的哪个操作。
    const XmlNode &op = body->children.first();
    req.bodyNamespace = op.ns;
    req.bodyName = op.name;
    req.body = op;
    return req;
}

int soapVersionFromContentType(const QByteArray &contentType)
{
    const QByteArray lower = contentType.toLower();
    if (lower.contains("application/soap+xml"))
        return 12;
    if (lower.contains("text/xml"))
        return 11;
    return 12;   // 拿不准就按 1.2：ONVIF 主流固件与参照客户端都发 1.2。
}

const char *contentTypeForVersion(int soapVersion)
{
    return soapVersion == 11 ? "text/xml; charset=utf-8" : "application/soap+xml; charset=utf-8";
}

} // namespace soap
} // namespace onvifsim
