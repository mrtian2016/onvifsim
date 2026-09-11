#include "soap/Namespaces.h"

#include <cstring>

namespace onvifsim {
namespace ns {
namespace {

struct ShortNameEntry {
    const char *uri;
    const char *shortName;
};

// 短名要与各 SoapService::serviceName() 一致：GetServices / GetCapabilities
// 回填 XAddr 时两边靠这个串对齐，写错就会把 Media2 的地址填到 Media 名下（A6 的坑）。
constexpr ShortNameEntry kShortNames[] = {
    { Device, "device" },
    { Media, "media" },
    { Media2, "media2" },
    { Ptz, "ptz" },
    { Imaging, "imaging" },
    { Events, "events" },
    { Analytics, "analytics" },
    { DeviceIo, "deviceio" },
    { Replay, "replay" },
    { Recording, "recording" },
    { Search, "search" },
};

} // namespace

const char *serviceShortName(const char *serviceNamespace)
{
    if (!serviceNamespace)
        return "";
    // 必须整串相等：Media(ver10) 与 Media2(ver20) 都含 "/media/"，按子串匹配会串档。
    for (const ShortNameEntry &e : kShortNames) {
        if (std::strcmp(e.uri, serviceNamespace) == 0)
            return e.shortName;
    }
    return "";
}

} // namespace ns
} // namespace onvifsim
