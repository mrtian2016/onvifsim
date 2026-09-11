#include "rtsp/RtpSender.h"

#include "core/Quirks.h"
#include "rtsp/RtpPacket_p.h"

#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QList>
#include <QtCore/QRandomGenerator>
#include <QtCore/QTimer>
#include <QtNetwork/QTcpSocket>
#include <QtNetwork/QUdpSocket>

namespace onvifsim {
namespace {

// interleaved 发送的写缓冲上限。与 HttpServer 的 kStreamBacklogLimit 同量级：
// 超过就丢包而不是继续积压。
constexpr qint64 kTcpBacklogLimit = 4 * 1024 * 1024;

constexpr int kRtcpIntervalMs = 5000;
// RFC 3550 §6.2：加入会话后的第一条 RTCP 要在半个间隔时发出，让接收端尽快拿到
// NTP ↔ RTP 时间戳的映射。等满 5 秒的话，客户端头几秒的音视频同步只能靠猜，
// 「不发 RTCP」这条 quirk 在短窗口里也就跟正常行为分不出来了。
constexpr int kRtcpFirstIntervalMs = kRtcpIntervalMs / 2;
// 限速时的排队上限。真机的发送队列也是有限的，满了丢最旧的包
// （丢掉的是最陈旧的画面，客户端表现为花屏而不是无限延迟）。
constexpr int kMaxQueuedPackets = 512;
constexpr int kRateTimerMs = 5;

// NTP 纪元 1900-01-01 与 Unix 纪元 1970-01-01 相差 70 年。
constexpr quint64 kNtpUnixDeltaSeconds = 2208988800ULL;

void appendRtcpHeader(QByteArray &out, int count, int payloadType, int lengthWords)
{
    out.append(char(0x80 | (count & 0x1f)));
    out.append(char(payloadType & 0xff));
    rtp::appendBE16(out, quint16(lengthWords));
}

// 从一条 RTCP 复合包里挑出第一个报告块的丢包率与抖动。
// 只关心 RR(201) 与带报告块的 SR(200)，别的（SDES / BYE / APP）跳过。
bool parseReceiverReport(const QByteArray &packet, int *lossPercent, qint64 *jitter)
{
    qsizetype offset = 0;
    while (offset + 4 <= packet.size()) {
        const uchar *p = rtp::bytesOf(packet) + offset;
        const int count = p[0] & 0x1f;
        const int payloadType = p[1];
        const int lengthBytes = (int(rtp::readBE16(p + 2)) + 1) * 4;
        if (lengthBytes <= 0 || offset + lengthBytes > packet.size())
            break;

        if ((payloadType == 201 || payloadType == 200) && count > 0) {
            // 报告块的起点：RR 头 8 字节，SR 头 28 字节。
            const qsizetype blockOffset = offset + (payloadType == 200 ? 28 : 8);
            if (blockOffset + 24 <= packet.size()) {
                const uchar *block = rtp::bytesOf(packet) + blockOffset;
                if (lossPercent)
                    *lossPercent = int(block[4]) * 100 / 256;   // fraction lost 是 8 位定点
                if (jitter)
                    *jitter = qint64(rtp::readBE32(block + 16));
                return true;
            }
        }
        offset += lengthBytes;
    }
    return false;
}

} // namespace

struct RtpSender::Private {
    RtpSender *q = nullptr;

    quint32 ssrc = 0;
    int payloadType = 96;
    int clockRate = 90000;
    quint16 sequence = 0;
    const Quirks *quirks = nullptr;
    bool running = false;
    RtpStats stats;

    // 目的地二选一：UDP 单播，或复用 RTSP 控制连接的 interleaved。
    QUdpSocket *udpRtp = nullptr;
    QUdpSocket *udpRtcp = nullptr;
    QHostAddress destination;
    quint16 destRtpPort = 0;
    quint16 destRtcpPort = 0;
    QTcpSocket *tcp = nullptr;
    int rtpChannel = 0;
    int rtcpChannel = 1;

    // RTCP SR 的累计量。
    QTimer *rtcpTimer = nullptr;
    quint32 packetCount = 0;
    quint32 octetCount = 0;

    // 时间戳跳变（quirk）：按周期往累计偏移里加量，而不是只跳一次，
    // 这样客户端的抖动缓冲会被反复冲击。
    QElapsedTimer clock;
    qint64 nextJumpMs = -1;
    qint64 timestampOffset = 0;

    // 限速（transport.rtp_rate_limit）：令牌桶 + 排队。带宽低于码流需求就持续积压，
    // 这正是这条 quirk 要复现的现象。
    QList<QByteArray> queue;
    QTimer *rateTimer = nullptr;
    double tokens = 0.0;
    qint64 lastRefillMs = 0;

    bool quirkOn(QuirkId id) const { return quirks && quirks->isEnabled(id); }

    void writeRtp(const QByteArray &packet);
    void enqueue(const QByteArray &packet);
    void drainQueue();
    void closeUdp();
};

void RtpSender::Private::closeUdp()
{
    delete udpRtp;
    delete udpRtcp;
    udpRtp = nullptr;
    udpRtcp = nullptr;
}

void RtpSender::Private::writeRtp(const QByteArray &packet)
{
    if (tcp) {
        if (tcp->state() != QAbstractSocket::ConnectedState)
            return;
        // interleaved（RTP over TCP）必须看积压。客户端一旦停止读取，TCP 窗口
        // 关到 0，QTcpSocket 的写缓冲就会无限长，而发送泵每 5 ms 还在往里灌 ——
        // 8 路 1080p 能在几十秒内把内存吃光。真机对这种消费者的表现就是丢包，
        // 所以这里也丢，并计进 packetsDropped（quirk E19 模拟的正是对端不排空缓冲，
        // 那条路径更要有这道闸）。同一个项目里 HttpServer 的流式响应早有同款保护。
        if (tcp->bytesToWrite() > kTcpBacklogLimit) {
            ++stats.packetsDropped;
            return;
        }
        tcp->write(rtp::interleavedFrame(rtpChannel, packet));
    } else if (udpRtp) {
        udpRtp->writeDatagram(packet, destination, destRtpPort);
    } else {
        return;   // 还没 SETUP 完，无处可发
    }

    ++stats.packetsSent;
    stats.bytesSent += packet.size();
    ++packetCount;
    octetCount += quint32(packet.size() - rtp::kHeaderSize);
}

void RtpSender::Private::enqueue(const QByteArray &packet)
{
    if (queue.size() >= kMaxQueuedPackets) {
        queue.removeFirst();
        ++stats.packetsDropped;
    }
    queue.append(packet);
    drainQueue();
    if (!queue.isEmpty() && !rateTimer->isActive())
        rateTimer->start();
}

void RtpSender::Private::drainQueue()
{
    if (queue.isEmpty())
        return;

    const int kbps = quirks ? quirks->paramInt(QuirkId::RtpRateLimit, QStringLiteral("kbps")) : 0;
    if (kbps <= 0) {
        while (!queue.isEmpty())
            writeRtp(queue.takeFirst());
        return;
    }

    const qint64 nowMs = clock.isValid() ? clock.elapsed() : 0;
    const double bytesPerMs = double(kbps) * 1000.0 / 8.0 / 1000.0;
    if (lastRefillMs == 0)
        lastRefillMs = nowMs;
    tokens += double(nowMs - lastRefillMs) * bytesPerMs;
    lastRefillMs = nowMs;
    // 桶深最多攒 100 ms：不封顶的话，一段空闲之后会突发一大串包，就不像限速了。
    tokens = qMin(tokens, bytesPerMs * 100.0);

    while (!queue.isEmpty() && tokens >= double(queue.first().size())) {
        const QByteArray packet = queue.takeFirst();
        tokens -= double(packet.size());
        writeRtp(packet);
    }
}

RtpSender::RtpSender(QObject *parent)
    : QObject(parent), d(new Private)
{
    d->q = this;
    // SSRC 与初始序列号按 RFC 3550 §5.1 随机取。固定初值会掩盖客户端
    // 「不看 SSRC 只按端口收流」这类实现缺陷。
    d->ssrc = QRandomGenerator::global()->generate();
    d->sequence = quint16(QRandomGenerator::global()->bounded(0x10000));
    d->stats.lastSequence = d->sequence;   // 还没发包时它就是「下一个要用的序号」

    d->rtcpTimer = new QTimer(this);
    d->rtcpTimer->setInterval(kRtcpIntervalMs);
    connect(d->rtcpTimer, &QTimer::timeout, this, &RtpSender::sendRtcpSenderReport);

    d->rateTimer = new QTimer(this);
    d->rateTimer->setTimerType(Qt::PreciseTimer);
    d->rateTimer->setInterval(kRateTimerMs);
    connect(d->rateTimer, &QTimer::timeout, this, [this] {
        d->drainQueue();
        if (d->queue.isEmpty())
            d->rateTimer->stop();
    });

    // RR 的统计只在这一处落库：UDP 收到的和 interleaved 转进来的都走同一条信号，
    // 两条路径不会各更新各的。
    connect(this, &RtpSender::receiverReport, this, [this](int lossPercent, qint64 jitter) {
        d->stats.lossPercentReported = lossPercent;
        d->stats.jitter = jitter;
    });
}

RtpSender::~RtpSender()
{
    stop();
    delete d;
}

void RtpSender::setPayloadType(int pt)
{
    d->payloadType = pt;
}

void RtpSender::setClockRate(int hz)
{
    if (hz > 0)
        d->clockRate = hz;
}

bool RtpSender::setUdpDestination(const QHostAddress &address, quint16 rtpPort, quint16 rtcpPort,
                                  const QHostAddress &bindAddress, QString *errorOut)
{
    d->closeUdp();
    QUdpSocket *rtpSocket = new QUdpSocket(this);
    QUdpSocket *rtcpSocket = new QUdpSocket(this);

    // RTP / RTCP 必须绑成相邻的偶 - 奇端口对（RFC 3550 §11）。随机起点重试若干次，
    // 免得多路流同时起来时抢同一个口。
    bool bound = false;
    for (int attempt = 0; attempt < 32 && !bound; ++attempt) {
        const quint16 candidate = quint16(20000 + 2 * QRandomGenerator::global()->bounded(10000));
        if (!rtpSocket->bind(bindAddress, candidate))
            continue;
        if (!rtcpSocket->bind(bindAddress, quint16(candidate + 1))) {
            rtpSocket->close();
            continue;
        }
        bound = true;
    }
    if (!bound) {
        if (errorOut)
            *errorOut = QStringLiteral("找不到可用的 RTP 端口对");
        delete rtpSocket;
        delete rtcpSocket;
        return false;
    }

    d->udpRtp = rtpSocket;
    d->udpRtcp = rtcpSocket;
    d->destination = address;
    d->destRtpPort = rtpPort;
    d->destRtcpPort = rtcpPort != 0 ? rtcpPort : quint16(rtpPort + 1);
    d->tcp = nullptr;

    connect(rtcpSocket, &QUdpSocket::readyRead, this, [this, rtcpSocket] {
        while (rtcpSocket->hasPendingDatagrams()) {
            QByteArray datagram;
            datagram.resize(int(rtcpSocket->pendingDatagramSize()));
            rtcpSocket->readDatagram(datagram.data(), datagram.size());
            int lossPercent = 0;
            qint64 jitter = 0;
            if (parseReceiverReport(datagram, &lossPercent, &jitter))
                emit receiverReport(lossPercent, jitter);
        }
    });
    return true;
}

void RtpSender::setInterleaved(QTcpSocket *socket, int rtpChannel, int rtcpChannel)
{
    d->tcp = socket;
    d->rtpChannel = rtpChannel;
    d->rtcpChannel = rtcpChannel;
    // interleaved 与 UDP 互斥：切过去就把 UDP socket 收掉，免得两头同时在发。
    d->closeUdp();
}

void RtpSender::setQuirks(const Quirks *quirks)
{
    d->quirks = quirks;
}

void RtpSender::sendPacket(const QByteArray &payload, quint32 timestamp, bool marker)
{
    if (!d->running)
        return;

    quint32 stamp = timestamp;
    // 时间戳跳变：到点往累计偏移里加一段，之后的包都带着这个偏移走。
    if (d->quirkOn(QuirkId::RtpTimestampJump) && d->clock.isValid()) {
        const qint64 periodMs = qint64(d->quirks->paramInt(QuirkId::RtpTimestampJump,
                                                           QStringLiteral("seconds"))) * 1000;
        if (periodMs > 0) {
            if (d->nextJumpMs < 0)
                d->nextJumpMs = periodMs;
            if (d->clock.elapsed() >= d->nextJumpMs) {
                const qint64 deltaMs = d->quirks->paramInt(QuirkId::RtpTimestampJump,
                                                           QStringLiteral("delta_ms"));
                d->timestampOffset += deltaMs * d->clockRate / 1000;
                d->nextJumpMs += periodMs;
            }
        }
        stamp = quint32(qint64(timestamp) + d->timestampOffset);
    }

    const quint16 sequence = d->sequence++;

    // 丢包：序列号照样消耗掉，客户端才看得到断层 —— 真实丢包就是这个样子。
    if (d->quirkOn(QuirkId::RtpPacketLoss)) {
        const double percent = d->quirks->paramDouble(QuirkId::RtpPacketLoss,
                                                      QStringLiteral("percent"));
        if (percent > 0.0 && QRandomGenerator::global()->bounded(100.0) < percent) {
            ++d->stats.packetsDropped;
            return;
        }
    }

    const QByteArray packet =
        rtp::buildPacket(d->payloadType, marker, sequence, stamp, d->ssrc, payload);
    d->stats.lastSequence = sequence;
    d->stats.lastTimestamp = stamp;

    if (d->quirkOn(QuirkId::RtpRateLimit)) {
        d->enqueue(packet);
        return;
    }
    d->writeRtp(packet);
}

void RtpSender::sendRtcpSenderReport()
{
    // NoRtcp：客户端拿不到 NTP 映射，音视频同步只能靠 RTP 时间戳硬估。
    if (d->quirkOn(QuirkId::NoRtcp) || !d->running)
        return;

    const qint64 unixMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    const quint32 ntpSeconds = quint32(quint64(unixMs / 1000) + kNtpUnixDeltaSeconds);
    const quint32 ntpFraction = quint32(double(unixMs % 1000) * 4294967.296);

    QByteArray sr;
    appendRtcpHeader(sr, 0, 200, 6);              // SR，无报告块：长度 6 个 32 位字
    rtp::appendBE32(sr, d->ssrc);
    rtp::appendBE32(sr, ntpSeconds);
    rtp::appendBE32(sr, ntpFraction);
    rtp::appendBE32(sr, d->stats.lastTimestamp);
    rtp::appendBE32(sr, d->packetCount);
    rtp::appendBE32(sr, d->octetCount);

    // 复合包必须跟一条 SDES CNAME（RFC 3550 §6.1）：少数客户端见不到就丢掉整个复合包。
    const QByteArray cname = "onvifsim@" + QByteArray::number(d->ssrc, 16);
    const int itemBytes = 2 + int(cname.size()) + 1;          // type + len + 文本 + 结束符
    const int padded = ((itemBytes + 3) / 4) * 4;
    QByteArray sdes;
    appendRtcpHeader(sdes, 1, 202, 1 + padded / 4);
    rtp::appendBE32(sdes, d->ssrc);
    sdes.append(char(1));                                     // CNAME
    sdes.append(char(cname.size()));
    sdes.append(cname);
    while (sdes.size() % 4 != 0)
        sdes.append(char(0));

    // 第一条发出去之后回到常规间隔（setInterval 会从此刻重新计时，正是想要的）。
    if (d->rtcpTimer->interval() != kRtcpIntervalMs)
        d->rtcpTimer->setInterval(kRtcpIntervalMs);

    const QByteArray compound = sr + sdes;
    if (d->tcp) {
        if (d->tcp->state() == QAbstractSocket::ConnectedState)
            d->tcp->write(rtp::interleavedFrame(d->rtcpChannel, compound));
    } else if (d->udpRtcp) {
        d->udpRtcp->writeDatagram(compound, d->destination, d->destRtcpPort);
    }
}

void RtpSender::start()
{
    if (d->running)
        return;
    d->running = true;
    d->clock.start();
    d->nextJumpMs = -1;
    d->lastRefillMs = 0;
    d->tokens = 0.0;
    if (!d->quirkOn(QuirkId::NoRtcp)) {
        d->rtcpTimer->setInterval(kRtcpFirstIntervalMs);
        d->rtcpTimer->start();
    }
}

void RtpSender::stop()
{
    if (!d->running)
        return;
    d->running = false;
    d->rtcpTimer->stop();
    d->rateTimer->stop();
    d->queue.clear();
}

bool RtpSender::isRunning() const
{
    return d->running;
}

quint16 RtpSender::localRtpPort() const
{
    return d->udpRtp ? d->udpRtp->localPort() : quint16(0);
}

quint16 RtpSender::localRtcpPort() const
{
    return d->udpRtcp ? d->udpRtcp->localPort() : quint16(0);
}

void RtpSender::feedRtcp(const QByteArray &packet)
{
    // 走的是和 UDP 那条一样的信号：统计只在构造函数里那一处落库。
    int lossPercent = 0;
    qint64 jitter = 0;
    if (parseReceiverReport(packet, &lossPercent, &jitter))
        emit receiverReport(lossPercent, jitter);
}

RtpStats RtpSender::stats() const
{
    return d->stats;
}

void RtpSender::resetStats()
{
    const quint16 sequence = d->stats.lastSequence;
    d->stats = RtpStats();
    d->stats.lastSequence = sequence;
    d->packetCount = 0;
    d->octetCount = 0;
}

} // namespace onvifsim
