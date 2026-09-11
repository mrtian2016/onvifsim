#include "media/AudioSource.h"

#include <QtCore/QFile>
#include <QtCore/QVector>
#include <QtCore/qendian.h>

#include <cmath>

// 见 H264Source.cpp：静态库里的 qrc 得显式点名一次，AAC 样片也在同一个资源里。
void onvifsimInitMediaResources();

namespace onvifsim {
namespace {

// kPi 不是标准 C++，MSVC 默认不给，自己定义一个。
constexpr double kPi = 3.14159265358979323846;

constexpr int kPacketMs = 20;                    // G.711 / G.722 的行业默认包长
constexpr int kAacSamplesPerFrame = 1024;        // AAC-LC 一个 AU 固定 1024 采样
const char *const kAacResource = ":/media/aac-16k.aac";

// ---- G.711 --------------------------------------------------------------
// μ-law / A-law 都是 8 bit 对数压扩，查表和算法等价；这里用算法版，
// 省掉两张 256 项的表，也省得表抄错了没人发现。

constexpr int kUlawBias = 0x84;
constexpr int kUlawClip = 32635;

quint8 linearToUlaw(int sample)
{
    int sign = 0;
    if (sample < 0) {
        sample = -sample;
        sign = 0x80;
    }
    if (sample > kUlawClip)
        sample = kUlawClip;
    sample += kUlawBias;

    // 段号就是加了 bias 之后最高位 1 的位置：等价于查那张 exp_lut。
    int segment = 0;
    for (int v = (sample >> 7) & 0xFF; v > 1; v >>= 1)
        ++segment;
    const int mantissa = (sample >> (segment + 3)) & 0x0F;
    return static_cast<quint8>(~(sign | (segment << 4) | mantissa));
}

qint16 ulawToLinear(quint8 code)
{
    const int value = ~static_cast<int>(code);
    int magnitude = ((value & 0x0F) << 3) + kUlawBias;
    magnitude <<= ((value & 0x70) >> 4);
    magnitude -= kUlawBias;
    return static_cast<qint16>((value & 0x80) ? -magnitude : magnitude);
}

quint8 linearToAlaw(int sample)
{
    // A-law 工作在 13 bit 上，先丢掉低 3 位。
    int value = sample >> 3;
    int mask;
    if (value >= 0) {
        mask = 0xD5;                 // 偶数位取反是 A-law 的规定，不是笔误
    } else {
        mask = 0x55;
        value = -value - 1;
    }
    if (value > 0x0FFF)
        value = 0x0FFF;

    int segment = 0;
    for (int v = value >> 5; v > 0 && segment < 7; v >>= 1)
        ++segment;

    // 段 0 和段 1 共用 >>1 的量化步长（A-law 的近线性段），别只判 segment == 0。
    const int code = (segment < 2 ? (value >> 1) : (value >> segment)) & 0x0F;
    return static_cast<quint8>((code | (segment << 4)) ^ mask);
}

qint16 alawToLinear(quint8 code)
{
    const int value = static_cast<int>(code) ^ 0x55;
    int magnitude = (value & 0x0F) << 4;
    const int segment = (value & 0x70) >> 4;
    if (segment == 0)
        magnitude += 8;
    else if (segment == 1)
        magnitude += 0x108;
    else
        magnitude = (magnitude + 0x108) << (segment - 1);
    return static_cast<qint16>((value & 0x80) ? magnitude : -magnitude);
}

// ---- G.722（近似实现）---------------------------------------------------
//
// 真正的 ITU-T G.722 是子带 ADPCM：24 抽头 QMF 分带，低带 6 bit / 高带 2 bit 自适应量化，
// 一整套 ITU 参考表。对这个模拟器没有价值 —— 客户端要验的是「有一路 16 kHz 音轨、
// 而 SDP 里的 RTP 时钟率却写 8000」这件事本身，不是解码音质。
//
// 所以这里保留 G.722 真正重要的外部特征：码流结构（1 字节 = 2 个 16 kHz 采样，
// 低 6 bit 低带 + 高 2 bit 高带）、8 kB/s 的字节率、160 字节/20 ms 的包长。
// 内部换成 Haar 分带（和/差）+ 复用 μ-law 的对数量化，自编自解一致，
// 电平表、wav 导出、丢包统计全都对得上。比特精确的 G.722 留到二期。
constexpr int kG722HighThreshold = 2048;
constexpr int kG722HighSmall = 512;
constexpr int kG722HighLarge = 6000;

quint8 encodeG722Pair(int first, int second)
{
    const int low = (first + second) / 2;
    const int high = (first - second) / 2;
    const quint8 lowCode = static_cast<quint8>(linearToUlaw(low) >> 2);
    // 高带 2 bit 是 mid-riser（没有零电平），和真 G.722 一样：
    // 输入接近零时码字来回跳，解出来平均下去仍然接近零。
    const quint8 level = (qAbs(high) > kG722HighThreshold) ? 1u : 0u;
    const quint8 sign = (high < 0) ? 2u : 0u;
    return static_cast<quint8>(((sign | level) << 6) | lowCode);
}

void decodeG722Byte(quint8 code, qint16 *first, qint16 *second)
{
    const int low = ulawToLinear(static_cast<quint8>((code & 0x3F) << 2));
    const quint8 highCode = static_cast<quint8>(code >> 6);
    int high = (highCode & 1u) ? kG722HighLarge : kG722HighSmall;
    if (highCode & 2u)
        high = -high;
    *first = static_cast<qint16>(qBound(-32768, low + high, 32767));
    *second = static_cast<qint16>(qBound(-32768, low - high, 32767));
}

// ---- 波形合成 -----------------------------------------------------------

constexpr double kToneAmplitude = 16384.0;       // 半幅：留足余量，避免压扩后削顶
constexpr double kSweepLowHz = 300.0;
constexpr double kSweepHighHz = 3300.0;
constexpr double kSweepPeriodSec = 4.0;

qint16 synthSample(ToneKind tone, qint64 n, int sampleRate)
{
    if (sampleRate <= 0)
        return 0;
    const double t = static_cast<double>(n) / static_cast<double>(sampleRate);
    switch (tone) {
    case ToneKind::Silence:
        return 0;
    case ToneKind::Sine440:
        // 相位直接由绝对采样序号算，包与包之间天然连续，不用维护状态（packet() 是 const）。
        return static_cast<qint16>(kToneAmplitude * std::sin(2.0 * kPi * 440.0 * t));
    case ToneKind::Sweep: {
        // 线性扫频，相位是瞬时频率的积分。每 kSweepPeriodSec 回到起点，
        // 回绕处有个极小的相位跳变（一次轻微咔哒声），换成对讲测试信号完全够用。
        const double phaseTime = std::fmod(t, kSweepPeriodSec);
        const double slope = (kSweepHighHz - kSweepLowHz) / kSweepPeriodSec;
        const double phase = 2.0 * kPi
            * (kSweepLowHz * phaseTime + 0.5 * slope * phaseTime * phaseTime);
        return static_cast<qint16>(kToneAmplitude * std::sin(phase));
    }
    case ToneKind::Noise: {
        // 用采样序号做哈希而不是 QRandomGenerator：packet(index) 必须可重放，
        // 否则同一帧重传两次内容不同，抓包对不上。
        quint32 h = static_cast<quint32>(n) * 1664525u + 1013904223u;
        h ^= h >> 16;
        h *= 2246822519u;
        h ^= h >> 13;
        const int centered = static_cast<int>(h >> 16) - 32768;
        return static_cast<qint16>(centered / 2);
    }
    }
    return 0;
}

// ---- ADTS ---------------------------------------------------------------

const int kAdtsSampleRates[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000, 7350, 0, 0, 0
};

struct AdtsStream {
    QVector<QByteArray> frames;      // 每项是一个不带 ADTS 头的 AU
    int sampleRate = 0;
    int channels = 0;
    QByteArray config;               // AudioSpecificConfig 的十六进制串
};

AdtsStream parseAdts(const QByteArray &data)
{
    AdtsStream out;
    int pos = 0;
    while (pos + 7 <= data.size()) {
        const quint8 *p = reinterpret_cast<const quint8 *>(data.constData()) + pos;
        if (p[0] != 0xFF || (p[1] & 0xF0) != 0xF0)
            break;                   // 同步字丢了就停，别拿错位的数据往下猜
        const bool protectionAbsent = (p[1] & 0x01) != 0;
        const int objectType = ((p[2] >> 6) & 0x03) + 1;
        const int freqIndex = (p[2] >> 2) & 0x0F;
        const int channels = ((p[2] & 0x01) << 2) | ((p[3] >> 6) & 0x03);
        const int frameLength = ((p[3] & 0x03) << 11) | (p[4] << 3) | ((p[5] >> 5) & 0x07);
        const int headerLength = protectionAbsent ? 7 : 9;
        if (frameLength <= headerLength || pos + frameLength > data.size())
            break;

        out.frames.append(data.mid(pos + headerLength, frameLength - headerLength));
        if (out.sampleRate == 0) {
            out.sampleRate = kAdtsSampleRates[freqIndex];
            out.channels = channels;
            // AudioSpecificConfig：objectType(5) + freqIndex(4) + channelConfig(4) + 3 位补零。
            // SDP 的 config= 要的是十六进制串，这里直接生成好，省得每次组 SDP 再算一遍。
            const quint32 asc = (static_cast<quint32>(objectType) << 11)
                | (static_cast<quint32>(freqIndex) << 7)
                | (static_cast<quint32>(channels) << 3);
            QByteArray raw;
            raw.append(static_cast<char>((asc >> 8) & 0xFF));
            raw.append(static_cast<char>(asc & 0xFF));
            out.config = raw.toHex();
        }
        pos += frameLength;
    }
    return out;
}

void appendLe16(QByteArray &out, qint16 value)
{
    char buf[2];
    qToLittleEndian<qint16>(value, buf);
    out.append(buf, 2);
}

} // namespace

// ---- Private ------------------------------------------------------------

struct AudioSource::Private {
    AudioCodec codec = AudioCodec::None;
    ToneKind tone = ToneKind::Sine440;
    int sampleRate = 0;
    int samplesPerPacket = 0;
    int packetDurationUs = 0;
    QVector<QByteArray> aacFrames;
    QByteArray aacConfig;
};

AudioSource::AudioSource()
    : d(new Private)
{
}

AudioSource::~AudioSource()
{
    delete d;
}

bool AudioSource::configure(AudioCodec codec, ToneKind tone, QString *errorOut)
{
    d->aacFrames.clear();
    d->aacConfig.clear();
    d->codec = codec;
    d->tone = tone;
    d->sampleRate = codec::audioSampleRate(codec);

    switch (codec) {
    case AudioCodec::None:
        d->samplesPerPacket = 0;
        d->packetDurationUs = 0;
        return true;
    case AudioCodec::PCMU:
    case AudioCodec::PCMA:
    case AudioCodec::G722:
        // 采样数按真实采样率算：G.722 是 16 kHz，20 ms 就是 320 个采样
        // （但只压成 160 字节，RTP 时间戳也只加 160 —— 时钟率写的是 8000）。
        d->samplesPerPacket = d->sampleRate * kPacketMs / 1000;
        d->packetDurationUs = kPacketMs * 1000;
        return true;
    case AudioCodec::AAC:
        break;
    }

    onvifsimInitMediaResources();
    QFile file(QString::fromLatin1(kAacResource));
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut)
            *errorOut = QStringLiteral("打不开内嵌 AAC 样片 %1：%2")
                            .arg(QString::fromLatin1(kAacResource), file.errorString());
        d->codec = AudioCodec::None;
        return false;
    }
    const AdtsStream stream = parseAdts(file.readAll());
    file.close();
    if (stream.frames.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("内嵌 AAC 样片里没解出 ADTS 帧");
        d->codec = AudioCodec::None;
        return false;
    }

    d->aacFrames = stream.frames;
    d->aacConfig = stream.config;
    d->sampleRate = stream.sampleRate > 0 ? stream.sampleRate : codec::audioSampleRate(codec);
    d->samplesPerPacket = kAacSamplesPerFrame;
    d->packetDurationUs = d->sampleRate > 0
        ? static_cast<int>(std::llround(kAacSamplesPerFrame * 1000000.0 / d->sampleRate))
        : 0;
    return true;
}

AudioCodec AudioSource::codec() const
{
    return d->codec;
}

ToneKind AudioSource::tone() const
{
    return d->tone;
}

void AudioSource::setTone(ToneKind tone)
{
    d->tone = tone;
}

int AudioSource::sampleRate() const
{
    return d->sampleRate;
}

int AudioSource::samplesPerPacket() const
{
    return d->samplesPerPacket;
}

int AudioSource::packetDurationUs() const
{
    return d->packetDurationUs;
}

QByteArray AudioSource::packet(int index) const
{
    if (d->codec == AudioCodec::AAC) {
        if (d->aacFrames.isEmpty())
            return QByteArray();
        const int count = static_cast<int>(d->aacFrames.size());
        int wrapped = index % count;
        if (wrapped < 0)
            wrapped += count;
        return d->aacFrames.at(wrapped);
    }
    if (d->samplesPerPacket <= 0)
        return QByteArray();

    // 起始采样号由包序号推出来，正弦相位因此跨包连续，循环点也不会有咔哒。
    const qint64 first = static_cast<qint64>(index) * d->samplesPerPacket;
    QByteArray out;
    switch (d->codec) {
    case AudioCodec::PCMU:
        out.reserve(d->samplesPerPacket);
        for (int i = 0; i < d->samplesPerPacket; ++i)
            out.append(static_cast<char>(linearToUlaw(synthSample(d->tone, first + i, d->sampleRate))));
        break;
    case AudioCodec::PCMA:
        out.reserve(d->samplesPerPacket);
        for (int i = 0; i < d->samplesPerPacket; ++i)
            out.append(static_cast<char>(linearToAlaw(synthSample(d->tone, first + i, d->sampleRate))));
        break;
    case AudioCodec::G722:
        out.reserve(d->samplesPerPacket / 2);
        for (int i = 0; i + 1 < d->samplesPerPacket; i += 2) {
            out.append(static_cast<char>(encodeG722Pair(
                synthSample(d->tone, first + i, d->sampleRate),
                synthSample(d->tone, first + i + 1, d->sampleRate))));
        }
        break;
    case AudioCodec::None:
    case AudioCodec::AAC:
        break;
    }
    return out;
}

QByteArray AudioSource::aacAuHeader(int auSize)
{
    // RFC 3640 的 mpeg4-generic，参数固定 sizelength=13;indexlength=3;indexdeltalength=3：
    //   2 字节 AU-headers-length（单位是 bit，不是字节 —— 这里最容易写错）
    //   + 1 个 AU header = 13 bit AU-size + 3 bit AU-index，正好 16 bit
    // 一个 RTP 包只放一个 AU，所以 index 恒为 0。
    QByteArray out;
    if (auSize < 0 || auSize > 0x1FFF)
        return out;
    const quint32 header = (static_cast<quint32>(auSize) << 3);
    out.reserve(4);
    out.append(static_cast<char>(0x00));
    out.append(static_cast<char>(0x10));     // 16 bit
    out.append(static_cast<char>((header >> 8) & 0xFF));
    out.append(static_cast<char>(header & 0xFF));
    return out;
}

QByteArray AudioSource::aacConfig() const
{
    return d->aacConfig;
}

double AudioSource::levelOf(const QByteArray &payload, AudioCodec codec)
{
    const QByteArray pcm = decodeToPcm16(payload, codec);
    if (pcm.size() < 2)
        return 0.0;

    // 用 RMS 而不是峰值：电平表要的是响度感受，峰值会被单个爆音钉在满格。
    const int count = pcm.size() / 2;
    double sum = 0.0;
    for (int i = 0; i < count; ++i) {
        const qint16 sample = qFromLittleEndian<qint16>(pcm.constData() + i * 2);
        sum += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return qBound(0.0, std::sqrt(sum / count) / 32768.0, 1.0);
}

QByteArray AudioSource::decodeToPcm16(const QByteArray &payload, AudioCodec codec)
{
    QByteArray out;
    switch (codec) {
    case AudioCodec::PCMU:
        out.reserve(payload.size() * 2);
        for (int i = 0; i < payload.size(); ++i)
            appendLe16(out, ulawToLinear(static_cast<quint8>(payload.at(i))));
        break;
    case AudioCodec::PCMA:
        out.reserve(payload.size() * 2);
        for (int i = 0; i < payload.size(); ++i)
            appendLe16(out, alawToLinear(static_cast<quint8>(payload.at(i))));
        break;
    case AudioCodec::G722:
        out.reserve(payload.size() * 4);        // 一字节还原成两个 16 kHz 采样
        for (int i = 0; i < payload.size(); ++i) {
            qint16 a = 0;
            qint16 b = 0;
            decodeG722Byte(static_cast<quint8>(payload.at(i)), &a, &b);
            appendLe16(out, a);
            appendLe16(out, b);
        }
        break;
    case AudioCodec::AAC:
        // 没有 AAC 解码器，也不打算引一个（零第三方库）。对讲用 AAC 的客户端极少见，
        // 真碰上了只统计包数与丢包，电平表停在 0。
        break;
    case AudioCodec::None:
        break;
    }
    return out;
}

} // namespace onvifsim
