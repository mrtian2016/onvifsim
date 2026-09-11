// 事件层单测：topic 目录的四套命名风格、TopicExpression 过滤、属性型事件的
// 成对状态、PullPoint 订阅的队列 / 槽位 / 过期 / 长轮询。
//
// 这里一律用 camera == nullptr 建 EventEngine / SubscriptionManager：建一台
// VirtualCamera 要连带拉起 HttpServer 与 RtspServer，单测不值当。quirk 从
// eventsFallbackQuirks() 这个测试接缝注入（定义在 src/events/EventTypes.cpp）。

#include "core/Quirks.h"
#include "events/EventEngine.h"
#include "events/EventTypes.h"
#include "events/SubscriptionManager.h"

#include <QtCore/QRegularExpression>
#include <QtTest/QtTest>

namespace onvifsim {
Quirks &eventsFallbackQuirks();
}

using namespace onvifsim;

namespace {

EventMessage makeMessage(const QString &topic)
{
    EventMessage m;
    m.topic = topic;
    m.dataItems.insert(QStringLiteral("State"), QStringLiteral("true"));
    return m;
}

// 长轮询的回调结果。responder 拿到什么、什么时候拿到，全靠它记下来。
struct PullResult {
    int calls = 0;
    bool subscriptionAlive = false;
    QVector<EventMessage> messages;

    SubscriptionManager::PullResponder responder()
    {
        return [this](const HttpExchangePtr &, Subscription *sub,
                      const QVector<EventMessage> &msgs) {
            ++calls;
            subscriptionAlive = (sub != nullptr);
            messages = msgs;
        };
    }
};

} // namespace

class TestEvents : public QObject
{
    Q_OBJECT

private slots:
    void init();

    // ---- TopicCatalog ----
    void catalogCoversEveryKindInEveryStyle_data();
    void catalogCoversEveryKindInEveryStyle();
    void catalogStyleSignatures();
    void kindNameRoundTrip();

    // ---- TopicFilter ----
    void filterEmptyAcceptsEverything();
    void filterMatchAllDialect();
    void filterExactSingle();
    void filterMultipleExpressions();
    void filterSubtreePrefix();
    void filterIgnoresNamespacePrefix();
    void filterRejectsOtherTopics();

    // ---- EventEngine ----
    void propertyEventIsPaired();
    void propertyEventNotPairedQuirk();
    void transientEventSendsOnce();
    void topicStyleQuirkSwitchesCatalog();
    void synchronizationSnapshotIsInitialized();
    void stormProducesEvents();

    // ---- SubscriptionManager ----
    void queueDropsOldestOnOverflow();
    void deliverRespectsFilter();
    void slotLimitFault();
    void slotLimitEvictsOldest();
    void slotLimitSilentFail();
    void reapRemovesExpired();
    void neverExpiresQuirkLeaksSlots();
    void portIncrementQuirk();
    void expiresAtOnceQuirk();
    void pullReturnsQueuedImmediately();
    void pullBlocksThenReturnsEmpty();
    void pullWakesOnDelivery();
    void pullAlwaysEmptyQuirk();
    void pullUnknownSubscription();
};

void TestEvents::init()
{
    // 每条用例都从「全关」起步，否则 quirk 会顺着进程串味。
    eventsFallbackQuirks().clear();
}

// ---------------------------------------------------------------- TopicCatalog

void TestEvents::catalogCoversEveryKindInEveryStyle_data()
{
    QTest::addColumn<QString>("style");
    for (const QString &s : TopicCatalog::styles())
        QTest::newRow(qPrintable(s)) << s;
}

void TestEvents::catalogCoversEveryKindInEveryStyle()
{
    QFETCH(QString, style);
    const QVector<TopicDef> topics = TopicCatalog::topicsForStyle(style);
    QVERIFY(!topics.isEmpty());
    QCOMPARE(topics.size(), static_cast<int>(EventKind::Count));

    // "前缀:段/段/段"，段里只允许字母数字下划线。
    const QRegularExpression shape(
        QStringLiteral("^[A-Za-z][A-Za-z0-9]*:[A-Za-z0-9_]+(/[A-Za-z0-9_]+)*$"));
    QSet<QString> seen;
    for (int i = 0; i < static_cast<int>(EventKind::Count); ++i) {
        const EventKind kind = static_cast<EventKind>(i);
        const TopicDef *def = TopicCatalog::findByKind(topics, kind);
        QVERIFY2(def != nullptr, qPrintable(QStringLiteral("%1 缺 %2")
                                                .arg(style, TopicCatalog::kindName(kind))));
        QVERIFY2(shape.match(def->topic).hasMatch(), qPrintable(def->topic));
        QVERIFY(!def->dataItemName.isEmpty());
        QVERIFY(!def->displayName.isEmpty());
        QVERIFY(!seen.contains(def->topic));   // 同一风格内 topic 不能撞
        seen.insert(def->topic);
    }
}

void TestEvents::catalogStyleSignatures()
{
    const QVector<TopicDef> onvif = TopicCatalog::topicsForStyle(QStringLiteral("onvif"));
    QCOMPARE(TopicCatalog::findByKind(onvif, EventKind::Motion)->topic,
             QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion"));
    QCOMPARE(TopicCatalog::findByKind(onvif, EventKind::MotionAlarm)->topic,
             QStringLiteral("tns1:VideoSource/MotionAlarm"));
    QCOMPARE(TopicCatalog::findByKind(onvif, EventKind::Motion)->dataItemName,
             QStringLiteral("IsMotion"));

    // TP-Link 真机：越界 / 入侵都不叫通用约定的名字。
    const QVector<TopicDef> tplink = TopicCatalog::topicsForStyle(QStringLiteral("tplink"));
    QVERIFY(TopicCatalog::findByKind(tplink, EventKind::LineCrossing)
                ->topic.endsWith(QStringLiteral("LineCrossDetector/LineCross")));
    QVERIFY(TopicCatalog::findByKind(tplink, EventKind::FieldIntrusion)
                ->topic.endsWith(QStringLiteral("IntrusionDetector/Intrusion")));

    // Reolink：AI 事件全挂 MyRuleDetector，规则名的 SimpleItem 叫 RuleName。
    const QVector<TopicDef> reolink = TopicCatalog::topicsForStyle(QStringLiteral("reolink"));
    const QVector<EventKind> aiKinds { EventKind::PeopleDetect, EventKind::VehicleDetect,
                                       EventKind::AnimalDetect, EventKind::FaceDetect };
    for (EventKind kind : aiKinds) {
        const TopicDef *def = TopicCatalog::findByKind(reolink, kind);
        QVERIFY2(def->topic.contains(QStringLiteral("MyRuleDetector")), qPrintable(def->topic));
        QCOMPARE(def->ruleItemName, QStringLiteral("RuleName"));
    }
    QVERIFY(TopicCatalog::findByKind(reolink, EventKind::AnimalDetect)
                ->topic.endsWith(QStringLiteral("DogCatDetect")));

    // Axis：自家事件带 tnsaxis: 前缀。
    const QVector<TopicDef> axis = TopicCatalog::topicsForStyle(QStringLiteral("axis"));
    int axisPrefixed = 0;
    for (const TopicDef &def : axis) {
        if (def.topic.startsWith(QStringLiteral("tnsaxis:")))
            ++axisPrefixed;
    }
    QVERIFY2(axisPrefixed >= 6, qPrintable(QString::number(axisPrefixed)));

    // 未知风格回落到标准 ONVIF，而不是空表。
    QCOMPARE(TopicCatalog::topicsForStyle(QStringLiteral("nosuchbrand")).size(), onvif.size());
}

void TestEvents::kindNameRoundTrip()
{
    for (int i = 0; i < static_cast<int>(EventKind::Count); ++i) {
        const EventKind kind = static_cast<EventKind>(i);
        const QString name = TopicCatalog::kindName(kind);
        QVERIFY(!name.isEmpty());
        bool ok = false;
        QCOMPARE(TopicCatalog::kindFromName(name, &ok), kind);
        QVERIFY(ok);
    }
    bool ok = true;
    TopicCatalog::kindFromName(QStringLiteral("not_a_kind"), &ok);
    QVERIFY(!ok);
    // 大小写与下划线都不该卡住 REST 调用方。
    QCOMPARE(TopicCatalog::kindFromName(QStringLiteral("MotionAlarm")), EventKind::MotionAlarm);
}

// ----------------------------------------------------------------- TopicFilter

void TestEvents::filterEmptyAcceptsEverything()
{
    const TopicFilter f;
    QVERIFY(f.isEmpty());
    QVERIFY(f.matches(QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion")));
    QVERIFY(TopicFilter(QString()).matches(QStringLiteral("tns1:VideoSource/MotionAlarm")));
}

void TestEvents::filterMatchAllDialect()
{
    const TopicFilter f(QStringLiteral("//."));
    QVERIFY(f.matches(QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion")));
    QVERIFY(f.matches(QStringLiteral("tnsaxis:VideoSource/Tampering")));
    QVERIFY(f.matches(QStringLiteral("whatever:Foo/Bar")));
    QCOMPARE(f.expression(), QStringLiteral("//."));
}

void TestEvents::filterExactSingle()
{
    const TopicFilter f(QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion"));
    QVERIFY(!f.isEmpty());
    QVERIFY(f.matches(QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion")));
    QVERIFY(!f.matches(QStringLiteral("tns1:RuleEngine/CellMotionDetector")));
    QVERIFY(!f.matches(QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion/Extra")));
}

void TestEvents::filterMultipleExpressions()
{
    const TopicFilter f(QStringLiteral(
        "tns1:RuleEngine/CellMotionDetector/Motion|tns1:VideoSource/MotionAlarm"));
    QVERIFY(f.matches(QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion")));
    QVERIFY(f.matches(QStringLiteral("tns1:VideoSource/MotionAlarm")));
    QVERIFY(!f.matches(QStringLiteral("tns1:VideoSource/ImageTooDark")));
}

void TestEvents::filterSubtreePrefix()
{
    const TopicFilter f(QStringLiteral("tns1:RuleEngine//."));
    QVERIFY(f.matches(QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion")));
    QVERIFY(f.matches(QStringLiteral("tns1:RuleEngine/TamperDetector/Tamper")));
    QVERIFY(!f.matches(QStringLiteral("tns1:VideoSource/MotionAlarm")));
}

void TestEvents::filterIgnoresNamespacePrefix()
{
    // 客户端把 tns1: 省了、或换了个自定义前缀，都得照样命中。
    const TopicFilter f(QStringLiteral("RuleEngine/CellMotionDetector/Motion"));
    QVERIFY(f.matches(QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion")));
    const TopicFilter g(QStringLiteral("tev:RuleEngine/CellMotionDetector/Motion"));
    QVERIFY(g.matches(QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion")));
}

void TestEvents::filterRejectsOtherTopics()
{
    const TopicFilter f(QStringLiteral("tns1:VideoSource/MotionAlarm"));
    QVERIFY(!f.matches(QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion")));
    QVERIFY(!f.matches(QString()));
}

// ----------------------------------------------------------------- EventEngine

void TestEvents::propertyEventIsPaired()
{
    EventEngine engine(nullptr);
    QVector<EventMessage> got;
    connect(&engine, &EventEngine::eventProduced, &engine,
            [&got](const EventMessage &m) { got.append(m); });

    engine.trigger(EventKind::Motion, 60);
    QCOMPARE(got.size(), 1);
    QCOMPARE(got.at(0).topic, QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion"));
    QVERIFY(got.at(0).isProperty);
    QCOMPARE(got.at(0).propertyOperation, QStringLiteral("Changed"));
    QCOMPARE(got.at(0).dataItems.value(QStringLiteral("IsMotion")), QStringLiteral("true"));
    QCOMPARE(got.at(0).sourceItems.value(QStringLiteral("Rule")),
             QStringLiteral("MyMotionDetectorRule"));
    QCOMPARE(engine.propertyStates().value(got.at(0).topic), true);

    // durationMs 之后自动补一条 false，这就是「成对」。
    QTRY_COMPARE_WITH_TIMEOUT(got.size(), 2, 2000);
    QCOMPARE(got.at(1).dataItems.value(QStringLiteral("IsMotion")), QStringLiteral("false"));
    QCOMPARE(engine.propertyStates().value(got.at(0).topic), false);
    QCOMPARE(engine.totalEventsSent(), qint64(2));
}

void TestEvents::propertyEventNotPairedQuirk()
{
    // D11：只发 true，客户端的移动侦测就此永远不复位。
    eventsFallbackQuirks().setEnabled(QuirkId::EventStateNotPaired, true);

    EventEngine engine(nullptr);
    int count = 0;
    connect(&engine, &EventEngine::eventProduced, &engine,
            [&count](const EventMessage &) { ++count; });

    engine.trigger(EventKind::Motion, 60);
    QCOMPARE(count, 1);
    QTest::qWait(250);
    QCOMPARE(count, 1);
    QCOMPARE(engine.propertyStates().value(
                 QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion")), true);
}

void TestEvents::transientEventSendsOnce()
{
    EventEngine engine(nullptr);
    QVector<EventMessage> got;
    connect(&engine, &EventEngine::eventProduced, &engine,
            [&got](const EventMessage &m) { got.append(m); });

    engine.trigger(EventKind::LineCrossing, 60);
    QCOMPARE(got.size(), 1);
    QVERIFY(!got.at(0).isProperty);
    QVERIFY(!got.at(0).dataItems.value(QStringLiteral("ObjectId")).isEmpty());
    QTest::qWait(200);
    QCOMPARE(got.size(), 1);   // 瞬时事件没有配对的 false

    // 人 / 车分类走 Data 的 Type。
    got.clear();
    engine.trigger(EventKind::VehicleDetect, 60);
    QCOMPARE(got.size(), 1);
    QCOMPARE(got.at(0).dataItems.value(QStringLiteral("Type")), QStringLiteral("Vehicle"));
}

void TestEvents::topicStyleQuirkSwitchesCatalog()
{
    EventEngine engine(nullptr);
    QCOMPARE(TopicCatalog::findByKind(engine.topics(), EventKind::LineCrossing)->topic,
             QStringLiteral("tns1:RuleEngine/LineDetector/Crossed"));

    eventsFallbackQuirks().setEnabled(QuirkId::TopicNamingStyle, true);
    eventsFallbackQuirks().setParam(QuirkId::TopicNamingStyle, QStringLiteral("value"),
                                    QStringLiteral("tplink"));
    engine.reloadTopics();
    QCOMPARE(TopicCatalog::findByKind(engine.topics(), EventKind::LineCrossing)->topic,
             QStringLiteral("tns1:RuleEngine/LineCrossDetector/LineCross"));

    // 换风格后触发的是新 topic。
    QVector<EventMessage> got;
    connect(&engine, &EventEngine::eventProduced, &engine,
            [&got](const EventMessage &m) { got.append(m); });
    engine.trigger(EventKind::LineCrossing, 0);
    QCOMPARE(got.size(), 1);
    QCOMPARE(got.at(0).topic, QStringLiteral("tns1:RuleEngine/LineCrossDetector/LineCross"));
}

void TestEvents::synchronizationSnapshotIsInitialized()
{
    EventEngine engine(nullptr);
    engine.trigger(EventKind::Motion, 60000);   // 撑住 true，别在快照前复位

    const QVector<EventMessage> snapshot = engine.synchronizationSnapshot();
    QVERIFY(!snapshot.isEmpty());
    bool sawMotion = false;
    for (const EventMessage &m : snapshot) {
        QVERIFY(m.isProperty);
        QCOMPARE(m.propertyOperation, QStringLiteral("Initialized"));
        if (m.topic == QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion")) {
            sawMotion = true;
            QCOMPARE(m.dataItems.value(QStringLiteral("IsMotion")), QStringLiteral("true"));
        }
    }
    QVERIFY(sawMotion);
    // 瞬时 topic 不进快照。
    for (const EventMessage &m : snapshot)
        QVERIFY(m.topic != QStringLiteral("tns1:Monitoring/ProcessorUsage"));
}

void TestEvents::stormProducesEvents()
{
    EventEngine engine(nullptr);
    int count = 0;
    connect(&engine, &EventEngine::eventProduced, &engine,
            [&count](const EventMessage &) { ++count; });

    engine.startStorm(50);
    QVERIFY(engine.isStorming());
    QTRY_VERIFY_WITH_TIMEOUT(count >= 5, 2000);
    engine.stopStorm();
    QVERIFY(!engine.isStorming());
    const int frozen = count;
    QTest::qWait(150);
    QCOMPARE(count, frozen);
}

// --------------------------------------------------------- SubscriptionManager

void TestEvents::queueDropsOldestOnOverflow()
{
    SubscriptionManager mgr(nullptr);
    Subscription *sub = mgr.create(TopicFilter(), QDateTime::currentDateTimeUtc().addSecs(3600),
                                   QStringLiteral("1.2.3.4:5555"));
    QVERIFY(sub);
    QCOMPARE(mgr.count(), 1);
    QVERIFY(sub->address.startsWith(QStringLiteral("http://")));
    QVERIFY(sub->address.endsWith(sub->path));
    QCOMPARE(sub->creatorPeer, QStringLiteral("1.2.3.4:5555"));

    sub->maxQueue = 3;
    for (int i = 0; i < 5; ++i)
        mgr.deliver(makeMessage(QStringLiteral("tns1:Test/Event%1").arg(i)));

    QCOMPARE(sub->queue.size(), 3);
    QCOMPARE(sub->droppedCount, qint64(2));
    QCOMPARE(sub->queue.head().topic, QStringLiteral("tns1:Test/Event2"));   // 最老的被丢了
}

void TestEvents::deliverRespectsFilter()
{
    SubscriptionManager mgr(nullptr);
    Subscription *motionOnly =
        mgr.create(TopicFilter(QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion")),
                   QDateTime::currentDateTimeUtc().addSecs(3600), QString());
    Subscription *all = mgr.create(TopicFilter(QStringLiteral("//.")),
                                   QDateTime::currentDateTimeUtc().addSecs(3600), QString());
    QVERIFY(motionOnly && all);

    mgr.deliver(makeMessage(QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion")));
    mgr.deliver(makeMessage(QStringLiteral("tns1:VideoSource/MotionAlarm")));

    QCOMPARE(motionOnly->queue.size(), 1);
    QCOMPARE(all->queue.size(), 2);
}

void TestEvents::slotLimitFault()
{
    // D3：槽位满了直接回 Fault。
    Quirks &q = eventsFallbackQuirks();
    q.setEnabled(QuirkId::SubscriptionSlotLimit, true);
    q.setParam(QuirkId::SubscriptionSlotLimit, QStringLiteral("max"), 2);
    q.setParam(QuirkId::SubscriptionSlotLimit, QStringLiteral("on_overflow"),
               QStringLiteral("fault"));

    SubscriptionManager mgr(nullptr);
    const QDateTime ttl = QDateTime::currentDateTimeUtc().addSecs(3600);
    QVERIFY(mgr.create(TopicFilter(), ttl, QString()));
    QVERIFY(mgr.create(TopicFilter(), ttl, QString()));

    QString reason;
    QVERIFY(!mgr.create(TopicFilter(), ttl, QString(), &reason));
    QCOMPARE(reason, QStringLiteral("slot_limit_fault"));
    QCOMPARE(mgr.count(), 2);
}

void TestEvents::slotLimitEvictsOldest()
{
    // D3 的另一档：并发订阅互踢，老的那条被顶掉。
    Quirks &q = eventsFallbackQuirks();
    q.setEnabled(QuirkId::SubscriptionSlotLimit, true);
    q.setParam(QuirkId::SubscriptionSlotLimit, QStringLiteral("max"), 2);
    q.setParam(QuirkId::SubscriptionSlotLimit, QStringLiteral("on_overflow"),
               QStringLiteral("evict_oldest"));

    SubscriptionManager mgr(nullptr);
    const QDateTime ttl = QDateTime::currentDateTimeUtc().addSecs(3600);
    Subscription *first = mgr.create(TopicFilter(), ttl, QString());
    QVERIFY(first);
    const QString firstId = first->id;
    QVERIFY(mgr.create(TopicFilter(), ttl, QString()));

    QSignalSpy removed(&mgr, &SubscriptionManager::subscriptionRemoved);
    QString reason;
    Subscription *third = mgr.create(TopicFilter(), ttl, QString(), &reason);
    QVERIFY(third);
    QVERIFY(reason.isEmpty());
    QCOMPARE(mgr.count(), 2);
    QCOMPARE(removed.size(), 1);
    QCOMPARE(removed.at(0).at(0).toString(), firstId);
    QVERIFY(!mgr.find(firstId));
}

void TestEvents::slotLimitSilentFail()
{
    // D3 最坏的一档：不报错也不建，客户端拉半天拉不到任何东西。
    Quirks &q = eventsFallbackQuirks();
    q.setEnabled(QuirkId::SubscriptionSlotLimit, true);
    q.setParam(QuirkId::SubscriptionSlotLimit, QStringLiteral("max"), 1);
    q.setParam(QuirkId::SubscriptionSlotLimit, QStringLiteral("on_overflow"),
               QStringLiteral("silent_fail"));

    SubscriptionManager mgr(nullptr);
    const QDateTime ttl = QDateTime::currentDateTimeUtc().addSecs(3600);
    QVERIFY(mgr.create(TopicFilter(), ttl, QString()));

    QString reason;
    QVERIFY(!mgr.create(TopicFilter(), ttl, QString(), &reason));
    QCOMPARE(reason, QStringLiteral("slot_limit_silent"));
    QCOMPARE(mgr.count(), 1);
}

void TestEvents::reapRemovesExpired()
{
    SubscriptionManager mgr(nullptr);
    Subscription *sub = mgr.create(TopicFilter(),
                                   QDateTime::currentDateTimeUtc().addSecs(-10), QString());
    QVERIFY(sub);
    QVERIFY(sub->expired());
    QCOMPARE(mgr.count(), 1);

    mgr.reapExpired();
    QCOMPARE(mgr.count(), 0);
}

void TestEvents::neverExpiresQuirkLeaksSlots()
{
    // A9：参照客户端每次建连都建一条订阅且从不 Unsubscribe。关掉回收就是真实的槽位泄漏。
    eventsFallbackQuirks().setEnabled(QuirkId::SubscriptionNeverExpires, true);

    SubscriptionManager mgr(nullptr);
    for (int i = 0; i < 5; ++i)
        QVERIFY(mgr.create(TopicFilter(), QDateTime::currentDateTimeUtc().addSecs(-10), QString()));

    mgr.reapExpired();
    QCOMPARE(mgr.count(), 5);   // 一条都没回收

    // 关掉 quirk 之后才回收得动。
    eventsFallbackQuirks().setEnabled(QuirkId::SubscriptionNeverExpires, false);
    mgr.reapExpired();
    QCOMPARE(mgr.count(), 0);
}

void TestEvents::portIncrementQuirk()
{
    // D1：订阅管理器开在独立端口且每次递增，路径形如 /event-1024_1024。
    Quirks &q = eventsFallbackQuirks();
    q.setEnabled(QuirkId::SubscriptionPortIncrement, true);
    q.setParam(QuirkId::SubscriptionPortIncrement, QStringLiteral("base_port"), 1024);

    SubscriptionManager mgr(nullptr);
    const QDateTime ttl = QDateTime::currentDateTimeUtc().addSecs(3600);
    Subscription *a = mgr.create(TopicFilter(), ttl, QString());
    Subscription *b = mgr.create(TopicFilter(), ttl, QString());
    QVERIFY(a && b);

    QCOMPARE(a->port, quint16(1024));
    QCOMPARE(a->path, QStringLiteral("/event-1024_1024"));
    QCOMPARE(b->port, quint16(1025));
    QCOMPARE(b->path, QStringLiteral("/event-1025_1025"));
    QVERIFY(a->address.contains(QStringLiteral(":1024/event-1024_1024")));

    // 订阅地址原样当 wsa:To 发回来也要找得到。
    QCOMPARE(mgr.find(b->address), b);
    QCOMPARE(mgr.find(b->path), b);
    QCOMPARE(mgr.find(b->id), b);
}

void TestEvents::expiresAtOnceQuirk()
{
    eventsFallbackQuirks().setEnabled(QuirkId::SubscriptionExpiresAtOnce, true);

    SubscriptionManager mgr(nullptr);
    Subscription *sub = mgr.create(TopicFilter(),
                                   QDateTime::currentDateTimeUtc().addSecs(3600), QString());
    QVERIFY(sub);
    QVERIFY(sub->terminationTime < QDateTime::currentDateTimeUtc());
    QVERIFY(sub->expired());
}

void TestEvents::pullReturnsQueuedImmediately()
{
    SubscriptionManager mgr(nullptr);
    Subscription *sub = mgr.create(TopicFilter(), QDateTime::currentDateTimeUtc().addSecs(3600),
                                   QString());
    QVERIFY(sub);
    mgr.deliver(makeMessage(QStringLiteral("tns1:VideoSource/MotionAlarm")));
    mgr.deliver(makeMessage(QStringLiteral("tns1:VideoSource/ImageTooDark")));

    PullResult r;
    mgr.pull(sub->id, 100, 8000, HttpExchangePtr(), r.responder());

    QCOMPARE(r.calls, 1);              // 队列里有货就当场回，不挂起
    QCOMPARE(r.messages.size(), 2);
    QVERIFY(r.subscriptionAlive);
    QCOMPARE(mgr.pendingPullCount(), 0);
    QCOMPARE(sub->pulledCount, qint64(2));
    QVERIFY(sub->queue.isEmpty());

    // MessageLimit 生效。
    for (int i = 0; i < 5; ++i)
        mgr.deliver(makeMessage(QStringLiteral("tns1:Test/E%1").arg(i)));
    PullResult limited;
    mgr.pull(sub->id, 2, 8000, HttpExchangePtr(), limited.responder());
    QCOMPARE(limited.messages.size(), 2);
    QCOMPARE(sub->queue.size(), 3);
}

void TestEvents::pullBlocksThenReturnsEmpty()
{
    SubscriptionManager mgr(nullptr);
    Subscription *sub = mgr.create(TopicFilter(), QDateTime::currentDateTimeUtc().addSecs(3600),
                                   QString());
    QVERIFY(sub);

    PullResult r;
    mgr.pull(sub->id, 100, 200, HttpExchangePtr(), r.responder());
    QCOMPARE(r.calls, 0);                  // 队列空 → 挂起，绝不阻塞事件循环
    QCOMPARE(mgr.pendingPullCount(), 1);

    QTRY_COMPARE_WITH_TIMEOUT(r.calls, 1, 2000);
    QVERIFY(r.messages.isEmpty());         // 空拉取是正常心跳，不是错误
    QVERIFY(r.subscriptionAlive);
    QCOMPARE(mgr.pendingPullCount(), 0);
}

void TestEvents::pullWakesOnDelivery()
{
    SubscriptionManager mgr(nullptr);
    Subscription *sub = mgr.create(TopicFilter(), QDateTime::currentDateTimeUtc().addSecs(3600),
                                   QString());
    QVERIFY(sub);

    PullResult r;
    mgr.pull(sub->id, 100, 8000, HttpExchangePtr(), r.responder());
    QCOMPARE(mgr.pendingPullCount(), 1);

    mgr.deliver(makeMessage(QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion")));
    QCOMPARE(r.calls, 1);                  // 挂起的 Pull 被新事件立刻唤醒
    QCOMPARE(r.messages.size(), 1);
    QCOMPARE(mgr.pendingPullCount(), 0);
    QVERIFY(sub->queue.isEmpty());
}

void TestEvents::pullAlwaysEmptyQuirk()
{
    // D10：订阅建得起来、Pull 也不报错，就是永远没有事件。
    eventsFallbackQuirks().setEnabled(QuirkId::PullMessagesAlwaysEmpty, true);

    SubscriptionManager mgr(nullptr);
    Subscription *sub = mgr.create(TopicFilter(), QDateTime::currentDateTimeUtc().addSecs(3600),
                                   QString());
    QVERIFY(sub);
    mgr.deliver(makeMessage(QStringLiteral("tns1:RuleEngine/CellMotionDetector/Motion")));
    QCOMPARE(sub->queue.size(), 1);        // 队列里其实有货

    PullResult r;
    mgr.pull(sub->id, 100, 200, HttpExchangePtr(), r.responder());
    QCOMPARE(r.calls, 0);
    QTRY_COMPARE_WITH_TIMEOUT(r.calls, 1, 2000);
    QVERIFY(r.messages.isEmpty());
    QVERIFY(r.subscriptionAlive);
    QCOMPARE(sub->queue.size(), 1);
}

void TestEvents::pullUnknownSubscription()
{
    SubscriptionManager mgr(nullptr);
    PullResult r;
    mgr.pull(QStringLiteral("sub-999"), 100, 8000, HttpExchangePtr(), r.responder());
    QCOMPARE(r.calls, 1);
    QVERIFY(!r.subscriptionAlive);         // sub == nullptr → 服务层回 Fault
    QCOMPARE(mgr.pendingPullCount(), 0);

    // Unsubscribe 之后挂起的 Pull 也要被交回去，不能一直吊着。
    Subscription *sub = mgr.create(TopicFilter(), QDateTime::currentDateTimeUtc().addSecs(3600),
                                   QString());
    QVERIFY(sub);
    const QString id = sub->id;
    PullResult pendingPull;
    mgr.pull(id, 100, 8000, HttpExchangePtr(), pendingPull.responder());
    QCOMPARE(mgr.pendingPullCount(), 1);
    QVERIFY(mgr.unsubscribe(id));
    QCOMPARE(pendingPull.calls, 1);
    QVERIFY(!pendingPull.subscriptionAlive);
    QCOMPARE(mgr.pendingPullCount(), 0);
    QCOMPARE(mgr.count(), 0);
}

QTEST_MAIN(TestEvents)
#include "tst_events.moc"
