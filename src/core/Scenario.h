#pragma once

// 场景文件：一份 JSON 描述整套模拟环境（全局配置 + 全部相机 + 各自的 quirks）。
// GUI 保存 / 加载、REST /api/scenario、CI 里的 --scenario 都走这里。
//
// 解析一律「尽力而为」：未知字段与非法值写进 errors 列表但不中断，
// 这样旧场景文件在新版本上仍然能用。

#include "core/CameraModel.h"
#include "core/Quirks.h"
#include "core/Simulator.h"

#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

namespace onvifsim {

struct ScenarioCamera {
    CameraModel model;
    Quirks quirks;
};

struct Scenario {
    int formatVersion = 1;
    QString name;
    QString description;
    SimulatorConfig config;
    Quirks globalQuirks;
    QList<ScenarioCamera> cameras;

    QJsonObject toJson() const;
    static Scenario fromJson(const QJsonObject &obj, QStringList *errors = nullptr);

    static bool load(const QString &path, Scenario *out, QStringList *errors = nullptr);
    bool save(const QString &path, QString *errorOut = nullptr) const;

    // 内置示例场景（assets/scenarios/*.json 的名字，不带扩展名）。
    static QStringList builtinNames();
    static bool loadBuiltin(const QString &name, Scenario *out, QStringList *errors = nullptr);
};

} // namespace onvifsim
