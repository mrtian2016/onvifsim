#include "rtsp/RtpReceiver.h"

#include "core/LogBus.h"
#include "core/Quirks.h"
#include "media/AudioSource.h"
#include "rtsp/RtpPacket_p.h"

#include <QtCore/QFile>

namespace onvifsim {
namespace {

// 距上一个包超过这么久，就认为上一段话已经结束、这是新的一段（talkspurt）。
// 20 ms 一包的 G.711 正常间隔在 20 ms 上下，200 ms 足够区分「连续说话」与「重新开口」，
// 又不会把一次网络抖动误判成新段。
constexpr qint64 kTalkspurtGapMs = 200;

constexpr int kWavHeaderSize = 44;

void writeLE16(QByteArray &out, quint16 value)
{
    out.append(char(value & 0xff));
    out.append(char((value >> 8) & 0xff));
}

void writeLE32(QByteArray &out, quint32 value)
{
    out.append(char(value & 0xff));
    out.append(char((value >> 8) & 0xff));
    out.append(char((value >> 16) & 0xff));
    out.append(char((value >> 24) & 0xff));
}

// 先写一个长度全 0 的头占位，停止录音时再回填 —— 边收边写，中途崩了也留得下已收到的部分。
QByteArray wavHeader(int sampleRate, quint32 dataBytes)
{
    QByteArray header;
    header.reserve(kWavHeaderSize);
    header.append("RIFF");
    writeLE32(header, 36 + dataBytes);
    header.append("WAVE");
    header.append("fmt ");
    writeLE32(header, 16);
    writeLE16(header, 1);                                   // PCM
    writeLE16(header, 1);                                   // 单声道
    writeLE32(header, quint32(sampleRate));
    writeLE32(header, quint32(sampleRate * 2));             // 字节率
    writeLE16(header, 2);                                   // 块对齐
    writeLE16(header, 16);                                  // 位深
    header.append("data");
    writeLE32(header, dataBytes);
    return header;
}

} // namespace

struct RtpReceiver::Private {
    AudioCodec codec = AudioCodec::PCMU;
    const Quirks *quirks = nullptr;
    TalkbackStats stats;

    bool haveSequence = false;
    quint16 expectedSequence = 0;
    // E13：缺 marker 的那一段整段静音丢弃，直到下一个带 marker 的包才恢复。
    bool mutedUntilMarker = false;

    QFile *wav = nullptr;
    quint32 wavDataBytes = 0;

    bool quirkOn(QuirkId id) const { return quirks && quirks->isEnabled(id); }

    // 这个类不持有相机，走全局日志总线（IpAlias 也是这么做的）。
    void log(LogLevel level, const QString &summary) const
    {
        if (LogBus *bus = LogBus::global()) {
            LogRecord rec;
            rec.level = level;
            rec.category = logcat::Rtp;
            rec.summary = summary;
            rec.ok = level < LogLevel::Warning;
            bus->post(rec);
        }
    }
};

RtpReceiver::RtpReceiver(QObject *parent)
    : QObject(parent), d(new Private)
{
    d->stats.codec = d->codec;
}

RtpReceiver::~RtpReceiver()
{
    stopRecording();
    delete d;
}

void RtpReceiver::setCodec(AudioCodec codec)
{
    d->codec = codec;
    d->stats.codec = codec;
}

void RtpReceiver::setQuirks(const Quirks *quirks)
{
    d->quirks = quirks;
}

void RtpReceiver::feed(const QByteArray &rtpPacket)
{
    rtp::Header header;
    // 版本不是 2 或者头都不完整：客户端发歪了，直接丢，不进任何统计。
    if (!rtp::parseHeader(rtpPacket, &header))
        return;

    const QByteArray payload = rtp::payloadOf(rtpPacket, header);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const bool firstEver = !d->stats.firstPacketAt.isValid();

    // 丢包按序列号断层推算。回绕用 16 位无符号减法处理；差值超过半个序号空间的
    // 当成乱序 / 迟到包，不计入丢包，否则一次乱序会被算成六万多个丢包。
    if (d->haveSequence) {
        const quint16 delta = quint16(header.sequence - d->expectedSequence);
        if (delta > 0 && delta < 0x8000)
            d->stats.packetsDropped += delta;
    }
    d->expectedSequence = quint16(header.sequence + 1);
    d->haveSequence = true;

    // talkspurt 边界：首包，或者离上一个包已经静了一段时间。
    const bool boundary = firstEver
        || (d->stats.lastPacketAt.isValid() && d->stats.lastPacketAt.msecsTo(now) > kTalkspurtGapMs);

    if (header.marker) {
        ++d->stats.markerCount;
        d->mutedUntilMarker = false;
    } else if (boundary) {
        // 客户端漏设了这一段的首包 marker。真机上冷启动的相机会整段丢掉，
        // 解码器还热着的时候又照常出声，所以现象是偶发的 —— E13 就是复现这个。
        ++d->stats.markerMissing;
        if (d->quirkOn(QuirkId::TalkbackRequireMarker))
            d->mutedUntilMarker = true;
    }

    ++d->stats.packetsReceived;
    d->stats.bytesReceived += rtpPacket.size();
    if (firstEver)
        d->stats.firstPacketAt = now;
    d->stats.lastPacketAt = now;

    if (d->mutedUntilMarker) {
        // 丢弃这一段：电平归零，也不进录音，但包本身照收（相机不会因此断连）。
        if (d->stats.currentLevel != 0.0) {
            d->stats.currentLevel = 0.0;
            emit levelChanged(0.0);
        }
        return;
    }

    const double level = AudioSource::levelOf(payload, d->codec);
    d->stats.currentLevel = level;
    d->stats.peakLevel = qMax(d->stats.peakLevel, level);

    if (d->wav) {
        const QByteArray pcm = AudioSource::decodeToPcm16(payload, d->codec);
        if (!pcm.isEmpty()) {
            // 写失败要说一声并停掉录制：静默截断的结果是用户拿到一个能打开
            // 但内容不全的 wav，日志里一个字都没有，等他发现已经是几天之后。
            if (d->wav->write(pcm) != pcm.size()) {
                d->log(LogLevel::Warning,
                       QStringLiteral("Talkback recording failed, stopped: %1")
                           .arg(d->wav->errorString()));
                stopRecordingInternal();
            } else {
                d->wavDataBytes += quint32(pcm.size());
            }
        }
    }

    emit levelChanged(level);
}

TalkbackStats RtpReceiver::stats() const
{
    return d->stats;
}

void RtpReceiver::resetStats()
{
    const AudioCodec codec = d->stats.codec;
    d->stats = TalkbackStats();
    d->stats.codec = codec;
    d->haveSequence = false;
    d->mutedUntilMarker = false;
}

bool RtpReceiver::startRecording(const QString &path, QString *errorOut)
{
    stopRecording();
    QFile *file = new QFile(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut)
            *errorOut = file->errorString();
        delete file;
        return false;
    }
    const int sampleRate = codec::audioSampleRate(d->codec);
    file->write(wavHeader(sampleRate > 0 ? sampleRate : 8000, 0));
    d->wav = file;
    d->wavDataBytes = 0;
    return true;
}

void RtpReceiver::stopRecording()
{
    stopRecordingInternal();
}

void RtpReceiver::stopRecordingInternal()
{
    if (!d->wav)
        return;
    // 回填 RIFF / data 两处长度，播放器才认。回填失败的文件是打不开的，
    // 必须说出来 —— 不然用户拿到的是一个「看起来录好了」的坏文件。
    const int sampleRate = codec::audioSampleRate(d->codec);
    const QByteArray header = wavHeader(sampleRate > 0 ? sampleRate : 8000, d->wavDataBytes);
    if (!d->wav->seek(0) || d->wav->write(header) != header.size()) {
        d->log(LogLevel::Warning,
               QStringLiteral("Could not patch the length fields of the talkback recording; the file may not open: %1")
                   .arg(d->wav->errorString()));
    }
    d->wav->close();
    delete d->wav;
    d->wav = nullptr;
    d->wavDataBytes = 0;
}

bool RtpReceiver::isRecording() const
{
    return d->wav != nullptr;
}

} // namespace onvifsim
