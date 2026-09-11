#pragma once

// 轻量 XML 树。用 QXmlStreamReader 构建，供 handler 按名字取值。
// 不用 QDomDocument：一是不想拖 QtXml，二是响应侧要能生成故意畸形的 XML。
//
// 查找一律按 localName，需要区分时再带命名空间。ONVIF 请求里同名不同 ns 的情况极少，
// 但 Body 首元素的 ns 决定分发到哪个服务，那里必须带 ns 比较。

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

class QXmlStreamReader;

namespace onvifsim {

struct XmlAttribute {
    QString ns;
    QString name;
    QString value;
};

struct XmlNode {
    QString ns;
    QString name;      // localName
    QString text;      // 直接子文本，已合并去空白
    QVector<XmlAttribute> attributes;
    QVector<XmlNode> children;

    bool isNull() const { return name.isEmpty(); }

    const XmlNode *child(const QString &name) const;
    const XmlNode *child(const QString &ns, const QString &name) const;
    QVector<const XmlNode *> childrenNamed(const QString &name) const;

    // 支持 "a/b/c" 形式的路径，按 localName 逐级下钻。
    const XmlNode *path(const QString &slashSeparated) const;

    QString attribute(const QString &name, const QString &fallback = QString()) const;
    QString childText(const QString &name, const QString &fallback = QString()) const;
    int childInt(const QString &name, int fallback = 0) const;
    double childDouble(const QString &name, double fallback = 0.0) const;
    bool childBool(const QString &name, bool fallback = false) const;
    bool hasChild(const QString &name) const;

    QString toDebugString(int indent = 0) const;
};

namespace xml {

// 把整段 XML 解析成树。失败返回空节点并填 errorOut。
XmlNode parse(const QByteArray &data, QString *errorOut = nullptr);

// 从已经定位在某个 StartElement 上的 reader 读出该元素的子树。
XmlNode readElement(QXmlStreamReader &reader);

// 给标签内未加引号的属性值补上引号（quirk D5 的镜像操作，供测试与容错解析用）。
QByteArray repairUnquotedAttributes(const QByteArray &data);

QString escape(const QString &text);
QString escapeAttribute(const QString &text);

} // namespace xml
} // namespace onvifsim
