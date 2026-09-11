#pragma once

// PTZ 服务（ver20）。位置、预置位、Home 全部落在 PtzState 上，
// 这一层只负责 XML 与 quirk 的表现形态。
//
// 这里管四条 quirk 的「怎么写响应」：C3 SupportedPTZSpaces 四档、C4 Range 退化、
// C5 非零 Zoom 畸形响应、C8/C9 预置位接口形态。
// C6/C7 与「移动了但状态不变」在 PtzState 里，C10 的响应延迟抖动在
// soap::applyResponseQuirks 里，服务层都不用管。

#include "services/ServiceBase.h"

namespace onvifsim {

class PtzService final : public SoapService
{
public:
    PtzService();

    const char *serviceNamespace() const override;
    const char *serviceName() const override;
    QString defaultPath() const override;
    void writeServiceCapabilities(SoapContext &ctx) const override;

private:
    void registerCapabilityOps();
    void registerMotionOps();
    void registerPresetOps();
    void registerHomeOps();
    void registerAuxiliaryOps();

};

} // namespace onvifsim
