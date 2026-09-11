#pragma once

// 事件引擎：topic 集、触发调度、把消息投给所有匹配的订阅。
//
// 触发来源：GUI 按钮（带持续时长）、REST、定时随机、脚本计划、事件风暴。
// 属性型事件默认成对发 IsMotion=true / false（quirk D11 可切成只发 true）。

#include "events/EventTypes.h"

#include <QtCore/QObject>
#include <QtCore/QVector>

namespace onvifsim {

class VirtualCamera;

// 自动触发计划：每 intervalSec 触发一次，持续 durationSec。
struct EventSchedule {
    EventKind kind = EventKind::Motion;
    int intervalSec = 30;
    int durationSec = 5;
    bool randomJitter = false;
    bool enabled = false;
};

class EventEngine : public QObject
{
    Q_OBJECT
public:
    explicit EventEngine(VirtualCamera *camera, QObject *parent = nullptr);
    ~EventEngine() override;

    // 当前命名风格下的 topic 集（受 quirk D9 影响）。
    QVector<TopicDef> topics() const;
    void reloadTopics();

    // ---- 触发 ----
    // 瞬时事件：发一条。属性型：发 true，durationMs 后自动发 false。
    void trigger(EventKind kind, int durationMs = 0);
    void trigger(const QString &topic, bool state, int durationMs = 0);

    // 属性型 topic 的当前状态，SetSynchronizationPoint 要回发这些。
    QMap<QString, bool> propertyStates() const;
    QVector<EventMessage> synchronizationSnapshot() const;

    // ---- 自动触发 ----
    QVector<EventSchedule> schedules() const;
    void setSchedules(const QVector<EventSchedule> &schedules);

    // quirk EventStorm：每秒 N 条。
    void startStorm(int perSecond);
    void stopStorm();
    // 按 quirk EventStorm 的当前状态同步风暴。只收回自己开的那一份 ——
    // GUI / REST 手动起的风暴不该被一次 quirk 刷新顺手停掉。
    void syncStorm();
    bool isStorming() const;

    QDateTime lastEventTime() const;
    QString lastEventTopic() const;
    qint64 totalEventsSent() const;

signals:
    // 订阅管理器接这个信号往各订阅队列里塞。
    void eventProduced(const onvifsim::EventMessage &message);
    void topicsChanged();
    void statisticsChanged();
    // 事件计划表被改过（REST 也能改）。界面必须跟着回填，否则用户看到的是
    // 切相机那一刻的旧值，一按「应用」就把过时的界面值倒灌回引擎。
    void schedulesChanged();

private:
    struct Private;
    Private *d;
};

} // namespace onvifsim
