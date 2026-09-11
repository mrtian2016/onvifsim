#include "soap/XmlWriter.h"

#include "soap/Namespaces.h"
#include "soap/XmlNode.h"

namespace onvifsim {
namespace {

// 浮点一律定点输出：xs:float 不认 "1e+06" 这类写法的客户端不在少数，
// 而且 ONVIF 里出现的都是坐标 / 帧率 / 速度这种小量程，定点足够。
QString formatDouble(double value)
{
    QString s = QString::number(value, 'f', 6);
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0')))
            s.chop(1);
        if (s.endsWith(QLatin1Char('.')))
            s.chop(1);
    }
    return s.isEmpty() ? QStringLiteral("0") : s;
}

const char *kXmlDeclaration = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";

// 信封元素的限定名，前缀取自 ns::prefix::Soap。
QString soapQName(const QString &localName)
{
    return QString::fromLatin1(ns::prefix::Soap) + QLatin1Char(':') + localName;
}

} // namespace

XmlWriter::XmlWriter(int soapVersion)
    : m_soapVersion(soapVersion == 11 ? 11 : 12)
{
    // 内置常用前缀：信封前缀随版本切换，其余是几乎每个响应都要用到的。
    declarePrefix(QLatin1String(ns::prefix::Soap),
                  QLatin1String(m_soapVersion == 11 ? ns::Soap11 : ns::Soap12));
    declarePrefix(QLatin1String(ns::prefix::Tt), QLatin1String(ns::Tt));
    declarePrefix(QLatin1String(ns::prefix::Ter), QLatin1String(ns::Ter));
    declarePrefix(QLatin1String(ns::prefix::Xsi), QLatin1String(ns::XmlSchemaInstance));
    declarePrefix(QLatin1String(ns::prefix::Xsd), QLatin1String(ns::XmlSchema));
}

void XmlWriter::declarePrefix(const QString &prefix, const QString &uri)
{
    for (QPair<QString, QString> &p : m_prefixes) {
        if (p.first == prefix) {
            p.second = uri;
            return;
        }
    }
    m_prefixes.append(qMakePair(prefix, uri));
}

void XmlWriter::declareServicePrefixes()
{
    declarePrefix(QLatin1String(ns::prefix::Tds), QLatin1String(ns::Device));
    declarePrefix(QLatin1String(ns::prefix::Trt), QLatin1String(ns::Media));
    declarePrefix(QLatin1String(ns::prefix::Tr2), QLatin1String(ns::Media2));
    declarePrefix(QLatin1String(ns::prefix::Tptz), QLatin1String(ns::Ptz));
    declarePrefix(QLatin1String(ns::prefix::Timg), QLatin1String(ns::Imaging));
    declarePrefix(QLatin1String(ns::prefix::Tev), QLatin1String(ns::Events));
    declarePrefix(QLatin1String(ns::prefix::Tan), QLatin1String(ns::Analytics));
    declarePrefix(QLatin1String(ns::prefix::Tmd), QLatin1String(ns::DeviceIo));
    // 事件相关的前缀也一并声明在 Envelope 上：declarePrefix() 只能在
    // startEnvelope() 之前调，而 Dispatcher 建好 writer 就直接开信封了，
    // 服务层没有机会往 Envelope 上补 —— 缺了它们只能挂在自己那层根元素上。
    declarePrefix(QLatin1String(ns::prefix::Tns1), QLatin1String(ns::Tns1));
    declarePrefix(QLatin1String(ns::prefix::Wsnt), QLatin1String(ns::Wsnt));
    declarePrefix(QLatin1String(ns::prefix::Wstop), QLatin1String(ns::Wstop));
    declarePrefix(QLatin1String(ns::prefix::Wsa), QLatin1String(ns::WsAddressing2005));
}

void XmlWriter::startEnvelope(bool withHeader)
{
    m_out.append(kXmlDeclaration);
    // 所有前缀都声明在 Envelope 上（参照客户端与多数真机就是这么发的），
    // 后面的元素直接用前缀，不再逐级重复 xmlns。
    start(soapQName(QStringLiteral("Envelope")));
    // xmlns 声明始终带引号：D5 只毁 handler 自己写的属性。真机（TP-Link TL-IPC）
    // 也只是 topic 属性没引号，信封本身是好的。
    const bool unquoted = m_unquoted;
    m_unquoted = false;
    for (const QPair<QString, QString> &p : m_prefixes)
        attr(QStringLiteral("xmlns:") + p.first, p.second);
    m_unquoted = unquoted;
    m_envelopeOpen = true;

    if (withHeader) {
        // 调用方接着写 Header 内容，写完调 endHeader() 才进 Body。
        m_inHeader = true;
        start(soapQName(QStringLiteral("Header")));
        return;
    }
    // 默认直接进 Body：handler 只关心自己那一层响应元素。
    start(soapQName(QStringLiteral("Body")));
}

void XmlWriter::endHeader()
{
    if (!m_inHeader)
        return;
    m_inHeader = false;
    end();   // 收掉 Header
    start(soapQName(QStringLiteral("Body")));
}

void XmlWriter::endEnvelope()
{
    // handler 少 end() 一层也不至于吐出半截 XML，这里一路收干净。
    // Header 开着就没人调过 endHeader()，补一个 Body 免得吐出没有 Body 的信封。
    if (m_inHeader)
        endHeader();
    while (!m_stack.isEmpty())
        end();
    m_envelopeOpen = false;
    m_inHeader = false;
}

void XmlWriter::startFragment()
{
    m_out.append(kXmlDeclaration);
    m_envelopeOpen = false;
}

void XmlWriter::start(const QString &qname)
{
    closeStartTag();
    m_out.append('<');
    m_out.append(qname.toUtf8());
    m_stack.append(qname);
    m_inStartTag = true;
}

void XmlWriter::attr(const QString &name, const QString &value)
{
    if (!m_inStartTag)
        return;
    m_out.append(' ');
    m_out.append(name.toUtf8());
    m_out.append('=');
    if (m_unquoted) {
        // quirk D5：属性值不加引号，故意产出非法 XML。转义也只做最低限度，
        // 加引号反而破坏了这条 quirk 的意义。
        m_out.append(value.toUtf8());
    } else {
        m_out.append('"');
        m_out.append(xml::escapeAttribute(value).toUtf8());
        m_out.append('"');
    }
}

void XmlWriter::text(const QString &value)
{
    closeStartTag();
    m_out.append(xml::escape(value).toUtf8());
}

void XmlWriter::raw(const QByteArray &data)
{
    closeStartTag();
    m_out.append(data);
}

void XmlWriter::end()
{
    if (m_stack.isEmpty())
        return;
    const QString qname = m_stack.takeLast();
    if (m_inStartTag) {
        // 没写过内容 → 自闭合
        m_out.append("/>");
        m_inStartTag = false;
        return;
    }
    m_out.append("</");
    m_out.append(qname.toUtf8());
    m_out.append('>');
}

void XmlWriter::element(const QString &qname, const QString &value)
{
    start(qname);
    if (!value.isEmpty())
        text(value);
    end();
}

void XmlWriter::element(const QString &qname, int value)
{
    element(qname, QString::number(value));
}

void XmlWriter::element(const QString &qname, double value)
{
    element(qname, formatDouble(value));
}

void XmlWriter::element(const QString &qname, bool value)
{
    element(qname, value ? QStringLiteral("true") : QStringLiteral("false"));
}

void XmlWriter::emptyElement(const QString &qname)
{
    start(qname);
    end();
}

void XmlWriter::intRange(const QString &qname, int min, int max)
{
    Scope s(*this, qname);
    element(QStringLiteral("tt:Min"), min);
    element(QStringLiteral("tt:Max"), max);
}

void XmlWriter::floatRange(const QString &qname, double min, double max)
{
    Scope s(*this, qname);
    element(QStringLiteral("tt:Min"), min);
    element(QStringLiteral("tt:Max"), max);
}

void XmlWriter::resolution(const QString &qname, int width, int height)
{
    Scope s(*this, qname);
    element(QStringLiteral("tt:Width"), width);
    element(QStringLiteral("tt:Height"), height);
}

void XmlWriter::setUnquotedAttributes(bool on)
{
    m_unquoted = on;
}

bool XmlWriter::unquotedAttributes() const
{
    return m_unquoted;
}

int XmlWriter::soapVersion() const
{
    return m_soapVersion;
}

bool XmlWriter::isEmpty() const
{
    return m_out.isEmpty();
}

QByteArray XmlWriter::take()
{
    QByteArray out;
    out.swap(m_out);
    // 前缀声明留着：同一个 writer 连着写第二份响应时不用重新声明一遍。
    m_stack.clear();
    m_inStartTag = false;
    m_envelopeOpen = false;
    return out;
}

QByteArray XmlWriter::peek() const
{
    return m_out;
}

void XmlWriter::closeStartTag()
{
    if (m_inStartTag) {
        m_out.append('>');
        m_inStartTag = false;
    }
}

} // namespace onvifsim
