#include "discovery/DiscoveryResponder.h"

#include "core/LogBus.h"
#include "core/Quirks.h"
#include "core/Simulator.h"
#include "core/VirtualCamera.h"
#include "discovery/WsdMessages.h"
#include "net/NetUtil.h"

#include <QtCore/QDateTime>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtCore/QVariant>
#include <QtNetwork/QNetworkDatagram>
#include <QtNetwork/QNetworkInterface>
#include <QtNetwork/QUdpSocket>

namespace onvifsim {
namespace {

const quint16 kWsdPort = 3702;

QHostAddress multicastGroup()
{
    return QHostAddress(QStringLiteral("239.255.255.250"));
}

// 绑在 AnyIPv4 上时 Qt 也可能把来源报成 ::ffff:a.b.c.d，
// 直接拿去当 peer 会让 preferredLocalAddress 选错网卡。
QHostAddress normalized(const QHostAddress &address)
{
    bool ok = false;
    const quint32 v4 = address.toIPv4Address(&ok);
    return ok ? QHostAddress(v4) : address;
}

// 相机没填 EPR 时兜一个稳定值：EPR 是客户端的去重键，
// 每次回不一样的话一台相机会被当成 N 台。
QString endpointReferenceOf(const VirtualCamera *camera)
{
    const QString epr = camera->model().endpointReference;
    if (!epr.isEmpty())
        return epr;
    return QStringLiteral("urn:uuid:%1")
        .arg(QUuid::createUuidV5(QUuid(), camera->id()).toString(QUuid::WithoutBraces));
}

bool isWildcardAddress(const QHostAddress &address)
{
    return address.isNull() || address == QHostAddress(QHostAddress::AnyIPv4)
        || address == QHostAddress(QHostAddress::Any);
}

} // namespace

// 实现全放在 Private 里：DiscoveryResponder.h 的公开契约不因为实现细节而变动。
struct DiscoveryResponder::Private {
    Simulator *simulator = nullptr;
    DiscoveryResponder *q = nullptr;

    // 接收：一个 socket bind 到 0.0.0.0:3702，再逐网卡 join 组播组。
    // Windows 上只 join 一次收不到别的网卡上的 Probe。
    QUdpSocket *listener = nullptr;
    QList<QNetworkInterface> joined;

    // 发送：每台相机一个，bind 到它自己的地址。独立 IP 模式下 ProbeMatch 的源 IP
    // 必须与 XAddrs 里的一致，共用一个 socket 就对不上了。
    QHash<QString, QUdpSocket *> senders;

    bool running = false;

    qint64 probes = 0;
    qint64 matches = 0;

    quint64 instanceId = 0;
    quint32 messageNumber = 0;

    QUdpSocket *senderFor(VirtualCamera *camera);
    void dropSender(const QString &cameraId);

    WsdMatch makeMatch(VirtualCamera *camera, const QHostAddress &peer) const;
    WsdDialect dialectFor(const VirtualCamera *camera, WsdDialect probed) const;
    WsdEnvelopeInfo envelope(WsdDialect dialect, const QString &relatesTo, const QString &to);

    void handleDatagram(const QByteArray &data, const QHostAddress &from, quint16 port);
    void respond(VirtualCamera *camera, const WsdMessage &request, const QHostAddress &to,
                 quint16 port, bool resolve);
    void sendMatch(VirtualCamera *camera, const WsdMessage &request, const QHostAddress &to,
                   quint16 port, bool resolve, bool alternateXAddrs);
    void multicast(VirtualCamera *camera, const QByteArray &datagram);

    void log(const VirtualCamera *camera, const QString &summary, const QByteArray &detail,
             const QString &quirkKey = QString(), bool warning = false) const;
};

// ------------------------------------------------------------------ socket

QUdpSocket *DiscoveryResponder::Private::senderFor(VirtualCamera *camera)
{
    const QString id = camera->id();
    auto it = senders.constFind(id);
    if (it != senders.constEnd())
        return it.value();

    QUdpSocket *socket = new QUdpSocket(q);
    QHostAddress bind = camera->model().bindAddress;
    if (bind.isNull())
        bind = QHostAddress(QHostAddress::AnyIPv4);
    // 端口用临时端口：ProbeMatch 是回给 Probe 的来源端口的，源端口是几无所谓，
    // 而 N 台相机都想抢 3702 只会互相踩。
    if (!socket->bind(bind, 0, QUdpSocket::ShareAddress)) {
        log(camera,
            QStringLiteral("Cannot bind discovery reply socket (%1): %2")
                .arg(bind.toString(), socket->errorString()),
            QByteArray(), QString(), true);
        delete socket;
        return nullptr;
    }
    socket->setSocketOption(QAbstractSocket::MulticastTtlOption, 1);
    // 同机自测时客户端与模拟器在一台机器上，关掉 loopback 就收不到 Hello。
    socket->setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);

    senders.insert(id, socket);
    return socket;
}

void DiscoveryResponder::Private::dropSender(const QString &cameraId)
{
    if (QUdpSocket *socket = senders.take(cameraId))
        socket->deleteLater();
}

// ------------------------------------------------------------- 报文内容装配

WsdMatch DiscoveryResponder::Private::makeMatch(VirtualCamera *camera,
                                                const QHostAddress &peer) const
{
    const Quirks &quirks = camera->quirks();

    WsdMatch m;
    m.endpointReference = endpointReferenceOf(camera);
    m.types = wsd::advertisedTypes();
    m.scopes = camera->model().scopes;

    if (quirks.isEnabled(QuirkId::DiscoveryScopesNoName)) {
        // 上层只从 Scopes 读 onvif.org/name/ 与 /hardware/，抽掉 name 相机就变成无名的。
        QStringList kept;
        for (const QString &s : m.scopes) {
            if (!s.contains(QLatin1String("/name/")))
                kept.append(s);
        }
        m.scopes = kept;
    }

    m.xaddrs = QStringList{ camera->deviceServiceXAddr(peer) };
    if (quirks.isEnabled(QuirkId::DiscoveryBadXAddrIp)) {
        m.xaddrs = wsd::applyBadXAddr(
            m.xaddrs, quirks.paramString(QuirkId::DiscoveryBadXAddrIp, QStringLiteral("address")),
            quirks.paramBool(QuirkId::DiscoveryBadXAddrIp, QStringLiteral("first")));
    }

    m.includeXAddrs = !quirks.isEnabled(QuirkId::DiscoveryNoXAddrs);
    m.includeMetadataVersion = !quirks.isEnabled(QuirkId::DiscoveryNoMetadataVersion);
    m.metadataVersion = 1;
    return m;
}

WsdDialect DiscoveryResponder::Private::dialectFor(const VirtualCamera *camera,
                                                   WsdDialect probed) const
{
    // 默认照抄 Probe 的命名空间：客户端只认自己发出去的那一套。
    if (!camera->quirks().isEnabled(QuirkId::DiscoveryDialect))
        return probed;
    const QString value = camera->quirks().choice(QuirkId::DiscoveryDialect,
                                                  QStringLiteral("echo"));
    if (value == QLatin1String("2005"))
        return WsdDialect::Ws2005;
    if (value == QLatin1String("2009"))
        return WsdDialect::Oasis2009;
    return probed;
}

WsdEnvelopeInfo DiscoveryResponder::Private::envelope(WsdDialect dialect, const QString &relatesTo,
                                                      const QString &to)
{
    WsdEnvelopeInfo info;
    info.dialect = dialect;
    info.messageId = wsd::newMessageId();
    info.relatesTo = relatesTo;
    info.to = to;
    info.instanceId = instanceId;
    info.messageNumber = ++messageNumber;
    return info;
}

// ------------------------------------------------------------------ 收发

void DiscoveryResponder::Private::handleDatagram(const QByteArray &data, const QHostAddress &from,
                                                 quint16 port)
{
    const WsdMessage msg = parseWsdMessage(data);
    if (!msg.valid)
        return;
    // 自己发出去的 ProbeMatches、别的设备的 Hello / Bye 都不用管。
    if (!msg.isProbe() && !msg.isResolve())
        return;

    if (msg.isProbe()) {
        ++probes;
    }

    // 同一 Probe 客户端会重发 4 次（MessageID 相同），**每一份都要回** ——
    // 真机就是这么做的，客户端那边按 EPR 去重。这里刻意不做去重。
    const QList<VirtualCamera *> cameras = simulator ? simulator->cameras()
                                                     : QList<VirtualCamera *>();
    for (VirtualCamera *camera : cameras) {
        if (!camera->isRunning() || camera->isOffline())
            continue;
        if (msg.isProbe()) {
            if (!probeMatchesTypes(msg, wsd::advertisedTypes()))
                continue;
            if (!probeMatchesScopes(msg, camera->model().scopes))
                continue;
            respond(camera, msg, from, port, false);
        } else {
            // Resolve 点名要某个 EPR 的地址，认错人就别答。
            if (msg.endpointReference != endpointReferenceOf(camera))
                continue;
            respond(camera, msg, from, port, true);
        }
    }
}

void DiscoveryResponder::Private::respond(VirtualCamera *camera, const WsdMessage &request,
                                          const QHostAddress &to, quint16 port, bool resolve)
{
    const Quirks &quirks = camera->quirks();
    if (quirks.isEnabled(QuirkId::DiscoveryNoReply)) {
        log(camera, QStringLiteral("Not answering discovery, per quirk"), QByteArray(),
            QStringLiteral("discovery.no_reply"));
        return;
    }

    // 客户端的 searchServices 是「发 Probe → 纯 sleep timeout → 收结果」，
    // 延迟超过那个窗口（默认 3s）就等于没回。
    const int delayMs = quirks.enabledInt(QuirkId::DiscoveryReplyDelay, QStringLiteral("ms"));

    QPointer<VirtualCamera> guard(camera);
    auto send = [this, guard, request, to, port, resolve] {
        if (!guard || !running)
            return;
        sendMatch(guard, request, to, port, resolve, false);

        if (guard->quirks().isEnabled(QuirkId::DiscoveryReplyTwice)) {
            const bool differing = guard->quirks().paramBool(QuirkId::DiscoveryReplyTwice,
                                                             QStringLiteral("differing_xaddrs"));
            // 隔一小会儿再发第二份：客户端「同 EPR 后到覆盖先到」，
            // 挤在同一毫秒里发出去就分不出谁是后到的了。
            QTimer::singleShot(50, q, [this, guard, request, to, port, resolve, differing] {
                if (guard && running)
                    sendMatch(guard, request, to, port, resolve, differing);
            });
        }
    };

    if (delayMs > 0)
        QTimer::singleShot(delayMs, q, send);
    else
        send();
}

void DiscoveryResponder::Private::sendMatch(VirtualCamera *camera, const WsdMessage &request,
                                            const QHostAddress &to, quint16 port, bool resolve,
                                            bool alternateXAddrs)
{
    QUdpSocket *socket = senderFor(camera);
    if (!socket)
        return;

    WsdMatch match = makeMatch(camera, to);
    if (resolve) {
        // ResolveMatch 的存在意义就是补上 XAddrs（quirk A2 的下半场），
        // 这里再抽掉客户端就彻底没辙了。
        match.includeXAddrs = true;
    }
    if (alternateXAddrs && !match.xaddrs.isEmpty()) {
        // 第二份换一个明显不可达的 host，e2e 里才能验证「最后到达那份的第一个 XAddr 胜出」。
        const QStringList replaced = wsd::applyBadXAddr(QStringList{ match.xaddrs.first() },
                                                        QStringLiteral("0.0.0.0"), true);
        match.xaddrs = QStringList{ replaced.first() };
    }

    const WsdDialect dialect = dialectFor(camera, request.dialect);
    const QString replyTo = request.replyTo.isEmpty() ? wsd::anonymousAddress(dialect)
                                                      : request.replyTo;
    const WsdEnvelopeInfo info = envelope(dialect, request.messageId, replyTo);

    const QByteArray datagram = resolve ? buildResolveMatches(match, info)
                                        : buildProbeMatches(QVector<WsdMatch>{ match }, info);

    // 单播回给探测方。独立 IP 模式下这个 socket 绑在相机自己的别名地址上，
    // 客户端看到的源 IP 才与 XAddrs 对得上。
    if (socket->writeDatagram(datagram, to, port) < 0) {
        log(camera, QStringLiteral("Failed to send ProbeMatch: %1").arg(socket->errorString()),
            QByteArray(), QString(), true);
        return;
    }

    ++matches;

    // 哪条 quirk 促成了这份「怪异」的应答，写进记录里，GUI 里一眼能对上。
    QString quirkKey;
    if (!match.includeMetadataVersion)
        quirkKey = QStringLiteral("discovery.no_metadata_version");
    else if (!match.includeXAddrs)
        quirkKey = QStringLiteral("discovery.no_xaddrs");

    log(camera,
        QStringLiteral("%1 → %2:%3")
            .arg(resolve ? QStringLiteral("ResolveMatch") : QStringLiteral("ProbeMatch"),
                 to.toString())
            .arg(port),
        datagram, quirkKey);
}

void DiscoveryResponder::Private::multicast(VirtualCamera *camera, const QByteArray &datagram)
{
    QUdpSocket *socket = senderFor(camera);
    if (!socket)
        return;

    // 绑到具体地址的相机（独立 IP 模式）只从它自己那块网卡出去；绑 0.0.0.0 的
    // （端口模式）则每块支持组播的网卡都发一份，否则多网卡机器上只有默认路由
    // 那一侧的客户端能收到 Hello。
    QList<QNetworkInterface> targets;
    const QHostAddress bind = camera->model().bindAddress;
    if (!isWildcardAddress(bind)) {
        const InterfaceInfo info = netutil::interfaceForAddress(bind);
        const QNetworkInterface iface = QNetworkInterface::interfaceFromName(info.name);
        if (iface.isValid())
            targets.append(iface);
    }
    if (targets.isEmpty())
        targets = joined;

    if (targets.isEmpty()) {
        socket->writeDatagram(datagram, multicastGroup(), kWsdPort);
        return;
    }
    for (const QNetworkInterface &iface : targets) {
        socket->setMulticastInterface(iface);
        socket->writeDatagram(datagram, multicastGroup(), kWsdPort);
    }
}

void DiscoveryResponder::Private::log(const VirtualCamera *camera, const QString &summary,
                                      const QByteArray &detail, const QString &quirkKey,
                                      bool warning) const
{
    LogBus *bus = simulator ? simulator->logBus() : LogBus::global();
    if (!bus)
        return;
    LogRecord r;
    r.level = warning ? LogLevel::Warning : LogLevel::Info;
    r.ok = !warning;
    r.category = QLatin1String(logcat::Discovery);
    r.cameraId = camera ? camera->id() : QString();
    r.summary = summary;
    r.detail = QString::fromUtf8(detail);
    r.quirkKey = quirkKey;
    bus->post(r);
}

// ------------------------------------------------------------ 构造与生命周期

DiscoveryResponder::DiscoveryResponder(Simulator *simulator, QObject *parent)
    : QObject(parent), d(new Private)
{
    d->simulator = simulator;
    d->q = this;
    // AppSequence 的 InstanceId 在进程存活期间恒定、重启后变大，
    // 客户端靠它判断「设备重启过」。
    d->instanceId = static_cast<quint64>(QDateTime::currentSecsSinceEpoch());

    if (simulator) {
        // 相机被删掉后它的发送 socket 必须跟着走，否则地址一直被占着。
        connect(simulator, &Simulator::cameraRemoved, this,
                [this](const QString &id) { d->dropSender(id); });
    }
}

DiscoveryResponder::~DiscoveryResponder()
{
    stop();
    delete d;
}

bool DiscoveryResponder::start(const QString &interfaceName, QString *errorOut)
{
    if (d->running)
        return true;
    if (!d->simulator) {
        if (errorOut)
            *errorOut = QStringLiteral("没有 Simulator");
        return false;
    }

    d->listener = new QUdpSocket(this);
    // ShareAddress：3702 上常常已经有别的 ONVIF 工具在听，独占绑定会直接失败。
    if (!d->listener->bind(QHostAddress(QHostAddress::AnyIPv4), kWsdPort,
                           QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        const QString message = QStringLiteral("绑定 0.0.0.0:%1 失败：%2")
                                    .arg(kWsdPort)
                                    .arg(d->listener->errorString());
        delete d->listener;
        d->listener = nullptr;
        if (errorOut)
            *errorOut = message;
        return false;
    }
    connect(d->listener, &QUdpSocket::readyRead, this, [this] {
        while (d->listener && d->listener->hasPendingDatagrams()) {
            const QNetworkDatagram dg = d->listener->receiveDatagram();
            if (dg.isValid()) {
                d->handleDatagram(dg.data(), normalized(dg.senderAddress()),
                                  quint16(dg.senderPort()));
            }
        }
    });

    // 逐接口 join。Linux 上只 join 一次多半也能收到，Windows 上必然收不全。
    const QList<QNetworkInterface> all = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : all) {
        if (!interfaceName.isEmpty() && iface.name() != interfaceName
            && iface.humanReadableName() != interfaceName) {
            continue;
        }
        const QNetworkInterface::InterfaceFlags flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp)
            || !flags.testFlag(QNetworkInterface::IsRunning)
            || !flags.testFlag(QNetworkInterface::CanMulticast)) {
            continue;
        }
        bool hasIPv4 = false;
        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                hasIPv4 = true;
                break;
            }
        }
        if (!hasIPv4)
            continue;
        if (d->listener->joinMulticastGroup(multicastGroup(), iface))
            d->joined.append(iface);
    }

    // 一块都没加成（容器里常见）时退回默认接口，加不上才算真失败。
    if (d->joined.isEmpty() && !d->listener->joinMulticastGroup(multicastGroup())) {
        const QString message = QStringLiteral("加入组播组 239.255.255.250 失败：%1")
                                    .arg(d->listener->errorString());
        d->listener->close();
        delete d->listener;
        d->listener = nullptr;
        if (errorOut)
            *errorOut = message;
        return false;
    }

    d->running = true;
    d->log(nullptr,
           QStringLiteral("WS-Discovery listening on :%1, joined on %2 interface(s)")
               .arg(kWsdPort)
               .arg(d->joined.size()),
           QByteArray());

    // Simulator::start() 先起相机再起发现，那批 Hello 发出去时这里还没监听，
    // 所以补一轮。放到事件循环下一轮，免得在 start() 里递归回调。
    QTimer::singleShot(0, this, [this] {
        if (d->running)
            announceAllHello();
    });
    return true;
}

void DiscoveryResponder::stop()
{
    if (!d->running)
        return;

    // 先把 Bye 发出去再拆 socket：Simulator::stop() 是先停发现再停相机，
    // 顺序反了客户端就永远收不到下线通知。
    announceAllBye();
    d->running = false;

    if (d->listener) {
        for (const QNetworkInterface &iface : d->joined)
            d->listener->leaveMulticastGroup(multicastGroup(), iface);
        d->listener->close();
        d->listener->deleteLater();
        d->listener = nullptr;
    }
    d->joined.clear();

    const QList<QUdpSocket *> sockets = d->senders.values();
    for (QUdpSocket *socket : sockets)
        socket->deleteLater();
    d->senders.clear();
}

bool DiscoveryResponder::isRunning() const
{
    return d->running;
}

// ------------------------------------------------------------- Hello / Bye

void DiscoveryResponder::announceHello(VirtualCamera *camera)
{
    if (!d->running || !camera)
        return;
    // 不回发现请求的设备也不该主动喊 Hello，否则这条 quirk 只挡住一半。
    if (camera->quirks().isEnabled(QuirkId::DiscoveryNoReply))
        return;

    // Hello 没有探测方可参考，方言取 quirk 指定的那套，默认站在参照客户端一边（1.0）。
    const WsdDialect dialect = d->dialectFor(camera, WsdDialect::Ws2005);
    const WsdMatch match = d->makeMatch(camera, QHostAddress());
    const WsdEnvelopeInfo info = d->envelope(dialect, QString(), wsd::multicastTo(dialect));

    const QByteArray datagram = buildHello(match, info);
    d->multicast(camera, datagram);
    d->log(camera, QStringLiteral("Hello multicast sent"), datagram);
}

void DiscoveryResponder::announceBye(VirtualCamera *camera)
{
    if (!d->running || !camera)
        return;

    const WsdDialect dialect = d->dialectFor(camera, WsdDialect::Ws2005);
    const WsdMatch match = d->makeMatch(camera, QHostAddress());
    const WsdEnvelopeInfo info = d->envelope(dialect, QString(), wsd::multicastTo(dialect));

    const QByteArray datagram = buildBye(match, info);
    d->multicast(camera, datagram);
    d->log(camera, QStringLiteral("Bye multicast sent"), datagram);
}

void DiscoveryResponder::announceAllHello()
{
    if (!d->simulator)
        return;
    const QList<VirtualCamera *> cameras = d->simulator->cameras();
    for (VirtualCamera *camera : cameras) {
        if (camera->isRunning() && !camera->isOffline())
            announceHello(camera);
    }
}

void DiscoveryResponder::announceAllBye()
{
    if (!d->simulator)
        return;
    const QList<VirtualCamera *> cameras = d->simulator->cameras();
    for (VirtualCamera *camera : cameras) {
        if (camera->isRunning())
            announceBye(camera);
    }
}

// ------------------------------------------------------------------ 杂项

qint64 DiscoveryResponder::probesReceived() const
{
    return d->probes;
}

qint64 DiscoveryResponder::matchesSent() const
{
    return d->matches;
}

} // namespace onvifsim
