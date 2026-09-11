#include "soap/WsSecurity.h"

#include "core/Quirks.h"

#include <QtCore/QCryptographicHash>

namespace onvifsim {
namespace {

// 时间窗没开 quirk 时的宽松默认：规范建议 ±5 分钟。
constexpr int kDefaultTimeWindowSeconds = 300;

// 把 xs:dateTime 原文解成 UTC 时刻。ONVIF 的 Created 约定是 UTC，
// 但真机常常不写 Z —— 缺时区就直接补一个再解析，免得落到本地时区上。
QDateTime parseCreated(const QString &text, bool *ok)
{
    *ok = false;
    QString s = text.trimmed();
    if (s.isEmpty())
        return QDateTime();

    const int tPos = s.indexOf(QLatin1Char('T'));
    const QString timePart = tPos >= 0 ? s.mid(tPos + 1) : QString();
    const bool hasZone = timePart.endsWith(QLatin1Char('Z'), Qt::CaseInsensitive)
                         || timePart.contains(QLatin1Char('+'))
                         || timePart.contains(QLatin1Char('-'));
    if (!hasZone)
        s += QLatin1Char('Z');

    QDateTime dt = QDateTime::fromString(s, Qt::ISODateWithMs);
    if (!dt.isValid())
        dt = QDateTime::fromString(s, Qt::ISODate);
    if (!dt.isValid())
        return QDateTime();

    *ok = true;
    return dt.toUTC();
}

int levelRank(UserLevel level)
{
    switch (level) {
    case UserLevel::Administrator: return 3;
    case UserLevel::Operator:      return 2;
    case UserLevel::User:          return 1;
    case UserLevel::Anonymous:     return 0;
    }
    return 0;
}

int levelRank(AuthLevel level)
{
    switch (level) {
    case AuthLevel::Administrator: return 3;
    case AuthLevel::Operator:      return 2;
    case AuthLevel::User:          return 1;
    case AuthLevel::PreAuth:       return 0;
    }
    return 0;
}

WsSecurityResult failure(const QString &username, const QString &reason)
{
    WsSecurityResult r;
    r.username = username;
    r.failureReason = reason;
    return r;
}

} // namespace

WsSecurity::WsSecurity() = default;

WsSecurityResult WsSecurity::verify(const SoapRequest &request, const CameraModel &model,
                                    const Quirks &quirks, const QDateTime &deviceTimeUtc)
{
    // 没带 Security 头（或带了但没 Username）就是匿名。参照客户端在 username
    // 为空时确实整个 Security 头都不发。是否放行由调用方按操作的 AuthLevel 决定。
    if (!request.hasSecurity || request.username.isEmpty()) {
        WsSecurityResult r;
        r.anonymous = true;
        r.level = UserLevel::Anonymous;
        r.failureReason = QStringLiteral("no WS-Security UsernameToken");
        return r;
    }

    const User *user = model.findUser(request.username);
    if (!user)
        return failure(request.username, QStringLiteral("unknown user"));

    if (request.passwordIsDigest && quirks.isEnabled(QuirkId::AuthPasswordTextOnly)) {
        // 参照客户端一律发 Digest，这条打开就是全线鉴权失败。
        return failure(request.username, QStringLiteral("PasswordDigest rejected by device"));
    }

    // Created 时间窗。deviceTimeUtc 是相机自己的钟（clock_skew 已由调用方叠好），
    // 客户端不做时钟补偿时，收紧窗口 + 拨偏时钟就能复现「全线 401」（A8）。
    if (!request.created.isEmpty()) {
        bool ok = false;
        const QDateTime created = parseCreated(request.created, &ok);
        if (!ok)
            return failure(request.username, QStringLiteral("malformed Created timestamp"));

        const int window = quirks.isEnabled(QuirkId::AuthTightTimeWindow)
                               ? quirks.paramInt(QuirkId::AuthTightTimeWindow,
                                                 QStringLiteral("seconds"))
                               : kDefaultTimeWindowSeconds;
        const qint64 drift = qAbs(created.secsTo(deviceTimeUtc));
        if (drift > window) {
            return failure(request.username,
                           QStringLiteral("Created is %1s off the device clock (window %2s)")
                               .arg(drift)
                               .arg(window));
        }
    }

    const QByteArray nonce = QByteArray::fromBase64(request.nonceBase64.toLatin1());
    const bool strictNonce = quirks.isEnabled(QuirkId::AuthNonceStrictOnce);
    if (strictNonce && !nonce.isEmpty()) {
        const int cacheSeconds =
            quirks.paramInt(QuirkId::AuthNonceStrictOnce, QStringLiteral("cache_seconds"));
        // 先按缓存时长清一遍，长跑进程的 nonce 表不能无上限涨。
        for (auto it = m_nonces.begin(); it != m_nonces.end();) {
            if (it.value().secsTo(deviceTimeUtc) > cacheSeconds)
                it = m_nonces.erase(it);
            else
                ++it;
        }
        if (m_nonces.contains(nonce))
            return failure(request.username, QStringLiteral("nonce replayed"));
    }

    if (request.passwordIsDigest) {
        const QString expected = computePasswordDigest(nonce, request.created, user->password);
        if (expected != request.password)
            return failure(request.username, QStringLiteral("password digest mismatch"));
    } else if (request.password != user->password) {
        return failure(request.username, QStringLiteral("password mismatch"));
    }

    // 只在整体校验通过后才把 nonce 记为已用：密码打错不该烧掉一个 nonce。
    if (strictNonce && !nonce.isEmpty())
        m_nonces.insert(nonce, deviceTimeUtc);

    WsSecurityResult r;
    r.authenticated = true;
    r.username = user->username;
    r.level = user->level;
    return r;
}

bool WsSecurity::levelSatisfies(UserLevel actual, AuthLevel required)
{
    // PreAuth 的 rank 是 0，匿名（也是 0）天然满足。
    // 「连 PRE_AUTH 也要鉴权」（AuthPreAuthRequired）由调用方把 required 抬到 User 来实现，
    // 这个函数保持纯函数，不看 quirk。
    return levelRank(actual) >= levelRank(required);
}

void WsSecurity::clearNonceCache()
{
    m_nonces.clear();
}

int WsSecurity::nonceCacheSize() const
{
    return static_cast<int>(m_nonces.size());
}

QString WsSecurity::computePasswordDigest(const QByteArray &nonce, const QString &created,
                                          const QString &password)
{
    // Base64(SHA1(nonce_raw + created_utf8 + password_utf8))。
    // nonce 参与摘要的是 Base64 解码后的原始字节，不是 Base64 串本身 —— 最常见的实现错误。
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(nonce);
    hash.addData(created.toUtf8());
    hash.addData(password.toUtf8());
    return QString::fromLatin1(hash.result().toBase64());
}

} // namespace onvifsim
