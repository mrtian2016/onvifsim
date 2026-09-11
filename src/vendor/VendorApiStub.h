#pragma once

// 厂商私有 HTTP API 桩。按参照客户端实际会调的端点收敛，
// 返回静态 XML / JSON，并与事件引擎、灯光状态真联动
//（Reolink 的 GetWhiteLed 与海康的 supplementLight 改的是同一份 ImagingState）。
//
// 每个厂商一个子类，由 Persona::vendorApi 决定装哪个。

#include "core/Persona.h"
#include "net/HttpTypes.h"

#include <QtCore/QObject>

namespace onvifsim {

class HttpServer;
class VirtualCamera;

class VendorApiStub : public QObject
{
    Q_OBJECT
public:
    explicit VendorApiStub(VirtualCamera *camera, QObject *parent = nullptr);
    ~VendorApiStub() override;

    // 往相机的 HttpServer 上挂本厂商的路由。
    virtual void registerRoutes(HttpServer *server) = 0;
    virtual VendorApi kind() const = 0;

    VirtualCamera *camera() const;

    // 长连接事件推送（海康 alertStream、大华 eventManager attach）的活跃连接数。
    virtual int streamingClients() const { return 0; }

    static VendorApiStub *create(VendorApi kind, VirtualCamera *camera);

protected:
    VirtualCamera *m_camera = nullptr;
};

} // namespace onvifsim
