#pragma once

// 事件与 topic 的通用类型。四套命名风格（quirk D9）的映射表也在这里。

#include <QtCore/QDateTime>
#include <QtCore/QMetaType>
#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

namespace onvifsim {

// 事件的语义种类。具体 topic 串按命名风格从 TopicCatalog 取。
enum class EventKind {
    Motion,             // tns1:RuleEngine/CellMotionDetector/Motion
    MotionAlarm,        // tns1:VideoSource/MotionAlarm
    LineCrossing,
    FieldIntrusion,
    Tamper,
    SceneChange,
    AudioDetected,
    ImageTooDark,
    DigitalInput,
    RelayOutput,
    ProcessorUsage,
    PeopleDetect,
    VehicleDetect,
    AnimalDetect,
    FaceDetect,
    Count
};

// 事件类别的中文显示名（"移动侦测" …）。**这是要翻译的数据** ——
// 界面上显示前必须经 gui/I18n.h 的 topicName()，直接拿去 setText
// 会让英文界面出现一列中文（踩过）。
QString eventKindDisplayName(EventKind kind);

// 一条 topic 的完整声明。
struct TopicDef {
    EventKind kind = EventKind::Motion;
    QString topic;              // "tns1:RuleEngine/CellMotionDetector/Motion"
    bool isProperty = false;    // 属性型（成对发 true/false）还是瞬时型
    QString sourceItemName;     // "VideoSourceConfigurationToken" / "VideoAnalyticsConfigurationToken"
    QString sourceItemValue;
    QString ruleItemName;       // "Rule"，可空
    QString ruleItemValue;
    QString dataItemName;       // "IsMotion" / "State"
    QString displayName;        // GUI 上显示的中文名
};

// 一条待推送的事件消息。
struct EventMessage {
    QDateTime utcTime = QDateTime::currentDateTimeUtc();
    QString topic;
    bool isProperty = false;
    QString propertyOperation = QStringLiteral("Changed");  // Initialized / Changed / Deleted
    QMap<QString, QString> sourceItems;
    QMap<QString, QString> dataItems;
};

// 按命名风格给出 topic 集。风格见 quirk D9。
class TopicCatalog
{
public:
    // style: "onvif" / "tplink" / "reolink" / "axis"
    static QVector<TopicDef> topicsForStyle(const QString &style);
    static const TopicDef *findByKind(const QVector<TopicDef> &topics, EventKind kind);
    static QString kindName(EventKind kind);
    static EventKind kindFromName(const QString &name, bool *ok = nullptr);
    static QStringList styles();
};

// TopicExpression 过滤（ConcreteSet 方言，支持 "//." 与 "|" 分隔的多条）。
class TopicFilter
{
public:
    TopicFilter();
    explicit TopicFilter(const QString &expression);

    bool isEmpty() const;           // 空过滤器 = 全收
    bool matches(const QString &topic) const;
    QString expression() const;
    bool isValid() const;
    QString parseError() const;

private:
    QString m_expression;
    QStringList m_patterns;
    bool m_matchAll = true;
    bool m_valid = true;
    QString m_error;
};

} // namespace onvifsim

// 让 eventProduced 能走队列连接（事件引擎将来挪到别的线程时用得上）。
Q_DECLARE_METATYPE(onvifsim::EventMessage)
