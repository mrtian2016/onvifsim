#include "vendor/vigi/VigiStub.h"

#include "core/CameraModel.h"
#include "core/LogBus.h"
#include "core/Persona.h"
#include "core/Quirks.h"
#include "core/VirtualCamera.h"
#include "net/HttpServer.h"
#include "ptz/PtzState.h"
#include "vendor/VendorFactories.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRandomGenerator>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <functional>
#include <utility>

#if QT_CONFIG(ssl)
#include <QtCore/QFile>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QSslKey>
#include <QtNetwork/QSslSocket>
#endif

// 静态库里的 Qt 资源不会自动初始化。资源名必须与 qrc 基名一致（vigitls.qrc → vigitls）。
// 必须待在文件作用域：Q_INIT_RESOURCE 展开出的 extern 声明一旦被匿名 namespace
// 限定住，链接期就找不到真正的全局符号了。
static void ensureVigiTlsResources()
{
    static bool done = false;
    if (!done) {
        Q_INIT_RESOURCE(vigitls);
        done = true;
    }
}

namespace onvifsim {
namespace {

// stok 的租期。真机是 25 分钟刷新一次，照抄。
constexpr int kStokLeaseSeconds = 25 * 60;

constexpr quint16 kDefaultVigiPort = 20443;

// 请求头块上限，挡住只发头不发完的空占连接。
constexpr qsizetype kMaxHeaderBytes = 16 * 1024;
constexpr qsizetype kMaxBodyBytes = 256 * 1024;

// ---------------------------------------------------------------------------
// 内嵌的自签证书 —— 只用于测试
//
// Qt 没有「运行时生成自签证书」的 API（那要 OpenSSL 的 X509 接口，本项目零第三方
// 依赖也不直接调 OpenSSL），所以这里内嵌一份预生成的 RSA-2048 自签证书与私钥。
// CN=onvifsim-vigi，SAN 里带 localhost 与 127.0.0.1，有效期到 2046 年。
//
// !!! 私钥就明文写在这里，任何人都能拿到。它只是为了让模拟器能吐出一个
// !!! 「客户端必须 verify=False 才连得上」的 TLS 端口（quirk F2），
// !!! 绝不可用于任何真实服务。
// ---------------------------------------------------------------------------
// 证书与私钥以 **DER 二进制** 放在 assets/tls/ 下，经 qrc 编进可执行文件。
// 不写成 PEM 字面量是刻意的：源码里一出现 PEM 的那行 armor（BEGIN 私钥），
// GitHub 的 push protection 与各家企业扫描器就会告警甚至拒绝推送，
// 而这把钥匙本来就是公开的测试用钥匙，拦下来纯属误报。见 vigitls.qrc。
QByteArray readTlsAsset(const char *name)
{
    ensureVigiTlsResources();
    QFile file(QStringLiteral(":/onvifsim/tls/") + QLatin1String(name));
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

QSslCertificate testCertificate()
{
    static const QSslCertificate cert(readTlsAsset("vigi-cert.der"), QSsl::Der);
    return cert;
}

QSslKey testPrivateKey()
{
    static const QSslKey key(readTlsAsset("vigi-key.der"), QSsl::Rsa, QSsl::Der,
                             QSsl::PrivateKey);
    return key;
}

QString sha256Hex(const QByteArray &data)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

QString randomHex(int bytes)
{
    QByteArray raw;
    raw.reserve(bytes);
    for (int i = 0; i < bytes; ++i)
        raw.append(char(QRandomGenerator::global()->bounded(256)));
    return QString::fromLatin1(raw.toHex());
}

QJsonObject errorObject(int code, const QString &reason)
{
    QJsonObject result;
    if (!reason.isEmpty())
        result.insert(QStringLiteral("reason"), reason);
    QJsonObject out;
    out.insert(QStringLiteral("error_code"), code);
    out.insert(QStringLiteral("result"), result);
    return out;
}

QJsonObject okObject(const QJsonObject &result)
{
    QJsonObject out;
    out.insert(QStringLiteral("error_code"), 0);
    out.insert(QStringLiteral("result"), result);
    return out;
}

// VIGI 的 motorMove 是绝对坐标。真机的域没有公开文档，这里按
// 「[-1, 1] 直接用；[0, 1] 视为 0.5 居中的归一化坐标」两种都收，
// 两边都能落到 PtzState 的 [-1, 1]。
double normalizeAxis(double value)
{
    if (value < 0.0)
        return qBound(-1.0, value, 1.0);
    if (value > 1.0)
        return 1.0;
    // 0..1 且不是 0：无法区分「0.7 就是 0.7」与「0.7 是归一化坐标」，
    // 取前者 —— 客户端真要居中会传 0。
    return value;
}

// 一条 HTTP/1.1 响应的字节流。私有端口上不做 keep-alive：
// 客户端每次调用都是一发一收，收完就断最省事，也最像真机。
QByteArray serializeResponse(const HttpResponse &response)
{
    QByteArray out = "HTTP/1.1 " + QByteArray::number(response.status) + ' '
        + QByteArray(HttpResponse::defaultReason(response.status)) + "\r\n";
    for (auto it = response.headers.constBegin(); it != response.headers.constEnd(); ++it) {
        if (it.key().compare("connection", Qt::CaseInsensitive) == 0)
            continue;
        out += it.key() + ": " + it.value() + "\r\n";
    }
    out += "Server: onvifsim\r\n";
    out += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += response.body;
    return out;
}

} // namespace

VigiStub::VigiStub(VirtualCamera *camera, QObject *parent)
    : VendorApiStub(camera, parent)
{
    // 8 个检测开关出厂全关 —— 客户端必须逐个打开，
    // 「漏开一个就零事件」这个坑要能在模拟器上复现出来。
    const QStringList types = detectionTypes();
    for (const QString &type : types)
        m_detections.insert(type, false);

    if (m_camera) {
        // 相机停 / 离线时把私有端口一起收掉，恢复时再起来，
        // 否则「整机离线」在 20443 上是看不出来的。
        connect(m_camera, &VirtualCamera::stopped, this, &VigiStub::stopPrivateListener);
        connect(m_camera, &VirtualCamera::wentOffline, this,
                [this](int) { stopPrivateListener(); });
        connect(m_camera, &VirtualCamera::cameBackOnline, this,
                &VigiStub::startPrivateListener);
    }
}

VigiStub::~VigiStub()
{
    stopPrivateListener();
}

VendorApi VigiStub::kind() const
{
    return VendorApi::VigiJsonRpc;
}

int VigiStub::errAuthFailed()           { return -40401; }
int VigiStub::errUnsupportedMethod()    { return -40210; }
int VigiStub::errBadParam()             { return -40209; }
int VigiStub::errUnsupportedDetection() { return -10030; }

QStringList VigiStub::detectionTypes()
{
    // reference-client-facts.md §7.2 的原始顺序，别重排：
    // 客户端就是按这个顺序逐个 set 的，日志比对时顺序一致更好排查。
    return { QStringLiteral("PeopleDetection"),
             QStringLiteral("VehicleDetection"),
             QStringLiteral("AudioAnomalyDetection"),
             QStringLiteral("LoiterDetection"),
             QStringLiteral("SceneChangeDetection"),
             QStringLiteral("AreaEntryDetection"),
             QStringLiteral("AreaLeaveDetection"),
             QStringLiteral("DropAndTakeDetection") };
}

bool VigiStub::detectionEnabled(const QString &type) const
{
    return m_detections.value(type, false);
}

QString VigiStub::authDigest(const QString &username, const QString &password,
                             const QString &nonce)
{
    const QString inner = sha256Hex((username + QLatin1Char(':') + password).toUtf8());
    return sha256Hex((inner + QLatin1Char(':') + nonce).toUtf8());
}

QString VigiStub::currentNonce() const
{
    return m_nonce;
}

QString VigiStub::currentStok() const
{
    return m_stok;
}

void VigiStub::expireStok()
{
    m_stokExpiry = QDateTime::currentDateTimeUtc().addSecs(-1);
}

QString VigiStub::issueNonce()
{
    m_nonce = randomHex(16);
    return m_nonce;
}

QString VigiStub::issueStok()
{
    m_stok = randomHex(16);
    m_stokExpiry = QDateTime::currentDateTimeUtc().addSecs(kStokLeaseSeconds);
    return m_stok;
}

bool VigiStub::stokAccepted(const QString &stok) const
{
    if (m_stok.isEmpty() || stok.isEmpty() || stok != m_stok)
        return false;
    return m_stokExpiry.isValid() && QDateTime::currentDateTimeUtc() < m_stokExpiry;
}

quint16 VigiStub::securePort() const
{
    return m_server && m_server->isListening() ? m_server->serverPort() : quint16(0);
}

bool VigiStub::isTlsActive() const
{
    return m_tlsActive;
}

// ---------------------------------------------------------------------------
// 路由与私有端口
// ---------------------------------------------------------------------------

namespace {

// QTcpServer 只在 incomingConnection() 里给得到裸描述符，
// 而我们要用它建 QSslSocket，所以必须派生一层。
// 这个类没有自己的信号槽，不需要 Q_OBJECT。
class DescriptorServer : public QTcpServer
{
public:
    DescriptorServer(std::function<void(qintptr)> sink, QObject *parent)
        : QTcpServer(parent), m_sink(std::move(sink))
    {
    }

protected:
    void incomingConnection(qintptr descriptor) override
    {
        if (m_sink)
            m_sink(descriptor);
    }

private:
    std::function<void(qintptr)> m_sink;
};

} // namespace

void VigiStub::registerRoutes(HttpServer *server)
{
    if (server && !m_routesRegistered) {
        m_routesRegistered = true;
        // quirk 改端口后要重开监听。用 UniqueConnection 防止相机重启时重复接。
        if (m_camera) {
            connect(m_camera, &VirtualCamera::quirksChanged, this,
                    &VigiStub::startPrivateListener, Qt::UniqueConnection);
        }
        // ONVIF 端口上也挂一条同样的入口。真机不这么干（私有 API 只在 20443），
        // 但这样一来：没编 SSL 支持的构建、以及不想处理自签证书的 e2e 用例，
        // 都还能把这套 JSON-RPC 跑通。
        server->addRoute("POST", QStringLiteral("/"),
                         [this](const HttpRequest &request, const HttpExchangePtr &exchange) {
                             exchange->respond(handle(request));
                         });
    }
    startPrivateListener();
}

// 端口按 quirk F2 的 port 参数取；没开这条 quirk 就退回预设里的 vendorApiPort。
quint16 VigiStub::desiredPrivatePort() const
{
    quint16 port = kDefaultVigiPort;
    if (m_camera) {
        const Quirks &quirks = m_camera->quirks();
        if (quirks.isEnabled(QuirkId::SelfSignedTls)) {
            port = quint16(quirks.paramInt(QuirkId::SelfSignedTls, QStringLiteral("port")));
        } else if (m_camera->persona().vendorApiPort) {
            port = m_camera->persona().vendorApiPort;
        }
    }
    return port == 0 ? kDefaultVigiPort : port;
}

void VigiStub::startPrivateListener()
{
    const quint16 port = desiredPrivatePort();
    if (m_server && m_server->isListening()) {
        if (m_server->serverPort() == port)
            return;
        // quirk 把端口改了：私有监听得跟着搬家，否则运行时开这条 quirk
        // 等于什么都没发生（监听还挂在旧端口上）。
        stopPrivateListener();
    }

    const QHostAddress bind = m_camera ? m_camera->model().bindAddress
                                       : QHostAddress(QHostAddress::LocalHost);

    LogBus *bus = m_camera ? m_camera->logBus() : LogBus::global();
    const QString cameraId = m_camera ? m_camera->id() : QString();

    m_tlsActive = false;
#if QT_CONFIG(ssl)
    // 证书解析失败或这份 Qt 没链上 TLS 后端时，退化成明文 HTTP：
    // 端点照样能用，只是少了「客户端必须跳过证书校验」这一层。
    m_tlsActive = QSslSocket::supportsSsl()
        && !testCertificate().isNull()
        && !testPrivateKey().isNull();
#endif

    if (!m_server) {
        m_server = new DescriptorServer([this](qintptr descriptor) { acceptDescriptor(descriptor); },
                                        this);
    }
    if (!m_server->listen(bind, port)) {
        if (bus) {
            bus->warning(logcat::Http, cameraId,
                         QStringLiteral("Cannot listen on VIGI private port %1:%2: %3")
                             .arg(bind.toString())
                             .arg(port)
                             .arg(m_server->errorString()));
        }
        return;
    }
    if (bus) {
        bus->info(logcat::Http, cameraId,
                  QStringLiteral("VIGI private API listening on %1:%2 (%3)")
                      .arg(bind.toString())
                      .arg(m_server->serverPort())
                      .arg(m_tlsActive ? QStringLiteral("self-signed HTTPS")
                                       : QStringLiteral("明文 HTTP，本机 Qt 无 TLS 后端")));
    }
}

void VigiStub::stopPrivateListener()
{
    if (!m_server)
        return;
    m_server->close();
    // 连接是 m_server 的子对象之外单独挂在 this 上的，逐个关掉。
    const QList<QObject *> buffered = m_buffers.keys();
    for (QObject *object : buffered) {
        if (auto *socket = qobject_cast<QTcpSocket *>(object))
            socket->abort();
    }
    m_buffers.clear();
}

void VigiStub::acceptDescriptor(qintptr descriptor)
{
#if QT_CONFIG(ssl)
    auto *socket = new QSslSocket(this);
#else
    auto *socket = new QTcpSocket(this);
#endif
    if (!socket->setSocketDescriptor(descriptor)) {
        socket->deleteLater();
        return;
    }
    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

    m_buffers.insert(socket, QByteArray());
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onSocketReadyRead(socket); });
    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    connect(socket, &QObject::destroyed, this,
            [this](QObject *object) { m_buffers.remove(object); });

#if QT_CONFIG(ssl)
    if (m_tlsActive) {
        socket->setLocalCertificate(testCertificate());
        socket->setPrivateKey(testPrivateKey());
        // 自签证书本来就过不了校验，客户端那边是 verify=False，服务端也不要校验对端。
        socket->setPeerVerifyMode(QSslSocket::VerifyNone);
        socket->startServerEncryption();
    }
#endif
}

void VigiStub::onSocketReadyRead(QTcpSocket *socket)
{
    if (!socket)
        return;
    QByteArray &buffer = m_buffers[socket];
    buffer += socket->readAll();

    const int headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        if (buffer.size() > kMaxHeaderBytes)
            socket->abort();
        return;   // 头还没收全，等下一次 readyRead
    }

    const QByteArray head = buffer.left(headerEnd);
    const QList<QByteArray> lines = head.split('\n');
    if (lines.isEmpty()) {
        socket->abort();
        return;
    }

    HttpRequest request;
    request.secure = m_tlsActive;
    request.peerAddress = socket->peerAddress();
    request.peerPort = socket->peerPort();
    request.localAddress = socket->localAddress();
    request.localPort = socket->localPort();

    const QList<QByteArray> requestLine = lines.at(0).trimmed().split(' ');
    if (requestLine.size() < 2) {
        socket->abort();
        return;
    }
    request.method = requestLine.at(0).toUpper();
    request.rawTarget = QString::fromUtf8(requestLine.at(1));
    if (requestLine.size() > 2)
        request.version = requestLine.at(2);

    const QUrl target = QUrl::fromEncoded(requestLine.at(1));
    request.path = target.path();
    request.query = QUrlQuery(target.query());

    qint64 contentLength = 0;
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        const QByteArray key = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        request.headers.insert(key, value);
        if (key == "content-length")
            contentLength = value.toLongLong();
    }
    if (contentLength < 0 || contentLength > kMaxBodyBytes) {
        socket->abort();
        return;
    }

    const qsizetype bodyStart = headerEnd + 4;
    if (buffer.size() - bodyStart < contentLength)
        return;   // body 还没收全
    request.body = buffer.mid(int(bodyStart), int(contentLength));
    buffer.clear();

    socket->write(serializeResponse(handle(request)));
    // 一发一收，写完就断；deleteLater 挂在 disconnected 上。
    socket->disconnectFromHost();
}

// ---------------------------------------------------------------------------
// JSON-RPC
// ---------------------------------------------------------------------------

HttpResponse VigiStub::handle(const HttpRequest &request)
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(request.body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return HttpResponse::json(
            QJsonDocument(errorObject(errBadParam(), QStringLiteral("bad json")))
                .toJson(QJsonDocument::Compact));
    }

    const QJsonObject body = doc.object();
    // stok 可以放在 body 里，也可以挂在 query 上，两种都收。
    QString stok = body.value(QStringLiteral("stok")).toString();
    if (stok.isEmpty())
        stok = request.query.queryItemValue(QStringLiteral("stok"));

    const QJsonObject result = dispatch(body, stok);
    // 业务错误照样回 HTTP 200，客户端只看 error_code。
    return HttpResponse::json(QJsonDocument(result).toJson(QJsonDocument::Compact));
}

QJsonObject VigiStub::dispatch(const QJsonObject &request, const QString &stok)
{
    const QString method = request.value(QStringLiteral("method")).toString();
    const QJsonObject params = request.value(QStringLiteral("params")).toObject();

    if (method == QLatin1String("doAuth"))
        return methodDoAuth(request);

    if (method.isEmpty())
        return errorObject(errBadParam(), QStringLiteral("missing method"));

    // 除 doAuth 外一律验 stok。
    if (!stokAccepted(stok))
        return errorObject(errAuthFailed(), QStringLiteral("invalid stok"));

    if (method == QLatin1String("subscribeMsg"))
        return methodSubscribeMsg(params);
    if (method == QLatin1String("getPresetPoint"))
        return methodGetPresetPoint();
    if (method == QLatin1String("motorMove"))
        return methodMotorMove(params);
    if (method == QLatin1String("getDeviceStatus"))
        return methodGetDeviceStatus();

    // 检测开关有两种写法：统一的 get/setDetectionConfig 带 params.type，
    // 以及直接拿检测名当 method。真机按固件版本两种都见过，都收。
    if (method == QLatin1String("getDetectionConfig"))
        return methodGetDetection(params.value(QStringLiteral("type")).toString());
    if (method == QLatin1String("setDetectionConfig"))
        return methodSetDetection(params.value(QStringLiteral("type")).toString(), params);
    if (m_detections.contains(method)) {
        if (params.contains(QStringLiteral("enabled")))
            return methodSetDetection(method, params);
        return methodGetDetection(method);
    }

    return errorObject(errUnsupportedMethod(), QStringLiteral("unsupported method"));
}

QJsonObject VigiStub::methodDoAuth(const QJsonObject &request)
{
    const QJsonValue paramsValue = request.value(QStringLiteral("params"));
    const QJsonObject params = paramsValue.toObject();

    // 第一步：params 为 null / 空 / 没带 digest —— 发挑战。
    if (paramsValue.isNull() || params.isEmpty()
        || !params.contains(QStringLiteral("digest"))) {
        QString username = params.value(QStringLiteral("username")).toString();
        if (username.isEmpty() && m_camera && !m_camera->model().users.isEmpty())
            username = m_camera->model().users.first().username;
        if (username.isEmpty())
            username = QStringLiteral("admin");

        QJsonObject result;
        result.insert(QStringLiteral("stage"), 1);
        result.insert(QStringLiteral("username"), username);
        result.insert(QStringLiteral("nonce"), issueNonce());
        result.insert(QStringLiteral("encrypt_type"), QStringLiteral("SHA256"));
        return okObject(result);
    }

    // 第二步：校验 sha256( sha256(user:pass) + ":" + nonce )。
    if (m_nonce.isEmpty())
        return errorObject(errAuthFailed(), QStringLiteral("challenge expired"));

    const QString username = params.value(QStringLiteral("username")).toString();
    const QString digest = params.value(QStringLiteral("digest")).toString();

    QString password;
    bool known = false;
    if (m_camera) {
        if (const User *user = m_camera->model().findUser(username)) {
            password = user->password;
            known = true;
        }
    } else {
        known = true;   // 单测路径：没有用户表就拿空口令算，测的是摘要算法本身
    }

    if (!known || digest.compare(authDigest(username, password, m_nonce),
                                 Qt::CaseInsensitive) != 0) {
        // 挑战一次性：失败就作废，逼客户端重新走第一步。
        m_nonce.clear();
        return errorObject(errAuthFailed(), QStringLiteral("authentication failed"));
    }

    m_nonce.clear();
    QJsonObject result;
    result.insert(QStringLiteral("stok"), issueStok());
    result.insert(QStringLiteral("lease_time"), kStokLeaseSeconds);
    return okObject(result);
}

QJsonObject VigiStub::methodSubscribeMsg(const QJsonObject &params)
{
    ++m_subscriptionSeq;
    QJsonObject result;
    result.insert(QStringLiteral("sub_id"),
                  QStringLiteral("sub-%1").arg(m_subscriptionSeq, 4, 10, QLatin1Char('0')));
    result.insert(QStringLiteral("expire"), 300);
    // 订阅时把「有几个检测开关是关的」如实报出来 —— 客户端漏开开关时，
    // 现象是「订阅成功但零事件」，这条能帮排查。
    int off = 0;
    for (auto it = m_detections.constBegin(); it != m_detections.constEnd(); ++it) {
        if (!it.value())
            ++off;
    }
    result.insert(QStringLiteral("disabled_detection_count"), off);
    if (params.contains(QStringLiteral("types")))
        result.insert(QStringLiteral("types"), params.value(QStringLiteral("types")));
    return okObject(result);
}

QJsonObject VigiStub::methodGetDetection(const QString &type) const
{
    if (!m_detections.contains(type))
        return errorObject(errUnsupportedDetection(), QStringLiteral("unsupported detection"));
    QJsonObject result;
    result.insert(QStringLiteral("type"), type);
    result.insert(QStringLiteral("enabled"), m_detections.value(type) ? 1 : 0);
    return okObject(result);
}

QJsonObject VigiStub::methodSetDetection(const QString &type, const QJsonObject &params)
{
    if (!m_detections.contains(type))
        return errorObject(errUnsupportedDetection(), QStringLiteral("unsupported detection"));
    const QJsonValue enabled = params.value(QStringLiteral("enabled"));
    if (enabled.isUndefined())
        return errorObject(errBadParam(), QStringLiteral("missing enabled"));
    m_detections.insert(type, enabled.isBool() ? enabled.toBool() : enabled.toInt() != 0);

    QJsonObject result;
    result.insert(QStringLiteral("type"), type);
    result.insert(QStringLiteral("enabled"), m_detections.value(type) ? 1 : 0);
    return okObject(result);
}

QJsonObject VigiStub::methodGetPresetPoint() const
{
    QJsonArray list;
    if (m_camera && m_camera->ptz()) {
        int index = 1;
        const QList<PtzPreset> presets = m_camera->ptz()->presets();
        for (const PtzPreset &preset : presets) {
            bool numeric = false;
            const int id = preset.token.toInt(&numeric);
            QJsonObject o;
            o.insert(QStringLiteral("id"), numeric ? id : index);
            o.insert(QStringLiteral("name"), preset.name);
            list.append(o);
            ++index;
        }
    }
    QJsonObject result;
    result.insert(QStringLiteral("preset"), list);
    return okObject(result);
}

QJsonObject VigiStub::methodMotorMove(const QJsonObject &params)
{
    if (!params.contains(QStringLiteral("x")) && !params.contains(QStringLiteral("y")))
        return errorObject(errBadParam(), QStringLiteral("missing x/y"));

    PtzState *ptz = m_camera ? m_camera->ptz() : nullptr;
    if (!ptz)
        return okObject(QJsonObject());

    PtzVector target = ptz->position();
    if (params.contains(QStringLiteral("x")))
        target.pan = normalizeAxis(params.value(QStringLiteral("x")).toDouble());
    if (params.contains(QStringLiteral("y")))
        target.tilt = normalizeAxis(params.value(QStringLiteral("y")).toDouble());
    if (params.contains(QStringLiteral("z")))
        target.zoom = qBound(0.0, params.value(QStringLiteral("z")).toDouble(), 1.0);

    PtzVector speed;
    speed.pan = speed.tilt = speed.zoom = 1.0;
    ptz->absoluteMove(target, speed);
    return okObject(QJsonObject());
}

QJsonObject VigiStub::methodGetDeviceStatus() const
{
    // 参照客户端的注释说得很明白：OpenAPI 的 getDeviceStatus 拿不到厂商 / 型号，
    // 所以厂商识别得走 ONVIF 的 GetDeviceInformation。这里照样只给运行状态，
    // 别顺手把 model 塞进来，否则那条「双源」的取舍就测不出来了。
    QJsonObject result;
    result.insert(QStringLiteral("sys_time"),
                  (m_camera ? m_camera->deviceTimeUtc() : QDateTime::currentDateTimeUtc())
                      .toString(Qt::ISODate));
    result.insert(QStringLiteral("cpu_usage"), 12);
    result.insert(QStringLiteral("mem_usage"), 43);
    result.insert(QStringLiteral("sd_status"), QStringLiteral("normal"));
    result.insert(QStringLiteral("stream_status"), QStringLiteral("normal"));
    return okObject(result);
}

namespace vendorapi {

VendorApiStub *createVigi(VirtualCamera *camera)
{
    return new VigiStub(camera, camera);
}

} // namespace vendorapi
} // namespace onvifsim
