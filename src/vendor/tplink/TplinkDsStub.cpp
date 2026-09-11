#include "vendor/tplink/TplinkDsStub.h"

#include "core/CameraModel.h"
#include "core/VirtualCamera.h"
#include "imaging/ImagingState.h"
#include "net/HttpServer.h"
#include "vendor/VendorFactories.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRandomGenerator>

namespace onvifsim {
namespace {

// stok 的租期。真机不公布确切值，取 30 分钟这个量级足够让客户端的刷新逻辑走到。
constexpr int kStokLeaseSeconds = 1800;

// 灯光模块名。真机不同型号叫法不一样（led / light / floodlight），
// 三个都收下，映射到同一份 ImagingState 的白光灯。
bool isLightModule(const QString &name)
{
    return name == QLatin1String("light") || name == QLatin1String("led")
        || name == QLatin1String("floodlight");
}

// 私有 JSON 里的布尔一律是 "on" / "off" 字符串，不是 true / false。
QString onOff(bool on)
{
    return on ? QStringLiteral("on") : QStringLiteral("off");
}

bool parseOnOff(const QJsonValue &value, bool fallback)
{
    if (value.isBool())
        return value.toBool();
    if (value.isDouble())
        return value.toInt() != 0;
    const QString s = value.toString();
    if (s.compare(QLatin1String("on"), Qt::CaseInsensitive) == 0
        || s.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0 || s == QLatin1String("1"))
        return true;
    if (s.compare(QLatin1String("off"), Qt::CaseInsensitive) == 0
        || s.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0 || s == QLatin1String("0"))
        return false;
    return fallback;
}

// 和 parseOnOff 同样的道理：真机的 /ds 接口既收 `"brightness": "80"` 也收
// `"brightness": 80`。只认字符串的话，QJsonValue::toString() 对数字返回空串、
// 空串 .toInt() 得到 0 —— 亮度归零、灯灭、error_code 还回 0，
// 客户端拿到的是「一次成功的反向操作」。
bool parseInt(const QJsonValue &value, int *out)
{
    if (value.isDouble()) {
        *out = value.toInt();
        return true;
    }
    if (value.isString()) {
        bool ok = false;
        const int parsed = value.toString().toInt(&ok);
        if (ok) {
            *out = parsed;
            return true;
        }
    }
    return false;
}

} // namespace

TplinkDsStub::TplinkDsStub(VirtualCamera *camera, QObject *parent)
    : VendorApiStub(camera, parent)
{
}

TplinkDsStub::~TplinkDsStub() = default;

VendorApi TplinkDsStub::kind() const
{
    return VendorApi::TplinkDs;
}

int TplinkDsStub::errInvalidStok()  { return -40401; }
int TplinkDsStub::errUnsupported()  { return -40210; }
int TplinkDsStub::errBadParam()     { return -40209; }

QByteArray TplinkDsStub::encode(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QString TplinkDsStub::stokFromPath(const QString &path)
{
    // /stok=abcdef/ds → abcdef
    const int start = path.indexOf(QLatin1String("stok="));
    if (start < 0)
        return QString();
    const int from = start + 5;
    int end = path.indexOf(QLatin1Char('/'), from);
    if (end < 0)
        end = path.size();
    return path.mid(from, end - from);
}

QString TplinkDsStub::currentStok() const
{
    return m_stok;
}

void TplinkDsStub::expireStok()
{
    m_stokExpiry = QDateTime::currentDateTimeUtc().addSecs(-1);
}

QString TplinkDsStub::issueStok()
{
    const QByteArray seed = QByteArray::number(QDateTime::currentMSecsSinceEpoch())
        + ':' + QByteArray::number(QRandomGenerator::global()->generate64(), 16);
    m_stok = QString::fromLatin1(
        QCryptographicHash::hash(seed, QCryptographicHash::Md5).toHex());
    m_stokExpiry = QDateTime::currentDateTimeUtc().addSecs(kStokLeaseSeconds);
    return m_stok;
}

bool TplinkDsStub::stokAccepted(const QString &stok) const
{
    if (m_stok.isEmpty() || stok.isEmpty() || stok != m_stok)
        return false;
    return m_stokExpiry.isValid() && QDateTime::currentDateTimeUtc() < m_stokExpiry;
}

void TplinkDsStub::registerRoutes(HttpServer *server)
{
    if (!server || m_routesRegistered)
        return;
    m_routesRegistered = true;

    const auto route = [this](const HttpRequest &request, const HttpExchangePtr &exchange) {
        exchange->respond(handle(request));
    };
    // 登录在根路径，业务在 /stok=<t>/ds —— stok 在路径里，只能用前缀路由。
    server->addRoute("POST", QStringLiteral("/"), route);
    server->addPrefixRoute("POST", QStringLiteral("/stok="), route);
}

HttpResponse TplinkDsStub::handle(const HttpRequest &request)
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(request.body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        QJsonObject out;
        out.insert(QStringLiteral("error_code"), errBadParam());
        return HttpResponse::json(encode(out), 200);
    }
    const QJsonObject body = doc.object();

    // 根路径只做登录；带 stok 的路径才是业务口。
    if (!request.path.contains(QLatin1String("stok=")))
        return HttpResponse::json(encode(handleLogin(body)));

    if (!stokAccepted(stokFromPath(request.path))) {
        QJsonObject out;
        out.insert(QStringLiteral("error_code"), errInvalidStok());
        // 真机在 stok 失效时 HTTP 状态仍是 200，错误只在 error_code 上，
        // 客户端据此重登一次再重试 —— 状态码写成 401 会让它直接放弃。
        return HttpResponse::json(encode(out), 200);
    }
    return HttpResponse::json(encode(handleDs(body)));
}

QJsonObject TplinkDsStub::handleLogin(const QJsonObject &request)
{
    // 两种写法都收：{"method":"login","params":{...}} 与 {"method":"do","login":{...}}。
    const QString method = request.value(QStringLiteral("method")).toString();
    QJsonObject login = request.value(QStringLiteral("params")).toObject();
    if (login.isEmpty())
        login = request.value(QStringLiteral("login")).toObject();

    if (method != QLatin1String("login") && method != QLatin1String("do")) {
        QJsonObject out;
        out.insert(QStringLiteral("error_code"), errUnsupported());
        return out;
    }

    const QString user = login.value(QStringLiteral("username")).toString();
    const QString password = login.value(QStringLiteral("password")).toString();
    // camera 为空是单测路径：没有用户表就一律放行。
    // 真机的口令是 MD5 大写十六进制，明文与摘要两种都收，免得客户端两种实现都要适配。
    bool ok = !m_camera;
    if (m_camera) {
        const CameraModel &model = m_camera->model();
        ok = model.checkPassword(user, password);
        if (!ok) {
            const User *entry = model.findUser(user);
            if (entry) {
                const QByteArray md5 = QCryptographicHash::hash(entry->password.toUtf8(),
                                                                QCryptographicHash::Md5).toHex();
                ok = password.compare(QString::fromLatin1(md5), Qt::CaseInsensitive) == 0;
            }
        }
    }
    if (!ok) {
        QJsonObject out;
        out.insert(QStringLiteral("error_code"), errInvalidStok());
        return out;
    }

    const QString stok = issueStok();
    QJsonObject result;
    result.insert(QStringLiteral("stok"), stok);
    result.insert(QStringLiteral("user_group"), QStringLiteral("root"));
    QJsonObject out;
    out.insert(QStringLiteral("error_code"), 0);
    out.insert(QStringLiteral("stok"), stok);   // 顶层也放一份，两种解析都能拿到
    out.insert(QStringLiteral("result"), result);
    return out;
}

QJsonObject TplinkDsStub::handleDs(const QJsonObject &request)
{
    const QString method = request.value(QStringLiteral("method")).toString();

    // multipleRequest：一次带一串子请求，逐条走同一套分派再把结果拼回去。
    if (method == QLatin1String("multipleRequest")) {
        const QJsonArray requests = request.value(QStringLiteral("params")).toObject()
                                        .value(QStringLiteral("requests")).toArray();
        QJsonArray responses;
        for (const QJsonValue &one : requests) {
            QJsonObject sub = handleDs(one.toObject());
            sub.insert(QStringLiteral("method"),
                       one.toObject().value(QStringLiteral("method")));
            responses.append(sub);
        }
        QJsonObject result;
        result.insert(QStringLiteral("responses"), responses);
        QJsonObject out;
        out.insert(QStringLiteral("error_code"), 0);
        out.insert(QStringLiteral("result"), result);
        return out;
    }

    // 找出请求里带的那个模块名（除 method / params 之外的第一个对象字段）。
    QString moduleName;
    QJsonObject module;
    for (auto it = request.constBegin(); it != request.constEnd(); ++it) {
        if (it.key() == QLatin1String("method") || it.key() == QLatin1String("params"))
            continue;
        if (!it.value().isObject())
            continue;
        moduleName = it.key();
        module = it.value().toObject();
        break;
    }

    QJsonObject out;
    if (!isLightModule(moduleName)) {
        // 只做灯光：别的模块一律回不支持，免得客户端以为这台机器什么都能干。
        out.insert(QStringLiteral("error_code"), errUnsupported());
        return out;
    }

    if (method == QLatin1String("get")) {
        out.insert(QStringLiteral("error_code"), 0);
        out.insert(moduleName, readLight());
        return out;
    }
    if (method == QLatin1String("set") || method == QLatin1String("do")) {
        out = writeLight(module);
        return out;
    }

    out.insert(QStringLiteral("error_code"), errUnsupported());
    return out;
}

QJsonObject TplinkDsStub::readLight() const
{
    ImagingState *imaging = m_camera ? m_camera->imaging() : nullptr;
    const bool on = imaging && imaging->settings().whiteLightOn;
    const int brightness = imaging ? imaging->settings().whiteLightBrightness : 100;

    QJsonObject config;
    config.insert(QStringLiteral("enabled"), onOff(on));
    // 数值也是字符串：TP-Link 的私有 JSON 里所有标量都是字符串。
    config.insert(QStringLiteral("brightness"), QString::number(brightness));
    config.insert(QStringLiteral("mode"), on ? QStringLiteral("manual")
                                             : QStringLiteral("auto"));
    QJsonObject light;
    light.insert(QStringLiteral("config"), config);
    return light;
}

QJsonObject TplinkDsStub::writeLight(const QJsonObject &module)
{
    ImagingState *imaging = m_camera ? m_camera->imaging() : nullptr;
    bool on = imaging && imaging->settings().whiteLightOn;
    int brightness = -1;
    bool touched = false;

    // 写法有两种：config.enabled 与 switch.status，真机按型号各用一种。
    const QJsonObject config = module.value(QStringLiteral("config")).toObject();
    if (config.contains(QStringLiteral("enabled"))) {
        on = parseOnOff(config.value(QStringLiteral("enabled")), on);
        touched = true;
    }
    if (config.contains(QStringLiteral("brightness"))) {
        int parsed = brightness;
        if (parseInt(config.value(QStringLiteral("brightness")), &parsed)) {
            brightness = qBound(0, parsed, 100);
            if (brightness == 0 && !config.contains(QStringLiteral("enabled")))
                on = false;
            touched = true;
        }
    }
    const QJsonObject sw = module.value(QStringLiteral("switch")).toObject();
    if (sw.contains(QStringLiteral("status"))) {
        on = parseOnOff(sw.value(QStringLiteral("status")), on);
        touched = true;
    }

    QJsonObject out;
    if (!touched) {
        out.insert(QStringLiteral("error_code"), errBadParam());
        return out;
    }
    if (imaging)
        imaging->setWhiteLight(on, brightness);
    out.insert(QStringLiteral("error_code"), 0);
    return out;
}

namespace vendorapi {

VendorApiStub *createTplinkDs(VirtualCamera *camera)
{
    return new TplinkDsStub(camera, camera);
}

} // namespace vendorapi
} // namespace onvifsim
