#include "media/Snapshot.h"

#include "core/CameraModel.h"
#include "core/Quirks.h"
#include "core/VirtualCamera.h"

#include <QtCore/QAtomicInt>
#include <QtCore/QBuffer>
#include <QtCore/QDateTime>
#include <QtGui/QFont>
#include <QtGui/QFontMetrics>
#include <QtGui/QImage>
#include <QtGui/QLinearGradient>
#include <QtGui/QPainter>
#include <QtGui/QPen>

#include <cmath>

namespace onvifsim {
namespace snapshot {
namespace {

// REST 与 SOAP 都能把尺寸喂进来，得有个上限：一个手滑的 99999x99999 会直接吃光内存。
constexpr int kMaxDimension = 7680;
constexpr int kMinDimension = 16;
// 只卡单边不够：7680×7680 的 RGB888 是 177 MB，而且**每次快照请求同步分配一次**
// 再做 JPEG 编码。尺寸来自 VideoEncoderConfiguration，而那个是
// SetVideoEncoderConfiguration 无界写进去的，等于把分配量交给了客户端。
// 按总像素再卡一道，超了等比缩到 4K 以内。
constexpr qint64 kMaxPixels = 3840LL * 2160LL;

// 「客户端有没有真的重新拉快照」是这个模拟器要暴露的一个高频问题。
// 时间戳只到毫秒，连着两次调用有可能落在同一毫秒里，那样两张图会一模一样、
// 测试就不稳。再叠一个进程内自增序号，保证「每次内容都不同」是硬保证而不是概率。
QAtomicInt g_sequence(0);

QFont pickFont(int pixelSize, bool bold)
{
    // 不写死字体名（三平台都不一样），只给 styleHint，让 Qt 去挑一个等宽字体。
    // 等宽是为了时间戳的数字不左右抖动，肉眼一眼就能看出画面在不在刷新。
    QFont font;
    font.setStyleHint(QFont::Monospace);
    font.setFamily(QStringLiteral("monospace"));
    font.setPixelSize(qMax(8, pixelSize));
    font.setBold(bold);
    return font;
}

void drawLabel(QPainter &painter, const QRect &rect, Qt::Alignment alignment,
               const QString &text, const QFont &font, const QColor &color)
{
    if (text.isEmpty())
        return;
    painter.setFont(font);
    const QFontMetrics metrics(font);
    QRect box = metrics.boundingRect(rect, static_cast<int>(alignment), text);
    box.adjust(-metrics.height() / 3, -metrics.height() / 6,
               metrics.height() / 3, metrics.height() / 6);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 150));
    painter.drawRoundedRect(box, 6, 6);
    painter.setPen(color);
    painter.drawText(rect, static_cast<int>(alignment), text);
}

void paintScene(QImage &image, const SnapshotOptions &options, int sequence)
{
    const int w = image.width();
    const int h = image.height();

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // 背景做成与 testsrc2 样片同一路数的深色渐变，快照和视频放一起不会显得是两台设备。
    QLinearGradient gradient(0, 0, w, h);
    gradient.setColorAt(0.0, QColor(0x10, 0x1c, 0x30));
    gradient.setColorAt(0.5, QColor(0x14, 0x3a, 0x4a));
    gradient.setColorAt(1.0, QColor(0x08, 0x12, 0x1e));
    painter.fillRect(image.rect(), gradient);

    // 网格：客户端做缩放/裁剪时，格子变形一眼可见。
    const int step = qMax(24, w / 16);
    painter.setPen(QPen(QColor(255, 255, 255, 28), 1));
    for (int x = step; x < w; x += step)
        painter.drawLine(x, 0, x, h);
    for (int y = step; y < h; y += step)
        painter.drawLine(0, y, w, y);
    painter.setPen(QPen(QColor(255, 255, 255, 70), 2));
    painter.drawRect(QRect(1, 1, w - 3, h - 3));

    const int margin = qMax(8, h / 40);
    const QRect inner = image.rect().adjusted(margin, margin, -margin, -margin);

    if (!options.cameraName.isEmpty()) {
        drawLabel(painter, inner, Qt::AlignLeft | Qt::AlignTop, options.cameraName,
                  pickFont(qMax(12, h / 22), true), QColor(0xe8, 0xf4, 0xff));
    }
    if (!options.profileName.isEmpty()) {
        drawLabel(painter, inner, Qt::AlignRight | Qt::AlignTop, options.profileName,
                  pickFont(qMax(11, h / 28), false), QColor(0x9f, 0xe8, 0xc0));
    }

    if (options.showTimestamp) {
        // 全部走 UTC（CLAUDE.md 硬性约束），并且精确到毫秒 —— 客户端缓存了旧快照的话，
        // 把两张图并排一看毫秒不动就实锤了。
        const QDateTime now = options.timestamp.isValid() ? options.timestamp
                                                          : QDateTime::currentDateTimeUtc();
        const QString stamp = now
                                  .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
            + QStringLiteral(" UTC");
        drawLabel(painter, inner, Qt::AlignHCenter | Qt::AlignVCenter, stamp,
                  pickFont(qMax(14, h / 12), true), QColor(0xff, 0xff, 0xff));
    }

    const QString footer = QStringLiteral("%1x%2  #%3")
                               .arg(w)
                               .arg(h)
                               .arg(sequence, 6, 10, QLatin1Char('0'));
    drawLabel(painter, inner, Qt::AlignHCenter | Qt::AlignBottom, footer,
              pickFont(qMax(11, h / 30), false), QColor(0xc8, 0xd8, 0xe8));

    // 沿边框绕圈走的方块，画在最后，免得被角上的文字标签盖住。
    // 快照被客户端缩成缩略图、文字全糊掉的时候，就靠它看出画面到底刷没刷新。
    const int marker = qMax(8, h / 24);
    const int steps = qMax(1, 2 * (w + h) / marker);
    const double ratio = static_cast<double>(sequence % steps) / steps;
    int markerX = 0;
    int markerY = 0;
    if (ratio < 0.25) {
        markerX = static_cast<int>(ratio * 4.0 * (w - marker));
    } else if (ratio < 0.5) {
        markerX = w - marker;
        markerY = static_cast<int>((ratio - 0.25) * 4.0 * (h - marker));
    } else if (ratio < 0.75) {
        markerX = w - marker - static_cast<int>((ratio - 0.5) * 4.0 * (w - marker));
        markerY = h - marker;
    } else {
        markerY = h - marker - static_cast<int>((ratio - 0.75) * 4.0 * (h - marker));
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0xff, 0xd5, 0x4f));
    painter.drawRect(QRect(markerX, markerY, marker, marker));
}

} // namespace

QByteArray render(const SnapshotOptions &options)
{
    QSize size = options.size;
    if (size.width() < kMinDimension || size.height() < kMinDimension)
        size = QSize(1920, 1080);
    size = size.boundedTo(QSize(kMaxDimension, kMaxDimension));
    const qint64 pixels = qint64(size.width()) * qint64(size.height());
    if (pixels > kMaxPixels) {
        // 等比缩到总像素上限之内 —— 客户端拿到的图仍然「像」它要的那张，只是小一号，
        // 比直接拒绝或者一次吃掉几百 MB 都好。
        const double factor = std::sqrt(double(kMaxPixels) / double(pixels));
        size = QSize(qMax(kMinDimension, int(size.width() * factor)),
                     qMax(kMinDimension, int(size.height() * factor)));
    }

    // RGB888 而不是 ARGB32：JPEG 本来就没有 alpha，少一次格式转换。
    QImage image(size, QImage::Format_RGB888);
    if (image.isNull())
        return QByteArray();

    const int sequence = g_sequence.fetchAndAddOrdered(1);
    if (options.black) {
        // quirk VideoBlack：整幅纯黑，连时间戳都不画 —— 客户端的黑屏检测就该在这里报警。
        image.fill(Qt::black);
    } else {
        paintScene(image, options, sequence);
    }

    QByteArray out;
    QBuffer buffer(&out);
    if (!buffer.open(QIODevice::WriteOnly))
        return QByteArray();
    if (!image.save(&buffer, "JPEG", qBound(1, options.jpegQuality, 100)))
        return QByteArray();
    buffer.close();
    return out;
}

QByteArray renderFor(const VirtualCamera *camera, const QString &profileToken)
{
    SnapshotOptions options;
    if (!camera) {
        options.profileName = profileToken;
        return render(options);
    }

    const CameraModel &model = camera->model();
    options.cameraName = model.displayName;

    const MediaProfile *profile = model.profileByToken(profileToken);
    if (!profile && !model.profiles.isEmpty())
        profile = &model.profiles.first();   // token 不认识就给主码流，别回空图

    if (profile) {
        options.profileName = profile->name;
        // 快照尺寸跟着 profile 的编码分辨率走 —— 客户端会拿它和码流对比。
        options.size = QSize(profile->videoEncoder.width, profile->videoEncoder.height);
    } else {
        options.profileName = profileToken;
    }

    // quirk：黑屏。客户端的黑屏检测正是拿快照做的，所以这里必须真的出全黑图。
    options.black = camera->quirks().isEnabled(QuirkId::VideoBlack);

    // 时间戳用设备自己的时钟：A8 注入的时钟偏移要在画面上看得见，
    // 这样排查「客户端说时间对不上」时一眼就能确认是设备侧偏了。
    options.timestamp = camera->deviceTimeUtc();

    return render(options);
}

} // namespace snapshot
} // namespace onvifsim
