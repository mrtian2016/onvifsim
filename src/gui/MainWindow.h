#pragma once

// 主窗口：工具栏 + 左栏相机列表 + 右栏 8 个 Tab + 底部日志面板。
//
// 主窗口是 Simulator 的**观察者**：它只调 Simulator / VirtualCamera 的公开接口，
// 一行协议逻辑都不碰。这样 headless 版本才能在没有 Widgets 的容器里编出来。
//
// 定时刷新只驱动「当前可见」的那个 Tab 与左栏列表，隐藏的 Tab 不跑代码。

#include <QtCore/QVector>
#include <QtWidgets/QMainWindow>

class QAction;
class QLabel;
class QMenu;
class QSplitter;
class QSystemTrayIcon;
class QTabWidget;
class QTimer;

namespace onvifsim {

class IpAlias;
class Simulator;
class VirtualCamera;

namespace gui {

class CameraListWidget;
class CameraTab;
class LogPanel;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(Simulator *simulator, QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

    // 带着原来的命令行参数重开一个自己，然后退出当前进程。
    // 界面语言只能在 QApplication 建起来时装翻译器，改完必须重启才生效。
    void restartApplication();

private slots:
    void addCamera(const QString &personaKey);
    void showAddCameraMenu();
    void toggleAll();
    void openScenario();
    void openScenarioPath(const QString &path);
    void saveScenario();
    void showNetworkDialog();
    void showSettings();
    void onCameraSelected(onvifsim::VirtualCamera *camera);
    void tick();

private:
    void createToolBar();
    void createCentralArea();
    void createTrayIcon(bool enabled);
    void rebuildScenarioMenu();
    void rememberScenario(const QString &path);
    void restoreLayout();
    void storeLayout();
    void reportStartFailure(const QString &error);

    Simulator *m_simulator = nullptr;
    IpAlias *m_ipAlias = nullptr;

    CameraListWidget *m_cameraList = nullptr;
    QTabWidget *m_tabs = nullptr;
    QVector<CameraTab *> m_cameraTabs;
    LogPanel *m_logPanel = nullptr;

    QSplitter *m_mainSplitter = nullptr;   // 左右
    QSplitter *m_outerSplitter = nullptr;  // 上下（内容 / 日志）

    QAction *m_addCameraAction = nullptr;
    QAction *m_removeCameraAction = nullptr;
    QAction *m_toggleAllAction = nullptr;
    QAction *m_openScenarioAction = nullptr;
    QAction *m_saveScenarioAction = nullptr;
    QMenu *m_scenarioMenu = nullptr;

    QLabel *m_statusLabel = nullptr;
    QTimer *m_timer = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    // 托盘开着时点窗口的 X 只是收起来，真退出要走托盘菜单的「退出」
    // —— 模拟器是常驻服务，关掉窗口就把相机全停了不合理。
    // （界面上没有菜单栏，全部操作都在工具栏与托盘菜单上。）
    bool m_reallyQuit = false;
    bool m_trayHintShown = false;
};

} // namespace gui
} // namespace onvifsim
