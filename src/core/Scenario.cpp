#include "core/Scenario.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

// 静态库里的 Qt 资源不会自动初始化，内置场景要显式注册一次。
// 资源名必须与 qrc 文件基名一致（scenarios.qrc → scenarios）。
// 注意：Q_INIT_RESOURCE 展开出的 extern 声明会被匿名 namespace 限定住，
// 导致链接期找不到真正的全局符号，所以这个函数必须待在文件作用域而不是匿名 namespace 里。
static void ensureScenarioResources()
{
    static bool done = false;
    if (!done) {
        Q_INIT_RESOURCE(scenarios);
        done = true;
    }
}

namespace onvifsim {
namespace {

QString networkModeName(NetworkMode mode)
{
    switch (mode) {
    case NetworkMode::IpAlias:  return QStringLiteral("ip_alias");
    case NetworkMode::External: return QStringLiteral("external");
    case NetworkMode::Ports:    break;
    }
    return QStringLiteral("ports");
}

NetworkMode networkModeFromName(const QString &name, bool *ok)
{
    if (ok)
        *ok = true;
    if (name == QLatin1String("ip_alias"))
        return NetworkMode::IpAlias;
    if (name == QLatin1String("external"))
        return NetworkMode::External;
    if (name == QLatin1String("ports"))
        return NetworkMode::Ports;
    if (ok)
        *ok = false;
    return NetworkMode::Ports;
}

QJsonObject configToJson(const SimulatorConfig &c)
{
    QJsonObject o;
    o.insert(QStringLiteral("networkMode"), networkModeName(c.networkMode));
    o.insert(QStringLiteral("bindAddress"), c.bindAddress.toString());
    o.insert(QStringLiteral("httpBasePort"), c.httpBasePort);
    o.insert(QStringLiteral("rtspBasePort"), c.rtspBasePort);
    o.insert(QStringLiteral("discoveryEnabled"), c.discoveryEnabled);
    if (!c.discoveryInterface.isEmpty())
        o.insert(QStringLiteral("discoveryInterface"), c.discoveryInterface);
    o.insert(QStringLiteral("controlApiEnabled"), c.controlApiEnabled);
    o.insert(QStringLiteral("controlApiAddress"), c.controlApiAddress.toString());
    o.insert(QStringLiteral("controlApiPort"), c.controlApiPort);
    return o;
}

SimulatorConfig configFromJson(const QJsonObject &o, QStringList *errors)
{
    SimulatorConfig c;
    if (o.contains(QStringLiteral("networkMode"))) {
        bool ok = true;
        c.networkMode = networkModeFromName(o.value(QStringLiteral("networkMode")).toString(), &ok);
        if (!ok && errors)
            errors->append(QCoreApplication::translate("onvifsim::core", "未知的网络模式：%1")
                               .arg(o.value(QStringLiteral("networkMode")).toString()));
    }
    const QString bind = o.value(QStringLiteral("bindAddress")).toString();
    if (!bind.isEmpty() && !c.bindAddress.setAddress(bind) && errors)
        errors->append(QCoreApplication::translate("onvifsim::core", "非法的绑定地址：%1").arg(bind));
    // 端口要卡范围而不是截断：写错的端口应该报错，不是悄悄换一个数字启动。
    auto port = [&](const char *key, quint16 fallback) -> quint16 {
        const int value = o.value(QLatin1String(key)).toInt(fallback);
        if (value < 1 || value > 65535) {
            if (errors) {
                errors->append(QCoreApplication::translate("onvifsim::core", "端口 %1 超出 1~65535：%2")
                                   .arg(QLatin1String(key)).arg(value));
            }
            return fallback;
        }
        return static_cast<quint16>(value);
    };
    if (o.contains(QStringLiteral("httpBasePort")))
        c.httpBasePort = port("httpBasePort", c.httpBasePort);
    if (o.contains(QStringLiteral("rtspBasePort")))
        c.rtspBasePort = port("rtspBasePort", c.rtspBasePort);
    if (o.contains(QStringLiteral("discoveryEnabled")))
        c.discoveryEnabled = o.value(QStringLiteral("discoveryEnabled")).toBool();
    c.discoveryInterface = o.value(QStringLiteral("discoveryInterface")).toString();
    if (o.contains(QStringLiteral("controlApiEnabled")))
        c.controlApiEnabled = o.value(QStringLiteral("controlApiEnabled")).toBool();
    const QString control = o.value(QStringLiteral("controlApiAddress")).toString();
    if (!control.isEmpty() && !c.controlApiAddress.setAddress(control) && errors)
        errors->append(QCoreApplication::translate("onvifsim::core", "非法的控制面地址：%1").arg(control));
    if (o.contains(QStringLiteral("controlApiPort")))
        c.controlApiPort = port("controlApiPort", c.controlApiPort);
    return c;
}

} // namespace

QJsonObject Scenario::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("formatVersion"), formatVersion);
    if (!name.isEmpty())
        o.insert(QStringLiteral("name"), name);
    if (!description.isEmpty())
        o.insert(QStringLiteral("description"), description);
    o.insert(QStringLiteral("config"), configToJson(config));
    const QJsonObject global = globalQuirks.toJson();
    if (!global.isEmpty())
        o.insert(QStringLiteral("globalQuirks"), global);

    QJsonArray array;
    for (const ScenarioCamera &c : cameras) {
        QJsonObject co = c.model.toJson();
        const QJsonObject q = c.quirks.toJson();
        if (!q.isEmpty())
            co.insert(QStringLiteral("quirks"), q);
        array.append(co);
    }
    o.insert(QStringLiteral("cameras"), array);
    return o;
}

Scenario Scenario::fromJson(const QJsonObject &obj, QStringList *errors)
{
    Scenario s;
    s.formatVersion = obj.value(QStringLiteral("formatVersion")).toInt(1);
    // 版本高于本程序能认的，仍然尽力解析 —— 未知字段本来就会被忽略。
    if (s.formatVersion > 1 && errors)
        errors->append(QCoreApplication::translate("onvifsim::core", "场景文件格式版本 %1 比本程序新，可能有字段被忽略")
                           .arg(s.formatVersion));

    s.name = obj.value(QStringLiteral("name")).toString();
    s.description = obj.value(QStringLiteral("description")).toString();
    s.config = configFromJson(obj.value(QStringLiteral("config")).toObject(), errors);
    s.globalQuirks = Quirks::fromJson(obj.value(QStringLiteral("globalQuirks")).toObject(), errors);

    for (const QJsonValue &v : obj.value(QStringLiteral("cameras")).toArray()) {
        const QJsonObject co = v.toObject();
        ScenarioCamera c;
        c.model = CameraModel::fromJson(co, errors);
        c.quirks = Quirks::fromJson(co.value(QStringLiteral("quirks")).toObject(), errors);
        s.cameras.append(c);
    }
    return s;
}

bool Scenario::load(const QString &path, Scenario *out, QStringList *errors)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errors)
            errors->append(QCoreApplication::translate("onvifsim::core", "无法打开场景文件 %1：%2").arg(path, f.errorString()));
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (errors)
            errors->append(QCoreApplication::translate("onvifsim::core", "场景文件不是合法 JSON（偏移 %1）：%2")
                               .arg(parseError.offset)
                               .arg(parseError.errorString()));
        return false;
    }
    if (!doc.isObject()) {
        if (errors)
            errors->append(QCoreApplication::translate("onvifsim::core", "场景文件的顶层必须是一个 JSON 对象"));
        return false;
    }
    *out = fromJson(doc.object(), errors);
    return true;
}

bool Scenario::save(const QString &path, QString *errorOut) const
{
    const QFileInfo info(path);
    if (!info.absoluteDir().exists() && !QDir().mkpath(info.absolutePath())) {
        if (errorOut)
            *errorOut = QCoreApplication::translate("onvifsim::core", "无法创建目录：%1").arg(info.absolutePath());
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut)
            *errorOut = QCoreApplication::translate("onvifsim::core", "无法写入 %1：%2").arg(path, f.errorString());
        return false;
    }
    // 写入必须查结果：磁盘满、只读挂载、配额超了都在这一步失败，而 GUI 的
    // 「另存为」就靠这个返回值决定弹「已保存」还是弹错误。不查的话，
    // 用户会拿到一个「保存成功」的提示和一个空文件。
    const QByteArray json = QJsonDocument(toJson()).toJson(QJsonDocument::Indented);
    if (f.write(json) != json.size() || !f.flush()) {
        if (errorOut)
            *errorOut = QCoreApplication::translate("onvifsim::core", "写入 %1 失败：%2").arg(path, f.errorString());
        f.close();
        return false;
    }
    f.close();
    if (f.error() != QFileDevice::NoError) {
        if (errorOut)
            *errorOut = QCoreApplication::translate("onvifsim::core", "写入 %1 失败：%2").arg(path, f.errorString());
        return false;
    }
    return true;
}

QStringList Scenario::builtinNames()
{
    ensureScenarioResources();
    QDir dir(QStringLiteral(":/scenarios"));
    QStringList names;
    for (const QString &f : dir.entryList(QStringList() << QStringLiteral("*.json"), QDir::Files))
        names.append(QFileInfo(f).completeBaseName());
    return names;
}

bool Scenario::loadBuiltin(const QString &name, Scenario *out, QStringList *errors)
{
    ensureScenarioResources();
    return load(QStringLiteral(":/scenarios/%1.json").arg(name), out, errors);
}

} // namespace onvifsim
