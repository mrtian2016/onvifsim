#include "services/ServiceBase.h"

#include "core/VirtualCamera.h"
#include "net/NetUtil.h"

namespace onvifsim {

const Quirks &SoapContext::quirks() const
{
    static const Quirks empty;
    return camera ? camera->quirks() : empty;
}

QString SoapContext::cameraId() const
{
    return camera ? camera->id() : QString();
}

QString SoapContext::advertisedHost() const
{
    if (!camera)
        return QString();
    // 优先按请求来源挑地址：客户端与模拟器同机时 XAddr 绝不能给 127.0.0.1。
    // Host 头也可以用，但它可能是主机名，而 ONVIF 客户端普遍更认 IP。
    return camera->advertisedHost(http ? http->peerAddress : QHostAddress());
}

quint16 SoapContext::advertisedHttpPort() const
{
    return camera ? camera->advertisedHttpPort() : 0;
}

QString SoapContext::serviceUri(const QString &path) const
{
    QString p = path;
    if (!p.startsWith(QLatin1Char('/')))
        p.prepend(QLatin1Char('/'));
    return QStringLiteral("http://%1:%2%3").arg(advertisedHost()).arg(advertisedHttpPort()).arg(p);
}

const XmlNode *SoapContext::argNode(const QString &name) const
{
    return body ? body->child(name) : nullptr;
}

QString SoapContext::arg(const QString &name, const QString &fallback) const
{
    const XmlNode *n = argNode(name);
    return n ? n->text : fallback;
}

int SoapContext::argInt(const QString &name, int fallback) const
{
    const XmlNode *n = argNode(name);
    if (!n)
        return fallback;
    bool ok = false;
    const int v = n->text.toInt(&ok);
    return ok ? v : fallback;
}

bool SoapContext::argBool(const QString &name, bool fallback) const
{
    const XmlNode *n = argNode(name);
    if (!n)
        return fallback;
    const QString t = n->text.trimmed();
    if (t.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0 || t == QLatin1String("1"))
        return true;
    if (t.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0 || t == QLatin1String("0"))
        return false;
    return fallback;
}

bool SoapContext::hasArg(const QString &name) const
{
    return argNode(name) != nullptr;
}

void SoapContext::fault(const SoapFault &f)
{
    m_hasFault = true;
    m_fault = f;
}

void SoapContext::fault(const QString &subcode, const QString &reason)
{
    SoapFault f;
    f.subcode = subcode;
    f.reason = reason;
    fault(f);
}

void SoapContext::faultNotSupported()
{
    fault(soap::notSupported(soap ? soap->bodyName : QString()));
}

bool SoapContext::hasFault() const
{
    return m_hasFault;
}

const SoapFault &SoapContext::pendingFault() const
{
    return m_fault;
}

void SoapContext::takeOverResponse()
{
    m_takenOver = true;
}

bool SoapContext::responseTakenOver() const
{
    return m_takenOver;
}

SoapService::~SoapService() = default;

void SoapService::writeServiceCapabilities(SoapContext &) const
{
}

void SoapService::op(const char *name, AuthLevel auth, SoapHandler handler)
{
    SoapOperation o;
    o.name = QString::fromLatin1(name);
    o.auth = auth;
    o.handler = std::move(handler);
    m_operations.insert(o.name, o);
}

void SoapService::cameraOp(const char *name, AuthLevel auth, SoapHandler handler)
{
    op(name, auth, [h = std::move(handler)](SoapContext &ctx) {
        if (!ctx.camera) {
            ctx.fault(QString::fromLatin1(ter::Receiver), QStringLiteral("设备未就绪"));
            return;
        }
        h(ctx);
    });
}

const SoapOperation *SoapService::findOperation(const QString &name) const
{
    const auto it = m_operations.constFind(name);
    return it == m_operations.constEnd() ? nullptr : &it.value();
}

} // namespace onvifsim
