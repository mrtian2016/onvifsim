#include "core/Simulator.h"

#include "control/ControlApi.h"
#include "core/Persona.h"
#include "core/Scenario.h"
#include "core/VirtualCamera.h"
#include "discovery/DiscoveryResponder.h"
#include "net/NetUtil.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>

namespace onvifsim {

struct Simulator::Private {
    SimulatorConfig config;
    LogBus *logBus = nullptr;
    DiscoveryResponder *discovery = nullptr;
    ControlApi *controlApi = nullptr;
    QList<VirtualCamera *> cameras;
    Quirks globalQuirks;
    bool running = false;
    int nextIndex = 0;
};

Simulator::Simulator(QObject *parent) : QObject(parent), d(new Private)
{
    d->logBus = new LogBus(this);
    // 进程内只有一个 Simulator，把它的总线设成全局的，
    // 这样深处的模块不必层层传指针也能记日志。
    if (!LogBus::global())
        LogBus::setGlobal(d->logBus);

    d->discovery = new DiscoveryResponder(this, this);
    d->controlApi = new ControlApi(this, this);
}

Simulator::~Simulator()
{
    stop();
    qDeleteAll(d->cameras);
    d->cameras.clear();
    if (LogBus::global() == d->logBus)
        LogBus::setGlobal(nullptr);
    delete d;
}

const SimulatorConfig &Simulator::config() const
{
    return d->config;
}

void Simulator::setConfig(const SimulatorConfig &config)
{
    const QString previousAssets = d->config.assetsDirectory;
    d->config = config;

    // 外部预设目录：把 *.json 覆盖进品牌预设表。要在建相机**之前**生效，
    // 所以挂在 setConfig 上而不是 start()。目录没变就不重复加载。
    if (!config.assetsDirectory.isEmpty() && config.assetsDirectory != previousAssets) {
        QStringList errors;
        const QString presetDir = QDir(config.assetsDirectory).filePath(QStringLiteral("presets"));
        // 既接受「资源根目录」（里面有 presets/），也接受直接指向 presets/ 本身 ——
        // 两种写法用户都会用，没必要为此让他读一遍文档。
        const QString target = QDir(presetDir).exists() ? presetDir : config.assetsDirectory;
        if (PersonaRegistry::loadOverrides(target, &errors)) {
            d->logBus->info(logcat::Core, QString(),
                            QStringLiteral("已从 %1 加载外部预设").arg(target));
        }
        for (const QString &error : errors)
            d->logBus->warning(logcat::Core, QString(), error);
    }

    if (!config.logFile.isEmpty()) {
        QString error;
        if (!d->logBus->setLogFile(config.logFile, &error))
            d->logBus->error(logcat::Core, QString(), error);
    }
    d->controlApi->setToken(config.controlApiToken);
    emit configChanged();
}

LogBus *Simulator::logBus() const { return d->logBus; }
DiscoveryResponder *Simulator::discovery() const { return d->discovery; }
ControlApi *Simulator::controlApi() const { return d->controlApi; }

QList<VirtualCamera *> Simulator::cameras() const
{
    return d->cameras;
}

VirtualCamera *Simulator::camera(const QString &id) const
{
    for (VirtualCamera *c : d->cameras) {
        if (c->id() == id)
            return c;
    }
    return nullptr;
}

int Simulator::cameraCount() const
{
    return d->cameras.size();
}

QString Simulator::nextCameraId() const
{
    int n = d->cameras.size() + 1;
    while (camera(QStringLiteral("cam%1").arg(n)))
        ++n;
    return QStringLiteral("cam%1").arg(n);
}

VirtualCamera *Simulator::addCamera(const QString &personaKey)
{
    int index = d->nextIndex++;
    CameraModel model = CameraModel::makeDefault(nextCameraId(), personaKey, index);

    switch (d->config.networkMode) {
    case NetworkMode::Ports: {
        // 同一 IP 上端口递增。客户端完全按 XAddr 走，功能上与独立 IP 模式等价。
        model.bindAddress = d->config.bindAddress;
        // 序号要跳过已经被别的相机占掉的端口：加载场景后序号会从 0 重新数，
        // 这时再手工加一台相机，直接用 base+index 必然和场景里的撞车。
        // 两道检查缺一不可：
        //   1. 本进程里别的相机占掉的端口（加载场景后序号会从 0 重新数，
        //      这时再手工加一台相机，直接用 base+index 必然和场景里的撞车）；
        //   2. **系统上别的进程占掉的端口**。少了这道，addCamera 会挑一个别人
        //      正在用的端口，然后 start() 失败回 409 —— 用户看到的是「加相机
        //      随机失败」，而原因在另一个毫不相干的进程上。
        auto taken = [this](quint16 http, quint16 rtsp) {
            for (const VirtualCamera *cam : d->cameras) {
                if (cam->model().httpPort == http || cam->model().rtspPort == rtsp
                    || cam->model().httpPort == rtsp || cam->model().rtspPort == http) {
                    return true;
                }
            }
            const QHostAddress &bind = d->config.bindAddress;
            return netutil::findFreePort(bind, http, 1) != http
                || netutil::findFreePort(bind, rtsp, 1) != rtsp;
        };
        while (index < 65535
               && taken(static_cast<quint16>(d->config.httpBasePort + index),
                        static_cast<quint16>(d->config.rtspBasePort + index))) {
            ++index;
        }
        d->nextIndex = index + 1;
        model.httpPort = static_cast<quint16>(d->config.httpBasePort + index);
        model.rtspPort = static_cast<quint16>(d->config.rtspBasePort + index);
        break;
    }
    case NetworkMode::IpAlias:
        // 地址由调用方（CLI 的 --ip-range / REST 的 /api/network）事先分配好，
        // 这里只给标准端口；绑不上会在 start() 时报错，不静默降级。
        model.httpPort = 80;
        model.rtspPort = 554;
        break;
    case NetworkMode::External:
        model.bindAddress = d->config.bindAddress;
        model.httpPort = d->config.httpBasePort;
        model.rtspPort = d->config.rtspBasePort;
        break;
    }

    return addCamera(model);
}

VirtualCamera *Simulator::addCamera(const CameraModel &model)
{
    CameraModel m = model;
    if (m.id.isEmpty())
        m.id = nextCameraId();
    if (camera(m.id)) {
        d->logBus->error(logcat::Core, m.id, QStringLiteral("Duplicate camera id, ignored"));
        return nullptr;
    }

    auto *cam = new VirtualCamera(m, this, this);
    // 全局 quirk 作为新相机的初值，叠在预设自带的那些之上。
    for (QuirkId id : d->globalQuirks.enabledIds()) {
        cam->mutableQuirks().setEnabled(id, true);
        for (const QuirkParamDef &p : QuirkRegistry::def(id).params) {
            cam->mutableQuirks().setParam(id, p.name, d->globalQuirks.param(id, p.name));
        }
    }
    d->cameras.append(cam);

    d->logBus->info(logcat::Core, cam->id(),
                    QStringLiteral("Added (persona %1)").arg(m.personaKey));
    emit cameraAdded(cam);
    return cam;
}

bool Simulator::removeCamera(const QString &id)
{
    VirtualCamera *cam = camera(id);
    if (!cam)
        return false;

    emit cameraAboutToBeRemoved(cam);
    cam->stop();
    d->cameras.removeOne(cam);
    cam->deleteLater();
    d->logBus->info(logcat::Core, id, QStringLiteral("Removed"));
    emit cameraRemoved(id);
    return true;
}

void Simulator::removeAllCameras()
{
    const QList<VirtualCamera *> copy = d->cameras;
    for (VirtualCamera *cam : copy)
        removeCamera(cam->id());
    d->nextIndex = 0;
}

bool Simulator::start(QString *errorOut)
{
    if (d->running)
        return true;

    QStringList failures;

    // 发现要先于相机起来：VirtualCamera::start() 会发 Hello，
    // 那时 responder 还没监听的话这条通告就白发了。
    if (d->config.discoveryEnabled) {
        QString error;
        if (!d->discovery->start(d->config.discoveryInterface, &error))
            failures.append(QStringLiteral("WS-Discovery: %1").arg(error));
    }

    for (VirtualCamera *cam : d->cameras) {
        if (!cam->model().enabled)
            continue;
        QString error;
        if (!cam->start(&error))
            failures.append(QStringLiteral("%1: %2").arg(cam->id(), error));
    }

    if (d->config.controlApiEnabled) {
        QString error;
        if (!d->controlApi->start(d->config.controlApiAddress, d->config.controlApiPort, &error))
            failures.append(QStringLiteral("控制面: %1").arg(error));
    }

    d->running = true;
    emit started();

    if (!failures.isEmpty()) {
        const QString message = failures.join(QStringLiteral("；"));
        d->logBus->error(logcat::Core, QString(), QStringLiteral("启动时有失败：%1").arg(message));
        if (errorOut)
            *errorOut = message;
        return false;
    }
    return true;
}

void Simulator::stop()
{
    if (!d->running)
        return;
    d->controlApi->stop();
    // 相机要先停：VirtualCamera::stop() 发的 Bye 得赶在 responder 拆掉 socket 之前出去，
    // 否则客户端只能靠超时才发现设备没了。
    for (VirtualCamera *cam : d->cameras)
        cam->stop();
    d->discovery->stop();
    d->running = false;
    emit stopped();
}

bool Simulator::isRunning() const
{
    return d->running;
}

bool Simulator::loadScenario(const QString &path, QStringList *errors)
{
    Scenario scenario;
    if (!Scenario::load(path, &scenario, errors))
        return false;

    QString error;
    const bool ok = applyScenario(scenario, &error);

    d->logBus->info(logcat::Core, QString(),
                    QStringLiteral("已加载场景 %1（%2 台相机）")
                        .arg(QFileInfo(path).fileName())
                        .arg(scenario.cameras.size()));
    if (!ok && errors)
        errors->append(error);
    return ok;
}

bool Simulator::applyScenario(const Scenario &scenario, QString *errorOut)
{
    const bool wasRunning = d->running;
    stop();
    removeAllCameras();

    setConfig(scenario.config);
    setGlobalQuirks(scenario.globalQuirks);

    for (const ScenarioCamera &sc : scenario.cameras) {
        VirtualCamera *cam = addCamera(sc.model);
        if (!cam)
            continue;
        // 场景里写的 quirk 叠加在预设自带的那些之上，不是整体替换 ——
        // 否则一个只想调一条 quirk 的场景会把品牌预设的默认行为全抹掉
        //（比如海康的 Media2 排前、大华的对讲双轨）。
        // 想关掉预设自带的某条，在场景里显式写 "key": false 即可。
        Quirks merged = cam->quirks();
        merged.merge(sc.quirks);
        cam->setQuirks(merged);
    }

    if (wasRunning) {
        QString error;
        if (!start(&error)) {
            if (errorOut)
                *errorOut = error;
            return false;
        }
    }
    return true;
}

bool Simulator::saveScenario(const QString &path, QString *errorOut) const
{
    Scenario scenario;
    scenario.name = QFileInfo(path).completeBaseName();
    scenario.config = d->config;
    scenario.globalQuirks = d->globalQuirks;
    for (const VirtualCamera *cam : d->cameras) {
        ScenarioCamera sc;
        sc.model = cam->model();
        sc.quirks = cam->quirks();
        scenario.cameras.append(sc);
    }
    return scenario.save(path, errorOut);
}

const Quirks &Simulator::globalQuirks() const
{
    return d->globalQuirks;
}

void Simulator::setGlobalQuirks(const Quirks &quirks)
{
    d->globalQuirks = quirks;
    emit globalQuirksChanged();
}

void Simulator::applyGlobalQuirksToAll()
{
    // 叠加而不是替换，与 applyScenario() / addCamera() 保持同一套语义。
    // 整体替换会把每台相机的品牌预设默认值一起抹掉 —— 界面上那个按钮写的是
    // 「应用到全部相机」，用户预期的是「把我调的这几条推下去」，
    // 不是「把所有相机恢复成只有这几条」。
    for (VirtualCamera *cam : d->cameras) {
        Quirks merged = cam->quirks();
        merged.merge(d->globalQuirks);
        cam->setQuirks(merged);
    }
}

} // namespace onvifsim
