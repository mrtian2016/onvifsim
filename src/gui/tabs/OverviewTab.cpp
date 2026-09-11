#include "gui/tabs/OverviewTab.h"

#include "gui/I18n.h"

#include "core/Simulator.h"
#include "core/VirtualCamera.h"
#include "gui/GuiUtil.h"

#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

namespace onvifsim {
namespace gui {

namespace {

// QFormLayout 没有「清空」接口，profile 数量变了只能一行行摘。
void clearForm(QFormLayout *form)
{
    while (form->rowCount() > 0)
        form->removeRow(0);
}

// 这一页每秒刷新一次，而 QLineEdit::setText 会把选区和光标一起清掉 ——
// 用户正拖蓝半条 URL 想复制时就白拖了。所以值没变就别碰控件。
void setTextIfChanged(QLineEdit *edit, const QString &text)
{
    if (edit->text() != text)
        edit->setText(text);
}

} // namespace

OverviewTab::OverviewTab(Simulator *simulator, QWidget *parent)
    : CameraTab(simulator, parent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    scroll->setWidget(content);

    // ---- 运行状态与开关 ----
    auto *stateBox = new QGroupBox(tr("运行状态"), content);
    auto *stateLayout = new QVBoxLayout(stateBox);
    m_statusLabel = new QLabel(stateBox);
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    stateLayout->addWidget(m_statusLabel);

    auto *buttons = new QHBoxLayout;
    m_startStopButton = new QPushButton(tr("启动"), stateBox);
    buttons->addWidget(m_startStopButton);
    m_offlineButton = new QPushButton(tr("模拟离线"), stateBox);
    buttons->addWidget(m_offlineButton);
    m_offlineSeconds = new QSpinBox(stateBox);
    m_offlineSeconds->setRange(1, 3600);
    m_offlineSeconds->setValue(30);
    m_offlineSeconds->setSuffix(tr(" 秒"));
    buttons->addWidget(m_offlineSeconds);
    buttons->addStretch(1);
    stateLayout->addLayout(buttons);
    layout->addWidget(stateBox);

    connect(m_startStopButton, &QPushButton::clicked, this, [this] {
        VirtualCamera *cam = camera();
        if (!cam)
            return;
        if (cam->isRunning()) {
            cam->stop();
        } else {
            QString error;
            if (!cam->start(&error))
                QMessageBox::warning(this, tr("启动失败"), error);
        }
        refresh();
    });
    connect(m_offlineButton, &QPushButton::clicked, this, [this] {
        if (VirtualCamera *cam = camera())
            cam->goOffline(m_offlineSeconds->value());
        refresh();
    });

    // ---- 服务地址 ----
    auto *serviceBox = new QGroupBox(tr("服务地址"), content);
    auto *serviceForm = new QFormLayout(serviceBox);
    m_deviceXAddr = addCopyRow(serviceForm, tr("设备服务 XAddr"));
    m_deviceXAddr->setToolTip(tr("客户端硬编码这条路径，任何故障注入都不会改它。"));
    m_mediaXAddr = addCopyRow(serviceForm, tr("Media 服务"));
    m_eventsXAddr = addCopyRow(serviceForm, tr("Events 服务"));
    m_ptzXAddr = addCopyRow(serviceForm, tr("PTZ 服务"));
    m_imagingXAddr = addCopyRow(serviceForm, tr("Imaging 服务"));
    m_snapshotUri = addCopyRow(serviceForm, tr("快照地址"));
    m_bindInfo = addCopyRow(serviceForm, tr("实际监听"));
    layout->addWidget(serviceBox);

    // ---- 码流 ----
    m_profileBox = new QGroupBox(tr("码流地址"), content);
    m_profileForm = new QFormLayout(m_profileBox);
    layout->addWidget(m_profileBox);

    // ---- 凭据 ----
    auto *credBox = new QGroupBox(tr("凭据"), content);
    auto *credForm = new QFormLayout(credBox);
    m_username = addCopyRow(credForm, tr("用户名"));
    m_password = addCopyRow(credForm, tr("密码"));
    layout->addWidget(credBox);

    layout->addStretch(1);
}

QLineEdit *OverviewTab::addCopyRow(QFormLayout *form, const QString &label)
{
    auto *row = new QWidget(form->parentWidget());
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);

    auto *edit = new QLineEdit(row);
    edit->setReadOnly(true);
    // 只读但仍要可选中：用户经常是拖蓝一段而不是整行复制。
    edit->setCursorPosition(0);
    rowLayout->addWidget(edit, 1);

    auto *copy = new QToolButton(row);
    copy->setText(tr("复制"));
    copy->setToolTip(tr("复制到剪贴板"));
    rowLayout->addWidget(copy);
    connect(copy, &QToolButton::clicked, this,
            [edit, copy] { util::copyToClipboard(edit->text(), copy); });

    form->addRow(label, row);
    return edit;
}

void OverviewTab::bindCamera()
{
    if (VirtualCamera *cam = camera()) {
        trackConnection(connect(cam, &VirtualCamera::statusChanged, this, &CameraTab::refreshNow));
        trackConnection(connect(cam, &VirtualCamera::modelChanged, this, [this] {
            rebuildProfileRows();
            refresh();
        }));
        trackConnection(connect(cam, &VirtualCamera::quirksChanged, this, &CameraTab::refreshNow));
    }
    rebuildProfileRows();
}

void OverviewTab::rebuildProfileRows()
{
    clearForm(m_profileForm);
    m_profileEdits.clear();

    VirtualCamera *cam = camera();
    if (!cam)
        return;

    for (const MediaProfile &profile : cam->model().profiles) {
        QLineEdit *edit = addCopyRow(m_profileForm,
                                     QStringLiteral("%1 (%2)").arg(profile.name, profile.token));
        m_profileEdits.append(edit);
    }
}

void OverviewTab::refresh()
{
    VirtualCamera *cam = camera();
    if (!cam) {
        m_statusLabel->setText(tr("没有选中相机"));
        return;
    }

    const CameraModel &model = cam->model();
    const CameraStatus status = cam->status();

    QString state;
    if (status.offline) {
        const qint64 left = QDateTime::currentDateTimeUtc().secsTo(status.offlineUntil);
        state = tr("离线中（还有 %1 秒恢复）").arg(qMax<qint64>(0, left));
    } else if (status.running) {
        state = tr("在线");
    } else {
        state = tr("已停止");
    }
    m_statusLabel->setText(tr("%1 · %2 · RTSP 会话 %3 · 订阅 %4 · HTTP 连接 %5 · 最近事件 %6")
                               .arg(state, i18n::personaName(cam->persona()))
                               .arg(status.rtspSessions)
                               .arg(status.subscriptions)
                               .arg(status.httpClients)
                               .arg(status.lastEventAt.isValid()
                                        ? util::formatRelative(status.lastEventAt)
                                        : tr("无")));
    m_startStopButton->setText(cam->isRunning() ? tr("停止") : tr("启动"));
    m_offlineButton->setEnabled(cam->isRunning() && !cam->isOffline());

    setTextIfChanged(m_deviceXAddr, cam->deviceServiceXAddr());
    setTextIfChanged(m_mediaXAddr, cam->serviceXAddr(QStringLiteral("media")));
    setTextIfChanged(m_eventsXAddr, cam->serviceXAddr(QStringLiteral("events")));
    setTextIfChanged(m_ptzXAddr, cam->serviceXAddr(QStringLiteral("ptz")));
    setTextIfChanged(m_imagingXAddr, cam->serviceXAddr(QStringLiteral("imaging")));
    setTextIfChanged(m_bindInfo,
                     tr("%1  HTTP %2  RTSP %3")
                            .arg(model.bindAddress.toString())
                            .arg(model.httpPort)
                            .arg(model.rtspPort));

    const QList<MediaProfile> &profiles = model.profiles;
    if (!profiles.isEmpty())
        setTextIfChanged(m_snapshotUri, cam->snapshotUri(profiles.first()));
    else
        m_snapshotUri->clear();

    // profile 数量对不上说明模型刚变过，重建一次再填。
    if (m_profileEdits.size() != profiles.size())
        rebuildProfileRows();
    for (int i = 0; i < m_profileEdits.size() && i < profiles.size(); ++i)
        setTextIfChanged(m_profileEdits[i], cam->streamUri(profiles.at(i)));

    if (!model.users.isEmpty()) {
        setTextIfChanged(m_username, model.users.first().username);
        setTextIfChanged(m_password, model.users.first().password);
    } else {
        setTextIfChanged(m_username, tr("（无用户，等于不鉴权）"));
        m_password->clear();
    }
}

} // namespace gui
} // namespace onvifsim
