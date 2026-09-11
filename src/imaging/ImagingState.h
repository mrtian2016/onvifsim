#pragma once

// 图像参数状态。与厂商私有接口联动：
// Reolink 的 GetWhiteLed、海康的 supplementLight / ircutFilter 改的是同一份状态。

#include <QtCore/QObject>
#include <QtCore/QString>

namespace onvifsim {

class VirtualCamera;

enum class IrCutFilterMode { On, Off, Auto };

struct ImagingSettings {
    double brightness = 50.0;
    double colorSaturation = 50.0;
    double contrast = 50.0;
    double sharpness = 50.0;
    IrCutFilterMode irCutFilter = IrCutFilterMode::Auto;
    // 补光灯 / 白光灯：ONVIF 没有标准字段，走厂商私有接口，但状态在这里。
    bool whiteLightOn = false;
    int whiteLightBrightness = 100;
    QString exposureMode = QStringLiteral("AUTO");
    QString focusMode = QStringLiteral("AUTO");
    QString wideDynamicRange = QStringLiteral("OFF");
    QString backlightCompensation = QStringLiteral("OFF");
};

// 注意：这个类的若干 const 方法（position() / settings() 一类）在内部会**推进
// 积分状态**再返回 —— 语义上是「读」，实现上会改 d 里的时间戳与当前值。
// 因此它**不是线程安全的**，也不要在同一个表达式里对同一对象调两次。
class ImagingState : public QObject
{
    Q_OBJECT
public:
    explicit ImagingState(VirtualCamera *camera, QObject *parent = nullptr);
    ~ImagingState() override;

    const ImagingSettings &settings() const;
    void setSettings(const ImagingSettings &settings);

    void setBrightness(double value);
    void setContrast(double value);
    void setIrCutFilter(IrCutFilterMode mode);
    void setWhiteLight(bool on, int brightness = -1);

    // 聚焦移动（Move / Stop / GetStatus）。
    void moveFocus(double distance, double speed);
    void stopFocus();
    bool isFocusMoving() const;
    double focusPosition() const;

    static QString irCutFilterName(IrCutFilterMode mode);
    static IrCutFilterMode irCutFilterFromName(const QString &name, bool *ok = nullptr);

signals:
    void settingsChanged();
    void whiteLightChanged(bool on);

private:
    struct Private;
    Private *d;
};

} // namespace onvifsim
