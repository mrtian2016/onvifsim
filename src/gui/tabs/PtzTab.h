#pragma once

// PTZ Tab：十字准星 + 摇杆 + 速度 + 预置位表。
//
// 「一键生成出厂 300 槽」对应故障注入 C6：真机出厂就把 300 个槽位填满，
// 客户端如果把 GetPresets 的结果原样铺到界面上会直接被撑爆。
//
// 图像参数（亮度 / 对比度 / 红外滤片 / 白光灯）也放在这一页：它们和 PTZ 一样
// 属于「云台侧的现场控制」，而且厂商私有接口改的就是同一份 ImagingState。

#include "gui/tabs/CameraTab.h"
#include "ptz/PtzState.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QTableWidget;

namespace onvifsim {
namespace gui {

class PtzPadWidget;

class PtzTab : public CameraTab
{
    Q_OBJECT
public:
    explicit PtzTab(Simulator *simulator, QWidget *parent = nullptr);

protected:
    void bindCamera() override;
    void refresh() override;

private:
    void reloadPresets();
    void refreshImaging();
    double speedFactor() const;
    void move(double pan, double tilt, double zoom);
    QString selectedPresetToken() const;

    PtzPadWidget *m_pad = nullptr;
    QSlider *m_speed = nullptr;
    QLabel *m_speedLabel = nullptr;
    QLabel *m_statusLabel = nullptr;

    QTableWidget *m_presets = nullptr;
    QLineEdit *m_presetName = nullptr;
    QSpinBox *m_factoryCount = nullptr;

    QSlider *m_brightness = nullptr;
    QSlider *m_contrast = nullptr;
    QComboBox *m_irCut = nullptr;
    QCheckBox *m_whiteLight = nullptr;
    QSpinBox *m_whiteLightBrightness = nullptr;
};

} // namespace gui
} // namespace onvifsim
