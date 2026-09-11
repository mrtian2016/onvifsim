#include "rtsp/Sdp.h"

#include "core/Quirks.h"
#include "rtsp/RtspInternal_p.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

namespace onvifsim {
namespace {

// SDP 的 rtpmap 编码名是协议层写法，跟 media 层的 codec::audioCodecName 不完全重合：
// AAC 在 SDP 里叫 mpeg4-generic（RFC 3640），所以这一层单独映射一次。
QByteArray rtpmapName(AudioCodec codec)
{
    switch (codec) {
    case AudioCodec::None:
        return QByteArray();
    case AudioCodec::PCMU:
        return "PCMU";
    case AudioCodec::PCMA:
        return "PCMA";
    case AudioCodec::G722:
        return "G722";
    case AudioCodec::AAC:
        return "mpeg4-generic";
    }
    return QByteArray();
}

// E10 声明的不规范 codec 各有各的时钟率；名字里带 7221 的是 G.722.1（16 kHz），
// 其余（G726-32 之类）按 8 kHz 算。
int nonstandardClockRate(const QByteArray &name)
{
    return name.toUpper().contains("7221") ? 16000 : 8000;
}

// E10 用的 payload type 必须落在动态区：留在静态区（0/8/9）的话，
// 客户端按 RFC 3551 静态表一反查就又对上了 PCMU，这条 quirk 就白开了。
constexpr int kNonstandardPayloadType = 98;

bool sessionLevelControlOnly(const Quirks &quirks)
{
    return quirks.isEnabled(QuirkId::SdpSessionLevelControl);
}

// E11：只对静态 payload type（RFC 3551 的 0..95）省掉 rtpmap ——
// 动态 PT 没有 rtpmap 就完全无法解码，真机上不会这么干。
bool omitRtpmap(const Quirks &quirks, int payloadType)
{
    return quirks.isEnabled(QuirkId::SdpNoRtpmap) && payloadType < 96;
}

void appendMediaControl(QByteArray &out, const SdpOptions &options, int trackIndex,
                        const Quirks &quirks)
{
    const QString control = sdp::trackControl(options, trackIndex, quirks);
    if (!control.isEmpty())
        out += "a=control:" + control.toUtf8() + "\r\n";
}

void appendAudioTrack(QByteArray &out, const SdpOptions &options, AudioCodec codec,
                      int payloadType, bool sendonly, int trackIndex, const Quirks &quirks)
{
    QByteArray name = rtpmapName(codec);
    // 时钟率直接问 media 层：G.722 写 8000（RFC 3551 §4.5.2 的历史遗留，
    // 写 16000 会让一大批客户端放成两倍速）这条规则只能有一处定义。
    const int clockRate = sdp::effectiveAudioClockRate(codec, quirks);
    const int pt = sdp::effectiveAudioPayloadType(payloadType, quirks);

    if (quirks.isEnabled(QuirkId::SdpNonstandardCodec))
        name = quirks.paramString(QuirkId::SdpNonstandardCodec, QStringLiteral("name")).toUtf8();

    out += "m=audio 0 RTP/AVP " + QByteArray::number(pt) + "\r\n";
    if (!omitRtpmap(quirks, pt) && !name.isEmpty()) {
        out += "a=rtpmap:" + QByteArray::number(pt) + ' ' + name
            + '/' + QByteArray::number(clockRate) + "/1\r\n";
    }
    // AAC 走 RFC 3640 的 mpeg4-generic：没有这条 fmtp，客户端拆不出 AU 头。
    if (codec == AudioCodec::AAC && !quirks.isEnabled(QuirkId::SdpNonstandardCodec)) {
        out += "a=fmtp:" + QByteArray::number(pt)
            + " streamtype=5;profile-level-id=1;mode=AAC-hbr;"
              "sizelength=13;indexlength=3;indexdeltalength=3";
        if (!options.aacConfig.isEmpty())
            out += ";config=" + options.aacConfig;
        out += "\r\n";
    }
    appendMediaControl(out, options, trackIndex, quirks);
    // 方向属性按 ONVIF Streaming Spec 从客户端视角写：相机 → 客户端的轨是 recvonly，
    // 对讲轨（客户端 → 相机）是 sendonly，客户端就是靠 sendonly 挑对讲轨的。
    out += sendonly ? "a=sendonly\r\n" : "a=recvonly\r\n";
}

void appendVideoTrack(QByteArray &out, const SdpOptions &options, int trackIndex,
                      const Quirks &quirks)
{
    const int pt = options.videoPayloadType;
    out += "m=video 0 RTP/AVP " + QByteArray::number(pt) + "\r\n";
    // 视频用的是动态 PT，E11 不适用（见 omitRtpmap 的说明）。
    out += "a=rtpmap:" + QByteArray::number(pt) + ' '
        + (options.videoCodec == VideoCodec::H265 ? QByteArray("H265") : QByteArray("H264"))
        + "/90000\r\n";

    QByteArray fmtp = "a=fmtp:" + QByteArray::number(pt) + " packetization-mode=1";
    if (!options.profileLevelId.isEmpty())
        fmtp += ";profile-level-id=" + options.profileLevelId;
    // SpsPpsPlacement=inband_only：SDP 里不给 sprop-parameter-sets，
    // 只认 SDP 参数集的解码器就会一直黑屏 —— 这条 quirk 要的就是这个效果。
    const QString placement = quirks.isEnabled(QuirkId::SpsPpsPlacement)
        ? quirks.choice(QuirkId::SpsPpsPlacement, QStringLiteral("both"))
        : QStringLiteral("both");
    if (!options.spropParameterSets.isEmpty() && placement != QLatin1String("inband_only"))
        fmtp += ";sprop-parameter-sets=" + options.spropParameterSets;
    out += fmtp + "\r\n";

    appendMediaControl(out, options, trackIndex, quirks);
    out += "a=recvonly\r\n";
}

} // namespace

int sdp::effectiveAudioClockRate(AudioCodec codec, const Quirks &quirks)
{
    if (quirks.isEnabled(QuirkId::SdpNonstandardCodec)) {
        const QByteArray name =
            quirks.paramString(QuirkId::SdpNonstandardCodec, QStringLiteral("name")).toUtf8();
        return nonstandardClockRate(name);
    }
    // 时钟率直接问 media 层：G.722 写 8000（RFC 3551 §4.5.2 的历史遗留，
    // 写 16000 会让一大批客户端放成两倍速）这条规则只能有一处定义。
    return codec::audioClockRate(codec);
}

int sdp::effectiveAudioPayloadType(int declared, const Quirks &quirks)
{
    if (quirks.isEnabled(QuirkId::SdpNonstandardCodec))
        return kNonstandardPayloadType;
    return declared;
}

QVector<sdp::TrackRole> sdp::trackLayout(const SdpOptions &options)
{
    QVector<TrackRole> layout;
    if (options.hasVideo)
        layout.append(TrackRole::Video);

    if (options.hasBackchannel) {
        // 单轨（海康）：开了 backchannel 就只给对讲轨，麦克风轨不出现。
        // 双轨（大华，E6）：麦克风 recvonly 在前 + 对讲 sendonly 在后 ——
        // 客户端不挑轨的话会推到麦克风轨上，相机那头一点声音都没有。
        if (options.dualTrackLayout && options.hasAudio
            && options.audioCodec != AudioCodec::None) {
            layout.append(TrackRole::Audio);
        }
        layout.append(TrackRole::Backchannel);
    } else if (options.hasAudio && options.audioCodec != AudioCodec::None) {
        layout.append(TrackRole::Audio);
    }
    return layout;
}

QByteArray sdp::generate(const SdpOptions &options, const Quirks &quirks)
{
    const QByteArray origin = options.originAddress.isEmpty()
        ? QByteArray("0.0.0.0")
        : options.originAddress.toUtf8();

    // session-id 从会话名与 control base 推出来：同一路流每次 DESCRIBE 拿到的 SDP 完全一致，
    // 客户端不会把两次 DESCRIBE 当成两个不同的会话（顺带让单测能断言全文）。
    const quint32 sessionId =
        quint32(qHash(options.sessionName + options.controlBase)) | 0x40000000u;

    QByteArray out;
    out += "v=0\r\n";
    out += "o=- " + QByteArray::number(sessionId) + " 1 IN IP4 " + origin + "\r\n";
    out += "s=" + options.sessionName.toUtf8() + "\r\n";
    // 连接地址写 0.0.0.0：RTSP 单播下真正的目的地址由 SETUP 的 Transport 决定。
    out += "c=IN IP4 0.0.0.0\r\n";
    out += "t=0 0\r\n";
    out += "a=range:npt=now-\r\n";
    // session 级 control 一直都在：E12 档下客户端只能靠这一条拼 SETUP URL。
    out += "a=control:"
        + (options.controlBase.isEmpty() ? QByteArray("*") : options.controlBase.toUtf8())
        + "\r\n";

    const QVector<TrackRole> layout = trackLayout(options);
    for (int i = 0; i < layout.size(); ++i) {
        switch (layout.at(i)) {
        case TrackRole::Video:
            appendVideoTrack(out, options, i, quirks);
            break;
        case TrackRole::Audio:
            appendAudioTrack(out, options, options.audioCodec, options.audioPayloadType,
                             false, i, quirks);
            break;
        case TrackRole::Backchannel:
            appendAudioTrack(out, options, options.backchannelCodec,
                             options.backchannelPayloadType, true, i, quirks);
            break;
        }
    }
    return out;
}

QString sdp::trackControl(const SdpOptions &options, int trackIndex, const Quirks &quirks)
{
    Q_UNUSED(options);
    // E12：媒体级一条 control 都不写。
    if (sessionLevelControlOnly(quirks))
        return QString();
    // 相对形式 trackID=<n>，客户端按 Content-Base（带尾斜杠）拼绝对 URL。
    // 这也是海康 / 大华真机的写法，大华那种带 query 的路径拼出来就是
    // rtsp://host/cam/realmonitor?channel=1&subtype=0/trackID=0。
    return QStringLiteral("trackID=%1").arg(trackIndex);
}

} // namespace onvifsim
