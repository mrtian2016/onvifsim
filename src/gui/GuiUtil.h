#pragma once

// GUI 内部的小工具函数。
//
// 放在一处而不是各 Tab 各写一份，是为了让「时间怎么显示」「字节数怎么显示」
// 在整个界面里保持一致 —— 用户在概览里看到的 "3 秒前" 和在日志里看到的
// 必须是同一套写法，否则对不上账。

#include "core/LogBus.h"

#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtGui/QIcon>

class QWidget;

namespace onvifsim {
namespace gui {
namespace util {

// 程序图标（窗口、托盘、关于框都用它）。内含 16~256 多档位图，
// QIcon 按实际像素密度挑，不要自己 scaled()。
QIcon appIcon();

// 复制到系统剪贴板，并在 anchor 附近弹一个短暂的提示气泡。
// anchor 传 nullptr 表示不提示。
void copyToClipboard(const QString &text, QWidget *anchor);

// UTC 时间戳 → 本地时间 "HH:mm:ss.zzz"。日志面板与各统计表都用这个。
QString formatTime(const QDateTime &utc);
// UTC 时间戳 → 本地 "MM-dd HH:mm:ss"，用于「开始时间」这类跨分钟的场合。
QString formatDateTime(const QDateTime &utc);
// "3 秒前" / "2 分钟前"；无效时间返回 "—"。
QString formatRelative(const QDateTime &utc);
// 1024 → "1.0 KB"
QString formatBytes(qint64 bytes);
// 微秒 → "1.2 ms"；负数表示不适用，返回空串。
QString formatDurationUs(qint64 us);

// 日志级别的中文名（英文由 i18n 提供）。LogRecord::levelName() 是给
// 文件与 REST 用的稳定英文键，界面上要显示的是这个。
QString levelName(LogLevel level);

} // namespace util
} // namespace gui
} // namespace onvifsim
