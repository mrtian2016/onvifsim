#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <QString>

#include <cstdio>

// svg2png <in.svg> <out.png> <size>
int main(int argc, char **argv)
{
    // 无头渲染：CI 和开发机上都不保证有显示服务器。
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    if (argc != 4) {
        std::fprintf(stderr, "用法: svg2png <in.svg> <out.png> <size>\n");
        return 2;
    }

    const QString input = QString::fromLocal8Bit(argv[1]);
    QSvgRenderer renderer(input);
    if (!renderer.isValid()) {
        std::fprintf(stderr, "Qt 解析不了这个 SVG: %s\n", argv[1]);
        return 1;
    }

    bool ok = false;
    const int size = QString::fromLocal8Bit(argv[3]).toInt(&ok);
    if (!ok || size <= 0 || size > 4096) {
        std::fprintf(stderr, "尺寸不合法: %s\n", argv[3]);
        return 2;
    }

    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    renderer.render(&painter);
    painter.end();

    if (!image.save(QString::fromLocal8Bit(argv[2]))) {
        std::fprintf(stderr, "写不出 %s\n", argv[2]);
        return 1;
    }
    return 0;
}
