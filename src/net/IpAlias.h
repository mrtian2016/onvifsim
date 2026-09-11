#pragma once

// 三平台 IP 别名增删（独立 IP 模式用）。
//
//   Linux   ip addr add 192.168.1.201/24 dev eth0
//   macOS   ifconfig en0 alias 192.168.1.201 255.255.255.0
//   Windows netsh interface ip add address "Ethernet" 192.168.1.201 255.255.255.0
//
// 提权失败时必须把手动命令原样给用户，不要静默降级到端口模式。

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtNetwork/QHostAddress>

namespace onvifsim {

struct IpAliasRequest {
    QString interfaceName;
    QHostAddress address;
    QHostAddress netmask = QHostAddress(QStringLiteral("255.255.255.0"));
};

struct IpAliasResult {
    bool ok = false;
    QString errorMessage;
    QStringList manualCommands;   // 失败时给用户照抄的命令
};

class IpAlias : public QObject
{
    Q_OBJECT
public:
    explicit IpAlias(QObject *parent = nullptr);
    ~IpAlias() override;

    // 本进程加过哪些别名（退出时回收）。
    QList<IpAliasRequest> ownedAliases() const;

    IpAliasResult add(const IpAliasRequest &request);
    IpAliasResult remove(const IpAliasRequest &request);
    // 批量分配一段连续地址。
    IpAliasResult addRange(const QString &interfaceName, const QHostAddress &start, int count,
                           const QHostAddress &netmask);
    void removeAllOwned();

    static bool aliasExists(const QString &interfaceName, const QHostAddress &address);
    // 网卡名是否安全可用。提权那条路最终要把它交给 sh，而这个名字一路来自
    // REST body / 场景文件 / 命令行 —— 全是外部输入。空名、超长、带 shell
    // 元字符的一律在这里就拒掉，别指望下游转义。
    static bool isValidInterfaceName(const QString &name);
    static QStringList commandsFor(const IpAliasRequest &request, bool add);
    // 平台是否需要提权；Linux 上有 CAP_NET_ADMIN 时可能不需要。
    static bool needsElevation();
    static QString elevationHint();

signals:

private:
    struct Private;
    Private *d;
};

} // namespace onvifsim
