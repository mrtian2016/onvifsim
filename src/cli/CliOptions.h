#pragma once

// 命令行解析与 headless 入口。
//
//   onvifsim                                   GUI，首启自动建 1 台 Generic 相机并开始
//   onvifsim --headless --scenario lab.json
//   onvifsim --headless --cameras 8 --preset hikvision --bind 0.0.0.0
//   onvifsim --headless --cameras 4 --ip-range 192.168.1.201 --iface eth0
//   onvifsim --list-presets | --list-quirks | --version

#include "core/Simulator.h"

#include <QtCore/QString>
#include <QtCore/QStringList>

class QCoreApplication;

namespace onvifsim {

struct CliOptions {
    bool headless = false;
    bool showVersion = false;
    bool listPresets = false;
    bool listQuirks = false;
    bool listScenarios = false;
    // --list-quirks --markdown 用来生成 docs/quirks.md
    bool markdownOutput = false;

    QString scenarioPath;
    int cameraCount = 0;
    QString personaKey = QStringLiteral("generic");
    QString bindAddress;
    quint16 httpBasePort = 8000;
    quint16 rtspBasePort = 8554;
    // 是否是用户在命令行里显式给的。加载场景文件时，显式给的端口要能覆盖
    // 场景里写死的那套 —— 否则场景一旦和本机已有服务撞端口就没法用了。
    bool httpPortExplicit = false;
    bool rtspPortExplicit = false;

    QString ipRangeStart;
    QString interfaceName;

    bool controlApi = true;
    // 控制面默认只听回环 —— 它能改整台设备的行为，不该随手暴露到网上。
    // 容器里要从宿主访问就得显式 --control-bind 0.0.0.0。
    QString controlApiBind;
    quint16 controlApiPort = 9000;
    bool controlPortExplicit = false;
    QString controlApiToken;

    bool discovery = true;
    // 外部预设目录。里面的 *.json 会覆盖内置品牌预设，没写到的字段保留默认。
    QString assetsDirectory;
    QString logFile;
    bool verbose = false;
    QStringList quirkOverrides;   // "key" 或 "key=value" 或 "key.param=value"

    QString errorMessage;         // 非空表示解析失败
    bool helpRequested = false;
    QString helpText;
};

namespace cli {

CliOptions parse(const QStringList &arguments);
// 把 options 应用到 simulator（建相机、配网络、加载场景）。
bool apply(const CliOptions &options, Simulator *simulator, QString *errorOut);

// --list-* 的输出。
QString presetsListing();
QString quirksListing(bool markdown);
QString scenariosListing();

// headless 主循环。返回进程退出码。
int runHeadless(const CliOptions &options, QCoreApplication &app);

} // namespace cli
} // namespace onvifsim
