#pragma once

// 快照生成：QImage 画一张与样片同风格的图，叠当前时间与相机名，Qt 自带 JPEG 编码。
// 每次内容不同，正好测「客户端快照有没有刷新」。

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QSize>
#include <QtCore/QString>

namespace onvifsim {

class VirtualCamera;

struct SnapshotOptions {
    QSize size = QSize(1920, 1080);
    QString cameraName;
    QString profileName;
    bool showTimestamp = true;
    bool black = false;      // quirk VideoBlack
    int jpegQuality = 80;
    // 画在图上的时间。留空表示用当前 UTC；renderFor() 会填设备自己的时钟，
    // 这样 quirk A8 注入的时钟偏移在快照上也看得见。
    QDateTime timestamp;
};

namespace snapshot {

QByteArray render(const SnapshotOptions &options);
// 按相机与 profile 生成（自动填尺寸、名字，并处理 black / freeze 类 quirk）。
QByteArray renderFor(const VirtualCamera *camera, const QString &profileToken);

} // namespace snapshot
} // namespace onvifsim
