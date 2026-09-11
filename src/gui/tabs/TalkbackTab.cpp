#include "gui/tabs/TalkbackTab.h"

#include "core/VirtualCamera.h"
#include "gui/GuiUtil.h"
#include "gui/widgets/LevelMeterWidget.h"
#include "rtsp/RtspServer.h"
#include "rtsp/RtspSession.h"

#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

namespace onvifsim {
namespace gui {

namespace {

// 统计表的行顺序，改这里就等于改界面。
enum StatRow {
    RowCodec = 0,
    RowPackets,
    RowBytes,
    RowDropped,
    RowMarker,
    RowMarkerMissing,
    RowLevel,
    RowPeak,
    RowFirst,
    RowLast,
    RowCount,
};

QString codecName(AudioCodec c)
{
    return c == AudioCodec::None ? TalkbackTab::tr("无") : codec::audioCodecName(c);
}
} // namespace

TalkbackTab::TalkbackTab(Simulator *simulator, QWidget *parent)
    : CameraTab(simulator, parent)
{
    auto *layout = new QVBoxLayout(this);

    auto *sessionRow = new QHBoxLayout;
    sessionRow->addWidget(new QLabel(tr("对讲会话"), this));
    m_sessions = new QComboBox(this);
    m_sessions->setMinimumWidth(280);
    sessionRow->addWidget(m_sessions, 1);
    m_reset = new QPushButton(tr("重置统计"), this);
    sessionRow->addWidget(m_reset);
    m_record = new QPushButton(tr("保存 wav…"), this);
    sessionRow->addWidget(m_record);
    layout->addLayout(sessionRow);

    auto *meterBox = new QGroupBox(tr("电平"), this);
    auto *meterLayout = new QVBoxLayout(meterBox);
    m_meter = new LevelMeterWidget(meterBox);
    meterLayout->addWidget(m_meter);
    m_hint = new QLabel(meterBox);
    m_hint->setWordWrap(true);
    meterLayout->addWidget(m_hint);
    layout->addWidget(meterBox);

    m_stats = new QTableWidget(RowCount, 2, this);
    m_stats->setHorizontalHeaderLabels({ tr("统计项"), tr("值") });
    m_stats->verticalHeader()->setVisible(false);
    m_stats->horizontalHeader()->setStretchLastSection(true);
    m_stats->setEditTriggers(QAbstractItemView::NoEditTriggers);
    const QStringList names = { tr("编码"),         tr("收到包数"),     tr("收到字节"),
                                tr("推算丢包"),     tr("marker 计数"),  tr("marker 缺失"),
                                tr("当前电平"),     tr("峰值电平"),     tr("首包时间"),
                                tr("末包时间") };
    for (int row = 0; row < RowCount; ++row) {
        m_stats->setItem(row, 0, new QTableWidgetItem(names.value(row)));
        m_stats->setItem(row, 1, new QTableWidgetItem(QString()));
    }
    m_stats->resizeColumnsToContents();
    layout->addWidget(m_stats, 1);

    connect(m_sessions, &QComboBox::currentIndexChanged, this, [this] {
        updateLevelConnection();
        refreshNow();
    });
    connect(m_reset, &QPushButton::clicked, this, [this] {
        if (RtpReceiver *receiver = currentReceiver())
            receiver->resetStats();
        m_meter->reset();
        refresh();
    });
    connect(m_record, &QPushButton::clicked, this, [this] {
        RtpReceiver *receiver = currentReceiver();
        if (!receiver)
            return;
        if (receiver->isRecording()) {
            receiver->stopRecording();
            refresh();
            return;
        }
        const QString path = QFileDialog::getSaveFileName(this, tr("保存对讲音频"),
                                                          QStringLiteral("talkback.wav"),
                                                          tr("WAV 音频 (*.wav)"));
        if (path.isEmpty())
            return;
        QString error;
        if (!receiver->startRecording(path, &error))
            QMessageBox::warning(this, tr("保存失败"), error);
        refresh();
    });
}

void TalkbackTab::bindCamera()
{
    VirtualCamera *cam = camera();
    QObject::disconnect(m_levelConnection);
    m_sessions->clear();
    m_meter->reset();
    if (!cam)
        return;

    RtspServer *rtsp = cam->rtspServer();
    trackConnection(connect(rtsp, &RtspServer::sessionCountChanged, this,
                            [this](int) { reloadSessions(); }));
    // 有音频进来就立刻动电平表，不等每秒那次刷新 —— 对讲是要「跟着说话跳」的。
    trackConnection(connect(rtsp, &RtspServer::talkbackActivity, this, &CameraTab::refreshNow));
    reloadSessions();
}

void TalkbackTab::reloadSessions()
{
    // refresh() 末尾的兜底分支会调回这里，不挡一下就是无限互调。
    if (m_reloading)
        return;
    m_reloading = true;

    VirtualCamera *cam = camera();
    const QString previous = m_sessions->currentData().toString();

    const UiUpdateGuard guard(this);
    m_sessions->clear();
    if (cam) {
        const QList<RtspSession *> sessions = cam->rtspServer()->sessions();
        for (RtspSession *session : sessions) {
            if (!session->hasBackchannel())
                continue;
            m_sessions->addItem(tr("%1 · %2")
                                    .arg(QString::fromLatin1(session->sessionId()),
                                         session->peerString()),
                                QString::fromLatin1(session->sessionId()));
        }
    }
    const int index = m_sessions->findData(previous);
    if (index >= 0)
        m_sessions->setCurrentIndex(index);

    updateLevelConnection();
    refresh();
    m_reloading = false;
}

int TalkbackTab::backchannelSessionCount() const
{
    VirtualCamera *cam = camera();
    if (!cam)
        return 0;
    int count = 0;
    for (const RtspSession *session : cam->rtspServer()->sessions()) {
        if (session->hasBackchannel())
            ++count;
    }
    return count;
}

void TalkbackTab::updateLevelConnection()
{
    // 每秒刷新一次的电平表跟不上说话的节奏，直接接 RtpReceiver 的信号。
    // 会话换了就要改接线，老连接必须先断，否则上一条会话的电平会串进来。
    QObject::disconnect(m_levelConnection);
    if (RtpReceiver *receiver = currentReceiver()) {
        m_levelConnection = connect(receiver, &RtpReceiver::levelChanged, m_meter,
                                    &LevelMeterWidget::setLevel);
    }
}

RtpReceiver *TalkbackTab::currentReceiver() const
{
    VirtualCamera *cam = camera();
    if (!cam)
        return nullptr;
    const QByteArray wanted = m_sessions->currentData().toString().toLatin1();
    const QList<RtspSession *> sessions = cam->rtspServer()->sessions();
    for (RtspSession *session : sessions) {
        if (session->sessionId() == wanted)
            return session->backchannelReceiver();
    }
    return nullptr;
}

void TalkbackTab::refresh()
{
    VirtualCamera *cam = camera();
    if (!cam) {
        // 没有选中相机时要**清空**，不能直接 return：留在界面上的是上一台相机的
        // 数字，灰着但读得出来，而且是错的。EventsTab / ConnectionsTab /
        // StreamsTab 都是清空的，这里以前漏了。
        const UiUpdateGuard guard(this);
        m_sessions->clear();
        m_meter->setLevel(0.0);
        for (int row = 0; row < RowCount; ++row)
            m_stats->item(row, 1)->setText(QString());
        m_hint->setText(tr("没有选中相机。"));
        m_record->setEnabled(false);
        m_reset->setEnabled(false);
        return;
    }

    // 会话数变了但信号没赶上时（比如刚切相机），这里兜一次底。
    // 必须拿「有对讲轨的会话数」比：RtspServer::sessionCount() 是连接总数，
    // 普通取流的会话也算在内，而它们不会进这个下拉框 —— 拿总数比的话，
    // 只要有人在拉流，这个条件就永远成立，refresh 与 reloadSessions 互相调到爆栈。
    if (!isUpdatingUi() && m_sessions->count() != backchannelSessionCount())
        reloadSessions();

    RtpReceiver *receiver = currentReceiver();
    m_record->setEnabled(receiver != nullptr);
    m_reset->setEnabled(receiver != nullptr);

    if (!receiver) {
        m_hint->setText(tr("还没有带 backchannel 的 RTSP 会话。客户端要先 DESCRIBE 时带上 "
                           "Require: www.onvif.org/ver20/backchannel，SETUP 那条 sendonly 轨，"
                           "再 PLAY，才会有音频推进来。"));
        m_meter->setLevel(0.0);
        for (int row = 0; row < RowCount; ++row)
            m_stats->item(row, 1)->setText(QString());
        m_record->setText(tr("保存 wav…"));
        return;
    }

    const TalkbackStats stats = receiver->stats();
    m_meter->setLevel(stats.currentLevel);
    m_hint->setText(stats.packetsReceived > 0
                        ? tr("已收到 %1 个 RTP 包。").arg(stats.packetsReceived)
                        : tr("会话已建立，但还没有 RTP 包进来。"));

    m_stats->item(RowCodec, 1)->setText(codecName(stats.codec));
    m_stats->item(RowPackets, 1)->setText(QString::number(stats.packetsReceived));
    m_stats->item(RowBytes, 1)->setText(util::formatBytes(stats.bytesReceived));
    m_stats->item(RowDropped, 1)->setText(QString::number(stats.packetsDropped));
    m_stats->item(RowMarker, 1)->setText(QString::number(stats.markerCount));
    m_stats->item(RowMarkerMissing, 1)->setText(QString::number(stats.markerMissing));
    m_stats->item(RowLevel, 1)->setText(QString::number(stats.currentLevel, 'f', 3));
    m_stats->item(RowPeak, 1)->setText(QString::number(stats.peakLevel, 'f', 3));
    m_stats->item(RowFirst, 1)->setText(util::formatTime(stats.firstPacketAt));
    m_stats->item(RowLast, 1)->setText(util::formatTime(stats.lastPacketAt));

    m_record->setText(receiver->isRecording() ? tr("停止录制") : tr("保存 wav…"));
}
} // namespace gui
} // namespace onvifsim
