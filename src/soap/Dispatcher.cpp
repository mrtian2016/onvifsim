#include "soap/Dispatcher.h"

#include "core/LogBus.h"
#include "core/VirtualCamera.h"
#include "soap/Namespaces.h"
#include "soap/WsSecurity.h"

#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QRandomGenerator>

namespace onvifsim {
namespace soap {

// 把 quirk 决定的响应侧行为一次性贴到 HttpResponse 上。
// 这些钩子的执行在 HttpServer 里，这里只负责「决定要不要」。
void applyResponseQuirks(const Quirks &q, HttpResponse &response, const HttpRequest &request,
                         const QString &bodyNamespace)
{
    // quirk C10：PTZ 响应延迟抖动 100~800ms，用来验证客户端的命令乱序防护。
    // 放在传输层而不是 PTZ handler 里，是为了让它能和 GlobalDelay / Slowloris /
    // 畸形响应这些叠加，日志也照常记录完整响应体。
    if (q.isEnabled(QuirkId::PtzResponseJitter)
        && bodyNamespace == QLatin1String(ns::Ptz)) {
        const int lo = q.paramInt(QuirkId::PtzResponseJitter, QStringLiteral("min_ms"));
        const int hi = q.paramInt(QuirkId::PtzResponseJitter, QStringLiteral("max_ms"));
        response.delayMs += lo < hi ? QRandomGenerator::global()->bounded(lo, hi + 1) : lo;
    }

    if (q.isEnabled(QuirkId::GlobalDelay))
        response.delayMs += q.paramInt(QuirkId::GlobalDelay, QStringLiteral("ms"));

    if (q.isEnabled(QuirkId::Slowloris)) {
        response.slowSendChunk = q.paramInt(QuirkId::Slowloris, QStringLiteral("chunk_bytes"));
        response.slowSendIntervalMs =
            q.paramInt(QuirkId::Slowloris, QStringLiteral("interval_ms"));
    }

    if (q.isEnabled(QuirkId::HugeResponse)) {
        // 塞填充到指定大小，用来测客户端的解析上限与内存。
        // 填充放在信封之后，XML 本身仍然合法。
        const int kb = q.paramInt(QuirkId::HugeResponse, QStringLiteral("kb"));
        const qint64 target = static_cast<qint64>(kb) * 1024;
        if (response.body.size() < target) {
            response.body.append(QByteArray("\n<!-- ")
                                 + QByteArray(static_cast<int>(target - response.body.size()), 'x')
                                 + QByteArray(" -->"));
        }
    }

    if (q.isEnabled(QuirkId::MalformedHttpResponse)) {
        const double percent =
            q.paramDouble(QuirkId::MalformedHttpResponse, QStringLiteral("percent"));
        if (QRandomGenerator::global()->bounded(100.0) < percent) {
            const QString mode = q.paramString(QuirkId::MalformedHttpResponse,
                                               QStringLiteral("value"));
            if (mode == QLatin1String("echo_500")) {
                // 真机行为：把收到的请求字节原样回显，再挂个 500。
                response.rawOverride = QByteArray("HTTP/1.1 500 Internal Server Error\r\n\r\n")
                                       + request.body;
            } else if (mode == QLatin1String("no_content_length")) {
                response.rawOverride = QByteArray("HTTP/1.1 200 OK\r\n"
                                                  "Content-Type: application/soap+xml\r\n\r\n")
                                       + response.body;
            } else if (mode == QLatin1String("bad_status_line")) {
                response.rawOverride = QByteArray("HTTP/1.1 OK\r\n\r\n") + response.body;
            } else if (mode == QLatin1String("truncated")) {
                response.rawOverride = QByteArray("HTTP/1.1 200 OK\r\n"
                                                  "Content-Type: application/soap+xml\r\n"
                                                  "Content-Length: 100000\r\n\r\n")
                                       + response.body.left(50);
            }
        }
    }
}

} // namespace soap

struct SoapDispatcher::Private {
    VirtualCamera *camera = nullptr;
    QList<SoapService *> services;
    WsSecurity security;
    // 最近一秒内收到的请求时刻，quirk F3 用它判过载。
    // 不能用「同时在处理的数量」：单进程单事件循环下 SOAP 请求是串行处理的，
    // 那个数永远是 1，这条 quirk 就永远触发不了。真机被打挂靠的是请求速率。
    QList<qint64> recentRequests;

    bool overloaded(const Quirks &quirks)
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        recentRequests.append(now);
        while (!recentRequests.isEmpty() && now - recentRequests.first() > 1000)
            recentRequests.removeFirst();
        if (!quirks.isEnabled(QuirkId::OverloadReboot))
            return false;
        return recentRequests.size()
               > quirks.paramInt(QuirkId::OverloadReboot, QStringLiteral("threshold"));
    }
};

SoapDispatcher::SoapDispatcher(VirtualCamera *camera) : d(new Private)
{
    d->camera = camera;
}

SoapDispatcher::~SoapDispatcher()
{
    qDeleteAll(d->services);
    delete d;
}

void SoapDispatcher::addService(SoapService *service)
{
    if (service)
        d->services.append(service);
}

void SoapDispatcher::clearServices()
{
    qDeleteAll(d->services);
    d->services.clear();
}

SoapService *SoapDispatcher::serviceByNamespace(const QString &ns) const
{
    // 必须精确匹配完整命名空间串。Media(ver10) 与 Media2(ver20) 的 ns 都含 "/media/"，
    // 按子串匹配会把两者混起来 —— quirk A6 就是拿这个做文章的。
    for (SoapService *s : d->services) {
        if (ns == QLatin1String(s->serviceNamespace()))
            return s;
    }
    return nullptr;
}

SoapService *SoapDispatcher::serviceByName(const QString &name) const
{
    for (SoapService *s : d->services) {
        if (name == QLatin1String(s->serviceName()))
            return s;
    }
    return nullptr;
}

QList<SoapService *> SoapDispatcher::services() const
{
    return d->services;
}

QList<SoapService *> SoapDispatcher::servicesForAdvertisement() const
{
    QList<SoapService *> list = d->services;

    // quirk A6：把 Media2 挪到 Media 前面。只按服务名匹配的客户端会把 media
    // 解析到 Media2 端点，然后往那儿发 ver10 的 GetProfiles 吃 ActionNotSupported。
    // 这是海康 / Axis 双栈固件的真实布局。
    if (d->camera && d->camera->quirks().isEnabled(QuirkId::ServicesMedia2First)) {
        int mediaIndex = -1;
        int media2Index = -1;
        for (int i = 0; i < list.size(); ++i) {
            const QString name = QString::fromLatin1(list.at(i)->serviceName());
            if (name == QLatin1String("media"))
                mediaIndex = i;
            else if (name == QLatin1String("media2"))
                media2Index = i;
        }
        if (mediaIndex >= 0 && media2Index > mediaIndex)
            list.move(media2Index, mediaIndex);
    }
    return list;
}

void SoapDispatcher::handle(const HttpRequest &request, const HttpExchangePtr &exchange)
{
    QElapsedTimer timer;
    timer.start();

    LogBus *log = d->camera ? d->camera->logBus() : LogBus::global();
    const Quirks &quirks = d->camera ? d->camera->quirks()
                                     : *[] { static Quirks q; return &q; }();
    const QString cameraId = d->camera ? d->camera->id() : QString();

    LogRecord record;
    record.category = QString::fromLatin1(logcat::Soap);
    record.cameraId = cameraId;
    record.peer = request.peerString();

    // quirk：随机掉线。模拟不稳定的 PoE / WiFi —— 设备时好时坏，
    // 客户端必须能自己缓过来，而不是一次失败就把相机标成永久离线。
    if (quirks.isEnabled(QuirkId::RandomDropout) && d->camera
        && QRandomGenerator::global()->bounded(100.0)
               < quirks.paramDouble(QuirkId::RandomDropout, QStringLiteral("percent"))) {
        const int seconds = quirks.paramInt(QuirkId::RandomDropout, QStringLiteral("seconds"));
        if (log) {
            record.level = LogLevel::Warning;
            record.ok = false;
            record.summary = QStringLiteral("Random dropout, %1 s").arg(seconds);
            record.quirkKey = QuirkRegistry::def(QuirkId::RandomDropout).key;
            log->post(record);
        }
        // 不发 Bye：真机掉线是静默的，客户端只能靠超时发现。
        d->camera->goOffline(seconds, false);
        return;
    }

    // quirk F3：请求速率过载时整机假死再回来，复现廉价固件被打挂。
    if (d->overloaded(quirks)) {
        const int seconds = quirks.paramInt(QuirkId::OverloadReboot,
                                            QStringLiteral("offline_seconds"));
        d->recentRequests.clear();   // 免得离线恢复后立刻又被旧记录判成过载
        if (log) {
            record.level = LogLevel::Warning;
            record.ok = false;
            record.summary = QStringLiteral("Overloaded, simulating a %1 s reboot").arg(seconds);
            record.quirkKey = QuirkRegistry::def(QuirkId::OverloadReboot).key;
            log->post(record);
        }
        if (d->camera)
            d->camera->goOffline(seconds);
        return;   // 不回包，连接会被 goOffline 关掉
    }

    const SoapRequest soapRequest = soap::parseEnvelope(request.body);
    record.detail = QString::fromUtf8(request.body);

    auto finish = [&](HttpResponse response, const QString &summary, bool ok,
                      LogLevel level = LogLevel::Info, const QString &quirkKey = QString()) {
        soap::applyResponseQuirks(quirks, response, request, soapRequest.bodyNamespace);
        record.summary = summary;
        record.ok = ok;
        record.level = level;
        record.durationUs = timer.nsecsElapsed() / 1000;
        record.quirkKey = quirkKey;
        if (!response.body.isEmpty())
            record.detail += QStringLiteral("\n---- response ----\n")
                             + QString::fromUtf8(response.body);
        if (log)
            log->post(record);
        exchange->respond(response);
    };

    auto respondFault = [&](const SoapFault &fault, const QString &summary,
                            const QString &quirkKey = QString()) {
        const int version = soapRequest.soapVersion;
        HttpResponse response = HttpResponse::soap(soap::makeFault(version, fault),
                                                   soap::faultHttpStatus(quirks, fault));
        finish(response, summary, false, LogLevel::Warning, quirkKey);
    };

    if (!soapRequest.isValid()) {
        SoapFault f;
        f.subcode = QString::fromLatin1(ter::InvalidArgs);
        f.reason = soapRequest.parseError.isEmpty()
                       ? QStringLiteral("Malformed SOAP envelope")
                       : soapRequest.parseError;
        respondFault(f, QStringLiteral("SOAP parse failed: %1").arg(f.reason));
        return;
    }

    record.summary = soapRequest.bodyName;

    SoapService *service = serviceByNamespace(soapRequest.bodyNamespace);
    const SoapOperation *operation = service ? service->findOperation(soapRequest.bodyName)
                                             : nullptr;

    // quirk A7：整个 GetServices 操作不实现。
    if (operation && soapRequest.bodyName == QLatin1String("GetServices")
        && quirks.isEnabled(QuirkId::NoGetServices)) {
        respondFault(soap::notSupported(soapRequest.bodyName),
                     QStringLiteral("GetServices disabled by quirk"),
                     QuirkRegistry::def(QuirkId::NoGetServices).key);
        return;
    }

    if (!service || !operation) {
        respondFault(soap::notSupported(soapRequest.bodyName),
                     QStringLiteral("Unsupported operation %1 (ns=%2)")
                         .arg(soapRequest.bodyName, soapRequest.bodyNamespace));
        return;
    }

    // ---- 鉴权 ----
    AuthLevel required = operation->auth;
    // quirk：连 PRE_AUTH 操作也要鉴权。规范允许 GetSystemDateAndTime /
    // GetCapabilities / GetServices / GetWsdlUrl 匿名调用，有些固件不允许。
    if (required == AuthLevel::PreAuth && quirks.isEnabled(QuirkId::AuthPreAuthRequired))
        required = AuthLevel::User;

    // 没有相机时**拒绝**而不是放行。生产路径上 dispatcher 一定带着相机，
    // 但「条件里带个 && camera 就顺手把鉴权整段跳过」是 fail-open，
    // 对一个别人会照着改的开源项目是很坏的示范。
    if (required != AuthLevel::PreAuth && !d->camera) {
        SoapFault fault;
        fault.subcode = QString::fromLatin1(ter::NotAuthorized);
        fault.reason = QStringLiteral("设备未就绪，无法校验凭据");
        respondFault(fault, QStringLiteral("%1 无相机绑定，拒绝").arg(soapRequest.bodyName));
        return;
    }

    if (required != AuthLevel::PreAuth) {
        const WsSecurityResult auth = d->security.verify(soapRequest, d->camera->model(), quirks,
                                                         d->camera->deviceTimeUtc());
        if (!auth.authenticated || !WsSecurity::levelSatisfies(auth.level, required)) {
            const QString why = auth.failureReason.isEmpty()
                                    ? QStringLiteral("权限不足")
                                    : auth.failureReason;
            // 表现形式可切：规范的 SOAP Fault，或直接 HTTP 401 带 WWW-Authenticate。
            if (quirks.isEnabled(QuirkId::AuthHttp401NotFault)) {
                HttpResponse response = HttpResponse::text(401, QStringLiteral("Unauthorized"));
                response.setHeader("WWW-Authenticate",
                                   QByteArray("Digest realm=\"onvifsim\", qop=\"auth\", nonce=\"")
                                       + QByteArray::number(QRandomGenerator::global()->generate())
                                       + QByteArray("\""));
                finish(response, QStringLiteral("%1 authentication failed (401): %2")
                                     .arg(soapRequest.bodyName, why),
                       false, LogLevel::Warning,
                       QuirkRegistry::def(QuirkId::AuthHttp401NotFault).key);
                return;
            }
            const SoapFault f = soap::authFault(quirks);
            respondFault(f, QStringLiteral("%1 authentication failed: %2").arg(soapRequest.bodyName, why),
                         quirks.isEnabled(QuirkId::AuthFaultWording)
                             ? QuirkRegistry::def(QuirkId::AuthFaultWording).key
                             : QString());
            return;
        }
        record.summary = QStringLiteral("%1（%2）").arg(soapRequest.bodyName, auth.username);
    }

    // ---- 调 handler ----
    XmlWriter writer(soapRequest.soapVersion);
    writer.declareServicePrefixes();
    // quirk D5：属性值不加引号的非法 XML。只对 GetEventProperties 生效，
    // 因为真机（TP-Link TL-IPC）只在这个响应上出这个毛病。
    if (quirks.isEnabled(QuirkId::EventPropertiesBadXml)
        && soapRequest.bodyName == QLatin1String("GetEventProperties")) {
        writer.setUnquotedAttributes(true);
    }
    writer.startEnvelope();

    SoapContext ctx;
    ctx.camera = d->camera;
    ctx.soap = &soapRequest;
    ctx.body = &soapRequest.body;
    ctx.http = &request;
    ctx.exchange = exchange;
    ctx.out = &writer;

    operation->handler(ctx);

    // handler 声明自己会回包（PullMessages 的长轮询、C5 的畸形响应）。
    if (ctx.responseTakenOver()) {
        record.summary += QStringLiteral(" (handler replied directly)");
        record.durationUs = timer.nsecsElapsed() / 1000;
        if (log)
            log->post(record);
        return;
    }

    if (ctx.hasFault()) {
        respondFault(ctx.pendingFault(),
                     QStringLiteral("%1 → Fault %2")
                         .arg(soapRequest.bodyName, ctx.pendingFault().subcode));
        return;
    }

    writer.endEnvelope();
    finish(HttpResponse::soap(writer.take()), record.summary, true);
}

} // namespace onvifsim
