// PTZ 积分模型、预置位（含出厂 300 槽）与图像参数状态的单元测试。
//
// 这里一律用 PtzState(nullptr) / ImagingState(nullptr) 直接构造：状态机对
// camera == nullptr 是容错的（退回默认 quirks 与默认 maxPresets），单测因此
// 不必先搭一台 VirtualCamera。需要开 quirk 才能观察的行为（C6 自动预填、
// C7 百分号编码）由 e2e 断言覆盖，这里只测不依赖开关的那一半。
//
// 积分类断言都用较短的时间窗 + 放宽的容差，避免 CI 上因调度抖动偶发失败。

#include "imaging/ImagingState.h"
#include "ptz/PtzState.h"

#include <QtCore/QUrl>
#include <QtTest/QtTest>

#include <cmath>

using namespace onvifsim;

namespace {

// 位置到位时是精确吸附到目标的，用一个很小的绝对容差比较即可；
// 不用 QCOMPARE 是因为它对浮点走的是模糊比较，0.0 附近的语义容易让人误判。
bool sameValue(double a, double b)
{
    return std::fabs(a - b) < 1e-9;
}

bool nearValue(double a, double b, double tolerance)
{
    return std::fabs(a - b) <= tolerance;
}

PtzVector vec(double pan, double tilt, double zoom)
{
    PtzVector v;
    v.pan = pan;
    v.tilt = tilt;
    v.zoom = zoom;
    return v;
}

PtzVector panTiltOnly(double pan, double tilt)
{
    PtzVector v = vec(pan, tilt, 0.0);
    v.hasZoom = false;
    return v;
}

PtzVector zoomOnly(double zoom)
{
    PtzVector v = vec(0.0, 0.0, zoom);
    v.hasPanTilt = false;
    return v;
}

} // namespace

#define COMPARE_POS(actual, expected)                                                              \
    QVERIFY2(sameValue((actual), (expected)),                                                      \
             qPrintable(QStringLiteral("%1 != %2").arg((actual)).arg((expected))))

#define VERIFY_NEAR(actual, expected, tol)                                                         \
    QVERIFY2(nearValue((actual), (expected), (tol)),                                               \
             qPrintable(QStringLiteral("%1 与 %2 相差超过 %3").arg((actual)).arg((expected)).arg((tol))))

class TstPtz : public QObject
{
    Q_OBJECT

private slots:
    void continuousMoveIntegratesOverTime();
    void continuousMoveStopsAtLimits();
    void continuousMoveZoomOnlyLeavesPanTiltAlone();
    void continuousMoveHonoursTimeout();
    void stopHandlesAxesSeparately();
    void absoluteMoveTakesTimeThenGoesIdle();
    void relativeMoveClampsToLimits();
    void setPositionCancelsMotion();

    void presetCrud();
    void presetOverCapacityFails();
    void factoryPresetsShareOnePosition();
    void presetNameStoredVerbatim();
    void homePositionRoundTrip();
    void auxiliaryCommandRecorded();

    void imagingIrCutFilterTristate();
    void imagingWhiteLightToggles();
    void imagingSettingsClamped();
    void imagingFocusMove();
};

void TstPtz::continuousMoveIntegratesOverTime()
{
    PtzState ptz(nullptr);
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Idle);

    int positionSignals = 0;
    connect(&ptz, &PtzState::positionChanged, this, [&positionSignals] { ++positionSignals; });
    QSignalSpy statusSpy(&ptz, &PtzState::moveStatusChanged);

    ptz.continuousMove(panTiltOnly(1.0, -0.5));
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Moving);
    QCOMPARE(statusSpy.count(), 1);

    QTest::qWait(250);
    const PtzVector first = ptz.position();
    // 满速 1.0 单位/秒，250 ms 大约走 0.25；容差放宽到 ±0.15 兜住调度抖动。
    VERIFY_NEAR(first.pan, 0.25, 0.15);
    VERIFY_NEAR(first.tilt, -0.125, 0.15);
    QVERIFY(first.pan > 0.0);

    QTest::qWait(200);
    const PtzVector second = ptz.position();
    QVERIFY2(second.pan > first.pan, "位置必须随时间继续变化");
    QVERIFY(positionSignals > 0);

    ptz.stop();
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Idle);
    const PtzVector stopped = ptz.position();
    QTest::qWait(150);
    // 停下之后再怎么等，位置都不能再动。
    COMPARE_POS(ptz.position().pan, stopped.pan);
    COMPARE_POS(ptz.position().tilt, stopped.tilt);
}

void TstPtz::continuousMoveStopsAtLimits()
{
    PtzState ptz(nullptr);
    ptz.setPosition(vec(0.9, -0.9, 0.95));

    ptz.continuousMove(vec(1.0, -1.0, 1.0));
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Moving);
    QCOMPARE(ptz.zoomStatus(), PtzMoveStatus::Moving);

    // pan/tilt 还差 0.1（满速 1.0/s），zoom 还差 0.05（满速 0.5/s），都在 100 ms 内到限位。
    QTest::qWait(500);
    const PtzVector p = ptz.position();
    COMPARE_POS(p.pan, 1.0);
    COMPARE_POS(p.tilt, -1.0);
    COMPARE_POS(p.zoom, 1.0);
    // 到限位就该停，不能越界，也不能一直报 MOVING。
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Idle);
    QCOMPARE(ptz.zoomStatus(), PtzMoveStatus::Idle);

    // 顶在限位上继续往外推：状态立刻回 Idle，位置纹丝不动。
    ptz.continuousMove(vec(1.0, -1.0, 1.0));
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Idle);
    QTest::qWait(120);
    COMPARE_POS(ptz.position().pan, 1.0);
    COMPARE_POS(ptz.position().zoom, 1.0);
}

void TstPtz::continuousMoveZoomOnlyLeavesPanTiltAlone()
{
    PtzState ptz(nullptr);
    ptz.setPosition(vec(0.25, -0.5, 0.0));

    // 客户端手写栈只发被命令的那个轴（非零 Zoom 会让某些固件吐畸形 HTTP），
    // 这时 PanTilt 一分一毫都不能动。
    ptz.continuousMove(zoomOnly(1.0));
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Idle);
    QCOMPARE(ptz.zoomStatus(), PtzMoveStatus::Moving);

    QTest::qWait(200);
    const PtzVector p = ptz.position();
    COMPARE_POS(p.pan, 0.25);
    COMPARE_POS(p.tilt, -0.5);
    QVERIFY2(p.zoom > 0.0, "只给 Zoom 时变焦必须动起来");
    VERIFY_NEAR(p.zoom, 0.1, 0.1);

    // 反过来只发 PanTilt，Zoom 保持原样继续走自己的。
    const double zoomBefore = ptz.position().zoom;
    ptz.continuousMove(panTiltOnly(-1.0, 0.0));
    QCOMPARE(ptz.zoomStatus(), PtzMoveStatus::Moving);
    QTest::qWait(150);
    QVERIFY(ptz.position().zoom > zoomBefore);
    QVERIFY(ptz.position().pan < 0.25);
}

void TstPtz::continuousMoveHonoursTimeout()
{
    PtzState ptz(nullptr);
    ptz.continuousMove(panTiltOnly(1.0, 0.0), 150);
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Moving);

    QTest::qWait(400);
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Idle);
    const double panAtTimeout = ptz.position().pan;
    // Timeout 之后的时间不能再被积分进去，否则超时形同虚设。
    VERIFY_NEAR(panAtTimeout, 0.15, 0.12);
    QTest::qWait(150);
    COMPARE_POS(ptz.position().pan, panAtTimeout);
}

void TstPtz::stopHandlesAxesSeparately()
{
    PtzState ptz(nullptr);
    ptz.continuousMove(vec(0.5, 0.5, 0.5));
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Moving);
    QCOMPARE(ptz.zoomStatus(), PtzMoveStatus::Moving);

    ptz.stop(true, false);
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Idle);
    QCOMPARE(ptz.zoomStatus(), PtzMoveStatus::Moving);

    const double panParked = ptz.position().pan;
    QTest::qWait(150);
    COMPARE_POS(ptz.position().pan, panParked);
    QVERIFY(ptz.position().zoom > 0.0);

    ptz.stop(false, true);
    QCOMPARE(ptz.zoomStatus(), PtzMoveStatus::Idle);
    const double zoomParked = ptz.position().zoom;
    QTest::qWait(150);
    COMPARE_POS(ptz.position().zoom, zoomParked);
}

void TstPtz::absoluteMoveTakesTimeThenGoesIdle()
{
    PtzState ptz(nullptr);
    QSignalSpy statusSpy(&ptz, &PtzState::moveStatusChanged);

    PtzVector target = panTiltOnly(0.5, 0.0);
    PtzVector speed = panTiltOnly(1.0, 1.0);
    ptz.absoluteMove(target, speed);

    // 不是瞬移：命令刚下去时还在路上，GetStatus 必须显示 MOVING。
    QVERIFY2(ptz.position().pan < 0.5, "AbsoluteMove 不能瞬间到位");
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Moving);
    QCOMPARE(statusSpy.count(), 1);

    QTest::qWait(800);   // 0.5 / 1.0 每秒 = 500 ms
    COMPARE_POS(ptz.position().pan, 0.5);
    COMPARE_POS(ptz.position().tilt, 0.0);
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Idle);
    QCOMPARE(ptz.zoomStatus(), PtzMoveStatus::Idle);
    QCOMPARE(statusSpy.count(), 2);

    // 目标就是当前位置时不该留一个假的 MOVING。
    ptz.absoluteMove(panTiltOnly(0.5, 0.0), speed);
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Idle);
}

void TstPtz::relativeMoveClampsToLimits()
{
    PtzState ptz(nullptr);
    ptz.setPosition(vec(0.4, 0.0, 0.8));

    ptz.relativeMove(vec(0.2, 0.0, 0.0), vec(1.0, 1.0, 1.0));
    QTest::qWait(500);
    COMPARE_POS(ptz.position().pan, 0.6);

    // 越界的相对位移要被限位吃掉，而不是把位置推到 1 以外。
    ptz.relativeMove(vec(5.0, -5.0, 5.0), vec(1.0, 1.0, 1.0));
    QTest::qWait(1400);
    const PtzVector p = ptz.position();
    COMPARE_POS(p.pan, 1.0);
    COMPARE_POS(p.tilt, -1.0);
    COMPARE_POS(p.zoom, 1.0);
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Idle);
}

void TstPtz::setPositionCancelsMotion()
{
    PtzState ptz(nullptr);
    ptz.continuousMove(vec(1.0, 1.0, 1.0));
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Moving);

    // 外部摆位要把残留速度一起清掉，否则刚设好的位置马上又被推走。
    ptz.setPosition(vec(-0.3, 0.7, 0.4));
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Idle);
    QCOMPARE(ptz.zoomStatus(), PtzMoveStatus::Idle);
    QTest::qWait(150);
    const PtzVector p = ptz.position();
    COMPARE_POS(p.pan, -0.3);
    COMPARE_POS(p.tilt, 0.7);
    COMPARE_POS(p.zoom, 0.4);

    // 越界的摆位一律 clamp。
    ptz.setPosition(vec(-9.0, 9.0, 9.0));
    const PtzVector clamped = ptz.position();
    COMPARE_POS(clamped.pan, -1.0);
    COMPARE_POS(clamped.tilt, 1.0);
    COMPARE_POS(clamped.zoom, 1.0);
}

void TstPtz::presetCrud()
{
    PtzState ptz(nullptr);
    QSignalSpy spy(&ptz, &PtzState::presetsChanged);
    QVERIFY(ptz.presets().isEmpty());
    QVERIFY(ptz.preset(QStringLiteral("1")) == nullptr);

    ptz.setPosition(vec(0.2, -0.4, 0.6));
    const QString first = ptz.setPreset(QStringLiteral("大门"));
    QCOMPARE(first, QStringLiteral("1"));
    QCOMPARE(spy.count(), 1);

    const PtzPreset *p = ptz.preset(first);
    QVERIFY(p != nullptr);
    QCOMPARE(p->name, QStringLiteral("大门"));
    QVERIFY(p->hasPosition);
    // 预置位记的是「按下按钮那一刻」的位置。
    COMPARE_POS(p->position.pan, 0.2);
    COMPARE_POS(p->position.tilt, -0.4);
    COMPARE_POS(p->position.zoom, 0.6);

    const QString second = ptz.setPreset(QStringLiteral("车库"));
    QCOMPARE(second, QStringLiteral("2"));
    QCOMPARE(ptz.presets().size(), 2);

    // 指定已有 token 就是覆盖：位置刷新，名字给空则保留原名。
    ptz.setPosition(vec(-0.5, 0.0, 0.0));
    const QString again = ptz.setPreset(QString(), first);
    QCOMPARE(again, first);
    QCOMPARE(ptz.presets().size(), 2);
    QCOMPARE(ptz.preset(first)->name, QStringLiteral("大门"));
    COMPARE_POS(ptz.preset(first)->position.pan, -0.5);

    // GotoPreset 走的是定点移动，不是瞬移。
    ptz.setPosition(vec(0.0, 0.0, 0.0));
    QVERIFY(ptz.gotoPreset(second, vec(1.0, 1.0, 1.0)));
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Moving);
    QTest::qWait(900);
    COMPARE_POS(ptz.position().pan, 0.2);
    COMPARE_POS(ptz.position().tilt, -0.4);
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Idle);
    QVERIFY(!ptz.gotoPreset(QStringLiteral("不存在"), vec(1.0, 1.0, 1.0)));

    QVERIFY(ptz.removePreset(first));
    QVERIFY(ptz.preset(first) == nullptr);
    QCOMPARE(ptz.presets().size(), 1);
    QVERIFY(!ptz.removePreset(first));

    // 删掉的槽位号要能被下一次 SetPreset 重新用上（真机就是按槽位分配的）。
    QCOMPARE(ptz.setPreset(QStringLiteral("新点")), QStringLiteral("1"));

    ptz.clearPresets();
    QVERIFY(ptz.presets().isEmpty());
}

void TstPtz::presetOverCapacityFails()
{
    PtzState ptz(nullptr);
    QCOMPARE(ptz.maxPresets(), 300);

    ptz.generateFactoryPresets(ptz.maxPresets());
    QCOMPARE(ptz.presets().size(), ptz.maxPresets());

    // 满槽之后再新建必须失败，并给出 ter:TooManyPresets 的由头 ——
    // 这正是「出厂预填 300 槽」的相机上客户端必然撞到的错误。
    QString error;
    const QString token = ptz.setPreset(QStringLiteral("再来一个"), QString(), &error);
    QVERIFY(token.isEmpty());
    QCOMPARE(error, QStringLiteral("ter:TooManyPresets"));
    QCOMPARE(ptz.presets().size(), 300);

    // 覆盖已有槽位不占新槽，不该被容量卡住。
    QString error2;
    const QString overwritten = ptz.setPreset(QStringLiteral("改名"), QStringLiteral("7"), &error2);
    QCOMPARE(overwritten, QStringLiteral("7"));
    QVERIFY(error2.isEmpty());
    QCOMPARE(ptz.preset(QStringLiteral("7"))->name, QStringLiteral("改名"));

    // 腾出一个槽位就又能建了。
    QVERIFY(ptz.removePreset(QStringLiteral("1")));
    QString error3;
    QCOMPARE(ptz.setPreset(QStringLiteral("补位"), QString(), &error3), QStringLiteral("1"));
    QVERIFY(error3.isEmpty());
}

void TstPtz::factoryPresetsShareOnePosition()
{
    PtzState ptz(nullptr);
    ptz.generateFactoryPresets(300);

    const QList<PtzPreset> list = ptz.presets();
    QCOMPARE(list.size(), 300);
    QCOMPARE(list.first().token, QStringLiteral("1"));
    QCOMPARE(list.first().name, QStringLiteral("预置点 1"));
    QCOMPARE(list.last().token, QStringLiteral("300"));
    QCOMPARE(list.last().name, QStringLiteral("预置点 300"));
    // 占位名里的数字必须与 token 对得上，客户端的过滤正则是连着 token 一起校验的。
    QCOMPARE(ptz.preset(QStringLiteral("128"))->name, QStringLiteral("预置点 128"));

    // 掺进去的功能槽：名字不是「预置点 N」，客户端要能同时看到两种。
    const PtzPreset *reboot = ptz.preset(QStringLiteral("94"));
    QVERIFY(reboot != nullptr);
    QCOMPARE(reboot->name, QStringLiteral("远程重启"));
    const PtzPreset *cruise = ptz.preset(QStringLiteral("90"));
    QVERIFY(cruise != nullptr);
    QCOMPARE(cruise->name, QStringLiteral("巡航扫描"));

    int functional = 0;
    for (const PtzPreset &p : list) {
        // 全部共享同一个假 PTZPosition —— 客户端的聚类启发式就是冲这个来的。
        COMPARE_POS(p.position.pan, 0.0);
        COMPARE_POS(p.position.tilt, 1.0);
        COMPARE_POS(p.position.zoom, 0.0);
        QVERIFY(p.hasPosition);
        if (!p.name.startsWith(QStringLiteral("预置点 ")))
            ++functional;
    }
    QVERIFY2(functional >= 2, "出厂预填里必须掺着功能槽");

    // 生成数不能突破自己声明的容量。
    ptz.generateFactoryPresets(400);
    QCOMPARE(ptz.presets().size(), ptz.maxPresets());
    ptz.generateFactoryPresets(0);
    QVERIFY(ptz.presets().isEmpty());
}

void TstPtz::presetNameStoredVerbatim()
{
    PtzState ptz(nullptr);

    // C7：真机把名字百分号编码后原样吐回。存进去什么样，GetPresets 就得是什么样，
    // 中间绝不能偷偷解码 —— 客户端那条「像 %XX 且能严格解 UTF-8 才 unquote」的
    // 判据要成立，得先真的收到一串 %XX。
    const QString plain = QStringLiteral("测试");
    const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(plain));
    QCOMPARE(encoded, QStringLiteral("%E6%B5%8B%E8%AF%95"));

    const QString token = ptz.setPreset(encoded);
    QCOMPARE(ptz.preset(token)->name, encoded);
    QCOMPARE(QUrl::fromPercentEncoding(ptz.preset(token)->name.toUtf8()), plain);

    // 反过来，没开 quirk 时中文名要原样保存，不许被顺手编码。
    const QString utf8Token = ptz.setPreset(plain);
    QCOMPARE(ptz.preset(utf8Token)->name, plain);

    // 名字给空时自动补一个，不留无名槽位。
    const QString unnamed = ptz.setPreset(QString());
    QVERIFY(!ptz.preset(unnamed)->name.isEmpty());
}

void TstPtz::homePositionRoundTrip()
{
    PtzState ptz(nullptr);
    QVERIFY(!ptz.isHomeSet());
    // 没设过 Home 就得拒绝，让服务层去吐 ter:NoHomePosition。
    QVERIFY(!ptz.gotoHome(vec(1.0, 1.0, 1.0)));

    ptz.setPosition(vec(0.3, -0.3, 0.5));
    ptz.setHomePosition();
    QVERIFY(ptz.isHomeSet());
    COMPARE_POS(ptz.homePosition().pan, 0.3);

    ptz.setPosition(vec(-0.7, 0.7, 0.0));
    QVERIFY(ptz.gotoHome(vec(1.0, 1.0, 1.0)));
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Moving);
    QTest::qWait(1500);
    const PtzVector p = ptz.position();
    COMPARE_POS(p.pan, 0.3);
    COMPARE_POS(p.tilt, -0.3);
    COMPARE_POS(p.zoom, 0.5);
    QCOMPARE(ptz.panTiltStatus(), PtzMoveStatus::Idle);
}

void TstPtz::auxiliaryCommandRecorded()
{
    PtzState ptz(nullptr);
    QVERIFY(ptz.lastAuxiliaryCommand().isEmpty());
    ptz.sendAuxiliaryCommand(QStringLiteral("tt:Wiper|On"));
    QCOMPARE(ptz.lastAuxiliaryCommand(), QStringLiteral("tt:Wiper|On"));
}

void TstPtz::imagingIrCutFilterTristate()
{
    ImagingState imaging(nullptr);
    QSignalSpy spy(&imaging, &ImagingState::settingsChanged);
    // 参照客户端只读这一个字段判白天 / 夜视，三档都得能落到状态里。
    QCOMPARE(imaging.settings().irCutFilter, IrCutFilterMode::Auto);

    imaging.setIrCutFilter(IrCutFilterMode::On);
    QCOMPARE(imaging.settings().irCutFilter, IrCutFilterMode::On);
    QCOMPARE(spy.count(), 1);
    imaging.setIrCutFilter(IrCutFilterMode::On);
    QCOMPARE(spy.count(), 1);   // 没变化就不该发信号

    imaging.setIrCutFilter(IrCutFilterMode::Off);
    QCOMPARE(imaging.settings().irCutFilter, IrCutFilterMode::Off);
    QCOMPARE(spy.count(), 2);

    QCOMPARE(ImagingState::irCutFilterName(IrCutFilterMode::On), QStringLiteral("ON"));
    QCOMPARE(ImagingState::irCutFilterName(IrCutFilterMode::Off), QStringLiteral("OFF"));
    QCOMPARE(ImagingState::irCutFilterName(IrCutFilterMode::Auto), QStringLiteral("AUTO"));

    bool ok = false;
    QCOMPARE(ImagingState::irCutFilterFromName(QStringLiteral("on"), &ok), IrCutFilterMode::On);
    QVERIFY(ok);
    QCOMPARE(ImagingState::irCutFilterFromName(QStringLiteral(" AUTO "), &ok),
             IrCutFilterMode::Auto);
    QVERIFY(ok);
    QCOMPARE(ImagingState::irCutFilterFromName(QStringLiteral("dark"), &ok), IrCutFilterMode::Auto);
    QVERIFY(!ok);
}

void TstPtz::imagingWhiteLightToggles()
{
    ImagingState imaging(nullptr);
    QSignalSpy lightSpy(&imaging, &ImagingState::whiteLightChanged);
    QSignalSpy settingsSpy(&imaging, &ImagingState::settingsChanged);
    QVERIFY(!imaging.settings().whiteLightOn);

    imaging.setWhiteLight(true, 60);
    QVERIFY(imaging.settings().whiteLightOn);
    QCOMPARE(imaging.settings().whiteLightBrightness, 60);
    QCOMPARE(lightSpy.count(), 1);
    QCOMPARE(lightSpy.takeFirst().at(0).toBool(), true);
    QCOMPARE(settingsSpy.count(), 1);

    imaging.setWhiteLight(true, 60);
    QCOMPARE(lightSpy.count(), 0);   // 幂等

    // brightness 缺省（-1）表示只改开关、保留亮度。
    imaging.setWhiteLight(false);
    QVERIFY(!imaging.settings().whiteLightOn);
    QCOMPARE(imaging.settings().whiteLightBrightness, 60);
    QCOMPARE(lightSpy.count(), 1);
    QCOMPARE(lightSpy.takeFirst().at(0).toBool(), false);

    // 走 SetImagingSettings 整体覆盖时，厂商私有接口那边也得跟着变：
    // GetWhiteLed / supplementLight 看的是同一份状态。
    ImagingSettings s = imaging.settings();
    s.whiteLightOn = true;
    s.whiteLightBrightness = 200;
    imaging.setSettings(s);
    QVERIFY(imaging.settings().whiteLightOn);
    QCOMPARE(imaging.settings().whiteLightBrightness, 100);   // 越界被 clamp
    QCOMPARE(lightSpy.count(), 1);
}

void TstPtz::imagingSettingsClamped()
{
    ImagingState imaging(nullptr);
    imaging.setBrightness(150.0);
    COMPARE_POS(imaging.settings().brightness, 100.0);
    imaging.setBrightness(-20.0);
    COMPARE_POS(imaging.settings().brightness, 0.0);
    imaging.setContrast(42.0);
    COMPARE_POS(imaging.settings().contrast, 42.0);

    ImagingSettings s;
    s.colorSaturation = 999.0;
    s.sharpness = -5.0;
    imaging.setSettings(s);
    COMPARE_POS(imaging.settings().colorSaturation, 100.0);
    COMPARE_POS(imaging.settings().sharpness, 0.0);
}

void TstPtz::imagingFocusMove()
{
    ImagingState imaging(nullptr);
    QVERIFY(!imaging.isFocusMoving());
    const double start = imaging.focusPosition();

    // 相对聚焦：0.3 行程、满速 0.5/s，约 600 ms 到位。
    imaging.moveFocus(0.3, 1.0);
    QVERIFY(imaging.isFocusMoving());
    QVERIFY2(imaging.focusPosition() < start + 0.3, "聚焦不能瞬移");
    QCOMPARE(imaging.settings().focusMode, QStringLiteral("MANUAL"));

    QTest::qWait(900);
    COMPARE_POS(imaging.focusPosition(), start + 0.3);
    QVERIFY(!imaging.isFocusMoving());

    // 连续聚焦：位移为 0 时按速度符号一路走，Stop 才停。
    imaging.moveFocus(0.0, -1.0);
    QVERIFY(imaging.isFocusMoving());
    QTest::qWait(200);
    const double moved = imaging.focusPosition();
    QVERIFY(moved < start + 0.3);
    imaging.stopFocus();
    QVERIFY(!imaging.isFocusMoving());
    QTest::qWait(150);
    COMPARE_POS(imaging.focusPosition(), moved);

    // 一路走到近端限位就自己停。
    imaging.moveFocus(0.0, -1.0);
    QTest::qWait(1800);
    COMPARE_POS(imaging.focusPosition(), 0.0);
    QVERIFY(!imaging.isFocusMoving());
}

QTEST_GUILESS_MAIN(TstPtz)

#include "tst_ptz.moc"
