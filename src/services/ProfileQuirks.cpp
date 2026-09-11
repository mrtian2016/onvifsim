#include "services/ProfileQuirks.h"

namespace onvifsim {
namespace profilequirks {

bool advertisesPtz(const MediaProfile &profile, const Quirks &quirks, int index)
{
    if (quirks.isEnabled(QuirkId::PtzUsableButUnadvertised))
        return false;                        // C2：一个都不挂，但 PTZ 照常能转
    if (quirks.isEnabled(QuirkId::PtzConfigOnSubOnly))
        return index == kSubStreamIndex;     // C1：只有子码流挂
    return profile.hasPtz;
}

bool acceptsPtz(const Quirks &quirks, const CameraModel &model, int index)
{
    if (quirks.isEnabled(QuirkId::PtzUsableButUnadvertised))
        return true;                         // C2：没宣称，但照单全收
    if (quirks.isEnabled(QuirkId::PtzConfigOnSubOnly))
        return index == kSubStreamIndex;     // C1：主码流和第三码流都要吃 Fault
    return index >= 0 && index < model.profiles.size() && model.profiles.at(index).hasPtz;
}

void declaredResolution(const VideoEncoderConfig &config, const Quirks &quirks,
                        int *width, int *height)
{
    *width = config.width;
    *height = config.height;
    if (!quirks.isEnabled(QuirkId::ResolutionMismatch))
        return;
    if (config.width == 640 && config.height == 360) {
        *width = 1280;
        *height = 720;
    } else if (config.width == 1280 && config.height == 720) {
        *width = 1920;
        *height = 1080;
    } else if (config.width == 1920 && config.height == 1080) {
        *width = 2560;
        *height = 1440;
    } else {
        *width = config.width * 2;
        *height = config.height * 2;
    }
}

QString ptSeconds(int seconds)
{
    return QStringLiteral("PT%1S").arg(seconds);
}

} // namespace profilequirks
} // namespace onvifsim
