#pragma once

// 内嵌 H.264 样片的读取与打包。
//
// 样片是 Annex B 裸流，三档（360p / 720p / 1080p），循环播放。
// 循环时时间戳连续递增、不回绕。SPS/PPS 的放置策略见 quirk SpsPpsPlacement。

#include "media/MediaTypes.h"

#include <QtCore/QtGlobal>
#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QVector>

namespace onvifsim {

class H264Source
{
public:
    H264Source();
    ~H264Source();

    // asset 可以是内嵌档位名（"360p" / "720p" / "1080p" / "black" / "freeze" / "noise"），
    // 也可以是外部 .h264 文件的绝对路径。
    bool load(const QString &asset, QString *errorOut = nullptr);
    bool isLoaded() const;
    QString assetName() const;

    int width() const;
    int height() const;
    double frameRate() const;
    qint64 durationUs() const;
    int frameCount() const;

    // SDP 的 sprop-parameter-sets / profile-level-id 从这里取。
    QByteArray sps() const;
    QByteArray pps() const;
    QByteArray spropParameterSets() const;   // Base64(SPS),Base64(PPS)
    QByteArray profileLevelId() const;       // 6 位十六进制

    // 取第 index 帧（循环取模）。返回的 data 是该帧的全部 NAL（Annex B）。
    EncodedFrame frame(int index) const;
    // 把一帧拆成 NAL 列表（去起始码）。
    static QVector<QByteArray> splitNalUnits(const QByteArray &annexB);

    // RFC 6184 打包：小于 mtu 走单 NAL，否则 FU-A 分片。
    static QVector<QByteArray> packetize(const QByteArray &nal, int mtu = 1400);

    // 内嵌样片清单。
    static QStringList builtinAssets();

private:
    // 裸 Private* + 析构里 delete d，默认拷贝构造会让两个对象指向同一块内存，
    // 第二次析构就是 double free —— 而编译器一个字都不会说。项目里其余 PIMPL
    // 类都继承自 QObject（天生禁拷贝），只有这几个是裸的。
    Q_DISABLE_COPY(H264Source)

    struct Private;
    Private *d;
};

} // namespace onvifsim
