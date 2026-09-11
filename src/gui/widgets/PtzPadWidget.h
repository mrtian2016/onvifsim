#pragma once

// PTZ 位置十字准星：自绘的方形控件，实时跟随 PtzState::position()。
//
// 除了显示，按住拖动还能当摇杆用：离中心多远就是多大速度，松开即停 ——
// 这样在界面上就能测「客户端还没发 Stop 时相机在干什么」。
// 所有颜色都从 QPalette 取，深浅色主题跟随系统时不用改一行代码。

#include "ptz/PtzState.h"

#include <QtWidgets/QWidget>

namespace onvifsim {
namespace gui {

class PtzPadWidget : public QWidget
{
    Q_OBJECT
public:
    explicit PtzPadWidget(QWidget *parent = nullptr);

    void setPosition(const PtzVector &position);
    PtzVector position() const;
    void setMoving(bool moving);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    // 摇杆按下 / 拖动：pan、tilt ∈ [-1, 1]，是速度不是位置。
    void velocityRequested(double pan, double tilt);
    void stopRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QRectF padRect() const;
    void emitVelocityFor(const QPointF &pos);

    PtzVector m_position;
    bool m_moving = false;
    bool m_dragging = false;
    QPointF m_dragPoint;
};

} // namespace gui
} // namespace onvifsim
