#pragma once

// SOAP Fault 生成。子码、措辞、HTTP 状态都可被 quirk 改写（D7 / D8）。
//
// 客户端靠关键词匹配判定认证错误：notauthorized / not authorized / unauthorized /
// authentication / sender not authorized / failedauthentication。
// AuthFaultWording 的 Unknown 档故意一个都不含，用来测客户端的兜底分支。

#include <QtCore/QByteArray>
#include <QtCore/QString>

namespace onvifsim {

class Quirks;

namespace ter {
// ONVIF 标准错误子码（ter: 前缀）
inline constexpr const char *NotAuthorized = "ter:NotAuthorized";
inline constexpr const char *ActionNotSupported = "ter:ActionNotSupported";
inline constexpr const char *OperationProhibited = "ter:OperationProhibited";
inline constexpr const char *InvalidArgVal = "ter:InvalidArgVal";
inline constexpr const char *InvalidArgs = "ter:InvalidArgs";
inline constexpr const char *NoProfile = "ter:NoProfile";
inline constexpr const char *NoConfig = "ter:NoConfig";
inline constexpr const char *NoToken = "ter:NoToken";
inline constexpr const char *NoPTZProfile = "ter:NoPTZProfile";
inline constexpr const char *NoVideoSource = "ter:NoVideoSource";
inline constexpr const char *NoAudioSource = "ter:NoAudioSource";
inline constexpr const char *NoAudioOutput = "ter:NoAudioOutput";
inline constexpr const char *NoSuchPreset = "ter:NoSuchPreset";
inline constexpr const char *TooManyPresets = "ter:TooManyPresets";
inline constexpr const char *PresetExist = "ter:PresetExist";
inline constexpr const char *ConfigModify = "ter:ConfigModify";
inline constexpr const char *ConfigurationConflict = "ter:ConfigurationConflict";
inline constexpr const char *MaxPullPoints = "ter:MaxPullPoints";
inline constexpr const char *InvalidFilterFault = "ter:InvalidFilterFault";
inline constexpr const char *UnacceptableTerminationTime = "ter:UnacceptableTerminationTime";
inline constexpr const char *ResourceUnknownFault = "ter:ResourceUnknownFault";
inline constexpr const char *NoHomePosition = "ter:NoHomePosition";
inline constexpr const char *MaxNVTProfiles = "ter:MaxNVTProfiles";
inline constexpr const char *DeletionOfFixedProfile = "ter:DeletionOfFixedProfile";
inline constexpr const char *ProfileExists = "ter:ProfileExists";
inline constexpr const char *FixedScope = "ter:FixedScope";
inline constexpr const char *UsernameClash = "ter:UsernameClash";
inline constexpr const char *UsernameMissing = "ter:UsernameMissing";
inline constexpr const char *PasswordTooWeak = "ter:PasswordTooWeak";
inline constexpr const char *TooManyUsers = "ter:TooManyUsers";
// Receiver 类故障的通用子码。ONVIF 里 env:Receiver 配的就是 ter:Action，
// 不是笔误：把 SoapFault::senderFault 置 false 再配这个子码即可。
inline constexpr const char *Receiver = "ter:Action";
} // namespace ter

struct SoapFault {
    QString subcode = QString::fromLatin1(ter::ActionNotSupported);
    QString reason;
    QString detail;
    bool senderFault = true;   // false → Receiver
    int httpStatus = 0;        // 0 = 按 quirk / 默认决定
};

namespace soap {

QByteArray makeFault(int soapVersion, const SoapFault &fault);

// 按 quirk D8 生成鉴权 Fault（子码与措辞随之变化）。
SoapFault authFault(const Quirks &quirks);

// 按 quirk D7 决定 Fault 走哪个 HTTP 状态码。
int faultHttpStatus(const Quirks &quirks, const SoapFault &fault);

SoapFault notSupported(const QString &operation);
SoapFault invalidArg(const QString &what);
SoapFault noProfile(const QString &token);

} // namespace soap
} // namespace onvifsim
