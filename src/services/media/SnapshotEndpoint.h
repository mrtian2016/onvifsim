#pragma once

// HTTP 快照端点：GET <persona.snapshotPath>?token=<profileToken>
//
// 它不是 SOAP 操作，所以由 VirtualCamera 单独挂到 HttpServer 的路由上。
// 这里集中处理 B4（四种坏响应）、B5（四档鉴权）、B6（URI 轮换失效）三条 quirk，
// 图本身交给 media/Snapshot 画。

#include <QtCore/QString>

namespace onvifsim {

class HttpServer;
class VirtualCamera;

namespace services {

// 把快照路由挂到相机的 HTTP 服务器上。VirtualCamera::setupServices() 调用一次。
void registerSnapshotRoutes(VirtualCamera *camera, HttpServer *server);

// B5 的 token 档：GetSnapshotUri 要把一次性令牌拼进 URI，端点再按同一算法校验。
// 令牌按时间窗滚动，过期即 401 —— 客户端只能重新 GetSnapshotUri 才拿得到新的。
QString snapshotAccessToken(const VirtualCamera *camera, const QString &profileToken);

} // namespace services
} // namespace onvifsim
