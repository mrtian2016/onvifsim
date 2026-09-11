#pragma once

// 海康 ISAPI 私有 HTTP API 桩（HTTP Digest 鉴权）。
//
// 端点集按 plan.md §4.13 / reference-client-facts.md §7.2 收敛 —— 只做参照客户端
// 真的会打的那几条：deviceInfo 用来让厂商识别命中，PTZCtrl / supplementLight /
// ircutFilter 是「改了要真生效」的写接口，alertStream 是事件长连接。
//
// 状态不自己存：灯光走 ImagingState、云台走 PtzState、事件接 EventEngine，
// 与 ONVIF 侧共享同一份状态，客户端从哪边改都能从另一边读回来。

#include "core/CameraModel.h"
#include "core/Persona.h"
#include "events/EventTypes.h"
#include "net/HttpAuth.h"
#include "net/HttpTypes.h"
#include "ptz/PtzState.h"
#include "vendor/VendorApiStub.h"

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QSharedPointer>
#include <QtCore/QStringList>
#include <QtCore/QVector>

namespace onvifsim {

class IsapiStub : public VendorApiStub
{
    Q_OBJECT
public:
    explicit IsapiStub(VirtualCamera *camera, QObject *parent = nullptr);
    ~IsapiStub() override;

    void registerRoutes(HttpServer *server) override;
    VendorApi kind() const override;
    int streamingClients() const override;

    // 同步处理一条非流式 ISAPI 请求。registerRoutes 装的 handler 就是薄薄一层
    // 「调它 → respond」；单测因此不必起 HttpServer 也能驱动全部端点。
    HttpResponse handle(const HttpRequest &request);

    // ---- 纯函数部分，单测直接打 ----

    // ISAPI 的 pan/tilt/zoom 是 -100..100 的整数，PtzState 用的是 [-1, 1]。
    // 两个方向都要有，因为 GET capabilities 与 PUT continuous 各用一个方向。
    static double speedFromIsapi(int value);
    static int speedToIsapi(double value);

    // 解析 <PTZData><pan>..</pan><tilt/><zoom/></PTZData>。
    // 缺失的分量在返回值里用 hasPanTilt / hasZoom 标出来。
    static PtzVector parsePtzData(const QByteArray &xml);
    static QByteArray buildPtzData(const PtzVector &velocity);

    static QByteArray deviceInfoXml(const CameraModel &model, const Persona &persona);
    static QByteArray responseStatusXml(const QString &requestUrl, int statusCode,
                                        const QString &statusString,
                                        const QString &subStatus = QString());

    // 一条 EventNotificationAlert。plate 非空时额外挂 <ANPR>，
    // 客户端读的就是 eventType / targetType / licensePlate 这三处。
    static QByteArray eventAlertXml(const QString &ipAddress, int channel,
                                    const QDateTime &utcTime, const QString &eventType,
                                    const QString &eventState, const QString &targetType,
                                    const QString &plate);

    // 把一条告警包成 alertStream 用的 multipart 片段（分隔串就是真机的 "boundary"）。
    static QByteArray multipartPart(const QByteArray &xml);

private:
    struct AlertClient;

    // ---- 各端点 ----
    HttpResponse handleDeviceInfo(const HttpRequest &request);
    HttpResponse handlePtz(const HttpRequest &request, const QStringList &segments);
    HttpResponse handleStreaming(const HttpRequest &request, const QStringList &segments);
    HttpResponse handleImage(const HttpRequest &request, const QStringList &segments);
    HttpResponse handleIo(const HttpRequest &request, const QStringList &segments);

    // ---- alertStream 长连接（HttpExchange 的 beginStream / writeChunk / endStream）----
    void beginAlertStream(const HttpRequest &request, const HttpExchangePtr &exchange);
    void onEventProduced(const EventMessage &message);
    bool push(const QSharedPointer<AlertClient> &client, const QByteArray &chunk);
    void drop(const QSharedPointer<AlertClient> &client);

    // 401 挑战。返回 true 表示已经放行，false 时 response 里装的是挑战。
    bool authorize(const HttpRequest &request, HttpResponse *response);

    const Persona &personaOrDefault() const;
    const MediaProfile *profileForChannel(int channelId) const;

    HttpAuth m_auth;
    QVector<QSharedPointer<AlertClient>> m_alertClients;
    bool m_routesRegistered = false;
};

} // namespace onvifsim
