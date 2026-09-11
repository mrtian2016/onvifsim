#pragma once

// REST 控制面，默认 127.0.0.1:9000。
// e2e 测试、CI、GUI 之外的自动化都走这里。附 OpenAPI JSON 与 Prometheus /metrics。
//
//   GET    /api/cameras                     列表与状态
//   POST   /api/cameras                     从预设创建
//   PATCH  /api/cameras/{id}                改属性、quirks、上下线
//   DELETE /api/cameras/{id}
//   POST   /api/cameras/{id}/events         触发 topic（state、duration）
//   POST   /api/cameras/{id}/ptz            移动 / 预置位
//   GET    /api/cameras/{id}/sessions       RTSP 会话、订阅、HTTP 客户端
//   GET    /api/cameras/{id}/talkback       对讲接收统计
//   POST   /api/cameras/{id}/offline?seconds=
//   GET|POST /api/scenario                  导出 / 加载场景
//   GET|POST /api/network                   网卡、网络模式、IP 别名
//   GET    /api/quirks                      全部开关的元数据
//   GET    /api/log                         SSE 日志流
//   GET    /metrics                         Prometheus 文本
//   GET    /openapi.json

#include <QtCore/QMap>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QHostAddress>

class QJsonObject;
class QTcpSocket;

namespace onvifsim {

class Simulator;
class VirtualCamera;

class ControlApi : public QObject
{
    Q_OBJECT
public:
    explicit ControlApi(Simulator *simulator, QObject *parent = nullptr);
    ~ControlApi() override;

    bool start(const QHostAddress &address, quint16 port, QString *errorOut = nullptr);
    void stop();
    bool isRunning() const;
    quint16 port() const;

    // 非空时要求请求带 Authorization: Bearer <token> 或 ?token=
    void setToken(const QString &token);
    QString token() const;

    int sseClientCount() const;

signals:

private:
    // handleRequest 只负责「记时间 → 交给 routeRequest → 统一收尾（发信号 + 写日志）」。
    // 路由本身在 routeRequest 里，它有二十多个 return 分支 —— 收尾逻辑要是跟着
    // 散在每个分支上，就会出现「某条路径忘了发信号」和「status 变量与实际发出去的
    // 状态码对不上」这两类问题，之前两样都有。
    void handleRequest(QTcpSocket *socket, const QByteArray &method, const QString &path,
                       const QUrlQuery &query, const QMap<QByteArray, QByteArray> &headers,
                       const QByteArray &body);
    void routeRequest(QTcpSocket *socket, const QByteArray &method, const QString &path,
                      const QUrlQuery &query, const QMap<QByteArray, QByteArray> &headers,
                      const QByteArray &body);

    // 每个 handler 返回 true 表示「这一段接手并已应答」，false 表示不归它管。
    // routeRequest 只负责按顺序问下去，最后没人接手就 404。
    bool handleMetrics(QTcpSocket *socket, const QString &path);
    bool handleOpenApi(QTcpSocket *socket, const QString &path);
    bool handleLogStream(QTcpSocket *socket, const QString &section);
    bool handleQuirks(QTcpSocket *socket, const QString &section);
    bool handlePresets(QTcpSocket *socket, const QString &section);
    bool handleNetwork(QTcpSocket *socket, const QByteArray &method, const QString &section,
                       const QByteArray &body);
    bool handleScenario(QTcpSocket *socket, const QByteArray &method, const QString &section,
                        const QByteArray &body);
    bool handleCameras(QTcpSocket *socket, const QByteArray &method, const QString &path,
                       const QStringList &segments, const QUrlQuery &query,
                       const QByteArray &body);
    bool handleCameraCollection(QTcpSocket *socket, const QByteArray &method,
                                const QByteArray &body);
    bool handleCameraItem(QTcpSocket *socket, const QByteArray &method, const QString &path,
                          VirtualCamera *camera, const QByteArray &body);
    bool handleCameraSubResource(QTcpSocket *socket, const QByteArray &method,
                                 const QString &path, const QString &sub,
                                 VirtualCamera *camera, const QUrlQuery &query,
                                 const QByteArray &body);

    // 三个各处都要的小应答。原来是 routeRequest 里的 lambda，拆函数之后
    // 提到这里，免得每个 handler 各抄一份。
    void sendNotFound(QTcpSocket *socket, const QString &path);
    void sendBadRequest(QTcpSocket *socket, const QString &why);
    bool parseJsonBody(QTcpSocket *socket, const QByteArray &body, QJsonObject *out);

    struct Private;
    Private *d;
};

} // namespace onvifsim
