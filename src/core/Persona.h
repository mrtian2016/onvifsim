#pragma once

// 品牌预设。决定「这台相机长得像谁」：
// 设备信息三件套、RTSP / 快照路径风格、profile 命名、topic 命名、
// 对讲轨道布局、私有 HTTP API 桩，以及默认打开哪些 quirk。
//
// 客户端按 Manufacturer / Model / FirmwareVersion 字串选厂商适配器，
// 所以这三个字段必须逐字可改。

#include "core/Quirks.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

namespace onvifsim {

// 厂商私有 API 桩的种类。
enum class VendorApi { None, Isapi, DahuaCgi, ReolinkJson, VigiJsonRpc, TplinkDs };

struct Persona {
    QString key;              // "hikvision"
    // 界面上显示的预设名，如 "海康威视 Hikvision"。
    // **这是要翻译的**：源串用 QT_TRANSLATE_NOOP 标在 Persona.cpp 的表里，
    // 显示时走 i18n::personaName()。直接把它塞进 QComboBox 的话，
    // 英文界面下会出现一列中文 —— 这个 bug 真出过。
    QString displayName;

    // 设备信息三件套（+ 序列号前缀、硬件 id）
    QString manufacturer;
    QString model;
    QString firmwareVersion;
    QString hardwareId;
    QString serialPrefix;

    // 服务路径。device_service 是客户端硬编码的，不要改；其余可换风格。
    QString devicePath = QStringLiteral("/onvif/device_service");
    QString servicePathPattern = QStringLiteral("/onvif/%1_service");  // %1 = 服务短名

    // RTSP 路径模板。%1 = 通道号(1..)，用于主 / 子 / 三码流。
    QString rtspMainPath;
    QString rtspSubPath;
    QString rtspThirdPath;
    QString snapshotPath = QStringLiteral("/onvif/snapshot");

    // profile 命名风格，见 quirk B1
    QString profileNamingStyle = QStringLiteral("main_sub");
    // topic 命名风格，见 quirk D9
    QString topicStyle = QStringLiteral("onvif");

    bool hasMedia2 = true;
    bool talkbackDualTrack = false;   // 大华风格：麦克风 recvonly 在前 + 对讲 sendonly 在后
    bool subscriptionOwnPort = false; // 订阅管理器开独立端口（D1）

    VendorApi vendorApi = VendorApi::None;
    quint16 vendorApiPort = 0;        // 0 = 与 ONVIF 同端口

    QStringList extraScopes;          // 追加到默认 scopes 之后

    // 该预设默认打开的 quirk（用户仍可逐项覆盖）。
    QVector<QuirkId> defaultQuirks;

    QJsonObject toJson() const;
};

class PersonaRegistry
{
public:
    // 内置预设：generic / hikvision / dahua / reolink / vigi / tplink / axis / uniview
    static const QVector<Persona> &all();
    static const Persona *find(const QString &key);
    static const Persona &generic();
    static QStringList keys();

    // 运行时用外部 JSON 覆盖内置预设（assets/presets/*.json）。
    static bool loadOverrides(const QString &directory, QStringList *errors = nullptr);
};

} // namespace onvifsim
