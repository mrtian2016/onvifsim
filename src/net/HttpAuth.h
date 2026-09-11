#pragma once

// HTTP Basic / Digest（RFC 2617，MD5，qop=auth）。HTTP 与 RTSP 共用同一份实现与用户表。
//
// 两个 quirk 直接落在这里：
//   E8 同时发 Basic + Digest 两条 WWW-Authenticate，顺序可配
//   E9 严格参数模式：客户端 Authorization 里出现挑战没给过的参数（如 algorithm=MD5）就 401

#include "core/CameraModel.h"

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QString>

namespace onvifsim {

class Quirks;

enum class AuthScheme { None, Basic, Digest };

struct AuthResult {
    bool authenticated = false;
    QString username;
    UserLevel level = UserLevel::Anonymous;
    AuthScheme scheme = AuthScheme::None;
    bool stale = false;         // nonce 过期，挑战里要带 stale=true
    QString failureReason;      // 进日志用
};

class HttpAuth
{
public:
    explicit HttpAuth(const QString &realm = QStringLiteral("onvifsim"));

    QString realm() const;
    void setNonceLifetime(int seconds);

    // 生成挑战头的**值**（不带 "WWW-Authenticate: " 前缀），HTTP 与 RTSP 通用。
    // quirk E8 打开时返回两条（Basic + Digest，顺序由参数决定）；
    // 放进 HttpResponse::headers 时要用 '\n' 串起来，见 HttpTypes.h 的说明。
    QList<QByteArray> challenges(const Quirks &quirks, bool stale = false);

    // 校验 Authorization 头。method 是 HTTP/RTSP 方法，uri 是请求行里的原始 target。
    AuthResult verify(const QByteArray &authorization, const QByteArray &method,
                      const QByteArray &uri, const CameraModel &model, const Quirks &quirks);

    // 供测试与 RTSP 复用。
    static QByteArray digestResponse(const QByteArray &username, const QByteArray &realm,
                                     const QByteArray &password, const QByteArray &method,
                                     const QByteArray &uri, const QByteArray &nonce,
                                     const QByteArray &nc, const QByteArray &cnonce,
                                     const QByteArray &qop);
    static QHash<QByteArray, QByteArray> parseAuthParams(const QByteArray &value);

private:
    struct Nonce {
        QDateTime issued;
        int useCount = 0;
    };
    QString m_realm;
    int m_nonceLifetime = 300;
    QHash<QByteArray, Nonce> m_nonces;
    QByteArray issueNonce();
};

} // namespace onvifsim
