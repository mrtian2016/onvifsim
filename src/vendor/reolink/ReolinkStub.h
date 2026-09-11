#pragma once

// Reolink 私有 JSON-RPC 桩（/api.cgi，token 认证，不走 Digest）。
//
// 报文形状按 reference-client-facts.md §7.2：请求体是一个命令数组
//   [{"cmd":"GetDevInfo","action":0,"param":{"channel":0}}]
// 响应也是数组，每项 {"cmd":…,"code":0,"value":{…}}；出错时 code 非 0 且带 error。
// token 过期用 code = -6 表示 —— 客户端就是靠这个值决定「重新 Login 再重试一次」。
//
// GetWhiteLed / SetWhiteLed 改的是 ImagingState 的白光灯，与海康 supplementLight、
// TP-Link /ds 是同一份状态。

#include "net/HttpTypes.h"
#include "vendor/VendorApiStub.h"

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

namespace onvifsim {

class ReolinkStub : public VendorApiStub
{
    Q_OBJECT
public:
    explicit ReolinkStub(VirtualCamera *camera, QObject *parent = nullptr);
    ~ReolinkStub() override;

    void registerRoutes(HttpServer *server) override;
    VendorApi kind() const override;

    // 同步处理一条 /api.cgi 请求；单测直接调它。
    HttpResponse handle(const HttpRequest &request);

    // ---- token（单测要能造出「过期」这个状态）----
    QString currentToken() const;
    void expireToken();
    static int tokenExpiredCode();      // -6

    // ---- 纯函数部分 ----
    static QByteArray encodeResponse(const QJsonArray &items);
    static QJsonObject successItem(const QString &cmd, const QJsonObject &value);
    static QJsonObject rspCodeItem(const QString &cmd, int rspCode);
    static QJsonObject errorItem(const QString &cmd, int rspCode, const QString &detail);

private:
    QJsonObject dispatch(const QString &cmd, const QJsonObject &item);

    QJsonObject cmdLogin(const QJsonObject &item);
    QJsonObject cmdGetDevInfo() const;
    QJsonObject cmdGetAbility() const;
    QJsonObject cmdGetAiState() const;
    QJsonObject cmdGetEnc() const;
    QJsonObject cmdGetWhiteLed() const;
    QJsonObject cmdSetWhiteLed(const QJsonObject &item);
    QJsonObject cmdGetPtzPreset() const;
    QJsonObject cmdSetPtzPreset(const QJsonObject &item);
    QJsonObject cmdDelPtzPreset(const QJsonObject &item);
    QJsonObject cmdPtzCtrl(const QJsonObject &item);
    QJsonObject cmdAudioAlarmPlay(const QJsonObject &item);

    QString issueToken();
    bool tokenAccepted(const QString &token) const;

    QString m_token;
    QDateTime m_tokenExpiry;
    bool m_routesRegistered = false;
};

} // namespace onvifsim
