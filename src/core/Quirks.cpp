#include "core/Quirks.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonValue>

namespace onvifsim {
namespace {

QuirkParamDef pInt(const QString &name, int def, int lo, int hi, const QString &desc)
{
    QuirkParamDef p;
    p.name = name;
    p.defaultValue = def;
    p.minValue = lo;
    p.maxValue = hi;
    p.description = desc;
    return p;
}

QuirkParamDef pDouble(const QString &name, double def, double lo, double hi, const QString &desc)
{
    QuirkParamDef p;
    p.name = name;
    p.defaultValue = def;
    p.minValue = lo;
    p.maxValue = hi;
    p.description = desc;
    return p;
}

QuirkParamDef pChoice(const QString &name, const QString &def, const QStringList &choices,
                      const QString &desc)
{
    QuirkParamDef p;
    p.name = name;
    p.defaultValue = def;
    p.choices = choices;
    p.description = desc;
    return p;
}

QuirkParamDef pText(const QString &name, const QString &def, const QString &desc)
{
    QuirkParamDef p;
    p.name = name;
    p.defaultValue = def;
    p.description = desc;
    return p;
}

QuirkParamDef pBool(const QString &name, bool def, const QString &desc)
{
    QuirkParamDef p;
    p.name = name;
    p.defaultValue = def;
    p.description = desc;
    return p;
}

void add(QVector<QuirkDef> &v, QuirkId id, const QString &key, QuirkGroup group,
         const QString &sourceId, const QString &title, const QString &description,
         const QVector<QuirkParamDef> &params = {})
{
    QuirkDef d;
    d.id = id;
    d.key = key;
    d.group = group;
    d.sourceId = sourceId;
    d.title = title;
    d.description = description;
    d.params = params;
    v.append(d);
}

QVector<QuirkDef> buildTable()
{
    QVector<QuirkDef> v;
    v.reserve(static_cast<int>(QuirkId::Count));

    // ---- A. 发现 -----------------------------------------------------
    add(v, QuirkId::DiscoveryDialect, QStringLiteral("discovery.dialect"), QuirkGroup::Discovery,
        QStringLiteral("A1"), QStringLiteral("WS-Discovery 命名空间方言"),
        QStringLiteral("默认照抄 Probe 的命名空间回复。参照客户端用 1.0 方言"
                       "（2005/04 discovery + 2004/08 addressing），ONVIF 另一常见方言是 2009/01；"
                       "强制成客户端不认的那套即可复现「搜不到设备」。"),
        { pChoice(QStringLiteral("value"), QStringLiteral("echo"),
                  { QStringLiteral("echo"), QStringLiteral("2005"), QStringLiteral("2009") },
                  QStringLiteral("echo=照抄 Probe，2005=强制 1.0，2009=强制 OASIS")) });

    add(v, QuirkId::DiscoveryNoXAddrs, QStringLiteral("discovery.no_xaddrs"), QuirkGroup::Discovery,
        QStringLiteral("A2"), QStringLiteral("ProbeMatch 不带 XAddrs"),
        QStringLiteral("客户端收到空 XAddrs 会自动多播 Resolve 补问，用来验证 Resolve 分支。"));

    add(v, QuirkId::DiscoveryNoMetadataVersion, QStringLiteral("discovery.no_metadata_version"),
        QuirkGroup::Discovery, QStringLiteral("A3"), QStringLiteral("ProbeMatch 缺 MetadataVersion"),
        QStringLiteral("wsdiscovery 2.1.2 解析 ProbeMatch 时缺这个字段会把整包丢掉，设备直接消失。"));

    add(v, QuirkId::DiscoveryBadXAddrIp, QStringLiteral("discovery.bad_xaddr_ip"),
        QuirkGroup::Discovery, QStringLiteral("A4"), QStringLiteral("XAddrs 放不可达地址"),
        QStringLiteral("客户端只取 getXAddrs()[0]，把坏地址放第一位就能测它的容错。"),
        { pText(QStringLiteral("address"), QStringLiteral("192.168.1.99"),
                QStringLiteral("要插入的 host，可填 0.0.0.0 或主机名")),
          pBool(QStringLiteral("first"), true, QStringLiteral("放在 XAddrs 第一位")) });

    add(v, QuirkId::DiscoveryNoReply, QStringLiteral("discovery.no_reply"), QuirkGroup::Discovery,
        QString(), QStringLiteral("不回 ProbeMatch"),
        QStringLiteral("设备在线但不应答发现，客户端只能手工填 IP。"));

    add(v, QuirkId::DiscoveryReplyDelay, QStringLiteral("discovery.reply_delay"),
        QuirkGroup::Discovery, QString(), QStringLiteral("延迟回复 ProbeMatch"),
        QStringLiteral("wsdiscovery 的 searchServices 是「发 Probe → sleep timeout → 收结果」，"
                       "延迟超过客户端窗口（默认 3s）就等于没回。"),
        { pInt(QStringLiteral("ms"), 1000, 0, 60000, QStringLiteral("延迟毫秒数")) });

    add(v, QuirkId::DiscoveryReplyTwice, QStringLiteral("discovery.reply_twice"),
        QuirkGroup::Discovery, QString(), QStringLiteral("同一 Probe 回两次"),
        QStringLiteral("客户端按 EPR 去重、同 EPR 后到覆盖先到；两份 XAddrs 不同时"
                       "「最后到达那份的第一个 XAddr 胜出」。"),
        { pBool(QStringLiteral("differing_xaddrs"), false,
                QStringLiteral("第二份用不同的 XAddrs")) });

    add(v, QuirkId::DiscoveryScopesNoName, QStringLiteral("discovery.scopes_no_name"),
        QuirkGroup::Discovery, QString(), QStringLiteral("Scopes 不含 name"),
        QStringLiteral("上层只从 Scopes 读 onvif.org/name/ 与 /hardware/，缺 name 时相机显示为无名。"));

    // ---- A. 建连与鉴权 -------------------------------------------------
    add(v, QuirkId::XAddrOddPort, QStringLiteral("connect.xaddr_odd_port"), QuirkGroup::Auth,
        QStringLiteral("A5"), QStringLiteral("XAddr 报非常规端口 / 路径"),
        QStringLiteral("真机 TL-IPC652P-A4 报 :2020/onvif/service 而实际连的是 80。"
                       "客户端只接管 host:port、保留 path，正好测这条逻辑。"),
        { pInt(QStringLiteral("port"), 2020, 1, 65535, QStringLiteral("对外宣称的端口")),
          pText(QStringLiteral("path"), QStringLiteral("/onvif/service"),
                QStringLiteral("对外宣称的服务路径")) });

    add(v, QuirkId::ServicesMedia2First, QStringLiteral("connect.media2_first"), QuirkGroup::Auth,
        QStringLiteral("A6"), QStringLiteral("GetServices 里 Media2 排在 Media 前"),
        QStringLiteral("Media(ver10) 与 Media2(ver20) 的命名空间都含 /media/；"
                       "只按服务名匹配的客户端会把 media 解析到 Media2 端点，"
                       "然后往那儿发 ver10 的 GetProfiles 吃 ActionNotSupported。"
                       "海康 / Axis 双栈固件的真实布局。"));

    add(v, QuirkId::NoGetServices, QStringLiteral("connect.no_get_services"), QuirkGroup::Auth,
        QStringLiteral("A7"), QStringLiteral("不实现 GetServices"),
        QStringLiteral("老固件常见。客户端要能回落到 GetCapabilities 或约定路径。"));

    add(v, QuirkId::AuthTightTimeWindow, QStringLiteral("auth.tight_time_window"), QuirkGroup::Auth,
        QStringLiteral("A8"), QStringLiteral("UsernameToken 时间窗收紧"),
        QStringLiteral("参照客户端不做时钟补偿（adjust_time 从不传 True）。"
                       "把 Created 允许偏差收紧再叠加时钟偏移，就能复现「全线 401」。"),
        { pInt(QStringLiteral("seconds"), 5, 0, 3600, QStringLiteral("允许的时间偏差（秒）")),
          pInt(QStringLiteral("clock_skew"), 0, -86400, 86400,
               QStringLiteral("设备时钟相对真实时间的偏移（秒）")) });

    add(v, QuirkId::SubscriptionNeverExpires, QStringLiteral("auth.subscription_never_expires"),
        QuirkGroup::Auth, QStringLiteral("A9"), QStringLiteral("订阅只增不回收"),
        QStringLiteral("onvif-zeep 每次构造 ONVIFCamera 都建一条 PullPoint 订阅且从不 Unsubscribe。"
                       "关掉过期回收即可复现真实高发的槽位泄漏。"));

    add(v, QuirkId::DeviceInfoMissingFields, QStringLiteral("connect.device_info_missing"),
        QuirkGroup::Auth, QStringLiteral("A10"), QStringLiteral("GetDeviceInformation 缺字段"),
        QStringLiteral("真机常缺 HardwareId 或 SerialNumber，按这些字串选厂商适配器的客户端会落空。"),
        { pText(QStringLiteral("fields"), QStringLiteral("HardwareId"),
                QStringLiteral("要省略的字段，逗号分隔：Manufacturer,Model,FirmwareVersion,"
                               "SerialNumber,HardwareId")) });

    add(v, QuirkId::AuthPasswordTextOnly, QStringLiteral("auth.password_text_only"),
        QuirkGroup::Auth, QString(), QStringLiteral("只接受 PasswordText"),
        QStringLiteral("参照客户端一律发 PasswordDigest，拒绝 Digest 即全线鉴权失败。"));

    add(v, QuirkId::AuthNonceStrictOnce, QStringLiteral("auth.nonce_strict_once"), QuirkGroup::Auth,
        QString(), QStringLiteral("nonce 严格一次性"),
        QStringLiteral("缓存已用 nonce，重复即 401，用来测客户端是否每次重新生成。"),
        { pInt(QStringLiteral("cache_seconds"), 300, 1, 86400,
               QStringLiteral("nonce 缓存时长（秒）")) });

    add(v, QuirkId::AuthPreAuthRequired, QStringLiteral("auth.preauth_required"), QuirkGroup::Auth,
        QString(), QStringLiteral("PRE_AUTH 操作也要鉴权"),
        QStringLiteral("规范允许 GetSystemDateAndTime / GetCapabilities / GetServices / GetWsdlUrl "
                       "匿名调用；有些固件不允许。"));

    add(v, QuirkId::AuthHttp401NotFault, QStringLiteral("auth.http_401_not_fault"),
        QuirkGroup::Auth, QString(), QStringLiteral("鉴权失败回 HTTP 401"),
        QStringLiteral("规范做法是 SOAP Fault ter:NotAuthorized；有些固件直接 401 带 "
                       "WWW-Authenticate: Digest。"));

    add(v, QuirkId::FaultHttpStatus, QStringLiteral("auth.fault_http_status"), QuirkGroup::Auth,
        QStringLiteral("D7"), QStringLiteral("SOAP Fault 的 HTTP 状态码"),
        QStringLiteral("Fault 既可能走 500 也可能走 200。客户端对 500 要放行给 body 解析。"),
        { pChoice(QStringLiteral("value"), QStringLiteral("500"),
                  { QStringLiteral("500"), QStringLiteral("200"), QStringLiteral("400") },
                  QStringLiteral("返回 Fault 时用的 HTTP 状态码")) });

    add(v, QuirkId::AuthFaultWording, QStringLiteral("auth.fault_wording"), QuirkGroup::Auth,
        QStringLiteral("D8"), QStringLiteral("鉴权 Fault 措辞变体"),
        QStringLiteral("客户端靠关键词匹配判定认证错误："
                       "notauthorized / not authorized / unauthorized / authentication / "
                       "sender not authorized / failedauthentication。"),
        { pChoice(QStringLiteral("value"), QStringLiteral("NotAuthorized"),
                  { QStringLiteral("NotAuthorized"), QStringLiteral("SenderNotAuthorized"),
                    QStringLiteral("FailedAuthentication"), QStringLiteral("Unknown") },
                  QStringLiteral("Fault 子码与措辞风格；Unknown=不含任何关键词")) });

    // ---- B. Media 与快照 ------------------------------------------------
    add(v, QuirkId::ProfileNamingStyle, QStringLiteral("media.profile_naming"), QuirkGroup::Media,
        QStringLiteral("B1"), QStringLiteral("profile 命名风格"),
        QStringLiteral("客户端判定主 / 子码流只看 profile Name 是否含 "
                       "main|primary|high / sub|secondary|low；"
                       "Profile_1 / Profile_2 这种没有主子语义的名字会逼它按顺序猜。"),
        { pChoice(QStringLiteral("value"), QStringLiteral("main_sub"),
                  { QStringLiteral("main_sub"), QStringLiteral("profile_n"),
                    QStringLiteral("chinese"), QStringLiteral("token_only") },
                  QStringLiteral("命名风格")) });

    add(v, QuirkId::StreamUriWithUserinfo, QStringLiteral("media.stream_uri_userinfo"),
        QuirkGroup::Media, QStringLiteral("B2"), QStringLiteral("StreamUri 自带 user:pass@"),
        QStringLiteral("客户端会丢弃相机返回的 userinfo 再注入自己的凭据，用来验证这段清洗逻辑。"),
        { pText(QStringLiteral("userinfo"), QStringLiteral("admin:wrongpass"),
                QStringLiteral("嵌入 URI 的 user:pass")) });

    add(v, QuirkId::StreamUriNested, QStringLiteral("media.stream_uri_nested"), QuirkGroup::Media,
        QStringLiteral("B3"), QStringLiteral("URI 套在 MediaUri/Uri 下"),
        QStringLiteral("响应结构多一层，只认顶层 Uri 的客户端会取空。"));

    add(v, QuirkId::StreamUriPlaceholderIp, QStringLiteral("media.stream_uri_placeholder_ip"),
        QuirkGroup::Media, QString(), QStringLiteral("StreamUri 用占位 IP"),
        QStringLiteral("固件把出厂默认 IP 写死进 URI，客户端必须用 XAddr 的 host 覆盖。"),
        { pText(QStringLiteral("address"), QStringLiteral("192.168.1.99"),
                QStringLiteral("占位 host，可填 0.0.0.0")) });

    add(v, QuirkId::SnapshotEmptyBody, QStringLiteral("media.snapshot_empty_body"),
        QuirkGroup::Media, QStringLiteral("B4"), QStringLiteral("快照 200 + 空体"),
        QStringLiteral("真踩过「200 + image/jpeg + 空体」。客户端必须按体长与魔数判定而不是状态码。"),
        { pChoice(QStringLiteral("value"), QStringLiteral("empty"),
                  { QStringLiteral("empty"), QStringLiteral("short_text"),
                    QStringLiteral("html_login"), QStringLiteral("wrong_content_type") },
                  QStringLiteral("坏响应形态")) });

    add(v, QuirkId::SnapshotAuthMode, QStringLiteral("media.snapshot_auth"), QuirkGroup::Media,
        QStringLiteral("B5"), QStringLiteral("快照鉴权方式"),
        QStringLiteral("客户端按 Digest → Basic → 无 顺序试，且只在 401 时才换下一种。"),
        { pChoice(QStringLiteral("value"), QStringLiteral("digest"),
                  { QStringLiteral("none"), QStringLiteral("basic"), QStringLiteral("digest"),
                    QStringLiteral("token") },
                  QStringLiteral("鉴权方式；token=URI 里带一次性令牌")) });

    add(v, QuirkId::SnapshotUriRotates, QStringLiteral("media.snapshot_uri_rotates"),
        QuirkGroup::Media, QStringLiteral("B6"), QStringLiteral("快照 URI 定期失效"),
        QStringLiteral("客户端连续 3 次取图失败才判 URI 失效并重探，中间会盲取。"),
        { pInt(QStringLiteral("seconds"), 60, 1, 86400, QStringLiteral("URI 有效期（秒）")) });

    add(v, QuirkId::NoGetSnapshotUri, QStringLiteral("media.no_get_snapshot_uri"),
        QuirkGroup::Media, QStringLiteral("B7"), QStringLiteral("不实现 GetSnapshotUri"),
        QStringLiteral("回 ActionNotSupported，客户端应退化为从码流抽帧或干脆没有快照。"));

    add(v, QuirkId::CodecMismatch, QStringLiteral("media.codec_mismatch"), QuirkGroup::Media,
        QString(), QStringLiteral("声明与实发编码不一致"),
        QStringLiteral("很多家用机 capability 列了 G.711 / G.722 / AAC，backchannel SDP 却只有 PCMU。"
                       "运行时必须以 SDP 为准。"),
        { pChoice(QStringLiteral("declared"), QStringLiteral("AAC"),
                  { QStringLiteral("AAC"), QStringLiteral("G722"), QStringLiteral("G726"),
                    QStringLiteral("PCMA") },
                  QStringLiteral("ONVIF 里声明的音频编码")),
          pChoice(QStringLiteral("actual"), QStringLiteral("PCMU"),
                  { QStringLiteral("PCMU"), QStringLiteral("PCMA"), QStringLiteral("G722"),
                    QStringLiteral("AAC") },
                  QStringLiteral("SDP 与 RTP 里实际用的编码")) });

    add(v, QuirkId::ResolutionMismatch, QStringLiteral("media.resolution_mismatch"),
        QuirkGroup::Media, QString(), QStringLiteral("声明分辨率与码流不符"),
        QStringLiteral("客户端不读 VideoEncoderConfiguration、全靠 ffprobe 探流，"
                       "这条用来测两边不一致时它信谁。"));

    // ---- C. PTZ -----------------------------------------------------------
    add(v, QuirkId::PtzConfigOnSubOnly, QStringLiteral("ptz.config_on_sub_only"), QuirkGroup::Ptz,
        QStringLiteral("C1"), QStringLiteral("PTZConfiguration 只挂子码流"),
        QStringLiteral("只看主码流 profile 的客户端会判定「没有 PTZ」。"));

    add(v, QuirkId::PtzUsableButUnadvertised, QStringLiteral("ptz.usable_but_unadvertised"),
        QuirkGroup::Ptz, QStringLiteral("C2"), QStringLiteral("不挂 PTZConfiguration 但 PTZ 可用"),
        QStringLiteral("所有 profile 都不带 PTZConfiguration，但 GetNodes 非空且能真转。"
                       "客户端应回落到 GetNodes 判定。"));

    add(v, QuirkId::PtzSpacesEmpty, QStringLiteral("ptz.spaces_empty"), QuirkGroup::Ptz,
        QStringLiteral("C3"), QStringLiteral("SupportedPTZSpaces 为空"),
        QStringLiteral("能力声明四档之一：完整 / 为空 / 只有 Default*Space / 只有 pan。"),
        { pChoice(QStringLiteral("value"), QStringLiteral("empty"),
                  { QStringLiteral("empty"), QStringLiteral("default_only"),
                    QStringLiteral("pan_only") },
                  QStringLiteral("声明档位")) });

    add(v, QuirkId::PtzRangeMinEqualsMax, QStringLiteral("ptz.range_min_equals_max"),
        QuirkGroup::Ptz, QStringLiteral("C4"), QStringLiteral("PTZ Range 的 Min == Max"),
        QStringLiteral("除零或退化区间会让客户端的归一化计算炸掉。"));

    add(v, QuirkId::PtzZoomMalformedResponse, QStringLiteral("ptz.zoom_malformed_response"),
        QuirkGroup::Ptz, QStringLiteral("C5"), QStringLiteral("非零 Zoom 触发畸形响应"),
        QStringLiteral("真机在 ContinuousMove 带非零 Zoom 时回显请求字节 + 500。"));

    add(v, QuirkId::PtzFactory300Presets, QStringLiteral("ptz.factory_300_presets"), QuirkGroup::Ptz,
        QStringLiteral("C6"), QStringLiteral("出厂预填 300 个预置位"),
        QStringLiteral("「预置点 1..300」加巡航扫描 / 远程重启等功能槽，共享同一个假 PTZPosition。"
                       "用来压客户端的预置位列表 UI。"),
        { pInt(QStringLiteral("count"), 300, 1, 1024, QStringLiteral("预置位数量")) });

    add(v, QuirkId::PtzPresetPercentEncoded, QStringLiteral("ptz.preset_percent_encoded"),
        QuirkGroup::Ptz, QStringLiteral("C7"), QStringLiteral("预置位名百分号编码"),
        QStringLiteral("相机把 %E9%A2%84 这类编码原样吐回，客户端要 unquote 才显示得对。"));

    add(v, QuirkId::PtzNoGetPresets, QStringLiteral("ptz.no_get_presets"), QuirkGroup::Ptz,
        QStringLiteral("C8"), QStringLiteral("不实现 GetPresets"),
        QStringLiteral("回 ActionNotSupported，或回一段 not implemented 文本。"),
        { pChoice(QStringLiteral("value"), QStringLiteral("action_not_supported"),
                  { QStringLiteral("action_not_supported"), QStringLiteral("text"),
                    QStringLiteral("empty_list") },
                  QStringLiteral("不支持的表现形式")) });

    add(v, QuirkId::PtzSetPresetReturnShape, QStringLiteral("ptz.set_preset_return_shape"),
        QuirkGroup::Ptz, QStringLiteral("C9"), QStringLiteral("SetPreset 返回形态"),
        QStringLiteral("有的固件返回裸字符串 token，有的返回带 PresetToken 的对象。"),
        { pChoice(QStringLiteral("value"), QStringLiteral("object"),
                  { QStringLiteral("object"), QStringLiteral("bare_string"),
                    QStringLiteral("empty") },
                  QStringLiteral("返回形态")) });

    add(v, QuirkId::PtzResponseJitter, QStringLiteral("ptz.response_jitter"), QuirkGroup::Ptz,
        QStringLiteral("C10"), QStringLiteral("PTZ 响应延迟抖动"),
        QStringLiteral("100~800ms 随机延迟，用来验证客户端的命令乱序防护。"),
        { pInt(QStringLiteral("min_ms"), 100, 0, 60000, QStringLiteral("最小延迟")),
          pInt(QStringLiteral("max_ms"), 800, 0, 60000, QStringLiteral("最大延迟")) });

    add(v, QuirkId::PtzGotoPresetSlow, QStringLiteral("ptz.goto_preset_slow"), QuirkGroup::Ptz,
        QString(), QStringLiteral("GotoPreset 特别慢"),
        QStringLiteral("响应正常但机械动作要好几秒，GetStatus 期间一直 MOVING。"),
        { pInt(QStringLiteral("ms"), 5000, 0, 120000, QStringLiteral("到位耗时")) });

    add(v, QuirkId::PtzMoveWithoutStatusChange, QStringLiteral("ptz.move_without_status_change"),
        QuirkGroup::Ptz, QString(), QStringLiteral("移动不反映到 GetStatus"),
        QStringLiteral("命令返回成功但 GetStatus 的 Position 永远不动，客户端无法闭环。"));

    // ---- D. 事件 -----------------------------------------------------------
    add(v, QuirkId::SubscriptionPortIncrement, QStringLiteral("events.subscription_port_increment"),
        QuirkGroup::Events, QStringLiteral("D1"), QStringLiteral("订阅管理器换独立端口且递增"),
        QStringLiteral("真机形如 :1024/event-1024_1024，下一条订阅换 :1025。"
                       "客户端必须用返回的订阅地址而不是主服务地址去 Pull。"),
        { pInt(QStringLiteral("base_port"), 1024, 1, 65535, QStringLiteral("起始端口")) });

    add(v, QuirkId::SubscriptionHostUnreachable, QStringLiteral("events.subscription_host_bad"),
        QuirkGroup::Events, QStringLiteral("D2"), QStringLiteral("订阅地址 host 不可达"),
        QStringLiteral("固件把内网地址写进订阅 URL，客户端要么接管 host 要么彻底卡住。"),
        { pText(QStringLiteral("address"), QStringLiteral("10.0.0.1"),
                QStringLiteral("写进订阅地址的 host")) });

    add(v, QuirkId::SubscriptionSlotLimit, QStringLiteral("events.subscription_slot_limit"),
        QuirkGroup::Events, QStringLiteral("D3"), QStringLiteral("订阅槽位上限"),
        QStringLiteral("配合 A9（只增不回收）就是真实的槽位耗尽故障。"),
        { pInt(QStringLiteral("max"), 4, 1, 1024, QStringLiteral("最大并发订阅数")),
          pChoice(QStringLiteral("on_overflow"), QStringLiteral("fault"),
                  { QStringLiteral("fault"), QStringLiteral("evict_oldest"),
                    QStringLiteral("silent_fail") },
                  QStringLiteral("超限行为")) });

    add(v, QuirkId::NoGetEventProperties, QStringLiteral("events.no_get_event_properties"),
        QuirkGroup::Events, QStringLiteral("D4"), QStringLiteral("不实现 GetEventProperties"),
        QStringLiteral("客户端拿不到 TopicSet，只能盲订阅全部。"),
        { pChoice(QStringLiteral("value"), QStringLiteral("action_not_supported"),
                  { QStringLiteral("action_not_supported"), QStringLiteral("empty_topicset") },
                  QStringLiteral("不支持的表现形式")) });

    add(v, QuirkId::EventPropertiesBadXml, QStringLiteral("events.bad_xml"), QuirkGroup::Events,
        QStringLiteral("D5"), QStringLiteral("GetEventProperties 返回非法 XML"),
        QStringLiteral("TP-Link TL-IPC 真机返回属性值不加引号的 XML（wstop:topic=true）。"
                       "客户端必须先补引号再解析。"));

    add(v, QuirkId::SubscriptionFlatAddress, QStringLiteral("events.flat_address"),
        QuirkGroup::Events, QStringLiteral("D6"), QStringLiteral("订阅 Address 不套 Reference"),
        QStringLiteral("Address 直接放响应下，不包 SubscriptionReference 一层。"));

    add(v, QuirkId::TopicNamingStyle, QStringLiteral("events.topic_style"), QuirkGroup::Events,
        QStringLiteral("D9"), QStringLiteral("Topic 命名风格"),
        QStringLiteral("标准 ONVIF / TP-Link（LineCrossDetector）/ Reolink（MyRuleDetector）/ "
                       "Axis（tnsaxis: 前缀）四套。"),
        { pChoice(QStringLiteral("value"), QStringLiteral("onvif"),
                  { QStringLiteral("onvif"), QStringLiteral("tplink"), QStringLiteral("reolink"),
                    QStringLiteral("axis") },
                  QStringLiteral("命名风格")) });

    add(v, QuirkId::PullMessagesAlwaysEmpty, QStringLiteral("events.pull_always_empty"),
        QuirkGroup::Events, QStringLiteral("D10"), QStringLiteral("PullMessages 永远返回空"),
        QStringLiteral("订阅建得起来、Pull 也不报错，就是永远没有事件。最难查的一类故障。"));

    add(v, QuirkId::EventStateNotPaired, QStringLiteral("events.state_not_paired"),
        QuirkGroup::Events, QStringLiteral("D11"), QStringLiteral("属性型事件不发配对的 false"),
        QStringLiteral("正常应成对发 IsMotion=true / false；只发 true 会让客户端的移动侦测永远不复位。"));

    add(v, QuirkId::RenewFails, QStringLiteral("events.renew_fails"), QuirkGroup::Events, QString(),
        QStringLiteral("Renew 总是失败"),
        QStringLiteral("客户端必须能在 Renew 失败后重建订阅而不是放弃。"));

    add(v, QuirkId::SubscriptionExpiresAtOnce, QStringLiteral("events.subscription_expires_at_once"),
        QuirkGroup::Events, QString(), QStringLiteral("订阅建完立刻过期"),
        QStringLiteral("CreatePullPointSubscription 成功但 TerminationTime 已是过去时间。"));

    add(v, QuirkId::EventStorm, QStringLiteral("events.storm"), QuirkGroup::Events, QString(),
        QStringLiteral("事件风暴"),
        QStringLiteral("每秒推 N 条事件，压客户端的队列与 UI。"),
        { pInt(QStringLiteral("per_second"), 50, 1, 10000, QStringLiteral("每秒事件数")) });

    add(v, QuirkId::NoSetSynchronizationPoint, QStringLiteral("events.no_sync_point"),
        QuirkGroup::Events, QString(), QStringLiteral("不支持 SetSynchronizationPoint"),
        QStringLiteral("客户端拿不到属性型 topic 的当前状态，只能等下一次变化。"));

    // ---- E. RTSP 与对讲 -----------------------------------------------------
    add(v, QuirkId::AudioCapabilityLie, QStringLiteral("rtsp.audio_capability_lie"),
        QuirkGroup::Rtsp, QStringLiteral("E1"), QStringLiteral("音频能力谎标"),
        QStringLiteral("ONVIF 声明的解码能力与 backchannel SDP 实际开放的 codec 不一致。"
                       "客户端必须以 SDP 为准。"));

    add(v, QuirkId::G722SampleRate8000, QStringLiteral("rtsp.g722_sample_rate_8000"),
        QuirkGroup::Rtsp, QStringLiteral("E2"), QStringLiteral("G.722 采样率错写成 8000"),
        QStringLiteral("G.722 的 RTP 时钟率按规范写 8000 但实际是 16000，"
                       "很多固件在 ONVIF 能力里也错写成 8000。客户端应强制按 16000 处理。"));

    add(v, QuirkId::BitrateUnitVariant, QStringLiteral("rtsp.bitrate_unit"), QuirkGroup::Rtsp,
        QStringLiteral("E3"), QStringLiteral("码率单位变体"),
        QStringLiteral("有的固件按 kbps 报，有的按 bps 报，差 1000 倍。"),
        { pChoice(QStringLiteral("value"), QStringLiteral("kbps"),
                  { QStringLiteral("kbps"), QStringLiteral("bps") },
                  QStringLiteral("码率字段单位")) });

    add(v, QuirkId::AudioDecoderOptionsShape, QStringLiteral("rtsp.audio_decoder_options_shape"),
        QuirkGroup::Rtsp, QStringLiteral("E4"),
        QStringLiteral("GetAudioDecoderConfigurationOptions 响应形态"),
        QStringLiteral("形态 A：G711/G722/G726/AAC 子元素直挂 opts；"
                       "形态 B：AudioDecoderOptions 列表，每项带 Encoding。两种都得支持。"),
        { pChoice(QStringLiteral("value"), QStringLiteral("a"),
                  { QStringLiteral("a"), QStringLiteral("b") },
                  QStringLiteral("a=子元素直挂，b=Options 列表")) });

    add(v, QuirkId::SampleRateShape, QStringLiteral("rtsp.sample_rate_shape"), QuirkGroup::Rtsp,
        QStringLiteral("E5"), QStringLiteral("采样率字段形态"),
        QStringLiteral("SampleRateRange / SampleRateList / 单值三种形态都在真机上出现过。"),
        { pChoice(QStringLiteral("value"), QStringLiteral("range"),
                  { QStringLiteral("range"), QStringLiteral("list"), QStringLiteral("single") },
                  QStringLiteral("字段形态")) });

    add(v, QuirkId::TalkbackDualTrack, QStringLiteral("rtsp.talkback_dual_track"), QuirkGroup::Rtsp,
        QStringLiteral("E6"), QStringLiteral("对讲双轨布局"),
        QStringLiteral("海康是单轨（只有 sendonly 对讲轨）；"
                       "大华是双轨（麦克风 recvonly 在前 + 对讲 sendonly 在后），"
                       "客户端要挑对轨才推得进去。"));

    add(v, QuirkId::TalkbackBusySlot, QStringLiteral("rtsp.talkback_busy_slot"), QuirkGroup::Rtsp,
        QStringLiteral("E7"), QStringLiteral("对讲忙槽位"),
        QStringLiteral("TEARDOWN 后 N 秒内对新的 backchannel DESCRIBE 回 401。"
                       "海康球机实测 3~4 秒。客户端必须重试而不是判定凭据错误。"),
        { pInt(QStringLiteral("seconds"), 4, 1, 120, QStringLiteral("忙槽位持续秒数")) });

    add(v, QuirkId::AuthDualChallenge, QStringLiteral("rtsp.auth_dual_challenge"), QuirkGroup::Rtsp,
        QStringLiteral("E8"), QStringLiteral("同时发 Basic + Digest 挑战"),
        QStringLiteral("两条 WWW-Authenticate，顺序可配。有的客户端只看第一条。"),
        { pChoice(QStringLiteral("order"), QStringLiteral("digest_first"),
                  { QStringLiteral("digest_first"), QStringLiteral("basic_first") },
                  QStringLiteral("两条挑战的顺序")) });

    add(v, QuirkId::AuthStrictDigestParams, QStringLiteral("rtsp.auth_strict_digest_params"),
        QuirkGroup::Rtsp, QStringLiteral("E9"), QStringLiteral("严格 Digest 参数校验"),
        QStringLiteral("客户端 Authorization 里出现挑战没给过的参数（如 algorithm=MD5）就 401。"
                       "真机上很多客户端因此连不上。"));

    add(v, QuirkId::SdpNonstandardCodec, QStringLiteral("rtsp.sdp_nonstandard_codec"),
        QuirkGroup::Rtsp, QStringLiteral("E10"), QStringLiteral("SDP 声明不规范 codec"),
        QStringLiteral("G7221 / G726-32 这类写法，客户端的 codec 表匹配不上要能优雅降级。"),
        { pText(QStringLiteral("name"), QStringLiteral("G7221"),
                QStringLiteral("写进 a=rtpmap 的编码名")) });

    add(v, QuirkId::SdpNoRtpmap, QStringLiteral("rtsp.sdp_no_rtpmap"), QuirkGroup::Rtsp,
        QStringLiteral("E11"), QStringLiteral("SDP 不写 rtpmap"),
        QStringLiteral("只给静态 payload type（0=PCMU / 8=PCMA），客户端要按 RFC 3551 静态表推断。"));

    add(v, QuirkId::SdpSessionLevelControl, QStringLiteral("rtsp.sdp_session_level_control"),
        QuirkGroup::Rtsp, QStringLiteral("E12"), QStringLiteral("只有 session 级 a=control"),
        QStringLiteral("媒体级没有 control 属性，客户端拼 SETUP URL 时只能用 session 级的。"));

    add(v, QuirkId::TalkbackRequireMarker, QStringLiteral("rtsp.talkback_require_marker"),
        QuirkGroup::Rtsp, QStringLiteral("E13"), QStringLiteral("校验 talkspurt 首包 marker"),
        QStringLiteral("相机要求每段话首包置 RTP marker 位，否则整段静音丢弃。"));

    add(v, QuirkId::SetupNoSessionHeader, QStringLiteral("rtsp.setup_no_session_header"),
        QuirkGroup::Rtsp, QStringLiteral("E14"), QStringLiteral("SETUP 响应不带 Session 头"),
        QStringLiteral("违反 RFC 2326，客户端后续 PLAY 无 Session 可用。"));

    add(v, QuirkId::IgnoreBackchannelRequire, QStringLiteral("rtsp.ignore_backchannel_require"),
        QuirkGroup::Rtsp, QStringLiteral("E15"), QStringLiteral("忽略 backchannel Require 头"),
        QStringLiteral("不带 Require 也返回含 sendonly 的 SDP，或严格按 RFC 2326 对不支持的 "
                       "Require 回 551。"),
        { pChoice(QStringLiteral("value"), QStringLiteral("ignore"),
                  { QStringLiteral("ignore"), QStringLiteral("strict_551") },
                  QStringLiteral("ignore=不看 Require 照给，strict_551=不支持就 551")) });

    add(v, QuirkId::NoAudioOutputConfig, QStringLiteral("rtsp.no_audio_output_config"),
        QuirkGroup::Rtsp, QStringLiteral("E16"), QStringLiteral("不实现 AudioOutput/Decoder 配置"),
        QStringLiteral("AddAudioOutputConfiguration / AddAudioDecoderConfiguration 整体回 "
                       "ActionNotSupported，客户端只能直接开 backchannel 试。"));

    add(v, QuirkId::TalkbackStopDraining, QStringLiteral("rtsp.talkback_stop_draining"),
        // E17 / E18 是有意留空：对应 TP-Link MULTITRANS 的两条，属于二期候选
        // （见 plan.md §7 末尾）。编号预留着不复用，免得日后实现时对不上出处。
        QuirkGroup::Rtsp, QStringLiteral("E19"), QStringLiteral("对讲推流期停止排空"),
        QStringLiteral("相机不再读 TCP 接收缓冲，客户端的 sendall 卡死在内核缓冲区满。"),
        { pInt(QStringLiteral("after_ms"), 3000, 0, 600000,
               QStringLiteral("推流开始后多久停止排空")) });

    add(v, QuirkId::RtspMaxSessions, QStringLiteral("rtsp.max_sessions"), QuirkGroup::Rtsp,
        QString(), QStringLiteral("RTSP 并发会话上限"),
        QStringLiteral("廉价相机常态：超过上限回 453 Not Enough Bandwidth。"),
        { pInt(QStringLiteral("max"), 2, 1, 256, QStringLiteral("最大并发会话数")) });

    add(v, QuirkId::RtspPeriodicTeardown, QStringLiteral("rtsp.periodic_teardown"),
        QuirkGroup::Rtsp, QString(), QStringLiteral("周期性主动断流"),
        QStringLiteral("每 N 秒相机自己 TEARDOWN，客户端必须能自动重连。"),
        // 下限取 1 秒：真机上确实有几秒就断一次的烂固件，
        // 而且 e2e 要在几秒内看到断流，下限定成 5 秒会让这条 quirk 没法测。
        { pInt(QStringLiteral("seconds"), 300, 1, 86400, QStringLiteral("断流间隔")) });

    add(v, QuirkId::RtpPacketLoss, QStringLiteral("rtsp.rtp_packet_loss"), QuirkGroup::Rtsp,
        QString(), QStringLiteral("RTP 丢包"),
        QStringLiteral("按比例随机丢弃 RTP 包，测客户端的花屏恢复。"),
        { pDouble(QStringLiteral("percent"), 5.0, 0.0, 100.0, QStringLiteral("丢包率（%）")) });

    add(v, QuirkId::RtpTimestampJump, QStringLiteral("rtsp.rtp_timestamp_jump"), QuirkGroup::Rtsp,
        QString(), QStringLiteral("RTP 时间戳跳变"),
        QStringLiteral("周期性让时间戳大幅跳跃，测客户端的抖动缓冲。"),
        { pInt(QStringLiteral("seconds"), 30, 1, 86400, QStringLiteral("跳变间隔")),
          pInt(QStringLiteral("delta_ms"), 5000, -600000, 600000,
               QStringLiteral("跳变量（毫秒，可负）")) });

    add(v, QuirkId::SpsPpsPlacement, QStringLiteral("rtsp.sps_pps_placement"), QuirkGroup::Rtsp,
        QString(), QStringLiteral("SPS/PPS 位置"),
        QStringLiteral("只在 SDP 的 sprop-parameter-sets / 只在带内 / 两处都有。"
                       "只认一处的解码器会黑屏。"),
        { pChoice(QStringLiteral("value"), QStringLiteral("both"),
                  { QStringLiteral("both"), QStringLiteral("sdp_only"),
                    QStringLiteral("inband_only") },
                  QStringLiteral("放置策略")) });

    add(v, QuirkId::VideoFreeze, QStringLiteral("rtsp.video_freeze"), QuirkGroup::Rtsp, QString(),
        QStringLiteral("画面冻结"),
        QStringLiteral("RTP 继续发但画面内容不变，测客户端的冻结检测。"));

    add(v, QuirkId::VideoBlack, QStringLiteral("rtsp.video_black"), QuirkGroup::Rtsp, QString(),
        QStringLiteral("黑屏"), QStringLiteral("切换到全黑样片，测客户端的黑屏检测。"));

    add(v, QuirkId::VideoFpsChange, QStringLiteral("rtsp.video_fps_change"), QuirkGroup::Rtsp,
        QString(), QStringLiteral("帧率突变"),
        QStringLiteral("实际帧率与声明不符且中途变化。"),
        { pDouble(QStringLiteral("fps"), 5.0, 0.1, 120.0, QStringLiteral("实际帧率")) });

    add(v, QuirkId::NoRtcp, QStringLiteral("rtsp.no_rtcp"), QuirkGroup::Rtsp, QString(),
        QStringLiteral("不发 RTCP SR"),
        QStringLiteral("客户端拿不到 NTP 映射，音视频同步只能靠 RTP 时间戳硬估。"));

    // ---- F. 传输与设备 -------------------------------------------------------
    add(v, QuirkId::MalformedHttpResponse, QStringLiteral("transport.malformed_http"),
        QuirkGroup::Transport, QStringLiteral("F1"), QStringLiteral("畸形 HTTP 响应"),
        QStringLiteral("回显请求原始字节 + 500，或缺 Content-Length、状态行残缺。"),
        { pChoice(QStringLiteral("value"), QStringLiteral("echo_500"),
                  { QStringLiteral("echo_500"), QStringLiteral("no_content_length"),
                    QStringLiteral("bad_status_line"), QStringLiteral("truncated") },
                  QStringLiteral("畸形形式")),
          pDouble(QStringLiteral("percent"), 100.0, 0.0, 100.0,
                  QStringLiteral("触发概率（%）")) });

    add(v, QuirkId::SelfSignedTls, QStringLiteral("transport.self_signed_tls"),
        QuirkGroup::Transport, QStringLiteral("F2"), QStringLiteral("自签证书 HTTPS"),
        QStringLiteral("TP-Link VIGI 的私有 API 走 20443 自签 HTTPS，客户端必须跳过证书校验。"),
        { pInt(QStringLiteral("port"), 20443, 1, 65535, QStringLiteral("HTTPS 端口")) });

    add(v, QuirkId::OverloadReboot, QStringLiteral("transport.overload_reboot"),
        QuirkGroup::Transport, QStringLiteral("F3"), QStringLiteral("并发过载时模拟重启"),
        QStringLiteral("一秒内的 SOAP 请求数超过阈值就整机假死再回来，复现廉价固件被打挂。"
                       "按速率而不是瞬时并发判定：单事件循环下请求本来就是串行处理的。"),
        { pInt(QStringLiteral("threshold"), 8, 1, 1024, QStringLiteral("每秒请求数阈值")),
          pInt(QStringLiteral("offline_seconds"), 20, 1, 600, QStringLiteral("假死秒数")) });

    add(v, QuirkId::SystemRebootReal, QStringLiteral("transport.system_reboot_real"),
        QuirkGroup::Transport, QString(), QStringLiteral("SystemReboot 真的离线"),
        QStringLiteral("发 Bye → 关掉全部端口 N 秒 → 发 Hello 回来，而不是只回个 OK。"),
        { pInt(QStringLiteral("seconds"), 30, 1, 600, QStringLiteral("离线秒数")) });

    add(v, QuirkId::RandomDropout, QStringLiteral("transport.random_dropout"),
        QuirkGroup::Transport, QString(), QStringLiteral("随机掉线"),
        QStringLiteral("按概率整机离线一小段时间，模拟不稳定的 PoE / WiFi。"),
        { pDouble(QStringLiteral("percent"), 5.0, 0.0, 100.0,
                  QStringLiteral("每次请求触发掉线的概率（%）")),
          pInt(QStringLiteral("seconds"), 10, 1, 600, QStringLiteral("每次掉线秒数")) });

    add(v, QuirkId::HugeResponse, QStringLiteral("transport.huge_response"), QuirkGroup::Transport,
        QString(), QStringLiteral("超大响应体"),
        QStringLiteral("塞进大量填充，测客户端的解析上限与内存。"),
        { pInt(QStringLiteral("kb"), 8192, 1, 262144, QStringLiteral("响应体大小（KB）")) });

    add(v, QuirkId::Slowloris, QStringLiteral("transport.slowloris"), QuirkGroup::Transport,
        QString(), QStringLiteral("慢发送"),
        QStringLiteral("响应分成小块、每块之间等一会儿，测客户端的读超时。"),
        { pInt(QStringLiteral("chunk_bytes"), 16, 1, 65536, QStringLiteral("每块字节数")),
          pInt(QStringLiteral("interval_ms"), 500, 1, 60000, QStringLiteral("块间隔")) });

    add(v, QuirkId::GlobalDelay, QStringLiteral("transport.global_delay"), QuirkGroup::Transport,
        QString(), QStringLiteral("整机响应延迟"),
        QStringLiteral("所有 HTTP 响应统一延迟，模拟远端 / 弱网相机。"),
        { pInt(QStringLiteral("ms"), 1000, 0, 120000, QStringLiteral("延迟毫秒数")) });

    add(v, QuirkId::RtpRateLimit, QStringLiteral("transport.rtp_rate_limit"), QuirkGroup::Transport,
        QString(), QStringLiteral("RTP 限速"),
        QStringLiteral("按给定带宽发 RTP，低于码流需求就会持续积压。"),
        { pInt(QStringLiteral("kbps"), 256, 8, 100000, QStringLiteral("上限带宽（kbps）")) });

    return v;
}

} // namespace

const QuirkParamDef *QuirkDef::param(const QString &name) const
{
    for (const QuirkParamDef &p : params) {
        if (p.name == name)
            return &p;
    }
    return nullptr;
}

const QVector<QuirkDef> &QuirkRegistry::all()
{
    static const QVector<QuirkDef> table = buildTable();
    return table;
}

const QuirkDef &QuirkRegistry::def(QuirkId id)
{
    static const QHash<int, int> index = [] {
        QHash<int, int> m;
        const QVector<QuirkDef> &t = all();
        for (int i = 0; i < t.size(); ++i)
            m.insert(static_cast<int>(t.at(i).id), i);
        return m;
    }();
    static const QuirkDef empty;
    const auto it = index.constFind(static_cast<int>(id));
    if (it == index.constEnd())
        return empty;
    return all().at(it.value());
}

const QuirkDef *QuirkRegistry::findByKey(const QString &key)
{
    static const QHash<QString, int> index = [] {
        QHash<QString, int> m;
        const QVector<QuirkDef> &t = all();
        for (int i = 0; i < t.size(); ++i)
            m.insert(t.at(i).key, i);
        return m;
    }();
    const auto it = index.constFind(key);
    if (it == index.constEnd())
        return nullptr;
    return &all().at(it.value());
}

QString QuirkRegistry::groupKey(QuirkGroup g)
{
    switch (g) {
    case QuirkGroup::Discovery: return QStringLiteral("discovery");
    case QuirkGroup::Auth:      return QStringLiteral("auth");
    case QuirkGroup::Media:     return QStringLiteral("media");
    case QuirkGroup::Ptz:       return QStringLiteral("ptz");
    case QuirkGroup::Events:    return QStringLiteral("events");
    case QuirkGroup::Rtsp:      return QStringLiteral("rtsp");
    case QuirkGroup::Transport: return QStringLiteral("transport");
    }
    return QString();
}

QString QuirkRegistry::groupTitle(QuirkGroup g)
{
    switch (g) {
    case QuirkGroup::Discovery: return QStringLiteral("发现");
    case QuirkGroup::Auth:      return QStringLiteral("建连与鉴权");
    case QuirkGroup::Media:     return QStringLiteral("Media 与快照");
    case QuirkGroup::Ptz:       return QStringLiteral("PTZ");
    case QuirkGroup::Events:    return QStringLiteral("事件");
    case QuirkGroup::Rtsp:      return QStringLiteral("RTSP 与对讲");
    case QuirkGroup::Transport: return QStringLiteral("传输与设备");
    }
    return QString();
}

bool Quirks::isEnabled(QuirkId id) const
{
    const auto it = m_states.constFind(static_cast<int>(id));
    return it != m_states.constEnd() && it->enabled;
}

void Quirks::setEnabled(QuirkId id, bool on)
{
    // 关掉的项也要留痕：调用方可能是在「显式关掉预设自带的某条 quirk」，
    // merge() 靠 m_states 里有没有这个键来区分「没提」与「明确要关」。
    // toJson() 会把纯 false 且无参数的项滤掉，所以序列化结果仍然精简。
    m_states[static_cast<int>(id)].enabled = on;
}

QVariant Quirks::param(QuirkId id, const QString &name) const
{
    const auto it = m_states.constFind(static_cast<int>(id));
    if (it != m_states.constEnd()) {
        const auto pit = it->params.constFind(name);
        if (pit != it->params.constEnd())
            return pit.value();
    }
    if (const QuirkParamDef *d = QuirkRegistry::def(id).param(name))
        return d->defaultValue;
    return QVariant();
}

void Quirks::setParam(QuirkId id, const QString &name, const QVariant &value)
{
    m_states[static_cast<int>(id)].params.insert(name, value);
}

int Quirks::paramInt(QuirkId id, const QString &name) const
{
    return param(id, name).toInt();
}

double Quirks::paramDouble(QuirkId id, const QString &name) const
{
    return param(id, name).toDouble();
}

QString Quirks::paramString(QuirkId id, const QString &name) const
{
    return param(id, name).toString();
}

bool Quirks::paramBool(QuirkId id, const QString &name) const
{
    return param(id, name).toBool();
}

QString Quirks::choice(QuirkId id, const QString &fallback) const
{
    if (!isEnabled(id))
        return fallback;
    const QString v = paramString(id, QStringLiteral("value"));
    return v.isEmpty() ? fallback : v;
}

int Quirks::enabledInt(QuirkId id, const QString &name) const
{
    return isEnabled(id) ? paramInt(id, name) : 0;
}

QList<QuirkId> Quirks::enabledIds() const
{
    QList<QuirkId> ids;
    for (const QuirkDef &d : QuirkRegistry::all()) {
        if (isEnabled(d.id))
            ids.append(d.id);
    }
    return ids;
}

void Quirks::merge(const Quirks &other)
{
    for (auto it = other.m_states.constBegin(); it != other.m_states.constEnd(); ++it) {
        State &target = m_states[it.key()];
        target.enabled = it->enabled;
        for (auto p = it->params.constBegin(); p != it->params.constEnd(); ++p)
            target.params.insert(p.key(), p.value());
    }
}

void Quirks::clear()
{
    m_states.clear();
}

QJsonObject Quirks::toJson() const
{
    QJsonObject obj;
    for (const QuirkDef &d : QuirkRegistry::all()) {
        const auto it = m_states.constFind(static_cast<int>(d.id));
        if (it == m_states.constEnd())
            continue;
        if (!it->enabled && it->params.isEmpty())
            continue;
        QJsonObject entry;
        entry.insert(QStringLiteral("enabled"), it->enabled);
        if (!it->params.isEmpty())
            entry.insert(QStringLiteral("params"), QJsonObject::fromVariantMap(it->params));
        obj.insert(d.key, entry);
    }
    return obj;
}

Quirks Quirks::fromJson(const QJsonObject &obj, QStringList *errors)
{
    Quirks q;
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        const QuirkDef *d = QuirkRegistry::findByKey(it.key());
        if (!d) {
            if (errors)
                errors->append(QCoreApplication::translate("onvifsim::core", "未知的 quirk：%1").arg(it.key()));
            continue;
        }
        // 允许两种写法：直接给 true/false，或给 {enabled, params} 对象。
        if (it.value().isBool()) {
            q.setEnabled(d->id, it.value().toBool());
            continue;
        }
        if (!it.value().isObject()) {
            if (errors)
                errors->append(QCoreApplication::translate("onvifsim::core", "quirk %1 的值必须是布尔或对象").arg(it.key()));
            continue;
        }
        const QJsonObject entry = it.value().toObject();
        q.setEnabled(d->id, entry.value(QStringLiteral("enabled")).toBool(true));
        const QJsonObject params = entry.value(QStringLiteral("params")).toObject();
        for (auto pit = params.constBegin(); pit != params.constEnd(); ++pit) {
            const QuirkParamDef *pd = d->param(pit.key());
            if (!pd) {
                if (errors)
                    errors->append(
                        QCoreApplication::translate("onvifsim::core", "quirk %1 没有参数 %2").arg(it.key(), pit.key()));
                continue;
            }
            const QVariant value = pit.value().toVariant();
            if (!pd->choices.isEmpty() && !pd->choices.contains(value.toString())) {
                if (errors)
                    errors->append(QCoreApplication::translate("onvifsim::core", "quirk %1 的参数 %2 取值非法：%3（可选 %4）")
                                       .arg(it.key(), pit.key(), value.toString(),
                                            pd->choices.join(QLatin1Char('/'))));
                continue;
            }
            if (pd->minValue.isValid() && value.toDouble() < pd->minValue.toDouble()) {
                if (errors)
                    errors->append(QCoreApplication::translate("onvifsim::core", "quirk %1 的参数 %2 小于下限 %3")
                                       .arg(it.key(), pit.key(), pd->minValue.toString()));
                continue;
            }
            if (pd->maxValue.isValid() && value.toDouble() > pd->maxValue.toDouble()) {
                if (errors)
                    errors->append(QCoreApplication::translate("onvifsim::core", "quirk %1 的参数 %2 大于上限 %3")
                                       .arg(it.key(), pit.key(), pd->maxValue.toString()));
                continue;
            }
            q.setParam(d->id, pit.key(), value);
        }
    }
    return q;
}

} // namespace onvifsim
