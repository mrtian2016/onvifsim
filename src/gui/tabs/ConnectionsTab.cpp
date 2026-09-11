#include "gui/tabs/ConnectionsTab.h"

#include "core/Simulator.h"
#include "core/VirtualCamera.h"
#include "gui/GuiUtil.h"
#include "net/HttpServer.h"
#include "rtsp/RtspServer.h"
#include "rtsp/RtspSession.h"

#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

namespace onvifsim {
namespace gui {

namespace {

QString trackSummary(const QVector<RtspTrack> &tracks)
{
    int video = 0;
    int audio = 0;
    int backchannel = 0;
    for (const RtspTrack &track : tracks) {
        switch (track.kind) {
        case RtspTrackKind::Video:
            ++video;
            break;
        case RtspTrackKind::Audio:
            ++audio;
            break;
        case RtspTrackKind::Backchannel:
            ++backchannel;
            break;
        }
    }
    return ConnectionsTab::tr("视频 %1 · 音频 %2 · 对讲 %3").arg(video).arg(audio).arg(backchannel);
}

} // namespace

ConnectionsTab::ConnectionsTab(Simulator *simulator, QWidget *parent)
    : CameraTab(simulator, parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    layout->addWidget(splitter);

    // ---- RTSP 会话 ----
    auto *sessionBox = new QGroupBox(tr("RTSP 会话"), splitter);
    auto *sessionLayout = new QVBoxLayout(sessionBox);
    m_sessions = new QTableWidget(0, 7, sessionBox);
    m_sessions->setHorizontalHeaderLabels({ tr("会话 id"), tr("客户端"), tr("Profile"),
                                            tr("轨道"), tr("状态"), tr("开始"), tr("最近活动") });
    m_sessions->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_sessions->setSelectionMode(QAbstractItemView::SingleSelection);
    m_sessions->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_sessions->verticalHeader()->setVisible(false);
    m_sessions->horizontalHeader()->setStretchLastSection(true);
    sessionLayout->addWidget(m_sessions, 1);

    auto *sessionButtons = new QHBoxLayout;
    m_teardown = new QPushButton(tr("断开选中"), sessionBox);
    m_teardownAll = new QPushButton(tr("全部断开"), sessionBox);
    m_teardown->setToolTip(tr("模拟相机侧主动 TEARDOWN，测客户端会不会自动重连。"));
    sessionButtons->addWidget(m_teardown);
    sessionButtons->addWidget(m_teardownAll);
    sessionButtons->addStretch(1);
    sessionLayout->addLayout(sessionButtons);
    splitter->addWidget(sessionBox);

    connect(m_teardown, &QPushButton::clicked, this, [this] {
        VirtualCamera *cam = camera();
        const int row = m_sessions->currentRow();
        if (!cam || row < 0)
            return;
        if (QTableWidgetItem *item = m_sessions->item(row, 0))
            cam->rtspServer()->teardownSession(item->text().toLatin1());
        refresh();
    });
    connect(m_teardownAll, &QPushButton::clicked, this, [this] {
        if (VirtualCamera *cam = camera())
            cam->rtspServer()->teardownAll();
        refresh();
    });

    // ---- HTTP 客户端 ----
    auto *clientBox = new QGroupBox(tr("HTTP 客户端"), splitter);
    auto *clientLayout = new QVBoxLayout(clientBox);
    m_clients = new QTableWidget(0, 5, clientBox);
    m_clients->setHorizontalHeaderLabels(
        { tr("地址"), tr("User-Agent"), tr("请求数"), tr("最近路径"), tr("最近时间") });
    m_clients->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_clients->verticalHeader()->setVisible(false);
    m_clients->horizontalHeader()->setStretchLastSection(true);
    clientLayout->addWidget(m_clients, 1);

    auto *clientButtons = new QHBoxLayout;
    m_clearClients = new QPushButton(tr("清空记录"), clientBox);
    clientButtons->addWidget(m_clearClients);
    clientButtons->addStretch(1);
    m_summary = new QLabel(clientBox);
    clientButtons->addWidget(m_summary);
    clientLayout->addLayout(clientButtons);
    splitter->addWidget(clientBox);

    connect(m_clearClients, &QPushButton::clicked, this, [this] {
        if (VirtualCamera *cam = camera())
            m_clientsByCamera.remove(cam->id());
        refresh();
    });

    // 每台相机的 HTTP 请求都要记，不只是当前选中那台 —— 切回来时记录还在。
    if (simulator) {
        const QList<VirtualCamera *> cameras = simulator->cameras();
        for (VirtualCamera *cam : cameras)
            watchCamera(cam);
        connect(simulator, &Simulator::cameraAdded, this, &ConnectionsTab::watchCamera);
        connect(simulator, &Simulator::cameraRemoved, this,
                [this](const QString &id) { m_clientsByCamera.remove(id); });
    }
}

void ConnectionsTab::watchCamera(VirtualCamera *cam)
{
    if (!cam || !cam->httpServer())
        return;
    const QString id = cam->id();
    // 连到 HttpServer 的信号上，相机析构时连接自动失效，不用手工管。
    connect(cam->httpServer(), &HttpServer::requestReceived, this,
            [this, id](const HttpRequest &request) { recordRequest(id, request); });
}

void ConnectionsTab::recordRequest(const QString &cameraId, const HttpRequest &request)
{
    // 只更新内存里的统计，界面等下一次 refresh() 再画：
    // 事件风暴或压测时请求可能每秒上千条，逐条重画表格会把界面卡死。
    QMap<QString, HttpClientInfo> &clients = m_clientsByCamera[cameraId];
    const QString key = request.peerAddress.toString();
    HttpClientInfo &info = clients[key];
    info.address = key;
    const QByteArray agent = request.header("user-agent");
    if (!agent.isEmpty())
        info.userAgent = QString::fromUtf8(agent);
    ++info.requests;
    info.lastSeen = QDateTime::currentDateTimeUtc();
    info.lastPath = request.path;
}

void ConnectionsTab::bindCamera()
{
    VirtualCamera *cam = camera();
    if (!cam) {
        m_sessions->setRowCount(0);
        m_clients->setRowCount(0);
        return;
    }
    trackConnection(connect(cam->rtspServer(), &RtspServer::sessionCountChanged, this,
                            [this](int) { refreshNow(); }));
}

void ConnectionsTab::refresh()
{
    VirtualCamera *cam = camera();
    if (!cam)
        return;

    const QList<RtspSession *> sessions = cam->rtspServer()->sessions();
    m_sessions->setRowCount(sessions.size());
    for (int row = 0; row < sessions.size(); ++row) {
        RtspSession *session = sessions.at(row);
        const QStringList cells = {
            QString::fromLatin1(session->sessionId()),
            session->peerString(),
            session->profileToken(),
            trackSummary(session->tracks()),
            session->isPlaying() ? tr("播放中") : tr("已建立"),
            util::formatDateTime(session->startedAt()),
            util::formatRelative(session->lastActivity()),
        };
        for (int col = 0; col < cells.size(); ++col)
            m_sessions->setItem(row, col, new QTableWidgetItem(cells.at(col)));
    }
    m_teardown->setEnabled(!sessions.isEmpty());
    m_teardownAll->setEnabled(!sessions.isEmpty());

    const QMap<QString, HttpClientInfo> clients = m_clientsByCamera.value(cam->id());
    m_clients->setRowCount(clients.size());
    int row = 0;
    for (auto it = clients.constBegin(); it != clients.constEnd(); ++it, ++row) {
        const HttpClientInfo &info = it.value();
        const QStringList cells = {
            info.address,
            info.userAgent.isEmpty() ? tr("（未带 User-Agent）") : info.userAgent,
            QString::number(info.requests),
            info.lastPath,
            util::formatRelative(info.lastSeen),
        };
        for (int col = 0; col < cells.size(); ++col)
            m_clients->setItem(row, col, new QTableWidgetItem(cells.at(col)));
    }

    m_summary->setText(tr("当前 HTTP 连接 %1 · 累计客户端 %2")
                           .arg(cam->status().httpClients)
                           .arg(clients.size()));
}

} // namespace gui
} // namespace onvifsim
