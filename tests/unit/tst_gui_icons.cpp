// 程序图标的资源接线测试。
//
// 图标接错是「编译通过、测试通过、界面上一片空白」的典型：qrc 里改个别名、
// 静态库少一次 Q_INIT_RESOURCE，QIcon 都只会安静地给出空图，直到用户看见
// 标题栏上一张白纸。这里把这条路径钉死。

#include "gui/GuiUtil.h"

#include <QtGui/QPixmap>
#include <QtTest/QtTest>

using onvifsim::gui::util::appIcon;

class TestGuiIcons : public QObject
{
    Q_OBJECT

private slots:
    void iconIsNotEmpty();
    void everyDeclaredSizeIsPresent();
    void smallSizesAreOpaque();
};

void TestGuiIcons::iconIsNotEmpty()
{
    QVERIFY(!appIcon().isNull());
    QVERIFY(!appIcon().availableSizes().isEmpty());
}

void TestGuiIcons::everyDeclaredSizeIsPresent()
{
    // 与 src/gui/icons.qrc 一一对应。少一档不会报错，只会让 QIcon 在那个
    // 尺寸上去缩放别的图，糊给用户看 —— 所以逐档确认原图存在。
    const QList<int> expected{16, 24, 32, 48, 64, 128, 256};
    const QList<QSize> available = appIcon().availableSizes();
    for (int size : expected)
        QVERIFY2(available.contains(QSize(size, size)),
                 qPrintable(QStringLiteral("图标缺少 %1x%1 这一档").arg(size)));
}

void TestGuiIcons::smallSizesAreOpaque()
{
    // 托盘那一档最容易出事：渲染失败时 QIcon 给的是一张全透明图，
    // 在托盘里表现为「图标不见了」而不是报错。
    const QPixmap pixmap = appIcon().pixmap(QSize(16, 16));
    QCOMPARE(pixmap.size(), QSize(16, 16));
    const QImage image = pixmap.toImage();
    QVERIFY(!image.isNull());
    QVERIFY(qAlpha(image.pixel(8, 8)) > 0);
}

QTEST_MAIN(TestGuiIcons)
#include "tst_gui_icons.moc"
