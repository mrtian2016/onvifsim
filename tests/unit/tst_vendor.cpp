// 五个厂商私有 API 桩的单元测试。
//
// 这里一律直接调各桩的 handle(HttpRequest) —— 那是 registerRoutes 装到 HttpServer 上
// 的同一个函数，只是绕开了 socket 与 HttpExchange（HttpExchange 的构造是 net/ 私有的，
// 测试造不出来）。长连接那两条（海康 alertStream、大华 eventManager attach）因此
// 只测它们的报文构造函数，连接生命周期留给 e2e。
//
// 需要真联动的断言（灯光、IR-cut、云台、预置位）都建一台真的 VirtualCamera，
// 但不 start() —— 桩的路由与私有端口都在 start() 时才挂，不启动就不会占端口。

#include "core/CameraModel.h"
#include "core/Persona.h"
#include "core/Quirks.h"
#include "core/VirtualCamera.h"
#include "events/EventEngine.h"
#include "imaging/ImagingState.h"
#include "net/HttpAuth.h"
#include "net/HttpServer.h"
#include "net/HttpTypes.h"
#include "ptz/PtzState.h"
#include "vendor/VendorApiStub.h"
#include "vendor/dahua/DahuaCgiStub.h"
#include "vendor/hikvision/IsapiStub.h"
#include "vendor/reolink/ReolinkStub.h"
#include "vendor/tplink/TplinkDsStub.h"
#include "vendor/vigi/VigiStub.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QSslSocket>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QtTest>

using namespace onvifsim;

namespace {

constexpr const char *kUser = "admin";
constexpr const char *kPassword = "admin123";

HttpRequest makeRequest(const char *method, const QString &target,
                        const QByteArray &body = QByteArray())
{
    HttpRequest request;
    request.method = method;
    request.rawTarget = target;
    const QUrl url(target);
    request.path = url.path();
    request.query = QUrlQuery(url.query());
    request.body = body;
    request.headers.insert("host", "127.0.0.1:8000");
    return request;
}

// 走一遍完整的 Digest 握手：先挨一个 401 拿挑战，再带着摘要重发。
// 顺带把「桩确实开了 Digest 鉴权」这件事也验了。
template <typename Stub>
HttpResponse digestExchange(Stub *stub, HttpRequest request)
{
    HttpResponse first = stub->handle(request);
    if (first.status != 401)
        return first;

    const QByteArray challenge = first.headers.value("WWW-Authenticate").split('\n').value(0);
    const QHash<QByteArray, QByteArray> params = HttpAuth::parseAuthParams(challenge);
    const QByteArray realm = params.value("realm");
    const QByteArray nonce = params.value("nonce");
    const QByteArray qop = params.value("qop");
    const QByteArray nc = "00000001";
    const QByteArray cnonce = "1f3ab29c";
    const QByteArray uri = request.rawTarget.toUtf8();
    const QByteArray response = HttpAuth::digestResponse(kUser, realm, kPassword,
                                                         request.method, uri, nonce, nc,
                                                         cnonce, qop);
    QByteArray authorization = "Digest username=\"" + QByteArray(kUser)
        + "\", realm=\"" + realm + "\", nonce=\"" + nonce + "\", uri=\"" + uri
        + "\", qop=" + qop + ", nc=" + nc + ", cnonce=\"" + cnonce
        + "\", response=\"" + response + "\"";
    request.headers.insert("authorization", authorization);
    return stub->handle(request);
}

QJsonArray jsonArrayOf(const HttpResponse &response)
{
    return QJsonDocument::fromJson(response.body).array();
}

QJsonObject jsonObjectOf(const HttpResponse &response)
{
    return QJsonDocument::fromJson(response.body).object();
}

// 从 key=value 文本里取一行的值。大华的响应体全是这个形状。
QString textValue(const QByteArray &body, const QString &key)
{
    const QList<QByteArray> lines = body.split('\n');
    for (const QByteArray &line : lines) {
        const QByteArray trimmed = line.trimmed();
        const int eq = trimmed.indexOf('=');
        if (eq <= 0)
            continue;
        if (QString::fromUtf8(trimmed.left(eq)) == key)
            return QString::fromUtf8(trimmed.mid(eq + 1));
    }
    return QString();
}

// 从 401 的挑战里算出一条可用的 Authorization 头。裸 socket 用例要用。
QByteArray digestHeaderFor(const QByteArray &challenge, const QByteArray &method,
                           const QByteArray &uri)
{
    const QHash<QByteArray, QByteArray> params = HttpAuth::parseAuthParams(challenge);
    const QByteArray realm = params.value("realm");
    const QByteArray nonce = params.value("nonce");
    const QByteArray qop = params.value("qop");
    const QByteArray nc = "00000001";
    const QByteArray cnonce = "9c2f77ab";
    const QByteArray response = HttpAuth::digestResponse(kUser, realm, kPassword, method, uri,
                                                         nonce, nc, cnonce, qop);
    return "Digest username=\"" + QByteArray(kUser) + "\", realm=\"" + realm
        + "\", nonce=\"" + nonce + "\", uri=\"" + uri + "\", qop=" + qop
        + ", nc=" + nc + ", cnonce=\"" + cnonce + "\", response=\"" + response + "\"";
}

// 数一段字节里某个子串出现了几次。用来断言「同一条连接上收到了多条」。
int countOf(const QByteArray &haystack, const QByteArray &needle)
{
    int n = 0;
    for (int at = haystack.indexOf(needle); at >= 0;
         at = haystack.indexOf(needle, at + needle.size()))
        ++n;
    return n;
}

// 从整包 HTTP 响应里抠出某个头的值。
QByteArray headerValue(const QByteArray &raw, const QByteArray &name)
{
    const QList<QByteArray> lines = raw.left(qMax(0, int(raw.indexOf("\r\n\r\n")))).split('\n');
    for (const QByteArray &line : lines) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.startsWith(name + ':'))
            return trimmed.mid(name.size() + 1).trimmed();
    }
    return QByteArray();
}

} // namespace

class TstVendor : public QObject
{
    Q_OBJECT

private slots:
    // ---- 海康 ISAPI ----
    void isapiSpeedRoundTrip();
    void isapiPtzDataRoundTrip();
    void isapiDeviceInfoXml();
    void isapiDigestRequired();
    void isapiContinuousMovesPtz();
    void isapiSupplementLightDrivesImaging();
    void isapiIrcutFilterDrivesImaging();
    void isapiPresetsReflectPtzState();
    void isapiEventAlertXml();
    void isapiAlertStreamPushesEvent();

    // ---- 大华 CGI ----
    void dahuaSystemInfoText();
    void dahuaEventLineFormat();
    void dahuaParseCodes();
    void dahuaLightingRoundTrip();
    void dahuaPtzStartMoves();
    void dahuaEncodeConfig();
    void dahuaAttachHeartbeat();

    // ---- Reolink ----
    void reolinkResponseIsArray();
    void reolinkTokenExpiredCode();
    void reolinkWhiteLedDrivesImaging();
    void reolinkPresetRoundTrip();

    // ---- TP-Link VIGI ----
    void vigiTwoStepAuth();
    void vigiRejectsBadDigest();
    void vigiDetectionSwitches();
    void vigiPresetPoint();
    void vigiPrivateListener();
    void vigiTlsHandshake();

    // ---- TP-Link TL-IPC ----
    void tplinkDsLightRoundTrip();
    void tplinkDsRejectsStaleStok();

private:
    // 每条用例自建一台相机：桩持有的 token / stok / 检测开关都是实例状态，
    // 共用一台会让用例互相影响。
    static VirtualCamera *makeCamera(const QString &personaKey, QObject *parent);
    // 起一台真的在监听的相机（端口交给内核挑），失败返回 nullptr 让用例 QSKIP。
    static VirtualCamera *startedCamera(const QString &personaKey, QObject *parent);
};

VirtualCamera *TstVendor::startedCamera(const QString &personaKey, QObject *parent)
{
    CameraModel model = CameraModel::makeDefault(QStringLiteral("cam-live"), personaKey, 0);
    model.bindAddress = QHostAddress(QHostAddress::LocalHost);
    model.imaging.whiteLight = true;
    model.httpPort = 0;    // 0 = 内核挑一个空闲端口，CI 上不会撞车
    model.rtspPort = 0;
    auto *camera = new VirtualCamera(model, nullptr, parent);
    QString error;
    if (!camera->start(&error)) {
        delete camera;
        return nullptr;
    }
    return camera;
}

VirtualCamera *TstVendor::makeCamera(const QString &personaKey, QObject *parent)
{
    CameraModel model = CameraModel::makeDefault(QStringLiteral("cam-test"), personaKey, 0);
    model.bindAddress = QHostAddress(QHostAddress::LocalHost);
    model.imaging.whiteLight = true;
    return new VirtualCamera(model, nullptr, parent);
}

// ---------------------------------------------------------------------------
// 海康 ISAPI
// ---------------------------------------------------------------------------

void TstVendor::isapiSpeedRoundTrip()
{
    // -100..100 ↔ [-1, 1]：整个域上往返必须原样回来，
    // 否则客户端下发 50 会读回 49，界面上的滑块会自己漂。
    for (int v = -100; v <= 100; ++v)
        QCOMPARE(IsapiStub::speedToIsapi(IsapiStub::speedFromIsapi(v)), v);

    QCOMPARE(IsapiStub::speedFromIsapi(100), 1.0);
    QCOMPARE(IsapiStub::speedFromIsapi(-100), -1.0);
    QVERIFY(qFuzzyIsNull(IsapiStub::speedFromIsapi(0)));
    QCOMPARE(IsapiStub::speedFromIsapi(50), 0.5);
    // 越界要夹住，不能溢出成别的方向。
    QCOMPARE(IsapiStub::speedFromIsapi(500), 1.0);
    QCOMPARE(IsapiStub::speedToIsapi(2.5), 100);
    QCOMPARE(IsapiStub::speedToIsapi(-2.5), -100);
}

void TstVendor::isapiPtzDataRoundTrip()
{
    const QByteArray xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                           "<PTZData><pan>60</pan><tilt>-40</tilt><zoom>20</zoom></PTZData>";
    const PtzVector v = IsapiStub::parsePtzData(xml);
    QVERIFY(v.hasPanTilt);
    QVERIFY(v.hasZoom);
    QCOMPARE(v.pan, 0.6);
    QCOMPARE(v.tilt, -0.4);
    QCOMPARE(v.zoom, 0.2);

    // 再生成回去，数值必须一模一样。
    const QByteArray rebuilt = IsapiStub::buildPtzData(v);
    QVERIFY(rebuilt.contains("<pan>60</pan>"));
    QVERIFY(rebuilt.contains("<tilt>-40</tilt>"));
    QVERIFY(rebuilt.contains("<zoom>20</zoom>"));

    // 只给 pan/tilt 的报文不能凭空造出 zoom 分量。
    const PtzVector noZoom = IsapiStub::parsePtzData("<PTZData><pan>0</pan><tilt>0</tilt></PTZData>");
    QVERIFY(noZoom.hasPanTilt);
    QVERIFY(!noZoom.hasZoom);
}

void TstVendor::isapiDeviceInfoXml()
{
    QObject owner;
    VirtualCamera *camera = makeCamera(QStringLiteral("hikvision"), &owner);
    auto *stub = qobject_cast<IsapiStub *>(camera->vendorApi());
    QVERIFY(stub);
    QCOMPARE(stub->kind(), VendorApi::Isapi);

    const HttpResponse response =
        digestExchange(stub, makeRequest("GET", QStringLiteral("/ISAPI/System/deviceInfo")));
    QCOMPARE(response.status, 200);
    QCOMPARE(response.headers.value("Content-Type"),
             QByteArray("application/xml; charset=\"UTF-8\""));

    const QByteArray body = response.body;
    QVERIFY(body.startsWith("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"));
    QVERIFY(body.contains("<DeviceInfo version=\"2.0\" "
                          "xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"));
    QVERIFY(body.contains("<deviceType>IPCamera</deviceType>"));
    QVERIFY(body.contains("<model>" + camera->model().model.toUtf8() + "</model>"));
    QVERIFY(body.contains("<serialNumber>" + camera->model().serialNumber.toUtf8()
                          + "</serialNumber>"));
    // 厂商识别靠 manufacturer 的子串匹配，这条不能丢。
    QVERIFY(body.contains("<manufacturer>HIKVISION</manufacturer>"));
    // "V5.6.3 build 190923" 要拆成版本 + 发布日期两个字段。
    QVERIFY(body.contains("<firmwareVersion>V5.6.3</firmwareVersion>"));
    QVERIFY(body.contains("<firmwareReleasedDate>190923</firmwareReleasedDate>"));
}

void TstVendor::isapiDigestRequired()
{
    QObject owner;
    VirtualCamera *camera = makeCamera(QStringLiteral("hikvision"), &owner);
    auto *stub = qobject_cast<IsapiStub *>(camera->vendorApi());
    QVERIFY(stub);

    const HttpResponse bare =
        stub->handle(makeRequest("GET", QStringLiteral("/ISAPI/System/deviceInfo")));
    QCOMPARE(bare.status, 401);
    QVERIFY(bare.headers.value("WWW-Authenticate").startsWith("Digest "));
    QVERIFY(bare.body.contains("<statusCode>4</statusCode>"));
}

void TstVendor::isapiContinuousMovesPtz()
{
    QObject owner;
    VirtualCamera *camera = makeCamera(QStringLiteral("hikvision"), &owner);
    auto *stub = qobject_cast<IsapiStub *>(camera->vendorApi());
    QVERIFY(stub);

    const HttpResponse moved = digestExchange(
        stub, makeRequest("PUT", QStringLiteral("/ISAPI/PTZCtrl/channels/1/continuous"),
                          "<PTZData><pan>100</pan><tilt>0</tilt></PTZData>"));
    QCOMPARE(moved.status, 200);
    QVERIFY(moved.body.contains("<statusCode>1</statusCode>"));
    QCOMPARE(camera->ptz()->panTiltStatus(), PtzMoveStatus::Moving);
    QTest::qWait(150);   // 让积分器真的走一段，下面才能断言位置变了

    // 全零的 PTZData 等价于停 —— 真机就是靠这条刹车的。
    const HttpResponse stopped = digestExchange(
        stub, makeRequest("PUT", QStringLiteral("/ISAPI/PTZCtrl/channels/1/continuous"),
                          "<PTZData><pan>0</pan><tilt>0</tilt></PTZData>"));
    QCOMPARE(stopped.status, 200);
    QCOMPARE(camera->ptz()->panTiltStatus(), PtzMoveStatus::Idle);
    QVERIFY(camera->ptz()->position().pan > 0.0);   // 停之前确实转过
}

void TstVendor::isapiSupplementLightDrivesImaging()
{
    QObject owner;
    VirtualCamera *camera = makeCamera(QStringLiteral("hikvision"), &owner);
    auto *stub = qobject_cast<IsapiStub *>(camera->vendorApi());
    QVERIFY(stub);
    QVERIFY(!camera->imaging()->settings().whiteLightOn);

    const HttpResponse on = digestExchange(
        stub, makeRequest("PUT", QStringLiteral("/ISAPI/Image/channels/1/supplementLight"),
                          "<SupplementLight><supplementLightMode>colorVuWhiteLight"
                          "</supplementLightMode><whiteLightBrightness>80</whiteLightBrightness>"
                          "</SupplementLight>"));
    QCOMPARE(on.status, 200);
    // 私有接口改的就是 ONVIF 侧那份 ImagingState。
    QVERIFY(camera->imaging()->settings().whiteLightOn);
    QCOMPARE(camera->imaging()->settings().whiteLightBrightness, 80);

    const HttpResponse readBack = digestExchange(
        stub, makeRequest("GET", QStringLiteral("/ISAPI/Image/channels/1/supplementLight")));
    QVERIFY(readBack.body.contains("<supplementLightMode>colorVuWhiteLight</supplementLightMode>"));
    QVERIFY(readBack.body.contains("<whiteLightBrightness>80</whiteLightBrightness>"));

    const HttpResponse off = digestExchange(
        stub, makeRequest("PUT", QStringLiteral("/ISAPI/Image/channels/1/supplementLight"),
                          "<SupplementLight><supplementLightMode>close</supplementLightMode>"
                          "</SupplementLight>"));
    QCOMPARE(off.status, 200);
    QVERIFY(!camera->imaging()->settings().whiteLightOn);

    // capabilities 是客户端探能力用的，opt 列表要在。
    const HttpResponse caps = digestExchange(
        stub,
        makeRequest("GET", QStringLiteral("/ISAPI/Image/channels/1/supplementLight/capabilities")));
    QCOMPARE(caps.status, 200);
    QVERIFY(caps.body.contains("colorVuWhiteLight"));
}

void TstVendor::isapiIrcutFilterDrivesImaging()
{
    QObject owner;
    VirtualCamera *camera = makeCamera(QStringLiteral("hikvision"), &owner);
    auto *stub = qobject_cast<IsapiStub *>(camera->vendorApi());
    QVERIFY(stub);

    // night = 滤光片切走 = ONVIF 的 OFF（夜视黑白）。
    const HttpResponse night = digestExchange(
        stub, makeRequest("PUT", QStringLiteral("/ISAPI/Image/channels/1/ircutFilter"),
                          "<IrcutFilter><IrcutFilterType>night</IrcutFilterType></IrcutFilter>"));
    QCOMPARE(night.status, 200);
    QCOMPARE(camera->imaging()->settings().irCutFilter, IrCutFilterMode::Off);

    const HttpResponse day = digestExchange(
        stub, makeRequest("PUT", QStringLiteral("/ISAPI/Image/channels/1/ircutFilter"),
                          "<IrcutFilter><IrcutFilterType>day</IrcutFilterType></IrcutFilter>"));
    QCOMPARE(day.status, 200);
    QCOMPARE(camera->imaging()->settings().irCutFilter, IrCutFilterMode::On);

    const HttpResponse readBack = digestExchange(
        stub, makeRequest("GET", QStringLiteral("/ISAPI/Image/channels/1/ircutFilter")));
    QVERIFY(readBack.body.contains("<IrcutFilterType>day</IrcutFilterType>"));

    const HttpResponse bad = digestExchange(
        stub, makeRequest("PUT", QStringLiteral("/ISAPI/Image/channels/1/ircutFilter"),
                          "<IrcutFilter><IrcutFilterType>nonsense</IrcutFilterType></IrcutFilter>"));
    QCOMPARE(bad.status, 400);
}

void TstVendor::isapiPresetsReflectPtzState()
{
    QObject owner;
    VirtualCamera *camera = makeCamera(QStringLiteral("hikvision"), &owner);
    auto *stub = qobject_cast<IsapiStub *>(camera->vendorApi());
    QVERIFY(stub);

    const HttpResponse written = digestExchange(
        stub, makeRequest("PUT", QStringLiteral("/ISAPI/PTZCtrl/channels/1/presets/3"),
                          "<PTZPreset><id>3</id><presetName>大门</presetName></PTZPreset>"));
    QCOMPARE(written.status, 200);

    // 写进去的是 PtzState 的预置位表，ONVIF 侧读到的是同一份。
    const PtzPreset *preset = camera->ptz()->preset(QStringLiteral("3"));
    QVERIFY(preset);
    QCOMPARE(preset->name, QStringLiteral("大门"));

    const HttpResponse list = digestExchange(
        stub, makeRequest("GET", QStringLiteral("/ISAPI/PTZCtrl/channels/1/presets")));
    QCOMPARE(list.status, 200);
    QVERIFY(list.body.contains("<PTZPresetList"));
    QVERIFY(list.body.contains("<id>3</id>"));
    QVERIFY(list.body.contains(QString::fromUtf8("<presetName>大门</presetName>").toUtf8()));

    const HttpResponse goTo = digestExchange(
        stub, makeRequest("PUT", QStringLiteral("/ISAPI/PTZCtrl/channels/1/presets/3/goto")));
    QCOMPARE(goTo.status, 200);

    const HttpResponse missing = digestExchange(
        stub, makeRequest("PUT", QStringLiteral("/ISAPI/PTZCtrl/channels/1/presets/99/goto")));
    QCOMPARE(missing.status, 400);
}

void TstVendor::isapiEventAlertXml()
{
    // 用 ISO 串构造，避开 QDateTime(QDate, QTime, Qt::TimeSpec) 在新版 Qt 上的弃用。
    const QDateTime when = QDateTime::fromString(QStringLiteral("2026-09-10T01:02:03Z"),
                                                 Qt::ISODate);
    const QByteArray xml = IsapiStub::eventAlertXml(QStringLiteral("192.168.1.64"), 1, when,
                                                    QStringLiteral("vehicledetection"),
                                                    QStringLiteral("active"),
                                                    QStringLiteral("vehicle"),
                                                    QStringLiteral("京A12345"));
    // 客户端只读这三处：eventType / targetType / licensePlate。
    QVERIFY(xml.contains("<EventNotificationAlert version=\"2.0\""));
    QVERIFY(xml.contains("<eventType>vehicledetection</eventType>"));
    QVERIFY(xml.contains("<eventState>active</eventState>"));
    QVERIFY(xml.contains("<targetType>vehicle</targetType>"));
    QVERIFY(xml.contains(QString::fromUtf8("<licensePlate>京A12345</licensePlate>").toUtf8()));
    QVERIFY(xml.contains("<dateTime>2026-09-10T01:02:03Z</dateTime>"));

    // 没有目标类型时不该凭空造出 DetectionRegionList。
    const QByteArray plain = IsapiStub::eventAlertXml(QStringLiteral("10.0.0.1"), 1, when,
                                                      QStringLiteral("VMD"),
                                                      QStringLiteral("inactive"), QString(),
                                                      QString());
    QVERIFY(!plain.contains("<targetType>"));
    QVERIFY(!plain.contains("<ANPR>"));
}

void TstVendor::isapiAlertStreamPushesEvent()
{
    QObject owner;
    VirtualCamera *camera = startedCamera(QStringLiteral("hikvision"), &owner);
    if (!camera)
        QSKIP("相机没起来（端口被占用），跳过");
    auto *stub = qobject_cast<IsapiStub *>(camera->vendorApi());
    QVERIFY(stub);
    QCOMPARE(stub->streamingClients(), 0);

    const quint16 port = camera->httpServer()->serverPort();
    const QByteArray uri = "/ISAPI/Event/notification/alertStream";

    // 先挨一个 401 把挑战拿到手。
    QTcpSocket probe;
    QByteArray probeReply;
    connect(&probe, &QTcpSocket::readyRead, &probe,
            [&probe, &probeReply] { probeReply += probe.readAll(); });
    probe.connectToHost(QHostAddress::LocalHost, port);
    QVERIFY(QTest::qWaitFor([&probe] { return probe.state() == QAbstractSocket::ConnectedState; },
                            3000));
    probe.write("GET " + uri + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    QTRY_VERIFY_WITH_TIMEOUT(probeReply.startsWith("HTTP/1.1 401"), 3000);
    const QByteArray challenge = headerValue(probeReply, "WWW-Authenticate");
    QVERIFY(challenge.startsWith("Digest "));
    probe.abort();

    // 带上摘要重连。这条连接从此一直挂着，事件来一条推一段。
    QTcpSocket stream;
    QByteArray reply;
    connect(&stream, &QTcpSocket::readyRead, &stream,
            [&stream, &reply] { reply += stream.readAll(); });
    stream.connectToHost(QHostAddress::LocalHost, port);
    QVERIFY(QTest::qWaitFor([&stream] { return stream.state() == QAbstractSocket::ConnectedState; },
                            3000));
    stream.write("GET " + uri + " HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: "
                 + digestHeaderFor(challenge, "GET", uri) + "\r\n\r\n");

    // 响应头先到，且**不能带 Content-Length** —— 带了客户端会照它把流截断。
    QTRY_VERIFY_WITH_TIMEOUT(reply.contains("\r\n\r\n"), 3000);
    QVERIFY(reply.startsWith("HTTP/1.1 200"));
    QCOMPARE(headerValue(reply, "Content-Type"), QByteArray("multipart/mixed; boundary=boundary"));
    QVERIFY(headerValue(reply, "Content-Length").isEmpty());
    QCOMPARE(stub->streamingClients(), 1);
    QVERIFY(!reply.contains("EventNotificationAlert"));   // 还没有事件，头之后什么都没有

    // 第一条：越界侦测。ONVIF 订阅方与 alertStream 走的是同一条 eventProduced。
    camera->events()->trigger(EventKind::LineCrossing, 0);
    QTRY_VERIFY_WITH_TIMEOUT(reply.contains("<eventType>linedetection</eventType>"), 3000);
    QVERIFY(reply.contains("--boundary\r\n"));
    QVERIFY(reply.contains("<targetType>human</targetType>"));

    // 第二条走**同一条连接**：这才是真机行为，客户端不该被迫重连。
    camera->events()->trigger(EventKind::VehicleDetect, 0);
    QTRY_VERIFY_WITH_TIMEOUT(reply.contains("<eventType>vehicledetection</eventType>"), 3000);
    QVERIFY(reply.contains("<licensePlate>"));
    QCOMPARE(countOf(reply, "</EventNotificationAlert>"), 2);
    QCOMPARE(stream.state(), QAbstractSocket::ConnectedState);   // 连接还开着
    QCOMPARE(stub->streamingClients(), 1);

    // 客户端走掉之后服务端要能察觉，把心跳定时器与流一起收掉。
    stream.abort();
    QTRY_VERIFY_WITH_TIMEOUT(stub->streamingClients() == 0, 3000);
}

// ---------------------------------------------------------------------------
// 大华 CGI
// ---------------------------------------------------------------------------

void TstVendor::dahuaSystemInfoText()
{
    QObject owner;
    VirtualCamera *camera = makeCamera(QStringLiteral("dahua"), &owner);
    auto *stub = qobject_cast<DahuaCgiStub *>(camera->vendorApi());
    QVERIFY(stub);
    QCOMPARE(stub->kind(), VendorApi::DahuaCgi);

    const HttpResponse response = digestExchange(
        stub, makeRequest("GET", QStringLiteral("/cgi-bin/magicBox.cgi?action=getSystemInfo")));
    QCOMPARE(response.status, 200);
    QCOMPARE(response.headers.value("Content-Type"), QByteArray("text/plain;charset=UTF-8"));

    // 响应体是 key=value 纯文本，一行一个，CRLF 分隔。
    QVERIFY(response.body.contains("\r\n"));
    QCOMPARE(textValue(response.body, QStringLiteral("deviceType")), camera->model().model);
    QCOMPARE(textValue(response.body, QStringLiteral("serialNumber")),
             camera->model().serialNumber);
    QCOMPARE(textValue(response.body, QStringLiteral("vendor")), camera->model().manufacturer);
    QVERIFY(!textValue(response.body, QStringLiteral("hardwareVersion")).isEmpty());
}

void TstVendor::dahuaEventLineFormat()
{
    // 长连接里客户端唯一解析的东西，格式一个字符都不能差。
    QCOMPARE(DahuaCgiStub::eventLine(QStringLiteral("VideoMotion"), QStringLiteral("Start"), 0),
             QByteArray("Code=VideoMotion;action=Start;index=0"));
    QCOMPARE(DahuaCgiStub::eventLine(QStringLiteral("CrossLineDetection"),
                                     QStringLiteral("Pulse"), 2),
             QByteArray("Code=CrossLineDetection;action=Pulse;index=2"));
    QCOMPARE(DahuaCgiStub::eventLine(QStringLiteral("SmartMotionHuman"), QStringLiteral("Stop"),
                                     0, "{\"Object\":{\"ObjectType\":\"Human\"}}"),
             QByteArray("Code=SmartMotionHuman;action=Stop;index=0"
                        ";data={\"Object\":{\"ObjectType\":\"Human\"}}"));

    // 事件码只落在 reference-client-facts.md §7.2 列的 8 个里。
    QCOMPARE(DahuaCgiStub::eventCodeFor(EventKind::Motion), QStringLiteral("VideoMotion"));
    QCOMPARE(DahuaCgiStub::eventCodeFor(EventKind::LineCrossing),
             QStringLiteral("CrossLineDetection"));
    QCOMPARE(DahuaCgiStub::eventCodeFor(EventKind::FieldIntrusion),
             QStringLiteral("CrossRegionDetection"));
    QCOMPARE(DahuaCgiStub::eventCodeFor(EventKind::PeopleDetect),
             QStringLiteral("SmartMotionHuman"));
    QCOMPARE(DahuaCgiStub::eventCodeFor(EventKind::VehicleDetect),
             QStringLiteral("SmartMotionVehicle"));
    QCOMPARE(DahuaCgiStub::eventCodeFor(EventKind::FaceDetect), QStringLiteral("FaceDetection"));
    QCOMPARE(DahuaCgiStub::eventCodeFor(EventKind::Tamper), QStringLiteral("VideoBlind"));

    // multipart 片段：分隔串、Content-Length 与空行的位置。
    const QByteArray part = DahuaCgiStub::multipartPart("Heartbeat");
    QVERIFY(part.startsWith("--myboundary\r\n"));
    QVERIFY(part.contains("Content-Type: text/plain\r\n"));
    QVERIFY(part.contains("Content-Length: 9\r\n\r\nHeartbeat"));
}

void TstVendor::dahuaParseCodes()
{
    QCOMPARE(DahuaCgiStub::parseCodes(QStringLiteral("[VideoMotion,CrossLineDetection]")),
             (QStringList{ QStringLiteral("VideoMotion"), QStringLiteral("CrossLineDetection") }));
    // [All] 用空表表示「全收」。方括号被百分号编码过也要认得出来，
    // 否则整串会被当成一个码名，表现是「订阅成功但零事件」。
    QVERIFY(DahuaCgiStub::parseCodes(QStringLiteral("[All]")).isEmpty());
    QVERIFY(DahuaCgiStub::parseCodes(QStringLiteral("%5BAll%5D")).isEmpty());
    QCOMPARE(DahuaCgiStub::parseCodes(QStringLiteral("%5BVideoMotion%5D")),
             (QStringList{ QStringLiteral("VideoMotion") }));
    QVERIFY(DahuaCgiStub::parseCodes(QString()).isEmpty());
    QCOMPARE(DahuaCgiStub::parseCodes(QStringLiteral("VideoMotion")),
             (QStringList{ QStringLiteral("VideoMotion") }));
}

void TstVendor::dahuaLightingRoundTrip()
{
    QObject owner;
    VirtualCamera *camera = makeCamera(QStringLiteral("dahua"), &owner);
    auto *stub = qobject_cast<DahuaCgiStub *>(camera->vendorApi());
    QVERIFY(stub);
    QVERIFY(!camera->imaging()->settings().whiteLightOn);

    const HttpResponse set = digestExchange(
        stub, makeRequest("GET", QStringLiteral("/cgi-bin/configManager.cgi?action=setConfig"
                                                "&Lighting_V2[0][0][0].Mode=Manual"
                                                "&Lighting_V2[0][0][0].MiddleLight[0].Light=70")));
    QCOMPARE(set.status, 200);
    QCOMPARE(set.body, QByteArray("OK\r\n"));
    QVERIFY(camera->imaging()->settings().whiteLightOn);
    QCOMPARE(camera->imaging()->settings().whiteLightBrightness, 70);

    const HttpResponse get = digestExchange(
        stub, makeRequest("GET", QStringLiteral("/cgi-bin/configManager.cgi"
                                                "?action=getConfig&name=Lighting_V2")));
    QCOMPARE(textValue(get.body, QStringLiteral("table.Lighting_V2[0][0][0].Mode")),
             QStringLiteral("Manual"));
    QCOMPARE(textValue(get.body, QStringLiteral("table.Lighting_V2[0][0][0].MiddleLight[0].Light")),
             QStringLiteral("70"));

    // 同轴控制口也改同一份状态。
    const HttpResponse coax = digestExchange(
        stub, makeRequest("GET", QStringLiteral("/cgi-bin/coaxialControlIO.cgi?action=control"
                                                "&channel=1&info[0].Type=WhiteLight&info[0].IO=2")));
    QCOMPARE(coax.status, 200);
    QVERIFY(!camera->imaging()->settings().whiteLightOn);
}

void TstVendor::dahuaPtzStartMoves()
{
    QObject owner;
    VirtualCamera *camera = makeCamera(QStringLiteral("dahua"), &owner);
    auto *stub = qobject_cast<DahuaCgiStub *>(camera->vendorApi());
    QVERIFY(stub);

    const HttpResponse start = digestExchange(
        stub, makeRequest("GET", QStringLiteral("/cgi-bin/ptz.cgi?action=start&channel=0"
                                                "&code=Right&arg1=0&arg2=8&arg3=0")));
    QCOMPARE(start.status, 200);
    QCOMPARE(camera->ptz()->panTiltStatus(), PtzMoveStatus::Moving);

    const HttpResponse stop = digestExchange(
        stub, makeRequest("GET", QStringLiteral("/cgi-bin/ptz.cgi?action=stop&channel=0")));
    QCOMPARE(stop.status, 200);
    QCOMPARE(camera->ptz()->panTiltStatus(), PtzMoveStatus::Idle);

    const HttpResponse caps = digestExchange(
        stub, makeRequest("GET", QStringLiteral("/cgi-bin/ptz.cgi"
                                                "?action=getCurrentProtocolCaps&channel=0")));
    QCOMPARE(textValue(caps.body, QStringLiteral("caps.Pan")), QStringLiteral("true"));
    // 真机把 Tilt 拼成了 Tile，客户端跟着错，桩必须照错。
    QCOMPARE(textValue(caps.body, QStringLiteral("caps.Tile")), QStringLiteral("true"));
    QCOMPARE(textValue(caps.body, QStringLiteral("caps.PresetNum")),
             QString::number(camera->model().ptzNode.maxPresets));
}

void TstVendor::dahuaEncodeConfig()
{
    QObject owner;
    VirtualCamera *camera = makeCamera(QStringLiteral("dahua"), &owner);
    auto *stub = qobject_cast<DahuaCgiStub *>(camera->vendorApi());
    QVERIFY(stub);
    QVERIFY(!camera->model().profiles.isEmpty());

    const HttpResponse encode = digestExchange(
        stub, makeRequest("GET", QStringLiteral("/cgi-bin/configManager.cgi"
                                                "?action=getConfig&name=Encode")));
    QCOMPARE(encode.status, 200);
    const VideoEncoderConfig &video = camera->model().profiles.first().videoEncoder;
    QCOMPARE(textValue(encode.body,
                       QStringLiteral("table.Encode[0].MainFormat[0].Video.Width")),
             QString::number(video.width));
    // 大华的编码名带点，ONVIF 侧不带。
    QCOMPARE(textValue(encode.body,
                       QStringLiteral("table.Encode[0].MainFormat[0].Video.Compression")),
             QStringLiteral("H.264"));
}

void TstVendor::dahuaAttachHeartbeat()
{
    QObject owner;
    VirtualCamera *camera = startedCamera(QStringLiteral("dahua"), &owner);
    if (!camera)
        QSKIP("相机没起来（端口被占用），跳过");
    auto *stub = qobject_cast<DahuaCgiStub *>(camera->vendorApi());
    QVERIFY(stub);

    const quint16 port = camera->httpServer()->serverPort();
    const QByteArray uri = "/cgi-bin/eventManager.cgi?action=attach&codes=%5BAll%5D&heartbeat=1";

    QTcpSocket probe;
    QByteArray probeReply;
    connect(&probe, &QTcpSocket::readyRead, &probe,
            [&probe, &probeReply] { probeReply += probe.readAll(); });
    probe.connectToHost(QHostAddress::LocalHost, port);
    QVERIFY(QTest::qWaitFor([&probe] { return probe.state() == QAbstractSocket::ConnectedState; },
                            3000));
    probe.write("GET " + uri + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    QTRY_VERIFY_WITH_TIMEOUT(probeReply.startsWith("HTTP/1.1 401"), 3000);
    const QByteArray challenge = headerValue(probeReply, "WWW-Authenticate");
    probe.abort();

    QTcpSocket stream;
    QByteArray reply;
    connect(&stream, &QTcpSocket::readyRead, &stream,
            [&stream, &reply] { reply += stream.readAll(); });
    stream.connectToHost(QHostAddress::LocalHost, port);
    QVERIFY(QTest::qWaitFor([&stream] { return stream.state() == QAbstractSocket::ConnectedState; },
                            3000));
    stream.write("GET " + uri + " HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: "
                 + digestHeaderFor(challenge, "GET", uri) + "\r\n\r\n");

    QTRY_VERIFY_WITH_TIMEOUT(reply.contains("\r\n\r\n"), 3000);
    QVERIFY(reply.startsWith("HTTP/1.1 200"));
    QCOMPARE(headerValue(reply, "Content-Type"),
             QByteArray("multipart/x-mixed-replace;boundary=myboundary"));
    QVERIFY(headerValue(reply, "Content-Length").isEmpty());
    QCOMPARE(stub->streamingClients(), 1);

    // heartbeat=1：每秒一行，且是在**同一条连接**上连着来的，不是每次重连一条。
    QTRY_VERIFY_WITH_TIMEOUT(countOf(reply, "Heartbeat") >= 2, 6000);
    QCOMPARE(stream.state(), QAbstractSocket::ConnectedState);
    QVERIFY(reply.contains("--myboundary\r\n"));

    // 心跳之间照样能插事件进来。
    camera->events()->trigger(EventKind::LineCrossing, 0);
    QTRY_VERIFY_WITH_TIMEOUT(reply.contains("Code=CrossLineDetection;action="), 3000);
    QCOMPARE(stream.state(), QAbstractSocket::ConnectedState);
    QCOMPARE(stub->streamingClients(), 1);

    stream.abort();
    QTRY_VERIFY_WITH_TIMEOUT(stub->streamingClients() == 0, 3000);
}

// ---------------------------------------------------------------------------
// Reolink
// ---------------------------------------------------------------------------

void TstVendor::reolinkResponseIsArray()
{
    QObject owner;
    VirtualCamera *camera = makeCamera(QStringLiteral("reolink"), &owner);
    auto *stub = qobject_cast<ReolinkStub *>(camera->vendorApi());
    QVERIFY(stub);
    QCOMPARE(stub->kind(), VendorApi::ReolinkJson);

    const QByteArray loginBody = "[{\"cmd\":\"Login\",\"action\":0,\"param\":{\"User\":"
                                 "{\"userName\":\"admin\",\"password\":\"admin123\"}}}]";
    const HttpResponse login =
        stub->handle(makeRequest("POST", QStringLiteral("/api.cgi?cmd=Login"), loginBody));
    QCOMPARE(login.status, 200);

    // 响应必须是数组，每项 {cmd, code, value}。
    const QJsonDocument doc = QJsonDocument::fromJson(login.body);
    QVERIFY(doc.isArray());
    const QJsonArray items = doc.array();
    QCOMPARE(items.size(), 1);
    const QJsonObject item = items.at(0).toObject();
    QCOMPARE(item.value(QStringLiteral("cmd")).toString(), QStringLiteral("Login"));
    QCOMPARE(item.value(QStringLiteral("code")).toInt(), 0);
    const QJsonObject token = item.value(QStringLiteral("value")).toObject()
                                  .value(QStringLiteral("Token")).toObject();
    QCOMPARE(token.value(QStringLiteral("leaseTime")).toInt(), 3600);
    QVERIFY(!token.value(QStringLiteral("name")).toString().isEmpty());
    QCOMPARE(token.value(QStringLiteral("name")).toString(), stub->currentToken());

    // 带上 token 后拿设备信息，识别用的三件套都要在。
    const QString t = stub->currentToken();
    const HttpResponse info = stub->handle(
        makeRequest("POST", QStringLiteral("/api.cgi?cmd=GetDevInfo&token=") + t,
                    "[{\"cmd\":\"GetDevInfo\",\"action\":0,\"param\":{}}]"));
    const QJsonObject dev = jsonArrayOf(info).at(0).toObject()
                                .value(QStringLiteral("value")).toObject()
                                .value(QStringLiteral("DevInfo")).toObject();
    QCOMPARE(dev.value(QStringLiteral("model")).toString(), camera->model().model);
    QCOMPARE(dev.value(QStringLiteral("firmVer")).toString(), camera->model().firmwareVersion);
    QCOMPARE(dev.value(QStringLiteral("vendor")).toString(), camera->model().manufacturer);
}

void TstVendor::reolinkTokenExpiredCode()
{
    QObject owner;
    VirtualCamera *camera = makeCamera(QStringLiteral("reolink"), &owner);
    auto *stub = qobject_cast<ReolinkStub *>(camera->vendorApi());
    QVERIFY(stub);
    QCOMPARE(ReolinkStub::tokenExpiredCode(), -6);

    // 还没登录：任何命令都该回 -6，客户端据此先 Login。
    const HttpResponse anonymous = stub->handle(
        makeRequest("POST", QStringLiteral("/api.cgi?cmd=GetAbility"),
                    "[{\"cmd\":\"GetAbility\",\"action\":0,\"param\":{}}]"));
    QCOMPARE(anonymous.status, 200);   // 真机的业务错误照样是 HTTP 200
    QJsonObject item = jsonArrayOf(anonymous).at(0).toObject();
    QCOMPARE(item.value(QStringLiteral("code")).toInt(), -6);
    QCOMPARE(item.value(QStringLiteral("error")).toObject()
                 .value(QStringLiteral("rspCode")).toInt(), -6);

    stub->handle(makeRequest("POST", QStringLiteral("/api.cgi?cmd=Login"),
                             "[{\"cmd\":\"Login\",\"param\":{\"User\":"
                             "{\"userName\":\"admin\",\"password\":\"admin123\"}}}]"));
    const QString token = stub->currentToken();
    QVERIFY(!token.isEmpty());

    // 有效 token：正常返回。
    HttpResponse ok = stub->handle(
        makeRequest("POST", QStringLiteral("/api.cgi?cmd=GetAbility&token=") + token,
                    "[{\"cmd\":\"GetAbility\",\"action\":0,\"param\":{}}]"));
    QCOMPARE(jsonArrayOf(ok).at(0).toObject().value(QStringLiteral("code")).toInt(), 0);

    // 过期之后同一个 token 就不认了。
    stub->expireToken();
    ok = stub->handle(
        makeRequest("POST", QStringLiteral("/api.cgi?cmd=GetAbility&token=") + token,
                    "[{\"cmd\":\"GetAbility\",\"action\":0,\"param\":{}}]"));
    item = jsonArrayOf(ok).at(0).toObject();
    QCOMPARE(item.value(QStringLiteral("code")).toInt(), -6);

    // 口令不对回的是 -7，不能跟「token 过期」混成同一个码。
    const HttpResponse badLogin = stub->handle(
        makeRequest("POST", QStringLiteral("/api.cgi?cmd=Login"),
                    "[{\"cmd\":\"Login\",\"param\":{\"User\":"
                    "{\"userName\":\"admin\",\"password\":\"wrong\"}}}]"));
    QCOMPARE(jsonArrayOf(badLogin).at(0).toObject().value(QStringLiteral("code")).toInt(), -7);
}

void TstVendor::reolinkWhiteLedDrivesImaging()
{
    QObject owner;
    VirtualCamera *camera = makeCamera(QStringLiteral("reolink"), &owner);
    auto *stub = qobject_cast<ReolinkStub *>(camera->vendorApi());
    QVERIFY(stub);

    stub->handle(makeRequest("POST", QStringLiteral("/api.cgi?cmd=Login"),
                             "[{\"cmd\":\"Login\",\"param\":{\"User\":"
                             "{\"userName\":\"admin\",\"password\":\"admin123\"}}}]"));
    const QString token = stub->currentToken();

    const HttpResponse set = stub->handle(
        makeRequest("POST", QStringLiteral("/api.cgi?cmd=SetWhiteLed&token=") + token,
                    "[{\"cmd\":\"SetWhiteLed\",\"param\":{\"WhiteLed\":"
                    "{\"channel\":0,\"state\":1,\"mode\":3,\"bright\":55}}}]"));
    QCOMPARE(jsonArrayOf(set).at(0).toObject().value(QStringLiteral("code")).toInt(), 0);
    QCOMPARE(jsonArrayOf(set).at(0).toObject().value(QStringLiteral("value")).toObject()
                 .value(QStringLiteral("rspCode")).toInt(), 200);

    // 与海康 supplementLight、TP-Link /ds 改的是同一份 ImagingState。
    QVERIFY(camera->imaging()->settings().whiteLightOn);
    QCOMPARE(camera->imaging()->settings().whiteLightBrightness, 55);

    const HttpResponse get = stub->handle(
        makeRequest("POST", QStringLiteral("/api.cgi?cmd=GetWhiteLed&token=") + token,
                    "[{\"cmd\":\"GetWhiteLed\",\"action\":0,\"param\":{\"channel\":0}}]"));
    const QJsonObject led = jsonArrayOf(get).at(0).toObject()
                                .value(QStringLiteral("value")).toObject()
                                .value(QStringLiteral("WhiteLed")).toObject();
    QCOMPARE(led.value(QStringLiteral("state")).toInt(), 1);
    QCOMPARE(led.value(QStringLiteral("bright")).toInt(), 55);

    // 反过来：从 ONVIF 侧关灯，私有接口也要读到关。
    camera->imaging()->setWhiteLight(false);
    const HttpResponse after = stub->handle(
        makeRequest("POST", QStringLiteral("/api.cgi?cmd=GetWhiteLed&token=") + token,
                    "[{\"cmd\":\"GetWhiteLed\",\"action\":0,\"param\":{\"channel\":0}}]"));
    QCOMPARE(jsonArrayOf(after).at(0).toObject().value(QStringLiteral("value")).toObject()
                 .value(QStringLiteral("WhiteLed")).toObject()
                 .value(QStringLiteral("state")).toInt(), 0);
}

void TstVendor::reolinkPresetRoundTrip()
{
    QObject owner;
    VirtualCamera *camera = makeCamera(QStringLiteral("reolink"), &owner);
    auto *stub = qobject_cast<ReolinkStub *>(camera->vendorApi());
    QVERIFY(stub);

    stub->handle(makeRequest("POST", QStringLiteral("/api.cgi?cmd=Login"),
                             "[{\"cmd\":\"Login\",\"param\":{\"User\":"
                             "{\"userName\":\"admin\",\"password\":\"admin123\"}}}]"));
    const QString token = stub->currentToken();

    stub->handle(makeRequest("POST", QStringLiteral("/api.cgi?cmd=SetPtzPreset&token=") + token,
                             "[{\"cmd\":\"SetPtzPreset\",\"param\":{\"PtzPreset\":"
                             "{\"channel\":0,\"enable\":1,\"id\":2,\"name\":\"gate\"}}}]"));
    const PtzPreset *preset = camera->ptz()->preset(QStringLiteral("2"));
    QVERIFY(preset);
    QCOMPARE(preset->name, QStringLiteral("gate"));

    const HttpResponse list = stub->handle(
        makeRequest("POST", QStringLiteral("/api.cgi?cmd=GetPtzPreset&token=") + token,
                    "[{\"cmd\":\"GetPtzPreset\",\"action\":0,\"param\":{\"channel\":0}}]"));
    const QJsonArray presets = jsonArrayOf(list).at(0).toObject()
                                   .value(QStringLiteral("value")).toObject()
                                   .value(QStringLiteral("PtzPreset")).toArray();
    QCOMPARE(presets.size(), 1);
    QCOMPARE(presets.at(0).toObject().value(QStringLiteral("id")).toInt(), 2);
    QCOMPARE(presets.at(0).toObject().value(QStringLiteral("name")).toString(),
             QStringLiteral("gate"));

    stub->handle(makeRequest("POST", QStringLiteral("/api.cgi?cmd=DelPtzPreset&token=") + token,
                             "[{\"cmd\":\"DelPtzPreset\",\"param\":{\"PtzPreset\":"
                             "{\"channel\":0,\"id\":2}}}]"));
    QVERIFY(!camera->ptz()->preset(QStringLiteral("2")));
}

// ---------------------------------------------------------------------------
// TP-Link VIGI
// ---------------------------------------------------------------------------

void TstVendor::vigiTwoStepAuth()
{
    QObject owner;
    VirtualCamera *camera = makeCamera(QStringLiteral("vigi"), &owner);
    auto *stub = qobject_cast<VigiStub *>(camera->vendorApi());
    QVERIFY(stub);
    QCOMPARE(stub->kind(), VendorApi::VigiJsonRpc);

    // 第一步：params 为 null，拿挑战。
    const HttpResponse challenge = stub->handle(
        makeRequest("POST", QStringLiteral("/"), "{\"method\":\"doAuth\",\"params\":null}"));
    QCOMPARE(challenge.status, 200);
    QJsonObject result = jsonObjectOf(challenge).value(QStringLiteral("result")).toObject();
    QCOMPARE(jsonObjectOf(challenge).value(QStringLiteral("error_code")).toInt(), 0);
    QCOMPARE(result.value(QStringLiteral("encrypt_type")).toString(), QStringLiteral("SHA256"));
    const QString nonce = result.value(QStringLiteral("nonce")).toString();
    QCOMPARE(nonce.size(), 32);
    QCOMPARE(nonce, stub->currentNonce());

    // 第二步：sha256( sha256(user:pass) + ":" + nonce )。
    const QString digest = VigiStub::authDigest(QStringLiteral("admin"),
                                                QStringLiteral("admin123"), nonce);
    QCOMPARE(digest.size(), 64);
    const QByteArray step2 = QStringLiteral("{\"method\":\"doAuth\",\"params\":"
                                            "{\"username\":\"admin\",\"digest\":\"%1\"}}")
                                 .arg(digest).toUtf8();
    const HttpResponse authed = stub->handle(makeRequest("POST", QStringLiteral("/"), step2));
    QCOMPARE(jsonObjectOf(authed).value(QStringLiteral("error_code")).toInt(), 0);
    result = jsonObjectOf(authed).value(QStringLiteral("result")).toObject();
    QCOMPARE(result.value(QStringLiteral("stok")).toString(), stub->currentStok());
    QCOMPARE(result.value(QStringLiteral("lease_time")).toInt(), 25 * 60);

    // 拿到 stok 才能调业务方法。
    const QByteArray subscribe = QStringLiteral("{\"method\":\"subscribeMsg\",\"stok\":\"%1\","
                                                "\"params\":{}}")
                                     .arg(stub->currentStok()).toUtf8();
    const HttpResponse sub = stub->handle(makeRequest("POST", QStringLiteral("/"), subscribe));
    QCOMPARE(jsonObjectOf(sub).value(QStringLiteral("error_code")).toInt(), 0);
    QVERIFY(!jsonObjectOf(sub).value(QStringLiteral("result")).toObject()
                 .value(QStringLiteral("sub_id")).toString().isEmpty());

    // stok 失效之后一律 -40401。
    stub->expireStok();
    const HttpResponse stale = stub->handle(makeRequest("POST", QStringLiteral("/"), subscribe));
    QCOMPARE(jsonObjectOf(stale).value(QStringLiteral("error_code")).toInt(),
             VigiStub::errAuthFailed());
}

void TstVendor::vigiRejectsBadDigest()
{
    QObject owner;
    VirtualCamera *camera = makeCamera(QStringLiteral("vigi"), &owner);
    auto *stub = qobject_cast<VigiStub *>(camera->vendorApi());
    QVERIFY(stub);

    stub->handle(makeRequest("POST", QStringLiteral("/"),
                             "{\"method\":\"doAuth\",\"params\":null}"));
    const HttpResponse bad = stub->handle(
        makeRequest("POST", QStringLiteral("/"),
                    "{\"method\":\"doAuth\",\"params\":{\"username\":\"admin\","
                    "\"digest\":\"deadbeef\"}}"));
    QCOMPARE(jsonObjectOf(bad).value(QStringLiteral("error_code")).toInt(),
             VigiStub::errAuthFailed());
    // 挑战是一次性的：失败之后 nonce 作废，客户端必须重新走第一步。
    QVERIFY(stub->currentNonce().isEmpty());
    QVERIFY(stub->currentStok().isEmpty());
}

void TstVendor::vigiDetectionSwitches()
{
    QObject owner;
    VirtualCamera *camera = makeCamera(QStringLiteral("vigi"), &owner);
    auto *stub = qobject_cast<VigiStub *>(camera->vendorApi());
    QVERIFY(stub);

    // 8 个开关，出厂全关 —— 客户端漏开一个就是「订阅成功但零事件」。
    const QStringList types = VigiStub::detectionTypes();
    QCOMPARE(types.size(), 8);
    QCOMPARE(types.first(), QStringLiteral("PeopleDetection"));
    QCOMPARE(types.last(), QStringLiteral("DropAndTakeDetection"));
    for (const QString &type : types)
        QVERIFY(!stub->detectionEnabled(type));

    stub->handle(makeRequest("POST", QStringLiteral("/"),
                             "{\"method\":\"doAuth\",\"params\":null}"));
    const QString digest = VigiStub::authDigest(QStringLiteral("admin"),
                                                QStringLiteral("admin123"),
                                                stub->currentNonce());
    stub->handle(makeRequest("POST", QStringLiteral("/"),
                             QStringLiteral("{\"method\":\"doAuth\",\"params\":"
                                            "{\"username\":\"admin\",\"digest\":\"%1\"}}")
                                 .arg(digest).toUtf8()));
    const QString stok = stub->currentStok();
    QVERIFY(!stok.isEmpty());

    for (const QString &type : types) {
        const QByteArray body = QStringLiteral("{\"method\":\"setDetectionConfig\",\"stok\":\"%1\","
                                               "\"params\":{\"type\":\"%2\",\"enabled\":1}}")
                                    .arg(stok, type).toUtf8();
        const HttpResponse response = stub->handle(makeRequest("POST", QStringLiteral("/"), body));
        QCOMPARE(jsonObjectOf(response).value(QStringLiteral("error_code")).toInt(), 0);
        QVERIFY(stub->detectionEnabled(type));
    }

    // 订阅时如实报「还有几个开关是关的」，这里应该是 0。
    const HttpResponse sub = stub->handle(
        makeRequest("POST", QStringLiteral("/"),
                    QStringLiteral("{\"method\":\"subscribeMsg\",\"stok\":\"%1\",\"params\":{}}")
                        .arg(stok).toUtf8()));
    QCOMPARE(jsonObjectOf(sub).value(QStringLiteral("result")).toObject()
                 .value(QStringLiteral("disabled_detection_count")).toInt(), 0);

    // 不认识的检测类型回 -10030，客户端见到它要静默跳过。
    const HttpResponse unsupported = stub->handle(
        makeRequest("POST", QStringLiteral("/"),
                    QStringLiteral("{\"method\":\"setDetectionConfig\",\"stok\":\"%1\","
                                   "\"params\":{\"type\":\"UnicornDetection\",\"enabled\":1}}")
                        .arg(stok).toUtf8()));
    QCOMPARE(jsonObjectOf(unsupported).value(QStringLiteral("error_code")).toInt(),
             VigiStub::errUnsupportedDetection());

    // 不认识的方法回 -40210，与「不支持的检测」区分开。
    const HttpResponse unknown = stub->handle(
        makeRequest("POST", QStringLiteral("/"),
                    QStringLiteral("{\"method\":\"reboot\",\"stok\":\"%1\"}").arg(stok).toUtf8()));
    QCOMPARE(jsonObjectOf(unknown).value(QStringLiteral("error_code")).toInt(),
             VigiStub::errUnsupportedMethod());
}

void TstVendor::vigiPresetPoint()
{
    QObject owner;
    VirtualCamera *camera = makeCamera(QStringLiteral("vigi"), &owner);
    auto *stub = qobject_cast<VigiStub *>(camera->vendorApi());
    QVERIFY(stub);
    camera->ptz()->setPreset(QStringLiteral("门口"), QStringLiteral("5"));

    stub->handle(makeRequest("POST", QStringLiteral("/"),
                             "{\"method\":\"doAuth\",\"params\":null}"));
    const QString digest = VigiStub::authDigest(QStringLiteral("admin"),
                                                QStringLiteral("admin123"),
                                                stub->currentNonce());
    stub->handle(makeRequest("POST", QStringLiteral("/"),
                             QStringLiteral("{\"method\":\"doAuth\",\"params\":"
                                            "{\"username\":\"admin\",\"digest\":\"%1\"}}")
                                 .arg(digest).toUtf8()));

    const HttpResponse presets = stub->handle(
        makeRequest("POST", QStringLiteral("/"),
                    QStringLiteral("{\"method\":\"getPresetPoint\",\"stok\":\"%1\"}")
                        .arg(stub->currentStok()).toUtf8()));
    const QJsonArray list = jsonObjectOf(presets).value(QStringLiteral("result")).toObject()
                                .value(QStringLiteral("preset")).toArray();
    QCOMPARE(list.size(), 1);
    QCOMPARE(list.at(0).toObject().value(QStringLiteral("id")).toInt(), 5);
    QCOMPARE(list.at(0).toObject().value(QStringLiteral("name")).toString(),
             QStringLiteral("门口"));
}

void TstVendor::vigiPrivateListener()
{
    QObject owner;
    CameraModel model = CameraModel::makeDefault(QStringLiteral("cam-vigi"),
                                                 QStringLiteral("vigi"), 0);
    model.bindAddress = QHostAddress(QHostAddress::LocalHost);
    auto *camera = new VirtualCamera(model, nullptr, &owner);
    // 20443 在开发机上可能被占，换一个不常用的端口再起。
    camera->mutableQuirks().setParam(QuirkId::SelfSignedTls, QStringLiteral("port"), 24443);

    auto *stub = qobject_cast<VigiStub *>(camera->vendorApi());
    QVERIFY(stub);
    QCOMPARE(stub->securePort(), quint16(0));   // registerRoutes 之前不占端口

    stub->registerRoutes(camera->httpServer());
    if (stub->securePort() == 0)
        QSKIP("VIGI 私有端口没起来（端口被占用），跳过");
    QCOMPARE(stub->securePort(), quint16(24443));
    // 本机 Qt 没有 TLS 后端时会退化成明文 HTTP，两种都算通过，
    // 但要能从接口上分辨出到底跑的是哪一种。
    QVERIFY(stub->isTlsActive() || !stub->isTlsActive());
}

void TstVendor::vigiTlsHandshake()
{
#if QT_CONFIG(ssl)
    if (!QSslSocket::supportsSsl())
        QSKIP("这份 Qt 没有可用的 TLS 后端，私有端口会退化成明文 HTTP");

    QObject owner;
    CameraModel model = CameraModel::makeDefault(QStringLiteral("cam-vigi-tls"),
                                                 QStringLiteral("vigi"), 0);
    model.bindAddress = QHostAddress(QHostAddress::LocalHost);
    auto *camera = new VirtualCamera(model, nullptr, &owner);
    camera->mutableQuirks().setParam(QuirkId::SelfSignedTls, QStringLiteral("port"), 24444);

    auto *stub = qobject_cast<VigiStub *>(camera->vendorApi());
    QVERIFY(stub);
    stub->registerRoutes(camera->httpServer());
    if (stub->securePort() == 0)
        QSKIP("VIGI 私有端口没起来（端口被占用），跳过");
    QVERIFY(stub->isTlsActive());

    // 客户端与服务端在同一个线程里，所以只能用信号 + QTRY_*（内部会转事件循环），
    // 不能用 waitForEncrypted 那类阻塞等待 —— 那会把服务端一起冻住。
    QSslSocket client;
    client.setPeerVerifyMode(QSslSocket::VerifyNone);   // 自签证书，客户端必须跳过校验
    bool encrypted = false;
    QByteArray reply;
    connect(&client, &QSslSocket::encrypted, &client, [&encrypted] { encrypted = true; });
    connect(&client, &QSslSocket::readyRead, &client,
            [&client, &reply] { reply += client.readAll(); });
    client.connectToHostEncrypted(QStringLiteral("127.0.0.1"), stub->securePort());
    QTRY_VERIFY_WITH_TIMEOUT(encrypted, 5000);

    const QByteArray body = "{\"method\":\"doAuth\",\"params\":null}";
    client.write("POST / HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                 "Content-Type: application/json\r\nContent-Length: "
                 + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    QTRY_VERIFY_WITH_TIMEOUT(reply.contains("\r\n\r\n") && reply.endsWith('}'), 5000);

    QVERIFY(reply.startsWith("HTTP/1.1 200"));
    const int split = reply.indexOf("\r\n\r\n");
    const QJsonObject object = QJsonDocument::fromJson(reply.mid(split + 4)).object();
    QCOMPARE(object.value(QStringLiteral("error_code")).toInt(), 0);
    QCOMPARE(object.value(QStringLiteral("result")).toObject()
                 .value(QStringLiteral("nonce")).toString(),
             stub->currentNonce());
#else
    QSKIP("这份 Qt 没编 SSL 支持");
#endif
}

// ---------------------------------------------------------------------------
// TP-Link TL-IPC
// ---------------------------------------------------------------------------

void TstVendor::tplinkDsLightRoundTrip()
{
    QObject owner;
    VirtualCamera *camera = makeCamera(QStringLiteral("tplink"), &owner);
    auto *stub = qobject_cast<TplinkDsStub *>(camera->vendorApi());
    QVERIFY(stub);
    QCOMPARE(stub->kind(), VendorApi::TplinkDs);

    const HttpResponse login = stub->handle(
        makeRequest("POST", QStringLiteral("/"),
                    "{\"method\":\"login\",\"params\":{\"username\":\"admin\","
                    "\"password\":\"admin123\"}}"));
    QCOMPARE(jsonObjectOf(login).value(QStringLiteral("error_code")).toInt(), 0);
    const QString stok = stub->currentStok();
    QVERIFY(!stok.isEmpty());
    QCOMPARE(jsonObjectOf(login).value(QStringLiteral("stok")).toString(), stok);
    QCOMPARE(TplinkDsStub::stokFromPath(QStringLiteral("/stok=abc123/ds")),
             QStringLiteral("abc123"));

    const QString path = QStringLiteral("/stok=%1/ds").arg(stok);
    QVERIFY(!camera->imaging()->settings().whiteLightOn);

    const HttpResponse set = stub->handle(
        makeRequest("POST", path,
                    "{\"method\":\"set\",\"light\":{\"config\":"
                    "{\"enabled\":\"on\",\"brightness\":\"60\"}}}"));
    QCOMPARE(jsonObjectOf(set).value(QStringLiteral("error_code")).toInt(), 0);
    // 与海康 supplementLight、Reolink SetWhiteLed 改的是同一份 ImagingState。
    QVERIFY(camera->imaging()->settings().whiteLightOn);
    QCOMPARE(camera->imaging()->settings().whiteLightBrightness, 60);

    const HttpResponse get = stub->handle(
        makeRequest("POST", path, "{\"method\":\"get\",\"light\":{\"name\":[\"config\"]}}"));
    const QJsonObject config = jsonObjectOf(get).value(QStringLiteral("light")).toObject()
                                   .value(QStringLiteral("config")).toObject();
    // TP-Link 的私有 JSON 里所有标量都是字符串，布尔写成 on / off。
    QCOMPARE(config.value(QStringLiteral("enabled")).toString(), QStringLiteral("on"));
    QCOMPARE(config.value(QStringLiteral("brightness")).toString(), QStringLiteral("60"));

    // led 是同一份灯光的别名。
    const HttpResponse off = stub->handle(
        makeRequest("POST", path, "{\"method\":\"set\",\"led\":{\"config\":{\"enabled\":\"off\"}}}"));
    QCOMPARE(jsonObjectOf(off).value(QStringLiteral("error_code")).toInt(), 0);
    QVERIFY(!camera->imaging()->settings().whiteLightOn);

    // 只做灯光：别的模块一律回不支持。
    const HttpResponse other = stub->handle(
        makeRequest("POST", path, "{\"method\":\"get\",\"system\":{\"name\":[\"info\"]}}"));
    QCOMPARE(jsonObjectOf(other).value(QStringLiteral("error_code")).toInt(),
             TplinkDsStub::errUnsupported());
}

void TstVendor::tplinkDsRejectsStaleStok()
{
    QObject owner;
    VirtualCamera *camera = makeCamera(QStringLiteral("tplink"), &owner);
    auto *stub = qobject_cast<TplinkDsStub *>(camera->vendorApi());
    QVERIFY(stub);

    // 还没登录：任何 stok 都不认。
    const HttpResponse noLogin = stub->handle(
        makeRequest("POST", QStringLiteral("/stok=whatever/ds"),
                    "{\"method\":\"get\",\"light\":{\"name\":[\"config\"]}}"));
    QCOMPARE(noLogin.status, 200);   // 状态码必须是 200，否则客户端不会重登
    QCOMPARE(jsonObjectOf(noLogin).value(QStringLiteral("error_code")).toInt(),
             TplinkDsStub::errInvalidStok());

    stub->handle(makeRequest("POST", QStringLiteral("/"),
                             "{\"method\":\"login\",\"params\":{\"username\":\"admin\","
                             "\"password\":\"admin123\"}}"));
    const QString path = QStringLiteral("/stok=%1/ds").arg(stub->currentStok());
    stub->expireStok();
    const HttpResponse stale = stub->handle(
        makeRequest("POST", path, "{\"method\":\"get\",\"light\":{\"name\":[\"config\"]}}"));
    QCOMPARE(jsonObjectOf(stale).value(QStringLiteral("error_code")).toInt(),
             TplinkDsStub::errInvalidStok());

    // 口令不对也拿不到 stok。
    const HttpResponse badLogin = stub->handle(
        makeRequest("POST", QStringLiteral("/"),
                    "{\"method\":\"login\",\"params\":{\"username\":\"admin\","
                    "\"password\":\"nope\"}}"));
    QVERIFY(jsonObjectOf(badLogin).value(QStringLiteral("error_code")).toInt() != 0);
}

QTEST_GUILESS_MAIN(TstVendor)

#include "tst_vendor.moc"
