#pragma once

// 一条 RTSP 会话：一个客户端连接上的若干条已 SETUP 的轨道。
// interleaved 模式下对讲 RTP 从同一个 TCP 连接的 channel 0 进来，
// 由本类拆出后喂给 RtpReceiver。

#include "media/MediaTypes.h"
#include "rtsp/RtspTypes.h"

#include <QtCore/QDateTime>
#include <QtCore/QObject>
#include <QtCore/QVector>

class QTcpSocket;

namespace onvifsim {

class RtpReceiver;
class RtpSender;
class RtspServer;
class VirtualCamera;

enum class RtspTrackKind { Video, Audio, Backchannel };

struct RtspTrack {
    RtspTrackKind kind = RtspTrackKind::Video;
    int index = 0;
    RtspTransport transport;
    RtpSender *sender = nullptr;     // Backchannel 轨没有 sender
    bool playing = false;
};

class RtspSession : public QObject
{
    Q_OBJECT
public:
    RtspSession(RtspServer *server, QTcpSocket *socket, QObject *parent = nullptr);
    ~RtspSession() override;

    QByteArray sessionId() const;
    QString profileToken() const;
    QString peerString() const;
    QDateTime startedAt() const;
    QDateTime lastActivity() const;
    bool isPlaying() const;
    bool hasBackchannel() const;

    QVector<RtspTrack> tracks() const;
    RtpReceiver *backchannelReceiver() const;

    void teardown();
    void touch();

signals:
    void playingChanged(bool playing);
    void endedByPeer();
    void backchannelDataReceived(qint64 bytes);

private:
    struct Private;
    Private *d;
};

} // namespace onvifsim
