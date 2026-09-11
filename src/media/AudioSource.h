#pragma once

// 音频源。PCMU / PCMA 运行时合成（正弦 / 静音 / 扫频），
// AAC 用内嵌的 10 秒 ADTS 循环。
//
// quirk CodecMismatch（E1 谎标）就是「声明」与「实发」两处分别可配，
// 所以这里只管实发，声明侧在 Media 服务里。

#include "media/MediaTypes.h"

#include <QtCore/QtGlobal>
#include <QtCore/QByteArray>
#include <QtCore/QString>

namespace onvifsim {

enum class ToneKind { Silence, Sine440, Sweep, Noise };

class AudioSource
{
public:
    AudioSource();
    ~AudioSource();

    bool configure(AudioCodec codec, ToneKind tone = ToneKind::Sine440, QString *errorOut = nullptr);
    AudioCodec codec() const;
    ToneKind tone() const;
    void setTone(ToneKind tone);

    int sampleRate() const;
    int samplesPerPacket() const;    // 一个 RTP 包装多少采样（G.711 默认 20ms = 160）
    int packetDurationUs() const;

    // 取第 index 个音频包的负载。AAC 时是一个 AU（不带 ADTS 头，按 RFC 3640 打包）。
    QByteArray packet(int index) const;
    // AAC 的 RFC 3640 AU 头（mpeg4-generic，sizelength=13;indexlength=3;indexdeltalength=3）。
    static QByteArray aacAuHeader(int auSize);
    // SDP 里 AAC 的 config 参数。
    QByteArray aacConfig() const;

    // 解码收到的对讲负载算电平（0..1），用于 GUI 电平表与统计。
    static double levelOf(const QByteArray &payload, AudioCodec codec);
    // PCMU / PCMA / G722 → 16 位 PCM，供保存 wav 与可选本机回放。
    static QByteArray decodeToPcm16(const QByteArray &payload, AudioCodec codec);

private:
    // 裸 Private* + 析构里 delete d，默认拷贝构造会让两个对象指向同一块内存，
    // 第二次析构就是 double free —— 而编译器一个字都不会说。项目里其余 PIMPL
    // 类都继承自 QObject（天生禁拷贝），只有这几个是裸的。
    Q_DISABLE_COPY(AudioSource)

    struct Private;
    Private *d;
};

} // namespace onvifsim
