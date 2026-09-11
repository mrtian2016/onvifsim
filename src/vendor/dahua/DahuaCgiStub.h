#pragma once

// 大华 CGI 私有 API 桩（HTTP Digest 鉴权）。
//
// 端点集按 plan.md §4.13 / reference-client-facts.md §7.2 收敛。大华的响应体
// 一律是 `key=value` 的纯文本（不是 XML 也不是 JSON），层级用 `table.X[0][1].Y`
// 这种下标串表达 —— 客户端的解析器就是按这个形状写的，格式必须逐字贴住。
//
// 状态与 ONVIF 侧共享：灯光走 ImagingState、云台走 PtzState、
// eventManager attach 接 EventEngine 的 eventProduced。

#include "core/CameraModel.h"
#include "core/Persona.h"
#include "events/EventTypes.h"
#include "net/HttpAuth.h"
#include "net/HttpTypes.h"
#include "vendor/VendorApiStub.h"

#include <QtCore/QByteArray>
#include <QtCore/QMap>
#include <QtCore/QSharedPointer>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

namespace onvifsim {

class DahuaCgiStub : public VendorApiStub
{
    Q_OBJECT
public:
    explicit DahuaCgiStub(VirtualCamera *camera, QObject *parent = nullptr);
    ~DahuaCgiStub() override;

    void registerRoutes(HttpServer *server) override;
    VendorApi kind() const override;
    int streamingClients() const override;

    // 同步处理一条非流式 CGI 请求；单测直接调它。
    HttpResponse handle(const HttpRequest &request);

    // ---- 纯函数部分 ----

    static QByteArray systemInfoText(const CameraModel &model, const Persona &persona);

    // 一行事件：`Code=VideoMotion;action=Start;index=0`（可选再挂 `;data={...}`）。
    // 这是 attach 长连接里客户端唯一会解析的东西，格式由单测钉死。
    static QByteArray eventLine(const QString &code, const QString &action, int index,
                                const QByteArray &dataJson = QByteArray());

    // 把一行事件包成 attach 用的 multipart 片段。
    static QByteArray multipartPart(const QByteArray &line);

    // EventKind → 大华订阅码。只落在 reference-client-facts.md §7.2 列的 8 个码里。
    static QString eventCodeFor(EventKind kind);

    // `codes=[VideoMotion,CrossLineDetection]` / `codes=[All]` → 码表。
    static QStringList parseCodes(const QString &value);

private:
    struct AttachClient;

    HttpResponse handleMagicBox(const HttpRequest &request);
    HttpResponse handlePtz(const HttpRequest &request);
    HttpResponse handleConfigManager(const HttpRequest &request);
    HttpResponse handleCoaxialControl(const HttpRequest &request);
    HttpResponse handleSnapshot(const HttpRequest &request);
    HttpResponse handleEventManager(const HttpRequest &request);

    // attach 长连接（HttpExchange 的 beginStream / writeChunk / endStream）。
    void beginAttach(const HttpRequest &request, const HttpExchangePtr &exchange);
    void onEventProduced(const EventMessage &message);
    bool push(const QSharedPointer<AttachClient> &client, const QByteArray &chunk);
    void drop(const QSharedPointer<AttachClient> &client);

    bool authorize(const HttpRequest &request, HttpResponse *response);
    const Persona &personaOrDefault() const;

    HttpAuth m_auth;
    QVector<QSharedPointer<AttachClient>> m_attachClients;
    bool m_routesRegistered = false;
};

} // namespace onvifsim
