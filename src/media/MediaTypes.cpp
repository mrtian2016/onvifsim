#include "media/MediaTypes.h"

namespace onvifsim {
namespace codec {
namespace {

// 一个 codec 在不同层里有好几个名字：ONVIF 的 AudioEncoderConfiguration 写 "G711"、
// SDP 的 rtpmap 写 "PCMU"、Media2 的 AudioEncoding 又写 "PCMU"。别名表统一收口，
// 免得每层各写一遍 if-else 还漏掉某个拼法（真机上 "G.711"、"G711U"、"ulaw" 都见过）。
struct AliasEntry {
    const char *name;
    AudioCodec codec;
};

const AliasEntry kAudioAliases[] = {
    { "none", AudioCodec::None },
    { "off", AudioCodec::None },
    { "pcmu", AudioCodec::PCMU },
    { "g711", AudioCodec::PCMU },       // ONVIF ver10 的枚举只有 "G711"，业界默认指 μ-law
    { "g711u", AudioCodec::PCMU },
    { "g.711", AudioCodec::PCMU },
    { "ulaw", AudioCodec::PCMU },
    { "mulaw", AudioCodec::PCMU },
    { "u-law", AudioCodec::PCMU },
    { "pcma", AudioCodec::PCMA },
    { "g711a", AudioCodec::PCMA },
    { "alaw", AudioCodec::PCMA },
    { "a-law", AudioCodec::PCMA },
    { "g722", AudioCodec::G722 },
    { "g.722", AudioCodec::G722 },
    { "aac", AudioCodec::AAC },
    { "aac-lc", AudioCodec::AAC },
    { "mpeg4-generic", AudioCodec::AAC },
    { "mp4a-latm", AudioCodec::AAC },
};

} // namespace

AudioCodec audioFromName(const QString &name, bool *ok)
{
    const QString key = name.trimmed().toLower();
    if (key.isEmpty()) {
        if (ok)
            *ok = true;
        return AudioCodec::None;
    }
    for (const AliasEntry &entry : kAudioAliases) {
        if (key == QLatin1String(entry.name)) {
            if (ok)
                *ok = true;
            return entry.codec;
        }
    }
    // 认不出来一律当没有音频，但把 ok 置假：调用方要能区分「明确关掉」和「写错了」。
    if (ok)
        *ok = false;
    return AudioCodec::None;
}

QString audioCodecName(AudioCodec codec)
{
    // 返回 RTP/SDP 侧的写法。ONVIF 侧的 "G711" 与 SDP 侧 AAC 的 "MPEG4-GENERIC"
    // 由各自的服务/SDP 生成器再映射一次，这里不掺和。
    switch (codec) {
    case AudioCodec::None:
        return QStringLiteral("None");
    case AudioCodec::PCMU:
        return QStringLiteral("PCMU");
    case AudioCodec::PCMA:
        return QStringLiteral("PCMA");
    case AudioCodec::G722:
        return QStringLiteral("G722");
    case AudioCodec::AAC:
        return QStringLiteral("AAC");
    }
    return QStringLiteral("None");
}

int audioClockRate(AudioCodec codec)
{
    switch (codec) {
    case AudioCodec::None:
        return 0;
    case AudioCodec::PCMU:
    case AudioCodec::PCMA:
        return 8000;
    case AudioCodec::G722:
        // RFC 3551 §4.5.2 的历史遗留：G.722 实际采 16 kHz，但 RTP 时钟率必须写 8000。
        // 写成 16000 会让相当一部分客户端把音频放成两倍速，是真机上高发的互操作坑。
        return 8000;
    case AudioCodec::AAC:
        return 16000;
    }
    return 0;
}

int audioSampleRate(AudioCodec codec)
{
    switch (codec) {
    case AudioCodec::None:
        return 0;
    case AudioCodec::PCMU:
    case AudioCodec::PCMA:
        return 8000;
    case AudioCodec::G722:
        return 16000;   // 真实采样率，和上面的时钟率故意不一样
    case AudioCodec::AAC:
        return 16000;
    }
    return 0;
}

int audioPayloadType(AudioCodec codec)
{
    switch (codec) {
    case AudioCodec::None:
        return -1;
    case AudioCodec::PCMU:
        return 0;
    case AudioCodec::PCMA:
        return 8;
    case AudioCodec::G722:
        return 9;
    case AudioCodec::AAC:
        return 97;      // 动态区；96 让给 H.264
    }
    return -1;
}

QString videoCodecName(VideoCodec codec)
{
    switch (codec) {
    case VideoCodec::H264:
        return QStringLiteral("H264");
    case VideoCodec::H265:
        return QStringLiteral("H265");
    case VideoCodec::Jpeg:
        return QStringLiteral("JPEG");
    }
    return QStringLiteral("H264");
}

} // namespace codec
} // namespace onvifsim
