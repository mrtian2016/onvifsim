#include "net/HttpAuth.h"

#include "core/Quirks.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QRandomGenerator>
#include <QtCore/QSet>

namespace onvifsim {
namespace {

QByteArray md5Hex(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex();
}

// 挑战里给了什么参数，客户端就只许回什么参数（外加它自己必须带的那几个）。
// E9 的严格模式按这张表判，多一个 algorithm=MD5 就 401。
const char *const kClientOwnedParams[] = {
    "username", "uri", "response", "nc", "cnonce"
};

QByteArray quoted(const QByteArray &value)
{
    QByteArray escaped = value;
    escaped.replace('\\', "\\\\").replace('"', "\\\"");
    return '"' + escaped + '"';
}

} // namespace

HttpAuth::HttpAuth(const QString &realm)
    : m_realm(realm)
{
}

QString HttpAuth::realm() const
{
    return m_realm;
}

void HttpAuth::setNonceLifetime(int seconds)
{
    m_nonceLifetime = seconds;
}

QByteArray HttpAuth::issueNonce()
{
    // 真机的 nonce 通常是 时间戳 + 随机数 的摘要。这里同样只求「不可预测且能查表」，
    // 有效期与用过几次都记在 m_nonces 里，不往 nonce 本身编码。
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QByteArray seed = QByteArray::number(now.toMSecsSinceEpoch())
        + ':' + QByteArray::number(QRandomGenerator::global()->generate64(), 16);
    const QByteArray nonce = md5Hex(seed);

    // 顺手清掉早就过期的，别让长跑进程把表撑起来。
    const QDateTime deadline = now.addSecs(-qMax(1, m_nonceLifetime) * 2);
    for (auto it = m_nonces.begin(); it != m_nonces.end();) {
        if (it->issued < deadline)
            it = m_nonces.erase(it);
        else
            ++it;
    }

    Nonce entry;
    entry.issued = now;
    m_nonces.insert(nonce, entry);
    return nonce;
}

QList<QByteArray> HttpAuth::challenges(const Quirks &quirks, bool stale)
{
    // 返回的是 WWW-Authenticate 的「值」，HTTP 与 RTSP 都直接拿去当头值用，
    // 两边的头名一样，所以不需要按 protocol 分支。
    QByteArray digest = "Digest realm=" + quoted(m_realm.toUtf8())
        + ", nonce=" + quoted(issueNonce())
        + ", qop=" + quoted("auth");
    if (stale)
        digest += ", stale=true";   // 只是 nonce 老了，客户端别再弹用户名密码框
    // 注意这里不写 algorithm：严格模式（E9）判的就是「挑战没给过的参数」，
    // 挑战自己带上 algorithm 反而会让那条 quirk 失效。
    const QByteArray basic = "Basic realm=" + quoted(m_realm.toUtf8());

    if (quirks.isEnabled(QuirkId::AuthDualChallenge)) {
        // E8：TP-LINK 的相机一次发两条，有的客户端只认第一条，所以顺序要能配。
        if (quirks.paramString(QuirkId::AuthDualChallenge, QStringLiteral("order"))
            == QLatin1String("basic_first"))
            return { basic, digest };
        return { digest, basic };
    }
    return { digest };
}

QHash<QByteArray, QByteArray> HttpAuth::parseAuthParams(const QByteArray &value)
{
    QHash<QByteArray, QByteArray> params;
    QByteArray input = value.trimmed();

    // 允许直接传整条头值（"Digest username=..."）：开头那个不带 '=' 的 token 是 scheme。
    const qsizetype firstSpace = input.indexOf(' ');
    if (firstSpace > 0) {
        const QByteArray head = input.left(firstSpace);
        if (!head.contains('='))
            input = input.mid(firstSpace + 1).trimmed();
    }

    qsizetype i = 0;
    while (i < input.size()) {
        while (i < input.size()
               && (input.at(i) == ' ' || input.at(i) == ',' || input.at(i) == '\t'))
            ++i;
        const qsizetype keyStart = i;
        while (i < input.size() && input.at(i) != '=' && input.at(i) != ',')
            ++i;
        const QByteArray key = input.mid(keyStart, i - keyStart).trimmed().toLower();
        if (key.isEmpty()) {
            ++i;
            continue;
        }
        if (i >= input.size() || input.at(i) != '=') {
            params.insert(key, QByteArray());   // 没有值的裸 token，也记下来给严格模式判
            continue;
        }
        ++i;   // 跳过 '='
        QByteArray val;
        if (i < input.size() && input.at(i) == '"') {
            ++i;
            while (i < input.size() && input.at(i) != '"') {
                // 引号里的逗号是值的一部分，转义要还原。
                if (input.at(i) == '\\' && i + 1 < input.size())
                    ++i;
                val += input.at(i);
                ++i;
            }
            ++i;   // 跳过收尾引号
        } else {
            const qsizetype valStart = i;
            while (i < input.size() && input.at(i) != ',')
                ++i;
            val = input.mid(valStart, i - valStart).trimmed();
        }
        params.insert(key, val);
    }
    return params;
}

QByteArray HttpAuth::digestResponse(const QByteArray &username, const QByteArray &realm,
                                    const QByteArray &password, const QByteArray &method,
                                    const QByteArray &uri, const QByteArray &nonce,
                                    const QByteArray &nc, const QByteArray &cnonce,
                                    const QByteArray &qop)
{
    // RFC 2617 §3.2.2.1，algorithm=MD5、qop=auth 这一支。
    const QByteArray ha1 = md5Hex(username + ':' + realm + ':' + password);
    const QByteArray ha2 = md5Hex(method + ':' + uri);
    if (qop.isEmpty())
        return md5Hex(ha1 + ':' + nonce + ':' + ha2);
    return md5Hex(ha1 + ':' + nonce + ':' + nc + ':' + cnonce + ':' + qop + ':' + ha2);
}

AuthResult HttpAuth::verify(const QByteArray &authorization, const QByteArray &method,
                            const QByteArray &uri, const CameraModel &model, const Quirks &quirks)
{
    AuthResult result;
    if (authorization.trimmed().isEmpty()) {
        result.failureReason = QStringLiteral("缺少 Authorization 头");
        return result;
    }

    const QByteArray trimmed = authorization.trimmed();
    const qsizetype space = trimmed.indexOf(' ');
    const QByteArray scheme = (space > 0 ? trimmed.left(space) : trimmed).toLower();
    const QByteArray rest = space > 0 ? trimmed.mid(space + 1).trimmed() : QByteArray();

    // 用户表直接遍历 model.users：Digest 要拿明文口令算 HA1，
    // 没法走 CameraModel::checkPassword 那种只回真假的接口。
    const auto findUser = [&model](const QString &name) -> const User * {
        for (const User &user : model.users) {
            if (user.username == name)
                return &user;
        }
        return nullptr;
    };

    if (scheme == "basic") {
        result.scheme = AuthScheme::Basic;
        const QByteArray decoded = QByteArray::fromBase64(rest);
        const qsizetype colon = decoded.indexOf(':');
        if (colon < 0) {
            result.failureReason = QStringLiteral("Basic 凭据格式非法");
            return result;
        }
        const QString username = QString::fromUtf8(decoded.left(colon));
        const QString password = QString::fromUtf8(decoded.mid(colon + 1));
        const User *user = findUser(username);
        result.username = username;
        if (!user || user->password != password) {
            result.failureReason = QStringLiteral("用户名或口令不对");
            return result;
        }
        result.authenticated = true;
        result.level = user->level;
        return result;
    }

    if (scheme != "digest") {
        result.failureReason = QStringLiteral("不支持的鉴权方式：%1")
                                   .arg(QString::fromUtf8(scheme));
        return result;
    }

    result.scheme = AuthScheme::Digest;
    const QHash<QByteArray, QByteArray> params = parseAuthParams(rest);

    if (quirks.isEnabled(QuirkId::AuthStrictDigestParams)) {
        // E9：TP-LINK 的 RTSP 服务对「挑战里没出现过的参数」零容忍，
        // 客户端习惯性带上的 algorithm=MD5 就足以让它全线 401。
        QSet<QByteArray> allowed;
        allowed.insert("realm");
        allowed.insert("nonce");
        allowed.insert("qop");
        for (const char *name : kClientOwnedParams)
            allowed.insert(QByteArray(name));
        for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
            if (!allowed.contains(it.key())) {
                result.failureReason = QStringLiteral("严格模式：挑战没给过的参数 %1（E9）")
                                           .arg(QString::fromUtf8(it.key()));
                return result;
            }
        }
    }

    const QString username = QString::fromUtf8(params.value("username"));
    const QByteArray nonce = params.value("nonce");
    const QByteArray qop = params.value("qop");
    const QByteArray nc = params.value("nc");
    const QByteArray cnonce = params.value("cnonce");
    const QByteArray response = params.value("response").toLower();
    // 客户端算摘要用的是它自己写进 Authorization 的 uri，服务端必须拿同一个串算，
    // 否则 XAddr 被改写过（rehost）就永远对不上。
    const QByteArray digestUri = params.contains("uri") ? params.value("uri") : uri;
    result.username = username;

    if (username.isEmpty() || nonce.isEmpty() || response.isEmpty()) {
        result.failureReason = QStringLiteral("Digest 凭据缺字段");
        return result;
    }

    const auto it = m_nonces.find(nonce);
    if (it == m_nonces.end()) {
        // 进程重启或 nonce 早被清掉了。回 stale 让客户端拿新挑战静默重试。
        result.stale = true;
        result.failureReason = QStringLiteral("nonce 不认识");
        return result;
    }
    if (it->issued.secsTo(QDateTime::currentDateTimeUtc()) > m_nonceLifetime) {
        result.stale = true;
        result.failureReason = QStringLiteral("nonce 已过期");
        return result;
    }
    if (quirks.isEnabled(QuirkId::AuthNonceStrictOnce) && it->useCount > 0) {
        // 严格一次性 nonce：重放即 401，逼客户端每次都重新握手。
        result.failureReason = QStringLiteral("nonce 只允许用一次");
        return result;
    }
    const User *user = findUser(username);
    if (!user) {
        result.failureReason = QStringLiteral("用户不存在");
        return result;
    }

    const QByteArray expected = digestResponse(username.toUtf8(), m_realm.toUtf8(),
                                               user->password.toUtf8(), method, digestUri,
                                               nonce, nc, cnonce, qop);
    if (expected != response) {
        result.failureReason = QStringLiteral("摘要对不上");
        return result;
    }

    // useCount 要在**校验通过之后**才自增。加在前面的话，一次打错口令就把
    // nonce 烧掉了（E9 严格一次性下尤其致命）：用户重试时拿到的失败原因变成
    // 「nonce 只允许用一次」，把真正的原因「口令错了」盖住 ——
    // 这正是 CLAUDE.md 里那条「日志要说清客户端到底卡在哪一步」反对的事。
    ++it->useCount;

    result.authenticated = true;
    result.level = user->level;
    return result;
}

} // namespace onvifsim
