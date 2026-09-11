#pragma once

// RTP 发送。UDP 单播与 TCP interleaved 都支持。
//
// 时基：H.264 用 90kHz，音频用各自时钟率。
// 发送节奏用 Qt::PreciseTimer 加「按 elapsed 累计补发」，抖动不累积。
// RTCP SR 每 5 秒一条（首条按 RFC 3550 §6.2 只等半个间隔），收 RR 做统计（quirk NoRtcp 可关）。

#include "media/MediaTypes.h"

#include <QtCore/QObject>
#include <QtNetwork/QHostAddress>

class QTcpSocket;
class QUdpSocket;

namespace onvifsim {

class Quirks;

struct RtpStats {
    qint64 packetsSent = 0;
    qint64 bytesSent = 0;
    qint64 packetsDropped = 0;      // 被 quirk 主动丢的
    quint32 lastTimestamp = 0;
    // 已发过包时是最后一个用掉的序号；一包未发时是「下一个要用的序号」——
    // PLAY 响应的 RTP-Info: seq= 要的正是后者。
    quint16 lastSequence = 0;
    int lossPercentReported = 0;    // 来自 RR
    qint64 jitter = 0;
};

class RtpSender : public QObject
{
    Q_OBJECT
public:
    explicit RtpSender(QObject *parent = nullptr);
    ~RtpSender() override;

    void setPayloadType(int pt);
    void setClockRate(int hz);

    // 二选一：UDP 单播，或 TCP interleaved（复用 RTSP 控制连接）。
    bool setUdpDestination(const QHostAddress &address, quint16 rtpPort, quint16 rtcpPort,
                           const QHostAddress &bindAddress, QString *errorOut = nullptr);
    void setInterleaved(QTcpSocket *socket, int rtpChannel, int rtcpChannel);

    // SETUP 的 Transport 响应要回 server_port=<rtp>-<rtcp>，取的就是这两个。
    // 未走 UDP 时返回 0。
    quint16 localRtpPort() const;
    quint16 localRtcpPort() const;

    // interleaved 模式下 RR 是从 RTSP 控制连接进来的，RtpSender 读不到那条 socket，
    // 由 RtspSession 拆出 RTCP 帧后喂进来。
    void feedRtcp(const QByteArray &packet);

    void setQuirks(const Quirks *quirks);

    // 发一个已经打好包的 RTP 负载。marker 由调用方决定（一帧最后一个包置位）。
    void sendPacket(const QByteArray &payload, quint32 timestamp, bool marker);
    void sendRtcpSenderReport();

    void start();
    void stop();
    bool isRunning() const;

    RtpStats stats() const;
    void resetStats();

signals:
    void receiverReport(int lossPercent, qint64 jitter);

private:
    struct Private;
    Private *d;
};

} // namespace onvifsim
