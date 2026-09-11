#include "net/IpAlias.h"

#include "core/LogBus.h"
#include "net/NetUtil.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QStringList>
#include <QtNetwork/QNetworkInterface>

#if !defined(Q_OS_WIN)
#include <unistd.h>
#endif

namespace onvifsim {
namespace {

constexpr int kDirectTimeoutMs = 15000;
// 提权那一步会弹密码框 / UAC，得给足人类反应时间。
constexpr int kElevatedTimeoutMs = 180000;

// 一条别名命令的两种形态：program+args 给 QProcess 跑（不经 shell，省掉转义），
// display 给用户照抄。两者必须描述同一件事。
struct AliasCommand {
    QString program;
    QStringList args;
    QString display;
};

// 只有 Linux 分支用得上：`ip addr add` 要的是 CIDR 前缀长度，
// 而 macOS 的 ifconfig 与 Windows 的 netsh 都直接收点分十进制掩码。
// 不加平台守卫的话，在那两个平台上它就是个死函数，-Werror 下直接编不过。
#if defined(Q_OS_LINUX)
int prefixLength(const QHostAddress &netmask)
{
    if (netmask.protocol() != QAbstractSocket::IPv4Protocol)
        return 24;
    quint32 mask = netmask.toIPv4Address();
    int bits = 0;
    while (mask & 0x80000000u) {
        ++bits;
        mask <<= 1;
    }
    return bits;
}
#endif

// 把一个参数包成 sh 能安全解析的单引号形式。单引号里除了单引号本身没有任何
// 元字符有意义，所以这是最省心的做法：'it'\''s' 这种拼法闭合再转义再续上。
QString shellQuote(const QString &value)
{
    QString quoted = value;
    quoted.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

// 提权时要把命令交给 sh，只能在这里把 program+args 逐个引起来 ——
// **绝不能拿 AliasCommand::display 去拼**：那是给人看的字符串，网卡名之类
// 是原样插值进去的，一个 `; curl x|sh` 就是以 root 执行任意命令。
QString shellLine(const AliasCommand &cmd)
{
    QStringList parts;
    parts.reserve(cmd.args.size() + 1);
    parts.append(shellQuote(cmd.program));
    for (const QString &arg : cmd.args)
        parts.append(shellQuote(arg));
    return parts.join(QLatin1Char(' '));
}

AliasCommand aliasCommand(const IpAliasRequest &request, bool add)
{
    AliasCommand cmd;
    const QString iface = request.interfaceName;
    const QString ip = request.address.toString();
    const QString mask = request.netmask.toString();

#if defined(Q_OS_LINUX)
    cmd.program = QStringLiteral("ip");
    const QString cidr = ip + QLatin1Char('/') + QString::number(prefixLength(request.netmask));
    cmd.args = { QStringLiteral("addr"), add ? QStringLiteral("add") : QStringLiteral("del"),
                 cidr, QStringLiteral("dev"), iface };
    cmd.display = QStringLiteral("ip addr %1 %2 dev %3")
                      .arg(add ? QStringLiteral("add") : QStringLiteral("del"), cidr, iface);
#elif defined(Q_OS_MACOS)
    cmd.program = QStringLiteral("ifconfig");
    if (add) {
        cmd.args = { iface, QStringLiteral("alias"), ip, mask };
        cmd.display = QStringLiteral("ifconfig %1 alias %2 %3").arg(iface, ip, mask);
    } else {
        cmd.args = { iface, QStringLiteral("-alias"), ip };
        cmd.display = QStringLiteral("ifconfig %1 -alias %2").arg(iface, ip);
    }
#elif defined(Q_OS_WIN)
    cmd.program = QStringLiteral("netsh");
    if (add) {
        cmd.args = { QStringLiteral("interface"), QStringLiteral("ip"), QStringLiteral("add"),
                     QStringLiteral("address"), iface, ip, mask };
        cmd.display = QStringLiteral("netsh interface ip add address \"%1\" %2 %3")
                          .arg(iface, ip, mask);
    } else {
        cmd.args = { QStringLiteral("interface"), QStringLiteral("ip"), QStringLiteral("delete"),
                     QStringLiteral("address"), iface, ip };
        cmd.display = QStringLiteral("netsh interface ip delete address \"%1\" %2").arg(iface, ip);
    }
#else
    Q_UNUSED(add)
    Q_UNUSED(mask)
    cmd.display = QStringLiteral("（当前平台没有内置的 IP 别名命令）");
#endif
    return cmd;
}

// 不提权直接跑。Linux 上进程带 CAP_NET_ADMIN（或本来就是 root）时这一步就成了，
// 用户一次密码都不用输。
bool runDirect(const AliasCommand &cmd, QString *errorOut)
{
    if (cmd.program.isEmpty()) {
        if (errorOut)
            *errorOut = QCoreApplication::translate("onvifsim::core", "当前平台不支持自动加 IP 别名");
        return false;
    }
    if (QStandardPaths::findExecutable(cmd.program).isEmpty()) {
        if (errorOut)
            *errorOut = QCoreApplication::translate("onvifsim::core", "找不到 %1 命令").arg(cmd.program);
        return false;
    }
    QProcess process;
    process.start(cmd.program, cmd.args);
    if (!process.waitForFinished(kDirectTimeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        if (errorOut)
            *errorOut = QCoreApplication::translate("onvifsim::core", "%1 执行超时").arg(cmd.program);
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errorOut) {
            const QString stderrText =
                QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
            *errorOut = stderrText.isEmpty()
                ? QCoreApplication::translate("onvifsim::core", "%1 返回 %2").arg(cmd.program).arg(process.exitCode())
                : stderrText;
        }
        return false;
    }
    return true;
}

// 提权跑一批命令。整批合成一次调用，别让用户为 8 台相机输 8 遍密码。
//
// 收的是 AliasCommand 而不是拼好的字符串：脚本必须在这里用 shellQuote() 现拼，
// 外部传进来的任何字符串都不可信。
bool runElevated(const QList<AliasCommand> &commands, QString *errorOut)
{
    if (commands.isEmpty())
        return true;
    QStringList quoted;
    quoted.reserve(commands.size());
    for (const AliasCommand &cmd : commands)
        quoted.append(shellLine(cmd));
    const QString script = quoted.join(QStringLiteral(" && "));

#if defined(Q_OS_LINUX)
    if (QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty()) {
        if (errorOut)
            *errorOut = QCoreApplication::translate("onvifsim::core", "没有 pkexec，无法自动提权");
        return false;
    }
    QProcess process;
    process.start(QStringLiteral("pkexec"),
                  { QStringLiteral("sh"), QStringLiteral("-c"), script });
#elif defined(Q_OS_MACOS)
    // osascript 的字符串里 \ 与 " 都要转义。
    QString escaped = script;
    escaped.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    escaped.replace(QLatin1Char('"'), QLatin1String("\\\""));
    QProcess process;
    process.start(QStringLiteral("osascript"),
                  { QStringLiteral("-e"),
                    QStringLiteral("do shell script \"%1\" with administrator privileges")
                        .arg(escaped) });
#else
    // Windows 没有命令行提权入口：进程本身没被 UAC 提升就只能让用户自己来。
    if (errorOut)
        *errorOut = QCoreApplication::translate("onvifsim::core", "请以管理员身份重新运行，或手动执行下面的命令");
    return false;
#endif

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    if (!process.waitForFinished(kElevatedTimeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        if (errorOut)
            *errorOut = QCoreApplication::translate("onvifsim::core", "提权执行超时（密码框没被处理？）");
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errorOut) {
            const QString stderrText =
                QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
            *errorOut = stderrText.isEmpty() ? QCoreApplication::translate("onvifsim::core", "提权执行失败（用户取消？）")
                                             : stderrText;
        }
        return false;
    }
    return true;
#endif
}

// 给用户照抄的形态：Unix 上补个 sudo，Windows 上靠 elevationHint() 提示开管理员窗口。
QStringList manualForm(const QStringList &commandLines)
{
#if defined(Q_OS_WIN)
    return commandLines;
#else
    QStringList result;
    result.reserve(commandLines.size());
    for (const QString &line : commandLines)
        result.append(QStringLiteral("sudo ") + line);
    return result;
#endif
}

struct ApplyOutcome {
    bool ok = false;
    int doneCount = 0;           // 前 doneCount 条已经生效
    QString error;
    QStringList pending;         // 没成功的命令原文
};

// 统一的执行路径：先逐条不提权试（有 CAP_NET_ADMIN 就够了），
// 第一条卡住就把「剩下的全部」合成一次提权调用。
ApplyOutcome applyCommands(const QList<IpAliasRequest> &requests, bool add)
{
    ApplyOutcome outcome;
    QList<AliasCommand> commands;
    commands.reserve(requests.size());
    for (const IpAliasRequest &request : requests)
        commands.append(aliasCommand(request, add));

    QString directError;
    int index = 0;
    for (; index < commands.size(); ++index) {
        if (!runDirect(commands.at(index), &directError))
            break;
    }
    outcome.doneCount = index;
    if (index == commands.size()) {
        outcome.ok = true;
        return outcome;
    }

    QList<AliasCommand> rest;
    QStringList restDisplay;
    for (int i = index; i < commands.size(); ++i) {
        rest.append(commands.at(i));
        restDisplay.append(commands.at(i).display);
    }

    QString elevatedError;
    if (runElevated(rest, &elevatedError)) {
        outcome.doneCount = commands.size();
        outcome.ok = true;
        return outcome;
    }

    // 绝不静默降级：把没做成的命令原样交出去，用户自己敲照样能用。
    outcome.error = QStringLiteral("直接执行失败（%1）；提权执行也失败（%2）")
                        .arg(directError, elevatedError);
    outcome.pending = restDisplay;
    return outcome;
}

} // namespace

struct IpAlias::Private
{
    QList<IpAliasRequest> owned;
};

IpAlias::IpAlias(QObject *parent)
    : QObject(parent), d(new Private)
{
}

IpAlias::~IpAlias()
{
    // 这里不自动回收：析构可能发生在退出流程的任意时刻，
    // 突然弹一个 pkexec 密码框比留下几个别名更糟。回收由调用方显式做。
    if (!d->owned.isEmpty()) {
        if (LogBus *bus = LogBus::global()) {
            bus->warning(logcat::Core, QString(),
                         QStringLiteral("IpAlias destroyed with %1 alias(es) still owned; "
                                        "removeAllOwned() should be called before exit")
                             .arg(d->owned.size()));
        }
    }
    delete d;
}

bool IpAlias::isValidInterfaceName(const QString &name)
{
    if (name.isEmpty())
        return false;
#if defined(Q_OS_WIN)
    // Windows 的适配器友好名可以带空格、括号、中文（"以太网"、"Wi-Fi 2"），
    // 没法按字符集卡。好在这个平台上 netsh 只经 QProcess 的参数数组，
    // 不过 shell，所以只需要拦住控制字符和引号。
    if (name.size() > 128)
        return false;
    for (const QChar ch : name) {
        if (ch.unicode() < 0x20 || ch == QLatin1Char('"'))
            return false;
    }
    return true;
#else
    // Linux 的 IFNAMSIZ 是 16（含结尾 \0），macOS 同量级。真实网卡名只会是
    // eth0 / enp3s0 / en0 / bridge0 / vlan10 / eth0.100 / eth0:1 这些形状。
    if (name.size() > 15)
        return false;
    for (const QChar ch : name) {
        const bool ok = (ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
            || (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
            || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
            || ch == QLatin1Char('_') || ch == QLatin1Char('.')
            || ch == QLatin1Char('-') || ch == QLatin1Char(':');
        if (!ok)
            return false;
    }
    return true;
#endif
}

QList<IpAliasRequest> IpAlias::ownedAliases() const
{
    return d->owned;
}

IpAliasResult IpAlias::add(const IpAliasRequest &request)
{
    IpAliasResult result;
    if (request.interfaceName.isEmpty() || request.address.isNull()) {
        result.errorMessage = QCoreApplication::translate("onvifsim::core", "网卡名或地址为空");
        return result;
    }
    if (!isValidInterfaceName(request.interfaceName)) {
        result.errorMessage = QCoreApplication::translate("onvifsim::core", "网卡名不合法：%1").arg(request.interfaceName);
        return result;
    }
    if (aliasExists(request.interfaceName, request.address)) {
        // 用户自己加好的，我们只管用、不接管（退出时也不会去删）。
        result.ok = true;
        return result;
    }

    const ApplyOutcome outcome = applyCommands({ request }, true);
    if (outcome.ok) {
        d->owned.append(request);
        result.ok = true;
        return result;
    }
    result.errorMessage = outcome.error + QLatin1Char('\n') + elevationHint();
    result.manualCommands = manualForm(outcome.pending);
    return result;
}

IpAliasResult IpAlias::remove(const IpAliasRequest &request)
{
    IpAliasResult result;
    if (request.interfaceName.isEmpty() || request.address.isNull()) {
        result.errorMessage = QCoreApplication::translate("onvifsim::core", "网卡名或地址为空");
        return result;
    }
    if (!isValidInterfaceName(request.interfaceName)) {
        result.errorMessage = QCoreApplication::translate("onvifsim::core", "网卡名不合法：%1").arg(request.interfaceName);
        return result;
    }

    const ApplyOutcome outcome = applyCommands({ request }, false);
    if (outcome.ok) {
        for (int i = 0; i < d->owned.size(); ++i) {
            if (d->owned.at(i).address == request.address
                && d->owned.at(i).interfaceName == request.interfaceName) {
                d->owned.removeAt(i);
                break;
            }
        }
        result.ok = true;
        return result;
    }
    result.errorMessage = outcome.error + QLatin1Char('\n') + elevationHint();
    result.manualCommands = manualForm(outcome.pending);
    return result;
}

IpAliasResult IpAlias::addRange(const QString &interfaceName, const QHostAddress &start, int count,
                                const QHostAddress &netmask)
{
    IpAliasResult result;
    if (count <= 0 || start.protocol() != QAbstractSocket::IPv4Protocol) {
        result.errorMessage = QCoreApplication::translate("onvifsim::core", "起始地址必须是 IPv4，台数必须为正");
        return result;
    }
    if (!isValidInterfaceName(interfaceName)) {
        result.errorMessage = QCoreApplication::translate("onvifsim::core", "网卡名不合法：%1").arg(interfaceName);
        return result;
    }
    // 一次别名申请就想铺满一个 /24 的，基本只会是手滑或者恶意。
    if (count > 254) {
        result.errorMessage = QCoreApplication::translate("onvifsim::core", "一次最多分配 254 个地址（要了 %1 个）").arg(count);
        return result;
    }

    QList<IpAliasRequest> requests;
    const quint32 base = start.toIPv4Address();
    for (int i = 0; i < count; ++i) {
        IpAliasRequest request;
        request.interfaceName = interfaceName;
        request.address = QHostAddress(base + quint32(i));
        if (!netmask.isNull())
            request.netmask = netmask;
        if (aliasExists(interfaceName, request.address))
            continue;
        requests.append(request);
    }
    if (requests.isEmpty()) {
        result.ok = true;
        return result;
    }

    // 整段一次提权：加 8 台相机不该弹 8 次密码框。
    const ApplyOutcome outcome = applyCommands(requests, true);
    for (int i = 0; i < outcome.doneCount; ++i)
        d->owned.append(requests.at(i));
    if (outcome.ok) {
        result.ok = true;
        return result;
    }
    result.errorMessage = outcome.error + QLatin1Char('\n') + elevationHint();
    result.manualCommands = manualForm(outcome.pending);
    return result;
}

void IpAlias::removeAllOwned()
{
    const QList<IpAliasRequest> owned = d->owned;
    if (owned.isEmpty())
        return;

    const ApplyOutcome outcome = applyCommands(owned, false);
    d->owned = owned.mid(outcome.doneCount);

    if (!outcome.ok) {
        if (LogBus *bus = LogBus::global()) {
            bus->warning(logcat::Core, QString(),
                         QStringLiteral("Failed to release IP aliases: %1").arg(outcome.error),
                         manualForm(outcome.pending).join(QLatin1Char('\n')));
        }
    }
}

bool IpAlias::aliasExists(const QString &interfaceName, const QHostAddress &address)
{
    const InterfaceInfo info = netutil::interfaceByName(interfaceName);
    for (const QHostAddress &candidate : info.addresses) {
        if (candidate == address)
            return true;
    }
    return false;
}

QStringList IpAlias::commandsFor(const IpAliasRequest &request, bool add)
{
    return { aliasCommand(request, add).display };
}

bool IpAlias::needsElevation()
{
#if defined(Q_OS_WIN)
    // 拿不到「当前进程是否已提权」的便宜答案，一律按需要处理；
    // 真提升过的话直接执行会成功，这个返回值只影响提示文案。
    return true;
#else
    // Linux 上带 CAP_NET_ADMIN 的非 root 进程也能加别名，所以这只是个提示：
    // 真正的判断是「先不提权试一把」（见 applyCommands）。
    return geteuid() != 0;
#endif
}

QString IpAlias::elevationHint()
{
#if defined(Q_OS_LINUX)
    return QStringLiteral("加 IP 别名需要 root：程序会用 pkexec 提权；"
                          "也可以给可执行文件 setcap cap_net_admin+ep，或手动执行下面的命令。");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("加 IP 别名需要管理员权限：程序会用 osascript 弹授权框，"
                          "也可以手动执行下面的命令。");
#elif defined(Q_OS_WIN)
    return QStringLiteral("加 IP 别名需要管理员权限：请以管理员身份重新运行程序，"
                          "或在管理员命令提示符里执行下面的命令。");
#else
    return QStringLiteral("当前平台需要手动配置 IP 别名。");
#endif
}

} // namespace onvifsim
