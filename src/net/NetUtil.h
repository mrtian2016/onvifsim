#pragma once

// 网卡枚举与「按目标选源 IP」。
// 同机测试时 XAddr 必须给 LAN IP 而不是 127.0.0.1，就靠这里挑。

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkInterface>

namespace onvifsim {

struct InterfaceInfo {
    QString name;
    QString displayName;
    QString hardwareAddress;
    QList<QHostAddress> addresses;   // 只含 IPv4
    QList<QHostAddress> netmasks;
    bool isUp = false;
    bool isLoopback = false;
    bool supportsMulticast = false;
};

namespace netutil {

QList<InterfaceInfo> interfaces(bool includeLoopback = false);
InterfaceInfo interfaceByName(const QString &name);

// 挑一个能被 peer 连上的本机地址。绑在 0.0.0.0 上时用它决定 XAddr 的 host。
QHostAddress preferredLocalAddress(const QHostAddress &peer);
QHostAddress preferredLocalAddress(const QHostAddress &peer, const QHostAddress &boundTo);

// 找出 peer 所在的那块网卡（WS-Discovery 回 ProbeMatch 要用）。
InterfaceInfo interfaceForAddress(const QHostAddress &address);

bool isLinkLocal(const QHostAddress &address);
bool isSameSubnet(const QHostAddress &a, const QHostAddress &b, const QHostAddress &netmask);

// 首个可用端口，用于自动分配。
quint16 findFreePort(const QHostAddress &address, quint16 preferred, quint16 searchRange = 200);

// RFC 7231 的 HTTP-date：`Sun, 06 Nov 1994 08:49:37 GMT`。
// HTTP 与 RTSP 的 Date 头是同一个格式，原来两边各写了一份 —— 而且格式串
// 一个用 HH 一个用 hh（后者只要有人再加个 AP 就变 12 小时制），
// 这种分叉不会有任何测试发现。
QByteArray httpDate();

} // namespace netutil
} // namespace onvifsim
