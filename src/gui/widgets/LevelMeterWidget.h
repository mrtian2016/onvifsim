#pragma once

// 对讲电平表：自绘的横向条，绿 → 黄 → 红三段，外加一条会缓慢回落的峰值线。
//
// 客户端推的音频到底有没有进来、是不是全零，看这一条最快；
// 峰值线保留 1.5 秒是为了让一闪而过的爆音也能被看见。

#include <QtCore/QElapsedTimer>
#include <QtWidgets/QWidget>

namespace onvifsim {
namespace gui {

class LevelMeterWidget : public QWidget
{
    Q_OBJECT
public:
    explicit LevelMeterWidget(QWidget *parent = nullptr);

    // level ∈ [0, 1]
    void setLevel(double level);
    double level() const;
    void reset();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    double m_level = 0.0;
    double m_peak = 0.0;
    QElapsedTimer m_peakAge;
};

} // namespace gui
} // namespace onvifsim
