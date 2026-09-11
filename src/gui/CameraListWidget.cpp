#include "gui/CameraListWidget.h"

#include "gui/I18n.h"

#include "core/Simulator.h"
#include "core/VirtualCamera.h"
#include "gui/GuiUtil.h"

#include <QtGui/QAction>
#include <QtWidgets/QApplication>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>

namespace onvifsim {
namespace gui {

namespace {

constexpr int kCameraIdRole = Qt::UserRole + 1;

} // namespace

CameraListWidget::CameraListWidget(Simulator *simulator, QWidget *parent)
    : QWidget(parent)
    , m_simulator(simulator)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_list = new QListWidget(this);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setIconSize(QSize(24, 24));
    m_list->setAlternatingRowColors(true);
    layout->addWidget(m_list);

    connect(m_list, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *current, QListWidgetItem *) {
                emit cameraSelected(cameraOf(current));
            });
    connect(m_list, &QListWidget::customContextMenuRequested, this,
            &CameraListWidget::showContextMenu);

    if (m_simulator) {
        connect(m_simulator, &Simulator::cameraAdded, this, &CameraListWidget::onCameraAdded);
        connect(m_simulator, &Simulator::cameraRemoved, this, &CameraListWidget::onCameraRemoved);
        // 加载场景是「清空 + 批量添加」，逐条信号处理会闪一下，索性整表重建。
        connect(m_simulator, &Simulator::configChanged, this, &CameraListWidget::rebuild);
    }
    rebuild();
}

VirtualCamera *CameraListWidget::cameraOf(QListWidgetItem *item) const
{
    if (!item || !m_simulator)
        return nullptr;
    return m_simulator->camera(item->data(kCameraIdRole).toString());
}

VirtualCamera *CameraListWidget::currentCamera() const
{
    return cameraOf(m_list->currentItem());
}

void CameraListWidget::selectCamera(const QString &id)
{
    for (int row = 0; row < m_list->count(); ++row) {
        if (m_list->item(row)->data(kCameraIdRole).toString() == id) {
            m_list->setCurrentRow(row);
            return;
        }
    }
}

void CameraListWidget::rebuild()
{
    if (!m_simulator)
        return;

    const QString previous =
        m_list->currentItem() ? m_list->currentItem()->data(kCameraIdRole).toString() : QString();

    m_list->clear();
    const QList<VirtualCamera *> cameras = m_simulator->cameras();
    for (VirtualCamera *cam : cameras) {
        auto *item = new QListWidgetItem(m_list);
        item->setData(kCameraIdRole, cam->id());
        updateItem(item, cam);
    }

    if (!previous.isEmpty())
        selectCamera(previous);
    if (!m_list->currentItem() && m_list->count() > 0)
        m_list->setCurrentRow(0);
}

void CameraListWidget::onCameraAdded(VirtualCamera *camera)
{
    if (!camera)
        return;
    auto *item = new QListWidgetItem(m_list);
    item->setData(kCameraIdRole, camera->id());
    updateItem(item, camera);
    m_list->setCurrentItem(item);
}

void CameraListWidget::onCameraRemoved(const QString &id)
{
    for (int row = 0; row < m_list->count(); ++row) {
        if (m_list->item(row)->data(kCameraIdRole).toString() == id) {
            delete m_list->takeItem(row);
            return;
        }
    }
}

void CameraListWidget::updateItem(QListWidgetItem *item, VirtualCamera *camera)
{
    const CameraStatus status = camera->status();
    const CameraModel &model = camera->model();

    QString state;
    QStyle::StandardPixmap icon = QStyle::SP_MediaStop;
    if (status.offline) {
        state = tr("离线");
        icon = QStyle::SP_MessageBoxWarning;
    } else if (status.running) {
        state = tr("在线");
        icon = QStyle::SP_DriveNetIcon;
    } else {
        state = tr("已停止");
    }

    const QString event = status.lastEventAt.isValid() ? util::formatRelative(status.lastEventAt)
                                                       : tr("无事件");
    item->setText(tr("%1  %2\n%3 · 流 %4 · 订阅 %5 · %6")
                      .arg(model.displayName, camera->id(), state)
                      .arg(status.rtspSessions)
                      .arg(status.subscriptions)
                      .arg(event));
    item->setIcon(QApplication::style()->standardIcon(icon));
    item->setToolTip(tr("%1\n%2:%3\n预设 %4")
                         .arg(camera->deviceServiceXAddr(), model.bindAddress.toString())
                         .arg(model.httpPort)
                         .arg(i18n::personaName(camera->persona())));
}

void CameraListWidget::refresh()
{
    if (!m_simulator)
        return;
    // 相机数量对不上说明有人在别处增删过（REST 控制面也能加相机），整表重建一次。
    if (m_list->count() != m_simulator->cameraCount()) {
        rebuild();
        return;
    }
    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem *item = m_list->item(row);
        if (VirtualCamera *cam = cameraOf(item))
            updateItem(item, cam);
    }
}

void CameraListWidget::showContextMenu(const QPoint &pos)
{
    QListWidgetItem *item = m_list->itemAt(pos);
    VirtualCamera *cam = cameraOf(item);

    QMenu menu(this);
    QAction *addAction = menu.addAction(tr("添加相机…"));
    QAction *startAction = nullptr;
    QAction *offlineAction = nullptr;
    QAction *copyAction = nullptr;
    QAction *renameAction = nullptr;
    QAction *removeAction = nullptr;

    if (cam) {
        menu.addSeparator();
        startAction = menu.addAction(cam->isRunning() ? tr("停止") : tr("启动"));
        offlineAction = menu.addAction(tr("模拟离线 30 秒"));
        offlineAction->setEnabled(cam->isRunning() && !cam->isOffline());
        copyAction = menu.addAction(tr("复制 XAddr"));
        renameAction = menu.addAction(tr("重命名…"));
        menu.addSeparator();
        removeAction = menu.addAction(tr("删除相机"));
    }

    QAction *chosen = menu.exec(m_list->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;

    if (chosen == addAction) {
        emit addCameraRequested();
    } else if (chosen == startAction) {
        if (cam->isRunning()) {
            cam->stop();
        } else {
            QString error;
            if (!cam->start(&error))
                QMessageBox::warning(this, tr("启动失败"), error);
        }
    } else if (chosen == offlineAction) {
        cam->goOffline(30);
    } else if (chosen == copyAction) {
        util::copyToClipboard(cam->deviceServiceXAddr(), this);
    } else if (chosen == renameAction) {
        bool ok = false;
        const QString name =
            QInputDialog::getText(this, tr("重命名相机"), tr("显示名"), QLineEdit::Normal,
                                  cam->model().displayName, &ok);
        if (ok && !name.isEmpty()) {
            cam->mutableModel().displayName = name;
            cam->applyModelChanges();
        }
    } else if (chosen == removeAction) {
        const QString id = cam->id();
        if (QMessageBox::question(this, tr("删除相机"), tr("确定删除 %1 吗？").arg(id))
            == QMessageBox::Yes) {
            m_simulator->removeCamera(id);
        }
    }
    refresh();
}

} // namespace gui
} // namespace onvifsim
