#pragma once

// 故障注入（Quirks）——— 本项目的核心。
//
// 每一条 quirk 都来自 docs/reference-client-facts.md §9 记录的真机实证，
// 并带一个出处编号（A1 / E7 / D5 …）。不要凭空新增。
//
// 新增一条 quirk 必须同时补齐：
//   1. 这里的 QuirkId 枚举项
//   2. Quirks.cpp 里的元数据表（key / 分组 / 出处 / 中文标题与说明 / 参数）
//   3. 消费它的模块代码
//   4. tests/e2e/ 里一条断言
// docs/quirks.md 与 --list-quirks 都从这张表生成，不要手改生成物。

#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>
#include <QtCore/QVector>

namespace onvifsim {

enum class QuirkGroup {
    Discovery,   // A. 发现
    Auth,        // A/D. 建连与鉴权
    Media,       // B. Media 与快照
    Ptz,         // C. PTZ
    Events,      // D. 事件
    Rtsp,        // E. RTSP 与对讲
    Transport,   // F. 传输与设备
};

enum class QuirkId {
    // ---- A. 发现 -------------------------------------------------------
    DiscoveryDialect,           // A1  强制 WS-Discovery 命名空间方言
    DiscoveryNoXAddrs,          // A2  ProbeMatch 不带 XAddrs，逼客户端发 Resolve
    DiscoveryNoMetadataVersion, // A3  缺 MetadataVersion（客户端会整包丢弃）
    DiscoveryBadXAddrIp,        // A4  XAddrs 里放不可达 IP / 0.0.0.0 / 主机名
    DiscoveryNoReply,           //     不回 ProbeMatch
    DiscoveryReplyDelay,        //     延迟回复
    DiscoveryReplyTwice,        //     同一 Probe 回两次
    DiscoveryScopesNoName,      //     Scopes 里不带 onvif.org/name/

    // ---- A. 建连与鉴权 -------------------------------------------------
    XAddrOddPort,               // A5  XAddr 报非常规端口（真机 :2020 而实连 80）
    ServicesMedia2First,        // A6  GetServices 里 Media2 排在 Media 前面
    NoGetServices,              // A7  不实现 GetServices
    AuthTightTimeWindow,        // A8  UsernameToken Created 时间窗收紧
    SubscriptionNeverExpires,   // A9  订阅只增不回收，复现槽位泄漏
    DeviceInfoMissingFields,    // A10 GetDeviceInformation 缺字段
    AuthPasswordTextOnly,       //     只接受 PasswordText，拒 PasswordDigest
    AuthNonceStrictOnce,        //     nonce 严格一次性，重放即 401
    AuthPreAuthRequired,        //     连 PRE_AUTH 操作也要鉴权
    AuthHttp401NotFault,        //     鉴权失败回 HTTP 401 而不是 SOAP Fault
    FaultHttpStatus,            // D7  SOAP Fault 走 HTTP 200 还是 500
    AuthFaultWording,           // D8  鉴权 Fault 的措辞变体

    // ---- B. Media 与快照 -----------------------------------------------
    ProfileNamingStyle,         // B1  profile 命名风格（无主子语义）
    StreamUriWithUserinfo,      // B2  GetStreamUri 返回 URI 自带 user:pass@
    StreamUriNested,            // B3  URI 套在 MediaUri/Uri 下
    StreamUriPlaceholderIp,     //     URI 用占位 IP（192.168.1.99 / 0.0.0.0）
    SnapshotEmptyBody,          // B4  200 + image/jpeg + 空体
    SnapshotAuthMode,           // B5  快照鉴权方式（无 / Basic / Digest / 一次性 token）
    SnapshotUriRotates,         // B6  快照 URI 定期轮换失效
    NoGetSnapshotUri,           // B7  不实现 GetSnapshotUri
    CodecMismatch,              //     声明的音频编码与实发不一致
    ResolutionMismatch,         //     声明分辨率与实际码流不符

    // ---- C. PTZ ---------------------------------------------------------
    PtzConfigOnSubOnly,         // C1  PTZConfiguration 只挂子码流
    PtzUsableButUnadvertised,   // C2  一个 profile 都不挂但 PTZ 照常可用
    PtzSpacesEmpty,             // C3  SupportedPTZSpaces 为空
    PtzRangeMinEqualsMax,       // C4  Range 的 Min == Max
    PtzZoomMalformedResponse,   // C5  非零 Zoom 触发畸形 HTTP 响应
    PtzFactory300Presets,       // C6  出厂预填 300 个预置位槽
    PtzPresetPercentEncoded,    // C7  预置位名百分号编码原样吐回
    PtzNoGetPresets,            // C8  GetPresets 回 ActionNotSupported
    PtzSetPresetReturnShape,    // C9  SetPreset 返回裸字符串还是对象
    PtzResponseJitter,          // C10 SOAP 响应延迟抖动，测命令乱序防护
    PtzGotoPresetSlow,          //     GotoPreset 特别慢
    PtzMoveWithoutStatusChange, //     移动了但 GetStatus 不变

    // ---- D. 事件 ---------------------------------------------------------
    SubscriptionPortIncrement,  // D1  订阅管理器开在独立端口且每次递增
    SubscriptionHostUnreachable,// D2  订阅地址 host 报不可达的内网 IP
    SubscriptionSlotLimit,      // D3  订阅槽位上限，超限 Fault 或踢最老
    NoGetEventProperties,       // D4  不实现 GetEventProperties
    EventPropertiesBadXml,      // D5  返回属性值不加引号的非法 XML（TP-Link 真机）
    SubscriptionFlatAddress,    // D6  Address 直接放响应下，不套 SubscriptionReference
    TopicNamingStyle,           // D9  topic 命名风格（onvif / tplink / reolink / axis）
    PullMessagesAlwaysEmpty,    // D10 PullMessages 永远返回空
    EventStateNotPaired,        // D11 属性型事件只发 true，不发配对的 false
    RenewFails,                 //     Renew 总是失败
    SubscriptionExpiresAtOnce,  //     订阅建完立刻过期
    EventStorm,                 //     事件风暴（每秒 N 条）
    NoSetSynchronizationPoint,  //     不支持 SetSynchronizationPoint

    // ---- E. RTSP 与对讲 --------------------------------------------------
    AudioCapabilityLie,         // E1  声明 AAC/G722 实发 PCMU
    G722SampleRate8000,         // E2  G.722 采样率错写成 8000
    BitrateUnitVariant,         // E3  码率单位 kbps / bps
    AudioDecoderOptionsShape,   // E4  GetAudioDecoderConfigurationOptions 响应形态
    SampleRateShape,            // E5  采样率字段形态（List / Range / 单值）
    TalkbackDualTrack,          // E6  对讲双轨布局（麦克风 recvonly + 对讲 sendonly）
    TalkbackBusySlot,           // E7  TEARDOWN 后 N 秒内新 DESCRIBE 回 401
    AuthDualChallenge,          // E8  同时发 Basic + Digest 两条 WWW-Authenticate
    AuthStrictDigestParams,     // E9  Authorization 出现挑战没给的参数就 401
    SdpNonstandardCodec,        // E10 SDP 声明 G7221 / G726 这类不规范 codec
    SdpNoRtpmap,                // E11 不写 rtpmap，只给静态 payload type
    SdpSessionLevelControl,     // E12 只有 session 级 a=control
    TalkbackRequireMarker,      // E13 校验 talkspurt 首包 marker，缺则丢弃
    SetupNoSessionHeader,       // E14 SETUP 响应不带 Session 头
    IgnoreBackchannelRequire,   // E15 忽略 Require 头（或严格回 551）
    NoAudioOutputConfig,        // E16 整体不实现 AudioOutput / AudioDecoder 配置
    TalkbackStopDraining,       // E19 推流期停止排空，让客户端 sendall 卡住
    RtspMaxSessions,            //     并发会话上限，超过回 453
    RtspPeriodicTeardown,       //     每 N 分钟主动 TEARDOWN
    RtpPacketLoss,              //     丢包
    RtpTimestampJump,           //     时间戳跳变
    SpsPpsPlacement,            //     SPS/PPS 只在 SDP / 带内 / 带内重复
    VideoFreeze,                //     画面冻结
    VideoBlack,                 //     黑屏
    VideoFpsChange,             //     帧率突变
    NoRtcp,                     //     不发 RTCP SR

    // ---- F. 传输与设备 ----------------------------------------------------
    MalformedHttpResponse,      // F1  回显请求字节 + 500
    SelfSignedTls,              // F2  自签证书 HTTPS
    OverloadReboot,             // F3  并发过载时模拟重启
    SystemRebootReal,           //     SystemReboot 真离线：Bye → 全端口关 N 秒 → Hello
    RandomDropout,              //     随机掉线
    HugeResponse,               //     超大响应体
    Slowloris,                  //     慢发送
    GlobalDelay,                //     整机响应延迟
    RtpRateLimit,               //     RTP 限速

    Count
};

struct QuirkParamDef {
    QString name;            // 参数名，如 "seconds"
    QVariant defaultValue;
    QVariant minValue;       // 数值型有效
    QVariant maxValue;
    QStringList choices;     // 非空则是枚举型，defaultValue 必须是其中一项
    QString description;     // 中文说明
};

struct QuirkDef {
    QuirkId id = QuirkId::Count;
    QString key;             // REST / 场景文件里的稳定字符串键，如 "discovery.dialect"
    QuirkGroup group = QuirkGroup::Discovery;
    QString sourceId;        // 出处编号，如 "A1"；无编号的留空
    QString title;           // 中文标题
    QString description;     // 中文说明，含出处背景
    QVector<QuirkParamDef> params;

    const QuirkParamDef *param(const QString &name) const;
};

// 全部 quirk 的元数据。docs/quirks.md、--list-quirks、REST /api/quirks 都从这里来。
class QuirkRegistry
{
public:
    static const QVector<QuirkDef> &all();
    static const QuirkDef &def(QuirkId id);
    static const QuirkDef *findByKey(const QString &key);
    static QString groupKey(QuirkGroup g);
    static QString groupTitle(QuirkGroup g);
};

// 一台相机（或全局默认）的 quirk 开关集。默认全关，只存被改过的项。
class Quirks
{
public:
    bool isEnabled(QuirkId id) const;
    void setEnabled(QuirkId id, bool on);

    // 取参数值；未显式设置时返回元数据里的默认值。
    QVariant param(QuirkId id, const QString &name) const;
    void setParam(QuirkId id, const QString &name, const QVariant &value);

    int paramInt(QuirkId id, const QString &name) const;
    double paramDouble(QuirkId id, const QString &name) const;
    QString paramString(QuirkId id, const QString &name) const;
    bool paramBool(QuirkId id, const QString &name) const;

    // 单参数枚举型 quirk 的便捷取值（参数名固定为 "value"）。
    QString choice(QuirkId id, const QString &fallback = QString()) const;

    // 开关且取整：常见于 "开启后延迟 N 毫秒" 这类。未开启返回 0。
    int enabledInt(QuirkId id, const QString &name) const;

    QList<QuirkId> enabledIds() const;
    // 把 other 里显式提到的项覆盖到自己身上，没提到的保持原样。
    void merge(const Quirks &other);
    void clear();

    QJsonObject toJson() const;
    // 未知键与非法值写入 errors，不中断解析。
    static Quirks fromJson(const QJsonObject &obj, QStringList *errors = nullptr);

private:
    struct State {
        bool enabled = false;
        QVariantMap params;
    };
    QHash<int, State> m_states;
};

} // namespace onvifsim
