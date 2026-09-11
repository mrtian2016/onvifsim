#pragma once

// 根对象：相机集合 + 发现响应器 + REST 控制面 + 日志总线。
// GUI 模式与 headless 模式共用同一个 Simulator，GUI 只是观察者。

#include "core/CameraModel.h"
#include "core/LogBus.h"
#include "core/Quirks.h"

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtNetwork/QHostAddress>

namespace onvifsim {

class ControlApi;
class DiscoveryResponder;
class VirtualCamera;
struct Scenario;

// 一个进程装 N 台相机的三种做法，见 plan.md §3.3。
enum class NetworkMode {
    Ports,      // 同一 IP，端口递增。默认，不需要任何权限。
    IpAlias,    // 宿主网卡上的 IP 别名，各绑标准端口。加别名要一次提权。
    External,   // 地址由外部（Docker macvlan 等）安排好，程序只负责绑定。
};

struct SimulatorConfig {
    NetworkMode networkMode = NetworkMode::Ports;
    QHostAddress bindAddress = QHostAddress(QHostAddress::AnyIPv4);
    quint16 httpBasePort = 8000;
    quint16 rtspBasePort = 8554;

    bool discoveryEnabled = true;
    QString discoveryInterface;      // 空 = 全部网卡

    bool controlApiEnabled = true;
    QHostAddress controlApiAddress = QHostAddress(QHostAddress::LocalHost);
    quint16 controlApiPort = 9000;
    QString controlApiToken;         // 空 = 不校验

    QString assetsDirectory;         // 覆盖内嵌资源，空 = 用编译进来的
    QString logFile;
};

class Simulator : public QObject
{
    Q_OBJECT
public:
    explicit Simulator(QObject *parent = nullptr);
    ~Simulator() override;

    const SimulatorConfig &config() const;
    void setConfig(const SimulatorConfig &config);

    LogBus *logBus() const;
    DiscoveryResponder *discovery() const;
    ControlApi *controlApi() const;

    // ---- 相机管理 ----
    QList<VirtualCamera *> cameras() const;
    VirtualCamera *camera(const QString &id) const;
    int cameraCount() const;

    // 按预设新建一台，端口按 networkMode 自动分配。
    VirtualCamera *addCamera(const QString &personaKey = QStringLiteral("generic"));
    VirtualCamera *addCamera(const CameraModel &model);
    bool removeCamera(const QString &id);
    void removeAllCameras();

    // ---- 生命周期 ----
    bool start(QString *errorOut = nullptr);
    void stop();
    bool isRunning() const;

    // ---- 场景 ----
    bool loadScenario(const QString &path, QStringList *errors = nullptr);
    // 套用一份已经解析好的场景（REST 把整份 JSON POST 进来时走这条）。
    // 和从文件加载**必须是同一条路** —— 分成两份实现的结果是：文件加载那边
    // 把场景 quirk 叠在预设之上，REST 这边整体替换，同一份 JSON 两种行为，
    // 而 e2e 全靠 REST 铺场景，测出来的和用户实际看到的就对不上了。
    bool applyScenario(const Scenario &scenario, QString *errorOut = nullptr);
    bool saveScenario(const QString &path, QString *errorOut = nullptr) const;

    // 全局 quirk 默认值：新建相机时作为初值，也可一键应用到全部相机。
    const Quirks &globalQuirks() const;
    void setGlobalQuirks(const Quirks &quirks);
    void applyGlobalQuirksToAll();

    QString nextCameraId() const;

signals:
    void cameraAdded(onvifsim::VirtualCamera *camera);
    void cameraAboutToBeRemoved(onvifsim::VirtualCamera *camera);
    void cameraRemoved(const QString &id);
    void started();
    void stopped();
    void configChanged();
    // 全局默认 quirk 变了。per-camera 有 VirtualCamera::quirksChanged，
    // 全局这份原来没有信号，REST 改完 GUI 的「全局默认」视图不会跟着刷新。
    void globalQuirksChanged();

private:
    struct Private;
    Private *d;
};

} // namespace onvifsim
