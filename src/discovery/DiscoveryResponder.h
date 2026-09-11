#pragma once

// WS-Discovery。所有相机共用一个 UDP :3702 接收 socket（按物理网卡各一个），
// 但 ProbeMatch 是单播回给探测方，必须从相机自己的地址发出 —— 独立 IP 模式下
// 每台相机一个发送用 socket 绑到它的别名地址，否则客户端看到的源 IP 与 XAddrs 不一致。
//
// 两套方言：1.0（2005/04 discovery + 2004/08 addressing）与 OASIS 2009/01。
// 默认照抄 Probe 的命名空间回复（quirk A1 可强制某一套）。
//
// Probe 不一定带 Types —— 参照客户端的 Probe 就没有，
// 所以不能以 dn:NetworkVideoTransmitter 出现为应答前提。
// 同一 Probe 会重发 4 次（MessageID 相同），可选每份都回或去重回一次。

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtNetwork/QHostAddress>

namespace onvifsim {

class Simulator;
class VirtualCamera;

enum class DiscoveryDialect { Echo, Ws2005, Oasis2009 };

class DiscoveryResponder : public QObject
{
    Q_OBJECT
public:
    explicit DiscoveryResponder(Simulator *simulator, QObject *parent = nullptr);
    ~DiscoveryResponder() override;

    // interfaceName 为空表示所有支持组播的网卡各 join 一次
    //（Windows 上必须逐接口加入）。
    bool start(const QString &interfaceName = QString(), QString *errorOut = nullptr);
    void stop();
    bool isRunning() const;

    // 相机上下线时主动发 Hello / Bye。
    void announceHello(VirtualCamera *camera);
    void announceBye(VirtualCamera *camera);
    void announceAllHello();
    void announceAllBye();

    qint64 probesReceived() const;
    qint64 matchesSent() const;

signals:

private:
    struct Private;
    Private *d;
};

} // namespace onvifsim
