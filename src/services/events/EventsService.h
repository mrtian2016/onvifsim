#pragma once

// Events 服务（ver10）+ PullPoint 订阅端口。
//
// 全项目最精细的一处是 PullMessages：它必须是真长轮询 —— 队列空时不能立刻回空，
// 而要把 HttpExchange 挂到 Timeout 再回。空拉取是正常心跳，不是错误。
// 实际的挂起逻辑在 SubscriptionManager 里，这一层只负责解析参数与渲染响应。
//
// 订阅相关的四个操作（PullMessages / Renew / Unsubscribe / SetSynchronizationPoint）
// 客户端是往「订阅地址」发的，那是一条独立 HTTP 路径、可能还在独立端口上，
// 不经过 SoapDispatcher（Renew / Unsubscribe 用的是 wsnt 命名空间，按 ns 分发根本
// 找不到本服务）。所以它们的实现放在这里以自由函数形式导出，由 SubscriptionEndpoint
// 复用，两条路径吐出来的 XML 保证一模一样。

#include "services/ServiceBase.h"

#include <QtCore/QDateTime>

namespace onvifsim {

class EventsService final : public SoapService
{
public:
    EventsService();

    const char *serviceNamespace() const override;
    const char *serviceName() const override;
    QString defaultPath() const override;
    void writeServiceCapabilities(SoapContext &ctx) const override;
};

namespace events {

// ISO 8601 duration（PT1H / PT8S / P1DT2H3M4S）→ 秒。解析失败返回 false。
//
// 事件订阅的 TerminationTime 和 PTZ 的 ContinuousMove Timeout 都用它 ——
// 曾经各写过一份逐字相同的实现，'T' 之前的 M 是月、之后是分钟这个坑
// 修一处漏一处，所以统一到这里。
bool parseIsoDuration(const QString &text, qint64 *secondsOut);

// InitialTerminationTime / TerminationTime：既可以是 duration 也可以是绝对 xs:dateTime。
QDateTime parseTerminationTime(const QString &text, const QDateTime &now, bool *ok);

// 订阅端口的四个操作。ctx 必须带 camera、soap、http、exchange 与 out。
void pullMessages(SoapContext &ctx);
void renew(SoapContext &ctx);
void unsubscribe(SoapContext &ctx);
void setSynchronizationPoint(SoapContext &ctx);

} // namespace events
} // namespace onvifsim
