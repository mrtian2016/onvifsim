#pragma once

// SDP 生成。各变体由 quirk 驱动：
//   E11 不写 rtpmap，只给静态 payload type
//   E12 只有 session 级 a=control
//   E10 声明 G7221 / G726 这类不规范 codec
//   E6  对讲双轨（麦克风 recvonly 在前 + 对讲 sendonly 在后）
//   E15 不带 Require 头也返回含 sendonly 的 SDP
//
// backchannel 走 ONVIF Streaming Spec 的做法：用 PLAY 不是 RECORD，
// 对讲轨是 a=sendonly（客户端 → 相机）。

#include "media/MediaTypes.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

namespace onvifsim {

class H264Source;
class Quirks;

struct SdpOptions {
    QString sessionName = QStringLiteral("onvifsim");
    QString originAddress = QStringLiteral("0.0.0.0");
    QString controlBase;             // "rtsp://host:port/path"

    // 视频轨
    bool hasVideo = true;
    VideoCodec videoCodec = VideoCodec::H264;
    int videoPayloadType = 96;
    QByteArray spropParameterSets;
    QByteArray profileLevelId;

    // 音频轨（相机 → 客户端）
    bool hasAudio = true;
    AudioCodec audioCodec = AudioCodec::PCMU;
    int audioPayloadType = 0;
    QByteArray aacConfig;

    // 对讲轨（客户端 → 相机）
    bool hasBackchannel = false;
    AudioCodec backchannelCodec = AudioCodec::PCMU;
    int backchannelPayloadType = 0;
    // E6：先来一条 recvonly 的麦克风轨，再来 sendonly 的对讲轨。
    bool dualTrackLayout = false;
};

namespace sdp {

QByteArray generate(const SdpOptions &options, const Quirks &quirks);

// 媒体级 a=control 的取值；session 级模式（E12）下媒体级不写。
QString trackControl(const SdpOptions &options, int trackIndex, const Quirks &quirks);

} // namespace sdp
} // namespace onvifsim
