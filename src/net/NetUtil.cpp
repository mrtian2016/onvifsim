#include "net/NetUtil.h"

#include <QtCore/QDateTime>
#include <QtCore/QLocale>

#include <QtNetwork/QNetworkAddressEntry>
#include <QtNetwork/QTcpServer>

namespace onvifsim {
namespace netutil {
namespace {

bool isIPv4(const QHostAddress &address)
{
    return address.protocol() == QAbstractSocket::IPv4Protocol;
}

// 0.0.0.0 / :: 表示「绑在所有网卡上」，这时才轮得到我们替客户端挑地址。
bool isWildcard(const QHostAddress &address)
{
    return address.isNull() || address == QHostAddress(QHostAddress::Any)
        || address == QHostAddress(QHostAddress::AnyIPv4)
        || address == QHostAddress(QHostAddress::AnyIPv6);
}

InterfaceInfo toInfo(const QNetworkInterface &iface)
{
    InterfaceInfo info;
    info.name = iface.name();
    info.displayName = iface.humanReadableName();
    info.hardwareAddress = iface.hardwareAddress();
    const QNetworkInterface::InterfaceFlags flags = iface.flags();
    info.isUp = flags.testFlag(QNetworkInterface::IsUp)
        && flags.testFlag(QNetworkInterface::IsRunning);
    info.isLoopback = flags.testFlag(QNetworkInterface::IsLoopBack);
    info.supportsMulticast = flags.testFlag(QNetworkInterface::CanMulticast);
    // 只收 IPv4：XAddr、SDP、WS-Discovery 全线走 v4，v6 是二期的事（plan §3.3）。
    const QList<QNetworkAddressEntry> entries = iface.addressEntries();
    for (const QNetworkAddressEntry &entry : entries) {
        if (!isIPv4(entry.ip()))
            continue;
        info.addresses.append(entry.ip());
        info.netmasks.append(entry.netmask());
    }
    return info;
}

// 没有同网段命中时的兜底排序：普通 LAN 地址 > 链路本地（169.254）> 回环。
int addressScore(const InterfaceInfo &info, const QHostAddress &address)
{
    if (info.isLoopback || address.isLoopback())
        return 0;
    if (isLinkLocal(address))
        return 1;
    return info.isUp ? 3 : 2;
}

} // namespace

QList<InterfaceInfo> interfaces(bool includeLoopback)
{
    QList<InterfaceInfo> result;
    const QList<QNetworkInterface> all = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : all) {
        const InterfaceInfo info = toInfo(iface);
        if (info.addresses.isEmpty())
            continue;
        if (info.isLoopback && !includeLoopback)
            continue;
        result.append(info);
    }
    return result;
}

InterfaceInfo interfaceByName(const QString &name)
{
    const QList<InterfaceInfo> all = interfaces(true);
    for (const InterfaceInfo &info : all) {
        // GUI 里显示的是 humanReadableName（Windows 上和 name 完全不同），两边都认。
        if (info.name == name || info.displayName == name)
            return info;
    }
    return InterfaceInfo();
}

QHostAddress preferredLocalAddress(const QHostAddress &peer)
{
    return preferredLocalAddress(peer, QHostAddress(QHostAddress::AnyIPv4));
}

QHostAddress preferredLocalAddress(const QHostAddress &peer, const QHostAddress &boundTo)
{
    // 绑在具体地址上（独立 IP 模式的别名）时没得挑：客户端只可能从那个地址访问我们，
    // XAddr 报别的地址就会出现「来源 IP 与 XAddr 对不上」。
    if (!isWildcard(boundTo))
        return boundTo;

    const QList<InterfaceInfo> all = interfaces(true);

    // 1. 与 peer 同网段的地址最靠谱，跨网卡也能挑对。
    if (!peer.isNull() && !peer.isLoopback()) {
        for (const InterfaceInfo &info : all) {
            for (int i = 0; i < info.addresses.size(); ++i) {
                const QHostAddress mask = i < info.netmasks.size() ? info.netmasks.at(i)
                                                                   : QHostAddress();
                if (isSameSubnet(info.addresses.at(i), peer, mask))
                    return info.addresses.at(i);
            }
        }
    }

    // 2. peer 在网关那头，或者干脆是 127.0.0.1（同机客户端）。
    //    这时也要给 LAN IP 而不是回环：XAddr 会被客户端存下来、
    //    甚至转手给别的进程（NVR / 转发器）用，回环地址一出这台机器就废了。
    QHostAddress best;
    int bestScore = -1;
    for (const InterfaceInfo &info : all) {
        for (const QHostAddress &address : info.addresses) {
            const int score = addressScore(info, address);
            if (score > bestScore) {
                bestScore = score;
                best = address;
            }
        }
    }
    if (bestScore > 0)
        return best;

    // 3. 一块可用网卡都没有（离线机器 / 容器里只有 lo）。
    return QHostAddress(QHostAddress::LocalHost);
}

InterfaceInfo interfaceForAddress(const QHostAddress &address)
{
    const QList<InterfaceInfo> all = interfaces(true);
    // 先找地址完全相同的那块（WS-Discovery 要按相机别名回单播）。
    for (const InterfaceInfo &info : all) {
        for (const QHostAddress &candidate : info.addresses) {
            if (candidate == address)
                return info;
        }
    }
    // 退一步：peer 落在谁的网段里就算谁的。
    for (const InterfaceInfo &info : all) {
        for (int i = 0; i < info.addresses.size(); ++i) {
            const QHostAddress mask = i < info.netmasks.size() ? info.netmasks.at(i)
                                                               : QHostAddress();
            if (isSameSubnet(info.addresses.at(i), address, mask))
                return info;
        }
    }
    return InterfaceInfo();
}

bool isLinkLocal(const QHostAddress &address)
{
    if (address.isNull())
        return false;
    if (isIPv4(address))
        return (address.toIPv4Address() & 0xFFFF0000u) == 0xA9FE0000u;   // 169.254.0.0/16
    return address.isLinkLocal();
}

bool isSameSubnet(const QHostAddress &a, const QHostAddress &b, const QHostAddress &netmask)
{
    if (!isIPv4(a) || !isIPv4(b) || !isIPv4(netmask))
        return false;
    const quint32 mask = netmask.toIPv4Address();
    if (mask == 0)
        return false;   // 掩码没拿到就别乱认，否则全世界都算同网段
    return (a.toIPv4Address() & mask) == (b.toIPv4Address() & mask);
}

quint16 findFreePort(const QHostAddress &address, quint16 preferred, quint16 searchRange)
{
    // 试着真绑一下，这是唯一靠谱的判断方式（真机端口被占同样只有绑了才知道）。
    for (int i = 0; i <= int(searchRange); ++i) {
        const int port = int(preferred) + i;
        if (port > 65535)
            break;
        QTcpServer probe;
        if (probe.listen(address, quint16(port))) {
            probe.close();
            return quint16(port);
        }
    }
    return 0;
}

QByteArray httpDate()
{
    // 必须用 C locale：格式里的 ddd / MMM 是英文缩写，跟着系统语言走的话
    // 中文环境下会吐出「周日, 06 11月 ...」，客户端一律解析失败。
    return QLocale::c()
        .toString(QDateTime::currentDateTimeUtc(),
                  QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"))
        .toLatin1();
}

} // namespace netutil
} // namespace onvifsim
