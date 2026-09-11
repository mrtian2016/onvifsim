#include "core/LogBus.h"

#include <atomic>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QTextStream>

#include <cstdio>

namespace onvifsim {
namespace {

// 进程内只会有一个 Simulator，日志总线做成全局可达能省掉到处传指针。
LogBus *g_global = nullptr;

const char *levelText(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug:   return "DEBUG";
    case LogLevel::Warning: return "WARN ";
    case LogLevel::Error:   return "ERROR";
    case LogLevel::Info:    break;
    }
    return "INFO ";
}

} // namespace

QString LogRecord::levelName() const
{
    return QString::fromLatin1(levelText(level)).trimmed();
}

struct LogBus::Private {
    QVector<LogRecord> history;
    int historyLimit = 5000;
    // 这三个是跨线程读写的：post() 可能从任意线程调（plan.md 里 RtspServer
    // 将来要整体 moveToThread），而设置它们的是主线程。用原子量而不是靠
    // d->mutex —— mutex 只护 history 与文件写入，这三个原来在锁外裸读裸写，
    // 真到多线程那天就是数据竞争。
    std::atomic<bool> stdoutEnabled{ false };
    std::atomic<bool> stdoutVerbose{ false };
    std::atomic<LogLevel> minimumLevel{ LogLevel::Info };
    QFile file;
    QMutex mutex;   // 只保护 history 与文件写入；信号发射在调用线程上
};

LogBus::LogBus(QObject *parent) : QObject(parent), d(new Private)
{
    qRegisterMetaType<LogRecord>("onvifsim::LogRecord");
}

LogBus::~LogBus()
{
    if (g_global == this)
        g_global = nullptr;
    if (d->file.isOpen())
        d->file.close();
    delete d;
}

LogBus *LogBus::global()
{
    return g_global;
}

void LogBus::setGlobal(LogBus *bus)
{
    g_global = bus;
}

void LogBus::post(const LogRecord &record)
{
    if (static_cast<int>(record.level) < static_cast<int>(d->minimumLevel.load()))
        return;

    {
        QMutexLocker lock(&d->mutex);
        d->history.append(record);
        if (d->historyLimit > 0 && d->history.size() > d->historyLimit)
            d->history.remove(0, d->history.size() - d->historyLimit);

        if (d->stdoutEnabled || d->file.isOpen()) {
            QString line = QStringLiteral("%1 %2 [%3]%4 %5")
                               .arg(record.timestamp.toString(Qt::ISODateWithMs),
                                    QString::fromLatin1(levelText(record.level)),
                                    record.category,
                                    record.cameraId.isEmpty()
                                        ? QString()
                                        : QStringLiteral("[%1]").arg(record.cameraId),
                                    record.summary);
            if (!record.peer.isEmpty())
                line += QStringLiteral(" <%1>").arg(record.peer);
            if (record.durationUs >= 0)
                line += QStringLiteral(" (%1ms)")
                            .arg(record.durationUs / 1000.0, 0, 'f', 1);
            if (!record.quirkKey.isEmpty())
                line += QStringLiteral(" [quirk:%1]").arg(record.quirkKey);
            line += QLatin1Char('\n');
            if (d->stdoutVerbose && !record.detail.isEmpty())
                line += record.detail + QLatin1Char('\n');

            if (d->stdoutEnabled) {
                const QByteArray utf8 = line.toUtf8();
                std::fwrite(utf8.constData(), 1, static_cast<size_t>(utf8.size()),
                            record.ok ? stdout : stderr);
                std::fflush(record.ok ? stdout : stderr);
            }
            if (d->file.isOpen()) {
                d->file.write(line.toUtf8());
                d->file.flush();
            }
        }
    }

    emit recordPosted(record);
}

void LogBus::log(LogLevel level, const char *category, const QString &cameraId,
                 const QString &summary, const QString &detail)
{
    LogRecord r;
    r.level = level;
    r.category = QString::fromLatin1(category);
    r.cameraId = cameraId;
    r.summary = summary;
    r.detail = detail;
    r.ok = level != LogLevel::Error;
    post(r);
}

void LogBus::debug(const char *c, const QString &id, const QString &s, const QString &d2)
{
    log(LogLevel::Debug, c, id, s, d2);
}

void LogBus::info(const char *c, const QString &id, const QString &s, const QString &d2)
{
    log(LogLevel::Info, c, id, s, d2);
}

void LogBus::warning(const char *c, const QString &id, const QString &s, const QString &d2)
{
    log(LogLevel::Warning, c, id, s, d2);
}

void LogBus::error(const char *c, const QString &id, const QString &s, const QString &d2)
{
    log(LogLevel::Error, c, id, s, d2);
}

QVector<LogRecord> LogBus::history() const
{
    QMutexLocker lock(&d->mutex);
    return d->history;
}

void LogBus::clearHistory()
{
    QMutexLocker lock(&d->mutex);
    d->history.clear();
}

void LogBus::setStdoutEnabled(bool on)
{
    d->stdoutEnabled = on;
}

void LogBus::setStdoutVerbose(bool on)
{
    d->stdoutVerbose = on;
    if (on)
        d->minimumLevel = LogLevel::Debug;
}

void LogBus::setMinimumLevel(LogLevel level)
{
    d->minimumLevel = level;
}

LogLevel LogBus::minimumLevel() const
{
    return d->minimumLevel;
}

bool LogBus::setLogFile(const QString &path, QString *errorOut)
{
    QMutexLocker lock(&d->mutex);
    if (d->file.isOpen())
        d->file.close();
    if (path.isEmpty())
        return true;

    const QFileInfo info(path);
    if (!info.absoluteDir().exists() && !QDir().mkpath(info.absolutePath())) {
        if (errorOut)
            *errorOut = QStringLiteral("无法创建日志目录：%1").arg(info.absolutePath());
        return false;
    }
    d->file.setFileName(path);
    if (!d->file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        if (errorOut)
            *errorOut = QStringLiteral("无法打开日志文件 %1：%2").arg(path, d->file.errorString());
        return false;
    }
    return true;
}

} // namespace onvifsim
