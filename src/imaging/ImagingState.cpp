#include "imaging/ImagingState.h"

#include <QtCore/QElapsedTimer>

#include <cmath>

namespace onvifsim {
namespace {

// ONVIF 的 Imaging 各项范围是设备自报的，这里统一用 0..100：GetOptions 也按这个吐，
// 客户端拿到什么范围就在什么范围里发，两边对得上就行。
constexpr double kSettingMin = 0.0;
constexpr double kSettingMax = 100.0;

// 聚焦位置也归一到 [0, 1]，和 PTZ 的 zoom 一个套路，省掉一次单位换算。
constexpr double kFocusMin = 0.0;
constexpr double kFocusMax = 1.0;

// 满速下每秒走过的聚焦行程：全程 2 秒，和云台变焦一致。
constexpr double kFocusRate = 0.5;

constexpr double kEps = 1e-9;

double clampRange(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// 速度归一到 (0, 1]。ONVIF 的 Move 可以不带 Speed，客户端也常发 0，
// 这两种都按满速走，否则速率为 0 的相对聚焦永远到不了目标。
double speedFactor(double raw)
{
    const double s = std::fabs(raw);
    if (s < kEps)
        return 1.0;
    return s > 1.0 ? 1.0 : s;
}

} // namespace

struct ImagingState::Private
{
    enum class FocusMode { Idle, Continuous, Target };

    VirtualCamera *camera = nullptr;   // 暂时只留着：Imaging 还没有自己的 quirk
    ImagingSettings settings;

    // 聚焦与 PTZ 用同一套模型：单调时钟 + 惰性积分，GetStatus 什么时候来都准。
    QElapsedTimer clock;
    qint64 lastTickMs = 0;
    FocusMode focusMode = FocusMode::Idle;
    double focus = 0.5;
    double focusVel = 0.0;      // 连续模式带符号，定点模式是正的速率
    double focusTarget = 0.0;

    void advance(qint64 now);
    qint64 nowMs() const { return clock.elapsed(); }
    void clampSettings();
};

void ImagingState::Private::advance(qint64 now)
{
    if (now < lastTickMs) {
        lastTickMs = now;
        return;
    }
    const double dt = static_cast<double>(now - lastTickMs) / 1000.0;
    lastTickMs = now;
    if (focusMode == FocusMode::Idle)
        return;

    if (focusMode == FocusMode::Continuous) {
        focus = clampRange(focus + focusVel * dt, kFocusMin, kFocusMax);
        // 顶到近端 / 远端就停，等同于真机的机械限位。
        const bool canMove = (focusVel > kEps && focus < kFocusMax - kEps)
                || (focusVel < -kEps && focus > kFocusMin + kEps);
        if (!canMove) {
            focusMode = FocusMode::Idle;
            focusVel = 0.0;
        }
        return;
    }

    const double remain = focusTarget - focus;
    const double step = focusVel * dt;
    if (std::fabs(remain) <= step + kEps) {
        focus = focusTarget;   // 吸附到目标，别让浮点残差把状态永远吊在 MOVING
        focusMode = FocusMode::Idle;
        focusVel = 0.0;
    } else {
        focus += remain > 0.0 ? step : -step;
    }
}

void ImagingState::Private::clampSettings()
{
    settings.brightness = clampRange(settings.brightness, kSettingMin, kSettingMax);
    settings.colorSaturation = clampRange(settings.colorSaturation, kSettingMin, kSettingMax);
    settings.contrast = clampRange(settings.contrast, kSettingMin, kSettingMax);
    settings.sharpness = clampRange(settings.sharpness, kSettingMin, kSettingMax);
    settings.whiteLightBrightness =
            qBound(0, settings.whiteLightBrightness, static_cast<int>(kSettingMax));
}

ImagingState::ImagingState(VirtualCamera *camera, QObject *parent)
    : QObject(parent), d(new Private)
{
    d->camera = camera;
    d->clock.start();
    d->lastTickMs = d->clock.elapsed();
}

ImagingState::~ImagingState()
{
    delete d;
}

const ImagingSettings &ImagingState::settings() const
{
    return d->settings;
}

void ImagingState::setSettings(const ImagingSettings &settings)
{
    const bool wasOn = d->settings.whiteLightOn;
    const int wasBrightness = d->settings.whiteLightBrightness;

    d->settings = settings;
    d->clampSettings();

    emit settingsChanged();
    // 白光灯的状态被 SetImagingSettings 整体覆盖时，厂商私有接口那边也要跟着变，
    // 所以这条信号照发不误 —— 两边看的本来就是同一份状态。
    if (wasOn != d->settings.whiteLightOn || wasBrightness != d->settings.whiteLightBrightness)
        emit whiteLightChanged(d->settings.whiteLightOn);
}

void ImagingState::setBrightness(double value)
{
    const double v = clampRange(value, kSettingMin, kSettingMax);
    if (d->settings.brightness == v)
        return;
    d->settings.brightness = v;
    emit settingsChanged();
}

void ImagingState::setContrast(double value)
{
    const double v = clampRange(value, kSettingMin, kSettingMax);
    if (d->settings.contrast == v)
        return;
    d->settings.contrast = v;
    emit settingsChanged();
}

void ImagingState::setIrCutFilter(IrCutFilterMode mode)
{
    if (d->settings.irCutFilter == mode)
        return;
    d->settings.irCutFilter = mode;
    // 参照客户端只读这一个字段来判白天 / 夜视，写则走厂商私有 API；
    // 两条路径改的都是这里，状态必须只有一份。
    emit settingsChanged();
}

void ImagingState::setWhiteLight(bool on, int brightness)
{
    const int level = brightness < 0
            ? d->settings.whiteLightBrightness
            : qBound(0, brightness, static_cast<int>(kSettingMax));
    if (d->settings.whiteLightOn == on && d->settings.whiteLightBrightness == level)
        return;

    d->settings.whiteLightOn = on;
    d->settings.whiteLightBrightness = level;
    // ONVIF 没有白光灯字段，Reolink 的 GetWhiteLed / 海康的 supplementLight 各说各话，
    // 但改的是同一份状态；GUI 与另一家的私有接口都靠这条信号跟上。
    emit whiteLightChanged(on);
    emit settingsChanged();
}

void ImagingState::moveFocus(double distance, double speed)
{
    const qint64 now = d->nowMs();
    d->advance(now);

    if (std::fabs(distance) > kEps) {
        // Relative / Absolute 聚焦：走一段固定行程，按 Speed 决定用多久。
        d->focusTarget = clampRange(d->focus + distance, kFocusMin, kFocusMax);
        d->focusVel = kFocusRate * speedFactor(speed);
        d->focusMode = Private::FocusMode::Target;
    } else if (std::fabs(speed) > kEps) {
        // Continuous 聚焦：位移为 0 时按 Speed 的符号一直走，直到 Stop 或顶到限位。
        d->focusVel = kFocusRate * clampRange(speed, -1.0, 1.0);
        d->focusMode = Private::FocusMode::Continuous;
    } else {
        d->focusMode = Private::FocusMode::Idle;
        d->focusVel = 0.0;
    }

    // 零时长积分：目标就是当前位置、或已经顶在限位上时立刻收敛，不留假的 MOVING。
    d->advance(now);

    // 手动聚焦会把相机踢出自动对焦，真机普遍如此；客户端读 FocusMode 判断能不能手动调。
    if (d->settings.focusMode != QStringLiteral("MANUAL")) {
        d->settings.focusMode = QStringLiteral("MANUAL");
        emit settingsChanged();
    }
}

void ImagingState::stopFocus()
{
    d->advance(d->nowMs());
    d->focusMode = Private::FocusMode::Idle;
    d->focusVel = 0.0;
}

bool ImagingState::isFocusMoving() const
{
    d->advance(d->nowMs());
    return d->focusMode != Private::FocusMode::Idle;
}

double ImagingState::focusPosition() const
{
    d->advance(d->nowMs());
    return d->focus;
}

QString ImagingState::irCutFilterName(IrCutFilterMode mode)
{
    switch (mode) {
    case IrCutFilterMode::On:  return QStringLiteral("ON");
    case IrCutFilterMode::Off: return QStringLiteral("OFF");
    case IrCutFilterMode::Auto: break;
    }
    return QStringLiteral("AUTO");
}

IrCutFilterMode ImagingState::irCutFilterFromName(const QString &name, bool *ok)
{
    if (ok)
        *ok = true;
    const QString v = name.trimmed().toUpper();
    if (v == QLatin1String("ON"))
        return IrCutFilterMode::On;
    if (v == QLatin1String("OFF"))
        return IrCutFilterMode::Off;
    if (v == QLatin1String("AUTO"))
        return IrCutFilterMode::Auto;
    // 认不出来就报错并回 AUTO：ONVIF 的默认档，比直接 Fault 更不容易把客户端卡住。
    if (ok)
        *ok = false;
    return IrCutFilterMode::Auto;
}

} // namespace onvifsim
