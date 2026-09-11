#include "gui/MainWindow.h"

#include "core/Persona.h"
#include "core/Scenario.h"
#include "core/Simulator.h"
#include "core/VirtualCamera.h"
#include "gui/CameraListWidget.h"
#include "gui/I18n.h"
#include "gui/GuiUtil.h"
#include "gui/LogPanel.h"
#include "gui/NetworkDialog.h"
#include "gui/tabs/ConnectionsTab.h"
#include "gui/tabs/EventsTab.h"
#include "gui/tabs/IdentityTab.h"
#include "gui/tabs/OverviewTab.h"
#include "gui/tabs/PtzTab.h"
#include "gui/tabs/QuirksTab.h"
#include "gui/tabs/StreamsTab.h"
#include "gui/tabs/TalkbackTab.h"
#include "net/IpAlias.h"

#include "version.h"

#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QCloseEvent>
#include <QtGui/QCursor>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QStyle>
#include <QtWidgets/QSystemTrayIcon>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

namespace onvifsim {
namespace gui {

namespace {

constexpr int kRefreshIntervalMs = 1000;
constexpr int kMaxRecentScenarios = 8;

QIcon standardIcon(QStyle::StandardPixmap pixmap)
{
    return QApplication::style()->standardIcon(pixmap);
}

} // namespace

MainWindow::MainWindow(Simulator *simulator, QWidget *parent)
    : QMainWindow(parent)
    , m_simulator(simulator)
{
    setWindowTitle(tr("onvifsim %1 —— ONVIF 摄像头模拟器")
                       .arg(QString::fromLatin1(ONVIFSIM_VERSION)));
    setWindowIcon(util::appIcon());

    // 别名对象要活得比对话框长：本进程加过哪些别名得记着，退出时统一回收。
    m_ipAlias = new IpAlias(this);

    createToolBar();
    createCentralArea();

    m_statusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusLabel);

    QSettings settings;
    // 默认开托盘：模拟器是常驻服务，关掉窗口不该把相机一起停了。
    // 开着托盘时点 X 只收窗口（见 closeEvent），真退出走托盘菜单。
    //
    // 这个默认值是后来从「关」改成「开」的，而改默认值对**已经存过配置**的用户
    // 完全不生效 —— 他们的 QSettings 里躺着旧的 false，表现就是「说好的托盘呢」。
    // 所以做一次性迁移：老配置强制拉到新默认，之后再听用户的。
    if (!settings.value(QStringLiteral("ui/trayDefaultV2"), false).toBool()) {
        settings.setValue(QStringLiteral("ui/trayDefaultV2"), true);
        settings.setValue(QStringLiteral("ui/tray"), true);
    }
    createTrayIcon(settings.value(QStringLiteral("ui/tray"), true).toBool());
    m_logPanel->setDisplayLimit(settings.value(QStringLiteral("log/limit"), 2000).toInt());
    if (m_simulator) {
        m_simulator->logBus()->setMinimumLevel(static_cast<LogLevel>(
            settings.value(QStringLiteral("log/minLevel"),
                           static_cast<int>(LogLevel::Debug)).toInt()));
    }

    m_timer = new QTimer(this);
    m_timer->setInterval(kRefreshIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &MainWindow::tick);
    m_timer->start();

    if (m_simulator) {
        connect(m_simulator, &Simulator::started, this, &MainWindow::tick);
        connect(m_simulator, &Simulator::stopped, this, &MainWindow::tick);
        // 相机就要没了：先把各 Tab 的指针摘掉，免得刷新时用到野指针。
        connect(m_simulator, &Simulator::cameraAboutToBeRemoved, this,
                [this](VirtualCamera *camera) {
                    if (!m_cameraTabs.isEmpty() && m_cameraTabs.first()->camera() == camera)
                        onCameraSelected(nullptr);
                });
    }

    restoreLayout();
    onCameraSelected(m_cameraList->currentCamera());
    tick();
}

MainWindow::~MainWindow()
{
    // 独立 IP 模式下加过的别名是对系统的真实改动，退出时必须收回去。
    if (m_ipAlias)
        m_ipAlias->removeAllOwned();
}

void MainWindow::createToolBar()
{
    auto *toolBar = addToolBar(tr("主工具栏"));
    toolBar->setObjectName(QStringLiteral("mainToolBar"));
    toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolBar->setMovable(false);

    m_addCameraAction = toolBar->addAction(standardIcon(QStyle::SP_FileDialogNewFolder),
                                           tr("添加相机"));
    m_addCameraAction->setToolTip(tr("按品牌预设新建一台相机"));
    connect(m_addCameraAction, &QAction::triggered, this, &MainWindow::showAddCameraMenu);
    // 工具栏按钮要能直接弹出预设菜单，所以拿到 widget 再挂一个 menu。
    if (auto *button = qobject_cast<QToolButton *>(toolBar->widgetForAction(m_addCameraAction))) {
        auto *menu = new QMenu(button);
        for (const Persona &persona : PersonaRegistry::all()) {
            const QString key = persona.key;
            QAction *action = menu->addAction(i18n::personaName(persona));
            connect(action, &QAction::triggered, this, [this, key] { addCamera(key); });
        }
        button->setMenu(menu);
        button->setPopupMode(QToolButton::MenuButtonPopup);
    }

    m_removeCameraAction = toolBar->addAction(standardIcon(QStyle::SP_TrashIcon), tr("删除相机"));
    connect(m_removeCameraAction, &QAction::triggered, this, [this] {
        VirtualCamera *cam = m_cameraList->currentCamera();
        if (!cam || !m_simulator)
            return;
        const QString id = cam->id();
        if (QMessageBox::question(this, tr("删除相机"), tr("确定删除 %1 吗？").arg(id))
            == QMessageBox::Yes) {
            m_simulator->removeCamera(id);
        }
    });

    toolBar->addSeparator();
    m_toggleAllAction = toolBar->addAction(standardIcon(QStyle::SP_MediaPlay), tr("全部启动"));
    connect(m_toggleAllAction, &QAction::triggered, this, &MainWindow::toggleAll);

    toolBar->addSeparator();
    m_openScenarioAction = toolBar->addAction(standardIcon(QStyle::SP_DialogOpenButton),
                                              tr("加载场景"));
    connect(m_openScenarioAction, &QAction::triggered, this, &MainWindow::openScenario);
    if (auto *button =
            qobject_cast<QToolButton *>(toolBar->widgetForAction(m_openScenarioAction))) {
        m_scenarioMenu = new QMenu(button);
        button->setMenu(m_scenarioMenu);
        button->setPopupMode(QToolButton::MenuButtonPopup);
        rebuildScenarioMenu();
    }

    m_saveScenarioAction = toolBar->addAction(standardIcon(QStyle::SP_DialogSaveButton),
                                              tr("保存场景"));
    connect(m_saveScenarioAction, &QAction::triggered, this, &MainWindow::saveScenario);

    toolBar->addSeparator();
    QAction *network = toolBar->addAction(standardIcon(QStyle::SP_DriveNetIcon), tr("网络"));
    network->setToolTip(tr("网卡选择、网络模式、一键分配 IP 段"));
    connect(network, &QAction::triggered, this, &MainWindow::showNetworkDialog);

    QAction *settings = toolBar->addAction(standardIcon(QStyle::SP_FileDialogDetailedView),
                                           tr("设置"));
    connect(settings, &QAction::triggered, this, &MainWindow::showSettings);
}

void MainWindow::createCentralArea()
{
    m_outerSplitter = new QSplitter(Qt::Vertical, this);
    m_outerSplitter->setObjectName(QStringLiteral("outerSplitter"));

    m_mainSplitter = new QSplitter(Qt::Horizontal, m_outerSplitter);
    m_mainSplitter->setObjectName(QStringLiteral("mainSplitter"));

    m_cameraList = new CameraListWidget(m_simulator, m_mainSplitter);
    m_cameraList->setMinimumWidth(200);
    m_mainSplitter->addWidget(m_cameraList);
    connect(m_cameraList, &CameraListWidget::cameraSelected, this,
            &MainWindow::onCameraSelected);
    connect(m_cameraList, &CameraListWidget::addCameraRequested, this,
            &MainWindow::showAddCameraMenu);

    m_tabs = new QTabWidget(m_mainSplitter);
    const struct {
        CameraTab *tab;
        QString title;
    } tabs[] = {
        { new OverviewTab(m_simulator, m_tabs), tr("概览") },
        { new IdentityTab(m_simulator, m_tabs), tr("身份") },
        { new StreamsTab(m_simulator, m_tabs), tr("流") },
        { new PtzTab(m_simulator, m_tabs), tr("PTZ") },
        { new EventsTab(m_simulator, m_tabs), tr("事件") },
        { new TalkbackTab(m_simulator, m_tabs), tr("对讲") },
        { new QuirksTab(m_simulator, m_tabs), tr("故障注入") },
        { new ConnectionsTab(m_simulator, m_tabs), tr("连接") },
    };
    for (const auto &entry : tabs) {
        m_tabs->addTab(entry.tab, entry.title);
        m_cameraTabs.append(entry.tab);
    }
    m_mainSplitter->addWidget(m_tabs);
    m_mainSplitter->setStretchFactor(1, 1);
    // 切到一个新 Tab 时立刻刷一次，不然要等最多一秒才有内容。
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) { tick(); });

    m_outerSplitter->addWidget(m_mainSplitter);
    m_logPanel = new LogPanel(m_simulator, m_outerSplitter);
    m_outerSplitter->addWidget(m_logPanel);
    m_outerSplitter->setStretchFactor(0, 3);
    m_outerSplitter->setStretchFactor(1, 1);

    setCentralWidget(m_outerSplitter);
}

void MainWindow::createTrayIcon(bool enabled)
{
    if (!enabled) {
        delete m_tray;
        m_tray = nullptr;
        return;
    }
    if (m_tray || !QSystemTrayIcon::isSystemTrayAvailable())
        return;

    m_tray = new QSystemTrayIcon(util::appIcon(), this);
    auto *menu = new QMenu(this);
    QAction *showAction = menu->addAction(tr("显示主窗口"));
    connect(showAction, &QAction::triggered, this, [this] {
        showNormal();
        raise();
        activateWindow();
    });
    menu->addSeparator();
    QAction *quitAction = menu->addAction(tr("退出"));
    connect(quitAction, &QAction::triggered, this, [this] {
        m_reallyQuit = true;
        // qApp->quit() 不会触发 closeEvent，而 storeLayout() 只在 closeEvent 和
        // restartApplication() 里调。托盘默认是开的、点 X 只收进托盘 —— 也就是说
        // 绝大多数用户的正常退出路径就是这里。不显式存一次的话，窗口大小、
        // 分割条位置、当前页签、日志条数上限永远存不下来。
        storeLayout();
        qApp->quit();
    });
    m_tray->setContextMenu(menu);
    m_tray->setToolTip(tr("onvifsim"));
    // 双击托盘图标恢复窗口，这是 Windows 上的惯例。
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger
                    || reason == QSystemTrayIcon::DoubleClick) {
                    showNormal();
                    raise();
                    activateWindow();
                }
            });
    m_tray->show();
}

void MainWindow::showAddCameraMenu()
{
    QMenu menu(this);
    for (const Persona &persona : PersonaRegistry::all()) {
        const QString key = persona.key;
        QAction *action = menu.addAction(i18n::personaName(persona));
        connect(action, &QAction::triggered, this, [this, key] { addCamera(key); });
    }
    menu.exec(QCursor::pos());
}

void MainWindow::addCamera(const QString &personaKey)
{
    if (!m_simulator)
        return;
    VirtualCamera *camera = m_simulator->addCamera(personaKey);
    if (!camera) {
        QMessageBox::warning(this, tr("添加相机失败"), tr("无法新建相机，详见日志。"));
        return;
    }
    // 模拟器已经在跑就把新相机也拉起来，不然用户还得再点一次「全部启动」。
    if (m_simulator->isRunning()) {
        QString error;
        if (!camera->start(&error))
            reportStartFailure(error);
    }
    m_cameraList->selectCamera(camera->id());
    tick();
}

void MainWindow::toggleAll()
{
    if (!m_simulator)
        return;
    if (m_simulator->isRunning()) {
        m_simulator->stop();
    } else {
        QString error;
        if (!m_simulator->start(&error))
            reportStartFailure(error);
    }
    tick();
}

void MainWindow::reportStartFailure(const QString &error)
{
    if (error.isEmpty())
        return;
    QMessageBox::warning(this, tr("启动时有失败"),
                         tr("%1\n\n常见原因：端口被占用，或者独立 IP 模式下别名还没建好。")
                             .arg(error));
}

void MainWindow::openScenario()
{
    QSettings settings;
    const QString last = settings.value(QStringLiteral("scenario/lastDir")).toString();
    const QString path = QFileDialog::getOpenFileName(this, tr("加载场景"), last,
                                                      tr("场景文件 (*.json)"));
    if (path.isEmpty())
        return;
    settings.setValue(QStringLiteral("scenario/lastDir"), QFileInfo(path).absolutePath());
    openScenarioPath(path);
}

void MainWindow::openScenarioPath(const QString &path)
{
    if (!m_simulator)
        return;

    QStringList errors;
    if (!m_simulator->loadScenario(path, &errors)) {
        QMessageBox::warning(this, tr("加载场景失败"), errors.join(QLatin1Char('\n')));
        return;
    }
    if (!errors.isEmpty()) {
        // 解析是「尽力而为」的：有告警也已经加载成功了，提示一下但不拦。
        QMessageBox::information(this, tr("场景已加载，但有告警"),
                                 errors.join(QLatin1Char('\n')));
    }
    rememberScenario(path);
    onCameraSelected(m_cameraList->currentCamera());
    tick();
}

void MainWindow::saveScenario()
{
    if (!m_simulator)
        return;
    QSettings settings;
    const QString last = settings.value(QStringLiteral("scenario/lastDir")).toString();
    const QString path = QFileDialog::getSaveFileName(this, tr("保存场景"),
                                                      last.isEmpty()
                                                          ? QStringLiteral("scenario.json")
                                                          : last + QStringLiteral("/scenario.json"),
                                                      tr("场景文件 (*.json)"));
    if (path.isEmpty())
        return;

    QString error;
    if (!m_simulator->saveScenario(path, &error)) {
        QMessageBox::warning(this, tr("保存场景失败"), error);
        return;
    }
    settings.setValue(QStringLiteral("scenario/lastDir"), QFileInfo(path).absolutePath());
    rememberScenario(path);
    statusBar()->showMessage(tr("已保存到 %1").arg(path), 4000);
}

void MainWindow::rememberScenario(const QString &path)
{
    QSettings settings;
    QStringList recent = settings.value(QStringLiteral("scenario/recent")).toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > kMaxRecentScenarios)
        recent.removeLast();
    settings.setValue(QStringLiteral("scenario/recent"), recent);
    settings.setValue(QStringLiteral("scenario/last"), path);
    rebuildScenarioMenu();
}

void MainWindow::rebuildScenarioMenu()
{
    if (!m_scenarioMenu)
        return;
    m_scenarioMenu->clear();

    QSettings settings;
    const QStringList recent = settings.value(QStringLiteral("scenario/recent")).toStringList();
    if (!recent.isEmpty()) {
        m_scenarioMenu->addSection(tr("最近使用"));
        for (const QString &path : recent) {
            QAction *action = m_scenarioMenu->addAction(QFileInfo(path).fileName());
            action->setToolTip(path);
            const QString target = path;
            connect(action, &QAction::triggered, this, [this, target] { openScenarioPath(target); });
        }
    }

    const QStringList builtin = Scenario::builtinNames();
    if (!builtin.isEmpty()) {
        m_scenarioMenu->addSection(tr("内置示例"));
        for (const QString &name : builtin) {
            QAction *action = m_scenarioMenu->addAction(name);
            const QString target = QStringLiteral(":/scenarios/%1.json").arg(name);
            connect(action, &QAction::triggered, this, [this, target] { openScenarioPath(target); });
        }
    }

    m_scenarioMenu->addSeparator();
    QAction *browse = m_scenarioMenu->addAction(tr("浏览…"));
    connect(browse, &QAction::triggered, this, &MainWindow::openScenario);
}

void MainWindow::showNetworkDialog()
{
    NetworkDialog dialog(m_simulator, m_ipAlias, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (!dialog.needsRestart() || !m_simulator || !m_simulator->isRunning())
        return;

    if (QMessageBox::question(this, tr("需要重启模拟器"),
                              tr("绑定地址 / 端口 / 发现设置改了，要现在停掉再启动吗？"))
        != QMessageBox::Yes) {
        return;
    }
    m_simulator->stop();
    QString error;
    if (!m_simulator->start(&error))
        reportStartFailure(error);
    tick();
}

void MainWindow::showSettings()
{
    QSettings settings;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("设置"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;

    auto *language = new QComboBox(&dialog);
    language->addItem(tr("跟随系统"), QStringLiteral("system"));
    language->addItem(QStringLiteral("简体中文"), QStringLiteral("zh_CN"));
    language->addItem(QStringLiteral("English"), QStringLiteral("en"));
    const int languageIndex =
        language->findData(settings.value(QStringLiteral("ui/language"),
                                          QStringLiteral("system")).toString());
    language->setCurrentIndex(languageIndex >= 0 ? languageIndex : 0);
    form->addRow(tr("界面语言"), language);

    auto *logLimit = new QSpinBox(&dialog);
    logLimit->setRange(200, 100000);
    logLimit->setSingleStep(500);
    logLimit->setValue(m_logPanel->displayLimit());
    form->addRow(tr("日志显示上限"), logLimit);

    auto *minLevel = new QComboBox(&dialog);
    minLevel->addItem(tr("调试"), static_cast<int>(LogLevel::Debug));
    minLevel->addItem(tr("信息"), static_cast<int>(LogLevel::Info));
    minLevel->addItem(tr("警告"), static_cast<int>(LogLevel::Warning));
    minLevel->addItem(tr("错误"), static_cast<int>(LogLevel::Error));
    if (m_simulator) {
        const int index = minLevel->findData(static_cast<int>(
            m_simulator->logBus()->minimumLevel()));
        minLevel->setCurrentIndex(index >= 0 ? index : 0);
    }
    minLevel->setToolTip(tr("这是 LogBus 层的阈值，比它低的记录连产生都不会产生。"));
    form->addRow(tr("最低日志级别"), minLevel);

    auto *tray = new QCheckBox(tr("显示系统托盘图标（关窗口时收进托盘，不退出）"), &dialog);
    tray->setChecked(settings.value(QStringLiteral("ui/tray"), true).toBool());
    tray->setEnabled(QSystemTrayIcon::isSystemTrayAvailable());
    form->addRow(QString(), tray);

    auto *autoLoad = new QCheckBox(tr("启动时自动加载上次的场景"), &dialog);
    autoLoad->setChecked(settings.value(QStringLiteral("scenario/autoLoad"), false).toBool());
    form->addRow(QString(), autoLoad);

    layout->addLayout(form);
    auto *hint = new QLabel(tr("改语言需要重启程序，确定后会问你要不要现在重启。"), &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString previousLanguage =
        settings.value(QStringLiteral("ui/language"), QStringLiteral("system")).toString();
    const QString chosenLanguage = language->currentData().toString();
    settings.setValue(QStringLiteral("ui/language"), chosenLanguage);
    settings.setValue(QStringLiteral("log/limit"), logLimit->value());
    settings.setValue(QStringLiteral("log/minLevel"), minLevel->currentData().toInt());
    settings.setValue(QStringLiteral("ui/tray"), tray->isChecked());
    settings.setValue(QStringLiteral("scenario/autoLoad"), autoLoad->isChecked());

    m_logPanel->setDisplayLimit(logLimit->value());
    if (m_simulator) {
        m_simulator->logBus()->setMinimumLevel(
            static_cast<LogLevel>(minLevel->currentData().toInt()));
    }
    createTrayIcon(tray->isChecked());

    // 翻译器只能在 QApplication 建起来时装，语言换了就得重开进程。
    // 与其让用户下次启动才看到效果，不如当场问一句。
    if (chosenLanguage != previousLanguage) {
        const auto answer = QMessageBox::question(
            this, tr("重启生效"),
            tr("界面语言已改。翻译要重启程序才能装上。\n\n"
               "现在重启吗？重启会让所有相机短暂离线（约一秒），"
               "已连接的客户端需要自己重连。"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::Yes)
            restartApplication();
    }
}

void MainWindow::restartApplication()
{
    storeLayout();

    // 原样带上命令行参数：端口、场景、绑定地址这些都在里面，丢了就等于换了台设备。
    QStringList args = QCoreApplication::arguments();
    if (args.isEmpty())
        return;
    const QString program = QCoreApplication::applicationFilePath();
    args.removeFirst();

    // 先把监听 socket 放掉再拉新进程，否则新进程抢不到同一批端口。
    if (m_simulator)
        m_simulator->stop();

    m_reallyQuit = true;
    if (!QProcess::startDetached(program, args)) {
        QMessageBox::warning(this, tr("重启失败"),
                             tr("没能重新拉起进程，请手动重开。"));
        m_reallyQuit = false;
        return;
    }
    qApp->quit();
}

void MainWindow::onCameraSelected(VirtualCamera *camera)
{
    for (CameraTab *tab : m_cameraTabs)
        tab->setCamera(camera);
    m_removeCameraAction->setEnabled(camera != nullptr);
}

void MainWindow::tick()
{
    m_cameraList->refresh();

    // 只刷新看得见的那个 Tab —— 8 个 Tab 一起轮询相机状态是纯浪费。
    if (auto *tab = qobject_cast<CameraTab *>(m_tabs->currentWidget()))
        tab->refreshNow();

    if (!m_simulator)
        return;

    int running = 0;
    int sessions = 0;
    int subscriptions = 0;
    const QList<VirtualCamera *> cameras = m_simulator->cameras();
    for (VirtualCamera *cam : cameras) {
        const CameraStatus status = cam->status();
        if (status.running && !status.offline)
            ++running;
        sessions += status.rtspSessions;
        subscriptions += status.subscriptions;
    }

    const SimulatorConfig &config = m_simulator->config();
    QString control = tr("控制面 关");
    if (config.controlApiEnabled) {
        control = tr("控制面 http://%1:%2")
                      .arg(config.controlApiAddress.toString())
                      .arg(config.controlApiPort);
    }
    m_statusLabel->setText(tr("相机 %1 · 在线 %2 · RTSP 会话 %3 · 订阅 %4 · 发现 %5 · %6")
                               .arg(cameras.size())
                               .arg(running)
                               .arg(sessions)
                               .arg(subscriptions)
                               .arg(config.discoveryEnabled ? tr("开") : tr("关"), control));

    const bool anyRunning = m_simulator->isRunning();
    m_toggleAllAction->setText(anyRunning ? tr("全部停止") : tr("全部启动"));
    m_toggleAllAction->setIcon(
        standardIcon(anyRunning ? QStyle::SP_MediaStop : QStyle::SP_MediaPlay));
}

void MainWindow::restoreLayout()
{
    QSettings settings;
    const QByteArray geometry = settings.value(QStringLiteral("ui/geometry")).toByteArray();
    if (geometry.isEmpty())
        resize(1280, 860);
    else
        restoreGeometry(geometry);
    restoreState(settings.value(QStringLiteral("ui/windowState")).toByteArray());

    const QByteArray mainState = settings.value(QStringLiteral("ui/mainSplitter")).toByteArray();
    if (!mainState.isEmpty())
        m_mainSplitter->restoreState(mainState);
    const QByteArray outerState = settings.value(QStringLiteral("ui/outerSplitter")).toByteArray();
    if (!outerState.isEmpty())
        m_outerSplitter->restoreState(outerState);
    m_tabs->setCurrentIndex(settings.value(QStringLiteral("ui/currentTab"), 0).toInt());
}

void MainWindow::storeLayout()
{
    QSettings settings;
    settings.setValue(QStringLiteral("ui/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("ui/windowState"), saveState());
    settings.setValue(QStringLiteral("ui/mainSplitter"), m_mainSplitter->saveState());
    settings.setValue(QStringLiteral("ui/outerSplitter"), m_outerSplitter->saveState());
    settings.setValue(QStringLiteral("ui/currentTab"), m_tabs->currentIndex());
    settings.setValue(QStringLiteral("log/limit"), m_logPanel->displayLimit());
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    storeLayout();

    // 托盘开着就只是收起窗口：相机还在跑，客户端那头不该因为你关了个窗口就断流。
    // 真退出走托盘菜单的「退出」。
    if (m_tray && m_tray->isVisible() && !m_reallyQuit) {
        hide();
        event->ignore();
        if (!m_trayHintShown) {
            m_trayHintShown = true;
            m_tray->showMessage(tr("onvifsim 还在后台跑"),
                                tr("相机没有停。要真正退出，右键托盘图标选「退出」。"),
                                QSystemTrayIcon::Information, 4000);
        }
        return;
    }
    QMainWindow::closeEvent(event);
}

} // namespace gui
} // namespace onvifsim
