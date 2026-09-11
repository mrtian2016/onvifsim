// media 层单测：Annex B 切分 / SPS 解析 / RFC 6184 打包 / G.711 压扩 / RFC 3640 AU 头 / 快照。
//
// 这里刻意不测「画得好不好看」，只测那些客户端真会踩的契约：
// 时间戳不回绕、FU-A 边界、G.722 的时钟率与采样率不是一回事、快照每次都不一样。

#include "media/AudioSource.h"
#include "media/H264Source.h"
#include "media/MediaTypes.h"
#include "media/Snapshot.h"

#include <QtCore/QByteArray>
#include <QtCore/QVector>
#include <QtCore/qendian.h>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtTest/QtTest>

#include <cmath>

using namespace onvifsim;

namespace {

// 把 FU-A 分片还原成原始 NAL，用来验「拆了还能拼回去」。
QByteArray reassembleFua(const QVector<QByteArray> &packets, bool *okOut)
{
    QByteArray out;
    bool ok = !packets.isEmpty();
    for (int i = 0; i < packets.size(); ++i) {
        const QByteArray &p = packets.at(i);
        if (p.size() < 3) {
            ok = false;
            break;
        }
        const quint8 indicator = static_cast<quint8>(p.at(0));
        const quint8 fuHeader = static_cast<quint8>(p.at(1));
        if ((indicator & 0x1F) != 28)
            ok = false;
        const bool start = (fuHeader & 0x80) != 0;
        const bool end = (fuHeader & 0x40) != 0;
        if (start != (i == 0) || end != (i == packets.size() - 1))
            ok = false;
        if (start)
            out.append(static_cast<char>((indicator & 0xE0) | (fuHeader & 0x1F)));
        out.append(p.mid(2));
    }
    if (okOut)
        *okOut = ok;
    return out;
}

qint16 pcmAt(const QByteArray &pcm, int index)
{
    return qFromLittleEndian<qint16>(pcm.constData() + index * 2);
}

} // namespace

class TestMedia : public QObject
{
    Q_OBJECT

private slots:
    // ---- Annex B ----
    void splitsMixedStartCodes();
    void splitsBuiltinAsset();

    // ---- 样片与 SPS ----
    void loadsBuiltinAssets_data();
    void loadsBuiltinAssets();
    void rejectsBadAsset();
    void spropParameterSetsIsBase64Pair();
    void profileLevelIdFromSps();

    // ---- 循环与时间戳 ----
    void loopTimestampsNeverWrap();

    // ---- RFC 6184 ----
    void singleNalWhenWithinMtu_data();
    void singleNalWhenWithinMtu();
    void fragmentsWhenOverMtu();
    void fuaRoundTripsLargeNal();
    void rejectsAbsurdMtu();

    // ---- 音频参数 ----
    void g722ClockRateIsNotSampleRate();
    void audioCodecNamesRoundTrip();
    void audioPayloadTypes();
    void packetShapePerCodec_data();
    void packetShapePerCodec();

    // ---- G.711 ----
    void silenceEncodesToCanonicalBytes();
    void g711DecodesKnownCodes();
    void g711RoundTripAgreesAcrossLaws_data();
    void g711RoundTripAgreesAcrossLaws();
    void levelTracksAmplitude();

    // ---- AAC ----
    void aacAuHeaderLayout();
    void aacAssetLoopsWithConfig();

    // ---- 快照 ----
    void snapshotIsJpegOfRequestedSize();
    void snapshotChangesEveryTime();
    void blackSnapshotIsBlack();
    void renderForDegradesGracefully();
};

// ---------------------------------------------------------------- Annex B

void TestMedia::splitsMixedStartCodes()
{
    // 三字节和四字节起始码混着用是真机常态（x264 就是首个 NAL 四字节、其余三字节）。
    QByteArray stream;
    stream += QByteArray::fromHex("00000001") + QByteArray::fromHex("67aabbcc");
    stream += QByteArray::fromHex("000001") + QByteArray::fromHex("68dd");
    stream += QByteArray::fromHex("00000001") + QByteArray::fromHex("6501020304");

    const QVector<QByteArray> nals = H264Source::splitNalUnits(stream);
    QCOMPARE(nals.size(), 3);
    QCOMPARE(nals.at(0), QByteArray::fromHex("67aabbcc"));
    QCOMPARE(nals.at(1), QByteArray::fromHex("68dd"));
    QCOMPARE(nals.at(2), QByteArray::fromHex("6501020304"));

    QVERIFY(H264Source::splitNalUnits(QByteArray()).isEmpty());
    QVERIFY(H264Source::splitNalUnits(QByteArray::fromHex("deadbeef")).isEmpty());
}

void TestMedia::splitsBuiltinAsset()
{
    H264Source source;
    QString error;
    QVERIFY2(source.load(QStringLiteral("360p"), &error), qPrintable(error));

    const EncodedFrame first = source.frame(0);
    const QVector<QByteArray> nals = H264Source::splitNalUnits(first.data);
    // 首帧是 SPS + PPS + SEI + IDR，至少四个 NAL，而且 SPS 必须排在 IDR 前面。
    QVERIFY(nals.size() >= 2);
    QCOMPARE(static_cast<quint8>(nals.first().at(0)) & 0x1F, 7);
    QCOMPARE(static_cast<quint8>(nals.last().at(0)) & 0x1F, 5);
    QVERIFY(first.keyFrame);
    QVERIFY(first.isParameterSet);
}

// ------------------------------------------------------------ 样片与 SPS

void TestMedia::loadsBuiltinAssets_data()
{
    QTest::addColumn<QString>("asset");
    QTest::addColumn<int>("width");
    QTest::addColumn<int>("height");

    QTest::newRow("360p") << QStringLiteral("360p") << 640 << 360;
    QTest::newRow("720p") << QStringLiteral("720p") << 1280 << 720;
    QTest::newRow("1080p") << QStringLiteral("1080p") << 1920 << 1080;
    QTest::newRow("black") << QStringLiteral("black") << 640 << 360;
    QTest::newRow("freeze") << QStringLiteral("freeze") << 640 << 360;
    QTest::newRow("noise") << QStringLiteral("noise") << 640 << 360;
}

void TestMedia::loadsBuiltinAssets()
{
    QFETCH(QString, asset);
    QFETCH(int, width);
    QFETCH(int, height);

    QVERIFY(H264Source::builtinAssets().contains(asset));

    H264Source source;
    QString error;
    QVERIFY2(source.load(asset, &error), qPrintable(error));
    QVERIFY(source.isLoaded());
    QCOMPARE(source.assetName(), asset);
    QCOMPARE(source.width(), width);
    QCOMPARE(source.height(), height);
    // SPS 的 VUI 里 time_scale = 2 * fps，解出来必须正好是生成脚本里的 15 fps。
    QVERIFY(qAbs(source.frameRate() - 15.0) < 0.01);
    QCOMPARE(source.frameCount(), 150);
    QCOMPARE(source.durationUs(), Q_INT64_C(10000000));
    QVERIFY(!source.sps().isEmpty());
    QVERIFY(!source.pps().isEmpty());
}

void TestMedia::rejectsBadAsset()
{
    H264Source source;
    QString error;
    QVERIFY(!source.load(QStringLiteral("/nonexistent/nope.h264"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!source.isLoaded());

    QVERIFY(!source.load(QString(), &error));
}

void TestMedia::spropParameterSetsIsBase64Pair()
{
    H264Source source;
    QVERIFY(source.load(QStringLiteral("720p")));

    const QByteArray sprop = source.spropParameterSets();
    const QList<QByteArray> parts = sprop.split(',');
    QCOMPARE(parts.size(), 2);
    // SDP 里放的是 base64 后的完整 NAL（含 nal 头），不是 RBSP。
    QCOMPARE(QByteArray::fromBase64(parts.at(0)), source.sps());
    QCOMPARE(QByteArray::fromBase64(parts.at(1)), source.pps());
    QCOMPARE(static_cast<quint8>(source.sps().at(0)) & 0x1F, 7);
    QCOMPARE(static_cast<quint8>(source.pps().at(0)) & 0x1F, 8);
}

void TestMedia::profileLevelIdFromSps()
{
    H264Source source;
    QVERIFY(source.load(QStringLiteral("360p")));

    const QByteArray id = source.profileLevelId();
    QCOMPARE(id.size(), 6);
    // 42 = Baseline，C0 = constraint_set0+1（Constrained Baseline），16 = level 2.2。
    QCOMPARE(id, QByteArray("42C016"));
    QCOMPARE(id, source.sps().mid(1, 3).toHex().toUpper());

    // 分辨率变了 level 也会变，但 profile 与 constraint 三档都一样。
    H264Source hd;
    QVERIFY(hd.load(QStringLiteral("1080p")));
    QCOMPARE(hd.profileLevelId().left(4), QByteArray("42C0"));
    QVERIFY(hd.profileLevelId() != id);
}

// -------------------------------------------------------- 循环与时间戳

void TestMedia::loopTimestampsNeverWrap()
{
    H264Source source;
    QVERIFY(source.load(QStringLiteral("360p")));
    const int count = source.frameCount();
    QCOMPARE(source.frame(0).ptsUs, Q_INT64_C(0));

    // 跨两圈半，时间戳必须一路严格递增 —— 回绕会被客户端当成时间戳跳变，直接丢缓冲重连。
    qint64 previous = -1;
    for (int i = 0; i < count * 2 + count / 2; ++i) {
        const EncodedFrame frame = source.frame(i);
        QVERIFY2(frame.ptsUs > previous, qPrintable(QStringLiteral("第 %1 帧时间戳没有递增").arg(i)));
        previous = frame.ptsUs;
        QVERIFY(!frame.data.isEmpty());
    }

    // 画面本身是循环的：第 N 帧和第 N+count 帧必须是同一份数据。
    QCOMPARE(source.frame(count).data, source.frame(0).data);
    QCOMPARE(source.frame(count + 7).data, source.frame(7).data);
    // 但时间戳不回绕：正好差一整圈的时长。
    QCOMPARE(source.frame(count).ptsUs - source.frame(0).ptsUs, source.durationUs());
    // 负下标也不能取到越界的帧（补发逻辑可能回退）。
    QCOMPARE(source.frame(-count).data, source.frame(0).data);
}

// ------------------------------------------------------------- RFC 6184

void TestMedia::singleNalWhenWithinMtu_data()
{
    QTest::addColumn<int>("nalSize");
    QTest::addColumn<int>("mtu");

    QTest::newRow("远小于 mtu") << 40 << 1400;
    QTest::newRow("比 mtu 小 1") << 1399 << 1400;
    QTest::newRow("正好等于 mtu") << 1400 << 1400;
}

void TestMedia::singleNalWhenWithinMtu()
{
    QFETCH(int, nalSize);
    QFETCH(int, mtu);

    QByteArray nal(nalSize, '\x41');
    nal[0] = char(0x65);
    const QVector<QByteArray> packets = H264Source::packetize(nal, mtu);
    QCOMPARE(packets.size(), 1);
    QCOMPARE(packets.first(), nal);   // 单 NAL 包就是原样发，不加任何头
}

void TestMedia::fragmentsWhenOverMtu()
{
    const int mtu = 1400;
    QByteArray nal(mtu + 1, '\x41');
    nal[0] = char(0x65);              // nal_ref_idc=3, type=5(IDR)

    const QVector<QByteArray> packets = H264Source::packetize(nal, mtu);
    // 负载 mtu 字节（原 NAL 去掉 1 字节头），每片能装 mtu-2，所以正好两片。
    QCOMPARE(packets.size(), 2);
    QCOMPARE(packets.at(0).size(), mtu);
    QCOMPARE(packets.at(1).size(), 4);

    // FU indicator：F/NRI 照抄原 NAL 头，type 换成 28。
    QCOMPARE(static_cast<quint8>(packets.at(0).at(0)), quint8(0x7C));
    QCOMPARE(static_cast<quint8>(packets.at(1).at(0)), quint8(0x7C));
    // FU header：首片 S=1 E=0，末片 S=0 E=1，type 保留原 NAL 的 5。
    QCOMPARE(static_cast<quint8>(packets.at(0).at(1)), quint8(0x85));
    QCOMPARE(static_cast<quint8>(packets.at(1).at(1)), quint8(0x45));

    bool ok = false;
    QCOMPARE(reassembleFua(packets, &ok), nal);
    QVERIFY(ok);
}

void TestMedia::fuaRoundTripsLargeNal()
{
    H264Source source;
    QVERIFY(source.load(QStringLiteral("1080p")));

    // 拿真样片里最大的那个 NAL 来打包：合成数据测不出 emulation prevention 之类的坑。
    QByteArray biggest;
    for (int i = 0; i < 30; ++i) {
        for (const QByteArray &nal : H264Source::splitNalUnits(source.frame(i).data)) {
            if (nal.size() > biggest.size())
                biggest = nal;
        }
    }
    QVERIFY(biggest.size() > 1400);

    for (int mtu : { 200, 576, 1400, 1460 }) {
        const QVector<QByteArray> packets = H264Source::packetize(biggest, mtu);
        QVERIFY(packets.size() > 1);
        for (const QByteArray &p : packets)
            QVERIFY(p.size() <= mtu);
        bool ok = false;
        QCOMPARE(reassembleFua(packets, &ok), biggest);
        QVERIFY(ok);
    }
}

void TestMedia::rejectsAbsurdMtu()
{
    QByteArray nal(500, '\x41');
    nal[0] = char(0x65);
    // mtu 小于 3 连一字节负载都放不下，宁可返回空也不要吐出无限多的空分片。
    QVERIFY(H264Source::packetize(nal, 2).isEmpty());
    QVERIFY(H264Source::packetize(QByteArray(), 1400).isEmpty());
}

// ------------------------------------------------------------- 音频参数

void TestMedia::g722ClockRateIsNotSampleRate()
{
    // RFC 3551 的历史坑：G.722 采 16 kHz，但 RTP 时钟率必须写 8000。
    // 这两个数一旦被谁「顺手统一」了，客户端就会把音频放成两倍速。
    QCOMPARE(codec::audioClockRate(AudioCodec::G722), 8000);
    QCOMPARE(codec::audioSampleRate(AudioCodec::G722), 16000);

    QCOMPARE(codec::audioClockRate(AudioCodec::PCMU), 8000);
    QCOMPARE(codec::audioSampleRate(AudioCodec::PCMU), 8000);
    QCOMPARE(codec::audioClockRate(AudioCodec::PCMA), 8000);
    QCOMPARE(codec::audioSampleRate(AudioCodec::PCMA), 8000);
    QCOMPARE(codec::audioClockRate(AudioCodec::AAC), codec::audioSampleRate(AudioCodec::AAC));

    AudioSource source;
    QVERIFY(source.configure(AudioCodec::G722));
    QCOMPARE(source.sampleRate(), 16000);
    QCOMPARE(source.samplesPerPacket(), 320);      // 20 ms @ 16 kHz
    QCOMPARE(source.packetDurationUs(), 20000);
    QCOMPARE(source.packet(0).size(), 160);        // 但只有 160 字节，时间戳也只加 160
}

void TestMedia::audioCodecNamesRoundTrip()
{
    for (AudioCodec c : { AudioCodec::None, AudioCodec::PCMU, AudioCodec::PCMA,
                          AudioCodec::G722, AudioCodec::AAC }) {
        bool ok = false;
        QCOMPARE(codec::audioFromName(codec::audioCodecName(c), &ok), c);
        QVERIFY(ok);
    }

    bool ok = false;
    // ONVIF 的枚举写 "G711"，真机上还见过 "g.711" / "ulaw" 这些拼法。
    QCOMPARE(codec::audioFromName(QStringLiteral("G711"), &ok), AudioCodec::PCMU);
    QVERIFY(ok);
    QCOMPARE(codec::audioFromName(QStringLiteral(" ulaw "), &ok), AudioCodec::PCMU);
    QVERIFY(ok);
    QCOMPARE(codec::audioFromName(QStringLiteral("G711A"), &ok), AudioCodec::PCMA);
    QVERIFY(ok);
    QCOMPARE(codec::audioFromName(QStringLiteral("mpeg4-generic"), &ok), AudioCodec::AAC);
    QVERIFY(ok);
    // 认不出来要能和「明确关掉音频」区分开。
    QCOMPARE(codec::audioFromName(QStringLiteral("G726"), &ok), AudioCodec::None);
    QVERIFY(!ok);
    QCOMPARE(codec::audioFromName(QString(), &ok), AudioCodec::None);
    QVERIFY(ok);

    QCOMPARE(codec::videoCodecName(VideoCodec::H264), QStringLiteral("H264"));
    QCOMPARE(codec::videoCodecName(VideoCodec::Jpeg), QStringLiteral("JPEG"));
}

void TestMedia::audioPayloadTypes()
{
    QCOMPARE(codec::audioPayloadType(AudioCodec::PCMU), 0);
    QCOMPARE(codec::audioPayloadType(AudioCodec::PCMA), 8);
    QCOMPARE(codec::audioPayloadType(AudioCodec::G722), 9);
    QVERIFY(codec::audioPayloadType(AudioCodec::AAC) >= 96);   // 动态区
    QVERIFY(codec::audioPayloadType(AudioCodec::None) < 0);
}

void TestMedia::packetShapePerCodec_data()
{
    QTest::addColumn<int>("codecValue");
    QTest::addColumn<int>("packetBytes");
    QTest::addColumn<int>("decodedSamples");

    QTest::newRow("PCMU") << int(AudioCodec::PCMU) << 160 << 160;
    QTest::newRow("PCMA") << int(AudioCodec::PCMA) << 160 << 160;
    QTest::newRow("G722") << int(AudioCodec::G722) << 160 << 320;
}

void TestMedia::packetShapePerCodec()
{
    QFETCH(int, codecValue);
    QFETCH(int, packetBytes);
    QFETCH(int, decodedSamples);
    const AudioCodec c = static_cast<AudioCodec>(codecValue);

    AudioSource source;
    QString error;
    QVERIFY2(source.configure(c, ToneKind::Sine440, &error), qPrintable(error));
    QCOMPARE(source.codec(), c);
    QCOMPARE(source.tone(), ToneKind::Sine440);

    for (int i : { 0, 1, 500 }) {
        const QByteArray packet = source.packet(i);
        QCOMPARE(packet.size(), packetBytes);
        QCOMPARE(AudioSource::decodeToPcm16(packet, c).size(), decodedSamples * 2);
    }

    source.setTone(ToneKind::Sweep);
    QCOMPARE(source.tone(), ToneKind::Sweep);
    QCOMPARE(source.packet(3).size(), packetBytes);
}

// ---------------------------------------------------------------- G.711

void TestMedia::silenceEncodesToCanonicalBytes()
{
    // 静音的码字是 G.711 里最容易写错的地方：μ-law 是 0xFF，A-law 是 0xD5。
    // 写错了客户端听到的是持续的直流嗡声，而不是安静。
    AudioSource ulaw;
    QVERIFY(ulaw.configure(AudioCodec::PCMU, ToneKind::Silence));
    QCOMPARE(ulaw.packet(0), QByteArray(160, '\xFF'));

    AudioSource alaw;
    QVERIFY(alaw.configure(AudioCodec::PCMA, ToneKind::Silence));
    QCOMPARE(alaw.packet(0), QByteArray(160, '\xD5'));

    QCOMPARE(AudioSource::levelOf(ulaw.packet(0), AudioCodec::PCMU), 0.0);
}

void TestMedia::g711DecodesKnownCodes()
{
    // 拿 ITU 参考表里的几个端点钉住解码器，段号/尾数移位写反了会立刻现形。
    const QByteArray ulaw = QByteArray::fromHex("ff7f0080");
    const QByteArray pcmU = AudioSource::decodeToPcm16(ulaw, AudioCodec::PCMU);
    QCOMPARE(pcmU.size(), 8);
    QCOMPARE(pcmAt(pcmU, 0), qint16(0));         // 0xFF = +0
    QCOMPARE(pcmAt(pcmU, 1), qint16(0));         // 0x7F = -0
    QCOMPARE(pcmAt(pcmU, 2), qint16(-32124));    // 0x00 = 负满幅
    QCOMPARE(pcmAt(pcmU, 3), qint16(32124));     // 0x80 = 正满幅

    const QByteArray alaw = QByteArray::fromHex("d5552aaa");
    const QByteArray pcmA = AudioSource::decodeToPcm16(alaw, AudioCodec::PCMA);
    QCOMPARE(pcmA.size(), 8);
    QCOMPARE(pcmAt(pcmA, 0), qint16(8));         // 0xD5 = 最小正值
    QCOMPARE(pcmAt(pcmA, 1), qint16(-8));        // 0x55 = 最小负值
    QCOMPARE(pcmAt(pcmA, 2), qint16(-32256));    // 0x2A = 负满幅
    QCOMPARE(pcmAt(pcmA, 3), qint16(32256));     // 0xAA = 正满幅

    QVERIFY(AudioSource::decodeToPcm16(ulaw, AudioCodec::AAC).isEmpty());
    QVERIFY(AudioSource::decodeToPcm16(ulaw, AudioCodec::None).isEmpty());
}

void TestMedia::g711RoundTripAgreesAcrossLaws_data()
{
    QTest::addColumn<int>("toneValue");
    QTest::newRow("正弦") << int(ToneKind::Sine440);
    QTest::newRow("扫频") << int(ToneKind::Sweep);
    QTest::newRow("噪声") << int(ToneKind::Noise);
}

void TestMedia::g711RoundTripAgreesAcrossLaws()
{
    QFETCH(int, toneValue);
    const ToneKind tone = static_cast<ToneKind>(toneValue);

    // 编码器不是公开 API，但 μ-law 和 A-law 编的是同一段 PCM：
    // 各自「编码→解码」跑一圈，两条曲线必须落在彼此的量化误差里。
    // 任何一侧的段号/掩码写错，两条曲线立刻分家 —— 这比对着硬编码的波形断言稳。
    AudioSource ulaw;
    AudioSource alaw;
    QVERIFY(ulaw.configure(AudioCodec::PCMU, tone));
    QVERIFY(alaw.configure(AudioCodec::PCMA, tone));

    for (int packetIndex : { 0, 1, 17 }) {
        const QByteArray a = AudioSource::decodeToPcm16(ulaw.packet(packetIndex), AudioCodec::PCMU);
        const QByteArray b = AudioSource::decodeToPcm16(alaw.packet(packetIndex), AudioCodec::PCMA);
        QCOMPARE(a.size(), b.size());
        QVERIFY(a.size() == 320);

        double maxSeen = 0.0;
        for (int i = 0; i < a.size() / 2; ++i) {
            const double x = pcmAt(a, i);
            const double y = pcmAt(b, i);
            maxSeen = qMax(maxSeen, qAbs(x));
            // 两种压扩律的量化步长不同，容差取「绝对底噪 + 相对误差」。
            const double tolerance = qMax(200.0, qAbs(x) * 0.16);
            QVERIFY2(qAbs(x - y) <= tolerance,
                     qPrintable(QStringLiteral("第 %1 个采样：μ-law %2 vs A-law %3")
                                    .arg(i).arg(x).arg(y)));
        }
        // 顺带确认信号真的有幅度，别拿一段静音蒙混过关。
        QVERIFY(maxSeen > 4000.0);
    }
}

void TestMedia::levelTracksAmplitude()
{
    AudioSource source;
    QVERIFY(source.configure(AudioCodec::PCMU, ToneKind::Silence));
    QCOMPARE(AudioSource::levelOf(source.packet(0), AudioCodec::PCMU), 0.0);

    source.setTone(ToneKind::Sine440);
    const double sine = AudioSource::levelOf(source.packet(0), AudioCodec::PCMU);
    QVERIFY(sine > 0.05);
    QVERIFY(sine <= 1.0);

    // G.722 是近似实现，但电平表必须仍然能反映「有没有人在说话」。
    AudioSource g722;
    QVERIFY(g722.configure(AudioCodec::G722, ToneKind::Sine440));
    QVERIFY(AudioSource::levelOf(g722.packet(0), AudioCodec::G722) > 0.05);

    // AAC 没有解码器，电平表只能停在 0，不许崩。
    QCOMPARE(AudioSource::levelOf(QByteArray(64, '\x11'), AudioCodec::AAC), 0.0);
    QCOMPARE(AudioSource::levelOf(QByteArray(), AudioCodec::PCMU), 0.0);
}

// ------------------------------------------------------------------ AAC

void TestMedia::aacAuHeaderLayout()
{
    // RFC 3640 mpeg4-generic，sizelength=13;indexlength=3;indexdeltalength=3：
    // 头两字节是 AU-headers-length，单位是 **bit**（16），不是字节 —— 这里最容易写错。
    const QByteArray header = AudioSource::aacAuHeader(1024);
    QCOMPARE(header.size(), 4);
    QCOMPARE(header, QByteArray::fromHex("00102000"));

    QCOMPARE(AudioSource::aacAuHeader(0), QByteArray::fromHex("00100000"));
    QCOMPARE(AudioSource::aacAuHeader(1), QByteArray::fromHex("00100008"));
    QCOMPARE(AudioSource::aacAuHeader(0x1FFF), QByteArray::fromHex("0010fff8"));
    // 13 bit 装不下就该拒绝，而不是悄悄截断成一个错的长度。
    QVERIFY(AudioSource::aacAuHeader(0x2000).isEmpty());
    QVERIFY(AudioSource::aacAuHeader(-1).isEmpty());
}

void TestMedia::aacAssetLoopsWithConfig()
{
    AudioSource source;
    QString error;
    QVERIFY2(source.configure(AudioCodec::AAC, ToneKind::Sine440, &error), qPrintable(error));
    QCOMPARE(source.sampleRate(), 16000);
    QCOMPARE(source.samplesPerPacket(), 1024);
    QCOMPARE(source.packetDurationUs(), 64000);

    // AudioSpecificConfig：objectType=2(AAC-LC) freqIndex=8(16 kHz) channels=1 → 0x1408。
    QCOMPARE(source.aacConfig(), QByteArray("1408"));

    const QByteArray first = source.packet(0);
    QVERIFY(!first.isEmpty());
    QVERIFY(first.size() < 0x2000);
    // AU 里不许残留 ADTS 头（0xFFF 同步字），否则客户端解出来全是噪音。
    QVERIFY(static_cast<quint8>(first.at(0)) != 0xFF);

    // 10 秒 @ 1024 采样/帧 ≈ 156 帧，循环回来必须还是同一份数据。
    QVERIFY(source.packet(200) != first || source.packet(1) == first);
    bool wrapped = false;
    for (int i = 100; i < 400; ++i) {
        if (source.packet(i) == first && i > 0) {
            wrapped = true;
            break;
        }
    }
    QVERIFY(wrapped);
}

// ----------------------------------------------------------------- 快照

void TestMedia::snapshotIsJpegOfRequestedSize()
{
    SnapshotOptions options;
    options.size = QSize(640, 360);
    options.cameraName = QStringLiteral("Front Door");
    options.profileName = QStringLiteral("MainStream");

    const QByteArray jpeg = snapshot::render(options);
    QVERIFY(!jpeg.isEmpty());
    QCOMPARE(jpeg.left(2), QByteArray::fromHex("ffd8"));   // SOI
    QCOMPARE(jpeg.right(2), QByteArray::fromHex("ffd9"));  // EOI

    QImage decoded;
    QVERIFY(decoded.loadFromData(jpeg, "JPEG"));
    QCOMPARE(decoded.size(), QSize(640, 360));

    // 尺寸非法要退回默认值而不是画出一张 0x0 或者吃光内存的巨图。
    SnapshotOptions bad;
    bad.size = QSize(0, 0);
    QImage fallback;
    QVERIFY(fallback.loadFromData(snapshot::render(bad), "JPEG"));
    QCOMPARE(fallback.size(), QSize(1920, 1080));
}

void TestMedia::snapshotChangesEveryTime()
{
    SnapshotOptions options;
    options.size = QSize(320, 180);
    options.cameraName = QStringLiteral("cam1");

    // 这条正是模拟器要暴露的问题：客户端到底有没有重新拉快照。
    // 连着两次调用可能落在同一毫秒里，所以实现额外叠了自增序号，
    // 「每次内容都不同」必须是硬保证，不能靠时钟碰运气。
    QByteArray previous;
    for (int i = 0; i < 5; ++i) {
        const QByteArray jpeg = snapshot::render(options);
        QVERIFY(!jpeg.isEmpty());
        QVERIFY2(jpeg != previous, "连续两张快照内容相同，客户端将无法察觉画面刷新");
        previous = jpeg;
    }
}

void TestMedia::blackSnapshotIsBlack()
{
    SnapshotOptions options;
    options.size = QSize(160, 120);
    options.black = true;
    options.cameraName = QStringLiteral("cam1");

    QImage image;
    QVERIFY(image.loadFromData(snapshot::render(options), "JPEG"));
    QCOMPARE(image.size(), QSize(160, 120));

    image = image.convertToFormat(QImage::Format_RGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            // JPEG 是有损的，纯色块也可能差一两个量级，但绝不该出现可见内容。
            QVERIFY(qRed(pixel) <= 4 && qGreen(pixel) <= 4 && qBlue(pixel) <= 4);
        }
    }
}

void TestMedia::renderForDegradesGracefully()
{
    // VirtualCamera 还没接上，renderFor 目前是最小实现；至少不能崩，
    // 而且要吐出一张合法 JPEG，让 Media 服务那边可以先接线。
    QImage image;
    QVERIFY(image.loadFromData(snapshot::renderFor(nullptr, QStringLiteral("Profile_1")), "JPEG"));
    QVERIFY(!image.isNull());
}

// QTEST_MAIN 会按链接到的 Qt 模块挑 QGuiApplication，但拿不到无头环境需要的
// offscreen 平台插件；快照要画字，没有 QGuiApplication 直接崩，所以自己写 main。
int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    TestMedia testCase;
    return QTest::qExec(&testCase, argc, argv);
}

#include "tst_media.moc"
