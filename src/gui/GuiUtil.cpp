#include "gui/GuiUtil.h"

#include <QtGui/QClipboard>
#include <QtWidgets/QApplication>
#include <QtWidgets/QToolTip>
#include <QtWidgets/QWidget>

// 静态库（onvifsim-gui-lib）里的 Qt 资源不会自动初始化，图标要显式注册一次。
// 资源名必须与 qrc 文件基名一致（icons.qrc → icons）。
// 这个函数必须待在文件作用域：Q_INIT_RESOURCE 展开出的 extern 声明一旦被匿名
// namespace 限定住，链接期就找不到真正的全局符号了。
static void ensureIconResources()
{
    static bool done = false;
    if (!done) {
        Q_INIT_RESOURCE(icons);
        done = true;
    }
}

namespace onvifsim {
namespace gui {
namespace util {

QIcon appIcon()
{
    ensureIconResources();
    static const QIcon icon = [] {
        QIcon result;
        for (int size : {16, 24, 32, 48, 64, 128, 256})
            result.addFile(QStringLiteral(":/onvifsim/logo-%1.png").arg(size),
                           QSize(size, size));
        return result;
    }();
    return icon;
}

void copyToClipboard(const QString &text, QWidget *anchor)
{
    QApplication::clipboard()->setText(text);
    if (!anchor)
        return;
    // 复制这个动作没有任何可见结果，不给反馈用户会反复点。
    QToolTip::showText(anchor->mapToGlobal(anchor->rect().bottomLeft()),
                       QCoreApplication::translate("onvifsim::gui::util", "已复制"), anchor,
                       QRect(), 1200);
}

QString formatTime(const QDateTime &utc)
{
    if (!utc.isValid())
        return QStringLiteral("—");
    return utc.toLocalTime().toString(QStringLiteral("HH:mm:ss.zzz"));
}

QString formatDateTime(const QDateTime &utc)
{
    if (!utc.isValid())
        return QStringLiteral("—");
    return utc.toLocalTime().toString(QStringLiteral("MM-dd HH:mm:ss"));
}

QString formatRelative(const QDateTime &utc)
{
    if (!utc.isValid())
        return QStringLiteral("—");
    const qint64 secs = utc.secsTo(QDateTime::currentDateTimeUtc());
    if (secs < 0)
        return formatTime(utc);
    if (secs < 60)
        return QCoreApplication::translate("onvifsim::gui::util", "%1 秒前").arg(secs);
    if (secs < 3600)
        return QCoreApplication::translate("onvifsim::gui::util", "%1 分钟前").arg(secs / 60);
    if (secs < 86400)
        return QCoreApplication::translate("onvifsim::gui::util", "%1 小时前").arg(secs / 3600);
    return formatDateTime(utc);
}

QString formatBytes(qint64 bytes)
{
    if (bytes < 1024)
        return QCoreApplication::translate("onvifsim::gui::util", "%1 B").arg(bytes);
    double value = static_cast<double>(bytes) / 1024.0;
    if (value < 1024.0)
        return QCoreApplication::translate("onvifsim::gui::util", "%1 KB")
            .arg(value, 0, 'f', 1);
    value /= 1024.0;
    if (value < 1024.0)
        return QCoreApplication::translate("onvifsim::gui::util", "%1 MB")
            .arg(value, 0, 'f', 1);
    value /= 1024.0;
    return QCoreApplication::translate("onvifsim::gui::util", "%1 GB").arg(value, 0, 'f', 2);
}

QString formatDurationUs(qint64 us)
{
    if (us < 0)
        return QString();
    if (us < 1000)
        return QCoreApplication::translate("onvifsim::gui::util", "%1 µs").arg(us);
    return QCoreApplication::translate("onvifsim::gui::util", "%1 ms")
        .arg(static_cast<double>(us) / 1000.0, 0, 'f', 1);
}

QString levelName(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug:
        return QCoreApplication::translate("onvifsim::gui::util", "调试");
    case LogLevel::Info:
        return QCoreApplication::translate("onvifsim::gui::util", "信息");
    case LogLevel::Warning:
        return QCoreApplication::translate("onvifsim::gui::util", "警告");
    case LogLevel::Error:
        return QCoreApplication::translate("onvifsim::gui::util", "错误");
    }
    return QString();
}

} // namespace util
} // namespace gui
} // namespace onvifsim
