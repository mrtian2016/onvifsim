#pragma once

// 一台虚拟相机：持有模型、quirks、各协议服务器与状态机。
// 所有协议 handler 都通过这个对象拿状态，GUI 只通过信号观察它。

#include "core/CameraModel.h"
#include "core/Persona.h"
#include "core/Quirks.h"

#include <QtCore/QDateTime>
#include <QtCore/QObject>
#include <QtCore/QString>

namespace onvifsim {

class EventEngine;
class HttpServer;
class ImagingState;
class LogBus;
class PtzState;
class RtspServer;
class Simulator;
class SoapDispatcher;
class SubscriptionManager;
class VendorApiStub;

// 相机对外可见的运行状态，REST 与 GUI 都用它。
struct CameraStatus {
    bool running = false;
    bool offline = false;          // 被 quirk 或 REST 打到离线
    QDateTime offlineUntil;
    int rtspSessions = 0;
    int subscriptions = 0;
    int httpClients = 0;
    qint64 talkbackBytes = 0;   // 对讲累计字节，/metrics 里以 counter 暴露
    QDateTime lastEventAt;
    QString lastEventTopic;
};

class VirtualCamera : public QObject
{
    Q_OBJECT
public:
    VirtualCamera(const CameraModel &model, Simulator *simulator, QObject *parent = nullptr);
    ~VirtualCamera() override;

    QString id() const;
    Simulator *simulator() const;
    LogBus *logBus() const;

    // ---- 模型与配置 ----
    const CameraModel &model() const;
    CameraModel &mutableModel();          // 改完要调 applyModelChanges()
    void setModel(const CameraModel &model);
    // 重建受影响的服务器 / 状态机：重铺服务与路由，必要时按新的地址
    // 端口重新监听。改完 mutableModel() 必须调它，否则改动只停在模型里。
    void applyModelChanges();

    const Persona &persona() const;
    void setPersona(const QString &key);

    const Quirks &quirks() const;
    Quirks &mutableQuirks();
    void setQuirks(const Quirks &quirks);

    // ---- 子系统 ----
    HttpServer *httpServer() const;
    RtspServer *rtspServer() const;
    SoapDispatcher *dispatcher() const;
    EventEngine *events() const;
    SubscriptionManager *subscriptions() const;
    PtzState *ptz() const;
    ImagingState *imaging() const;
    VendorApiStub *vendorApi() const;

    // ---- 生命周期 ----
    bool start(QString *errorOut = nullptr);
    void stop();
    bool isRunning() const;

    // 模拟离线：关闭全部监听端口 N 秒后自动恢复（SystemReboot / 随机掉线用）。
    void goOffline(int seconds, bool announceBye = true);
    bool isOffline() const;

    CameraStatus status() const;

    // ---- 对外地址 ----
    // 按请求来源挑一个客户端能连上的地址，再叠加 quirk（XAddrOddPort 等）。
    QString advertisedHost(const QHostAddress &peer = QHostAddress()) const;
    quint16 advertisedHttpPort() const;
    quint16 advertisedRtspPort() const;
    QString deviceServiceXAddr(const QHostAddress &peer = QHostAddress()) const;
    QString serviceXAddr(const QString &serviceName, const QHostAddress &peer = QHostAddress()) const;
    QString streamUri(const MediaProfile &profile, const QHostAddress &peer = QHostAddress()) const;
    QString snapshotUri(const MediaProfile &profile, const QHostAddress &peer = QHostAddress()) const;

    // 设备时钟。AuthTightTimeWindow 的 clock_skew 会让它偏离真实时间。
    QDateTime deviceTimeUtc() const;

signals:
    void statusChanged();
    void modelChanged();
    void quirksChanged();
    void started();
    void stopped();
    void wentOffline(int seconds);
    void cameBackOnline();

private slots:
    // quirk D1：为走独立端口的订阅开一个额外的监听 socket。
    void openSubscriptionPort(const QString &subscriptionId);

private:
    // 按当前模型里的地址 / 端口重新监听。失败时把相机停下来并如实报错。
    bool restartListeners(QString *errorOut);

    // 挂厂商私有 API 的路由。幂等，重复调用不会重复注册。
    void registerVendorRoutes();

    // 按能力开关装配 ONVIF 服务并注册 HTTP 路由。**可重入**：先清空再铺，
    // 构造时调一次，applyModelChanges() 里每次能力变更再调。
    void setupServices();
    // 按 quirk B1 / 预设重命名各 profile。
    void applyProfileNamingStyle();
    // 按 quirk A5 给「对外宣称的路径」补一条 SOAP 路由。
    void syncAdvertisedPathRoute();

    struct Private;
    Private *d;
};

} // namespace onvifsim
