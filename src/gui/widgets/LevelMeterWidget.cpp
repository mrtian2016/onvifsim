#include "gui/widgets/LevelMeterWidget.h"

#include <QtGui/QPainter>

namespace onvifsim {
namespace gui {

namespace {

constexpr int kSegments = 40;
constexpr qint64 kPeakHoldMs = 1500;

} // namespace

LevelMeterWidget::LevelMeterWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(22);
    m_peakAge.start();
}

QSize LevelMeterWidget::sizeHint() const
{
    return QSize(320, 24);
}

QSize LevelMeterWidget::minimumSizeHint() const
{
    return QSize(120, 22);
}

void LevelMeterWidget::setLevel(double level)
{
    m_level = qBound(0.0, level, 1.0);
    if (m_level >= m_peak || m_peakAge.elapsed() > kPeakHoldMs) {
        m_peak = m_level;
        m_peakAge.restart();
    }
    update();
}

double LevelMeterWidget::level() const
{
    return m_level;
}

void LevelMeterWidget::reset()
{
    m_level = 0.0;
    m_peak = 0.0;
    m_peakAge.restart();
    update();
}

void LevelMeterWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const QPalette &pal = palette();
    const QRectF area = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    painter.setPen(QPen(pal.color(QPalette::Mid), 1.0));
    painter.setBrush(pal.brush(QPalette::Base));
    painter.drawRoundedRect(area, 3.0, 3.0);

    // 分段画，纯色长条看不出「爆了没有」，分段能数格子。
    const double segWidth = (area.width() - 4.0) / kSegments;
    const int lit = static_cast<int>(m_level * kSegments + 0.5);
    for (int i = 0; i < kSegments; ++i) {
        QColor color;
        if (i < kSegments * 3 / 5)
            color = QColor(0x2e, 0xa0, 0x43);
        else if (i < kSegments * 9 / 10)
            color = QColor(0xd2, 0x9d, 0x1e);
        else
            color = QColor(0xc2, 0x3b, 0x22);
        if (i >= lit)
            color.setAlpha(38);   // 熄灭的格子留个淡影，条的总长才看得出来

        const QRectF seg(area.left() + 2.0 + i * segWidth, area.top() + 2.0,
                         segWidth - 1.0, area.height() - 4.0);
        painter.fillRect(seg, color);
    }

    if (m_peak > 0.0) {
        const double x = area.left() + 2.0 + m_peak * (area.width() - 4.0);
        painter.setPen(QPen(pal.color(QPalette::Text), 1.5));
        painter.drawLine(QPointF(x, area.top() + 1.0), QPointF(x, area.bottom() - 1.0));
    }
}

} // namespace gui
} // namespace onvifsim
