#pragma once

// 身份 Tab：品牌预设 + 设备信息三件套 + scopes + 用户表。
//
// 客户端是按 Manufacturer / Model / FirmwareVersion 这三个字串挑厂商适配器的，
// 所以这三行必须逐字可改 —— 换预设只是把它们填上默认值，不是锁死。
//
// 这一页全是可编辑控件，所以**不跟着每秒刷新走**：QLineEdit::setText 会把光标
// 打回行首，每秒来一次就没法打字了。只有模型真的从别处（REST / 场景加载）
// 变过时才回填一次，靠 m_dirty 标记。

#include "core/CameraModel.h"
#include "gui/tabs/CameraTab.h"

class QComboBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;

namespace onvifsim {
namespace gui {

class IdentityTab : public CameraTab
{
    Q_OBJECT
public:
    explicit IdentityTab(Simulator *simulator, QWidget *parent = nullptr);

protected:
    void bindCamera() override;
    void refresh() override;

private:
    void applyToModel();
    void applyPersona();
    void addUserRow(const User &user);
    void reload();

    bool m_dirty = true;   // 模型从别处变过，下次刷新要回填

    QComboBox *m_persona = nullptr;
    QPushButton *m_applyPersona = nullptr;

    QLineEdit *m_displayName = nullptr;
    QLineEdit *m_manufacturer = nullptr;
    QLineEdit *m_model = nullptr;
    QLineEdit *m_firmware = nullptr;
    QLineEdit *m_hardwareId = nullptr;
    QLineEdit *m_serial = nullptr;
    QLineEdit *m_hostname = nullptr;
    QLineEdit *m_location = nullptr;
    QLineEdit *m_endpoint = nullptr;

    QListWidget *m_scopes = nullptr;
    QTableWidget *m_users = nullptr;
};

} // namespace gui
} // namespace onvifsim
