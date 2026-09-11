#pragma once

// PullPoint 订阅管理器的 HTTP 端点。
//
// 订阅地址是一条独立路径（D1 打开时还在独立端口上），客户端把 PullMessages /
// Renew / Unsubscribe / SetSynchronizationPoint 都发到那里。这些请求不能走
// SoapDispatcher —— Renew 与 Unsubscribe 的 Body 在 wsnt 命名空间下，
// 按命名空间分发根本落不到 Events 服务上。所以这里自己解信封、自己鉴权、
// 再调 EventsService.h 导出的那几个实现，保证两条路径吐出来的 XML 一致。

#include <QtCore/QtGlobal>

namespace onvifsim {

class HttpServer;
class VirtualCamera;

namespace services {

// 把订阅路径的前缀路由挂到 server 上。
// 可以对同一台相机的多个 HttpServer 重复调用（D1 的每个独立端口一个），
// 同一个 server 重复挂不会叠加路由。
void registerSubscriptionRoutes(VirtualCamera *camera, HttpServer *server);

} // namespace services
} // namespace onvifsim
