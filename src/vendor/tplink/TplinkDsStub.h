#pragma once

// TP-Link TL-IPC 的 `/stok=<t>/ds` 私有 JSON 桩。
//
// 参照客户端对这条线的用法很窄：事件与 PTZ 全走通用 ONVIF 适配器，
// 私有接口只用来补 ONVIF 没有的灯光控制（reference-client-facts.md §7.2）。
// 所以这里只做「登录拿 stok」+「读写灯光」两件事，别的一律回不支持。
//
// 对讲的 MULTITRANS 私有协议（554 端口）按 plan.md §4.13 列为二期，不在这里。

#include "net/HttpTypes.h"
#include "vendor/VendorApiStub.h"

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

namespace onvifsim {

class TplinkDsStub : public VendorApiStub
{
    Q_OBJECT
public:
    explicit TplinkDsStub(VirtualCamera *camera, QObject *parent = nullptr);
    ~TplinkDsStub() override;

    void registerRoutes(HttpServer *server) override;
    VendorApi kind() const override;

    // 同步处理一条请求（登录或 /ds）；单测直接调它。
    HttpResponse handle(const HttpRequest &request);

    // ---- stok（客户端在 stok 失效时只重试一次，单测要能造出这个状态）----
    QString currentStok() const;
    void expireStok();

    // 错误码。0 以外都是错，客户端只看 error_code 这一个字段。
    static int errInvalidStok();      // -40401
    static int errUnsupported();      // -40210
    static int errBadParam();         // -40209

    static QByteArray encode(const QJsonObject &object);
    // 从 /stok=<t>/ds 这样的路径里抠出 stok。
    static QString stokFromPath(const QString &path);

private:
    QJsonObject handleLogin(const QJsonObject &request);
    QJsonObject handleDs(const QJsonObject &request);
    QJsonObject readLight() const;
    QJsonObject writeLight(const QJsonObject &module);

    QString issueStok();
    bool stokAccepted(const QString &stok) const;

    QString m_stok;
    QDateTime m_stokExpiry;
    bool m_routesRegistered = false;
};

} // namespace onvifsim
