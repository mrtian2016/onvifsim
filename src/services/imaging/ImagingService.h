#pragma once

// Imaging 服务（ver20）。状态全在 ImagingState 上，与厂商私有接口共享同一份数据
//（Reolink 的 GetWhiteLed、海康的 ircutFilter 改的就是它）。
//
// 参照客户端实际只读 IrCutFilter，但响应仍然按 WSDL 写全：
// onvif-zeep 会严格按 schema 解析，缺字段直接抛异常，那就不是「像真机」而是「坏掉」。

#include "services/ServiceBase.h"

namespace onvifsim {

class ImagingService final : public SoapService
{
public:
    ImagingService();

    const char *serviceNamespace() const override;
    const char *serviceName() const override;
    QString defaultPath() const override;
    void writeServiceCapabilities(SoapContext &ctx) const override;
};

} // namespace onvifsim
