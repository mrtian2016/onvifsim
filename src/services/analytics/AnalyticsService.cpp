#include "services/analytics/AnalyticsService.h"

#include "core/VirtualCamera.h"
#include "events/EventEngine.h"
#include "soap/Namespaces.h"

#include <QtCore/QVector>

namespace onvifsim {
namespace {

// 规则引擎的类型名（tt:Config 的 Type 属性，是个 QName）。
// 已知的几类写死，免得从 topic 反推出「tt:Audio」这种莫名其妙的类型；
// 其余按 topic 的倒数第二段推，四套命名风格都能落到一个像样的名字上。
QString ruleTypeFor(const TopicDef &def)
{
    switch (def.kind) {
    case EventKind::Motion:         return QStringLiteral("tt:CellMotionDetector");
    case EventKind::LineCrossing:   return QStringLiteral("tt:LineDetector");
    case EventKind::FieldIntrusion: return QStringLiteral("tt:FieldDetector");
    case EventKind::Tamper:         return QStringLiteral("tt:TamperDetector");
    case EventKind::AudioDetected:  return QStringLiteral("tt:AudioDetector");
    // 人 / 车 / 动物在标准 topic 里共用 ObjectDetector 那一段，直接按 topic 推
    // 会推出三条同名规则；客户端按 Type 建索引时后两条会把前面的顶掉。
    case EventKind::PeopleDetect:   return QStringLiteral("tt:PeopleDetector");
    case EventKind::VehicleDetect:  return QStringLiteral("tt:VehicleDetector");
    case EventKind::AnimalDetect:   return QStringLiteral("tt:AnimalDetector");
    case EventKind::FaceDetect:     return QStringLiteral("tt:FaceDetector");
    default:                        break;
    }
    const QStringList parts = def.topic.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() >= 2)
        return QStringLiteral("tt:") + parts.at(parts.size() - 2);
    return QStringLiteral("tt:RuleDetector");
}

// 只有挂了 Rule 名的 topic 才算「规则」——RuleEngine 那一族。
// tns1:Device/Trigger/* 与 tns1:Monitoring/* 是设备自己上报的，不归 Analytics 管。
QVector<TopicDef> ruleTopics(const SoapContext &ctx)
{
    QVector<TopicDef> rules;
    EventEngine *events = ctx.camera->events();
    if (!events)
        return rules;
    for (const TopicDef &def : events->topics()) {
        if (!def.ruleItemValue.isEmpty())
            rules.append(def);
    }
    return rules;
}

void writeSimpleItemDescription(XmlWriter &w, const QString &name, const QString &type)
{
    XmlWriter::Scope s(w, QStringLiteral("tt:SimpleItemDescription"));
    w.attr(QStringLiteral("Name"), name);
    w.attr(QStringLiteral("Type"), type);
}

void writeSimpleItem(XmlWriter &w, const QString &name, const QString &value)
{
    XmlWriter::Scope s(w, QStringLiteral("tt:SimpleItem"));
    w.attr(QStringLiteral("Name"), name);
    w.attr(QStringLiteral("Value"), value);
}

// Data 项的 xs 类型：状态位是 boolean，ObjectId 是 int，其余按字串处理。
QString dataItemType(const TopicDef &def)
{
    if (def.dataItemName == QLatin1String("ObjectId"))
        return QStringLiteral("xs:int");
    if (def.dataItemName == QLatin1String("Value"))
        return QStringLiteral("xs:float");
    return def.isProperty ? QStringLiteral("xs:boolean") : QStringLiteral("xs:string");
}

} // namespace

const char *AnalyticsService::serviceNamespace() const
{
    return ns::Analytics;
}

const char *AnalyticsService::serviceName() const
{
    return "analytics";
}

QString AnalyticsService::defaultPath() const
{
    return QStringLiteral("/onvif/analytics_service");
}

void AnalyticsService::writeServiceCapabilities(SoapContext &ctx) const
{
    XmlWriter &w = *ctx.out;
    XmlWriter::Scope caps(w, QStringLiteral("tan:Capabilities"));
    w.attr(QStringLiteral("RuleSupport"), QStringLiteral("true"));
    w.attr(QStringLiteral("AnalyticsModuleSupport"), QStringLiteral("true"));
    w.attr(QStringLiteral("CellBasedSceneDescriptionSupported"), QStringLiteral("false"));
    w.attr(QStringLiteral("RuleOptionsSupported"), QStringLiteral("false"));
}

AnalyticsService::AnalyticsService()
{
    cameraOp("GetServiceCapabilities", AuthLevel::PreAuth, [this](SoapContext &ctx) {
        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tan:GetServiceCapabilitiesResponse"));
        writeServiceCapabilities(ctx);
    });

    // ConfigurationToken 在 WSDL 里是必填的，但真机对它普遍不较真，
    // 而 Media 侧的 VideoAnalyticsConfiguration token 由别的服务定义 ——
    // 这里刻意不校验，宁可多答也不要因为 token 对不上让客户端拿不到规则。
    cameraOp("GetSupportedRules", AuthLevel::User, [](SoapContext &ctx) {
        const QVector<TopicDef> rules = ruleTopics(ctx);

        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tan:GetSupportedRulesResponse"));
        XmlWriter::Scope sup(*ctx.out, QStringLiteral("tan:SupportedRules"));
        ctx.out->element(QStringLiteral("tt:RuleContentSchemaLocation"),
                         QStringLiteral("http://www.onvif.org/ver10/schema/onvif.xsd"));
        for (const TopicDef &def : rules) {
            XmlWriter::Scope desc(*ctx.out, QStringLiteral("tt:RuleDescription"));
            ctx.out->attr(QStringLiteral("Name"), ruleTypeFor(def));
            {
                XmlWriter::Scope params(*ctx.out, QStringLiteral("tt:Parameters"));
                writeSimpleItemDescription(*ctx.out, QStringLiteral("Sensitivity"),
                                           QStringLiteral("xs:int"));
            }
            XmlWriter::Scope msg(*ctx.out, QStringLiteral("tt:Messages"));
            ctx.out->attr(QStringLiteral("IsProperty"),
                          def.isProperty ? QStringLiteral("true") : QStringLiteral("false"));
            {
                XmlWriter::Scope src(*ctx.out, QStringLiteral("tt:Source"));
                writeSimpleItemDescription(*ctx.out, def.sourceItemName,
                                           QStringLiteral("tt:ReferenceToken"));
            }
            {
                XmlWriter::Scope data(*ctx.out, QStringLiteral("tt:Data"));
                writeSimpleItemDescription(*ctx.out, def.dataItemName, dataItemType(def));
            }
            // ParentTopic 就是事件层真正会推的那个 topic 串，两边必须一模一样。
            ctx.out->element(QStringLiteral("tt:ParentTopic"), def.topic);
        }
    });

    cameraOp("GetRules", AuthLevel::User, [](SoapContext &ctx) {
        const QVector<TopicDef> rules = ruleTopics(ctx);

        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tan:GetRulesResponse"));
        for (const TopicDef &def : rules) {
            XmlWriter::Scope rule(*ctx.out, QStringLiteral("tan:Rule"));
            // Name 用事件消息里 Rule 这一项的取值，客户端把两边对起来才认得出
            // 「刚报警的就是我列表里的这条规则」。
            ctx.out->attr(QStringLiteral("Name"), def.ruleItemValue);
            ctx.out->attr(QStringLiteral("Type"), ruleTypeFor(def));
            XmlWriter::Scope params(*ctx.out, QStringLiteral("tt:Parameters"));
            writeSimpleItem(*ctx.out, QStringLiteral("Sensitivity"), QStringLiteral("50"));
        }
    });

    cameraOp("GetAnalyticsModules", AuthLevel::User, [](SoapContext &ctx) {
        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tan:GetAnalyticsModulesResponse"));
        {
            XmlWriter::Scope mod(*ctx.out, QStringLiteral("tan:AnalyticsModule"));
            ctx.out->attr(QStringLiteral("Name"), QStringLiteral("MyCellMotionModule"));
            ctx.out->attr(QStringLiteral("Type"), QStringLiteral("tt:CellMotionEngine"));
            XmlWriter::Scope params(*ctx.out, QStringLiteral("tt:Parameters"));
            writeSimpleItem(*ctx.out, QStringLiteral("Sensitivity"), QStringLiteral("50"));
            writeSimpleItem(*ctx.out, QStringLiteral("Layout"), QStringLiteral("22x18"));
        }
        {
            XmlWriter::Scope mod(*ctx.out, QStringLiteral("tan:AnalyticsModule"));
            ctx.out->attr(QStringLiteral("Name"), QStringLiteral("MyObjectTrackerModule"));
            ctx.out->attr(QStringLiteral("Type"), QStringLiteral("tt:ObjectTracker"));
            XmlWriter::Scope params(*ctx.out, QStringLiteral("tt:Parameters"));
            writeSimpleItem(*ctx.out, QStringLiteral("MinFrames"), QStringLiteral("5"));
        }
    });
}

namespace services {

SoapService *createAnalytics()
{
    return new AnalyticsService;
}

} // namespace services
} // namespace onvifsim
