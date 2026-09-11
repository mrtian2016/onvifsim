#include "gui/widgets/PtzPadWidget.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>

namespace onvifsim {
namespace gui {

namespace {

constexpr int kZoomBarWidth = 18;
constexpr int kZoomBarGap = 10;
constexpr int kTextHeight = 18;

double clamp(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

PtzPadWidget::PtzPadWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(180, 180);
    setCursor(Qt::CrossCursor);
    setToolTip(tr("按住拖动 = 连续移动（离中心越远越快），松开即停。"));
}

QSize PtzPadWidget::sizeHint() const
{
    return QSize(260, 260);
}

QSize PtzPadWidget::minimumSizeHint() const
{
    return QSize(180, 180);
}

void PtzPadWidget::setPosition(const PtzVector &position)
{
    m_position = position;
    update();
}

PtzVector PtzPadWidget::position() const
{
    return m_position;
}

void PtzPadWidget::setMoving(bool moving)
{
    if (m_moving == moving)
        return;
    m_moving = moving;
    update();
}

QRectF PtzPadWidget::padRect() const
{
    const double side = qMin(width() - kZoomBarWidth - kZoomBarGap - 2.0,
                             height() - kTextHeight - 2.0);
    const double clamped = qMax(40.0, side);
    return QRectF(1.0, 1.0, clamped, clamped);
}

void PtzPadWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QPalette &pal = palette();
    const QRectF pad = padRect();

    // 底板
    painter.setPen(QPen(pal.color(QPalette::Mid), 1.0));
    painter.setBrush(pal.brush(QPalette::Base));
    painter.drawRoundedRect(pad, 4.0, 4.0);

    // 网格：每 0.25 一条，让「现在偏了多少」一眼能读出来
    painter.setPen(QPen(pal.color(QPalette::Midlight), 1.0, Qt::DotLine));
    for (int i = 1; i < 8; ++i) {
        const double t = pad.left() + pad.width() * i / 8.0;
        painter.drawLine(QPointF(t, pad.top()), QPointF(t, pad.bottom()));
        const double u = pad.top() + pad.height() * i / 8.0;
        painter.drawLine(QPointF(pad.left(), u), QPointF(pad.right(), u));
    }

    // 中轴
    painter.setPen(QPen(pal.color(QPalette::Mid), 1.0));
    painter.drawLine(QPointF(pad.center().x(), pad.top()),
                     QPointF(pad.center().x(), pad.bottom()));
    painter.drawLine(QPointF(pad.left(), pad.center().y()),
                     QPointF(pad.right(), pad.center().y()));

    // 当前位置。tilt 为正表示向上，屏幕坐标要反过来。
    const double px = pad.left() + (m_position.pan + 1.0) / 2.0 * pad.width();
    const double py = pad.top() + (1.0 - m_position.tilt) / 2.0 * pad.height();
    const QColor accent = m_moving ? pal.color(QPalette::Highlight)
                                   : pal.color(QPalette::Highlight).darker(120);

    painter.setPen(QPen(accent, 1.0, Qt::DashLine));
    painter.drawLine(QPointF(px, pad.top()), QPointF(px, pad.bottom()));
    painter.drawLine(QPointF(pad.left(), py), QPointF(pad.right(), py));

    painter.setPen(QPen(accent, 2.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QPointF(px, py), 9.0, 9.0);
    painter.setBrush(accent);
    painter.drawEllipse(QPointF(px, py), 2.5, 2.5);

    // 右侧 zoom 竖条
    const QRectF zoomBar(pad.right() + kZoomBarGap, pad.top(), kZoomBarWidth, pad.height());
    painter.setPen(QPen(pal.color(QPalette::Mid), 1.0));
    painter.setBrush(pal.brush(QPalette::Base));
    painter.drawRoundedRect(zoomBar, 3.0, 3.0);
    const double zoom = clamp(m_position.zoom, 0.0, 1.0);
    QRectF fill = zoomBar.adjusted(2.0, 0.0, -2.0, -2.0);
    fill.setTop(fill.bottom() - fill.height() * zoom);
    painter.setPen(Qt::NoPen);
    painter.setBrush(accent);
    painter.drawRoundedRect(fill, 2.0, 2.0);

    // 数值
    painter.setPen(pal.color(QPalette::Text));
    const QRectF textRect(0.0, pad.bottom() + 1.0, width(), kTextHeight);
    painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                     tr("P %1  T %2  Z %3")
                         .arg(m_position.pan, 0, 'f', 3)
                         .arg(m_position.tilt, 0, 'f', 3)
                         .arg(m_position.zoom, 0, 'f', 3));
}

void PtzPadWidget::emitVelocityFor(const QPointF &pos)
{
    const QRectF pad = padRect();
    const double pan = clamp((pos.x() - pad.center().x()) / (pad.width() / 2.0), -1.0, 1.0);
    const double tilt = clamp((pad.center().y() - pos.y()) / (pad.height() / 2.0), -1.0, 1.0);
    emit velocityRequested(pan, tilt);
}

void PtzPadWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    m_dragPoint = event->position();
    emitVelocityFor(m_dragPoint);
}

void PtzPadWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging)
        return;
    m_dragPoint = event->position();
    emitVelocityFor(m_dragPoint);
}

void PtzPadWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;
    emit stopRequested();
}

} // namespace gui
} // namespace onvifsim
