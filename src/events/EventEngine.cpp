#include "events/EventEngine.h"

#include "core/VirtualCamera.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QRandomGenerator>
#include <QtCore/QTimer>

#include <limits>
#include <utility>

namespace onvifsim {

// 定义在 EventTypes.cpp：camera 为空（单元测试）时的 quirk 来源。
Quirks &eventsFallbackQuirks();

namespace {

// GUI 只按一下按钮、REST 不带 duration 时，属性型事件默认持续这么久再发 false。
// 取 5s 是因为真机的移动侦测「事件段」普遍就在这个量级。
constexpr int kDefaultDurationMs = 5000;

// 事件风暴用固定 100ms 的节拍，靠余数累加逼近目标速率 ——
// 直接用 1000/N 当间隔的话，N 大了会退化成 1ms 定时器，把事件循环压死。
constexpr int kStormTickMs = 100;

// 风暴与统计信号的节流窗口：每秒上万条时，GUI 不需要每条都刷新一次。
constexpr int kStatsThrottleMs = 200;

// 人 / 车 / 动物 / 人脸走 Data 的 Type，客户端就是靠它做分类的。
QString classificationFor(EventKind kind)
{
    switch (kind) {
    case EventKind::PeopleDetect:  return QStringLiteral("Human");
    case EventKind::VehicleDetect: return QStringLiteral("Vehicle");
    case EventKind::AnimalDetect:  return QStringLiteral("Animal");
    case EventKind::FaceDetect:    return QStringLiteral("Face");
    default:                       return QString();
    }
}

} // namespace

struct EventEngine::Private {
    Private(EventEngine *owner, VirtualCamera *cam) : q(owner), camera(cam) {}

    EventEngine *q = nullptr;
    VirtualCamera *camera = nullptr;

    QString style;
    QVector<TopicDef> topics;
    QMap<QString, bool> propertyStates;      // topic -> 当前状态
    QHash<QString, QTimer *> offTimers;      // topic -> 配对 false 的定时器

    QVector<EventSchedule> schedules;
    QHash<int, QTimer *> scheduleTimers;     // EventKind -> 定时器

    QTimer *stormTimer = nullptr;
    bool stormFromQuirk = false;             // 由 quirk 起的风暴才允许被 quirk 停掉
    int stormPerSecond = 0;
    double stormCarry = 0.0;
    int stormCursor = 0;

    QDateTime lastEventTime;
    QString lastEventTopic;
    qint64 total = 0;
    qint64 objectIdSeq = 0;
    QElapsedTimer statsClock;
    qint64 lastStatsEmit = 0;

    const Quirks &quirks() const;
    QString styleFromConfig() const;
    QDateTime nowUtc() const;

    const TopicDef *find(EventKind kind) const;
    const TopicDef *findTopic(const QString &topic) const;

    EventMessage build(const TopicDef &def, bool state, const QString &operation);
    void send(const EventMessage &m, bool throttleStats);
    void scheduleOff(const TopicDef &def, int durationMs);
    void cancelOff(const QString &topic);

    void rearm(const EventSchedule &s);
    void clearScheduleTimer(EventKind kind);
    void syncStormWithQuirk();
    void stormTick();
};

const Quirks &EventEngine::Private::quirks() const
{
    return camera ? camera->quirks() : eventsFallbackQuirks();
}

QString EventEngine::Private::styleFromConfig() const
{
    // quirk D9 优先，没开就听品牌预设的。
    const QString fromPersona =
        camera ? camera->persona().topicStyle : QStringLiteral("onvif");
    return quirks().choice(QuirkId::TopicNamingStyle,
                           fromPersona.isEmpty() ? QStringLiteral("onvif") : fromPersona);
}

QDateTime EventEngine::Private::nowUtc() const
{
    // 走设备时钟：A8 的 clock_skew 要能把事件时间戳一起带偏。
    return camera ? camera->deviceTimeUtc() : QDateTime::currentDateTimeUtc();
}

const TopicDef *EventEngine::Private::find(EventKind kind) const
{
    return TopicCatalog::findByKind(topics, kind);
}

const TopicDef *EventEngine::Private::findTopic(const QString &topic) const
{
    // 循环变量不能叫 d：会遮蔽 Private 自己的 d 成员，MSVC 直接报 C4458。
    for (const TopicDef &candidate : topics) {
        if (candidate.topic.compare(topic, Qt::CaseInsensitive) == 0)
            return &candidate;
    }
    // 客户端 / REST 常把 tns1: 前缀省了，末段对上也算。
    for (const TopicDef &candidate : topics) {
        if (candidate.topic.endsWith(topic, Qt::CaseInsensitive))
            return &candidate;
    }
    return nullptr;
}

EventMessage EventEngine::Private::build(const TopicDef &def, bool state,
                                         const QString &operation)
{
    EventMessage m;
    m.utcTime = nowUtc();
    m.topic = def.topic;
    m.isProperty = def.isProperty;
    m.propertyOperation = operation;

    if (!def.sourceItemName.isEmpty())
        m.sourceItems.insert(def.sourceItemName, def.sourceItemValue);
    if (!def.ruleItemName.isEmpty())
        m.sourceItems.insert(def.ruleItemName, def.ruleItemValue);

    if (!def.dataItemName.isEmpty()) {
        if (def.dataItemName == QLatin1String("ObjectId")) {
            // 瞬时的越界事件，Data 是对象序号而不是状态位。
            m.dataItems.insert(def.dataItemName, QString::number(++objectIdSeq));
        } else if (def.dataItemName == QLatin1String("Value")) {
            // Monitoring/ProcessorUsage 这类数值型，给个像样的占用率。
            const double usage = 0.05 + QRandomGenerator::global()->bounded(90) / 100.0;
            m.dataItems.insert(def.dataItemName, QString::number(usage, 'f', 2));
        } else {
            m.dataItems.insert(def.dataItemName,
                               state ? QStringLiteral("true") : QStringLiteral("false"));
        }
    }

    const QString cls = classificationFor(def.kind);
    if (!cls.isEmpty())
        m.dataItems.insert(QStringLiteral("Type"), cls);

    return m;
}

void EventEngine::Private::send(const EventMessage &m, bool throttleStats)
{
    ++total;
    lastEventTime = m.utcTime;
    lastEventTopic = m.topic;

    emit q->eventProduced(m);

    if (throttleStats) {
        const qint64 now = statsClock.elapsed();
        if (now - lastStatsEmit < kStatsThrottleMs)
            return;
        lastStatsEmit = now;
    }
    emit q->statisticsChanged();
}

void EventEngine::Private::scheduleOff(const TopicDef &def, int durationMs)
{
    cancelOff(def.topic);

    QTimer *t = new QTimer(q);
    t->setSingleShot(true);
    t->setTimerType(Qt::CoarseTimer);
    // 按值捕获 TopicDef：reloadTopics() 随时可能整表重建，指针会失效。
    const TopicDef copy = def;
    QObject::connect(t, &QTimer::timeout, q, [this, copy]() {
        QTimer *done = offTimers.take(copy.topic);
        if (done)
            done->deleteLater();
        propertyStates.insert(copy.topic, false);
        send(build(copy, false, QStringLiteral("Changed")), false);
    });
    offTimers.insert(def.topic, t);
    t->start(durationMs);
}

void EventEngine::Private::cancelOff(const QString &topic)
{
    if (QTimer *t = offTimers.take(topic)) {
        t->stop();
        t->deleteLater();
    }
}

void EventEngine::Private::rearm(const EventSchedule &s)
{
    clearScheduleTimer(s.kind);
    if (!s.enabled || s.intervalSec <= 0)
        return;

    QTimer *t = new QTimer(q);
    t->setSingleShot(true);   // 每次重排，抖动才有意义
    t->setTimerType(Qt::CoarseTimer);
    const EventKind kind = s.kind;
    QObject::connect(t, &QTimer::timeout, q, [this, kind]() {
        for (const EventSchedule &cur : schedules) {
            if (cur.kind != kind || !cur.enabled)
                continue;
            q->trigger(kind, cur.durationSec > 0 ? cur.durationSec * 1000 : kDefaultDurationMs);
            rearm(cur);
            return;
        }
        clearScheduleTimer(kind);
    });
    scheduleTimers.insert(static_cast<int>(kind), t);

    // 全程用 qint64 算：界面上的间隔最大能填 86400 秒，intervalSec * 1000 * 140
    // 早就越过了 int 的量程。一旦溢出成负数，qMax(100, ms) 会把它钉成 100 ——
    // 「每 12 小时报一次移动侦测」当场变成「每 100 毫秒报一次」，
    // 订阅队列瞬间打满。有符号溢出本身还是未定义行为。
    qint64 ms = static_cast<qint64>(s.intervalSec) * 1000;
    if (s.randomJitter)   // ±40%，让多台相机的事件不排成整齐的一列
        ms = ms * (60 + QRandomGenerator::global()->bounded(81)) / 100;
    t->start(static_cast<int>(qBound<qint64>(100, ms, qint64(std::numeric_limits<int>::max()))));
}

void EventEngine::Private::clearScheduleTimer(EventKind kind)
{
    if (QTimer *t = scheduleTimers.take(static_cast<int>(kind))) {
        t->stop();
        t->deleteLater();
    }
}

void EventEngine::Private::syncStormWithQuirk()
{
    const bool on = quirks().isEnabled(QuirkId::EventStorm);
    if (on) {
        const int n = quirks().paramInt(QuirkId::EventStorm, QStringLiteral("per_second"));
        q->startStorm(n);
        stormFromQuirk = true;
    } else if (stormFromQuirk) {
        // 只收回自己开的：GUI / REST 手动起的风暴不该被 quirk 刷新顺手停掉。
        q->stopStorm();
        stormFromQuirk = false;
    }
}

void EventEngine::Private::stormTick()
{
    if (topics.isEmpty() || stormPerSecond <= 0)
        return;

    stormCarry += stormPerSecond * (kStormTickMs / 1000.0);
    int n = static_cast<int>(stormCarry);
    stormCarry -= n;

    while (n-- > 0) {
        if (stormCursor >= topics.size())
            stormCursor = 0;
        const TopicDef &def = topics.at(stormCursor++);
        // 风暴只灌管道，不改属性状态 —— 否则 SetSynchronizationPoint 的快照会被搅乱。
        send(build(def, true, QStringLiteral("Changed")), true);
    }
}

EventEngine::EventEngine(VirtualCamera *camera, QObject *parent)
    : QObject(parent), d(new Private(this, camera))
{
    d->statsClock.start();
    reloadTopics();
}

EventEngine::~EventEngine()
{
    stopStorm();
    for (QTimer *t : std::as_const(d->offTimers))
        t->stop();
    for (QTimer *t : std::as_const(d->scheduleTimers))
        t->stop();
    delete d;
}

QVector<TopicDef> EventEngine::topics() const
{
    return d->topics;
}

void EventEngine::reloadTopics()
{
    const QString style = d->styleFromConfig();
    d->style = style;
    d->topics = TopicCatalog::topicsForStyle(style);

    // 属性状态表跟着 topic 集走：新 topic 补一个 false（SetSynchronizationPoint
    // 要能立刻回一份完整快照），换风格后消失的 topic 连同它的配对定时器一起丢。
    QMap<QString, bool> kept;
    for (const TopicDef &def : d->topics) {
        if (!def.isProperty)
            continue;
        kept.insert(def.topic, d->propertyStates.value(def.topic, false));
    }
    for (auto it = d->propertyStates.constBegin(); it != d->propertyStates.constEnd(); ++it) {
        if (!kept.contains(it.key()))
            d->cancelOff(it.key());
    }
    d->propertyStates = kept;

    d->syncStormWithQuirk();
    emit topicsChanged();
}

void EventEngine::syncStorm()
{
    d->syncStormWithQuirk();
}

void EventEngine::trigger(EventKind kind, int durationMs)
{
    const TopicDef *def = d->find(kind);
    if (!def)
        return;
    trigger(def->topic, true, durationMs);
}

void EventEngine::trigger(const QString &topic, bool state, int durationMs)
{
    const TopicDef *def = d->findTopic(topic);
    if (!def) {
        // 目录之外的 topic 也让打 —— 场景文件与 REST 要能试探客户端的未知 topic 处理。
        EventMessage m;
        m.utcTime = d->nowUtc();
        m.topic = topic;
        m.isProperty = true;
        m.propertyOperation = QStringLiteral("Changed");
        m.dataItems.insert(QStringLiteral("State"),
                           state ? QStringLiteral("true") : QStringLiteral("false"));
        d->send(m, false);
        return;
    }

    if (!def->isProperty) {
        d->send(d->build(*def, state, QStringLiteral("Changed")), false);
        return;
    }

    const TopicDef copy = *def;   // send() 之下有可能重建 topic 表
    d->cancelOff(copy.topic);
    d->propertyStates.insert(copy.topic, state);
    d->send(d->build(copy, state, QStringLiteral("Changed")), false);

    if (!state)
        return;
    // D11：真机上有只发 true 不发 false 的，客户端的移动侦测就此永远不复位。
    if (d->quirks().isEnabled(QuirkId::EventStateNotPaired))
        return;
    d->scheduleOff(copy, durationMs > 0 ? durationMs : kDefaultDurationMs);
}

QMap<QString, bool> EventEngine::propertyStates() const
{
    return d->propertyStates;
}

QVector<EventMessage> EventEngine::synchronizationSnapshot() const
{
    QVector<EventMessage> out;
    for (const TopicDef &def : d->topics) {
        if (!def.isProperty)
            continue;
        // PropertyOperation=Initialized 是 SetSynchronizationPoint 的关键：
        // 客户端据此知道这是「当前状态」而不是一次新的状态变化。
        out.append(d->build(def, d->propertyStates.value(def.topic, false),
                            QStringLiteral("Initialized")));
    }
    return out;
}

QVector<EventSchedule> EventEngine::schedules() const
{
    return d->schedules;
}

void EventEngine::setSchedules(const QVector<EventSchedule> &schedules)
{
    for (auto it = d->scheduleTimers.constBegin(); it != d->scheduleTimers.constEnd(); ++it) {
        it.value()->stop();
        it.value()->deleteLater();
    }
    d->scheduleTimers.clear();

    d->schedules = schedules;
    for (const EventSchedule &s : d->schedules)
        d->rearm(s);
    emit schedulesChanged();
}

void EventEngine::startStorm(int perSecond)
{
    if (perSecond <= 0) {
        stopStorm();
        return;
    }
    d->stormPerSecond = qBound(1, perSecond, 10000);
    d->stormCarry = 0.0;
    if (!d->stormTimer) {
        d->stormTimer = new QTimer(this);
        d->stormTimer->setTimerType(Qt::CoarseTimer);
        connect(d->stormTimer, &QTimer::timeout, this, [this]() { d->stormTick(); });
    }
    d->stormTimer->start(kStormTickMs);
}

void EventEngine::stopStorm()
{
    if (d->stormTimer)
        d->stormTimer->stop();
    d->stormPerSecond = 0;
    d->stormCarry = 0.0;
}

bool EventEngine::isStorming() const
{
    return d->stormTimer && d->stormTimer->isActive();
}

QDateTime EventEngine::lastEventTime() const
{
    return d->lastEventTime;
}

QString EventEngine::lastEventTopic() const
{
    return d->lastEventTopic;
}

qint64 EventEngine::totalEventsSent() const
{
    return d->total;
}

} // namespace onvifsim
