#pragma once

// TP-Link VIGI 私有 JSON-RPC 桩。
//
// 与别家不同的一点：它不在 ONVIF 那个端口上，而是自己开一个 20443 的
// 自签 HTTPS（quirk F2 `transport.self_signed_tls`，端口取该 quirk 的 port 参数）。
// ONVIF 仍留在原端口不动 —— 客户端是「ONVIF + OpenAPI 双源」的用法：
// 厂商识别与连续 PTZ 走 ONVIF，检测开关 / 订阅 / 预置位走这里。
//
// Qt 6.2 没有 QSslServer（6.4 才有），所以这里自己拿 QTcpServer 起监听、
// 在 incomingConnection 里把 socket 描述符交给 QSslSocket 并
// startServerEncryption()，再手写一层极小的 HTTP/1.1 解析。
// 这个头刻意不引 QtNetwork 的 SSL 头：没编 SSL 支持的 Qt 上，
// QSslCertificate / QSslKey 是编不过的，相关代码全部关在 .cpp 的条件编译里。
//
// 证书是编译期内嵌的一份自签 PEM，**只用于本模拟器的测试**，
// 私钥公开在源码里，绝不可用于任何真实场景。

#include "net/HttpTypes.h"
#include "vendor/VendorApiStub.h"

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtCore/QStringList>

QT_BEGIN_NAMESPACE
class QTcpServer;
class QTcpSocket;
QT_END_NAMESPACE

namespace onvifsim {

class VigiStub : public VendorApiStub
{
    Q_OBJECT
public:
    explicit VigiStub(VirtualCamera *camera, QObject *parent = nullptr);
    ~VigiStub() override;

    void registerRoutes(HttpServer *server) override;
    VendorApi kind() const override;

    // 同步处理一条 JSON-RPC 请求；单测与两条传输（TLS / 明文回退）共用它。
    HttpResponse handle(const HttpRequest &request);

    // ---- 私有端口的状态 ----
    quint16 securePort() const;     // 实际监听到的端口，0 表示没起来
    bool isTlsActive() const;       // true = 真的在跑 TLS，false = 退化成明文 HTTP

    // ---- 两步挑战 ----
    // 第二步的应答摘要：sha256( sha256(user:password) + ":" + nonce )，全小写十六进制。
    static QString authDigest(const QString &username, const QString &password,
                              const QString &nonce);
    QString currentNonce() const;
    QString currentStok() const;
    void expireStok();

    // ---- 8 个检测开关 ----
    // 客户端在 subscribeMsg 之前必须逐个打开，漏一个的表现是「订阅成功但零事件」。
    static QStringList detectionTypes();
    bool detectionEnabled(const QString &type) const;

    // ---- 错误码 ----
    static int errAuthFailed();             // -40401
    static int errUnsupportedMethod();      // -40210
    static int errBadParam();               // -40209
    static int errUnsupportedDetection();   // -10030，客户端见到它要静默跳过

private:
    QJsonObject dispatch(const QJsonObject &request, const QString &stok);
    QJsonObject methodDoAuth(const QJsonObject &request);
    QJsonObject methodSubscribeMsg(const QJsonObject &params);
    QJsonObject methodGetDetection(const QString &type) const;
    QJsonObject methodSetDetection(const QString &type, const QJsonObject &params);
    QJsonObject methodGetPresetPoint() const;
    QJsonObject methodMotorMove(const QJsonObject &params);
    QJsonObject methodGetDeviceStatus() const;

    QString issueNonce();
    QString issueStok();
    bool stokAccepted(const QString &stok) const;

    quint16 desiredPrivatePort() const;
    void startPrivateListener();
    void stopPrivateListener();
    void acceptDescriptor(qintptr descriptor);
    void onSocketReadyRead(QTcpSocket *socket);

    QTcpServer *m_server = nullptr;
    bool m_tlsActive = false;
    QHash<QObject *, QByteArray> m_buffers;   // 每条连接的收包缓冲

    QString m_nonce;
    QString m_stok;
    QDateTime m_stokExpiry;
    QMap<QString, bool> m_detections;
    int m_subscriptionSeq = 0;
    bool m_routesRegistered = false;
};

} // namespace onvifsim
