#pragma once

// ONVIF Analytics 服务（ver20，tan:）。
//
// 最小实现：Frigate / Home Assistant 会拉规则列表来判断「这台机器支不支持
// 移动侦测 / 越界 / 区域入侵」。规则的 ParentTopic 必须与事件层实际推的 topic
// 对得上（含 quirk D9 的四套命名风格），否则客户端订阅了一个不存在的 topic，
// 界面上就是「有规则但永远不报警」。

#include "services/ServiceBase.h"

namespace onvifsim {

class AnalyticsService : public SoapService
{
public:
    AnalyticsService();

    const char *serviceNamespace() const override;
    const char *serviceName() const override;
    QString defaultPath() const override;
    void writeServiceCapabilities(SoapContext &ctx) const override;

private:
};

} // namespace onvifsim
