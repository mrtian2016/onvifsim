#pragma once

// 每台相机一个 RTSP 服务器：主/子/三码流 + 对讲 backchannel。
//
// RTSP 1.0：OPTIONS / DESCRIBE / SETUP / PLAY / PAUSE / GET_PARAMETER（keepalive）/
// SET_PARAMETER / TEARDOWN。会话超时默认 60s，不 keepalive 就回收。
// 鉴权 Basic / Digest，与 HTTP 共用用户表和挑战变体（E8 / E9）。

#include "net/HttpAuth.h"
#include "rtsp/RtspTypes.h"

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtNetwork/QHostAddress>

namespace onvifsim {

class RtspSession;
class VirtualCamera;

class RtspServer : public QObject
{
    Q_OBJECT
public:
    explicit RtspServer(VirtualCamera *camera, QObject *parent = nullptr);
    ~RtspServer() override;

    // 会话要拿它做鉴权、读 model / quirks / 样片。
    VirtualCamera *camera() const;

    // 鉴权状态按相机共享，不是每条连接一份：真机的 nonce 是设备级的，
    // 同一个 nonce 在不同连接上都认 —— quirk AuthNonceStrictOnce
    // （nonce 严格一次性、重放即 401）只有在共享时才有意义。
    HttpAuth *auth() const;

    bool listen(const QHostAddress &address, quint16 port, QString *errorOut = nullptr);
    void close();
    bool isListening() const;
    quint16 serverPort() const;

    QList<RtspSession *> sessions() const;
    // 已建立的 TCP 连接数（含还没 SETUP 的）。453 的并发上限另按「已 SETUP 的会话」判，
    // 两者刻意不同：连上来还没取流的客户端不该占掉别人的码流槽位。
    int sessionCount() const;
    void teardownAll();
    void teardownSession(const QByteArray &sessionId);

    int sessionTimeout() const;

    // quirk E7：TEARDOWN 后一段时间内新的 backchannel DESCRIBE 一律 401。
    bool backchannelBusy() const;
    void markBackchannelBusy();

    void suspend();
    // 返回 false 表示端口没抢回来 —— 调用方必须看，否则相机会对外
    // 报「在线」但其实一个端口都没绑上。
    bool resume(QString *errorOut = nullptr);

    // 按路径找 profile（路径风格由 Persona 决定）。
    QString profileTokenForPath(const QString &path) const;

signals:
    void sessionCountChanged(int count);
    void talkbackActivity();

private:
    struct Private;
    Private *d;
};

} // namespace onvifsim
