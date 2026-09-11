#include "soap/Fault.h"

#include "core/Quirks.h"
#include "soap/Namespaces.h"
#include "soap/XmlWriter.h"

namespace onvifsim {
namespace {

struct PrefixEntry {
    const char *prefix;
    const char *uri;
};

// 子码可能用到的前缀。XmlWriter 内置了 s / tt / ter / xsi / xsd，
// 别的（比如 FailedAuthentication 用的 wsse）要在写之前补声明，否则响应自己就不合法了。
constexpr PrefixEntry kSubcodePrefixes[] = {
    { ns::prefix::Wsse, ns::WsseSecext },
    { ns::prefix::Wsa, ns::WsAddressing2005 },
    { ns::prefix::Wsnt, ns::Wsnt },
    { ns::prefix::Tns1, ns::Tns1 },
};

void declareSubcodePrefix(XmlWriter &w, const QString &subcode)
{
    const int colon = subcode.indexOf(QLatin1Char(':'));
    if (colon <= 0)
        return;
    const QString prefix = subcode.left(colon);
    for (const PrefixEntry &e : kSubcodePrefixes) {
        if (prefix == QLatin1String(e.prefix)) {
            w.declarePrefix(prefix, QLatin1String(e.uri));
            return;
        }
    }
    // 表里没有的前缀不管：调用方要么用内置前缀，要么自己知道在干什么。
}

QString soapQName(const QString &localName)
{
    return QString::fromLatin1(ns::prefix::Soap) + QLatin1Char(':') + localName;
}

} // namespace

namespace soap {

QByteArray makeFault(int soapVersion, const SoapFault &fault)
{
    XmlWriter w(soapVersion);
    declareSubcodePrefix(w, fault.subcode);
    w.startEnvelope();
    {
        XmlWriter::Scope faultScope(w, soapQName(QStringLiteral("Fault")));
        if (w.soapVersion() == 11) {
            // SOAP 1.1 的 Fault 子元素是无命名空间的，且没有 Subcode 这一层：
            // 真机（gSOAP 系固件）直接把 ter: 子码塞进 faultcode，客户端也照这个抓。
            w.element(QStringLiteral("faultcode"), fault.subcode);
            w.element(QStringLiteral("faultstring"), fault.reason);
            if (!fault.detail.isEmpty())
                w.element(QStringLiteral("detail"), fault.detail);
        } else {
            {
                XmlWriter::Scope code(w, soapQName(QStringLiteral("Code")));
                w.element(soapQName(QStringLiteral("Value")),
                          soapQName(fault.senderFault ? QStringLiteral("Sender")
                                                      : QStringLiteral("Receiver")));
                XmlWriter::Scope sub(w, soapQName(QStringLiteral("Subcode")));
                w.element(soapQName(QStringLiteral("Value")), fault.subcode);
            }
            {
                XmlWriter::Scope reason(w, soapQName(QStringLiteral("Reason")));
                w.start(soapQName(QStringLiteral("Text")));
                w.attr(QStringLiteral("xml:lang"), QStringLiteral("en"));
                w.text(fault.reason);
                w.end();
            }
            if (!fault.detail.isEmpty()) {
                XmlWriter::Scope detail(w, soapQName(QStringLiteral("Detail")));
                w.element(soapQName(QStringLiteral("Text")), fault.detail);
            }
        }
    }
    w.endEnvelope();
    return w.take();
}

SoapFault authFault(const Quirks &quirks)
{
    SoapFault f;
    f.senderFault = true;

    // D8：客户端靠关键词匹配判定「这是认证错误」，四档措辞就是拿来打这套匹配的。
    const QString wording =
        quirks.choice(QuirkId::AuthFaultWording, QStringLiteral("NotAuthorized"));

    if (wording == QLatin1String("SenderNotAuthorized")) {
        f.subcode = QString::fromLatin1(ter::NotAuthorized);
        f.reason = QStringLiteral("Sender not Authorized");
        f.detail = QStringLiteral("The sender is not authorized to perform this operation");
    } else if (wording == QLatin1String("FailedAuthentication")) {
        // wsse 风格：子码来自 WS-Security 而不是 ter:，措辞里带 authentication。
        f.subcode = QString::fromLatin1(ns::prefix::Wsse) + QStringLiteral(":FailedAuthentication");
        f.reason = QStringLiteral("The security token could not be authenticated");
        f.detail = QStringLiteral("Authentication failed");
    } else if (wording == QLatin1String("Unknown")) {
        // 故意一个关键词都不含（notauthorized / not authorized / unauthorized /
        // authentication / sender not authorized / failedauthentication），
        // 用来测客户端认不出认证错误时的兜底分支。
        f.subcode = QString::fromLatin1(ter::OperationProhibited);
        f.reason = QStringLiteral("Access denied by device policy");
        f.detail = QStringLiteral("The device refused to process the request");
    } else {
        f.subcode = QString::fromLatin1(ter::NotAuthorized);
        f.reason = QStringLiteral("Not Authorized");
        f.detail = QStringLiteral("The action requested requires authorization");
    }
    return f;
}

int faultHttpStatus(const Quirks &quirks, const SoapFault &fault)
{
    // 调用方显式指定的优先（少数 handler 要精确控制状态码）。
    if (fault.httpStatus > 0)
        return fault.httpStatus;

    // D7：Fault 走 500 还是 200（少数固件甚至 400）。默认 500 —— 规范做法，
    // 参照客户端对 500 会放行给 body 解析。
    bool ok = false;
    const int status = quirks.choice(QuirkId::FaultHttpStatus, QStringLiteral("500")).toInt(&ok);
    return ok ? status : 500;
}

SoapFault notSupported(const QString &operation)
{
    SoapFault f;
    f.subcode = QString::fromLatin1(ter::ActionNotSupported);
    f.reason = QStringLiteral("Optional Action Not Implemented");
    f.detail = operation;
    return f;
}

SoapFault invalidArg(const QString &what)
{
    SoapFault f;
    f.subcode = QString::fromLatin1(ter::InvalidArgVal);
    f.reason = QStringLiteral("Invalid Argument Value");
    f.detail = what;
    return f;
}

SoapFault noProfile(const QString &token)
{
    SoapFault f;
    f.subcode = QString::fromLatin1(ter::NoProfile);
    f.reason = QStringLiteral("The requested profile token does not exist");
    f.detail = token;
    return f;
}

} // namespace soap
} // namespace onvifsim
