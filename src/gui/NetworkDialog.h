#pragma once

// 网络设置对话框：网卡选择 / 网络模式 / 一键分配 IP 段。
//
// 独立 IP 模式要往宿主网卡上加别名，三平台的提权方式各不相同。
// 失败时**不静默降级回端口模式** —— 那样用户会以为成功了，
// 而是把该敲的命令原样列出来让他自己执行。

#include "core/Simulator.h"
#include "net/NetUtil.h"

#include <QtWidgets/QDialog>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QRadioButton;
class QSpinBox;

namespace onvifsim {

class IpAlias;

namespace gui {

class NetworkDialog : public QDialog
{
    Q_OBJECT
public:
    // ipAlias 由 MainWindow 长期持有：本进程加过哪些别名要记着，退出时回收。
    NetworkDialog(Simulator *simulator, IpAlias *ipAlias, QWidget *parent = nullptr);

    // 用户点了确定后要不要重启模拟器（改绑定地址 / 端口都得重启才生效）。
    bool needsRestart() const;

private slots:
    void assignAddressRange();
    void releaseAddresses();

private:
    void loadFromSimulator();
    // 返回 false 表示界面上有填错的值，对话框不该关。
    bool applyToSimulator();
    void updateModeState();
    QString selectedInterface() const;

    Simulator *m_simulator = nullptr;
    IpAlias *m_ipAlias = nullptr;
    bool m_needsRestart = false;

    QComboBox *m_interface = nullptr;
    QRadioButton *m_modePorts = nullptr;
    QRadioButton *m_modeIpAlias = nullptr;
    QRadioButton *m_modeExternal = nullptr;

    // 换网卡时把绑定地址 / 掩码 / 别名起始地址一起刷成这张卡的。
    void applyInterfaceAddresses(const QList<InterfaceInfo> &interfaces, int index);

    QLineEdit *m_bindAddress = nullptr;
    QSpinBox *m_httpPort = nullptr;
    QSpinBox *m_rtspPort = nullptr;

    QLineEdit *m_rangeStart = nullptr;
    QSpinBox *m_rangeCount = nullptr;
    QLineEdit *m_netmask = nullptr;
    QPlainTextEdit *m_aliasOutput = nullptr;

    QCheckBox *m_discovery = nullptr;
    QComboBox *m_discoveryInterface = nullptr;
    QCheckBox *m_controlApi = nullptr;
    QSpinBox *m_controlApiPort = nullptr;
    QLineEdit *m_controlApiToken = nullptr;
};

} // namespace gui
} // namespace onvifsim
