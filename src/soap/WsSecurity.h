#pragma once

// WS-Security UsernameToken 校验。
// PasswordText 与 PasswordDigest（Base64(SHA1(nonce + created + password))）都支持。
//
// 参照客户端一律发 PasswordDigest 且不做时钟补偿，
// 所以 AuthTightTimeWindow 收紧时间窗 + 注入时钟偏移就能复现「全线 401」。

#include "core/CameraModel.h"
#include "services/ServiceBase.h"
#include "soap/Envelope.h"

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QString>

namespace onvifsim {

class Quirks;

struct WsSecurityResult {
    bool authenticated = false;
    bool anonymous = false;      // 没带 Security 头
    QString username;
    UserLevel level = UserLevel::Anonymous;
    QString failureReason;
};

class WsSecurity
{
public:
    WsSecurity();

    // deviceTime 是相机自己的时钟（可能被 clock_skew 拨偏）。
    WsSecurityResult verify(const SoapRequest &request, const CameraModel &model,
                            const Quirks &quirks, const QDateTime &deviceTimeUtc);

    // 权限门控：操作要求的级别 vs 实际身份。PreAuth 允许匿名，
    // 除非 quirk AuthPreAuthRequired 打开。
    static bool levelSatisfies(UserLevel actual, AuthLevel required);

    void clearNonceCache();
    int nonceCacheSize() const;

    static QString computePasswordDigest(const QByteArray &nonce, const QString &created,
                                         const QString &password);

private:
    QHash<QByteArray, QDateTime> m_nonces;
};

} // namespace onvifsim
