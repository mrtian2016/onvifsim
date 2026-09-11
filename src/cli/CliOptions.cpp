#include "cli/CliOptions.h"

#include "core/LogBus.h"
#include "core/Persona.h"
#include "core/Quirks.h"
#include "core/Scenario.h"
#include "core/VirtualCamera.h"
#include "version.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>

#include <csignal>
#include <cstdio>

namespace onvifsim {
namespace cli {
namespace {

const char *kHelp = R"(onvifsim —— ONVIF 摄像头模拟器

用法:
  onvifsim                                    起 GUI（未编 GUI 时等同 --headless）
  onvifsim --headless --scenario lab.json     按场景文件跑，无界面
  onvifsim --headless --cameras 8 --preset hikvision --bind 0.0.0.0
  onvifsim --headless --cameras 4 --ip-range 192.168.1.201 --iface eth0

选项:
  -H, --headless             无界面模式
      --scenario <file>      加载场景文件
      --cameras <n>          创建 n 台相机
      --preset <key>         相机预设（--list-presets 看全部）
      --bind <addr>          监听地址，默认 0.0.0.0
      --http-port <p>        HTTP 起始端口，默认 8000
      --rtsp-port <p>        RTSP 起始端口，默认 8554
      --ip-range <addr>      独立 IP 模式，起始地址（别名需已存在或有权限）
      --iface <name>         指定网卡（IP 别名与 WS-Discovery 用）
      --no-discovery         不开 WS-Discovery
      --no-control-api       不开 REST 控制面
      --control-port <p>     控制面端口，默认 9000
      --control-bind <addr>  控制面监听地址，默认 127.0.0.1（容器里要填 0.0.0.0）
      --control-token <t>    控制面访问令牌
      --quirk <k[=v]>        打开一个 quirk，可重复；形如 rtsp.max_sessions.max=1
      --assets-dir <dir>     外部预设目录，*.json 覆盖内置品牌预设
      --log-file <path>      日志落盘
      --verbose              日志带完整报文
      --list-presets         列出全部品牌预设
      --list-quirks          列出全部故障注入开关
      --list-quirks --markdown  以 Markdown 输出（生成 docs/quirks.md 用）
      --list-scenarios       列出内置场景
  -v, --version              版本号
  -h, --help                 本帮助
)";

bool takeValue(const QStringList &args, int &i, const QString &name, QString *out,
               QString *errorOut)
{
    if (i + 1 >= args.size()) {
        *errorOut = QStringLiteral("选项 %1 缺少参数").arg(name);
        return false;
    }
    *out = args.at(++i);
    return true;
}

bool takeInt(const QStringList &args, int &i, const QString &name, int *out, QString *errorOut)
{
    QString text;
    if (!takeValue(args, i, name, &text, errorOut))
        return false;
    bool ok = false;
    const int value = text.toInt(&ok);
    if (!ok) {
        *errorOut = QStringLiteral("选项 %1 的参数不是整数：%2").arg(name, text);
        return false;
    }
    *out = value;
    return true;
}

// 端口必须当场卡范围，不能靠 static_cast<quint16> 截断。
// `--http-port 99999` 截成 33903 之后照样能启动，用户拿着「端口不对」来问
// 的时候，日志里一个字的线索都没有。
bool takePort(const QStringList &args, int &i, const QString &name, quint16 *out,
              QString *errorOut)
{
    int value = 0;
    if (!takeInt(args, i, name, &value, errorOut))
        return false;
    if (value < 1 || value > 65535) {
        *errorOut = QStringLiteral("选项 %1 的端口超出 1~65535：%2").arg(name).arg(value);
        return false;
    }
    *out = static_cast<quint16>(value);
    return true;
}


// Ctrl-C 与 SIGTERM 必须走 Qt 的正常退出路径，不能让进程直接被干掉。
//
// 这不只是退出码的问题：退出前要收回的东西都挂在 aboutToQuit 上 ——
// 独立 IP 模式加的那些 IP 别名尤其要紧，进程被硬杀之后它们会留在系统网卡上，
// 用户下次启动才发现地址被自己占着。
//
// 处理函数里只能碰 volatile sig_atomic_t（这是异步信号安全的全部），
// 真正的退出动作交给主线程的定时器去做。
volatile std::sig_atomic_t g_quitRequested = 0;

extern "C" void onTerminationSignal(int)
{
    g_quitRequested = 1;
}

void installSignalHandlers(QCoreApplication &app)
{
    std::signal(SIGINT, onTerminationSignal);
    std::signal(SIGTERM, onTerminationSignal);

    auto *poll = new QTimer(&app);
    poll->setInterval(200);
    QObject::connect(poll, &QTimer::timeout, &app, [&app, poll] {
        if (g_quitRequested) {
            poll->stop();
            app.quit();
        }
    });
    poll->start();
}


} // namespace

CliOptions parse(const QStringList &arguments)
{
    CliOptions o;
    o.helpText = QString::fromUtf8(kHelp);

    for (int i = 1; i < arguments.size(); ++i) {
        const QString a = arguments.at(i);
        QString error;
        int number = 0;

        if (a == QLatin1String("-h") || a == QLatin1String("--help")) {
            o.helpRequested = true;
        } else if (a == QLatin1String("-v") || a == QLatin1String("--version")) {
            o.showVersion = true;
        } else if (a == QLatin1String("-H") || a == QLatin1String("--headless")) {
            o.headless = true;
        } else if (a == QLatin1String("--list-presets")) {
            o.listPresets = true;
        } else if (a == QLatin1String("--list-quirks")) {
            o.listQuirks = true;
        } else if (a == QLatin1String("--list-scenarios")) {
            o.listScenarios = true;
        } else if (a == QLatin1String("--markdown")) {
            o.markdownOutput = true;
        } else if (a == QLatin1String("--scenario")) {
            if (!takeValue(arguments, i, a, &o.scenarioPath, &error))
                o.errorMessage = error;
        } else if (a == QLatin1String("--cameras")) {
            if (takeInt(arguments, i, a, &number, &error))
                o.cameraCount = number;
            else
                o.errorMessage = error;
        } else if (a == QLatin1String("--preset")) {
            if (!takeValue(arguments, i, a, &o.personaKey, &error))
                o.errorMessage = error;
        } else if (a == QLatin1String("--bind")) {
            if (!takeValue(arguments, i, a, &o.bindAddress, &error))
                o.errorMessage = error;
        } else if (a == QLatin1String("--http-port")) {
            if (takePort(arguments, i, a, &o.httpBasePort, &error))
                o.httpPortExplicit = true;
            else
                o.errorMessage = error;
        } else if (a == QLatin1String("--rtsp-port")) {
            if (takePort(arguments, i, a, &o.rtspBasePort, &error))
                o.rtspPortExplicit = true;
            else
                o.errorMessage = error;
        } else if (a == QLatin1String("--ip-range")) {
            if (!takeValue(arguments, i, a, &o.ipRangeStart, &error))
                o.errorMessage = error;
        } else if (a == QLatin1String("--iface")) {
            if (!takeValue(arguments, i, a, &o.interfaceName, &error))
                o.errorMessage = error;
        } else if (a == QLatin1String("--no-discovery")) {
            o.discovery = false;
        } else if (a == QLatin1String("--no-control-api")) {
            o.controlApi = false;
        } else if (a == QLatin1String("--control-port")) {
            if (takePort(arguments, i, a, &o.controlApiPort, &error))
                o.controlPortExplicit = true;
            else
                o.errorMessage = error;
        } else if (a == QLatin1String("--control-bind")) {
            if (!takeValue(arguments, i, a, &o.controlApiBind, &error))
                o.errorMessage = error;
        } else if (a == QLatin1String("--control-token")) {
            if (!takeValue(arguments, i, a, &o.controlApiToken, &error))
                o.errorMessage = error;
        } else if (a == QLatin1String("--quirk")) {
            QString value;
            if (takeValue(arguments, i, a, &value, &error))
                o.quirkOverrides.append(value);
            else
                o.errorMessage = error;
        } else if (a == QLatin1String("--assets-dir")) {
            if (!takeValue(arguments, i, a, &o.assetsDirectory, &error))
                o.errorMessage = error;
        } else if (a == QLatin1String("--log-file")) {
            if (!takeValue(arguments, i, a, &o.logFile, &error))
                o.errorMessage = error;
        } else if (a == QLatin1String("--verbose")) {
            o.verbose = true;
        } else {
            o.errorMessage = QStringLiteral("无法识别的选项：%1\n\n%2").arg(a, o.helpText);
        }

        if (!o.errorMessage.isEmpty())
            return o;
    }

    if (!o.scenarioPath.isEmpty() && !QFileInfo::exists(o.scenarioPath))
        o.errorMessage = QStringLiteral("场景文件不存在：%1").arg(o.scenarioPath);

    return o;
}

QString quirksListing(bool markdown)
{
    QString out;
    QTextStream s(&out);

    QVector<QuirkGroup> groups{ QuirkGroup::Discovery, QuirkGroup::Auth,  QuirkGroup::Media,
                                QuirkGroup::Ptz,       QuirkGroup::Events, QuirkGroup::Rtsp,
                                QuirkGroup::Transport };

    if (markdown) {
        s << "# 故障注入全表\n\n";
        s << "> 本文件由 `onvifsim --list-quirks --markdown` 生成，不要手改。\n";
        s << "> 每条 quirk 的出处编号指向 `docs/reference-client-facts.md` §9。\n";
        s << "> **编号跟的是证据出处，不是下面的分组**：所以「建连与鉴权」一节里\n";
        s << "> 会出现 D7 / D8 这样的 D 号 —— 那两条的实证记在 §9 的 D 段里。\n";
        s << "> 编号中间有断档（如 E 组缺 E17 / E18）也是正常的：对应的条目\n";
        s << "> 属于还没实现的二期候选，编号预留着不复用。\n\n";
    }

    for (QuirkGroup g : groups) {
        const QString title = QuirkRegistry::groupTitle(g);
        const QString key = QuirkRegistry::groupKey(g);
        if (markdown)
            s << "## " << title << "（" << key << "）\n\n";
        else
            s << "== " << title << " (" << key << ") ==\n";

        for (const QuirkDef &d : QuirkRegistry::all()) {
            if (d.group != g)
                continue;
            const QString source = d.sourceId.isEmpty() ? QStringLiteral("—") : d.sourceId;
            if (markdown) {
                s << "### `" << d.key << "`\n\n";
                s << "**" << d.title << "**　出处：" << source << "\n\n";
                s << d.description << "\n\n";
                if (!d.params.isEmpty()) {
                    s << "| 参数 | 默认 | 取值 | 说明 |\n|---|---|---|---|\n";
                    for (const QuirkParamDef &p : d.params) {
                        QString range;
                        if (!p.choices.isEmpty())
                            range = p.choices.join(QStringLiteral(" / "));
                        else if (p.minValue.isValid())
                            range = QStringLiteral("%1 ~ %2")
                                        .arg(p.minValue.toString(), p.maxValue.toString());
                        else
                            range = QStringLiteral("—");
                        s << "| `" << p.name << "` | `" << p.defaultValue.toString() << "` | "
                          << range << " | " << p.description << " |\n";
                    }
                    s << "\n";
                }
            } else {
                s << QStringLiteral("  %1  [%2]  %3\n")
                         .arg(d.key, -44)
                         .arg(source, -4)
                         .arg(d.title);
                for (const QuirkParamDef &p : d.params) {
                    QString range;
                    if (!p.choices.isEmpty())
                        range = p.choices.join(QLatin1Char('|'));
                    else if (p.minValue.isValid())
                        range = QStringLiteral("%1..%2")
                                    .arg(p.minValue.toString(), p.maxValue.toString());
                    s << QStringLiteral("      .%1 = %2  %3\n")
                             .arg(p.name, -18)
                             .arg(p.defaultValue.toString(), -12)
                             .arg(range);
                }
            }
        }
        s << "\n";
    }

    if (!markdown)
        s << QStringLiteral("共 %1 项。\n").arg(QuirkRegistry::all().size());
    return out;
}

QString presetsListing()
{
    QString out;
    QTextStream s(&out);
    for (const Persona &p : PersonaRegistry::all()) {
        s << QStringLiteral("  %1  %2\n").arg(p.key, -12).arg(p.displayName);
        s << QStringLiteral("      %1 / %2 / %3\n")
                 .arg(p.manufacturer, p.model, p.firmwareVersion);
        s << QStringLiteral("      RTSP 主码流 %1\n").arg(p.rtspMainPath);
        if (!p.defaultQuirks.isEmpty()) {
            QStringList keys;
            for (QuirkId id : p.defaultQuirks)
                keys.append(QuirkRegistry::def(id).key);
            s << QStringLiteral("      默认开启 %1\n").arg(keys.join(QStringLiteral(", ")));
        }
    }
    s << QStringLiteral("共 %1 个预设。\n").arg(PersonaRegistry::all().size());
    return out;
}

QString scenariosListing()
{
    QString out;
    QTextStream s(&out);
    const QStringList names = Scenario::builtinNames();
    if (names.isEmpty()) {
        s << QStringLiteral("（没有内置场景）\n");
        return out;
    }
    for (const QString &name : names) {
        Scenario scenario;
        QStringList errors;
        if (Scenario::loadBuiltin(name, &scenario, &errors)) {
            s << QStringLiteral("  %1  %2 台相机  %3\n")
                     .arg(name, -22)
                     .arg(scenario.cameras.size(), 2)
                     .arg(scenario.description);
        } else {
            s << QStringLiteral("  %1  (读取失败)\n").arg(name, -22);
        }
    }
    return out;
}

bool apply(const CliOptions &options, Simulator *simulator, QString *errorOut)
{
    // 场景文件与命令行参数二选一：给了场景就以场景为准，
    // 命令行只能再覆盖控制面 / 日志这类进程级选项。
    if (!options.scenarioPath.isEmpty()) {
        QStringList errors;
        if (!simulator->loadScenario(options.scenarioPath, &errors)) {
            if (errorOut)
                *errorOut = errors.join(QStringLiteral("；"));
            return false;
        }
        for (const QString &warning : errors)
            simulator->logBus()->warning(logcat::Core, QString(), warning);

        SimulatorConfig config = simulator->config();
        config.controlApiEnabled = options.controlApi;
        // 和 http/rtsp 端口一样：没显式给就听场景文件的，
        // 无条件覆盖会让场景里写的 controlApiPort 永远被默认值 9000 顶掉。
        if (options.controlPortExplicit)
            config.controlApiPort = options.controlApiPort;
        if (!options.controlApiToken.isEmpty())
            config.controlApiToken = options.controlApiToken;
        if (!options.controlApiBind.isEmpty()
            && !config.controlApiAddress.setAddress(options.controlApiBind)) {
            if (errorOut)
                *errorOut = QStringLiteral("非法的控制面监听地址：%1").arg(options.controlApiBind);
            return false;
        }
        config.discoveryEnabled = options.discovery && config.discoveryEnabled;
        if (!options.assetsDirectory.isEmpty())
            config.assetsDirectory = options.assetsDirectory;
        if (!options.logFile.isEmpty())
            config.logFile = options.logFile;
        if (options.httpPortExplicit)
            config.httpBasePort = options.httpBasePort;
        if (options.rtspPortExplicit)
            config.rtspBasePort = options.rtspBasePort;
        simulator->setConfig(config);

        // 命令行显式给了端口就按它重排整场景的相机端口。场景文件里的端口是
        // 作者写死的，撞上本机已有服务时用户总得有个不改文件的出路。
        if (options.httpPortExplicit || options.rtspPortExplicit) {
            int index = 0;
            for (VirtualCamera *cam : simulator->cameras()) {
                CameraModel m = cam->model();
                if (options.httpPortExplicit)
                    m.httpPort = static_cast<quint16>(options.httpBasePort + index);
                if (options.rtspPortExplicit)
                    m.rtspPort = static_cast<quint16>(options.rtspBasePort + index);
                cam->setModel(m);
                ++index;
            }
        }

        // 绑定地址同理：--bind 给了就整场景覆盖。
        if (!options.bindAddress.isEmpty()) {
            QHostAddress bind;
            if (!bind.setAddress(options.bindAddress)) {
                if (errorOut)
                    *errorOut = QStringLiteral("非法的绑定地址：%1").arg(options.bindAddress);
                return false;
            }
            for (VirtualCamera *cam : simulator->cameras()) {
                CameraModel m = cam->model();
                m.bindAddress = bind;
                cam->setModel(m);
            }
        }
        return true;
    }

    SimulatorConfig config;
    config.discoveryEnabled = options.discovery;
    config.discoveryInterface = options.interfaceName;
    config.controlApiEnabled = options.controlApi;
    config.controlApiPort = options.controlApiPort;
    config.controlApiToken = options.controlApiToken;
    if (!options.controlApiBind.isEmpty()
        && !config.controlApiAddress.setAddress(options.controlApiBind)) {
        if (errorOut)
            *errorOut = QStringLiteral("非法的控制面监听地址：%1").arg(options.controlApiBind);
        return false;
    }
    config.httpBasePort = options.httpBasePort;
    config.rtspBasePort = options.rtspBasePort;
    config.assetsDirectory = options.assetsDirectory;
    config.logFile = options.logFile;

    if (!options.bindAddress.isEmpty() && !config.bindAddress.setAddress(options.bindAddress)) {
        if (errorOut)
            *errorOut = QStringLiteral("非法的绑定地址：%1").arg(options.bindAddress);
        return false;
    }

    // --ip-range 意味着独立 IP 模式：每台相机绑自己的地址、用标准端口。
    // 别名本身要么已经存在，要么由 REST / GUI 那边提权添加 —— 这里不擅自提权。
    QHostAddress rangeStart;
    if (!options.ipRangeStart.isEmpty()) {
        if (!rangeStart.setAddress(options.ipRangeStart)) {
            if (errorOut)
                *errorOut = QStringLiteral("非法的起始 IP：%1").arg(options.ipRangeStart);
            return false;
        }
        config.networkMode = NetworkMode::IpAlias;
    }
    simulator->setConfig(config);

    // 全局 quirk 覆盖：--quirk key / key=value / key.param=value
    Quirks global;
    for (const QString &spec : options.quirkOverrides) {
        const int eq = spec.indexOf(QLatin1Char('='));
        const QString lhs = eq < 0 ? spec : spec.left(eq);
        const QString value = eq < 0 ? QString() : spec.mid(eq + 1);

        const QuirkDef *def = QuirkRegistry::findByKey(lhs);
        QString paramName;
        if (!def) {
            // 试着按 "key.param" 拆开
            const int dot = lhs.lastIndexOf(QLatin1Char('.'));
            if (dot > 0) {
                def = QuirkRegistry::findByKey(lhs.left(dot));
                paramName = lhs.mid(dot + 1);
            }
        }
        if (!def) {
            if (errorOut)
                *errorOut = QStringLiteral("未知的 quirk：%1（用 --list-quirks 看全部）").arg(lhs);
            return false;
        }

        // 参数值要按 quirk 表里的类型和量程校验，并按类型存 ——
        // 原来一律当 QString 收下：`--quirk rtsp.max_sessions.max=abc` 照单全收，
        // 而且整型参数导出 JSON 时会变成字符串 "5"，和 REST / 场景文件写进去的
        // 数字 5 对不上。
        auto applyParam = [&](const QuirkParamDef &pdef, const QString &raw) -> bool {
            QVariant converted = raw;
            if (!pdef.choices.isEmpty()) {
                if (!pdef.choices.contains(raw)) {
                    if (errorOut) {
                        *errorOut = QStringLiteral("quirk %1 的参数 %2 只能取 %3，给的是 %4")
                                        .arg(def->key, pdef.name,
                                             pdef.choices.join(QStringLiteral(" / ")), raw);
                    }
                    return false;
                }
            } else if (pdef.defaultValue.typeId() == QMetaType::Int) {
                bool ok = false;
                const int number = raw.toInt(&ok);
                if (!ok) {
                    if (errorOut) {
                        *errorOut = QStringLiteral("quirk %1 的参数 %2 要整数，给的是 %3")
                                        .arg(def->key, pdef.name, raw);
                    }
                    return false;
                }
                if ((pdef.minValue.isValid() && number < pdef.minValue.toInt())
                    || (pdef.maxValue.isValid() && number > pdef.maxValue.toInt())) {
                    if (errorOut) {
                        *errorOut = QStringLiteral("quirk %1 的参数 %2 超出 %3~%4：%5")
                                        .arg(def->key, pdef.name)
                                        .arg(pdef.minValue.toInt())
                                        .arg(pdef.maxValue.toInt())
                                        .arg(number);
                    }
                    return false;
                }
                converted = number;
            } else if (pdef.defaultValue.typeId() == QMetaType::Bool) {
                converted = raw.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0
                    || raw == QLatin1String("1");
            }
            global.setParam(def->id, pdef.name, converted);
            return true;
        };

        global.setEnabled(def->id, true);
        if (!paramName.isEmpty()) {
            const QuirkParamDef *pdef = def->param(paramName);
            if (!pdef) {
                if (errorOut)
                    *errorOut = QStringLiteral("quirk %1 没有参数 %2").arg(def->key, paramName);
                return false;
            }
            if (!applyParam(*pdef, value))
                return false;
        } else if (!value.isEmpty()) {
            // 单参数枚举型的简写：key=value 等价于 key.value=value
            if (const QuirkParamDef *pdef = def->param(QStringLiteral("value"))) {
                if (!applyParam(*pdef, value))
                    return false;
            }
        }
    }
    simulator->setGlobalQuirks(global);

    const int count = options.cameraCount > 0 ? options.cameraCount : 1;
    for (int i = 0; i < count; ++i) {
        VirtualCamera *cam = simulator->addCamera(options.personaKey);
        if (!cam) {
            if (errorOut)
                *errorOut = QStringLiteral("创建第 %1 台相机失败").arg(i + 1);
            return false;
        }
        if (!rangeStart.isNull()) {
            // 起始地址逐台加一。IPv4 才这么算，IPv6 走二期。
            const quint32 base = rangeStart.toIPv4Address();
            CameraModel m = cam->model();
            m.bindAddress = QHostAddress(base + static_cast<quint32>(i));
            cam->setModel(m);
        }
    }
    return true;
}

int runHeadless(const CliOptions &options, QCoreApplication &app)
{
    QTextStream out(stdout);

    if (options.helpRequested) {
        out << options.helpText;
        return 0;
    }
    if (options.showVersion) {
        out << QStringLiteral("onvifsim %1\n").arg(QLatin1String(ONVIFSIM_VERSION));
        return 0;
    }
    if (options.listQuirks) {
        out << quirksListing(options.markdownOutput);
        return 0;
    }
    if (options.listPresets) {
        // --assets-dir 在这条路径上也要生效：用户改了外部预设之后，
        // 第一件事就是 --list-presets 看看有没有被吃进去。
        if (!options.assetsDirectory.isEmpty()) {
            QStringList errors;
            const QString presetDir =
                QDir(options.assetsDirectory).filePath(QStringLiteral("presets"));
            const QString target = QDir(presetDir).exists() ? presetDir : options.assetsDirectory;
            PersonaRegistry::loadOverrides(target, &errors);
            for (const QString &error : errors)
                std::fputs(qPrintable(error + QLatin1Char('\n')), stderr);
        }
        out << presetsListing();
        return 0;
    }
    if (options.listScenarios) {
        out << scenariosListing();
        return 0;
    }

    auto *simulator = new Simulator(&app);
    simulator->logBus()->setStdoutEnabled(true);
    simulator->logBus()->setStdoutVerbose(options.verbose);

    QString error;
    if (!apply(options, simulator, &error)) {
        out.flush();
        QTextStream(stderr) << error << QLatin1Char('\n');
        return 2;
    }

    // 部分相机起不来时仍然继续跑 —— 剩下的能用就有价值，但退出码要能让 CI
    // 看出有问题。注释一直是这么写的，实现却只往 stderr 打了一行、退出码照样是 0，
    // 于是 CI 里「8 台相机起来 2 台」和「全起来了」看上去一模一样。
    const bool startedCleanly = simulator->start(&error);
    if (!startedCleanly)
        QTextStream(stderr) << QStringLiteral("启动时有失败：%1\n").arg(error);

    // 退出时把 IP 别名之类的外部改动收回去。要真的跑到，就得先让
    // Ctrl-C / SIGTERM 走 Qt 的退出流程而不是直接杀进程。
    QObject::connect(&app, &QCoreApplication::aboutToQuit, simulator, [simulator] {
        simulator->stop();
    });
    installSignalHandlers(app);

    out << QStringLiteral("onvifsim 已启动，%1 台相机。Ctrl-C 退出。\n")
               .arg(simulator->cameraCount());
    out.flush();

    const int code = app.exec();
    // 正常退出（Ctrl-C / SIGTERM）时，启动阶段有过失败就回 1。
    return code != 0 ? code : (startedCleanly ? 0 : 1);
}

} // namespace cli
} // namespace onvifsim
