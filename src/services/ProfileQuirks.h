#pragma once

// Media / Media2 / PTZ 三个服务共用的 profile 相关判定与常量。
//
// 这里的东西原本在三个 .cpp 里各写了一份，而它们**必须**保持一致：
// 客户端判定「这台相机有没有 PTZ」只看 Media 响应里的 profile，真要转的时候
// 又是拿同一个 token 去调 PTZ 服务。两边对「哪个 profile 挂了 PTZConfiguration」
// 的理解一旦不同，相机就会对外说「Profile_3 没有 PTZ」却又老老实实接受
// Profile_3 的 ContinuousMove —— 自相矛盾的 quirk 比没有 quirk 更糟。

#include "core/CameraModel.h"
#include "core/Quirks.h"

#include <QtCore/QString>

namespace onvifsim {

namespace profilequirks {

// ONVIF 标准 PTZ 坐标空间 URI。客户端只按 XRange / YRange 判能力、不看这些串，
// 但 onvif-zeep 会按 WSDL 校验元素必填，一个都不能省。
//
// 原来 PtzService 和 MediaService 各写了一套（同样 8 条 URI、不同的变量名）。
// 写岔一个字符就是难查的互通性 bug：PTZConfiguration 里声明的空间和 PTZ 服务
// 实际接受的空间对不上，而客户端那边只表现为「命令发出去没反应」。
namespace ptzspace {
inline constexpr const char *kAbsolutePanTilt =
    "http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace";
inline constexpr const char *kAbsoluteZoom =
    "http://www.onvif.org/ver10/tptz/ZoomSpaces/PositionGenericSpace";
inline constexpr const char *kRelativePanTilt =
    "http://www.onvif.org/ver10/tptz/PanTiltSpaces/TranslationGenericSpace";
inline constexpr const char *kRelativeZoom =
    "http://www.onvif.org/ver10/tptz/ZoomSpaces/TranslationGenericSpace";
inline constexpr const char *kContinuousPanTilt =
    "http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace";
inline constexpr const char *kContinuousZoom =
    "http://www.onvif.org/ver10/tptz/ZoomSpaces/VelocityGenericSpace";
inline constexpr const char *kPanTiltSpeed =
    "http://www.onvif.org/ver10/tptz/PanTiltSpaces/GenericSpeedSpace";
inline constexpr const char *kZoomSpeed =
    "http://www.onvif.org/ver10/tptz/ZoomSpaces/ZoomGenericSpeedSpace";
} // namespace ptzspace

// 出厂 profile 数量的上限。
constexpr int kMaxProfiles = 8;

// C1（PtzConfigOnSubOnly）下，只有这个下标的 profile 还挂着 PTZConfiguration。
// 出厂固定三档：0=主码流、1=子码流、2=第三码流。quirk 表里写的是「只挂子码流」，
// 所以是 1，不是「除主码流外都挂」。
constexpr int kSubStreamIndex = 1;

// profile 对外**宣称**挂了 PTZConfiguration 吗（Media / Media2 写响应用）。
bool advertisesPtz(const MediaProfile &profile, const Quirks &quirks, int index);

// PTZ 服务**接受**这个 profile 的控制指令吗（PtzService 校验 token 用）。
//
// 与 advertisesPtz 的差别只在 C2：C2 的定义就是「能转但不宣称」，
// 所以宣称那边返回 false、接受这边返回 true。这不是漂移，是这条 quirk 的本意。
bool acceptsPtz(const Quirks &quirks, const CameraModel &model, int index);

// quirk ResolutionMismatch：对外声明的分辨率比实际码流高一档。
// 客户端不读 VideoEncoderConfiguration、全靠 ffprobe 探流，这条就是用来看它信谁的。
void declaredResolution(const VideoEncoderConfig &config, const Quirks &quirks,
                        int *width, int *height);

// ONVIF 的 xs:duration。会话超时之类都用这个形态。
QString ptSeconds(int seconds);

} // namespace profilequirks
} // namespace onvifsim
