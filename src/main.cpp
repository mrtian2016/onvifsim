// 唯一入口。编进 GUI 时默认起界面，--headless 或未编 GUI 时走 CLI。

#include "cli/CliOptions.h"
#include "core/LogBus.h"
#include "core/Simulator.h"
#include "version.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>
#include <QtGui/QGuiApplication>

#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#ifdef ONVIFSIM_HAVE_GUI
#include "gui/GuiEntry.h"
#include <QtWidgets/QApplication>
#endif

namespace {

// 「只查信息、不起服务」的那些选项。**唯一真源**：wantsHeadless() 与
// isPureQuery() 原来各写了一份，加一个 --list-xxx 忘了改前一处，症状是
// 「在没有显示器的机器上直接起不来」—— 一个很难联想到这里的错。
bool isQueryOption(const QByteArray &arg)
{
    static const QByteArray kOptions[] = {
        "--list-presets", "--list-quirks", "--list-scenarios",
        "--version", "-v", "--help", "-h",
    };
    for (const QByteArray &option : kOptions) {
        if (arg == option)
            return true;
    }
    return false;
}

// 需要在建 QApplication 之前就知道走哪条路：GUI 要拉起窗口系统，
// headless 在没有显示器的容器里必须避开它。
bool wantsHeadless(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg == "--headless" || arg == "-H" || isQueryOption(arg))
            return true;
    }
#ifdef ONVIFSIM_HAVE_GUI
    return false;
#else
    return true;
#endif
}

#ifdef Q_OS_WIN
// Windows 上 GUI 子系统的程序不带控制台，从 cmd 里跑 --headless 或 --list-quirks
// 会一个字都看不到。附加到父进程已有的控制台并把 stdio 接过去；
// 双击启动时没有父控制台，AttachConsole 直接失败，也就不会弹出黑框。
//
// 更干净的用法是命令行里跑 onvifsim-cli.exe（控制台子系统，见顶层 CMakeLists），
// 这里只是让 onvifsim.exe 在命令行下也不至于哑掉。
void attachParentConsole()
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    FILE *stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    freopen_s(&stream, "CONIN$", "r", stdin);
}
#endif

// 只查参数、不建应用对象的轻量判断：这些子命令连事件循环都不需要。
bool isPureQuery(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (isQueryOption(QByteArray(argv[i])))
            return true;
    }
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    const bool headless = wantsHeadless(argc, argv);

#ifdef Q_OS_WIN
    if (headless)
        attachParentConsole();
#endif

    // headless 也要能出快照，而快照是 QImage + QPainter 画的文字 —— QFontDatabase
    // 必须有 QGuiApplication 才能用，QCoreApplication 下会直接 abort。
    // 所以无界面模式用 QGuiApplication 配 offscreen 平台插件：不需要显示器，
    // 容器和 CI 里照样能跑，也不引入任何外部依赖（插件是 Qt 自带的）。
    // 用户显式设过 QT_QPA_PLATFORM 就尊重他的选择。
    if (headless && !isPureQuery(argc, argv) && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

#ifdef ONVIFSIM_HAVE_GUI
    if (!headless) {
        QApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("onvifsim"));
        QCoreApplication::setApplicationVersion(QStringLiteral(ONVIFSIM_VERSION));
        QCoreApplication::setOrganizationName(QStringLiteral("onvifsim"));

        const onvifsim::CliOptions options = onvifsim::cli::parse(QCoreApplication::arguments());
        if (!options.errorMessage.isEmpty()) {
            std::fputs(qPrintable(options.errorMessage + QLatin1Char('\n')), stderr);
            return 2;
        }
        return onvifsim::gui::run(options, app);
    }
#endif

    // --list-* / --version / --help 不需要 GUI 栈，用最轻的 QCoreApplication，
    // 这样在完全没有图形库的机器上也能查看帮助与 quirk 清单。
    if (isPureQuery(argc, argv)) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("onvifsim"));
        QCoreApplication::setApplicationVersion(QStringLiteral(ONVIFSIM_VERSION));
        const onvifsim::CliOptions options = onvifsim::cli::parse(QCoreApplication::arguments());
        if (!options.errorMessage.isEmpty()) {
            std::fputs(qPrintable(options.errorMessage + QLatin1Char('\n')), stderr);
            return 2;
        }
        return onvifsim::cli::runHeadless(options, app);
    }

    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("onvifsim"));
    QCoreApplication::setApplicationVersion(QStringLiteral(ONVIFSIM_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("onvifsim"));

    const onvifsim::CliOptions options = onvifsim::cli::parse(QCoreApplication::arguments());
    if (!options.errorMessage.isEmpty()) {
        std::fputs(qPrintable(options.errorMessage + QLatin1Char('\n')), stderr);
        return 2;
    }
    return onvifsim::cli::runHeadless(options, app);
}
