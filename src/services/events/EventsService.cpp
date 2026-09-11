#include "services/events/EventsService.h"

#include "core/VirtualCamera.h"
#include "events/EventEngine.h"
#include "events/SubscriptionManager.h"
#include "soap/Dispatcher.h"
#include "soap/Namespaces.h"
#include "soap/XmlNode.h"

#include <QtCore/QMap>
#include <QtCore/QPointer>

namespace onvifsim {
namespace {

// 参照客户端发的就是这几个值，拿它们当缺省最贴近真实流量。
constexpr int kDefaultMessageLimit = 100;
constexpr int kDefaultPullTimeoutMs = 8000;
constexpr int kMaxPullTimeoutMs = 300000;
constexpr int kDefaultMaxPullPoints = 10;

constexpr const char *kConcreteSetDialect =
    "http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet";
constexpr const char *kConcreteDialect =
    "http://docs.oasis-open.org/wsn/t-1/TopicExpression/Concrete";
constexpr const char *kItemFilterDialect =
    "http://www.onvif.org/ver10/tev/messageContentFilter/ItemFilter";
constexpr const char *kTopicNamespaceLocation =
    "http://www.onvif.org/onvif/ver10/topics/topicns.xml";
constexpr const char *kMessageSchemaLocation =
    "http://www.onvif.org/onvif/ver10/schema/onvif.xsd";

// wsnt / wstop / wsa / tns1 由 declareServicePrefixes() 声明在 Envelope 上。
// tnsaxis 不在那张表里（只有 D9 的 axis 风格 topic 用得到），信封已经开好了补不进去，
// 只能挂在本层根元素上 —— 一样是合法 XML，客户端照收。
// 必须在 start() 之后、写任何子内容之前调用。
void declareEventPrefixes(XmlWriter &w)
{
    w.attr(QStringLiteral("xmlns:tnsaxis"), QString::fromLatin1(ns::Axis));
}

QString isoUtc(const QDateTime &dt)
{
    return dt.toUTC().toString(Qt::ISODate);
}

// 一条 NotificationMessage。客户端把 Message/Source 与 Message/Data 下的 SimpleItem
// 展平成一个 dict，所以两边都要如实写出来（人车分类在 Data 的 Type，规则名在 Source 的 Rule）。
void writeNotificationMessage(XmlWriter &w, const EventMessage &message)
{
    XmlWriter::Scope note(w, QStringLiteral("wsnt:NotificationMessage"));
    w.start(QStringLiteral("wsnt:Topic"));
    w.attr(QStringLiteral("Dialect"), QString::fromLatin1(kConcreteDialect));
    w.text(message.topic);
    w.end();
    {
        XmlWriter::Scope wrapper(w, QStringLiteral("wsnt:Message"));
        w.start(QStringLiteral("tt:Message"));
        w.attr(QStringLiteral("UtcTime"), isoUtc(message.utcTime));
        if (message.isProperty)
            w.attr(QStringLiteral("PropertyOperation"), message.propertyOperation);
        if (!message.sourceItems.isEmpty()) {
            XmlWriter::Scope source(w, QStringLiteral("tt:Source"));
            for (auto it = message.sourceItems.constBegin();
                 it != message.sourceItems.constEnd(); ++it) {
                w.start(QStringLiteral("tt:SimpleItem"));
                w.attr(QStringLiteral("Name"), it.key());
                w.attr(QStringLiteral("Value"), it.value());
                w.end();
            }
        }
        if (!message.dataItems.isEmpty()) {
            XmlWriter::Scope data(w, QStringLiteral("tt:Data"));
            for (auto it = message.dataItems.constBegin();
                 it != message.dataItems.constEnd(); ++it) {
                w.start(QStringLiteral("tt:SimpleItem"));
                w.attr(QStringLiteral("Name"), it.key());
                w.attr(QStringLiteral("Value"), it.value());
                w.end();
            }
        }
        w.end();
    }
}

// 长轮询回来时已经没有 SoapContext 了（handler 早返回了），所以这里从零建一个
// XmlWriter 把整封响应渲染出来。
QByteArray renderPullMessagesResponse(int soapVersion, const QDateTime &current,
                                      const QDateTime &termination,
                                      const QVector<EventMessage> &messages)
{
    XmlWriter w(soapVersion);
    w.declareServicePrefixes();   // tev / tt / wsnt / wstop / wsa / tns1 都在里面
    // 这条不在那张表里，而 axis 风格的 topic 串带 tnsaxis: 前缀，得自己补。
    w.declarePrefix(QStringLiteral("tnsaxis"), QString::fromLatin1(ns::Axis));
    w.startEnvelope();
    {
        XmlWriter::Scope response(w, QStringLiteral("tev:PullMessagesResponse"));
        w.element(QStringLiteral("tev:CurrentTime"), isoUtc(current));
        w.element(QStringLiteral("tev:TerminationTime"), isoUtc(termination));
        for (const EventMessage &message : messages)
            writeNotificationMessage(w, message);
    }
    w.endEnvelope();
    return w.take();
}

// 这次请求打的是哪条订阅。
// 正常路径是 HTTP 路径（客户端把设备给的订阅地址原样拿去 POST），
// wsa:To 是第二依据（同一条地址），都对不上再退到「只有一条订阅就是它」。
QString subscriptionKey(SoapContext &ctx)
{
    SubscriptionManager *mgr = ctx.camera->subscriptions();
    if (ctx.http && !ctx.http->path.isEmpty() && mgr->find(ctx.http->path))
        return ctx.http->path;
    if (ctx.soap && !ctx.soap->to.isEmpty() && mgr->find(ctx.soap->to))
        return ctx.soap->to;
    const QList<Subscription *> all = mgr->subscriptions();
    if (all.size() == 1)
        return all.first()->id;
    return ctx.http ? ctx.http->path : QString();
}

SoapFault subscriptionGoneFault()
{
    SoapFault f;
    f.subcode = QString::fromLatin1(ter::ResourceUnknownFault);
    f.reason = QStringLiteral("The subscription does not exist or has expired");
    return f;
}

// ---- GetEventProperties 的 TopicSet 树 ----------------------------------
// topic 串形如 "tns1:RuleEngine/CellMotionDetector/Motion"，按 '/' 拆成嵌套元素。
// 只有首段带命名空间前缀，后面几段真机上就是不带前缀的裸元素名。
struct TopicTreeNode {
    QMap<QString, TopicTreeNode> children;   // QMap 保证输出顺序稳定
    const TopicDef *leaf = nullptr;
};

const char *simpleItemType(const QString &name)
{
    if (name == QLatin1String("IsMotion") || name == QLatin1String("State")
        || name == QLatin1String("IsInside") || name == QLatin1String("Active")) {
        return "xs:boolean";
    }
    if (name == QLatin1String("ObjectId"))
        return "xs:integer";
    if (name == QLatin1String("Value"))
        return "xs:float";
    return "xs:string";
}

void writeMessageDescription(XmlWriter &w, const TopicDef &def)
{
    XmlWriter::Scope desc(w, QStringLiteral("tt:MessageDescription"));
    w.attr(QStringLiteral("IsProperty"),
           def.isProperty ? QStringLiteral("true") : QStringLiteral("false"));
    if (!def.sourceItemName.isEmpty() || !def.ruleItemName.isEmpty()) {
        XmlWriter::Scope source(w, QStringLiteral("tt:Source"));
        if (!def.sourceItemName.isEmpty()) {
            w.start(QStringLiteral("tt:SimpleItemDescription"));
            w.attr(QStringLiteral("Name"), def.sourceItemName);
            w.attr(QStringLiteral("Type"), QStringLiteral("tt:ReferenceToken"));
            w.end();
        }
        if (!def.ruleItemName.isEmpty()) {
            w.start(QStringLiteral("tt:SimpleItemDescription"));
            w.attr(QStringLiteral("Name"), def.ruleItemName);
            w.attr(QStringLiteral("Type"), QStringLiteral("xs:string"));
            w.end();
        }
    }
    if (!def.dataItemName.isEmpty()) {
        XmlWriter::Scope data(w, QStringLiteral("tt:Data"));
        w.start(QStringLiteral("tt:SimpleItemDescription"));
        w.attr(QStringLiteral("Name"), def.dataItemName);
        w.attr(QStringLiteral("Type"), QString::fromLatin1(simpleItemType(def.dataItemName)));
        w.end();
    }
}

void writeTopicTree(XmlWriter &w, const TopicTreeNode &node)
{
    for (auto it = node.children.constBegin(); it != node.children.constEnd(); ++it) {
        w.start(it.key());
        // 叶子标记：客户端的判据就是「带 topic="true" 或没有子 topic 元素」。
        // D5 打开时 XmlWriter 会把这个属性值写成不加引号的 topic=true —— 那正是真机的样子。
        if (it.value().leaf) {
            w.attr(QStringLiteral("wstop:topic"), QStringLiteral("true"));
            writeMessageDescription(w, *it.value().leaf);
        }
        writeTopicTree(w, it.value());
        w.end();
    }
}

void writeTopicSet(XmlWriter &w, const QVector<TopicDef> &topics)
{
    TopicTreeNode root;
    for (const TopicDef &def : topics) {
        const QStringList segments = def.topic.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (segments.isEmpty())
            continue;
        TopicTreeNode *cursor = &root;
        for (const QString &segment : segments)
            cursor = &cursor->children[segment];
        cursor->leaf = &def;
    }

    w.start(QStringLiteral("wstop:TopicSet"));
    writeTopicTree(w, root);
    w.end();
}

} // namespace

namespace events {

bool parseIsoDuration(const QString &text, qint64 *secondsOut)
{
    QString t = text.trimmed().toUpper();
    bool negative = false;
    if (t.startsWith(QLatin1Char('-'))) {
        negative = true;
        t.remove(0, 1);
    }
    if (!t.startsWith(QLatin1Char('P')))
        return false;

    qint64 total = 0;
    QString digits;
    bool inTime = false;
    bool sawUnit = false;
    for (int i = 1; i < t.size(); ++i) {
        const QChar c = t.at(i);
        if (c == QLatin1Char('T')) {
            inTime = true;
            digits.clear();
            continue;
        }
        if (c.isDigit() || c == QLatin1Char('.')) {
            digits.append(c);
            continue;
        }
        bool ok = false;
        const double value = digits.toDouble(&ok);
        digits.clear();
        if (!ok)
            return false;
        if (c == QLatin1Char('Y'))
            total += static_cast<qint64>(value * 365 * 24 * 3600);
        else if (c == QLatin1Char('W'))
            total += static_cast<qint64>(value * 7 * 24 * 3600);
        else if (c == QLatin1Char('D'))
            total += static_cast<qint64>(value * 24 * 3600);
        else if (c == QLatin1Char('H'))
            total += static_cast<qint64>(value * 3600);
        else if (c == QLatin1Char('S'))
            total += static_cast<qint64>(value);
        else if (c == QLatin1Char('M'))
            // 'T' 之前的 M 是月，之后才是分钟 —— xs:duration 最容易写错的一处。
            total += static_cast<qint64>(value * (inTime ? 60 : 30 * 24 * 3600));
        else
            return false;
        sawUnit = true;
    }
    if (!sawUnit)
        return false;
    if (secondsOut)
        *secondsOut = negative ? -total : total;
    return true;
}

QDateTime parseTerminationTime(const QString &text, const QDateTime &now, bool *ok)
{
    if (ok)
        *ok = false;
    const QString t = text.trimmed();
    if (t.isEmpty())
        return QDateTime();

    qint64 seconds = 0;
    if (parseIsoDuration(t, &seconds)) {
        if (ok)
            *ok = true;
        return now.toUTC().addSecs(seconds);
    }
    // 少数客户端发绝对时间。规范两种都允许，都要认。
    const QDateTime absolute = QDateTime::fromString(t, Qt::ISODate);
    if (absolute.isValid()) {
        if (ok)
            *ok = true;
        return absolute.toUTC();
    }
    return QDateTime();
}

void pullMessages(SoapContext &ctx)
{
    if (!ctx.camera) {
        ctx.fault(subscriptionGoneFault());
        return;
    }
    // ctx.exchange 为空只会出现在单测里。这里不拦：pull() 本身容得下空句柄，
    // responder 也会自己跳过回包，队列该怎么消费还是怎么消费。
    SubscriptionManager *mgr = ctx.camera->subscriptions();

    int messageLimit = ctx.argInt(QStringLiteral("MessageLimit"), kDefaultMessageLimit);
    if (messageLimit <= 0)
        messageLimit = kDefaultMessageLimit;

    int timeoutMs = kDefaultPullTimeoutMs;
    const QString timeout = ctx.arg(QStringLiteral("Timeout"));
    if (!timeout.isEmpty()) {
        qint64 seconds = 0;
        if (parseIsoDuration(timeout, &seconds))
            timeoutMs = static_cast<int>(
                qBound<qint64>(0, seconds * 1000, static_cast<qint64>(kMaxPullTimeoutMs)));
    }

    const int soapVersion = ctx.soap ? ctx.soap->soapVersion : 12;
    const SoapFault gone = subscriptionGoneFault();
    const QByteArray goneBody = soap::makeFault(soapVersion, gone);
    const int goneStatus = soap::faultHttpStatus(ctx.quirks(), gone);
    QPointer<VirtualCamera> camera(ctx.camera);

    // 下面这两份都按值拷进闭包，不能捕引用：handler 一返回 ctx 就没了，
    // 而回调最晚可能在 Timeout（默认 8 秒）之后才触发。
    //   quirks —— 用「客户端发起 Pull 那一刻」的快照。中途 REST 改了开关不该
    //             影响已经挂起的这一次响应，那样才符合直觉。
    //   req    —— F1 的 echo_500 要把请求原始字节回显出去，没有它就贴不了。
    const Quirks quirks = ctx.quirks();
    const HttpRequest request = ctx.http ? *ctx.http : HttpRequest();

    // 从这里开始响应归我们自己：队列空时 pull() 会把 exchange 挂到 Timeout，
    // 中间一次事件循环都不阻塞。Dispatcher 见到这个标记就彻底撒手。
    ctx.takeOverResponse();
    mgr->pull(subscriptionKey(ctx), messageLimit, timeoutMs, ctx.exchange,
              [soapVersion, goneBody, goneStatus, camera, quirks,
               request](const HttpExchangePtr &exchange, Subscription *sub,
                        const QVector<EventMessage> &messages) {
                  if (!exchange)
                      return;
                  HttpResponse response;
                  if (sub) {
                      const QDateTime now = camera ? camera->deviceTimeUtc()
                                                   : QDateTime::currentDateTimeUtc();
                      // 空列表是正常心跳，照样回 200 —— 客户端不把它当错误。
                      response = HttpResponse::soap(renderPullMessagesResponse(
                          soapVersion, now, sub->terminationTime, messages));
                  } else {
                      // 订阅中途没了（Unsubscribe / 过期回收 / 被挤掉），回 Fault 而不是吊着。
                      response = HttpResponse::soap(goneBody, goneStatus);
                  }
                  // 长轮询绕开了 Dispatcher 的回包路径，传输层 quirk 得自己贴一次。
                  // Slowloris / HugeResponse / F1 对长轮询恰恰最有意义 ——
                  // 真机在事件积压时就是这几种表现。
                  soap::applyResponseQuirks(quirks, response, request,
                                            QLatin1String(ns::Events));
                  exchange->respond(response);
              });
}

void renew(SoapContext &ctx)
{
    if (!ctx.camera) {
        ctx.fault(subscriptionGoneFault());
        return;
    }
    SubscriptionManager *mgr = ctx.camera->subscriptions();
    Subscription *sub = mgr->find(subscriptionKey(ctx));
    if (!sub) {
        ctx.fault(subscriptionGoneFault());
        return;
    }

    const QDateTime now = ctx.camera->deviceTimeUtc();
    // 参照客户端发的是 <wsnt:TerminationTime>PT1H</wsnt:TerminationTime>。
    const QString requested = ctx.arg(QStringLiteral("TerminationTime"));
    QDateTime termination;
    if (!requested.isEmpty()) {
        bool ok = false;
        termination = parseTerminationTime(requested, now, &ok);
        if (!ok) {
            ctx.fault(QString::fromLatin1(ter::UnacceptableTerminationTime),
                      QStringLiteral("Cannot parse TerminationTime %1").arg(requested));
            return;
        }
    }

    if (!mgr->renew(sub->id, termination)) {
        // quirk events.renew_fails：客户端必须能在 Renew 失败后重建订阅而不是放弃。
        ctx.fault(QString::fromLatin1(ter::UnacceptableTerminationTime),
                  QStringLiteral("Renew refused"));
        return;
    }

    XmlWriter &w = *ctx.out;
    w.start(QStringLiteral("wsnt:RenewResponse"));
    declareEventPrefixes(w);
    w.element(QStringLiteral("wsnt:TerminationTime"), isoUtc(sub->terminationTime));
    w.element(QStringLiteral("wsnt:CurrentTime"), isoUtc(now));
    w.end();
}

void unsubscribe(SoapContext &ctx)
{
    if (!ctx.camera) {
        ctx.fault(subscriptionGoneFault());
        return;
    }
    SubscriptionManager *mgr = ctx.camera->subscriptions();
    Subscription *sub = mgr->find(subscriptionKey(ctx));
    if (!sub || !mgr->unsubscribe(sub->id)) {
        ctx.fault(subscriptionGoneFault());
        return;
    }
    XmlWriter &w = *ctx.out;
    w.start(QStringLiteral("wsnt:UnsubscribeResponse"));
    declareEventPrefixes(w);
    w.end();
}

void setSynchronizationPoint(SoapContext &ctx)
{
    if (!ctx.camera) {
        ctx.fault(subscriptionGoneFault());
        return;
    }
    if (ctx.quirks().isEnabled(QuirkId::NoSetSynchronizationPoint)) {
        // 客户端拿不到属性型 topic 的当前状态，只能等下一次变化。
        ctx.fault(QString::fromLatin1(ter::ActionNotSupported),
                  QStringLiteral("SetSynchronizationPoint is not supported"));
        return;
    }

    // 把所有属性型 topic 的当前状态以 PropertyOperation=Initialized 重发一遍。
    // 走 deliver() 而不是直接塞队列：只有它会顺手唤醒正挂着的长轮询，
    // 否则客户端得等到本次 PullMessages 超时才看得见这批消息。
    const QVector<EventMessage> snapshot = ctx.camera->events()->synchronizationSnapshot();
    for (const EventMessage &message : snapshot)
        ctx.camera->subscriptions()->deliver(message);

    ctx.out->emptyElement(QStringLiteral("tev:SetSynchronizationPointResponse"));
}

} // namespace events

EventsService::EventsService()
{
    op("GetServiceCapabilities", AuthLevel::PreAuth, [this](SoapContext &ctx) {
        XmlWriter::Scope s(*ctx.out, QStringLiteral("tev:GetServiceCapabilitiesResponse"));
        writeServiceCapabilities(ctx);
    });

    op("GetEventProperties", AuthLevel::User, [](SoapContext &ctx) {
        if (!ctx.camera) {
            ctx.faultNotSupported();
            return;
        }
        const Quirks &q = ctx.quirks();
        if (q.isEnabled(QuirkId::NoGetEventProperties)
            && q.choice(QuirkId::NoGetEventProperties, QStringLiteral("action_not_supported"))
                   != QLatin1String("empty_topicset")) {
            ctx.fault(QString::fromLatin1(ter::ActionNotSupported),
                      QStringLiteral("GetEventProperties is not supported"));
            return;
        }
        // 另一档：响应结构完整但 TopicSet 是空的，客户端只能盲订阅全部。
        const bool emptyTopicSet = q.isEnabled(QuirkId::NoGetEventProperties);

        XmlWriter &w = *ctx.out;
        w.start(QStringLiteral("tev:GetEventPropertiesResponse"));
        declareEventPrefixes(w);
        w.element(QStringLiteral("tev:TopicNamespaceLocation"),
                  QString::fromLatin1(kTopicNamespaceLocation));
        w.element(QStringLiteral("wsnt:FixedTopicSet"), true);
        writeTopicSet(w, emptyTopicSet ? QVector<TopicDef>() : ctx.camera->events()->topics());
        w.element(QStringLiteral("wsnt:TopicExpressionDialect"),
                  QString::fromLatin1(kConcreteDialect));
        w.element(QStringLiteral("wsnt:TopicExpressionDialect"),
                  QString::fromLatin1(kConcreteSetDialect));
        w.element(QStringLiteral("tev:MessageContentFilterDialect"),
                  QString::fromLatin1(kItemFilterDialect));
        w.element(QStringLiteral("tev:MessageContentSchemaLocation"),
                  QString::fromLatin1(kMessageSchemaLocation));
        w.end();
    });

    op("CreatePullPointSubscription", AuthLevel::User, [](SoapContext &ctx) {
        if (!ctx.camera) {
            ctx.faultNotSupported();
            return;
        }
        // Filter/TopicExpression（ConcreteSet 方言，支持 "//." 与 '|'）。缺省即全收。
        QString expression;
        if (const XmlNode *filter = ctx.argNode(QStringLiteral("Filter"))) {
            if (const XmlNode *topic = filter->child(QStringLiteral("TopicExpression")))
                expression = topic->text.trimmed();
        }
        const TopicFilter filter(expression);
        // 解析不了的 TopicExpression 必须报错，不能静默退化成「全收」。
        // 退化的后果很隐蔽：客户端自以为订了一个很精确的过滤器，实际拿到全量
        // 事件，两边都不会发现 —— 而真机在这种情况下是回 Fault 的。
        if (!filter.isValid()) {
            ctx.fault(QString::fromLatin1(ter::InvalidFilterFault), filter.parseError());
            return;
        }

        const QDateTime now = ctx.camera->deviceTimeUtc();
        QDateTime termination;
        const QString initial = ctx.arg(QStringLiteral("InitialTerminationTime"));
        if (!initial.isEmpty()) {
            bool ok = false;
            termination = events::parseTerminationTime(initial, now, &ok);
            if (!ok) {
                ctx.fault(QString::fromLatin1(ter::UnacceptableTerminationTime),
                          QStringLiteral("Cannot parse InitialTerminationTime %1").arg(initial));
                return;
            }
        }

        QString failure;
        Subscription *sub = ctx.camera->subscriptions()->create(
            filter, termination, ctx.http ? ctx.http->peerString() : QString(), &failure);

        QString address;
        QDateTime terminationOut = termination.isValid() ? termination : now.addSecs(3600);
        if (sub) {
            address = sub->address;
            terminationOut = sub->terminationTime;
        } else if (failure == QLatin1String("slot_limit_silent")) {
            // D3 的 silent_fail 档：回一个看上去成功的响应，但订阅根本没建，
            // 后续 Pull 全部 Fault —— 真机上最难查的那种。
            address = ctx.serviceUri(QStringLiteral("/onvif/subscription-0"));
        } else {
            ctx.fault(QString::fromLatin1(ter::MaxPullPoints),
                      QStringLiteral("Maximum number of pull point subscriptions reached"));
            return;
        }

        XmlWriter &w = *ctx.out;
        w.start(QStringLiteral("tev:CreatePullPointSubscriptionResponse"));
        declareEventPrefixes(w);
        if (ctx.quirks().isEnabled(QuirkId::SubscriptionFlatAddress)) {
            // D6：Address 直接放响应下，不套 SubscriptionReference 那一层。
            // 客户端的兜底是「找不到 SubscriptionReference 就在整个响应里找 Address」。
            w.element(QStringLiteral("wsa:Address"), address);
        } else {
            XmlWriter::Scope reference(w, QStringLiteral("tev:SubscriptionReference"));
            w.element(QStringLiteral("wsa:Address"), address);
        }
        w.element(QStringLiteral("wsnt:CurrentTime"), isoUtc(now));
        w.element(QStringLiteral("wsnt:TerminationTime"), isoUtc(terminationOut));
        w.end();
    });

    // 下面四个操作客户端通常是往订阅地址发的（由 SubscriptionEndpoint 接），
    // 但也有客户端直接打事件服务地址，这里一并接住。
    op("PullMessages", AuthLevel::User, events::pullMessages);
    op("Renew", AuthLevel::User, events::renew);
    op("Unsubscribe", AuthLevel::User, events::unsubscribe);
    op("SetSynchronizationPoint", AuthLevel::User, events::setSynchronizationPoint);
}

const char *EventsService::serviceNamespace() const
{
    return ns::Events;
}

const char *EventsService::serviceName() const
{
    return "events";
}

QString EventsService::defaultPath() const
{
    return QStringLiteral("/onvif/events_service");
}

void EventsService::writeServiceCapabilities(SoapContext &ctx) const
{
    const Quirks &q = ctx.quirks();
    // 声明的槽位数要与 D3 真正卡的那个数一致，否则客户端连「为什么建不上」都判不出来。
    const int maxPullPoints = q.isEnabled(QuirkId::SubscriptionSlotLimit)
                                  ? qMax(1, q.paramInt(QuirkId::SubscriptionSlotLimit,
                                                       QStringLiteral("max")))
                                  : kDefaultMaxPullPoints;

    XmlWriter &w = *ctx.out;
    w.start(QStringLiteral("tev:Capabilities"));
    w.attr(QStringLiteral("WSSubscriptionPolicySupport"), QStringLiteral("false"));
    w.attr(QStringLiteral("WSPullPointSupport"), QStringLiteral("true"));
    w.attr(QStringLiteral("WSPausableSubscriptionManagerInterfaceSupport"),
           QStringLiteral("false"));
    w.attr(QStringLiteral("MaxNotificationProducers"), QString::number(maxPullPoints));
    w.attr(QStringLiteral("MaxPullPoints"), QString::number(maxPullPoints));
    w.attr(QStringLiteral("PersistentNotificationStorage"), QStringLiteral("false"));
    w.end();
}

namespace services {

SoapService *createEvents()
{
    return new EventsService;
}

} // namespace services
} // namespace onvifsim
