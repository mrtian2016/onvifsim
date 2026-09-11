// Events 服务层单测：CreatePullPointSubscription（正常 + D6 扁平 Address）、
// InitialTerminationTime / Renew 的 ISO duration 解析、PullMessages 的 MessageLimit
// 与「队列空就挂起」的长轮询行为、GetEventProperties 的 TopicSet 结构、D4 两档、
// Unsubscribe 与 SetSynchronizationPoint。
//
// 长轮询这里不用真的 HttpExchange：pull() 允许空句柄，队列与 pendingPullCount()
// 已经能把「有消息立刻回 / 没消息挂起 / 来了消息唤醒」三种路径全测到。

#include <QtTest/QtTest>

#include "core/CameraModel.h"
#include "core/Quirks.h"
#include "core/VirtualCamera.h"
#include "events/EventEngine.h"
#include "events/EventTypes.h"
#include "events/SubscriptionManager.h"
#include "net/HttpTypes.h"
#include "services/ServiceBase.h"
#include "services/events/EventsService.h"
#include "soap/Envelope.h"
#include "soap/XmlWriter.h"

using namespace onvifsim;

namespace {

// 普通转义字符串，不用 raw string：moc 会把 raw string 里的 "//" 当行注释，
// 顺手把后面的 Q_OBJECT 一起吃掉。
const char *kEnvelopeHead =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
    " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\""
    " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""
    " xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
    "<s:Body>";
const char *kEnvelopeTail = "</s:Body></s:Envelope>";

struct Result {
    QByteArray xml;
    bool fault = false;
    QString subcode;
    QString reason;
    bool takenOver = false;
};

Result call(SoapService *service, VirtualCamera *camera, const QString &operation,
            const QByteArray &bodyElement, const QString &path = QString())
{
    Result result;
    const QByteArray raw = QByteArray(kEnvelopeHead) + bodyElement + kEnvelopeTail;
    const SoapRequest request = soap::parseEnvelope(raw);

    HttpRequest http;
    http.method = "POST";
    http.path = path.isEmpty() ? QStringLiteral("/onvif/events_service") : path;
    http.body = raw;

    XmlWriter writer(request.soapVersion);
    writer.declareServicePrefixes();
    writer.startEnvelope();

    SoapContext ctx;
    ctx.camera = camera;
    ctx.soap = &request;
    ctx.body = &request.body;
    ctx.http = &http;
    ctx.out = &writer;

    const SoapOperation *op = service->findOperation(operation);
    if (!op)
        return result;
    op->handler(ctx);

    result.takenOver = ctx.responseTakenOver();
    if (ctx.hasFault()) {
        result.fault = true;
        result.subcode = ctx.pendingFault().subcode;
        result.reason = ctx.pendingFault().reason;
        return result;
    }
    writer.endEnvelope();
    result.xml = writer.take();
    return result;
}

EventMessage motionMessage(bool state)
{
    EventMessage m;
    m.topic = QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion");
    m.isProperty = true;
    m.propertyOperation = QStringLiteral("Changed");
    m.sourceItems.insert(QStringLiteral("VideoAnalyticsConfigurationToken"),
                         QStringLiteral("VideoAnalyticsConfig"));
    m.dataItems.insert(QStringLiteral("IsMotion"),
                       state ? QStringLiteral("true") : QStringLiteral("false"));
    return m;
}

} // namespace

class TstEventsService : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void isoDurationParsing();
    void createSubscriptionNested();
    void createSubscriptionFlatAddress();
    void createSubscriptionInitialTerminationTime();
    void createSubscriptionSlotLimit();
    void pullMessagesRespectsMessageLimit();
    void pullMessagesSuspendsWhenQueueEmpty();
    void getEventPropertiesTopicSet();
    void getEventPropertiesUnsupportedShapes();
    void renewParsesDuration();
    void unsubscribeRemovesSubscription();
    void setSynchronizationPoint();

private:
    Subscription *createSubscription(const QByteArray &inner = QByteArray());

    VirtualCamera *m_camera = nullptr;
    SoapService *m_service = nullptr;
};

void TstEventsService::init()
{
    m_camera = new VirtualCamera(CameraModel::makeDefault(QStringLiteral("cam1"),
                                                          QStringLiteral("generic"), 0),
                                 nullptr);
    m_service = services::createEvents();
}

void TstEventsService::cleanup()
{
    delete m_service;
    m_service = nullptr;
    delete m_camera;
    m_camera = nullptr;
}

Subscription *TstEventsService::createSubscription(const QByteArray &inner)
{
    const QByteArray body = "<tev:CreatePullPointSubscription>" + inner
                            + "</tev:CreatePullPointSubscription>";
    const Result r = call(m_service, m_camera, QStringLiteral("CreatePullPointSubscription"), body);
    if (r.fault)
        return nullptr;
    const QList<Subscription *> all = m_camera->subscriptions()->subscriptions();
    return all.isEmpty() ? nullptr : all.last();
}

void TstEventsService::isoDurationParsing()
{
    qint64 seconds = 0;
    QVERIFY(events::parseIsoDuration(QStringLiteral("PT1H"), &seconds));
    QCOMPARE(seconds, qint64(3600));
    QVERIFY(events::parseIsoDuration(QStringLiteral("PT60S"), &seconds));
    QCOMPARE(seconds, qint64(60));
    QVERIFY(events::parseIsoDuration(QStringLiteral("PT8S"), &seconds));
    QCOMPARE(seconds, qint64(8));
    // T 之前的 M 是月、之后才是分钟 —— 这条最容易写错。
    QVERIFY(events::parseIsoDuration(QStringLiteral("PT1M30S"), &seconds));
    QCOMPARE(seconds, qint64(90));
    QVERIFY(events::parseIsoDuration(QStringLiteral("P1DT2H3M4S"), &seconds));
    QCOMPARE(seconds, qint64(24 * 3600 + 2 * 3600 + 3 * 60 + 4));

    QVERIFY(!events::parseIsoDuration(QStringLiteral("60"), &seconds));
    QVERIFY(!events::parseIsoDuration(QStringLiteral(""), &seconds));
    QVERIFY(!events::parseIsoDuration(QStringLiteral("2026-09-10T12:00:00Z"), &seconds));

    // 绝对时间也要认：规范两种写法都允许。
    const QDateTime now = QDateTime::currentDateTimeUtc();
    bool ok = false;
    const QDateTime absolute =
        events::parseTerminationTime(QStringLiteral("2030-01-01T00:00:00Z"), now, &ok);
    QVERIFY(ok);
    QCOMPARE(absolute.date().year(), 2030);
}

void TstEventsService::createSubscriptionNested()
{
    const Result r = call(m_service, m_camera, QStringLiteral("CreatePullPointSubscription"),
                          "<tev:CreatePullPointSubscription>"
                          "<tev:Filter><wsnt:TopicExpression Dialect=\"x\">"
                          "tns1:RuleEngine//.</wsnt:TopicExpression></tev:Filter>"
                          "<tev:InitialTerminationTime>PT1H</tev:InitialTerminationTime>"
                          "</tev:CreatePullPointSubscription>");
    QVERIFY(!r.fault);
    // 默认形态：Address 套在 SubscriptionReference 下。
    QVERIFY(r.xml.contains("<tev:SubscriptionReference>"));
    QVERIFY(r.xml.contains("<wsa:Address>http://"));
    QVERIFY(r.xml.contains("wsnt:CurrentTime"));
    QVERIFY(r.xml.contains("wsnt:TerminationTime"));
    // 前缀必须随响应带出去，否则客户端解析不了。
    QVERIFY(r.xml.contains("xmlns:wsa=\"http://www.w3.org/2005/08/addressing\""));

    QCOMPARE(m_camera->subscriptions()->count(), 1);
    Subscription *sub = m_camera->subscriptions()->subscriptions().first();
    QVERIFY(r.xml.contains(sub->address.toUtf8()));
    // TopicExpression 真的进了过滤器：匹配的收，不匹配的不收。
    QVERIFY(sub->filter.matches(QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion")));
    QVERIFY(!sub->filter.matches(QStringLiteral("tns1:Device/Trigger/DigitalInput")));
}

void TstEventsService::createSubscriptionFlatAddress()
{
    // D6：Address 直接放响应下，不套 SubscriptionReference 那一层。
    Quirks q = m_camera->quirks();
    q.setEnabled(QuirkId::SubscriptionFlatAddress, true);
    m_camera->setQuirks(q);

    const Result r = call(m_service, m_camera, QStringLiteral("CreatePullPointSubscription"),
                          "<tev:CreatePullPointSubscription/>");
    QVERIFY(!r.fault);
    QVERIFY(!r.xml.contains("SubscriptionReference"));
    QVERIFY(r.xml.contains("<wsa:Address>http://"));
    QCOMPARE(m_camera->subscriptions()->count(), 1);
}

void TstEventsService::createSubscriptionInitialTerminationTime()
{
    const QDateTime before = QDateTime::currentDateTimeUtc();
    Subscription *sub = createSubscription(
        "<tev:InitialTerminationTime>PT60S</tev:InitialTerminationTime>");
    QVERIFY(sub);
    const qint64 ttl = before.secsTo(sub->terminationTime);
    QVERIFY2(ttl >= 55 && ttl <= 65, qPrintable(QString::number(ttl)));

    // 解析不了的 duration 要回 Fault，而不是悄悄按默认 TTL 建一条。
    const Result bad = call(m_service, m_camera, QStringLiteral("CreatePullPointSubscription"),
                            "<tev:CreatePullPointSubscription>"
                            "<tev:InitialTerminationTime>forever</tev:InitialTerminationTime>"
                            "</tev:CreatePullPointSubscription>");
    QVERIFY(bad.fault);
    QCOMPARE(bad.subcode, QStringLiteral("ter:UnacceptableTerminationTime"));
    QCOMPARE(m_camera->subscriptions()->count(), 1);
}

void TstEventsService::createSubscriptionSlotLimit()
{
    // D3：槽位上限 1，第二条要么 Fault，要么「假装成功」。
    Quirks q = m_camera->quirks();
    q.setEnabled(QuirkId::SubscriptionSlotLimit, true);
    q.setParam(QuirkId::SubscriptionSlotLimit, QStringLiteral("max"), 1);
    q.setParam(QuirkId::SubscriptionSlotLimit, QStringLiteral("on_overflow"),
               QStringLiteral("fault"));
    m_camera->setQuirks(q);

    QVERIFY(createSubscription());
    const Result overflow = call(m_service, m_camera,
                                 QStringLiteral("CreatePullPointSubscription"),
                                 "<tev:CreatePullPointSubscription/>");
    QVERIFY(overflow.fault);
    QCOMPARE(overflow.subcode, QStringLiteral("ter:MaxPullPoints"));
    QCOMPARE(m_camera->subscriptions()->count(), 1);

    // silent_fail：响应看着成功，订阅其实没建 —— 真机上最难查的那种。
    q.setParam(QuirkId::SubscriptionSlotLimit, QStringLiteral("on_overflow"),
               QStringLiteral("silent_fail"));
    m_camera->setQuirks(q);
    const Result silent = call(m_service, m_camera, QStringLiteral("CreatePullPointSubscription"),
                               "<tev:CreatePullPointSubscription/>");
    QVERIFY(!silent.fault);
    QVERIFY(silent.xml.contains("<wsa:Address>http://"));
    QCOMPARE(m_camera->subscriptions()->count(), 1);
}

void TstEventsService::pullMessagesRespectsMessageLimit()
{
    Subscription *sub = createSubscription();
    QVERIFY(sub);
    for (int i = 0; i < 5; ++i)
        m_camera->subscriptions()->deliver(motionMessage(i % 2 == 0));
    QCOMPARE(sub->queue.size(), 5);

    // 走订阅自己的 HTTP 路径，和真实客户端一样。
    const Result r = call(m_service, m_camera, QStringLiteral("PullMessages"),
                          "<tev:PullMessages>"
                          "<tev:Timeout>PT8S</tev:Timeout>"
                          "<tev:MessageLimit>2</tev:MessageLimit>"
                          "</tev:PullMessages>",
                          sub->path);
    // 长轮询一律接管响应，Dispatcher 不再包信封。
    QVERIFY(r.takenOver);
    QVERIFY(!r.fault);
    QCOMPARE(sub->queue.size(), 3);
    QCOMPARE(sub->pulledCount, qint64(2));
    QCOMPARE(m_camera->subscriptions()->pendingPullCount(), 0);

    // 剩下的一次拉干净。
    call(m_service, m_camera, QStringLiteral("PullMessages"),
         "<tev:PullMessages><tev:MessageLimit>100</tev:MessageLimit></tev:PullMessages>",
         sub->path);
    QCOMPARE(sub->queue.size(), 0);
    QCOMPARE(sub->pulledCount, qint64(5));
}

void TstEventsService::pullMessagesSuspendsWhenQueueEmpty()
{
    Subscription *sub = createSubscription();
    QVERIFY(sub);
    QCOMPARE(sub->queue.size(), 0);

    const Result r = call(m_service, m_camera, QStringLiteral("PullMessages"),
                          "<tev:PullMessages>"
                          "<tev:Timeout>PT8S</tev:Timeout>"
                          "<tev:MessageLimit>100</tev:MessageLimit>"
                          "</tev:PullMessages>",
                          sub->path);
    QVERIFY(r.takenOver);
    QVERIFY(!r.fault);
    // 关键断言：队列空时不立刻回空，而是挂起 —— 这才是真长轮询。
    QCOMPARE(m_camera->subscriptions()->pendingPullCount(), 1);

    // 来了消息立刻唤醒，不必等到 Timeout。
    m_camera->subscriptions()->deliver(motionMessage(true));
    QCOMPARE(m_camera->subscriptions()->pendingPullCount(), 0);
    QCOMPARE(sub->queue.size(), 0);
    QCOMPARE(sub->pulledCount, qint64(1));

    // 订阅不存在时要回 Fault，不能把请求吊着。
    m_camera->subscriptions()->clear();
    const Result gone = call(m_service, m_camera, QStringLiteral("PullMessages"),
                             "<tev:PullMessages><tev:Timeout>PT8S</tev:Timeout>"
                             "</tev:PullMessages>",
                             QStringLiteral("/onvif/subscription-99"));
    QVERIFY(gone.takenOver);
    QCOMPARE(m_camera->subscriptions()->pendingPullCount(), 0);
}

void TstEventsService::getEventPropertiesTopicSet()
{
    const Result r = call(m_service, m_camera, QStringLiteral("GetEventProperties"),
                          "<tev:GetEventProperties/>");
    QVERIFY(!r.fault);
    QVERIFY(r.xml.contains("<wsnt:FixedTopicSet>true</wsnt:FixedTopicSet>"));
    QVERIFY(r.xml.contains("<wstop:TopicSet>"));
    // topic 串按 '/' 展成嵌套元素，只有首段带前缀。
    QVERIFY(r.xml.contains("<tns1:RuleEngine>"));
    QVERIFY(r.xml.contains("<CellMotionDetector>"));
    QVERIFY(r.xml.contains("<Motion wstop:topic=\"true\">"));
    // 叶子上挂 MessageDescription，客户端据此知道 Source / Data 各有什么。
    QVERIFY(r.xml.contains("<tt:MessageDescription IsProperty=\"true\">"));
    QVERIFY(r.xml.contains("Name=\"IsMotion\""));
    QVERIFY(r.xml.contains("Type=\"xs:boolean\""));
    QVERIFY(r.xml.contains("tev:TopicNamespaceLocation"));
    QVERIFY(r.xml.contains("wsnt:TopicExpressionDialect"));
    QVERIFY(r.xml.contains("tev:MessageContentFilterDialect"));

    // topic 集与 EventEngine 报的是同一份。
    const QVector<TopicDef> topics = m_camera->events()->topics();
    QVERIFY(!topics.isEmpty());
    for (const TopicDef &def : topics) {
        const QString leaf = def.topic.section(QLatin1Char('/'), -1);
        QVERIFY2(r.xml.contains(leaf.toUtf8()), qPrintable(def.topic));
    }
}

void TstEventsService::getEventPropertiesUnsupportedShapes()
{
    // D4 第一档：整个操作不实现。
    Quirks q = m_camera->quirks();
    q.setEnabled(QuirkId::NoGetEventProperties, true);
    q.setParam(QuirkId::NoGetEventProperties, QStringLiteral("value"),
               QStringLiteral("action_not_supported"));
    m_camera->setQuirks(q);
    const Result notSupported = call(m_service, m_camera, QStringLiteral("GetEventProperties"),
                                     "<tev:GetEventProperties/>");
    QVERIFY(notSupported.fault);
    QCOMPARE(notSupported.subcode, QStringLiteral("ter:ActionNotSupported"));

    // D4 第二档：响应结构完整，TopicSet 是空的 —— 客户端只能盲订阅全部。
    q.setParam(QuirkId::NoGetEventProperties, QStringLiteral("value"),
               QStringLiteral("empty_topicset"));
    m_camera->setQuirks(q);
    const Result empty = call(m_service, m_camera, QStringLiteral("GetEventProperties"),
                              "<tev:GetEventProperties/>");
    QVERIFY(!empty.fault);
    QVERIFY(empty.xml.contains("<wstop:TopicSet/>"));
    QVERIFY(!empty.xml.contains("CellMotionDetector"));
}

void TstEventsService::renewParsesDuration()
{
    Subscription *sub = createSubscription(
        "<tev:InitialTerminationTime>PT60S</tev:InitialTerminationTime>");
    QVERIFY(sub);

    const QDateTime before = QDateTime::currentDateTimeUtc();
    const Result r = call(m_service, m_camera, QStringLiteral("Renew"),
                          "<wsnt:Renew>"
                          "<wsnt:TerminationTime>PT1H</wsnt:TerminationTime>"
                          "</wsnt:Renew>",
                          sub->path);
    QVERIFY(!r.fault);
    // 响应在 wsnt 命名空间下，TerminationTime 在前、CurrentTime 在后。
    QVERIFY(r.xml.contains("<wsnt:RenewResponse"));
    QVERIFY(r.xml.contains("<wsnt:TerminationTime>"));
    QVERIFY(r.xml.contains("<wsnt:CurrentTime>"));
    const qint64 ttl = before.secsTo(sub->terminationTime);
    QVERIFY2(ttl >= 3595 && ttl <= 3605, qPrintable(QString::number(ttl)));

    // quirk：Renew 总是失败，客户端必须能重建订阅而不是放弃。
    Quirks q = m_camera->quirks();
    q.setEnabled(QuirkId::RenewFails, true);
    m_camera->setQuirks(q);
    const Result failed = call(m_service, m_camera, QStringLiteral("Renew"),
                               "<wsnt:Renew>"
                               "<wsnt:TerminationTime>PT1H</wsnt:TerminationTime>"
                               "</wsnt:Renew>",
                               sub->path);
    QVERIFY(failed.fault);
}

void TstEventsService::unsubscribeRemovesSubscription()
{
    Subscription *sub = createSubscription();
    QVERIFY(sub);
    const QString path = sub->path;

    const Result r = call(m_service, m_camera, QStringLiteral("Unsubscribe"),
                          "<wsnt:Unsubscribe/>", path);
    QVERIFY(!r.fault);
    QVERIFY(r.xml.contains("wsnt:UnsubscribeResponse"));
    QCOMPARE(m_camera->subscriptions()->count(), 0);

    // 再退一次就该 Fault 了。
    const Result again = call(m_service, m_camera, QStringLiteral("Unsubscribe"),
                              "<wsnt:Unsubscribe/>", path);
    QVERIFY(again.fault);
    QCOMPARE(again.subcode, QStringLiteral("ter:ResourceUnknownFault"));
}

void TstEventsService::setSynchronizationPoint()
{
    Subscription *sub = createSubscription();
    QVERIFY(sub);

    const Result r = call(m_service, m_camera, QStringLiteral("SetSynchronizationPoint"),
                          "<tev:SetSynchronizationPoint/>", sub->path);
    QVERIFY(!r.fault);
    QVERIFY(r.xml.contains("tev:SetSynchronizationPointResponse"));
    // 所有属性型 topic 的当前状态都被重发了一遍。
    const QVector<EventMessage> snapshot = m_camera->events()->synchronizationSnapshot();
    QVERIFY(!snapshot.isEmpty());
    QCOMPARE(sub->queue.size(), snapshot.size());
    QCOMPARE(sub->queue.first().propertyOperation, QStringLiteral("Initialized"));

    // quirk：不支持 SetSynchronizationPoint，客户端只能等下一次状态变化。
    Quirks q = m_camera->quirks();
    q.setEnabled(QuirkId::NoSetSynchronizationPoint, true);
    m_camera->setQuirks(q);
    const Result unsupported = call(m_service, m_camera,
                                    QStringLiteral("SetSynchronizationPoint"),
                                    "<tev:SetSynchronizationPoint/>", sub->path);
    QVERIFY(unsupported.fault);
    QCOMPARE(unsupported.subcode, QStringLiteral("ter:ActionNotSupported"));
}

QTEST_GUILESS_MAIN(TstEventsService)

#include "tst_events_service.moc"
