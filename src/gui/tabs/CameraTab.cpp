#include "gui/tabs/CameraTab.h"

#include "core/VirtualCamera.h"

namespace onvifsim {
namespace gui {

CameraTab::CameraTab(Simulator *simulator, QWidget *parent)
    : QWidget(parent)
    , m_simulator(simulator)
{
}

CameraTab::~CameraTab() = default;

void CameraTab::setCamera(VirtualCamera *camera)
{
    // 相机可能是被删掉才换的，先把旧连接清干净再动指针，
    // 否则 disconnect 会碰到已经析构的对象。
    for (const QMetaObject::Connection &c : m_connections)
        QObject::disconnect(c);
    m_connections.clear();

    m_camera = camera;
    setEnabled(camera != nullptr);
    bindCamera();
    refresh();
}

VirtualCamera *CameraTab::camera() const
{
    return m_camera;
}

Simulator *CameraTab::simulator() const
{
    return m_simulator;
}

void CameraTab::refreshNow()
{
    if (m_updating)
        return;
    refresh();
}

void CameraTab::trackConnection(const QMetaObject::Connection &connection)
{
    m_connections.append(connection);
}

bool CameraTab::isUpdatingUi() const
{
    return m_updating > 0;
}

} // namespace gui
} // namespace onvifsim
