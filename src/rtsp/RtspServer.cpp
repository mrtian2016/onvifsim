#include "rtsp/RtspServer.h"

#include "core/LogBus.h"
#include "core/Quirks.h"
#include "core/VirtualCamera.h"
#include "rtsp/RtspInternal_p.h"
#include "rtsp/RtspSession.h"

#include <QtCore/QDateTime>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

namespace onvifsim {
namespace {

// 超时巡检的粒度。会话超时是 60s 量级，1s 一扫足够，也不至于空转太频繁。
constexpr int kSweepIntervalMs = 1000;

} // namespace

struct RtspServer::Private {
    RtspServer *q = nullptr;
    VirtualCamera *camera = nullptr;
    // nonce 表按相机共享，不按连接。真机的 nonce 是设备级的：同一个 nonce 换条连接
    // 照样认 —— quirk AuthNonceStrictOnce（重放即 401）只有共享时才复现得出来。
    HttpAuth auth;
    QTcpServer *listener = nullptr;
    QList<RtspSession *> sessions;

    int sessionTimeout = 60;
    QHostAddress bindAddress = QHostAddress(QHostAddress::AnyIPv4);
    quint16 desiredPort = 0;
    bool suspended = false;

    QTimer *sweepTimer = nullptr;
    QDateTime backchannelBusyUntil;
    QDateTime lastPeriodicTeardown;

    LogBus *log() const { return camera ? camera->logBus() : nullptr; }
    QString cameraId() const { return camera ? camera->id() : QString(); }

    void adopt(QTcpSocket *socket);
    void removeSession(RtspSession *session);
    void sweep();
};

void RtspServer::Private::adopt(QTcpSocket *socket)
{
    RtspSession *session = new RtspSession(q, socket, q);
    sessions.append(session);

    QObject::connect(session, &RtspSession::endedByPeer, q,
                     [this, session] { removeSession(session); });
    // 对讲有数据进来就冒个泡，GUI 的电平表与 REST 的统计都靠它刷新。
    QObject::connect(session, &RtspSession::backchannelDataReceived, q,
                     [this](qint64) { emit q->talkbackActivity(); });
    QObject::connect(session, &RtspSession::playingChanged, q,
                     [this](bool) { emit q->sessionCountChanged(sessions.size()); });

    emit q->sessionCountChanged(sessions.size());
}

void RtspServer::Private::removeSession(RtspSession *session)
{
    if (!sessions.removeOne(session))
        return;
    // 信号可能是从 session 自己的槽里发出来的，不能就地 delete。
    session->deleteLater();
    emit q->sessionCountChanged(sessions.size());
}

void RtspServer::Private::sweep()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();

    // 会话超时回收：客户端不 keepalive（GET_PARAMETER / OPTIONS）就当它已经死了。
    // 不回收的话，客户端异常退出留下的会话会一直占着并发槽位。
    const QList<RtspSession *> snapshot = sessions;
    for (RtspSession *session : snapshot) {
        const QDateTime last = session->lastActivity();
        if (!last.isValid() || last.secsTo(now) <= sessionTimeout)
            continue;
        if (LogBus *bus = log()) {
            bus->info(logcat::Rtsp, cameraId(),
                      QStringLiteral("Session %1 reclaimed on timeout (%2 s idle)")
                          .arg(QString::fromLatin1(session->sessionId()))
                          .arg(sessionTimeout));
        }
        session->teardown();
    }

    // quirk：每 N 秒相机自己 TEARDOWN，客户端必须能自动重连。
    if (VirtualCamera *cam = camera; cam && cam->quirks().isEnabled(QuirkId::RtspPeriodicTeardown)) {
        const int seconds =
            cam->quirks().paramInt(QuirkId::RtspPeriodicTeardown, QStringLiteral("seconds"));
        if (!lastPeriodicTeardown.isValid()) {
            lastPeriodicTeardown = now;
        } else if (seconds > 0 && lastPeriodicTeardown.secsTo(now) >= seconds) {
            lastPeriodicTeardown = now;
            if (!sessions.isEmpty()) {
                if (LogBus *bus = log()) {
                    LogRecord record;
                    record.level = LogLevel::Warning;
                    record.cameraId = cameraId();
                    record.category = QString::fromLatin1(logcat::Rtsp);
                    record.summary = QStringLiteral("周期性主动断流，踢掉 %1 条会话")
                                         .arg(sessions.size());
                    record.quirkKey = QStringLiteral("rtsp.periodic_teardown");
                    bus->post(record);
                }
                q->teardownAll();
            }
        }
    }
}

RtspServer::RtspServer(VirtualCamera *camera, QObject *parent)
    : QObject(parent), d(new Private)
{
    d->q = this;
    d->camera = camera;

    d->sweepTimer = new QTimer(this);
    d->sweepTimer->setInterval(kSweepIntervalMs);
    connect(d->sweepTimer, &QTimer::timeout, this, [this] { d->sweep(); });
}

RtspServer::~RtspServer()
{
    close();
    delete d;
}

VirtualCamera *RtspServer::camera() const
{
    return d->camera;
}

HttpAuth *RtspServer::auth() const
{
    return &d->auth;
}

bool RtspServer::listen(const QHostAddress &address, quint16 port, QString *errorOut)
{
    close();

    d->listener = new QTcpServer(this);
    connect(d->listener, &QTcpServer::newConnection, this, [this] {
        while (d->listener && d->listener->hasPendingConnections())
            d->adopt(d->listener->nextPendingConnection());
    });

    if (!d->listener->listen(address, port)) {
        if (errorOut)
            *errorOut = d->listener->errorString();
        delete d->listener;
        d->listener = nullptr;
        return false;
    }

    d->bindAddress = address;
    d->desiredPort = d->listener->serverPort();
    d->suspended = false;
    d->sweepTimer->start();
    return true;
}

void RtspServer::close()
{
    teardownAll();
    d->sweepTimer->stop();
    if (d->listener) {
        d->listener->close();
        delete d->listener;
        d->listener = nullptr;
    }
}

bool RtspServer::isListening() const
{
    return d->listener && d->listener->isListening();
}

quint16 RtspServer::serverPort() const
{
    return d->listener && d->listener->isListening() ? d->listener->serverPort() : d->desiredPort;
}

QList<RtspSession *> RtspServer::sessions() const
{
    return d->sessions;
}

int RtspServer::sessionCount() const
{
    return int(d->sessions.size());
}

void RtspServer::teardownAll()
{
    const QList<RtspSession *> snapshot = d->sessions;
    for (RtspSession *session : snapshot)
        session->teardown();
}

void RtspServer::teardownSession(const QByteArray &sessionId)
{
    const QList<RtspSession *> snapshot = d->sessions;
    for (RtspSession *session : snapshot) {
        if (session->sessionId() == sessionId)
            session->teardown();
    }
}

int RtspServer::sessionTimeout() const
{
    return d->sessionTimeout;
}

bool RtspServer::backchannelBusy() const
{
    VirtualCamera *camera = d->camera;
    if (!camera || !camera->quirks().isEnabled(QuirkId::TalkbackBusySlot))
        return false;
    return d->backchannelBusyUntil.isValid()
        && QDateTime::currentDateTimeUtc() < d->backchannelBusyUntil;
}

void RtspServer::markBackchannelBusy()
{
    VirtualCamera *camera = d->camera;
    if (!camera || !camera->quirks().isEnabled(QuirkId::TalkbackBusySlot))
        return;
    const int seconds =
        camera->quirks().paramInt(QuirkId::TalkbackBusySlot, QStringLiteral("seconds"));
    d->backchannelBusyUntil = QDateTime::currentDateTimeUtc().addSecs(seconds);
    if (LogBus *bus = d->log()) {
        LogRecord record;
        record.level = LogLevel::Info;
        record.cameraId = d->cameraId();
        record.category = QString::fromLatin1(logcat::Rtsp);
        record.summary = QStringLiteral("对讲槽位进入忙状态 %1 秒，期间新的 backchannel "
                                        "DESCRIBE 一律 401").arg(seconds);
        record.quirkKey = QStringLiteral("rtsp.talkback_busy_slot");
        bus->post(record);
    }
}

void RtspServer::suspend()
{
    if (!d->listener)
        return;
    teardownAll();
    d->listener->close();
    d->sweepTimer->stop();
    d->suspended = true;
}

bool RtspServer::resume(QString *errorOut)
{
    if (!d->suspended)
        return true;
    // 端口保持不变：客户端拿到的 XAddr / StreamUri 在离线期间不该失效。
    return listen(d->bindAddress, d->desiredPort, errorOut);
}

QString RtspServer::profileTokenForPath(const QString &path) const
{
    VirtualCamera *camera = d->camera;
    if (!camera)
        return QString();

    const QString wanted = rtspInternal::normalizeStreamPath(path);
    const CameraModel &model = camera->model();
    for (const MediaProfile &profile : model.profiles) {
        // streamPath 为空时 VirtualCamera::streamUri 会生成 /profile<token>，这里对齐。
        const QString candidate = profile.streamPath.isEmpty()
            ? QStringLiteral("/profile%1").arg(profile.token)
            : profile.streamPath;
        if (rtspInternal::normalizeStreamPath(candidate).compare(wanted, Qt::CaseInsensitive) == 0)
            return profile.token;
    }
    // 兜底：有的客户端不认 GetStreamUri，直接拿 profile token 自己拼路径。
    for (const MediaProfile &profile : model.profiles) {
        if (wanted.endsWith(QLatin1Char('/') + profile.token, Qt::CaseInsensitive))
            return profile.token;
    }
    return QString();
}
} // namespace onvifsim
