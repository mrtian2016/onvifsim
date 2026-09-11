#include "services/media/SnapshotEndpoint.h"

#include "core/LogBus.h"
#include "core/VirtualCamera.h"
#include "media/Snapshot.h"
#include "net/HttpAuth.h"
#include "net/HttpServer.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QList>
#include <QtCore/QSharedPointer>

#include <optional>

namespace onvifsim {
namespace {

// B5 的 token 档：令牌按时间窗滚动。校验时同时接受上一窗，
// 否则客户端刚拿到 URI 就可能撞上换窗，那不是我们想测的失败。
constexpr int kTokenWindowSeconds = 300;

// 一次性令牌 = 相机 id + profile + 时间窗的摘要。两边同算法即可，不必存状态。
QString tokenForWindow(const VirtualCamera *camera, const QString &profileToken, qint64 window)
{
    QByteArray seed = camera->id().toUtf8();
    seed += '|';
    seed += profileToken.toUtf8();
    seed += '|';
    seed += QByteArray::number(window);
    seed += "|onvifsim-snapshot";
    return QString::fromLatin1(
        QCryptographicHash::hash(seed, QCryptographicHash::Md5).toHex().left(16));
}

bool verifyAccessToken(const VirtualCamera *camera, const QString &profileToken,
                       const QString &provided)
{
    if (provided.isEmpty())
        return false;
    const qint64 window = QDateTime::currentSecsSinceEpoch() / kTokenWindowSeconds;
    return provided == tokenForWindow(camera, profileToken, window)
           || provided == tokenForWindow(camera, profileToken, window - 1);
}

// 挑出指定 scheme 的挑战头。E8 打开时 HttpAuth 会同时给 Basic 与 Digest，
// 但快照这边由 B5 指定了唯一一种，只发那一条 ——
// 客户端「Digest → Basic → 无，且只在 401 时才换」的顺序才测得准。
QByteArray challengeHeader(HttpAuth &auth, const Quirks &quirks, AuthScheme scheme, bool stale)
{
    const QList<QByteArray> all = auth.challenges(quirks, stale);
    const QByteArray wanted = scheme == AuthScheme::Digest ? QByteArray("Digest")
                                                           : QByteArray("Basic");
    QList<QByteArray> picked;
    for (const QByteArray &c : all) {
        if (c.startsWith(wanted))
            picked.append(c);
    }
    if (picked.isEmpty())
        picked = all;
    // HttpServer 把 '\n' 拆成多行同名头，这是它约定的「一个 key 发多条」写法。
    QByteArray joined;
    for (const QByteArray &c : picked) {
        if (!joined.isEmpty())
            joined += '\n';
        joined += c;
    }
    return joined;
}

// 快照的坏响应之一：HTML 登录页。真机上「会话过期」时就返回这个，
// 客户端如果只看 HTTP 状态码就会把它当成一张图。
const char *kLoginPage =
    "<html><head><title>Login</title></head><body>"
    "<h1>Authentication required</h1>"
    "<form action=\"/login\" method=\"post\">"
    "<input name=\"username\"><input name=\"password\" type=\"password\">"
    "<input type=\"submit\" value=\"Login\"></form></body></html>";

// level 不给就按 ok 自动决定。之所以要能显式指定：
// HTTP Digest 的第一次 401 是**协议规定的握手**（客户端先裸发一次、服务端回挑战、
// 客户端带着 nonce 重发），不是故障。把它记成 WARN 会让日志看起来全是错误，
// 实际每条后面 17 毫秒就跟着一个 200。
void respondAndLog(VirtualCamera *camera, const HttpRequest &request,
                   const HttpExchangePtr &exchange, const HttpResponse &response,
                   const QString &summary, bool ok, const QString &quirkKey = QString(),
                   std::optional<LogLevel> level = std::nullopt)
{
    if (LogBus *log = camera->logBus()) {
        LogRecord rec;
        rec.category = QString::fromLatin1(logcat::Http);
        rec.cameraId = camera->id();
        rec.peer = request.peerString();
        rec.summary = summary;
        rec.ok = ok;
        rec.level = level.value_or(ok ? LogLevel::Info : LogLevel::Warning);
        rec.quirkKey = quirkKey;
        log->post(rec);
    }
    exchange->respond(response);
}

// B4：四种坏响应。客户端的成功判据是「空体直接拒 / 有 JPEG 魔数就信 /
// 无魔数则 content-type 以 image/ 开头且 body ≥ 64 字节」，四种正好各打一个点。
bool respondWithBadSnapshot(VirtualCamera *camera, const QString &profileToken,
                            const HttpRequest &request, const HttpExchangePtr &exchange)
{
    const Quirks &quirks = camera->quirks();
    if (!quirks.isEnabled(QuirkId::SnapshotEmptyBody))
        return false;

    const QString mode = quirks.choice(QuirkId::SnapshotEmptyBody, QStringLiteral("empty"));
    const QString quirkKey = QuirkRegistry::def(QuirkId::SnapshotEmptyBody).key;
    HttpResponse response;
    QString summary;

    if (mode == QLatin1String("short_text")) {
        // 20 来字节的错误文本，Content-Type 却还是 image/jpeg。
        response = HttpResponse::jpeg(QByteArray("ERROR: no image data"));
        summary = QStringLiteral("Snapshot returned a short text body (B4 short_text)");
    } else if (mode == QLatin1String("html_login")) {
        response.status = 200;
        response.setBody(QByteArray(kLoginPage), "text/html; charset=utf-8");
        summary = QStringLiteral("Snapshot returned an HTML login page (B4 html_login)");
    } else if (mode == QLatin1String("wrong_content_type")) {
        // 图是真的，只是 Content-Type 写错。有魔数的客户端应该照样能认。
        response.status = 200;
        response.setBody(snapshot::renderFor(camera, profileToken),
                         "application/octet-stream");
        summary = QStringLiteral("Snapshot sent the wrong Content-Type (B4 wrong_content_type)");
    } else {
        response = HttpResponse::jpeg(QByteArray());
        summary = QStringLiteral("Snapshot returned 200 with an empty body (B4 empty)");
    }

    respondAndLog(camera, request, exchange, response, summary, false, quirkKey);
    return true;
}

void handleSnapshot(VirtualCamera *camera, const QSharedPointer<HttpAuth> &auth,
                    const HttpRequest &request, const HttpExchangePtr &exchange)
{
    const Quirks &quirks = camera->quirks();
    const CameraModel &model = camera->model();

    // profile：客户端用的是 GetSnapshotUri 给的那个 token；没带就取第一个。
    // 参照客户端只对 profiles[0] 调 GetSnapshotUri，所以这个兜底几乎总是对的。
    QString profileToken = request.query.queryItemValue(QStringLiteral("token"));
    if (profileToken.isEmpty() && !model.profiles.isEmpty())
        profileToken = model.profiles.first().token;
    if (!model.profileByToken(profileToken)) {
        respondAndLog(camera, request, exchange,
                      HttpResponse::text(404, QStringLiteral("Unknown profile token")),
                      QStringLiteral("Unknown snapshot profile: %1").arg(profileToken), false);
        return;
    }

    // B6：URI 定期轮换失效。VirtualCamera::snapshotUri() 把当前时间窗写进 nonce，
    // 过窗的旧 URI 直接 404 —— 客户端要连续 3 次失败才会重探。
    if (quirks.isEnabled(QuirkId::SnapshotUriRotates)) {
        const int window = qMax(1, quirks.paramInt(QuirkId::SnapshotUriRotates,
                                                   QStringLiteral("seconds")));
        const qint64 current = QDateTime::currentSecsSinceEpoch() / window;
        const QString nonce = request.query.queryItemValue(QStringLiteral("nonce"));
        if (nonce.isEmpty() || nonce.toLongLong() != current) {
            respondAndLog(camera, request, exchange,
                          HttpResponse::text(404, QStringLiteral("Snapshot URI expired")),
                          QStringLiteral("Snapshot URI has expired (B6)"), false,
                          QuirkRegistry::def(QuirkId::SnapshotUriRotates).key);
            return;
        }
    }

    // B5：四档鉴权。quirk 关着时走 Digest —— 海康、大华的快照端点默认都要鉴权，
    // 参照客户端也正是按 Digest → Basic → 无 的顺序试、只在 401 时才换下一种。
    // 「不鉴权」是四档里的异常档，要注入才有，不该是默认。
    const QString authMode = quirks.choice(QuirkId::SnapshotAuthMode,
                                           QStringLiteral("digest"));
    const QString authQuirkKey = QuirkRegistry::def(QuirkId::SnapshotAuthMode).key;

    if (authMode == QLatin1String("token")) {
        const QString provided = request.query.queryItemValue(QStringLiteral("auth"));
        if (!verifyAccessToken(camera, profileToken, provided)) {
            // 故意不发 WWW-Authenticate：这台「相机」根本不走 HTTP 鉴权，
            // 客户端把 Digest → Basic → 无 全试一遍也过不去，只能重新 GetSnapshotUri。
            respondAndLog(camera, request, exchange,
                          HttpResponse::text(401, QStringLiteral("Invalid snapshot token")),
                          QStringLiteral("Invalid snapshot token (B5 token)"), false, authQuirkKey);
            return;
        }
    } else if (authMode == QLatin1String("basic") || authMode == QLatin1String("digest")) {
        const AuthScheme want = authMode == QLatin1String("basic") ? AuthScheme::Basic
                                                                   : AuthScheme::Digest;
        const QByteArray authorization = request.header("authorization");
        AuthResult result;
        if (!authorization.isEmpty()) {
            result = auth->verify(authorization, request.method, request.rawTarget.toUtf8(),
                                  model, quirks);
        }
        if (!result.authenticated || result.scheme != want) {
            HttpResponse response = HttpResponse::text(401, QStringLiteral("Unauthorized"));
            response.setHeader("WWW-Authenticate",
                               challengeHeader(*auth, quirks, want, result.stale));

            // 把客户端到底发了什么写进日志。只说「要求 Digest」帮不上忙 ——
            // 排查快照 401 时真正要知道的是：它带没带 Authorization、带的是哪种方案、
            // 校验卡在哪一步。少了这些就只能靠抓包。
            QString detail;
            if (authorization.isEmpty()) {
                detail = QStringLiteral("客户端没带 Authorization 头");
            } else {
                const QByteArray scheme = authorization.left(authorization.indexOf(' '));
                detail = QStringLiteral("客户端用的是 %1").arg(QString::fromLatin1(scheme));
                if (!result.authenticated && !result.failureReason.isEmpty())
                    detail += QStringLiteral("，校验失败：%1").arg(result.failureReason);
                else if (result.scheme != want)
                    detail += QStringLiteral("，但这台相机配的是 %1").arg(authMode);
            }
            // 没带 Authorization 的那次 401 是握手的第一步，不是错误；
            // 带了却过不去（密码错、方案不对、nonce 重放）才值得 WARN。
            const bool handshake = authorization.isEmpty();
            respondAndLog(camera, request, exchange, response,
                          handshake
                              ? QStringLiteral("Snapshot → 401 challenge (%1) — %2")
                                    .arg(authMode, detail)
                              : QStringLiteral("Snapshot → 401: %1 authentication required (B5) — %2")
                                    .arg(authMode, detail),
                          false, authQuirkKey,
                          handshake ? LogLevel::Debug : LogLevel::Warning);
            return;
        }
    }

    if (respondWithBadSnapshot(camera, profileToken, request, exchange))
        return;

    const QByteArray jpeg = snapshot::renderFor(camera, profileToken);
    respondAndLog(camera, request, exchange, HttpResponse::jpeg(jpeg),
                  QStringLiteral("Snapshot %1 (%2 bytes)").arg(profileToken).arg(jpeg.size()), true);
}

} // namespace

namespace services {

QString snapshotAccessToken(const VirtualCamera *camera, const QString &profileToken)
{
    if (!camera)
        return QString();
    return tokenForWindow(camera, profileToken,
                          QDateTime::currentSecsSinceEpoch() / kTokenWindowSeconds);
}

void registerSnapshotRoutes(VirtualCamera *camera, HttpServer *server)
{
    if (!camera || !server)
        return;

    // Digest 的 nonce 要跨请求记住，这份 HttpAuth 得跟路由活得一样久。
    // 用 shared pointer 让 lambda 持有，路由被清掉时自然释放。
    auto auth = QSharedPointer<HttpAuth>::create(QStringLiteral("onvifsim"));

    // 路径按 persona（海康 /ISAPI/... 风格之类），query 部分由 snapshotUri() 拼，
    // 路由只认路径。
    QString path = camera->persona().snapshotPath;
    const int question = path.indexOf(QLatin1Char('?'));
    if (question >= 0)
        path.truncate(question);
    if (path.isEmpty())
        path = QStringLiteral("/onvif/snapshot");

    server->addRoute("GET", path,
                     [camera, auth](const HttpRequest &request, const HttpExchangePtr &exchange) {
                         handleSnapshot(camera, auth, request, exchange);
                     });
}

} // namespace services
} // namespace onvifsim
