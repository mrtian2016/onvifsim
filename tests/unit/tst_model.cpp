// CameraModel / Scenario 的 JSON 往返。
//
// 「存一份场景，再读回来」必须得到同一台相机。这条不成立的时候没有任何现象：
// 程序照常跑、客户端照常连，只是某些字段悄悄回到了默认值 —— 真踩过的是
// profileToJson() 整段丢掉 videoSource，于是一台 4K 相机存盘再加载之后
// VideoSource 的 Bounds 退回 1920×1080，而它会被写进 GetVideoSourceConfigurations
// 的响应里。所以这里逐字段比，不抽样。

#include "core/CameraModel.h"
#include "core/Scenario.h"

#include <QtTest/QtTest>

using namespace onvifsim;

class TestModel : public QObject
{
    Q_OBJECT

private slots:
    void defaultModelRoundTrips();
    void customisedModelRoundTrips();
    void scenarioRoundTrips();
    void portsOutOfRangeAreRejected();

private:
    static void compareProfiles(const MediaProfile &a, const MediaProfile &b);
};

void TestModel::compareProfiles(const MediaProfile &a, const MediaProfile &b)
{
    QCOMPARE(a.token, b.token);
    QCOMPARE(a.name, b.name);
    QCOMPARE(a.fixed, b.fixed);

    QCOMPARE(a.videoSource.token, b.videoSource.token);
    QCOMPARE(a.videoSource.sourceToken, b.videoSource.sourceToken);
    QCOMPARE(a.videoSource.useCount, b.videoSource.useCount);
    QCOMPARE(a.videoSource.boundsWidth, b.videoSource.boundsWidth);
    QCOMPARE(a.videoSource.boundsHeight, b.videoSource.boundsHeight);

    QCOMPARE(a.videoEncoder.width, b.videoEncoder.width);
    QCOMPARE(a.videoEncoder.height, b.videoEncoder.height);
    QCOMPARE(a.videoEncoder.encoding, b.videoEncoder.encoding);
    QCOMPARE(a.videoEncoder.bitrateKbps, b.videoEncoder.bitrateKbps);
    QCOMPARE(a.videoEncoder.govLength, b.videoEncoder.govLength);
    QCOMPARE(a.videoEncoder.sessionTimeoutSec, b.videoEncoder.sessionTimeoutSec);

    QCOMPARE(a.hasAudio, b.hasAudio);
    QCOMPARE(a.audioSource.sourceToken, b.audioSource.sourceToken);
    QCOMPARE(a.audioEncoder.encoding, b.audioEncoder.encoding);

    QCOMPARE(a.hasPtz, b.hasPtz);
    QCOMPARE(a.ptzConfigToken, b.ptzConfigToken);

    QCOMPARE(a.hasAudioOutput, b.hasAudioOutput);
    QCOMPARE(a.audioOutput.sendPrimacy, b.audioOutput.sendPrimacy);
    QCOMPARE(a.audioOutput.outputLevel, b.audioOutput.outputLevel);
    QCOMPARE(a.hasAudioDecoder, b.hasAudioDecoder);
    QCOMPARE(a.audioDecoder.token, b.audioDecoder.token);

    QCOMPARE(a.hasMetadata, b.hasMetadata);
    QCOMPARE(a.metadata.analytics, b.metadata.analytics);
    QCOMPARE(a.metadata.events, b.metadata.events);
    QCOMPARE(a.metadata.ptzStatus, b.metadata.ptzStatus);

    QCOMPARE(a.streamPath, b.streamPath);
    QCOMPARE(a.mediaAsset, b.mediaAsset);
}

void TestModel::defaultModelRoundTrips()
{
    const CameraModel original = CameraModel::makeDefault(QStringLiteral("cam1"),
                                                          QStringLiteral("hikvision"), 0);
    QStringList errors;
    const CameraModel restored = CameraModel::fromJson(original.toJson(), &errors);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QStringLiteral("; "))));

    QCOMPARE(restored.id, original.id);
    QCOMPARE(restored.personaKey, original.personaKey);
    QCOMPARE(restored.httpPort, original.httpPort);
    QCOMPARE(restored.rtspPort, original.rtspPort);
    QCOMPARE(restored.profiles.size(), original.profiles.size());
    for (int i = 0; i < original.profiles.size(); ++i)
        compareProfiles(restored.profiles.at(i), original.profiles.at(i));
}

void TestModel::customisedModelRoundTrips()
{
    // 每个「曾经丢过」的字段都改成非默认值，默认值相同会让丢字段的 bug 逃掉。
    CameraModel original = CameraModel::makeDefault(QStringLiteral("cam9"),
                                                    QStringLiteral("generic"), 3);
    QVERIFY(!original.profiles.isEmpty());
    MediaProfile &p = original.profiles.first();
    p.videoSource.sourceToken = QStringLiteral("VideoSource_4K");
    p.videoSource.useCount = 7;
    p.videoSource.boundsWidth = 3840;
    p.videoSource.boundsHeight = 2160;
    p.videoEncoder.sessionTimeoutSec = 123;
    p.audioSource.sourceToken = QStringLiteral("AudioSource_9");
    p.hasAudioOutput = true;
    p.audioOutput.sendPrimacy = QStringLiteral("www.onvif.org/ver20/HalfDuplex/Client");
    p.audioOutput.outputLevel = 77;
    p.hasAudioDecoder = true;
    p.audioDecoder.token = QStringLiteral("AudioDecoder_9");
    p.metadata.analytics = false;
    p.metadata.ptzStatus = false;

    original.ptzNode.nodeName = QStringLiteral("云台节点");
    original.ptzNode.configName = QStringLiteral("云台配置");
    original.imaging.brightness = false;
    original.imaging.irCutFilter = false;
    original.imaging.focus = true;

    QStringList errors;
    const CameraModel restored = CameraModel::fromJson(original.toJson(), &errors);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QStringLiteral("; "))));

    compareProfiles(restored.profiles.first(), original.profiles.first());
    QCOMPARE(restored.ptzNode.nodeName, original.ptzNode.nodeName);
    QCOMPARE(restored.ptzNode.configName, original.ptzNode.configName);
    QCOMPARE(restored.imaging.brightness, original.imaging.brightness);
    QCOMPARE(restored.imaging.irCutFilter, original.imaging.irCutFilter);
    QCOMPARE(restored.imaging.focus, original.imaging.focus);
}

void TestModel::scenarioRoundTrips()
{
    Scenario original;
    original.name = QStringLiteral("round-trip");
    original.config.httpBasePort = 18000;
    original.config.rtspBasePort = 18554;

    ScenarioCamera sc;
    sc.model = CameraModel::makeDefault(QStringLiteral("cam1"), QStringLiteral("dahua"), 0);
    sc.model.profiles.first().videoSource.boundsWidth = 2560;
    sc.quirks.setEnabled(QuirkId::PtzConfigOnSubOnly, true);
    original.cameras.append(sc);

    QStringList errors;
    const Scenario restored = Scenario::fromJson(original.toJson(), &errors);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QStringLiteral("; "))));

    QCOMPARE(restored.config.httpBasePort, original.config.httpBasePort);
    QCOMPARE(restored.config.rtspBasePort, original.config.rtspBasePort);
    QCOMPARE(restored.cameras.size(), 1);
    compareProfiles(restored.cameras.first().model.profiles.first(),
                    original.cameras.first().model.profiles.first());
    QVERIFY(restored.cameras.first().quirks.isEnabled(QuirkId::PtzConfigOnSubOnly));
}

void TestModel::portsOutOfRangeAreRejected()
{
    // 越界端口以前是 static_cast<quint16> 静默截断的：99999 变成 33903，
    // 相机起在一个谁也想不到的端口上，errors 里一个字都没有。
    QJsonObject obj = CameraModel::makeDefault(QStringLiteral("cam1"),
                                               QStringLiteral("generic"), 0).toJson();
    QJsonObject network = obj.value(QStringLiteral("network")).toObject();
    network.insert(QStringLiteral("httpPort"), 99999);
    obj.insert(QStringLiteral("network"), network);

    QStringList errors;
    const CameraModel restored = CameraModel::fromJson(obj, &errors);
    QVERIFY2(!errors.isEmpty(), "越界端口必须报错，不能静默截断");
    QVERIFY(restored.httpPort != 33903);
}

QTEST_MAIN(TestModel)
#include "tst_model.moc"
