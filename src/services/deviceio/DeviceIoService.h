#pragma once

// ONVIF DeviceIO 服务（ver10，tmd:）。
//
// 最小实现：参照客户端与 Frigate / Home Assistant 只用它探「这台机器有没有
// 音频输出 / 继电器 / 数字输入」，拿到的 token 会直接拿去跟事件的 Source 对。
// 所以 token 必须与 CameraModel 和事件层用的那一套完全一致。

#include "services/ServiceBase.h"

namespace onvifsim {

class DeviceIoService : public SoapService
{
public:
    DeviceIoService();

    const char *serviceNamespace() const override;
    const char *serviceName() const override;
    QString defaultPath() const override;
    void writeServiceCapabilities(SoapContext &ctx) const override;

private:
};

} // namespace onvifsim
