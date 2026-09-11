#include "events/EventTypes.h"

#include <QtCore/QCoreApplication>

#include "core/CameraModel.h"

#include "core/Quirks.h"

#include <QtCore/QHash>

namespace onvifsim {

// 单元测试接缝：事件层的 camera 指针为空只会发生在单测里（VirtualCamera 要
// 拖上 HttpServer / RtspServer 一整套，单测不值得建）。此时 quirk 从这份进程级
// 副本读，测试就能不建相机直接驱动 D1 / D3 / D9 / D10 / D11 各分支。
// 故意不写进公共头：生产代码永远有相机，不该看见它。
Quirks &eventsFallbackQuirks()
{
    static Quirks q;
    return q;
}

namespace {

TopicDef makeTopic(EventKind kind, const QString &topic, bool isProperty,
                   const QString &sourceName, const QString &sourceValue,
                   const QString &ruleName, const QString &ruleValue,
                   const QString &dataName, const QString &display)
{
    TopicDef d;
    d.kind = kind;
    d.topic = topic;
    d.isProperty = isProperty;
    d.sourceItemName = sourceName;
    d.sourceItemValue = sourceValue;
    d.ruleItemName = ruleName;
    d.ruleItemValue = ruleValue;
    d.dataItemName = dataName;
    d.displayName = display;
    return d;
}

// ---- 标准 ONVIF ---------------------------------------------------------
// 名字与 Core / Analytics 规范一致，SimpleItem 也按规范摆：分析类挂
// VideoAnalyticsConfigurationToken + Rule，视频源类挂 VideoSourceConfigurationToken。
QVector<TopicDef> buildOnvif()
{
    const QString vs = QStringLiteral("VideoSourceConfigurationToken");
    const QString vsv = QStringLiteral("VideoSourceConfig");
    const QString va = QStringLiteral("VideoAnalyticsConfigurationToken");
    const QString vav = QStringLiteral("VideoAnalyticsConfig");
    const QString rule = QStringLiteral("Rule");
    const QString state = QStringLiteral("State");

    QVector<TopicDef> v;
    v << makeTopic(EventKind::Motion, QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion"),
                   true, va, vav, rule, QStringLiteral("MyMotionDetectorRule"),
                   QStringLiteral("IsMotion"), eventKindDisplayName(EventKind::Motion))
      << makeTopic(EventKind::MotionAlarm, QStringLiteral("tns1:VideoSource/MotionAlarm"), true,
                   vs, vsv, QString(), QString(), state, eventKindDisplayName(EventKind::MotionAlarm))
      // Crossed 在规范里是瞬时事件，Data 是 ObjectId 而不是状态位。
      << makeTopic(EventKind::LineCrossing, QStringLiteral("tns1:RuleEngine/LineDetector/Crossed"),
                   false, va, vav, rule, QStringLiteral("MyLineDetectorRule"),
                   QStringLiteral("ObjectId"), eventKindDisplayName(EventKind::LineCrossing))
      << makeTopic(EventKind::FieldIntrusion,
                   QStringLiteral("tns1:RuleEngine/FieldDetector/ObjectsInside"), true, va, vav,
                   rule, QStringLiteral("MyFieldDetectorRule"), QStringLiteral("IsInside"),
                   eventKindDisplayName(EventKind::FieldIntrusion))
      << makeTopic(EventKind::Tamper, QStringLiteral("tns1:RuleEngine/TamperDetector/Tamper"), true,
                   va, vav, rule, QStringLiteral("MyTamperDetectorRule"),
                   QStringLiteral("IsTamper"), eventKindDisplayName(EventKind::Tamper))
      << makeTopic(EventKind::SceneChange, QStringLiteral("tns1:VideoSource/GlobalSceneChange"),
                   true, vs, vsv, QString(), QString(), state,
                   eventKindDisplayName(EventKind::SceneChange))
      << makeTopic(EventKind::AudioDetected,
                   QStringLiteral("tns1:AudioAnalytics/Audio/DetectedSound"), true,
                   QStringLiteral("AudioSourceConfigurationToken"),
                   QStringLiteral("AudioSourceConfig"), rule,
                   QStringLiteral("MyAudioDetectorRule"), state,
                   eventKindDisplayName(EventKind::AudioDetected))
      << makeTopic(EventKind::ImageTooDark, QStringLiteral("tns1:VideoSource/ImageTooDark"), true,
                   vs, vsv, QString(), QString(), state, eventKindDisplayName(EventKind::ImageTooDark))
      << makeTopic(EventKind::DigitalInput, QStringLiteral("tns1:Device/Trigger/DigitalInput"),
                   true, QStringLiteral("InputToken"), QLatin1String(iotoken::DigitalInput), QString(),
                   QString(), QStringLiteral("LogicalState"),
                   eventKindDisplayName(EventKind::DigitalInput))
      << makeTopic(EventKind::RelayOutput, QStringLiteral("tns1:Device/Trigger/Relay"), true,
                   QStringLiteral("RelayToken"), QLatin1String(iotoken::Relay), QString(),
                   QString(), QStringLiteral("LogicalState"),
                   eventKindDisplayName(EventKind::RelayOutput))
      // Monitoring 是瞬时上报，Data 是数值不是状态位（EventEngine 特判 "Value"）。
      << makeTopic(EventKind::ProcessorUsage, QStringLiteral("tns1:Monitoring/ProcessorUsage"),
                   false, QStringLiteral("Token"), QStringLiteral("Processor_1"), QString(),
                   QString(), QStringLiteral("Value"), eventKindDisplayName(EventKind::ProcessorUsage))
      // 人 / 车 / 动物 / 人脸：分类放 Data 的 Type，与状态位并存。
      << makeTopic(EventKind::PeopleDetect,
                   QStringLiteral("tns1:RuleEngine/ObjectDetector/PeopleDetect"), true, va, vav,
                   rule, QStringLiteral("MyPeopleDetectorRule"), state,
                   eventKindDisplayName(EventKind::PeopleDetect))
      << makeTopic(EventKind::VehicleDetect,
                   QStringLiteral("tns1:RuleEngine/ObjectDetector/VehicleDetect"), true, va, vav,
                   rule, QStringLiteral("MyVehicleDetectorRule"), state,
                   eventKindDisplayName(EventKind::VehicleDetect))
      << makeTopic(EventKind::AnimalDetect,
                   QStringLiteral("tns1:RuleEngine/ObjectDetector/AnimalDetect"), true, va, vav,
                   rule, QStringLiteral("MyAnimalDetectorRule"), state,
                   eventKindDisplayName(EventKind::AnimalDetect))
      << makeTopic(EventKind::FaceDetect, QStringLiteral("tns1:RuleEngine/FaceDetector/FaceDetect"),
                   true, va, vav, rule, QStringLiteral("MyFaceDetectorRule"), state,
                   eventKindDisplayName(EventKind::FaceDetect));
    return v;
}

// ---- TP-Link（TL-IPC / VIGI）--------------------------------------------
// 真机实测：越界是 LineCrossDetector/LineCross，入侵是 IntrusionDetector/Intrusion，
// 与通用约定的 LineDetector/Crossed、FieldDetector/ObjectsInside 都对不上。
// 客户端的 topic 映射表必须靠子串包含才能兜住这一套。
QVector<TopicDef> buildTplink()
{
    const QString vs = QStringLiteral("VideoSourceConfigurationToken");
    const QString vsv = QStringLiteral("VideoSourceToken0");
    const QString rule = QStringLiteral("Rule");
    const QString state = QStringLiteral("State");

    QVector<TopicDef> v;
    v << makeTopic(EventKind::Motion, QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion"),
                   true, vs, vsv, rule, QStringLiteral("MyMotionDetectorRule"),
                   QStringLiteral("IsMotion"), eventKindDisplayName(EventKind::Motion))
      << makeTopic(EventKind::MotionAlarm, QStringLiteral("tns1:VideoSource/MotionAlarm"), true,
                   vs, vsv, QString(), QString(), state, eventKindDisplayName(EventKind::MotionAlarm))
      << makeTopic(EventKind::LineCrossing,
                   QStringLiteral("tns1:RuleEngine/LineCrossDetector/LineCross"), true, vs, vsv,
                   rule, QStringLiteral("MyLineCrossRule"), state,
                   eventKindDisplayName(EventKind::LineCrossing))
      << makeTopic(EventKind::FieldIntrusion,
                   QStringLiteral("tns1:RuleEngine/IntrusionDetector/Intrusion"), true, vs, vsv,
                   rule, QStringLiteral("MyIntrusionRule"), state,
                   eventKindDisplayName(EventKind::FieldIntrusion))
      << makeTopic(EventKind::Tamper, QStringLiteral("tns1:RuleEngine/TamperDetector/Tamper"), true,
                   vs, vsv, rule, QStringLiteral("MyTamperRule"), state,
                   eventKindDisplayName(EventKind::Tamper))
      << makeTopic(EventKind::SceneChange,
                   QStringLiteral("tns1:RuleEngine/SceneChangeDetector/SceneChange"), true, vs, vsv,
                   rule, QStringLiteral("MySceneChangeRule"), state,
                   eventKindDisplayName(EventKind::SceneChange))
      << makeTopic(EventKind::AudioDetected,
                   QStringLiteral("tns1:RuleEngine/AudioDetector/AudioDetect"), true, vs, vsv, rule,
                   QStringLiteral("MyAudioRule"), state, eventKindDisplayName(EventKind::AudioDetected))
      << makeTopic(EventKind::ImageTooDark, QStringLiteral("tns1:VideoSource/ImageTooDark"), true,
                   vs, vsv, QString(), QString(), state, eventKindDisplayName(EventKind::ImageTooDark))
      << makeTopic(EventKind::DigitalInput, QStringLiteral("tns1:Device/Trigger/DigitalInput"),
                   true, QStringLiteral("InputToken"), QStringLiteral("AlarmIn0"), QString(),
                   QString(), QStringLiteral("LogicalState"),
                   eventKindDisplayName(EventKind::DigitalInput))
      << makeTopic(EventKind::RelayOutput, QStringLiteral("tns1:Device/Trigger/Relay"), true,
                   QStringLiteral("RelayToken"), QStringLiteral("AlarmOut0"), QString(), QString(),
                   QStringLiteral("LogicalState"), eventKindDisplayName(EventKind::RelayOutput))
      << makeTopic(EventKind::ProcessorUsage, QStringLiteral("tns1:Monitoring/ProcessorUsage"),
                   false, QStringLiteral("Token"), QStringLiteral("CPU0"), QString(), QString(),
                   QStringLiteral("Value"), eventKindDisplayName(EventKind::ProcessorUsage))
      << makeTopic(EventKind::PeopleDetect,
                   QStringLiteral("tns1:RuleEngine/PeopleDetector/PeopleDetect"), true, vs, vsv,
                   rule, QStringLiteral("MyPeopleRule"), state,
                   eventKindDisplayName(EventKind::PeopleDetect))
      << makeTopic(EventKind::VehicleDetect,
                   QStringLiteral("tns1:RuleEngine/VehicleDetector/VehicleDetect"), true, vs, vsv,
                   rule, QStringLiteral("MyVehicleRule"), state,
                   eventKindDisplayName(EventKind::VehicleDetect))
      << makeTopic(EventKind::AnimalDetect, QStringLiteral("tns1:RuleEngine/PetDetector/PetDetect"),
                   true, vs, vsv, rule, QStringLiteral("MyPetRule"), state,
                   eventKindDisplayName(EventKind::AnimalDetect))
      << makeTopic(EventKind::FaceDetect, QStringLiteral("tns1:RuleEngine/FaceDetector/FaceDetect"),
                   true, vs, vsv, rule, QStringLiteral("MyFaceRule"), state,
                   eventKindDisplayName(EventKind::FaceDetect));
    return v;
}

// ---- Reolink（自定义 MyRuleDetector）------------------------------------
// AI 事件全挂在 MyRuleDetector 下，规则名的 SimpleItem 叫 RuleName 而不是 Rule ——
// 只认 "Rule" 的客户端会取不到规则名。
QVector<TopicDef> buildReolink()
{
    const QString vs = QStringLiteral("VideoSourceConfigurationToken");
    const QString vsv = QStringLiteral("000");
    const QString rule = QStringLiteral("RuleName");
    const QString state = QStringLiteral("State");
    const QString my = QStringLiteral("tns1:RuleEngine/MyRuleDetector/%1");

    QVector<TopicDef> v;
    v << makeTopic(EventKind::Motion, QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion"),
                   true, vs, vsv, rule, QStringLiteral("MyMotionDetectorRule"),
                   QStringLiteral("IsMotion"), eventKindDisplayName(EventKind::Motion))
      << makeTopic(EventKind::MotionAlarm, QStringLiteral("tns1:VideoSource/MotionAlarm"), true,
                   vs, vsv, QString(), QString(), state, eventKindDisplayName(EventKind::MotionAlarm))
      << makeTopic(EventKind::LineCrossing, my.arg(QStringLiteral("LineCrossDetect")), true, vs,
                   vsv, rule, QStringLiteral("MyLineCrossRule"), state,
                   eventKindDisplayName(EventKind::LineCrossing))
      << makeTopic(EventKind::FieldIntrusion, my.arg(QStringLiteral("IntrusionDetect")), true, vs,
                   vsv, rule, QStringLiteral("MyIntrusionRule"), state,
                   eventKindDisplayName(EventKind::FieldIntrusion))
      << makeTopic(EventKind::Tamper, QStringLiteral("tns1:RuleEngine/TamperDetector/Tamper"), true,
                   vs, vsv, rule, QStringLiteral("MyTamperRule"), QStringLiteral("IsTamper"),
                   eventKindDisplayName(EventKind::Tamper))
      << makeTopic(EventKind::SceneChange, QStringLiteral("tns1:VideoSource/GlobalSceneChange"),
                   true, vs, vsv, QString(), QString(), state,
                   eventKindDisplayName(EventKind::SceneChange))
      << makeTopic(EventKind::AudioDetected,
                   QStringLiteral("tns1:AudioAnalytics/Audio/DetectedSound"), true,
                   QStringLiteral("AudioSourceConfigurationToken"), QStringLiteral("000"), rule,
                   QStringLiteral("MyAudioRule"), state, eventKindDisplayName(EventKind::AudioDetected))
      << makeTopic(EventKind::ImageTooDark, QStringLiteral("tns1:VideoSource/ImageTooDark"), true,
                   vs, vsv, QString(), QString(), state, eventKindDisplayName(EventKind::ImageTooDark))
      << makeTopic(EventKind::DigitalInput, QStringLiteral("tns1:Device/Trigger/DigitalInput"),
                   true, QStringLiteral("InputToken"), QStringLiteral("AlarmIn"), QString(),
                   QString(), QStringLiteral("LogicalState"),
                   eventKindDisplayName(EventKind::DigitalInput))
      << makeTopic(EventKind::RelayOutput, QStringLiteral("tns1:Device/Trigger/Relay"), true,
                   QStringLiteral("RelayToken"), QStringLiteral("AlarmOut"), QString(), QString(),
                   QStringLiteral("LogicalState"), eventKindDisplayName(EventKind::RelayOutput))
      << makeTopic(EventKind::ProcessorUsage, QStringLiteral("tns1:Monitoring/ProcessorUsage"),
                   false, QStringLiteral("Token"), QStringLiteral("000"), QString(), QString(),
                   QStringLiteral("Value"), eventKindDisplayName(EventKind::ProcessorUsage))
      << makeTopic(EventKind::PeopleDetect, my.arg(QStringLiteral("PeopleDetect")), true, vs, vsv,
                   rule, QStringLiteral("MyPeopleRule"), state,
                   eventKindDisplayName(EventKind::PeopleDetect))
      << makeTopic(EventKind::VehicleDetect, my.arg(QStringLiteral("VehicleDetect")), true, vs, vsv,
                   rule, QStringLiteral("MyVehicleRule"), state,
                   eventKindDisplayName(EventKind::VehicleDetect))
      << makeTopic(EventKind::AnimalDetect, my.arg(QStringLiteral("DogCatDetect")), true, vs, vsv,
                   rule, QStringLiteral("MyDogCatRule"), state,
                   eventKindDisplayName(EventKind::AnimalDetect))
      << makeTopic(EventKind::FaceDetect, my.arg(QStringLiteral("FaceDetect")), true, vs, vsv, rule,
                   QStringLiteral("MyFaceRule"), state, eventKindDisplayName(EventKind::FaceDetect));
    return v;
}

// ---- Axis ---------------------------------------------------------------
// 自家事件全在 tnsaxis: 前缀下，状态位是小写的 active —— 只按 IsMotion / State
// 判状态的客户端会把「事件结束」当成一条新事件重复上报。
QVector<TopicDef> buildAxis()
{
    const QString vs = QStringLiteral("VideoSourceConfigurationToken");
    const QString vsv = QStringLiteral("0");
    const QString token = QStringLiteral("Token");
    const QString active = QStringLiteral("active");
    const QString state = QStringLiteral("State");
    const QString oa = QStringLiteral("tnsaxis:CameraApplicationPlatform/ObjectAnalytics/%1");

    QVector<TopicDef> v;
    v << makeTopic(EventKind::Motion,
                   QStringLiteral("tnsaxis:CameraApplicationPlatform/VMD/Camera1Profile1"), true,
                   token, QStringLiteral("Camera1Profile1"), QString(), QString(), active,
                   eventKindDisplayName(EventKind::Motion))
      << makeTopic(EventKind::MotionAlarm, QStringLiteral("tns1:VideoSource/MotionAlarm"), true,
                   vs, vsv, QString(), QString(), state, eventKindDisplayName(EventKind::MotionAlarm))
      << makeTopic(EventKind::LineCrossing, oa.arg(QStringLiteral("Device1Scenario1")), true, token,
                   QStringLiteral("Device1Scenario1"), QString(), QString(), active,
                   eventKindDisplayName(EventKind::LineCrossing))
      << makeTopic(EventKind::FieldIntrusion, oa.arg(QStringLiteral("Device1Scenario2")), true,
                   token, QStringLiteral("Device1Scenario2"), QString(), QString(), active,
                   eventKindDisplayName(EventKind::FieldIntrusion))
      << makeTopic(EventKind::Tamper, QStringLiteral("tnsaxis:VideoSource/Tampering"), true, token,
                   vsv, QString(), QString(), QStringLiteral("tampering"),
                   eventKindDisplayName(EventKind::Tamper))
      << makeTopic(EventKind::SceneChange,
                   QStringLiteral("tns1:VideoSource/GlobalSceneChange/ImagingService"), true, vs,
                   vsv, QString(), QString(), state, eventKindDisplayName(EventKind::SceneChange))
      << makeTopic(EventKind::AudioDetected, QStringLiteral("tnsaxis:AudioSource/TriggerLevel"),
                   true, token, QStringLiteral("1"), QString(), QString(),
                   QStringLiteral("triggered"), eventKindDisplayName(EventKind::AudioDetected))
      << makeTopic(EventKind::ImageTooDark,
                   QStringLiteral("tns1:VideoSource/ImageTooDark/ImagingService"), true, vs, vsv,
                   QString(), QString(), state, eventKindDisplayName(EventKind::ImageTooDark))
      << makeTopic(EventKind::DigitalInput, QStringLiteral("tns1:Device/Trigger/DigitalInput"),
                   true, QStringLiteral("InputToken"), QStringLiteral("1"), QString(), QString(),
                   QStringLiteral("LogicalState"), eventKindDisplayName(EventKind::DigitalInput))
      << makeTopic(EventKind::RelayOutput, QStringLiteral("tns1:Device/Trigger/Relay"), true,
                   QStringLiteral("RelayToken"), QStringLiteral("1"), QString(), QString(),
                   QStringLiteral("LogicalState"), eventKindDisplayName(EventKind::RelayOutput))
      << makeTopic(EventKind::ProcessorUsage, QStringLiteral("tns1:Monitoring/ProcessorUsage"),
                   false, token, QStringLiteral("1"), QString(), QString(),
                   QStringLiteral("Value"), eventKindDisplayName(EventKind::ProcessorUsage))
      << makeTopic(EventKind::PeopleDetect, oa.arg(QStringLiteral("PeopleDetect")), true, token,
                   QStringLiteral("Device1ScenarioHuman"), QString(), QString(), active,
                   eventKindDisplayName(EventKind::PeopleDetect))
      << makeTopic(EventKind::VehicleDetect, oa.arg(QStringLiteral("VehicleDetect")), true, token,
                   QStringLiteral("Device1ScenarioVehicle"), QString(), QString(), active,
                   eventKindDisplayName(EventKind::VehicleDetect))
      << makeTopic(EventKind::AnimalDetect, oa.arg(QStringLiteral("AnimalDetect")), true, token,
                   QStringLiteral("Device1ScenarioAnimal"), QString(), QString(), active,
                   eventKindDisplayName(EventKind::AnimalDetect))
      << makeTopic(EventKind::FaceDetect,
                   QStringLiteral("tnsaxis:CameraApplicationPlatform/FaceDetector/FaceDetect"),
                   true, token, QStringLiteral("1"), QString(), QString(), active,
                   eventKindDisplayName(EventKind::FaceDetect));
    return v;
}

// REST / 场景文件里的稳定键。改这里等于改外部契约。
QString kindKey(EventKind kind)
{
    switch (kind) {
    case EventKind::Motion:         return QStringLiteral("motion");
    case EventKind::MotionAlarm:    return QStringLiteral("motion_alarm");
    case EventKind::LineCrossing:   return QStringLiteral("line_crossing");
    case EventKind::FieldIntrusion: return QStringLiteral("field_intrusion");
    case EventKind::Tamper:         return QStringLiteral("tamper");
    case EventKind::SceneChange:    return QStringLiteral("scene_change");
    case EventKind::AudioDetected:  return QStringLiteral("audio_detected");
    case EventKind::ImageTooDark:   return QStringLiteral("image_too_dark");
    case EventKind::DigitalInput:   return QStringLiteral("digital_input");
    case EventKind::RelayOutput:    return QStringLiteral("relay_output");
    case EventKind::ProcessorUsage: return QStringLiteral("processor_usage");
    case EventKind::PeopleDetect:   return QStringLiteral("people_detect");
    case EventKind::VehicleDetect:  return QStringLiteral("vehicle_detect");
    case EventKind::AnimalDetect:   return QStringLiteral("animal_detect");
    case EventKind::FaceDetect:     return QStringLiteral("face_detect");
    case EventKind::Count:          break;
    }
    return QString();
}

// 归一化：大小写、下划线、连字符都不计。"MotionAlarm" 与 "motion_alarm" 同义。
QString normalizeName(const QString &name)
{
    QString out;
    out.reserve(name.size());
    for (const QChar c : name) {
        if (c == QLatin1Char('_') || c == QLatin1Char('-') || c == QLatin1Char(' '))
            continue;
        out.append(c.toLower());
    }
    return out;
}

// 一段 topic 去掉命名空间前缀。ONVIF 的前缀是可自定义的，客户端发来的表达式
// 里带不带 tns1: 都得认。
QString stripPrefix(const QString &segment)
{
    const int colon = segment.indexOf(QLatin1Char(':'));
    return colon >= 0 ? segment.mid(colon + 1) : segment;
}

} // namespace

// 四套风格共用的中文名，GUI 与 REST 的 topic 列表直接显示它。
QString eventKindDisplayName(EventKind kind)
{
    switch (kind) {
    case EventKind::Motion:         return QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::events", "移动侦测"));
    case EventKind::MotionAlarm:    return QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::events", "移动报警"));
    case EventKind::LineCrossing:   return QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::events", "越界侦测"));
    case EventKind::FieldIntrusion: return QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::events", "区域入侵"));
    case EventKind::Tamper:         return QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::events", "遮挡篡改"));
    case EventKind::SceneChange:    return QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::events", "场景变化"));
    case EventKind::AudioDetected:  return QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::events", "声音侦测"));
    case EventKind::ImageTooDark:   return QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::events", "画面过暗"));
    case EventKind::DigitalInput:   return QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::events", "数字输入"));
    case EventKind::RelayOutput:    return QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::events", "继电器输出"));
    case EventKind::ProcessorUsage: return QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::events", "CPU 占用"));
    case EventKind::PeopleDetect:   return QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::events", "人形侦测"));
    case EventKind::VehicleDetect:  return QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::events", "车辆侦测"));
    case EventKind::AnimalDetect:   return QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::events", "动物侦测"));
    case EventKind::FaceDetect:     return QStringLiteral(QT_TRANSLATE_NOOP("onvifsim::events", "人脸侦测"));
    case EventKind::Count:          break;
    }
    return QString();
}


QVector<TopicDef> TopicCatalog::topicsForStyle(const QString &style)
{
    const QString s = style.trimmed().toLower();
    if (s == QLatin1String("tplink") || s == QLatin1String("vigi"))
        return buildTplink();
    if (s == QLatin1String("reolink"))
        return buildReolink();
    if (s == QLatin1String("axis"))
        return buildAxis();
    return buildOnvif();
}

const TopicDef *TopicCatalog::findByKind(const QVector<TopicDef> &topics, EventKind kind)
{
    for (const TopicDef &d : topics) {
        if (d.kind == kind)
            return &d;
    }
    return nullptr;
}

QString TopicCatalog::kindName(EventKind kind)
{
    return kindKey(kind);
}

EventKind TopicCatalog::kindFromName(const QString &name, bool *ok)
{
    static const QHash<QString, EventKind> table = [] {
        QHash<QString, EventKind> t;
        for (int i = 0; i < static_cast<int>(EventKind::Count); ++i) {
            const EventKind k = static_cast<EventKind>(i);
            t.insert(normalizeName(kindKey(k)), k);
        }
        return t;
    }();

    const auto it = table.constFind(normalizeName(name));
    if (it != table.constEnd()) {
        if (ok)
            *ok = true;
        return it.value();
    }
    if (ok)
        *ok = false;
    return EventKind::Motion;
}

QStringList TopicCatalog::styles()
{
    return { QStringLiteral("onvif"), QStringLiteral("tplink"), QStringLiteral("reolink"),
             QStringLiteral("axis") };
}

// ---- TopicFilter --------------------------------------------------------

TopicFilter::TopicFilter() = default;

TopicFilter::TopicFilter(const QString &expression)
    : m_expression(expression.trimmed())
{
    if (m_expression.isEmpty())
        return;   // 空表达式 = 全收

    // ConcreteSet 方言：多条表达式用 '|' 分隔，"//." 表示「这棵子树全要」。
    const QStringList raw = m_expression.split(QLatin1Char('|'));
    for (const QString &item : raw) {
        const QString p = item.simplified().remove(QLatin1Char(' '));
        if (p.isEmpty()) {
            // "a||b" 这种写法真机上见过，别为它整条拒绝。
            m_valid = false;
            m_error = QStringLiteral("表达式里有空的子项");
            continue;
        }
        if (p == QLatin1String("//.") || p == QLatin1String("//*") || p == QLatin1String("*")
            || p == QLatin1String(".")) {
            m_matchAll = true;
            m_patterns.clear();
            return;
        }
        if (p.contains(QLatin1Char('[')) || p.contains(QLatin1Char('('))) {
            // 完整 XPath（谓词、函数）不支持；真机也普遍不支持，退化成全收。
            m_valid = false;
            m_error = QStringLiteral("不支持的 XPath 构造：%1").arg(p);
            continue;
        }
        m_patterns.append(p);
    }
    m_matchAll = m_patterns.isEmpty();
}

bool TopicFilter::isEmpty() const
{
    return m_patterns.isEmpty();
}

bool TopicFilter::matches(const QString &topic) const
{
    if (m_matchAll || m_patterns.isEmpty())
        return true;

    const QStringList actual = topic.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (actual.isEmpty())
        return false;

    for (const QString &pattern : m_patterns) {
        QString p = pattern;
        // "tns1:RuleEngine//." = 以该路径打头的整棵子树。
        bool prefixOnly = false;
        if (p.endsWith(QLatin1String("//.")) || p.endsWith(QLatin1String("//*"))) {
            p.chop(3);
            prefixOnly = true;
        }
        const QStringList want = p.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (want.isEmpty())
            return true;
        if (actual.size() < want.size())
            continue;
        if (!prefixOnly && actual.size() != want.size())
            continue;

        bool hit = true;
        for (int i = 0; i < want.size(); ++i) {
            const QString w = stripPrefix(want.at(i));
            if (w == QLatin1String("*"))
                continue;
            // 大小写不敏感：真机与客户端在 topic 大小写上都不严谨。
            if (stripPrefix(actual.at(i)).compare(w, Qt::CaseInsensitive) != 0) {
                hit = false;
                break;
            }
        }
        if (hit)
            return true;
    }
    return false;
}

QString TopicFilter::expression() const
{
    return m_expression;
}

bool TopicFilter::isValid() const
{
    return m_valid;
}

QString TopicFilter::parseError() const
{
    return m_error;
}

} // namespace onvifsim
