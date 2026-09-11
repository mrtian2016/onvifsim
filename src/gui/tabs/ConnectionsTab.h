#pragma once

// 连接 Tab：RTSP 会话 + HTTP 客户端。
//
// HTTP 那张表是靠 HttpServer::requestReceived 攒出来的（GUI 只观察信号，
// 不碰任何协议解析）。它按相机分开记，切走再切回来记录还在 ——
// 排查「到底是哪个客户端在狂刷 GetCapabilities」时这点很要紧。

#include "gui/tabs/CameraTab.h"

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QMap>

class QLabel;
class QPushButton;
class QTableWidget;

namespace onvifsim {

struct HttpRequest;

namespace gui {

class ConnectionsTab : public CameraTab
{
    Q_OBJECT
public:
    explicit ConnectionsTab(Simulator *simulator, QWidget *parent = nullptr);

protected:
    void bindCamera() override;
    void refresh() override;

private:
    struct HttpClientInfo {
        QString address;
        QString userAgent;
        qint64 requests = 0;
        QDateTime lastSeen;
        QString lastPath;
    };

    void watchCamera(VirtualCamera *camera);
    void recordRequest(const QString &cameraId, const HttpRequest &request);

    QTableWidget *m_sessions = nullptr;
    QTableWidget *m_clients = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_teardown = nullptr;
    QPushButton *m_teardownAll = nullptr;
    QPushButton *m_clearClients = nullptr;

    // cameraId → 客户端地址 → 统计
    QHash<QString, QMap<QString, HttpClientInfo>> m_clientsByCamera;
};

} // namespace gui
} // namespace onvifsim
