#pragma once

// 左栏相机列表：图标 + 两行状态（在线 / 流数 / 订阅数 / 最近事件）。
//
// 列表项用两行文字而不是自定义 delegate：信息量刚好，改起来也便宜。
// 刷新由 MainWindow 的一秒定时器驱动，且只改已有项的文字，
// 不重建整张表 —— 重建会把用户的选中状态和滚动位置一起弄丢。

#include <QtWidgets/QWidget>

class QListWidget;
class QListWidgetItem;

namespace onvifsim {

class Simulator;
class VirtualCamera;

namespace gui {

class CameraListWidget : public QWidget
{
    Q_OBJECT
public:
    explicit CameraListWidget(Simulator *simulator, QWidget *parent = nullptr);

    VirtualCamera *currentCamera() const;
    void selectCamera(const QString &id);
    void refresh();

signals:
    void cameraSelected(onvifsim::VirtualCamera *camera);
    // 请求 MainWindow 弹「添加相机」菜单（工具栏那份逻辑不重复写一遍）。
    void addCameraRequested();

private slots:
    void onCameraAdded(onvifsim::VirtualCamera *camera);
    void onCameraRemoved(const QString &id);
    void showContextMenu(const QPoint &pos);

private:
    void rebuild();
    void updateItem(QListWidgetItem *item, VirtualCamera *camera);
    VirtualCamera *cameraOf(QListWidgetItem *item) const;

    Simulator *m_simulator = nullptr;
    QListWidget *m_list = nullptr;
};

} // namespace gui
} // namespace onvifsim
