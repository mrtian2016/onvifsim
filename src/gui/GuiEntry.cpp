#include "gui/GuiEntry.h"

#include "core/Simulator.h"
#include "gui/GuiUtil.h"
#include "gui/MainWindow.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QLibraryInfo>
#include <QtCore/QLocale>
#include <QtCore/QSettings>
#include <QtCore/QTranslator>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>

namespace onvifsim {
namespace gui {

namespace {

// 翻译文件的查找顺序：内嵌资源 → 可执行文件旁的 i18n/ → 安装前缀下的 share/ →
// 构建目录（开发期直接跑 build/bin/onvifsim 时用）。
QStringList translationDirectories()
{
    QStringList dirs;
    dirs.append(QStringLiteral(":/i18n"));
    const QString appDir = QCoreApplication::applicationDirPath();
    dirs.append(appDir + QStringLiteral("/i18n"));
    dirs.append(appDir + QStringLiteral("/../share/onvifsim/i18n"));
#ifdef ONVIFSIM_I18N_BUILD_DIR
    dirs.append(QStringLiteral(ONVIFSIM_I18N_BUILD_DIR));
#endif
    return dirs;
}

// 中英双语默认跟随系统，设置里也能锁定成某一种。
void installTranslators(QApplication &app)
{
    QSettings settings;
    const QString preference =
        settings.value(QStringLiteral("ui/language"), QStringLiteral("system")).toString();
    const QLocale locale = preference == QLatin1String("system") ? QLocale::system()
                                                                 : QLocale(preference);

    // Qt 自带的翻译，管的是「确定 / 取消」这类标准对话框按钮。
    auto *qtTranslator = new QTranslator(&app);
    if (qtTranslator->load(locale, QStringLiteral("qtbase"), QStringLiteral("_"),
                           QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        app.installTranslator(qtTranslator);
    } else {
        delete qtTranslator;
    }

    // 界面里的源字串本身是中文，中文环境下加载不到 .qm 也不影响显示 ——
    // QTranslator 找不到译文就回落到源字串。
    auto *appTranslator = new QTranslator(&app);
    const QStringList dirs = translationDirectories();
    for (const QString &dir : dirs) {
        if (appTranslator->load(locale, QStringLiteral("onvifsim"), QStringLiteral("_"), dir)) {
            app.installTranslator(appTranslator);
            return;
        }
    }
    delete appTranslator;
}

} // namespace

int run(const CliOptions &options, QApplication &app)
{
    // QSettings 要靠这两个名字定位配置文件，必须在任何 QSettings 之前设好。
    QCoreApplication::setOrganizationName(QStringLiteral("onvifsim"));
    QCoreApplication::setApplicationName(QStringLiteral("onvifsim"));
    // 让对话框、任务栏都拿到同一个图标，而不是只有主窗口有。
    QApplication::setWindowIcon(util::appIcon());
    // Wayland / GNOME 靠这个名字把窗口和 onvifsim.desktop 对上，否则任务栏里
    // 显示的是一个通用的问号图标。名字必须与 .desktop 的文件名一致。
    QGuiApplication::setDesktopFileName(QStringLiteral("onvifsim"));

    installTranslators(app);

    // 声明顺序决定析构顺序：窗口先于模拟器销毁，窗口里的观察者不会碰到已死对象。
    Simulator simulator;
    QString error;
    if (!cli::apply(options, &simulator, &error)) {
        QMessageBox::critical(nullptr, QCoreApplication::translate("onvifsim::gui", "启动失败"),
                              error);
        return 2;
    }

    QSettings settings;
    // 命令行没给场景时，可以按设置自动接着上次的场景干活。
    if (options.scenarioPath.isEmpty()
        && settings.value(QStringLiteral("scenario/autoLoad"), false).toBool()) {
        const QString last = settings.value(QStringLiteral("scenario/last")).toString();
        if (!last.isEmpty() && QFile::exists(last)) {
            QStringList errors;
            if (!simulator.loadScenario(last, &errors)) {
                simulator.logBus()->warning(logcat::Core, QString(),
                                            QCoreApplication::translate(
                                                "onvifsim::gui", "自动加载上次场景失败：%1")
                                                .arg(errors.join(QStringLiteral("；"))));
            }
        }
    }

    // 首启没有场景也没有相机时，直接给一台 Generic —— 打开就能用，
    // 不要求用户先懂「怎么加相机」。
    if (simulator.cameraCount() == 0)
        simulator.addCamera(QStringLiteral("generic"));

    if (!simulator.start(&error)) {
        // 起不来的相机不阻塞整个界面：日志里已经有详细原因，用户可以改端口后重试。
        simulator.logBus()->warning(logcat::Core, QString(), error);
    }

    MainWindow window(&simulator);
    window.show();

    // 退出前把监听端口、IP 别名这些对系统的改动收回去。
    QObject::connect(&app, &QApplication::aboutToQuit, &simulator, [&simulator] {
        simulator.stop();
    });

    return app.exec();
}

} // namespace gui
} // namespace onvifsim
