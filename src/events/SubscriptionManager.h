#pragma once

// PullPoint 订阅管理。
//
// 订阅地址既是后续 Pull / Renew / Unsubscribe 的 HTTP 目标，也是 wsa:To。
// 可以与主服务同端口，也可以开独立端口且每次递增（quirk D1，形如
// :1024/event-1024_1024 → :1025/…），host 还能报不可达地址（D2）。
//
// PullMessages 是真长轮询：无消息时把 HttpExchange 挂起到 Timeout 再回空，
// 用 QTimer 实现，绝不阻塞事件循环。

#include "events/EventTypes.h"
#include "net/HttpTypes.h"

#include <QtCore/QDateTime>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QQueue>
#include <QtCore/QString>

namespace onvifsim {

class HttpServer;
class VirtualCamera;

struct Subscription {
    QString id;                    // 内部 id
    QString address;               // 对外的完整订阅 URL
    QString path;                  // 该订阅在 HTTP 服务器上的路径
    quint16 port = 0;              // 独立端口模式下的端口，0 = 用主端口
    TopicFilter filter;
    QDateTime createdAt;
    QDateTime terminationTime;
    QQueue<EventMessage> queue;
    int maxQueue = 1000;
    qint64 pulledCount = 0;
    qint64 droppedCount = 0;
    QString creatorPeer;
    bool expired() const;
};

class SubscriptionManager : public QObject
{
    Q_OBJECT
public:
    explicit SubscriptionManager(VirtualCamera *camera, QObject *parent = nullptr);
    ~SubscriptionManager() override;

    // ---- 订阅生命周期 ----
    // 返回 nullptr 表示被 quirk D3 的槽位上限挡下。failureReason 是约定串，服务层据此决定表现：
    //   "slot_limit_fault"  → 回 SOAP Fault ter:MaxPullPoints
    //   "slot_limit_silent" → 照回一个成功的响应，但订阅其实没建（最难查的那种真机行为）
    // 注意 create() 内部已经调过 nextSubscriptionPort()，服务层不要再调一次。
    Subscription *create(const TopicFilter &filter, const QDateTime &terminationTime,
                         const QString &creatorPeer, QString *failureReason = nullptr);
    Subscription *find(const QString &idOrPath) const;
    bool renew(const QString &id, const QDateTime &newTerminationTime);
    bool unsubscribe(const QString &id);
    QList<Subscription *> subscriptions() const;
    int count() const;
    void clear();

    // ---- 长轮询 ----
    // 队列里有消息就立刻回，否则挂起 exchange 直到 timeoutMs 或有新消息。
    // responder 负责把消息渲染成 PullMessagesResponse。
    // responder 收到 sub == nullptr 表示订阅不存在或中途消失（被 Unsubscribe / 回收 / 挤掉），
    // 服务层应据此回 Fault，而不是把请求一直吊着。
    using PullResponder = std::function<void(const HttpExchangePtr &, Subscription *,
                                             const QVector<EventMessage> &)>;
    void pull(const QString &id, int messageLimit, int timeoutMs, const HttpExchangePtr &exchange,
              PullResponder responder);
    int pendingPullCount() const;

    // 事件引擎产出的消息投递到所有匹配订阅。
    void deliver(const EventMessage &message);

    // 过期回收。quirk A9 打开时这个函数什么都不做，复现槽位泄漏。
    void reapExpired();

    // 独立端口模式（D1）下，为下一条订阅分配端口与路径。
    quint16 nextSubscriptionPort();

signals:
    void countChanged(int count);
    void subscriptionCreated(const QString &id);
    void subscriptionRemoved(const QString &id);

private:
    struct Private;
    Private *d;
};

} // namespace onvifsim
