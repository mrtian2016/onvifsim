#pragma once

// 结构化日志总线。请求处理链上每一步都往这里发一条记录，
// GUI 的日志面板、stdout、文件、REST 的 SSE 流都是它的订阅者。

#include <QtCore/QDateTime>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QVector>

namespace onvifsim {

enum class LogLevel { Debug, Info, Warning, Error };

// 记录归属的子系统。GUI 按它过滤。
namespace logcat {
inline constexpr const char *Http = "http";
inline constexpr const char *Soap = "soap";
inline constexpr const char *Rtsp = "rtsp";
inline constexpr const char *Rtp = "rtp";
inline constexpr const char *Discovery = "discovery";
inline constexpr const char *Event = "event";
inline constexpr const char *Ptz = "ptz";
inline constexpr const char *Imaging = "imaging";
inline constexpr const char *Media = "media";
inline constexpr const char *Vendor = "vendor";
inline constexpr const char *Control = "control";
inline constexpr const char *Core = "core";
inline constexpr const char *Quirk = "quirk";
} // namespace logcat

struct LogRecord {
    QDateTime timestamp = QDateTime::currentDateTimeUtc();
    LogLevel level = LogLevel::Info;
    QString cameraId;    // 空 = 全局记录
    QString category;    // 见 logcat
    QString peer;        // 客户端地址，形如 192.168.1.10:51234
    QString summary;     // 一行摘要，GUI 列表直接显示
    QString detail;      // SOAP 原文 / 完整报文，GUI 里可折叠展开
    qint64 durationUs = -1;  // 处理耗时，<0 表示不适用
    bool ok = true;          // false 会在 GUI 里标红
    QString quirkKey;        // 由某个 quirk 促成时填其 key，便于「这条怪异行为是我开的」

    QString levelName() const;
};

class LogBus : public QObject
{
    Q_OBJECT
public:
    explicit LogBus(QObject *parent = nullptr);
    ~LogBus() override;

    // 进程内单例。Simulator 构造时会把自己的实例设为全局实例。
    static LogBus *global();
    static void setGlobal(LogBus *bus);

    void post(const LogRecord &record);

    // 便捷入口。cameraId 传空表示全局。
    void log(LogLevel level, const char *category, const QString &cameraId,
             const QString &summary, const QString &detail = QString());
    void debug(const char *category, const QString &cameraId, const QString &summary,
               const QString &detail = QString());
    void info(const char *category, const QString &cameraId, const QString &summary,
              const QString &detail = QString());
    void warning(const char *category, const QString &cameraId, const QString &summary,
                 const QString &detail = QString());
    void error(const char *category, const QString &cameraId, const QString &summary,
               const QString &detail = QString());

    QVector<LogRecord> history() const;
    void clearHistory();

    // headless 模式下把记录打到 stdout。
    void setStdoutEnabled(bool on);
    void setStdoutVerbose(bool on);  // 连 detail 一起打

    void setMinimumLevel(LogLevel level);
    LogLevel minimumLevel() const;

    // 落盘。路径为空表示关闭。
    bool setLogFile(const QString &path, QString *errorOut = nullptr);

signals:
    void recordPosted(const onvifsim::LogRecord &record);

private:
    struct Private;
    Private *d;
};

} // namespace onvifsim

Q_DECLARE_METATYPE(onvifsim::LogRecord)
