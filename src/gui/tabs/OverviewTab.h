#pragma once

// 概览 Tab：把「客户端要拿去填的那几行字」摆在最显眼处 ——
// XAddr、各 profile 的 RTSP URL、快照地址、用户名密码，每一项都带复制按钮。
// 手测 ODM / VLC / Home Assistant 时，这个页面是使用频率最高的。

#include "gui/tabs/CameraTab.h"

class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace onvifsim {
namespace gui {

class OverviewTab : public CameraTab
{
    Q_OBJECT
public:
    explicit OverviewTab(Simulator *simulator, QWidget *parent = nullptr);

protected:
    void bindCamera() override;
    void refresh() override;

private:
    // 造一行「标签 + 只读输入框 + 复制按钮」，返回那个输入框。
    QLineEdit *addCopyRow(QFormLayout *form, const QString &label);
    void rebuildProfileRows();

    QLabel *m_statusLabel = nullptr;
    QLineEdit *m_deviceXAddr = nullptr;
    QLineEdit *m_mediaXAddr = nullptr;
    QLineEdit *m_eventsXAddr = nullptr;
    QLineEdit *m_ptzXAddr = nullptr;
    QLineEdit *m_imagingXAddr = nullptr;
    QLineEdit *m_snapshotUri = nullptr;
    QLineEdit *m_username = nullptr;
    QLineEdit *m_password = nullptr;
    QLineEdit *m_bindInfo = nullptr;

    QGroupBox *m_profileBox = nullptr;
    QFormLayout *m_profileForm = nullptr;
    QVector<QLineEdit *> m_profileEdits;

    QPushButton *m_startStopButton = nullptr;
    QPushButton *m_offlineButton = nullptr;
    QSpinBox *m_offlineSeconds = nullptr;
};

} // namespace gui
} // namespace onvifsim
