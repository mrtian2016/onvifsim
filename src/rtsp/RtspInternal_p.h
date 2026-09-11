#pragma once

// rtsp 模块内部的胶水声明。都是「公共头里没有、但模块内部必须共享」的东西：
//
//   1. SDP 的轨道布局与最终 payload type：DESCRIBE 生成 SDP 与 SETUP 解析 trackID
//      必须用同一套顺序，各写一遍就会在双轨（E6）/ 单轨切换时错位。
//   2. 报文与 URL 的纯解析：RtspServer 按路径反查 profile、RtspSession 按 trackID
//      定位轨，两边要用同一套规则。

// 外部模块（GUI / REST）没有理由碰这些，所以不进公共头。

#include "media/MediaTypes.h"
#include "rtsp/RtspTypes.h"

#include <QtCore/QByteArray>
#include <QtCore/QVector>
#include <QtCore/QtGlobal>

namespace onvifsim {

class Quirks;
struct SdpOptions;

namespace sdp {

// 一条 SDP 里 m= 段的顺序与角色。索引就是 a=control:trackID=<n> 里的 n。
enum class TrackRole { Video, Audio, Backchannel };

QVector<TrackRole> trackLayout(const SdpOptions &options);

// SDP 里最终写出去的音频 payload type：E10 会把它挪到动态区，
// 实际发 RTP 时也得用同一个值，否则声明与实发对不上就不只是「不规范」而是发不出去了。
int effectiveAudioPayloadType(int declared, const Quirks &quirks);

// SDP 里 a=rtpmap 实际写出去的音频时钟率。**RTP 时间戳必须按同一个数递增** ——
// E10（SdpNonstandardCodec）把 codec 名改成 G722.1 之类时 SDP 会声明 16000，
// 而发送侧原来仍按底层 codec 的 8000 走，客户端拿到的就是两倍速播放。
// 那不是这条 quirk 想复现的行为（它要的是「名字不规范」），是实现漂移。
int effectiveAudioClockRate(AudioCodec codec, const Quirks &quirks);

} // namespace sdp

namespace rtspInternal {

// ---- 报文与 URL 的纯解析（实现在 RtspTypes.cpp）------------------------------
// 放这里是因为 RtspServer 与 RtspSession 两边都要用同一套规则：
// 一边按路径反查 profile，一边按 trackID 定位轨，两处各写一份迟早对不上。

// 解析一条完整的 RTSP 请求。返回消耗掉的字节数；0 表示还没收够，缓冲区不要动。
qsizetype parseRequest(const QByteArray &buffer, RtspRequest *out, bool *malformed = nullptr);
// 请求行的 target 可能是绝对 URI、绝对路径或 "*"，取「路径 + query」整串。
QString requestTarget(const QString &uri);
// SETUP URL 末尾那段 trackID=N；没有返回 -1。
int trackIndexOf(const QString &target);
// 归一到能与 profile.streamPath 比对的形态（剥掉 trackID 与尾斜杠）。
QString normalizeStreamPath(const QString &raw);

} // namespace rtspInternal
} // namespace onvifsim
