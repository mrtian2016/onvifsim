#pragma once

// ONVIF Device 服务（ver10，tds:）。
//
// 这是客户端建连的唯一入口：device_service 的路径被客户端硬编码，
// 参照客户端只调 GetCapabilities(Category=All) 拿各服务 XAddr，从不调 GetServices。
// 所以 GetCapabilities 写错一个 XAddr，整台相机就加不进去。
//
// 服务实例是每台相机一个（VirtualCamera::setupServices 里创建，dispatcher 持有），
// 因此那些 CameraModel 里没有字段、又必须「真的能改」的小状态
// （DiscoveryMode、继电器输出）直接存在本对象上。

#include "services/ServiceBase.h"

#include <QtCore/QHash>

namespace onvifsim {

class DeviceService : public SoapService
{
public:
    DeviceService();

    const char *serviceNamespace() const override;
    const char *serviceName() const override;
    QString defaultPath() const override;
    void writeServiceCapabilities(SoapContext &ctx) const override;

private:
    // 构造函数原来是 539 行的一串 op(...) 注册，按原有的注释横线拆成六段。
    void registerIdentityOps();
    void registerTimeOps();
    void registerCapabilityOps();
    void registerScopeOps();
    void registerNetworkOps();
    void registerUserOps();
    void registerSystemOps();

private:

    // Discoverable / NonDiscoverable。CameraModel 里没有对应字段，
    // 又必须被 SetDiscoveryMode 真的改到，所以挂在服务实例上。
    QString m_discoveryMode;
    // 继电器输出的当前逻辑状态，token → active。
    QHash<QString, bool> m_relayStates;
};

} // namespace onvifsim
