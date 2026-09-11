#include "core/VirtualCamera.h"

#include "core/LogBus.h"
#include "core/Simulator.h"
#include "discovery/DiscoveryResponder.h"
#include "events/EventEngine.h"
#include "events/SubscriptionManager.h"
#include "imaging/ImagingState.h"
#include "net/HttpServer.h"
#include "net/NetUtil.h"
#include "ptz/PtzState.h"
#include "rtsp/RtpReceiver.h"
#include "rtsp/RtspServer.h"
#include "rtsp/RtspSession.h"
#include "soap/Dispatcher.h"
#include "vendor/VendorApiStub.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QHash>
#include <QtCore/QTimer>
#include <QtCore/QUrl>

namespace onvifsim {

// 这两个由 services 层提供：快照 HTTP 端点与订阅管理器路径。
// 它们不是 SOAP 操作，所以要单独往 HttpServer 上挂路由。
namespace services {
void registerSnapshotRoutes(VirtualCamera *camera, HttpServer *server);
void registerSubscriptionRoutes(VirtualCamera *camera, HttpServer *server);
} // namespace services

namespace {
// 离线时长上限，与 quirk 表里所有同类参数取值一致。
constexpr int kMaxOfflineSeconds = 600;
} // namespace

struct VirtualCamera::Private {
    Simulator *simulator = nullptr;
    CameraModel model;
    Quirks quirks;
    Persona persona;

    HttpServer *http = nullptr;
    RtspServer *rtsp = nullptr;
    SoapDispatcher *dispatcher = nullptr;
    EventEngine *events = nullptr;
    SubscriptionManager *subscriptions = nullptr;
    PtzState *ptz = nullptr;
    ImagingState *imaging = nullptr;
    VendorApiStub *vendor = nullptr;

    bool running = false;
    bool offline = false;
    bool vendorRoutesRegistered = false;
    QDateTime offlineUntil;
    QTimer *offlineTimer = nullptr;

    // quirk D1：订阅管理器开在独立端口且每次递增（真机形如 :1024/event-1024_1024）。
    // SubscriptionManager 只负责分配端口与路径，真正的监听 socket 在这里开。
    QHash<quint16, HttpServer *> subscriptionServers;

    // quirk A5 当前已经注册过路由的那个「对外宣称路径」，用来去重。
    QString advertisedPathRoute;
};

VirtualCamera::VirtualCamera(const CameraModel &model, Simulator *simulator, QObject *parent)
    : QObject(parent), d(new Private)
{
    d->simulator = simulator;
    d->model = model;

    if (const Persona *p = PersonaRegistry::find(model.personaKey))
        d->persona = *p;
    else
        d->persona = PersonaRegistry::generic();

    // 预设自带的 quirk 作为初值；用户随后可以逐项覆盖。
    for (QuirkId id : d->persona.defaultQuirks)
        d->quirks.setEnabled(id, true);

    d->ptz = new PtzState(this, this);
    d->imaging = new ImagingState(this, this);
    d->events = new EventEngine(this, this);
    d->subscriptions = new SubscriptionManager(this, this);
    d->http = new HttpServer(this, this);
    d->rtsp = new RtspServer(this, this);
    d->dispatcher = new SoapDispatcher(this);

    if (d->persona.vendorApi != VendorApi::None)
        d->vendor = VendorApiStub::create(d->persona.vendorApi, this);

    setupServices();
    applyProfileNamingStyle();

    // 事件引擎产出的消息直接进订阅队列。两者不互相持引用，只靠信号连接。
    connect(d->events, &EventEngine::eventProduced,
            d->subscriptions, &SubscriptionManager::deliver);
    connect(d->subscriptions, &SubscriptionManager::countChanged,
            this, &VirtualCamera::statusChanged);
    connect(d->subscriptions, &SubscriptionManager::subscriptionCreated,
            this, &VirtualCamera::openSubscriptionPort);
    connect(d->rtsp, &RtspServer::sessionCountChanged, this, &VirtualCamera::statusChanged);
    connect(d->http, &HttpServer::connectionCountChanged, this, &VirtualCamera::statusChanged);

    d->offlineTimer = new QTimer(this);
    d->offlineTimer->setSingleShot(true);
    connect(d->offlineTimer, &QTimer::timeout, this, [this] {
        // 每个 resume 单独收错误：共用一个 error 变量的话，后一个成功会把
        // 前一个的错误串盖掉；而 offline 早在上面就置了 false，于是端口根本
        // 没绑回来、相机却照样 emit cameBackOnline() 并对外报「在线」。
        QStringList failures;
        auto resumeOne = [&failures](auto *server, const QString &what) {
            QString error;
            if (!server->resume(&error))
                failures.append(QStringLiteral("%1：%2").arg(what, error));
        };
        resumeOne(d->http, QStringLiteral("HTTP"));
        resumeOne(d->rtsp, QStringLiteral("RTSP"));
        for (HttpServer *server : d->subscriptionServers)
            resumeOne(server, QStringLiteral("订阅端口"));

        if (!failures.isEmpty()) {
            // 端口没回来就还不算上线：保持 offline，让状态与事实一致，
            // 下面的 Hello 和 cameBackOnline 也不发 —— 对外宣告一台起不来的
            // 相机「回来了」，客户端只会连上去再失败一次。
            // offlineUntil 清掉，免得界面上挂着一个早就过期的倒计时；
            // 不自动重试，端口被别人占着就是被占着，重试多少次都一样，
            // 由用户看到日志后手工重启这台相机。
            d->offlineUntil = QDateTime();
            logBus()->error(logcat::Core, id(),
                            QStringLiteral("Failed to come back online, camera stays "
                                           "offline: %1 (the port may be taken by another "
                                           "process; restart this camera to retry)")
                                .arg(failures.join(QStringLiteral("；"))));
            emit statusChanged();
            return;
        }

        d->offline = false;
        d->offlineUntil = QDateTime();
        if (d->simulator && d->simulator->discovery())
            d->simulator->discovery()->announceHello(this);
        logBus()->info(logcat::Core, id(), QStringLiteral("Back online"));
        emit cameBackOnline();
        emit statusChanged();
    });
}

VirtualCamera::~VirtualCamera()
{
    stop();
    delete d->dispatcher;
    delete d;
}

QString VirtualCamera::id() const
{
    return d->model.id;
}

Simulator *VirtualCamera::simulator() const
{
    return d->simulator;
}

LogBus *VirtualCamera::logBus() const
{
    if (d->simulator && d->simulator->logBus())
        return d->simulator->logBus();
    if (LogBus *global = LogBus::global())
        return global;
    // 单测里可以不建 Simulator 就构造相机，这时给一份进程级兜底总线，
    // 免得每个调用点都要判空 —— 日志丢了不要紧，崩了才要紧。
    static LogBus fallback;
    return &fallback;
}

const CameraModel &VirtualCamera::model() const
{
    return d->model;
}

CameraModel &VirtualCamera::mutableModel()
{
    return d->model;
}

void VirtualCamera::setModel(const CameraModel &model)
{
    d->model = model;
    applyModelChanges();
}

// 按相机的能力开关决定装哪些 ONVIF 服务，并把每个服务的路径挂到 HTTP 路由上。
// 离线 / 恢复只动监听 socket，不必重新注册；但能力开关在运行时被改了之后
// 必须整套重来 —— 所以这个函数是**可重入**的：先清空再铺，不能只往上加。
void VirtualCamera::setupServices()
{
    d->dispatcher->clearServices();
    d->http->clearRoutes();
    d->advertisedPathRoute.clear();

    d->dispatcher->addService(services::createDevice());
    d->dispatcher->addService(services::createMedia());
    if (d->model.hasMedia2Service && d->persona.hasMedia2)
        d->dispatcher->addService(services::createMedia2());
    if (d->model.hasPtzService)
        d->dispatcher->addService(services::createPtz());
    if (d->model.hasImagingService)
        d->dispatcher->addService(services::createImaging());
    if (d->model.hasEventsService)
        d->dispatcher->addService(services::createEvents());
    if (d->model.hasAnalyticsService)
        d->dispatcher->addService(services::createAnalytics());
    if (d->model.hasDeviceIoService)
        d->dispatcher->addService(services::createDeviceIo());

    auto soapRoute = [this](const HttpRequest &request, const HttpExchangePtr &exchange) {
        d->dispatcher->handle(request, exchange);
    };

    // device_service 的路径是客户端硬编码的，必须固定可达。
    d->http->addRoute("POST", d->persona.devicePath, soapRoute);

    for (SoapService *service : d->dispatcher->services()) {
        QString path;
        if (d->persona.servicePathPattern.contains(QLatin1String("%1")))
            path = d->persona.servicePathPattern.arg(QString::fromLatin1(service->serviceName()));
        else
            path = d->persona.servicePathPattern;   // TP-Link 那种所有服务共用一个路径
        d->http->addRoute("POST", path, soapRoute);
        // 约定路径也挂上：客户端在 GetServices 失败时会回落到这些。
        d->http->addRoute("POST", service->defaultPath(), soapRoute);
    }

    syncAdvertisedPathRoute();

    services::registerSnapshotRoutes(this, d->http);
    if (d->model.hasEventsService)
        services::registerSubscriptionRoutes(this, d->http);

    // 厂商私有路由单独一个函数：clearRoutes() 刚把它们一起清掉了，相机正在跑
    // 的话要立刻补回来。没跑的时候不补 —— VIGI 的 registerRoutes() 会真的去绑
    // 一个 TLS 端口，一台还没 start() 的相机不该占着端口。
    d->vendorRoutesRegistered = false;
    if (d->running)
        registerVendorRoutes();
}

// 幂等：路由表被 setupServices() 清空后才会真的再注册一次。
// 原来这段直接写在 start() 里，于是每次 stop()→start() 都往路由表里重复塞一遍。
void VirtualCamera::registerVendorRoutes()
{
    if (!d->vendor || d->vendorRoutesRegistered)
        return;
    d->vendor->registerRoutes(d->http);
    d->vendorRoutesRegistered = true;
}

// 为一条走独立端口的订阅开监听（quirk D1）。同一端口只开一次。
// 客户端必须用返回的订阅地址去 Pull，用主服务地址是拉不到的 —— 这正是要测的点。
void VirtualCamera::openSubscriptionPort(const QString &subscriptionId)
{
    Subscription *sub = d->subscriptions->find(subscriptionId);
    if (!sub || sub->port == 0 || sub->port == d->http->serverPort())
        return;   // 用主端口，不必另开
    if (d->subscriptionServers.contains(sub->port))
        return;

    auto *server = new HttpServer(this, this);
    QString error;
    if (!server->listen(d->model.bindAddress, sub->port, &error)) {
        // 端口占用时退回主端口：订阅仍然可用，只是少复现一条怪癖。
        logBus()->warning(logcat::Event, id(),
                          QStringLiteral("Cannot listen on subscription port %1, falling back to the main port: %2")
                              .arg(sub->port)
                              .arg(error));
        sub->port = d->http->serverPort();
        sub->address = QStringLiteral("http://%1:%2%3")
                           .arg(advertisedHost())
                           .arg(d->http->serverPort())
                           .arg(sub->path);
        delete server;
        return;
    }

    services::registerSubscriptionRoutes(this, server);
    d->subscriptionServers.insert(sub->port, server);
    logBus()->debug(logcat::Event, id(),
                    QStringLiteral("Subscription manager for %1 runs on its own port %2")
                        .arg(subscriptionId)
                        .arg(sub->port));
}

// quirk A5 让相机对外宣称一个与本地路由不同的路径（真机 TL-IPC652P-A4 报
// :2020/onvif/service）。客户端只接管 host:port、保留 path，所以请求最终会打到
// 那个宣称的路径上 —— 必须给它挂一条路由，否则客户端拿到 404，A5 就不是
// 「能扛过去的怪癖」而是「彻底连不上」了。
// 开关和参数都能在运行时改，所以构造时与每次 setQuirks 都要同步一次。
void VirtualCamera::syncAdvertisedPathRoute()
{
    if (!d->quirks.isEnabled(QuirkId::XAddrOddPort))
        return;
    const QString path = d->quirks.paramString(QuirkId::XAddrOddPort, QStringLiteral("path"));
    if (path.isEmpty() || path == d->advertisedPathRoute)
        return;

    d->advertisedPathRoute = path;
    d->http->addRoute("POST", path, [this](const HttpRequest &request,
                                           const HttpExchangePtr &exchange) {
        d->dispatcher->handle(request, exchange);
    });
}

void VirtualCamera::applyModelChanges()
{
    if (const Persona *p = PersonaRegistry::find(d->model.personaKey))
        d->persona = *p;
    d->events->reloadTopics();

    // 能力开关 / 预设变了就得把服务集合和路由整套重铺。少了这一步，
    // `PATCH {"capabilities":{"ptz":false}}` 只改了模型，PtzService 还挂在
    // dispatcher 上照常接受 ContinuousMove —— 接口回 200，用户以为改了。
    setupServices();

    // 端口或绑定地址变了就得重新监听。端口只在 start() 里读一次，
    // 不重绑的话 `PATCH {"network":{"httpPort":9000}}` 同样是静默无效。
    if (d->running) {
        const bool rebindHttp = d->http->serverAddress() != d->model.bindAddress
            || d->http->serverPort() != d->model.httpPort;
        // RtspServer 没有 serverAddress()，绑定地址变了就跟着 HTTP 一起重绑。
        const bool rebindRtsp = d->rtsp->serverPort() != d->model.rtspPort;
        if (rebindHttp || rebindRtsp) {
            QString error;
            if (!restartListeners(&error)) {
                logBus()->error(logcat::Core, id(),
                                QStringLiteral("Failed to re-listen with the new configuration: %1").arg(error));
            }
        }
    }

    emit modelChanged();
    emit statusChanged();
}

// 按当前模型里的地址 / 端口重新绑定。失败时把相机停下来并如实报错 ——
// 半绑着的状态（HTTP 在新端口、RTSP 还在老端口）比干脆停掉更难排查。
bool VirtualCamera::restartListeners(QString *errorOut)
{
    d->rtsp->teardownAll();
    d->http->close();
    d->rtsp->close();

    QString error;
    if (!d->http->listen(d->model.bindAddress, d->model.httpPort, &error)) {
        if (errorOut)
            *errorOut = QStringLiteral("HTTP %1:%2 —— %3")
                            .arg(d->model.bindAddress.toString())
                            .arg(d->model.httpPort)
                            .arg(error);
        d->running = false;
        return false;
    }
    if (!d->rtsp->listen(d->model.bindAddress, d->model.rtspPort, &error)) {
        d->http->close();
        if (errorOut)
            *errorOut = QStringLiteral("RTSP %1:%2 —— %3")
                            .arg(d->model.bindAddress.toString())
                            .arg(d->model.rtspPort)
                            .arg(error);
        d->running = false;
        return false;
    }
    logBus()->info(logcat::Core, id(),
                   QStringLiteral("Re-listening with the new configuration: HTTP %1:%2 / RTSP %1:%3")
                       .arg(d->model.bindAddress.toString())
                       .arg(d->http->serverPort())
                       .arg(d->rtsp->serverPort()));
    return true;
}

const Persona &VirtualCamera::persona() const
{
    return d->persona;
}

void VirtualCamera::setPersona(const QString &key)
{
    const Persona *p = PersonaRegistry::find(key);
    if (!p)
        return;
    d->persona = *p;
    d->model.personaKey = key;
    d->model.manufacturer = p->manufacturer;
    d->model.model = p->model;
    d->model.firmwareVersion = p->firmwareVersion;
    d->model.hardwareId = p->hardwareId;
    // 预设换了，RTSP 路径风格也要跟着换。
    const QString paths[] = { p->rtspMainPath, p->rtspSubPath, p->rtspThirdPath };
    for (int i = 0; i < d->model.profiles.size() && i < 3; ++i)
        d->model.profiles[i].streamPath = paths[i];
    applyModelChanges();
}

const Quirks &VirtualCamera::quirks() const
{
    return d->quirks;
}

Quirks &VirtualCamera::mutableQuirks()
{
    return d->quirks;
}

void VirtualCamera::setQuirks(const Quirks &quirks)
{
    // C6 的出厂预置位是构造时按 quirk 铺好的，运行中翻转开关必须重铺一次，
    // 否则 REST / GUI 改了开关而预置位列表纹丝不动。
    const bool hadFactory = d->quirks.isEnabled(QuirkId::PtzFactory300Presets);
    const bool wantsFactory = quirks.isEnabled(QuirkId::PtzFactory300Presets);

    d->quirks = quirks;

    if (wantsFactory && !hadFactory) {
        d->ptz->generateFactoryPresets(
            quirks.paramInt(QuirkId::PtzFactory300Presets, QStringLiteral("count")));
    } else if (!wantsFactory && hadFactory) {
        d->ptz->clearPresets();
    }

    applyProfileNamingStyle();
    syncAdvertisedPathRoute();
    d->events->reloadTopics();
    d->events->syncStorm();
    emit quirksChanged();
}

// quirk B1：profile 命名风格。客户端判定主 / 子码流只看 Name 字串，
// 换成 Profile_1 / Profile_2 这种没有主子语义的名字，它就只能按顺序猜。
void VirtualCamera::applyProfileNamingStyle()
{
    const QString style = d->quirks.isEnabled(QuirkId::ProfileNamingStyle)
                              ? d->quirks.choice(QuirkId::ProfileNamingStyle,
                                                 d->persona.profileNamingStyle)
                              : d->persona.profileNamingStyle;
    for (int i = 0; i < d->model.profiles.size(); ++i)
        d->model.profiles[i].name = CameraModel::profileNameForStyle(style, i);
}

HttpServer *VirtualCamera::httpServer() const { return d->http; }
RtspServer *VirtualCamera::rtspServer() const { return d->rtsp; }
SoapDispatcher *VirtualCamera::dispatcher() const { return d->dispatcher; }
EventEngine *VirtualCamera::events() const { return d->events; }
SubscriptionManager *VirtualCamera::subscriptions() const { return d->subscriptions; }
PtzState *VirtualCamera::ptz() const { return d->ptz; }
ImagingState *VirtualCamera::imaging() const { return d->imaging; }
VendorApiStub *VirtualCamera::vendorApi() const { return d->vendor; }

bool VirtualCamera::start(QString *errorOut)
{
    if (d->running)
        return true;

    QString error;
    if (!d->http->listen(d->model.bindAddress, d->model.httpPort, &error)) {
        if (errorOut)
            *errorOut = QCoreApplication::translate("onvifsim::core", "HTTP 监听失败（%1:%2）：%3")
                            .arg(d->model.bindAddress.toString())
                            .arg(d->model.httpPort)
                            .arg(error);
        return false;
    }
    if (!d->rtsp->listen(d->model.bindAddress, d->model.rtspPort, &error)) {
        d->http->close();
        if (errorOut)
            *errorOut = QCoreApplication::translate("onvifsim::core", "RTSP 监听失败（%1:%2）：%3")
                            .arg(d->model.bindAddress.toString())
                            .arg(d->model.rtspPort)
                            .arg(error);
        return false;
    }

    d->running = true;
    d->offline = false;
    registerVendorRoutes();

    // quirk C6：出厂预填的预置位在启动时生成，这样客户端一连上就看到 300 条。
    if (d->quirks.isEnabled(QuirkId::PtzFactory300Presets))
        d->ptz->generateFactoryPresets(d->quirks.paramInt(QuirkId::PtzFactory300Presets,
                                                          QStringLiteral("count")));

    d->events->syncStorm();

    logBus()->info(logcat::Core, id(),
                   QStringLiteral("Started: HTTP %1:%2 / RTSP %1:%3")
                       .arg(d->model.bindAddress.toString())
                       .arg(d->http->serverPort())
                       .arg(d->rtsp->serverPort()));

    if (d->simulator && d->simulator->discovery())
        d->simulator->discovery()->announceHello(this);

    emit started();
    emit statusChanged();
    return true;
}

void VirtualCamera::stop()
{
    if (!d->running)
        return;

    if (d->simulator && d->simulator->discovery())
        d->simulator->discovery()->announceBye(this);

    d->events->stopStorm();
    d->rtsp->teardownAll();
    d->rtsp->close();
    d->http->close();
    qDeleteAll(d->subscriptionServers);
    d->subscriptionServers.clear();
    d->subscriptions->clear();
    d->offlineTimer->stop();
    d->running = false;
    d->offline = false;

    logBus()->info(logcat::Core, id(), QStringLiteral("Stopped"));
    emit stopped();
    emit statusChanged();
}

bool VirtualCamera::isRunning() const
{
    return d->running;
}

void VirtualCamera::goOffline(int seconds, bool announceBye)
{
    if (!d->running || seconds <= 0)
        return;
    // 卡上限而不是原样用：offline?seconds=9999999 会让 seconds * 1000 溢出成负数，
    // QTimer::start(负数) 只打一行警告就不启动 —— 相机永久离线，回不来了，
    // 而日志里还写着「模拟离线 9999999 秒」，完全对不上。
    seconds = qMin(seconds, kMaxOfflineSeconds);

    // 真离线：发 Bye → 关掉全部端口 → N 秒后发 Hello 回来。
    // 只回个 OK 是测不出客户端重连逻辑的。
    if (announceBye && d->simulator && d->simulator->discovery())
        d->simulator->discovery()->announceBye(this);

    d->rtsp->teardownAll();
    d->rtsp->suspend();
    d->http->suspend();
    for (HttpServer *server : d->subscriptionServers)
        server->suspend();
    d->offline = true;
    d->offlineUntil = QDateTime::currentDateTimeUtc().addSecs(seconds);
    d->offlineTimer->start(seconds * 1000);

    logBus()->warning(logcat::Core, id(), QStringLiteral("Simulating %1 s offline").arg(seconds));
    emit wentOffline(seconds);
    emit statusChanged();
}

bool VirtualCamera::isOffline() const
{
    return d->offline;
}

CameraStatus VirtualCamera::status() const
{
    CameraStatus s;
    s.running = d->running;
    s.offline = d->offline;
    s.offlineUntil = d->offlineUntil;
    s.rtspSessions = d->rtsp->sessionCount();
    s.subscriptions = d->subscriptions->count();
    s.httpClients = d->http->connectionCount();
    s.lastEventAt = d->events->lastEventTime();
    s.lastEventTopic = d->events->lastEventTopic();

    // 对讲字节数按会话汇总。GUI 与 REST 都要这个数，让它们各自去翻
    // RtspSession → RtpReceiver 太啰嗦，也把内部结构泄漏出去了。
    for (const RtspSession *session : d->rtsp->sessions()) {
        if (session->hasBackchannel() && session->backchannelReceiver())
            s.talkbackBytes += session->backchannelReceiver()->stats().bytesReceived;
    }
    return s;
}

QString VirtualCamera::advertisedHost(const QHostAddress &peer) const
{
    // 优先级：模型里写死的 > 按 peer 选的本机地址 > 绑定地址。
    // 同机测试时绝不能给 127.0.0.1，客户端连不上自己以外的东西。
    if (!d->model.advertisedHost.isEmpty())
        return d->model.advertisedHost;

    if (!peer.isNull()) {
        const QHostAddress local = netutil::preferredLocalAddress(peer, d->model.bindAddress);
        if (!local.isNull())
            return local.toString();
    }

    if (d->model.bindAddress == QHostAddress(QHostAddress::AnyIPv4)
        || d->model.bindAddress == QHostAddress(QHostAddress::Any)) {
        const QHostAddress local = netutil::preferredLocalAddress(QHostAddress());
        if (!local.isNull())
            return local.toString();
    }
    return d->model.bindAddress.toString();
}

quint16 VirtualCamera::advertisedHttpPort() const
{
    // quirk A5：对外报一个和实际监听不同的端口。真机 TL-IPC652P-A4 报 :2020
    // 而实际连的是 80 —— 客户端只接管 host:port、保留 path，正好测这条逻辑。
    if (d->quirks.isEnabled(QuirkId::XAddrOddPort))
        return static_cast<quint16>(d->quirks.paramInt(QuirkId::XAddrOddPort,
                                                       QStringLiteral("port")));
    if (d->model.advertisedHttpPort)
        return d->model.advertisedHttpPort;
    return d->http->isListening() ? d->http->serverPort() : d->model.httpPort;
}

quint16 VirtualCamera::advertisedRtspPort() const
{
    if (d->model.advertisedRtspPort)
        return d->model.advertisedRtspPort;
    return d->rtsp->isListening() ? d->rtsp->serverPort() : d->model.rtspPort;
}

QString VirtualCamera::deviceServiceXAddr(const QHostAddress &peer) const
{
    // device_service 的路径是客户端硬编码的，任何 quirk 都不该改它，
    // 否则连第一步都走不到。A5 只改 host:port 与其余服务的 path。
    QString path = d->persona.devicePath;
    if (d->quirks.isEnabled(QuirkId::XAddrOddPort)) {
        const QString override = d->quirks.paramString(QuirkId::XAddrOddPort,
                                                       QStringLiteral("path"));
        if (!override.isEmpty())
            path = override;
    }
    return QStringLiteral("http://%1:%2%3")
        .arg(advertisedHost(peer))
        .arg(advertisedHttpPort())
        .arg(path);
}

QString VirtualCamera::serviceXAddr(const QString &serviceName, const QHostAddress &peer) const
{
    if (serviceName == QLatin1String("device"))
        return deviceServiceXAddr(peer);

    QString path;
    if (d->persona.servicePathPattern.contains(QLatin1String("%1")))
        path = d->persona.servicePathPattern.arg(serviceName);
    else
        path = d->persona.servicePathPattern;   // TP-Link 那种所有服务共用一个路径

    if (d->quirks.isEnabled(QuirkId::XAddrOddPort)) {
        const QString override = d->quirks.paramString(QuirkId::XAddrOddPort,
                                                       QStringLiteral("path"));
        if (!override.isEmpty())
            path = override;
    }
    return QStringLiteral("http://%1:%2%3")
        .arg(advertisedHost(peer))
        .arg(advertisedHttpPort())
        .arg(path);
}

QString VirtualCamera::streamUri(const MediaProfile &profile, const QHostAddress &peer) const
{
    QString host = advertisedHost(peer);
    quint16 port = advertisedRtspPort();

    // quirk：固件把出厂默认 IP 写死进 URI，客户端必须用 XAddr 的 host 覆盖。
    if (d->quirks.isEnabled(QuirkId::StreamUriPlaceholderIp))
        host = d->quirks.paramString(QuirkId::StreamUriPlaceholderIp, QStringLiteral("address"));

    QString path = profile.streamPath;
    if (path.isEmpty())
        path = QStringLiteral("/profile%1").arg(profile.token);
    if (!path.startsWith(QLatin1Char('/')))
        path.prepend(QLatin1Char('/'));

    QString userinfo;
    // quirk B2：URI 自带 user:pass@。客户端会丢弃它再注入自己的凭据。
    if (d->quirks.isEnabled(QuirkId::StreamUriWithUserinfo)) {
        const QString ui = d->quirks.paramString(QuirkId::StreamUriWithUserinfo,
                                                 QStringLiteral("userinfo"));
        if (!ui.isEmpty())
            userinfo = ui + QLatin1Char('@');
    }

    return QStringLiteral("rtsp://%1%2:%3%4").arg(userinfo, host).arg(port).arg(path);
}

QString VirtualCamera::snapshotUri(const MediaProfile &profile, const QHostAddress &peer) const
{
    QString path = d->persona.snapshotPath;
    if (!path.contains(QLatin1Char('?')))
        path += QStringLiteral("?token=%1").arg(profile.token);

    // quirk B6：URI 定期轮换失效。带上一个随时间变化的 token，
    // 过期后旧 URI 取图会失败，客户端要能重探。
    if (d->quirks.isEnabled(QuirkId::SnapshotUriRotates)) {
        const int window = qMax(1, d->quirks.paramInt(QuirkId::SnapshotUriRotates,
                                                      QStringLiteral("seconds")));
        const qint64 epoch = QDateTime::currentSecsSinceEpoch() / window;
        path += QStringLiteral("&nonce=%1").arg(epoch);
    }

    return QStringLiteral("http://%1:%2%3")
        .arg(advertisedHost(peer))
        .arg(advertisedHttpPort())
        .arg(path);
}

QDateTime VirtualCamera::deviceTimeUtc() const
{
    // quirk A8 的 clock_skew：把设备时钟拨偏，配合收紧的时间窗就能复现
    // 「客户端不做时钟补偿 → 全线 401」。
    const int skew = d->quirks.isEnabled(QuirkId::AuthTightTimeWindow)
                         ? d->quirks.paramInt(QuirkId::AuthTightTimeWindow,
                                              QStringLiteral("clock_skew"))
                         : 0;
    return QDateTime::currentDateTimeUtc().addSecs(skew);
}

} // namespace onvifsim
