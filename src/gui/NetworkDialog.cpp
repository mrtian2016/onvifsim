#include "gui/NetworkDialog.h"

#include "core/VirtualCamera.h"
#include "net/IpAlias.h"
#include "net/NetUtil.h"

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

namespace onvifsim {
namespace gui {

NetworkDialog::NetworkDialog(Simulator *simulator, IpAlias *ipAlias, QWidget *parent)
    : QDialog(parent)
    , m_simulator(simulator)
    , m_ipAlias(ipAlias)
{
    setWindowTitle(tr("网络设置"));
    setModal(true);

    auto *layout = new QVBoxLayout(this);

    // ---- 网卡 ----
    auto *ifaceBox = new QGroupBox(tr("网卡"), this);
    auto *ifaceForm = new QFormLayout(ifaceBox);
    m_interface = new QComboBox(ifaceBox);
    m_discoveryInterface = new QComboBox(ifaceBox);
    m_discoveryInterface->addItem(tr("全部网卡"), QString());
    const QList<InterfaceInfo> interfaces = netutil::interfaces(true);
    for (const InterfaceInfo &info : interfaces) {
        QStringList addresses;
        for (const QHostAddress &address : info.addresses)
            addresses.append(address.toString());
        const QString label = QStringLiteral("%1  %2")
                                  .arg(info.displayName.isEmpty() ? info.name : info.displayName,
                                       addresses.join(QStringLiteral(", ")));
        m_interface->addItem(label, info.name);
        m_discoveryInterface->addItem(label, info.name);
    }
    ifaceForm->addRow(tr("目标网卡"), m_interface);
    layout->addWidget(ifaceBox);

    // 换网卡时把下面的地址跟着改掉。不这么做的话，用户选了「以太网」
    // 但绑定地址还停在上一张卡的 IP 上，起来之后 XAddr 报的是错网段 ——
    // 客户端能发现设备却连不上，最难查的那种。
    // 用 activated 而不是 currentIndexChanged：只在用户真的动手时才覆盖，
    // 程序自己 setCurrentIndex 时不打扰用户已经填好的值。
    connect(m_interface, &QComboBox::activated, this,
            [this, interfaces](int index) { applyInterfaceAddresses(interfaces, index); });

    // ---- 网络模式 ----
    auto *modeBox = new QGroupBox(tr("网络模式"), this);
    auto *modeLayout = new QVBoxLayout(modeBox);
    m_modePorts = new QRadioButton(tr("端口模式：同一 IP、端口递增（默认，不需要权限）"),
                                   modeBox);
    m_modeIpAlias = new QRadioButton(tr("独立 IP 模式：每台相机一个 IP 别名、绑标准端口 80 / 554"),
                                     modeBox);
    m_modeExternal = new QRadioButton(tr("外部编排：地址由 Docker macvlan 等安排好，只负责绑定"),
                                      modeBox);
    modeLayout->addWidget(m_modePorts);
    modeLayout->addWidget(m_modeIpAlias);
    modeLayout->addWidget(m_modeExternal);

    auto *modeForm = new QFormLayout;
    m_bindAddress = new QLineEdit(modeBox);
    m_bindAddress->setToolTip(tr("0.0.0.0 表示所有网卡。同机测试时对外报的地址会自动挑 LAN IP，"
                                 "不会给出 127.0.0.1。"));
    modeForm->addRow(tr("绑定地址"), m_bindAddress);
    m_httpPort = new QSpinBox(modeBox);
    m_httpPort->setRange(1, 65535);
    modeForm->addRow(tr("HTTP 起始端口"), m_httpPort);
    m_rtspPort = new QSpinBox(modeBox);
    m_rtspPort->setRange(1, 65535);
    modeForm->addRow(tr("RTSP 起始端口"), m_rtspPort);
    modeLayout->addLayout(modeForm);
    layout->addWidget(modeBox);

    // ---- IP 别名 ----
    auto *aliasBox = new QGroupBox(tr("IP 别名（独立 IP 模式）"), this);
    auto *aliasLayout = new QVBoxLayout(aliasBox);
    auto *aliasForm = new QFormLayout;
    m_rangeStart = new QLineEdit(aliasBox);
    m_rangeStart->setPlaceholderText(QStringLiteral("192.168.1.201"));
    aliasForm->addRow(tr("起始地址"), m_rangeStart);
    m_rangeCount = new QSpinBox(aliasBox);
    m_rangeCount->setRange(1, 64);
    m_rangeCount->setValue(1);
    aliasForm->addRow(tr("数量"), m_rangeCount);
    m_netmask = new QLineEdit(QStringLiteral("255.255.255.0"), aliasBox);
    aliasForm->addRow(tr("子网掩码"), m_netmask);
    aliasLayout->addLayout(aliasForm);

    auto *aliasButtons = new QHBoxLayout;
    auto *assign = new QPushButton(tr("一键分配 IP 段"), aliasBox);
    assign->setToolTip(tr("给现有相机逐台分配地址，需要的话会提权添加网卡别名。"));
    auto *release = new QPushButton(tr("回收本进程添加的别名"), aliasBox);
    aliasButtons->addWidget(assign);
    aliasButtons->addWidget(release);
    aliasButtons->addStretch(1);
    aliasLayout->addLayout(aliasButtons);

    m_aliasOutput = new QPlainTextEdit(aliasBox);
    m_aliasOutput->setReadOnly(true);
    m_aliasOutput->setPlaceholderText(tr("提权失败时，这里会列出可以手工执行的命令。"));
    m_aliasOutput->setMaximumHeight(110);
    aliasLayout->addWidget(m_aliasOutput);
    layout->addWidget(aliasBox);

    connect(assign, &QPushButton::clicked, this, &NetworkDialog::assignAddressRange);
    connect(release, &QPushButton::clicked, this, &NetworkDialog::releaseAddresses);

    // ---- 发现与控制面 ----
    auto *serviceBox = new QGroupBox(tr("发现与控制面"), this);
    auto *serviceForm = new QFormLayout(serviceBox);
    m_discovery = new QCheckBox(tr("启用 WS-Discovery（UDP 3702）"), serviceBox);
    serviceForm->addRow(QString(), m_discovery);
    serviceForm->addRow(tr("发现使用的网卡"), m_discoveryInterface);
    m_controlApi = new QCheckBox(tr("启用 REST 控制面"), serviceBox);
    serviceForm->addRow(QString(), m_controlApi);
    m_controlApiPort = new QSpinBox(serviceBox);
    m_controlApiPort->setRange(1, 65535);
    serviceForm->addRow(tr("控制面端口"), m_controlApiPort);
    m_controlApiToken = new QLineEdit(serviceBox);
    m_controlApiToken->setPlaceholderText(tr("留空表示不校验"));
    serviceForm->addRow(tr("控制面 token"), m_controlApiToken);
    layout->addWidget(serviceBox);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        // 填错了就别关对话框 —— 老写法是静默丢弃非法地址然后照样 accept()，
        // 用户接着还会看到「要现在停掉再启动吗」，全程没有一个字提示他填错了。
        if (applyToSimulator())
            accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    for (QRadioButton *button : { m_modePorts, m_modeIpAlias, m_modeExternal })
        connect(button, &QRadioButton::toggled, this, [this] { updateModeState(); });

    loadFromSimulator();
    updateModeState();
}

bool NetworkDialog::needsRestart() const
{
    return m_needsRestart;
}

// 把选中网卡的地址填进绑定地址与 IP 别名起始地址。
void NetworkDialog::applyInterfaceAddresses(const QList<InterfaceInfo> &interfaces, int index)
{
    if (index < 0 || index >= interfaces.size())
        return;
    const InterfaceInfo &info = interfaces.at(index);
    if (info.addresses.isEmpty()) {
        // 没有 IPv4 的网卡（比如刚插上还没拿到地址）：退回 0.0.0.0，
        // 总比留着上一张卡的地址强。
        m_bindAddress->setText(QStringLiteral("0.0.0.0"));
        return;
    }

    const QHostAddress &address = info.addresses.first();
    m_bindAddress->setText(address.toString());

    // 子网掩码跟着这张卡走。
    const int maskIndex = info.addresses.indexOf(address);
    if (maskIndex >= 0 && maskIndex < info.netmasks.size())
        m_netmask->setText(info.netmasks.at(maskIndex).toString());

    // 别名起始地址给个同网段的建议值：本机地址所在网段的 .201，
    // 这是家用网段里通常没人占的一段。用户可以改。
    const quint32 raw = address.toIPv4Address();
    if (raw != 0) {
        const QHostAddress suggestion((raw & 0xFFFFFF00u) | 201u);
        m_rangeStart->setText(suggestion.toString());
    }
}

QString NetworkDialog::selectedInterface() const
{
    return m_interface->currentData().toString();
}

void NetworkDialog::loadFromSimulator()
{
    if (!m_simulator)
        return;
    const SimulatorConfig &config = m_simulator->config();

    switch (config.networkMode) {
    case NetworkMode::Ports:
        m_modePorts->setChecked(true);
        break;
    case NetworkMode::IpAlias:
        m_modeIpAlias->setChecked(true);
        break;
    case NetworkMode::External:
        m_modeExternal->setChecked(true);
        break;
    }

    m_bindAddress->setText(config.bindAddress.toString());
    m_httpPort->setValue(config.httpBasePort);
    m_rtspPort->setValue(config.rtspBasePort);
    m_discovery->setChecked(config.discoveryEnabled);
    const int discoveryIndex = m_discoveryInterface->findData(config.discoveryInterface);
    m_discoveryInterface->setCurrentIndex(discoveryIndex >= 0 ? discoveryIndex : 0);
    m_controlApi->setChecked(config.controlApiEnabled);
    m_controlApiPort->setValue(config.controlApiPort);
    m_controlApiToken->setText(config.controlApiToken);

    // 起始地址默认给当前网卡的第一个地址，省得用户自己抄。
    const InterfaceInfo info = netutil::interfaceByName(selectedInterface());
    if (!info.addresses.isEmpty())
        m_rangeStart->setText(info.addresses.first().toString());
    m_rangeCount->setValue(qMax(1, m_simulator->cameraCount()));
}

void NetworkDialog::updateModeState()
{
    const bool aliasMode = m_modeIpAlias->isChecked();
    m_rangeStart->setEnabled(aliasMode);
    m_rangeCount->setEnabled(aliasMode);
    m_netmask->setEnabled(aliasMode);
    // 独立 IP 模式下端口是固定的 80 / 554，起始端口没有意义。
    m_httpPort->setEnabled(!aliasMode);
    m_rtspPort->setEnabled(!aliasMode);
}

bool NetworkDialog::applyToSimulator()
{
    if (!m_simulator)
        return true;

    SimulatorConfig config = m_simulator->config();
    const SimulatorConfig before = config;

    if (m_modeIpAlias->isChecked())
        config.networkMode = NetworkMode::IpAlias;
    else if (m_modeExternal->isChecked())
        config.networkMode = NetworkMode::External;
    else
        config.networkMode = NetworkMode::Ports;

    QHostAddress bind;
    if (!bind.setAddress(m_bindAddress->text())) {
        QMessageBox::warning(this, tr("地址不合法"),
                             tr("绑定地址「%1」不是合法的 IP，"
                                "填 0.0.0.0 表示监听全部网卡。")
                                 .arg(m_bindAddress->text()));
        m_bindAddress->setFocus();
        m_bindAddress->selectAll();
        return false;
    }
    config.bindAddress = bind;
    config.httpBasePort = static_cast<quint16>(m_httpPort->value());
    config.rtspBasePort = static_cast<quint16>(m_rtspPort->value());
    config.discoveryEnabled = m_discovery->isChecked();
    config.discoveryInterface = m_discoveryInterface->currentData().toString();
    config.controlApiEnabled = m_controlApi->isChecked();
    config.controlApiPort = static_cast<quint16>(m_controlApiPort->value());
    config.controlApiToken = m_controlApiToken->text();

    m_needsRestart = before.networkMode != config.networkMode
                     || before.bindAddress != config.bindAddress
                     || before.httpBasePort != config.httpBasePort
                     || before.rtspBasePort != config.rtspBasePort
                     || before.discoveryEnabled != config.discoveryEnabled
                     || before.discoveryInterface != config.discoveryInterface
                     || before.controlApiEnabled != config.controlApiEnabled
                     || before.controlApiPort != config.controlApiPort;

    m_simulator->setConfig(config);
    return true;
}

void NetworkDialog::assignAddressRange()
{
    if (!m_simulator || !m_ipAlias)
        return;

    QHostAddress start;
    if (!start.setAddress(m_rangeStart->text())) {
        QMessageBox::warning(this, tr("地址不合法"), tr("请填一个 IPv4 地址，例如 192.168.1.201"));
        return;
    }
    QHostAddress netmask;
    if (!netmask.setAddress(m_netmask->text()))
        netmask = QHostAddress(QStringLiteral("255.255.255.0"));

    const QString iface = selectedInterface();
    const int count = m_rangeCount->value();
    const IpAliasResult result = m_ipAlias->addRange(iface, start, count, netmask);

    if (!result.ok) {
        // 提权失败不降级：把命令原样给用户，让他知道差的是什么。
        m_aliasOutput->setPlainText(
            tr("添加别名失败：%1\n\n可以手工执行：\n%2\n\n%3")
                .arg(result.errorMessage, result.manualCommands.join(QLatin1Char('\n')),
                     IpAlias::elevationHint()));
        return;
    }

    // 别名建好了才把地址分给相机，顺序反了会得到一堆绑不上的相机。
    const quint32 base = start.toIPv4Address();
    const QList<VirtualCamera *> cameras = m_simulator->cameras();
    QStringList assigned;
    for (int i = 0; i < cameras.size() && i < count; ++i) {
        VirtualCamera *cam = cameras.at(i);
        CameraModel model = cam->model();
        model.bindAddress = QHostAddress(base + static_cast<quint32>(i));
        model.httpPort = 80;
        model.rtspPort = 554;
        cam->setModel(model);
        assigned.append(QStringLiteral("%1 → %2").arg(cam->id(), model.bindAddress.toString()));
    }

    m_modeIpAlias->setChecked(true);
    m_needsRestart = true;
    m_aliasOutput->setPlainText(tr("已分配 %1 个地址：\n%2")
                                    .arg(assigned.size())
                                    .arg(assigned.join(QLatin1Char('\n'))));
}

void NetworkDialog::releaseAddresses()
{
    if (!m_ipAlias)
        return;
    m_ipAlias->removeAllOwned();
    m_aliasOutput->setPlainText(tr("已回收本进程添加的全部别名。"));
}

} // namespace gui
} // namespace onvifsim
