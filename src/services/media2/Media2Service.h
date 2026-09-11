#pragma once

// ONVIF Media2 服务（ver20）。Profile T 的客户端会先探这里，
// 探不到才回落 ver10 —— 所以「装不装」本身就是一条要复现的行为：
// 相机不装时 Dispatcher 会对 tr2 命名空间的请求自动回 ActionNotSupported。
//
// 装不装由 VirtualCamera 按 model().hasMedia2Service 与 persona.hasMedia2 决定，
// 这里只管实现；createMedia2() 任何时候都能构造。

#include "services/ServiceBase.h"

namespace onvifsim {

class Media2Service : public SoapService
{
public:
    Media2Service();

    const char *serviceNamespace() const override;
    const char *serviceName() const override;
    QString defaultPath() const override;

    void writeServiceCapabilities(SoapContext &ctx) const override;
};

} // namespace onvifsim
