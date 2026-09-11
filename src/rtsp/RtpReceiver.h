#pragma once

// 对讲接收侧。解 RTP 头，按声明的 codec 算电平与丢包，
// 校验 talkspurt 首包 marker（quirk E13，缺 marker 可选静音丢弃），统计写 REST。

#include "media/MediaTypes.h"

#include <QtCore/QDateTime>
#include <QtCore/QObject>

namespace onvifsim {

class Quirks;

struct TalkbackStats {
    qint64 packetsReceived = 0;
    qint64 bytesReceived = 0;
    qint64 packetsDropped = 0;       // 序列号断层推算
    qint64 markerCount = 0;          // talkspurt 起始包数
    qint64 markerMissing = 0;        // 该有 marker 却没有的段数
    double currentLevel = 0.0;       // 0..1
    double peakLevel = 0.0;
    QDateTime firstPacketAt;
    QDateTime lastPacketAt;
    AudioCodec codec = AudioCodec::None;
};

class RtpReceiver : public QObject
{
    Q_OBJECT
public:
    explicit RtpReceiver(QObject *parent = nullptr);
    ~RtpReceiver() override;

    void setCodec(AudioCodec codec);
    void setQuirks(const Quirks *quirks);

    // 喂一个完整的 RTP 包（interleaved 模式下由 RtspSession 拆出来后送进来）。
    void feed(const QByteArray &rtpPacket);

    TalkbackStats stats() const;
    void resetStats();

    // 保存收到的音频为 wav（二期的本机回放也从这里取 PCM）。
    bool startRecording(const QString &path, QString *errorOut = nullptr);
    void stopRecording();
    bool isRecording() const;

signals:
    void levelChanged(double level);

private:
    // stopRecording() 的实现体。写入路径上出错时也要走它收尾，
    // 而那条路径本身就在 Private 里，不能再回头调公开的槽。
    void stopRecordingInternal();

    struct Private;
    Private *d;
};

} // namespace onvifsim
