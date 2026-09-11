#include "services/events/SubscriptionEndpoint.h"

#include "core/LogBus.h"
#include "core/VirtualCamera.h"
#include "events/SubscriptionManager.h"
#include "net/HttpServer.h"
#include "services/events/EventsService.h"
#include "soap/Dispatcher.h"
#include "soap/Envelope.h"
#include "soap/Namespaces.h"
#include "soap/WsSecurity.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>

namespace onvifsim {
namespace {

// SubscriptionManager 生成的两种路径：同端口模式 /onvif/subscription-N，
// D1 独立端口模式 /event-<port>_<port>（真机 TL-IPC652P-A4 就长这样）。
constexpr const char *kSubscriptionPrefixes[] = { "/onvif/subscription-", "/event-" };

// 一台相机在一个 HttpServer 上的订阅端点。挂在 server 下面当子对象，
// server 没了它跟着没 —— D1 每建一条订阅就开一个新端口的服务器，靠这个自动回收。
class SubscriptionEndpoint : public QObject
{
public:
    explicit SubscriptionEndpoint(VirtualCamera *camera, HttpServer *server)
        : QObject(server), m_camera(camera)
    {
    }

    void handle(const HttpRequest &request, const HttpExchangePtr &exchange);

private:
    VirtualCamera *m_camera = nullptr;
    // nonce 重放缓存要跨请求存活，所以 WsSecurity 是成员而不是局部量。
    WsSecurity m_security;
};

void SubscriptionEndpoint::handle(const HttpRequest &request, const HttpExchangePtr &exchange)
{
    QElapsedTimer timer;
    timer.start();

    LogBus *log = m_camera ? m_camera->logBus() : LogBus::global();
    static const Quirks kNoQuirks;
    const Quirks &quirks = m_camera ? m_camera->quirks() : kNoQuirks;

    LogRecord record;
    record.category = QString::fromLatin1(logcat::Soap);
    record.cameraId = m_camera ? m_camera->id() : QString();
    record.peer = request.peerString();
    record.detail = QString::fromUtf8(request.body);

    const SoapRequest soapRequest = soap::parseEnvelope(request.body);

    auto finish = [&](HttpResponse response, const QString &summary, bool ok,
                      LogLevel level) {
        // 这条链路不走 Dispatcher，传输层 quirk（GlobalDelay / Slowloris /
        // HugeResponse / F1 畸形响应）得自己贴一次，否则订阅地址上一条都不生效。
        soap::applyResponseQuirks(quirks, response, request, soapRequest.bodyNamespace);
        record.summary = summary;
        record.ok = ok;
        record.level = level;
        record.durationUs = timer.nsecsElapsed() / 1000;
        if (!response.body.isEmpty())
            record.detail += QStringLiteral("\n---- response ----\n")
                             + QString::fromUtf8(response.body);
        if (log)
            log->post(record);
        exchange->respond(response);
    };

    auto respondFault = [&](const SoapFault &fault, const QString &summary) {
        finish(HttpResponse::soap(soap::makeFault(soapRequest.soapVersion, fault),
                                  soap::faultHttpStatus(quirks, fault)),
               summary, false, LogLevel::Warning);
    };

    if (!soapRequest.isValid()) {
        SoapFault f;
        f.subcode = QString::fromLatin1(ter::InvalidArgs);
        f.reason = soapRequest.parseError.isEmpty() ? QStringLiteral("Malformed SOAP envelope")
                                                    : soapRequest.parseError;
        respondFault(f, QStringLiteral("订阅端点 SOAP parse failed: %1").arg(f.reason));
        return;
    }

    // ---- 鉴权 ----
    // 这四个操作都是 USER 级。Dispatcher 那边的 AuthPreAuthRequired 与本处无关
    //（没有 PreAuth 操作），但 D8 的措辞变体、时间窗 quirk 都要照样生效。
    // 没有相机就拒绝，不是跳过鉴权（fail-open 是坏示范，理由同 Dispatcher）。
    if (!m_camera) {
        respondFault(soap::authFault(quirks),
                     QStringLiteral("%1 无相机绑定，拒绝").arg(soapRequest.bodyName));
        return;
    }

    {
        const WsSecurityResult auth = m_security.verify(soapRequest, m_camera->model(), quirks,
                                                        m_camera->deviceTimeUtc());
        if (!auth.authenticated || !WsSecurity::levelSatisfies(auth.level, AuthLevel::User)) {
            const QString why = auth.failureReason.isEmpty() ? QStringLiteral("权限不足")
                                                             : auth.failureReason;
            respondFault(soap::authFault(quirks),
                         QStringLiteral("%1 authentication failed: %2").arg(soapRequest.bodyName, why));
            return;
        }
    }

    XmlWriter writer(soapRequest.soapVersion);
    writer.declareServicePrefixes();
    writer.startEnvelope();

    SoapContext ctx;
    ctx.camera = m_camera;
    ctx.soap = &soapRequest;
    ctx.body = &soapRequest.body;
    ctx.http = &request;
    ctx.exchange = exchange;
    ctx.out = &writer;

    const QString name = soapRequest.bodyName;
    if (name == QLatin1String("PullMessages"))
        events::pullMessages(ctx);
    else if (name == QLatin1String("Renew"))
        events::renew(ctx);
    else if (name == QLatin1String("Unsubscribe"))
        events::unsubscribe(ctx);
    else if (name == QLatin1String("SetSynchronizationPoint"))
        events::setSynchronizationPoint(ctx);
    else
        ctx.fault(soap::notSupported(name));

    // PullMessages 的长轮询走这条：handler 已经把 exchange 接管走了，
    // 这里绝不能再回一次包，否则 respond 的幂等保护会把真正的响应吃掉。
    if (ctx.responseTakenOver()) {
        record.summary = name + QStringLiteral(" (long poll pending)");
        record.durationUs = timer.nsecsElapsed() / 1000;
        if (log)
            log->post(record);
        return;
    }

    if (ctx.hasFault()) {
        respondFault(ctx.pendingFault(),
                     QStringLiteral("%1 → Fault %2").arg(name, ctx.pendingFault().subcode));
        return;
    }

    writer.endEnvelope();
    finish(HttpResponse::soap(writer.take()), name, true, LogLevel::Info);
}

} // namespace

namespace services {

void registerSubscriptionRoutes(VirtualCamera *camera, HttpServer *server)
{
    if (!camera || !server)
        return;
    // 幂等：同一个 server 已经挂过就直接返回，别把前缀路由叠成两份。
    // 用动态属性做标记而不是 findChild —— 端点类没有 Q_OBJECT，也不值得为此拖上 moc。
    static const char *kRegisteredFlag = "onvifsim.subscriptionRoutes";
    if (server->property(kRegisteredFlag).toBool())
        return;
    server->setProperty(kRegisteredFlag, true);

    SubscriptionEndpoint *endpoint = new SubscriptionEndpoint(camera, server);
    auto handler = [endpoint](const HttpRequest &request, const HttpExchangePtr &exchange) {
        endpoint->handle(request, exchange);
    };
    for (const char *prefix : kSubscriptionPrefixes)
        server->addPrefixRoute("POST", QString::fromLatin1(prefix), handler);
}

} // namespace services
} // namespace onvifsim
