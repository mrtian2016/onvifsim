#include "services/imaging/ImagingService.h"

#include "core/VirtualCamera.h"
#include "imaging/ImagingState.h"
#include "soap/Namespaces.h"
#include "soap/XmlNode.h"

namespace onvifsim {
namespace {

// 参数量程。ONVIF 没规定绝对刻度，各家都在 0..100 或 0..255 之间选，
// 这里统一 0..100，与 ImagingState 的默认值（50）对得上。
constexpr double kLevelMin = 0.0;
constexpr double kLevelMax = 100.0;

// 聚焦位置量程。GetMoveOptions / GetStatus 都按这个报。
constexpr double kFocusMin = 0.0;
constexpr double kFocusMax = 1.0;

// VideoSourceToken 来自 Media 的 GetVideoSources，也就是 profile 里的 sourceToken。
// 客户端拿到的永远是 video_sources[0].token，所以「第一个 profile 的源」就是默认值。
QString defaultSourceToken(const CameraModel &model)
{
    if (model.profiles.isEmpty())
        return QStringLiteral("VideoSource_1");
    return model.profiles.first().videoSource.sourceToken;
}

// 校验 VideoSourceToken。缺省时按第一个源处理 —— 真机普遍如此，
// 而且客户端偶尔会漏发这个参数，为此回 Fault 只会平白制造一类假故障。
bool requireVideoSource(SoapContext &ctx)
{
    if (!ctx.camera)
        return false;
    const QString token = ctx.arg(QStringLiteral("VideoSourceToken"));
    if (token.isEmpty())
        return true;
    const CameraModel &model = ctx.camera->model();
    if (token == defaultSourceToken(model))
        return true;
    for (const MediaProfile &p : model.profiles) {
        if (p.videoSource.sourceToken == token)
            return true;
    }
    ctx.fault(QString::fromLatin1(ter::NoVideoSource),
              QStringLiteral("No video source with token %1").arg(token));
    return false;
}

void writeModeLevel(XmlWriter &w, const QString &qname, const QString &mode, double level)
{
    XmlWriter::Scope s(w, qname);
    w.element(QStringLiteral("tt:Mode"), mode);
    if (mode != QLatin1String("OFF"))
        w.element(QStringLiteral("tt:Level"), level);
}

// tt:ImagingSettings20 的子元素顺序是 schema 定死的 sequence，不能重排。
void writeSettings(SoapContext &ctx, XmlWriter &w, const QString &qname)
{
    const ImagingSettings &s = ctx.camera->imaging()->settings();
    const ImagingCapabilities &caps = ctx.camera->model().imaging;

    XmlWriter::Scope scope(w, qname);
    writeModeLevel(w, QStringLiteral("tt:BacklightCompensation"), s.backlightCompensation, 0.0);
    if (caps.brightness)
        w.element(QStringLiteral("tt:Brightness"), s.brightness);
    if (caps.saturation)
        w.element(QStringLiteral("tt:ColorSaturation"), s.colorSaturation);
    if (caps.contrast)
        w.element(QStringLiteral("tt:Contrast"), s.contrast);
    {
        XmlWriter::Scope exposure(w, QStringLiteral("tt:Exposure"));
        w.element(QStringLiteral("tt:Mode"), s.exposureMode);
        w.element(QStringLiteral("tt:Priority"), QStringLiteral("FrameRate"));
    }
    if (caps.focus) {
        XmlWriter::Scope focus(w, QStringLiteral("tt:Focus"));
        w.element(QStringLiteral("tt:AutoFocusMode"), s.focusMode);
    }
    if (caps.irCutFilter) {
        // 客户端唯一真正会读的字段：ON = 白天，OFF = 夜视，AUTO = 自动。
        w.element(QStringLiteral("tt:IrCutFilter"), ImagingState::irCutFilterName(s.irCutFilter));
    }
    if (caps.sharpness)
        w.element(QStringLiteral("tt:Sharpness"), s.sharpness);
    writeModeLevel(w, QStringLiteral("tt:WideDynamicRange"), s.wideDynamicRange, 0.0);
    {
        XmlWriter::Scope wb(w, QStringLiteral("tt:WhiteBalance"));
        w.element(QStringLiteral("tt:Mode"), QStringLiteral("AUTO"));
    }
}

void writeOptions(SoapContext &ctx, XmlWriter &w, const QString &qname)
{
    const ImagingCapabilities &caps = ctx.camera->model().imaging;

    XmlWriter::Scope scope(w, qname);
    {
        XmlWriter::Scope blc(w, QStringLiteral("tt:BacklightCompensation"));
        w.element(QStringLiteral("tt:Mode"), QStringLiteral("OFF"));
        w.element(QStringLiteral("tt:Mode"), QStringLiteral("ON"));
        w.floatRange(QStringLiteral("tt:Level"), kLevelMin, kLevelMax);
    }
    if (caps.brightness)
        w.floatRange(QStringLiteral("tt:Brightness"), kLevelMin, kLevelMax);
    if (caps.saturation)
        w.floatRange(QStringLiteral("tt:ColorSaturation"), kLevelMin, kLevelMax);
    if (caps.contrast)
        w.floatRange(QStringLiteral("tt:Contrast"), kLevelMin, kLevelMax);
    {
        XmlWriter::Scope exposure(w, QStringLiteral("tt:Exposure"));
        w.element(QStringLiteral("tt:Mode"), QStringLiteral("AUTO"));
        w.element(QStringLiteral("tt:Mode"), QStringLiteral("MANUAL"));
        w.element(QStringLiteral("tt:Priority"), QStringLiteral("LowNoise"));
        w.element(QStringLiteral("tt:Priority"), QStringLiteral("FrameRate"));
        w.floatRange(QStringLiteral("tt:MinExposureTime"), 10.0, 40000.0);
        w.floatRange(QStringLiteral("tt:MaxExposureTime"), 10.0, 40000.0);
        w.floatRange(QStringLiteral("tt:MinGain"), 0.0, 100.0);
        w.floatRange(QStringLiteral("tt:MaxGain"), 0.0, 100.0);
    }
    if (caps.focus) {
        XmlWriter::Scope focus(w, QStringLiteral("tt:Focus"));
        w.element(QStringLiteral("tt:AutoFocusModes"), QStringLiteral("AUTO"));
        w.element(QStringLiteral("tt:AutoFocusModes"), QStringLiteral("MANUAL"));
        w.floatRange(QStringLiteral("tt:DefaultSpeed"), 0.0, 1.0);
        w.floatRange(QStringLiteral("tt:NearLimit"), kFocusMin, kFocusMax);
        w.floatRange(QStringLiteral("tt:FarLimit"), kFocusMin, kFocusMax);
    }
    if (caps.irCutFilter) {
        w.element(QStringLiteral("tt:IrCutFilterModes"), QStringLiteral("ON"));
        w.element(QStringLiteral("tt:IrCutFilterModes"), QStringLiteral("OFF"));
        w.element(QStringLiteral("tt:IrCutFilterModes"), QStringLiteral("AUTO"));
    }
    if (caps.sharpness)
        w.floatRange(QStringLiteral("tt:Sharpness"), kLevelMin, kLevelMax);
    {
        XmlWriter::Scope wdr(w, QStringLiteral("tt:WideDynamicRange"));
        w.element(QStringLiteral("tt:Mode"), QStringLiteral("OFF"));
        w.element(QStringLiteral("tt:Mode"), QStringLiteral("ON"));
        w.floatRange(QStringLiteral("tt:Level"), kLevelMin, kLevelMax);
    }
    {
        XmlWriter::Scope wb(w, QStringLiteral("tt:WhiteBalance"));
        w.element(QStringLiteral("tt:Mode"), QStringLiteral("AUTO"));
        w.element(QStringLiteral("tt:Mode"), QStringLiteral("MANUAL"));
        w.floatRange(QStringLiteral("tt:YrGain"), 0.0, 255.0);
        w.floatRange(QStringLiteral("tt:YbGain"), 0.0, 255.0);
    }
}

// 把请求里的 ImagingSettings 合并进当前状态：只覆盖真的带了的字段。
// 客户端普遍只发想改的那一个（比如单独一条 IrCutFilter），整份覆盖会把其余参数清零。
void applySettings(SoapContext &ctx, const XmlNode *node)
{
    if (!node)
        return;
    ImagingState *state = ctx.camera->imaging();
    ImagingSettings s = state->settings();

    if (node->hasChild(QStringLiteral("Brightness")))
        s.brightness = node->childDouble(QStringLiteral("Brightness"), s.brightness);
    if (node->hasChild(QStringLiteral("ColorSaturation")))
        s.colorSaturation = node->childDouble(QStringLiteral("ColorSaturation"), s.colorSaturation);
    if (node->hasChild(QStringLiteral("Contrast")))
        s.contrast = node->childDouble(QStringLiteral("Contrast"), s.contrast);
    if (node->hasChild(QStringLiteral("Sharpness")))
        s.sharpness = node->childDouble(QStringLiteral("Sharpness"), s.sharpness);
    if (node->hasChild(QStringLiteral("IrCutFilter"))) {
        bool ok = false;
        const IrCutFilterMode mode = ImagingState::irCutFilterFromName(
            node->childText(QStringLiteral("IrCutFilter")), &ok);
        if (ok)
            s.irCutFilter = mode;
    }
    if (const XmlNode *exposure = node->child(QStringLiteral("Exposure"))) {
        if (exposure->hasChild(QStringLiteral("Mode")))
            s.exposureMode = exposure->childText(QStringLiteral("Mode"), s.exposureMode);
    }
    if (const XmlNode *focus = node->child(QStringLiteral("Focus"))) {
        if (focus->hasChild(QStringLiteral("AutoFocusMode")))
            s.focusMode = focus->childText(QStringLiteral("AutoFocusMode"), s.focusMode);
    }
    if (const XmlNode *wdr = node->child(QStringLiteral("WideDynamicRange"))) {
        if (wdr->hasChild(QStringLiteral("Mode")))
            s.wideDynamicRange = wdr->childText(QStringLiteral("Mode"), s.wideDynamicRange);
    }
    if (const XmlNode *blc = node->child(QStringLiteral("BacklightCompensation"))) {
        if (blc->hasChild(QStringLiteral("Mode"))) {
            s.backlightCompensation =
                blc->childText(QStringLiteral("Mode"), s.backlightCompensation);
        }
    }

    // 一次性写回：ImagingState 内部会做 clamp 并发 settingsChanged，
    // 厂商私有接口和 GUI 都挂在那个信号上。
    state->setSettings(s);
}

} // namespace

ImagingService::ImagingService()
{
    op("GetServiceCapabilities", AuthLevel::PreAuth, [this](SoapContext &ctx) {
        XmlWriter::Scope s(*ctx.out, QStringLiteral("timg:GetServiceCapabilitiesResponse"));
        writeServiceCapabilities(ctx);
    });

    op("GetImagingSettings", AuthLevel::User, [](SoapContext &ctx) {
        if (!requireVideoSource(ctx))
            return;
        XmlWriter::Scope s(*ctx.out, QStringLiteral("timg:GetImagingSettingsResponse"));
        writeSettings(ctx, *ctx.out, QStringLiteral("timg:ImagingSettings"));
    });

    op("SetImagingSettings", AuthLevel::Operator, [](SoapContext &ctx) {
        if (!requireVideoSource(ctx))
            return;
        applySettings(ctx, ctx.argNode(QStringLiteral("ImagingSettings")));
        ctx.out->emptyElement(QStringLiteral("timg:SetImagingSettingsResponse"));
    });

    op("GetOptions", AuthLevel::User, [](SoapContext &ctx) {
        if (!requireVideoSource(ctx))
            return;
        XmlWriter::Scope s(*ctx.out, QStringLiteral("timg:GetOptionsResponse"));
        writeOptions(ctx, *ctx.out, QStringLiteral("timg:ImagingOptions"));
    });

    op("GetMoveOptions", AuthLevel::User, [](SoapContext &ctx) {
        if (!requireVideoSource(ctx))
            return;
        XmlWriter &w = *ctx.out;
        XmlWriter::Scope s(w, QStringLiteral("timg:GetMoveOptionsResponse"));
        XmlWriter::Scope options(w, QStringLiteral("timg:MoveOptions"));
        {
            XmlWriter::Scope absolute(w, QStringLiteral("tt:Absolute"));
            w.floatRange(QStringLiteral("tt:Position"), kFocusMin, kFocusMax);
            w.floatRange(QStringLiteral("tt:Speed"), 0.0, 1.0);
        }
        {
            XmlWriter::Scope relative(w, QStringLiteral("tt:Relative"));
            w.floatRange(QStringLiteral("tt:Distance"), -1.0, 1.0);
            w.floatRange(QStringLiteral("tt:Speed"), 0.0, 1.0);
        }
        {
            XmlWriter::Scope continuous(w, QStringLiteral("tt:Continuous"));
            w.floatRange(QStringLiteral("tt:Speed"), -1.0, 1.0);
        }
    });

    op("Move", AuthLevel::Operator, [](SoapContext &ctx) {
        if (!requireVideoSource(ctx))
            return;
        const XmlNode *focus = ctx.argNode(QStringLiteral("Focus"));
        if (!focus) {
            ctx.fault(QString::fromLatin1(ter::InvalidArgVal),
                      QStringLiteral("Missing Focus move descriptor"));
            return;
        }

        // 三种移动方式换算成同一件事：往哪个方向走多远、多快。
        // ImagingState 只有一个一维聚焦轴，绝对定位换成「到目标的差值」。
        double distance = 0.0;
        double speed = 0.5;
        if (const XmlNode *absolute = focus->child(QStringLiteral("Absolute"))) {
            const double target = absolute->childDouble(QStringLiteral("Position"), 0.0);
            distance = target - ctx.camera->imaging()->focusPosition();
            speed = absolute->childDouble(QStringLiteral("Speed"), speed);
        } else if (const XmlNode *relative = focus->child(QStringLiteral("Relative"))) {
            distance = relative->childDouble(QStringLiteral("Distance"), 0.0);
            speed = relative->childDouble(QStringLiteral("Speed"), speed);
        } else if (const XmlNode *continuous = focus->child(QStringLiteral("Continuous"))) {
            // 连续聚焦没有终点：ImagingState 约定 distance == 0 且 speed != 0 就是
            // 「按 speed 的符号一直走，直到 stopFocus() 或撞限位」。
            distance = 0.0;
            speed = continuous->childDouble(QStringLiteral("Speed"), 0.0);
        } else {
            ctx.fault(QString::fromLatin1(ter::InvalidArgVal),
                      QStringLiteral("Focus carries no Absolute/Relative/Continuous move"));
            return;
        }

        ctx.camera->imaging()->moveFocus(distance, speed);
        ctx.out->emptyElement(QStringLiteral("timg:MoveResponse"));
    });

    op("Stop", AuthLevel::Operator, [](SoapContext &ctx) {
        if (!requireVideoSource(ctx))
            return;
        ctx.camera->imaging()->stopFocus();
        ctx.out->emptyElement(QStringLiteral("timg:StopResponse"));
    });

    op("GetStatus", AuthLevel::User, [](SoapContext &ctx) {
        if (!requireVideoSource(ctx))
            return;
        ImagingState *state = ctx.camera->imaging();
        XmlWriter &w = *ctx.out;
        XmlWriter::Scope s(w, QStringLiteral("timg:GetStatusResponse"));
        XmlWriter::Scope status(w, QStringLiteral("timg:Status"));
        XmlWriter::Scope focus(w, QStringLiteral("tt:FocusStatus20"));
        w.element(QStringLiteral("tt:Position"), state->focusPosition());
        w.element(QStringLiteral("tt:MoveStatus"),
                  state->isFocusMoving() ? QStringLiteral("MOVING") : QStringLiteral("IDLE"));
    });
}

const char *ImagingService::serviceNamespace() const
{
    return ns::Imaging;
}

const char *ImagingService::serviceName() const
{
    return "imaging";
}

QString ImagingService::defaultPath() const
{
    return QStringLiteral("/onvif/imaging_service");
}

void ImagingService::writeServiceCapabilities(SoapContext &ctx) const
{
    XmlWriter &w = *ctx.out;
    const bool presets = ctx.camera && ctx.camera->model().imaging.focus;
    w.start(QStringLiteral("timg:Capabilities"));
    w.attr(QStringLiteral("ImageStabilization"), QStringLiteral("false"));
    // Presets 指的是聚焦预置位，没有聚焦马达就谈不上。
    w.attr(QStringLiteral("Presets"), presets ? QStringLiteral("true") : QStringLiteral("false"));
    w.end();
}

namespace services {

SoapService *createImaging()
{
    return new ImagingService;
}

} // namespace services
} // namespace onvifsim
