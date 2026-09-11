#pragma once

// 每台相机一个 HTTP 服务器：SOAP 各服务 + 快照 + 厂商私有 API + 订阅管理器路径。
//
// HTTP/1.1，keep-alive，POST / GET / PUT，body 上限 1 MB，
// 不支持 chunked 请求体（回 411）。所有故障注入钩子都在响应侧，
// 由 SoapDispatcher / 各 handler 填进 HttpResponse。

#include "net/HttpTypes.h"

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtNetwork/QHostAddress>

namespace onvifsim {

class VirtualCamera;

class HttpServer : public QObject
{
    Q_OBJECT
public:
    explicit HttpServer(VirtualCamera *camera, QObject *parent = nullptr);
    ~HttpServer() override;

    bool listen(const QHostAddress &address, quint16 port, QString *errorOut = nullptr);
    void close();
    bool isListening() const;
    QHostAddress serverAddress() const;
    quint16 serverPort() const;

    // 精确路径路由。method 传空表示不限方法。
    void addRoute(const QByteArray &method, const QString &path, HttpHandler handler);
    // 前缀路由，用于订阅管理器（/event-1024_1024...）与厂商 API 的可变路径。
    void addPrefixRoute(const QByteArray &method, const QString &prefix, HttpHandler handler);
    void clearRoutes();
    void setFallbackHandler(HttpHandler handler);

    int connectionCount() const;
    void setMaxBodySize(qint64 bytes);

    // 离线模拟：关掉监听但保留路由，恢复时不必重新注册。
    void suspend();
    // 返回 false 表示端口没抢回来 —— 调用方必须看，否则相机会对外
    // 报「在线」但其实一个端口都没绑上。
    bool resume(QString *errorOut = nullptr);
    bool isSuspended() const;

signals:
    void connectionCountChanged(int count);
    void requestReceived(const onvifsim::HttpRequest &request);

private:
    struct Private;
    Private *d;
};

} // namespace onvifsim
