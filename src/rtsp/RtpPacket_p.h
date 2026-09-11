#pragma once

// RTP 包头（RFC 3550 §5.1）与 RTSP interleaved 帧（RFC 2326 §10.12）的字节编解码。
//
// 发送侧（RtpSender）、接收侧（RtpReceiver）、以及在 RTSP 控制连接上拆包的
// RtspSession 都要碰这套位序，三处各写一遍迟早对不齐，所以收口到这里。
// 放在私有头里还有一个好处：单元测试能脱开 socket 直接验字节。

#include <QtCore/QByteArray>
#include <QtCore/QtGlobal>

namespace onvifsim {
namespace rtp {

constexpr int kHeaderSize = 12;
constexpr int kInterleavedHeaderSize = 4;
constexpr char kInterleavedMagic = '$';

struct Header {
    int version = 2;
    bool padding = false;
    bool extension = false;
    int csrcCount = 0;
    bool marker = false;
    int payloadType = 0;
    quint16 sequence = 0;
    quint32 timestamp = 0;
    quint32 ssrc = 0;
    int headerBytes = kHeaderSize;   // 含 CSRC 与扩展，负载从这个偏移开始
};

inline void appendBE16(QByteArray &out, quint16 value)
{
    out.append(char((value >> 8) & 0xff));
    out.append(char(value & 0xff));
}

inline void appendBE32(QByteArray &out, quint32 value)
{
    out.append(char((value >> 24) & 0xff));
    out.append(char((value >> 16) & 0xff));
    out.append(char((value >> 8) & 0xff));
    out.append(char(value & 0xff));
}

inline quint16 readBE16(const uchar *p)
{
    return quint16(quint16(p[0]) << 8 | quint16(p[1]));
}

inline quint32 readBE32(const uchar *p)
{
    return quint32(p[0]) << 24 | quint32(p[1]) << 16 | quint32(p[2]) << 8 | quint32(p[3]);
}

inline const uchar *bytesOf(const QByteArray &data)
{
    return reinterpret_cast<const uchar *>(data.constData());
}

// 固定 12 字节头：V=2，无填充、无扩展、无 CSRC。模拟器不需要更花的形态。
inline QByteArray buildPacket(int payloadType, bool marker, quint16 sequence, quint32 timestamp,
                              quint32 ssrc, const QByteArray &payload)
{
    QByteArray out;
    out.reserve(kHeaderSize + payload.size());
    // 不写成 char(0x80)：0x80 超出 char 的取值范围，MSVC 会报 C4310「常量被截断」。
    // '\x80' 本身就是 char 字面量；带位运算的那行走 uchar 再落回 char。
    out.append('\x80');
    out.append(static_cast<char>(
        static_cast<unsigned char>((marker ? 0x80 : 0x00) | (payloadType & 0x7f))));
    appendBE16(out, sequence);
    appendBE32(out, timestamp);
    appendBE32(out, ssrc);
    out.append(payload);
    return out;
}

// 对讲侧收到的包是客户端发的，什么形态都可能出现：CSRC、头扩展、尾填充都要能跳过，
// 否则算电平时会把这些字节当成音频采样。
inline bool parseHeader(const QByteArray &packet, Header *out)
{
    if (packet.size() < kHeaderSize)
        return false;
    const uchar *p = bytesOf(packet);
    Header h;
    h.version = (p[0] >> 6) & 0x03;
    if (h.version != 2)
        return false;
    h.padding = (p[0] & 0x20) != 0;
    h.extension = (p[0] & 0x10) != 0;
    h.csrcCount = p[0] & 0x0f;
    h.marker = (p[1] & 0x80) != 0;
    h.payloadType = p[1] & 0x7f;
    h.sequence = readBE16(p + 2);
    h.timestamp = readBE32(p + 4);
    h.ssrc = readBE32(p + 8);

    int bytes = kHeaderSize + 4 * h.csrcCount;
    if (packet.size() < bytes)
        return false;
    if (h.extension) {
        if (packet.size() < bytes + 4)
            return false;
        const int words = int(readBE16(bytesOf(packet) + bytes + 2));
        bytes += 4 + 4 * words;
        if (packet.size() < bytes)
            return false;
    }
    h.headerBytes = bytes;
    if (out)
        *out = h;
    return true;
}

inline QByteArray payloadOf(const QByteArray &packet, const Header &header)
{
    QByteArray payload = packet.mid(header.headerBytes);
    // 填充位：最后一个字节是填充长度，且把它自己算在内。
    if (header.padding && !payload.isEmpty()) {
        const int pad = int(uchar(payload.at(payload.size() - 1)));
        if (pad > 0 && pad <= payload.size())
            payload.chop(pad);
    }
    return payload;
}

inline QByteArray interleavedFrame(int channel, const QByteArray &payload)
{
    QByteArray out;
    out.reserve(kInterleavedHeaderSize + payload.size());
    out.append(kInterleavedMagic);
    out.append(char(channel & 0xff));
    appendBE16(out, quint16(payload.size() & 0xffff));
    out.append(payload);
    return out;
}

// 从缓冲区头部取一个完整的 interleaved 帧。返回 false 表示还没收够（缓冲区不动），
// 调用方等下一批数据再试。长度字段只有 16 位，所以打包侧必须把 MTU 压在 64K 以内。
inline bool takeInterleavedFrame(QByteArray &buffer, int *channel, QByteArray *payload)
{
    if (buffer.size() < kInterleavedHeaderSize)
        return false;
    if (buffer.at(0) != kInterleavedMagic)
        return false;
    const uchar *p = bytesOf(buffer);
    const int length = int(readBE16(p + 2));
    if (buffer.size() < kInterleavedHeaderSize + length)
        return false;
    if (channel)
        *channel = int(p[1]);
    if (payload)
        *payload = buffer.mid(kInterleavedHeaderSize, length);
    buffer.remove(0, kInterleavedHeaderSize + length);
    return true;
}

} // namespace rtp
} // namespace onvifsim
