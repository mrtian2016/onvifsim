#pragma once

// 媒体源的通用类型。视频来自内嵌的预编码 H.264 样片（不做实时编码），
// 音频 PCMU/PCMA 运行时合成、AAC 用内嵌 ADTS 循环。

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QVector>

namespace onvifsim {

enum class VideoCodec { H264, H265, Jpeg };
enum class AudioCodec { None, PCMU, PCMA, G722, AAC };

struct EncodedFrame {
    QByteArray data;        // H.264 Annex B（含起始码）或一帧音频负载
    qint64 ptsUs = 0;       // 相对流起点的展示时间戳
    bool keyFrame = false;
    bool isParameterSet = false;   // SPS / PPS
};

namespace codec {
AudioCodec audioFromName(const QString &name, bool *ok = nullptr);
QString audioCodecName(AudioCodec codec);
int audioClockRate(AudioCodec codec);       // G722 按 RFC 3551 写 8000，实际 16000
int audioSampleRate(AudioCodec codec);      // 真实采样率
int audioPayloadType(AudioCodec codec);     // 静态 PT：PCMU=0 PCMA=8 G722=9，AAC 动态
QString videoCodecName(VideoCodec codec);
} // namespace codec

} // namespace onvifsim
