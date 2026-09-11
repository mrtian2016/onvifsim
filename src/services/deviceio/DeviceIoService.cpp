#include "services/deviceio/DeviceIoService.h"

#include "core/VirtualCamera.h"
#include "soap/Namespaces.h"

namespace onvifsim {
namespace {

// 与 Device 服务、events/EventTypes.cpp 里的 tns1:Device/Trigger/* 用同一串。
// 三处写岔了，客户端就把「继电器 token」和「继电器事件的 Source」当成两个东西。
const char *kRelayToken = iotoken::Relay;
const char *kDigitalInputToken = iotoken::DigitalInput;

// 音频输出 token：优先用 profile 上已经绑好的，没有就退回 CameraModel 的默认值，
// 免得在两处各写一遍字面量。
QStringList audioOutputTokens(const CameraModel &model)
{
    QStringList tokens;
    for (const MediaProfile &p : model.profiles) {
        if (p.hasAudioOutput && !tokens.contains(p.audioOutput.outputToken))
            tokens.append(p.audioOutput.outputToken);
    }
    if (tokens.isEmpty())
        tokens.append(AudioOutputConfig().outputToken);
    return tokens;
}

} // namespace

const char *DeviceIoService::serviceNamespace() const
{
    return ns::DeviceIo;
}

const char *DeviceIoService::serviceName() const
{
    return "deviceio";
}

QString DeviceIoService::defaultPath() const
{
    return QStringLiteral("/onvif/deviceio_service");
}

void DeviceIoService::writeServiceCapabilities(SoapContext &ctx) const
{
    // 相机为空只可能出现在单测里；用一份出厂默认模型兜底，别在这里 crash。
    static const CameraModel kFallbackModel;
    const CameraModel &model = ctx.camera ? ctx.camera->model() : kFallbackModel;
    XmlWriter &w = *ctx.out;
    XmlWriter::Scope caps(w, QStringLiteral("tmd:Capabilities"));
    w.attr(QStringLiteral("VideoSources"), QStringLiteral("1"));
    w.attr(QStringLiteral("VideoOutputs"), QStringLiteral("0"));
    w.attr(QStringLiteral("AudioSources"), QStringLiteral("1"));
    w.attr(QStringLiteral("AudioOutputs"),
           model.hasAudioBackchannel ? QStringLiteral("1") : QStringLiteral("0"));
    w.attr(QStringLiteral("RelayOutputs"),
           model.hasRelayOutputs ? QStringLiteral("1") : QStringLiteral("0"));
    w.attr(QStringLiteral("DigitalInputs"),
           model.hasDigitalInputs ? QStringLiteral("1") : QStringLiteral("0"));
    w.attr(QStringLiteral("SerialPorts"), QStringLiteral("0"));
    w.attr(QStringLiteral("DigitalInputOptions"), QStringLiteral("false"));
}

DeviceIoService::DeviceIoService()
{
    cameraOp("GetServiceCapabilities", AuthLevel::PreAuth, [this](SoapContext &ctx) {
        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tmd:GetServiceCapabilitiesResponse"));
        writeServiceCapabilities(ctx);
    });

    // DeviceIO 的 GetAudioOutputs 只回 token 列表（Media 那边才回完整配置）。
    cameraOp("GetAudioOutputs", AuthLevel::User, [](SoapContext &ctx) {
        const CameraModel &model = ctx.camera->model();
        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tmd:GetAudioOutputsResponse"));
        if (!model.hasAudioBackchannel)
            return;   // 没有对讲能力就回空列表，比报 Fault 更像真机
        for (const QString &token : audioOutputTokens(model))
            ctx.out->element(QStringLiteral("tmd:Token"), token);
    });

    cameraOp("GetRelayOutputs", AuthLevel::User, [](SoapContext &ctx) {
        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tmd:GetRelayOutputsResponse"));
        if (!ctx.camera->model().hasRelayOutputs)
            return;
        XmlWriter::Scope one(*ctx.out, QStringLiteral("tmd:RelayOutputs"));
        ctx.out->attr(QStringLiteral("token"), QString::fromLatin1(kRelayToken));
        XmlWriter::Scope props(*ctx.out, QStringLiteral("tt:Properties"));
        ctx.out->element(QStringLiteral("tt:Mode"), QStringLiteral("Bistable"));
        ctx.out->element(QStringLiteral("tt:DelayTime"), QStringLiteral("PT1S"));
        ctx.out->element(QStringLiteral("tt:IdleState"), QStringLiteral("closed"));
    });

    cameraOp("GetDigitalInputs", AuthLevel::User, [](SoapContext &ctx) {
        XmlWriter::Scope resp(*ctx.out, QStringLiteral("tmd:GetDigitalInputsResponse"));
        if (!ctx.camera->model().hasDigitalInputs)
            return;
        // tt:DigitalInput 只有属性没有子元素，token 与 IdleState 都走属性。
        XmlWriter::Scope one(*ctx.out, QStringLiteral("tmd:DigitalInputs"));
        ctx.out->attr(QStringLiteral("token"), QString::fromLatin1(kDigitalInputToken));
        ctx.out->attr(QStringLiteral("IdleState"), QStringLiteral("closed"));
    });
}

namespace services {

SoapService *createDeviceIo()
{
    return new DeviceIoService;
}

} // namespace services
} // namespace onvifsim
