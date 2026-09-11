#include "events/SubscriptionManager.h"

#include "core/VirtualCamera.h"

#include <QtCore/QTimer>
#include <QtCore/QUrl>

#include <utility>

namespace onvifsim {

// 定义在 EventTypes.cpp：camera 为空（单元测试）时的 quirk 来源。
Quirks &eventsFallbackQuirks();

namespace {

// PullMessages 的 Timeout 上限。参照客户端发的是 PT8S，给到 5 分钟足够，
// 再长就只是白占一个 HTTP 连接。
constexpr int kMaxPullTimeoutMs = 300000;

// 一次 Pull 最多回多少条。客户端发的 MessageLimit 是 100，这里只兜住离谱值。
constexpr int kMaxMessageLimit = 1024;

// 没给 InitialTerminationTime 时的默认 TTL（客户端发的是 PT1H）。
constexpr int kDefaultTtlSec = 3600;

// 过期订阅的后台回收周期。A9 打开时 reapExpired() 什么都不做，这个定时器
// 照转不误，槽位就这么一直泄漏下去 —— 与真机表现一致。
constexpr int kReapIntervalMs = 30000;

} // namespace

bool Subscription::expired() const
{
    return terminationTime.isValid() && terminationTime <= QDateTime::currentDateTimeUtc();
}

// 一次挂起的长轮询。队列空时 PullMessages 就变成这个东西躺在这里，
// 直到超时、来了新事件、或者客户端把连接掐了。
struct PendingPull {
    QString subscriptionId;
    HttpExchangePtr exchange;
    SubscriptionManager::PullResponder responder;
    QTimer *timer = nullptr;
    int messageLimit = 100;
    QMetaObject::Connection abortConn;
};

struct SubscriptionManager::Private {
    Private(SubscriptionManager *owner, VirtualCamera *cam) : q(owner), camera(cam) {}

    SubscriptionManager *q = nullptr;
    VirtualCamera *camera = nullptr;

    QList<Subscription *> subs;
    QList<PendingPull *> pending;
    QTimer *reaper = nullptr;

    quint16 nextPort = 0;   // D1 递增用的游标，0 = 还没分配过
    int counter = 0;        // 生成订阅 id / 路径的序号

    const Quirks &quirks() const;
    QDateTime nowUtc() const;
    bool ownPortMode() const;

    QString advertisedHost() const;
    quint16 advertisedPort() const;

    QVector<EventMessage> take(Subscription *sub, int limit) const;
    void destroySubscription(Subscription *sub);
    void finishPending(PendingPull *p, Subscription *sub, const QVector<EventMessage> &msgs);
    void dropPending(PendingPull *p, bool respondGone);
    void wakePending(Subscription *sub);
};

const Quirks &SubscriptionManager::Private::quirks() const
{
    return camera ? camera->quirks() : eventsFallbackQuirks();
}

QDateTime SubscriptionManager::Private::nowUtc() const
{
    return camera ? camera->deviceTimeUtc() : QDateTime::currentDateTimeUtc();
}

bool SubscriptionManager::Private::ownPortMode() const
{
    // D1 是显式开关，品牌预设也能默认打开（TL-IPC 就是这么长的）。
    return quirks().isEnabled(QuirkId::SubscriptionPortIncrement)
           || (camera && camera->persona().subscriptionOwnPort);
}

QString SubscriptionManager::Private::advertisedHost() const
{
    // D2：固件把自己那个不可达的内网地址写进订阅 URL，客户端要么接管 host 要么卡死。
    if (quirks().isEnabled(QuirkId::SubscriptionHostUnreachable)) {
        const QString bad = quirks().paramString(QuirkId::SubscriptionHostUnreachable,
                                                 QStringLiteral("address"));
        if (!bad.isEmpty())
            return bad;
    }
    const QString host = camera ? camera->advertisedHost() : QString();
    return host.isEmpty() ? QStringLiteral("127.0.0.1") : host;
}

quint16 SubscriptionManager::Private::advertisedPort() const
{
    const quint16 port = camera ? camera->advertisedHttpPort() : 0;
    return port == 0 ? 80 : port;
}

QVector<EventMessage> SubscriptionManager::Private::take(Subscription *sub, int limit) const
{
    QVector<EventMessage> out;
    while (!sub->queue.isEmpty() && out.size() < limit)
        out.append(sub->queue.dequeue());
    sub->pulledCount += out.size();
    return out;
}

// 回调一律在把挂起项摘干净、对象销毁之后再发：responder 里如果又回头动了
// 订阅（比如服务层顺手 Unsubscribe），不至于把自己脚下的内存拆了。
void SubscriptionManager::Private::finishPending(PendingPull *p, Subscription *sub,
                                                 const QVector<EventMessage> &msgs)
{
    pending.removeOne(p);
    QObject::disconnect(p->abortConn);
    if (p->timer)
        p->timer->deleteLater();
    PullResponder responder = std::move(p->responder);
    const HttpExchangePtr exchange = p->exchange;
    delete p;
    if (responder)
        responder(exchange, sub, msgs);
}

void SubscriptionManager::Private::dropPending(PendingPull *p, bool respondGone)
{
    pending.removeOne(p);
    QObject::disconnect(p->abortConn);
    if (p->timer)
        p->timer->deleteLater();
    PullResponder responder = std::move(p->responder);
    const HttpExchangePtr exchange = p->exchange;
    delete p;
    // 订阅没了（Unsubscribe / 过期回收 / 被挤掉）：把挂着的 Pull 用 sub=nullptr
    // 交回服务层，让它回一条「订阅不存在」的 Fault，而不是让请求一直吊着。
    if (respondGone && responder)
        responder(exchange, nullptr, {});
}

void SubscriptionManager::Private::wakePending(Subscription *sub)
{
    // D10：假装队列永远是空的，Pull 就一直挂到超时 —— 订阅正常、不报错、没事件。
    if (quirks().isEnabled(QuirkId::PullMessagesAlwaysEmpty))
        return;

    const QList<PendingPull *> snapshot = pending;
    for (PendingPull *p : snapshot) {
        if (!pending.contains(p) || p->subscriptionId != sub->id)
            continue;
        if (sub->queue.isEmpty())
            return;
        finishPending(p, sub, take(sub, p->messageLimit));
    }
}

void SubscriptionManager::Private::destroySubscription(Subscription *sub)
{
    const QString id = sub->id;
    const QList<PendingPull *> snapshot = pending;
    for (PendingPull *p : snapshot) {
        if (pending.contains(p) && p->subscriptionId == id)
            dropPending(p, true);
    }
    subs.removeOne(sub);
    delete sub;
    emit q->subscriptionRemoved(id);
}

SubscriptionManager::SubscriptionManager(VirtualCamera *camera, QObject *parent)
    : QObject(parent), d(new Private(this, camera))
{
    d->reaper = new QTimer(this);
    d->reaper->setTimerType(Qt::CoarseTimer);
    connect(d->reaper, &QTimer::timeout, this, &SubscriptionManager::reapExpired);
    d->reaper->start(kReapIntervalMs);
}

SubscriptionManager::~SubscriptionManager()
{
    const QList<PendingPull *> snapshot = d->pending;
    for (PendingPull *p : snapshot)
        d->dropPending(p, false);
    for (Subscription *sub : std::as_const(d->subs))
        delete sub;
    d->subs.clear();
    delete d;
}

Subscription *SubscriptionManager::create(const TopicFilter &filter,
                                          const QDateTime &terminationTime,
                                          const QString &creatorPeer, QString *failureReason)
{
    if (failureReason)
        failureReason->clear();

    // 先回收再判上限：A9 打开时这一步是空转，槽位就此只增不减。
    reapExpired();

    const Quirks &q = d->quirks();
    if (q.isEnabled(QuirkId::SubscriptionSlotLimit)) {
        const int maxSlots = qMax(1, q.paramInt(QuirkId::SubscriptionSlotLimit,
                                                QStringLiteral("max")));
        const QString mode = q.paramString(QuirkId::SubscriptionSlotLimit,
                                           QStringLiteral("on_overflow"));
        while (d->subs.size() >= maxSlots) {
            if (mode == QLatin1String("evict_oldest")) {
                // 真机实测：并发订阅互踢，生产订阅会被探测订阅顶掉后 churn 重连。
                d->destroySubscription(d->subs.first());
                continue;
            }
            // 服务层按这两个约定串决定回什么：
            //   slot_limit_fault  → SOAP Fault
            //   slot_limit_silent → 照常回一个「成功」，但订阅其实没建（后续 Pull 全 Fault）
            if (failureReason) {
                *failureReason = (mode == QLatin1String("silent_fail"))
                                     ? QStringLiteral("slot_limit_silent")
                                     : QStringLiteral("slot_limit_fault");
            }
            return nullptr;
        }
    }

    Subscription *sub = new Subscription;
    sub->id = QStringLiteral("sub-%1").arg(++d->counter);
    sub->filter = filter;
    sub->createdAt = d->nowUtc();
    sub->creatorPeer = creatorPeer;
    sub->port = nextSubscriptionPort();
    // D1 真机形如 /event-1025_1025；同端口模式下给一个自己的路径，
    // 客户端必须用返回的地址而不是事件服务地址去 Pull。
    sub->path = sub->port != 0 ? QStringLiteral("/event-%1_%1").arg(sub->port)
                               : QStringLiteral("/onvif/subscription-%1").arg(d->counter);
    sub->address = QStringLiteral("http://%1:%2%3")
                       .arg(d->advertisedHost())
                       .arg(sub->port != 0 ? sub->port : d->advertisedPort())
                       .arg(sub->path);

    sub->terminationTime = terminationTime.isValid()
                               ? terminationTime.toUTC()
                               : QDateTime::currentDateTimeUtc().addSecs(kDefaultTtlSec);
    if (q.isEnabled(QuirkId::SubscriptionExpiresAtOnce)) {
        // 建得成功，TerminationTime 却已经是过去时 —— 客户端要么立刻续订要么白跑。
        sub->terminationTime = QDateTime::currentDateTimeUtc().addSecs(-1);
    }

    d->subs.append(sub);
    const QString newId = sub->id;
    emit subscriptionCreated(newId);
    emit countChanged(count());
    // 信号发完之后再确认一次：槽里理论上可以把这条订阅退掉（D1 的独立端口
    // 就挂在 subscriptionCreated 上），那样 sub 已经是野指针了。
    return find(newId);
}

Subscription *SubscriptionManager::find(const QString &idOrPath) const
{
    if (idOrPath.isEmpty())
        return nullptr;

    // 客户端把订阅地址原样当 wsa:To 发回来，所以整条 URL 也要认。
    QString path = idOrPath;
    if (path.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
        || path.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
        path = QUrl(idOrPath).path();
    }
    const int query = path.indexOf(QLatin1Char('?'));
    if (query >= 0)
        path.truncate(query);
    while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);

    for (Subscription *s : d->subs) {
        if (s->id == idOrPath || s->address == idOrPath || s->path == path)
            return s;
    }
    return nullptr;
}

bool SubscriptionManager::renew(const QString &id, const QDateTime &newTerminationTime)
{
    Subscription *sub = find(id);
    if (!sub)
        return false;
    // Renew 失败后客户端应该重建订阅而不是放弃，这条 quirk 就是来验那段代码的。
    if (d->quirks().isEnabled(QuirkId::RenewFails))
        return false;

    sub->terminationTime = newTerminationTime.isValid()
                               ? newTerminationTime.toUTC()
                               : QDateTime::currentDateTimeUtc().addSecs(kDefaultTtlSec);
    if (d->quirks().isEnabled(QuirkId::SubscriptionExpiresAtOnce))
        sub->terminationTime = QDateTime::currentDateTimeUtc().addSecs(-1);
    return true;
}

bool SubscriptionManager::unsubscribe(const QString &id)
{
    Subscription *sub = find(id);
    if (!sub)
        return false;
    d->destroySubscription(sub);
    emit countChanged(count());
    return true;
}

QList<Subscription *> SubscriptionManager::subscriptions() const
{
    return d->subs;
}

int SubscriptionManager::count() const
{
    return static_cast<int>(d->subs.size());
}

void SubscriptionManager::clear()
{
    const QList<Subscription *> snapshot = d->subs;
    for (Subscription *sub : snapshot)
        d->destroySubscription(sub);
    emit countChanged(count());
}

void SubscriptionManager::pull(const QString &id, int messageLimit, int timeoutMs,
                               const HttpExchangePtr &exchange, PullResponder responder)
{
    if (!responder)
        return;

    Subscription *sub = find(id);
    if (!sub) {
        // 订阅不存在（没建过 / 过期回收了 / 被挤掉了）：交回服务层回 Fault。
        responder(exchange, nullptr, {});
        return;
    }
    // 已经过期的订阅不能再拉：回收定时器 30 秒才跑一次，中间这段时间
    // 订阅还挂在表里，但对客户端来说它已经死了（quirk
    // events.subscription_expires_at_once 建完就是过期状态，正是拿这个做文章）。
    // A9 打开时不回收的是「到期后不清表」，不是「到期后还能用」。
    if (sub->expired()) {
        responder(exchange, nullptr, {});
        return;
    }

    messageLimit = messageLimit <= 0 ? 1 : qMin(messageLimit, kMaxMessageLimit);
    const bool alwaysEmpty = d->quirks().isEnabled(QuirkId::PullMessagesAlwaysEmpty);

    if (!alwaysEmpty && !sub->queue.isEmpty()) {
        responder(exchange, sub, d->take(sub, messageLimit));
        return;
    }

    timeoutMs = qBound(0, timeoutMs, kMaxPullTimeoutMs);
    if (timeoutMs == 0) {
        // Timeout=0 就是「有什么给什么」，空拉取本来就是正常心跳。
        responder(exchange, sub, {});
        return;
    }

    PendingPull *p = new PendingPull;
    p->subscriptionId = sub->id;
    p->exchange = exchange;
    p->responder = std::move(responder);
    p->messageLimit = messageLimit;

    p->timer = new QTimer(this);
    p->timer->setSingleShot(true);
    p->timer->setTimerType(Qt::CoarseTimer);
    connect(p->timer, &QTimer::timeout, this, [this, p]() {
        Subscription *s = find(p->subscriptionId);
        if (!s) {
            d->dropPending(p, true);
            return;
        }
        const bool empty = d->quirks().isEnabled(QuirkId::PullMessagesAlwaysEmpty);
        d->finishPending(p, s, empty ? QVector<EventMessage>() : d->take(s, p->messageLimit));
    });

    if (exchange) {
        // 客户端断开：连接都没了就别再渲染响应，直接把挂起项清掉。
        p->abortConn = connect(exchange.data(), &HttpExchange::aborted, this,
                               [this, p]() { d->dropPending(p, false); });
    }

    d->pending.append(p);
    p->timer->start(timeoutMs);
}

int SubscriptionManager::pendingPullCount() const
{
    return static_cast<int>(d->pending.size());
}

void SubscriptionManager::deliver(const EventMessage &message)
{
    // 快照迭代：wakePending 会调到服务层的 responder，responder 理论上能反手
    // 改订阅表，所以每一轮都重新确认这条订阅还在。
    const QList<Subscription *> snapshot = d->subs;
    for (Subscription *sub : snapshot) {
        if (!d->subs.contains(sub))
            continue;
        if (!sub->filter.matches(message.topic))
            continue;
        if (sub->maxQueue > 0 && sub->queue.size() >= sub->maxQueue) {
            // 满了丢最老的：客户端拉得慢就该丢掉旧事件，而不是把内存吃干净。
            sub->queue.dequeue();
            ++sub->droppedCount;
        }
        sub->queue.enqueue(message);
        d->wakePending(sub);
    }
}

void SubscriptionManager::reapExpired()
{
    // A9：参照客户端每次建连都建一条 PullPoint 订阅且从不 Unsubscribe。
    // 关掉回收就复现了真实高发的槽位泄漏 —— 症状是「加相机时好时坏」。
    if (d->quirks().isEnabled(QuirkId::SubscriptionNeverExpires))
        return;

    const QList<Subscription *> snapshot = d->subs;
    int removed = 0;
    for (Subscription *sub : snapshot) {
        if (!sub->expired())
            continue;
        d->destroySubscription(sub);
        ++removed;
    }
    if (removed > 0)
        emit countChanged(count());
}

quint16 SubscriptionManager::nextSubscriptionPort()
{
    if (!d->ownPortMode())
        return 0;   // 0 = 与主服务同端口

    const int configured = d->quirks().paramInt(QuirkId::SubscriptionPortIncrement,
                                                QStringLiteral("base_port"));
    const quint16 base = static_cast<quint16>(qBound(1, configured, 65535));
    if (d->nextPort < base)
        d->nextPort = base;

    const quint16 port = d->nextPort;
    d->nextPort = (d->nextPort >= 65535) ? base : static_cast<quint16>(d->nextPort + 1);
    return port;
}

} // namespace onvifsim
