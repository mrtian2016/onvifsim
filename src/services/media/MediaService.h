#pragma once

// ONVIF Media 服务（ver10）。参照客户端的加相机流程几乎全落在这里：
// GetProfiles → 逐个 GetStreamUri → GetSnapshotUri，对讲还要走
// GetCompatibleAudioOutputConfigurations / AddAudioOutputConfiguration。
//
// 写操作必须真的改 CameraModel 的 profile 状态：客户端靠
// AddAudioOutputConfiguration / AddAudioDecoderConfiguration 把对讲绑上去，
// 如果这里只是「回个成功」，后续 backchannel 的行为就对不上了。

#include "services/ServiceBase.h"

namespace onvifsim {

class MediaService : public SoapService
{
public:
    MediaService();

    const char *serviceNamespace() const override;
    const char *serviceName() const override;
    QString defaultPath() const override;

    void writeServiceCapabilities(SoapContext &ctx) const override;

private:
    void registerProfileOps();
    void registerVideoSourceOps();
    void registerVideoEncoderOps();
    void registerAudioOps();
    void registerAudioOutputOps();
    void registerUriOps();
    void registerMiscReadOps();
    void registerMulticastOps();
};

} // namespace onvifsim
