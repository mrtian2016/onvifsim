#pragma once

// 响应 XML 生成器。刻意不用 QXmlStreamWriter / QDomDocument：
// quirk D5 要求能吐出「属性值不加引号」的非法 XML，正规写入器做不到。
//
// 用法：
//   XmlWriter w(12);
//   w.startEnvelope();                       // 声明前缀 + 进入 Body
//   { XmlWriter::Scope s(w, "tds:GetDeviceInformationResponse");
//     w.element("tds:Manufacturer", "ACME"); }
//   w.endEnvelope();

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

namespace onvifsim {

class XmlWriter
{
public:
    explicit XmlWriter(int soapVersion = 12);

    // ---- 前缀 ----
    // 内置一批常用前缀（s / tt / ter / xsi …）。服务自己的前缀在 startEnvelope 前声明。
    void declarePrefix(const QString &prefix, const QString &uri);
    void declareServicePrefixes();  // 一次性把 tds/trt/tr2/tptz/timg/tev/tan/tmd 全声明上

    // ---- 信封 ----
    // withHeader=true 时先进 SOAP Header（Events 的 Notify 要在 Header 里放
    // wsa:Action 与 SubscriptionReference），写完 Header 调 endHeader() 进 Body。
    // 默认 false 直接进 Body —— 绝大多数 ONVIF 响应不需要 Header。
    void startEnvelope(bool withHeader = false);
    void endHeader();
    void endEnvelope();
    // 只写片段（不带信封），用于 SDP 之外的裸 XML 响应与厂商私有 API。
    void startFragment();

    // ---- 元素 ----
    void start(const QString &qname);
    // **只能紧跟在开元素之后调用**：一旦已经写过子元素或文本，
    // 属性会被静默丢弃（QXmlStreamWriter 的限制）。写属性的代码要么紧跟
    // Scope 构造，要么就别写。
    void attr(const QString &name, const QString &value);
    void text(const QString &value);
    void raw(const QByteArray &data);       // 原样插入，调用方自己负责合法性
    void end();

    void element(const QString &qname, const QString &text);
    void element(const QString &qname, int value);
    void element(const QString &qname, double value);
    void element(const QString &qname, bool value);
    void emptyElement(const QString &qname);

    // 常用片段
    void intRange(const QString &qname, int min, int max);
    void floatRange(const QString &qname, double min, double max);
    void resolution(const QString &qname, int width, int height);

    // ---- 畸形模式（quirk）----
    void setUnquotedAttributes(bool on);   // D5：属性值不加引号
    bool unquotedAttributes() const;

    int soapVersion() const;
    bool isEmpty() const;
    QByteArray take();          // 取走结果并清空
    QByteArray peek() const;

    // RAII 元素作用域。
    class Scope
    {
    public:
        Scope(XmlWriter &w, const QString &qname) : m_w(w) { m_w.start(qname); }
        ~Scope() { m_w.end(); }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

    private:
        XmlWriter &m_w;
    };

private:
    void closeStartTag();

    QByteArray m_out;
    QVector<QString> m_stack;
    QVector<QPair<QString, QString>> m_prefixes;
    int m_soapVersion;
    bool m_inStartTag = false;
    bool m_unquoted = false;
    bool m_envelopeOpen = false;
    bool m_inHeader = false;
};

} // namespace onvifsim
