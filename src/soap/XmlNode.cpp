#include "soap/XmlNode.h"

#include <QtCore/QByteArray>
#include <QtCore/QXmlStreamReader>

#include <cstring>

namespace onvifsim {
namespace {

bool isXmlSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// 解析一遍，成功返回根元素。errorOut 非空即失败。
XmlNode parseOnce(const QByteArray &data, QString *errorOut)
{
    QXmlStreamReader reader(data);
    XmlNode root;
    while (!reader.atEnd()) {
        if (reader.readNext() == QXmlStreamReader::StartElement) {
            root = xml::readElement(reader);
            break;
        }
    }
    // 根元素读完后继续读到底：尾部残缺 / 多余根元素也要算解析失败，
    // 否则畸形报文会被当成半棵好树用下去。
    while (!reader.atEnd() && !reader.hasError())
        reader.readNext();

    if (reader.hasError()) {
        if (errorOut) {
            *errorOut = QStringLiteral("XML parse error at line %1: %2")
                            .arg(reader.lineNumber())
                            .arg(reader.errorString());
        }
        return XmlNode();
    }
    if (root.isNull() && errorOut)
        *errorOut = QStringLiteral("XML has no root element");
    return root;
}

} // namespace

const XmlNode *XmlNode::child(const QString &childName) const
{
    for (const XmlNode &c : children) {
        if (c.name == childName)
            return &c;
    }
    return nullptr;
}

const XmlNode *XmlNode::child(const QString &childNs, const QString &childName) const
{
    for (const XmlNode &c : children) {
        if (c.name == childName && c.ns == childNs)
            return &c;
    }
    return nullptr;
}

QVector<const XmlNode *> XmlNode::childrenNamed(const QString &childName) const
{
    QVector<const XmlNode *> found;
    for (const XmlNode &c : children) {
        if (c.name == childName)
            found.append(&c);
    }
    return found;
}

const XmlNode *XmlNode::path(const QString &slashSeparated) const
{
    const XmlNode *node = this;
    // 允许 "a//b" 与首尾斜杠，客户端拼路径时多一道斜杠很常见。
    const QStringList parts = slashSeparated.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        node = node->child(part);
        if (!node)
            return nullptr;
    }
    return node == this ? nullptr : node;
}

QString XmlNode::attribute(const QString &attrName, const QString &fallback) const
{
    for (const XmlAttribute &a : attributes) {
        if (a.name == attrName)
            return a.value;
    }
    return fallback;
}

QString XmlNode::childText(const QString &childName, const QString &fallback) const
{
    const XmlNode *c = child(childName);
    return c ? c->text : fallback;
}

int XmlNode::childInt(const QString &childName, int fallback) const
{
    const XmlNode *c = child(childName);
    if (!c)
        return fallback;
    bool ok = false;
    const int v = c->text.toInt(&ok);
    return ok ? v : fallback;
}

double XmlNode::childDouble(const QString &childName, double fallback) const
{
    const XmlNode *c = child(childName);
    if (!c)
        return fallback;
    bool ok = false;
    const double v = c->text.toDouble(&ok);
    return ok ? v : fallback;
}

bool XmlNode::childBool(const QString &childName, bool fallback) const
{
    const XmlNode *c = child(childName);
    if (!c)
        return fallback;
    // xs:boolean 的四种字面量都要认；真机还会发 "True" 这类大小写混写。
    const QString v = c->text.trimmed().toLower();
    if (v == QLatin1String("true") || v == QLatin1String("1"))
        return true;
    if (v == QLatin1String("false") || v == QLatin1String("0"))
        return false;
    return fallback;
}

bool XmlNode::hasChild(const QString &childName) const
{
    return child(childName) != nullptr;
}

QString XmlNode::toDebugString(int indent) const
{
    QString out(indent, QLatin1Char(' '));
    out += QLatin1Char('<') + name;
    for (const XmlAttribute &a : attributes)
        out += QLatin1Char(' ') + a.name + QStringLiteral("=\"") + a.value + QLatin1Char('"');
    out += QLatin1Char('>');
    if (!text.isEmpty())
        out += text;
    out += QLatin1Char('\n');
    for (const XmlNode &c : children)
        out += c.toDebugString(indent + 2);
    return out;
}

namespace xml {

XmlNode parse(const QByteArray &data, QString *errorOut)
{
    if (errorOut)
        errorOut->clear();

    QString firstError;
    XmlNode root = parseOnce(data, &firstError);
    if (firstError.isEmpty())
        return root;

    // 真机（TP-Link TL-IPC 的 GetEventProperties）会吐属性值不加引号的非法 XML，
    // 参照客户端的做法是先补引号再解析一次 —— 这里照做，模拟器自己也要收得下。
    QString repairError;
    const XmlNode repaired = parseOnce(repairUnquotedAttributes(data), &repairError);
    if (repairError.isEmpty())
        return repaired;

    // 补引号仍解析不了，报第一次的错（那个才对应原始报文的位置）。
    if (errorOut)
        *errorOut = firstError;
    return XmlNode();
}

XmlNode readElement(QXmlStreamReader &reader)
{
    XmlNode node;
    node.ns = reader.namespaceUri().toString();
    node.name = reader.name().toString();

    const QXmlStreamAttributes attrs = reader.attributes();
    node.attributes.reserve(attrs.size());
    for (const QXmlStreamAttribute &a : attrs) {
        XmlAttribute attr;
        attr.ns = a.namespaceUri().toString();
        attr.name = a.name().toString();
        attr.value = a.value().toString();
        node.attributes.append(attr);
    }

    QString text;
    while (!reader.atEnd()) {
        switch (reader.readNext()) {
        case QXmlStreamReader::StartElement:
            node.children.append(readElement(reader));
            break;
        case QXmlStreamReader::EndElement:
            node.text = text.trimmed();
            return node;
        case QXmlStreamReader::Characters:
        case QXmlStreamReader::EntityReference:
            // 缩进空白不算文本；混合内容里被子元素切开的文本段直接拼起来。
            if (!reader.isWhitespace())
                text += reader.text();
            break;
        case QXmlStreamReader::Invalid:
            node.text = text.trimmed();
            return node;
        default:
            break;
        }
    }
    node.text = text.trimmed();
    return node;
}

QByteArray repairUnquotedAttributes(const QByteArray &data)
{
    const int n = data.size();
    QByteArray out;
    out.reserve(n + 32);

    const char *raw = data.constData();
    auto startsWith = [raw, n](int pos, const char *token) {
        const int len = static_cast<int>(std::strlen(token));
        return pos + len <= n && std::memcmp(raw + pos, token, static_cast<size_t>(len)) == 0;
    };
    // 原样透传 [pos, end)，end 越界时透传到结尾。
    auto passThrough = [&out, &data, n](int pos, int end) {
        const int stop = (end < 0 || end > n) ? n : end;
        out.append(data.mid(pos, stop - pos));
        return stop;
    };

    int i = 0;
    while (i < n) {
        if (raw[i] != '<') {
            out.append(raw[i]);
            ++i;
            continue;
        }
        // 注释 / CDATA / 处理指令 / DOCTYPE 里的 '=' 不是属性，整段照抄。
        if (startsWith(i, "<!--")) {
            const int e = data.indexOf("-->", i);
            i = passThrough(i, e < 0 ? -1 : e + 3);
            continue;
        }
        if (startsWith(i, "<![CDATA[")) {
            const int e = data.indexOf("]]>", i);
            i = passThrough(i, e < 0 ? -1 : e + 3);
            continue;
        }
        if (startsWith(i, "<?") || startsWith(i, "<!")) {
            const int e = data.indexOf('>', i);
            i = passThrough(i, e < 0 ? -1 : e + 1);
            continue;
        }

        out.append('<');
        ++i;
        // 元素名（结束标签的 '/' 也一并抄走）
        while (i < n && !isXmlSpace(raw[i]) && raw[i] != '>')
            out.append(raw[i++]);

        while (i < n && raw[i] != '>') {
            if (isXmlSpace(raw[i]) || raw[i] == '/') {
                out.append(raw[i++]);
                continue;
            }
            // 属性名
            while (i < n && !isXmlSpace(raw[i]) && raw[i] != '=' && raw[i] != '>' && raw[i] != '/')
                out.append(raw[i++]);
            // 名字与 '=' 之间允许空白；没有 '=' 说明是个无值属性，交给外层循环照抄。
            int eq = i;
            while (eq < n && isXmlSpace(raw[eq]))
                ++eq;
            if (eq >= n || raw[eq] != '=')
                continue;
            while (i < eq)
                out.append(raw[i++]);
            out.append('=');
            ++i;
            while (i < n && isXmlSpace(raw[i]))
                out.append(raw[i++]);

            if (i < n && (raw[i] == '"' || raw[i] == '\'')) {
                const char quote = raw[i];
                out.append(raw[i++]);
                while (i < n && raw[i] != quote)
                    out.append(raw[i++]);
                if (i < n)
                    out.append(raw[i++]);
                continue;
            }
            // 这里才是要修的：值没引号，读到空白或 '>' 为止。
            QByteArray value;
            while (i < n && !isXmlSpace(raw[i]) && raw[i] != '>')
                value.append(raw[i++]);
            QByteArray tail;
            if (value.endsWith('/') && i < n && raw[i] == '>') {
                // "topic=true/>" 里的 '/' 属于标签而不属于值。
                value.chop(1);
                tail = "/";
            }
            const char quote = value.contains('"') ? '\'' : '"';
            out.append(quote);
            out.append(value);
            out.append(quote);
            out.append(tail);
        }
        if (i < n)
            out.append(raw[i++]);   // '>'
    }
    return out;
}

QString escape(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '&': out += QLatin1String("&amp;"); break;
        case '<': out += QLatin1String("&lt;"); break;
        case '>': out += QLatin1String("&gt;"); break;
        default: out += c; break;
        }
    }
    return out;
}

QString escapeAttribute(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '&': out += QLatin1String("&amp;"); break;
        case '<': out += QLatin1String("&lt;"); break;
        case '>': out += QLatin1String("&gt;"); break;
        case '"': out += QLatin1String("&quot;"); break;
        case '\'': out += QLatin1String("&apos;"); break;
        // 属性值里的裸换行/制表符按规范要归一成空格，这里编码掉以免丢失。
        case '\n': out += QLatin1String("&#10;"); break;
        case '\r': out += QLatin1String("&#13;"); break;
        case '\t': out += QLatin1String("&#9;"); break;
        default: out += c; break;
        }
    }
    return out;
}

} // namespace xml
} // namespace onvifsim
