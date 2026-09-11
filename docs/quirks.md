# 故障注入全表

> 本文件由 `onvifsim --list-quirks --markdown` 生成，不要手改。
> 每条 quirk 的出处编号指向 `docs/reference-client-facts.md` §9。
> **编号跟的是证据出处，不是下面的分组**：所以「建连与鉴权」一节里
> 会出现 D7 / D8 这样的 D 号 —— 那两条的实证记在 §9 的 D 段里。
> 编号中间有断档（如 E 组缺 E17 / E18）也是正常的：对应的条目
> 属于还没实现的二期候选，编号预留着不复用。

## 发现（discovery）

### `discovery.dialect`

**WS-Discovery 命名空间方言**　出处：A1

默认照抄 Probe 的命名空间回复。参照客户端用 1.0 方言（2005/04 discovery + 2004/08 addressing），ONVIF 另一常见方言是 2009/01；强制成客户端不认的那套即可复现「搜不到设备」。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `value` | `echo` | echo / 2005 / 2009 | echo=照抄 Probe，2005=强制 1.0，2009=强制 OASIS |

### `discovery.no_xaddrs`

**ProbeMatch 不带 XAddrs**　出处：A2

客户端收到空 XAddrs 会自动多播 Resolve 补问，用来验证 Resolve 分支。

### `discovery.no_metadata_version`

**ProbeMatch 缺 MetadataVersion**　出处：A3

wsdiscovery 2.1.2 解析 ProbeMatch 时缺这个字段会把整包丢掉，设备直接消失。

### `discovery.bad_xaddr_ip`

**XAddrs 放不可达地址**　出处：A4

客户端只取 getXAddrs()[0]，把坏地址放第一位就能测它的容错。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `address` | `192.168.1.99` | — | 要插入的 host，可填 0.0.0.0 或主机名 |
| `first` | `true` | — | 放在 XAddrs 第一位 |

### `discovery.no_reply`

**不回 ProbeMatch**　出处：—

设备在线但不应答发现，客户端只能手工填 IP。

### `discovery.reply_delay`

**延迟回复 ProbeMatch**　出处：—

wsdiscovery 的 searchServices 是「发 Probe → sleep timeout → 收结果」，延迟超过客户端窗口（默认 3s）就等于没回。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `ms` | `1000` | 0 ~ 60000 | 延迟毫秒数 |

### `discovery.reply_twice`

**同一 Probe 回两次**　出处：—

客户端按 EPR 去重、同 EPR 后到覆盖先到；两份 XAddrs 不同时「最后到达那份的第一个 XAddr 胜出」。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `differing_xaddrs` | `false` | — | 第二份用不同的 XAddrs |

### `discovery.scopes_no_name`

**Scopes 不含 name**　出处：—

上层只从 Scopes 读 onvif.org/name/ 与 /hardware/，缺 name 时相机显示为无名。


## 建连与鉴权（auth）

### `connect.xaddr_odd_port`

**XAddr 报非常规端口 / 路径**　出处：A5

真机 TL-IPC652P-A4 报 :2020/onvif/service 而实际连的是 80。客户端只接管 host:port、保留 path，正好测这条逻辑。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `port` | `2020` | 1 ~ 65535 | 对外宣称的端口 |
| `path` | `/onvif/service` | — | 对外宣称的服务路径 |

### `connect.media2_first`

**GetServices 里 Media2 排在 Media 前**　出处：A6

Media(ver10) 与 Media2(ver20) 的命名空间都含 /media/；只按服务名匹配的客户端会把 media 解析到 Media2 端点，然后往那儿发 ver10 的 GetProfiles 吃 ActionNotSupported。海康 / Axis 双栈固件的真实布局。

### `connect.no_get_services`

**不实现 GetServices**　出处：A7

老固件常见。客户端要能回落到 GetCapabilities 或约定路径。

### `auth.tight_time_window`

**UsernameToken 时间窗收紧**　出处：A8

参照客户端不做时钟补偿（adjust_time 从不传 True）。把 Created 允许偏差收紧再叠加时钟偏移，就能复现「全线 401」。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `seconds` | `5` | 0 ~ 3600 | 允许的时间偏差（秒） |
| `clock_skew` | `0` | -86400 ~ 86400 | 设备时钟相对真实时间的偏移（秒） |

### `auth.subscription_never_expires`

**订阅只增不回收**　出处：A9

onvif-zeep 每次构造 ONVIFCamera 都建一条 PullPoint 订阅且从不 Unsubscribe。关掉过期回收即可复现真实高发的槽位泄漏。

### `connect.device_info_missing`

**GetDeviceInformation 缺字段**　出处：A10

真机常缺 HardwareId 或 SerialNumber，按这些字串选厂商适配器的客户端会落空。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `fields` | `HardwareId` | — | 要省略的字段，逗号分隔：Manufacturer,Model,FirmwareVersion,SerialNumber,HardwareId |

### `auth.password_text_only`

**只接受 PasswordText**　出处：—

参照客户端一律发 PasswordDigest，拒绝 Digest 即全线鉴权失败。

### `auth.nonce_strict_once`

**nonce 严格一次性**　出处：—

缓存已用 nonce，重复即 401，用来测客户端是否每次重新生成。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `cache_seconds` | `300` | 1 ~ 86400 | nonce 缓存时长（秒） |

### `auth.preauth_required`

**PRE_AUTH 操作也要鉴权**　出处：—

规范允许 GetSystemDateAndTime / GetCapabilities / GetServices / GetWsdlUrl 匿名调用；有些固件不允许。

### `auth.http_401_not_fault`

**鉴权失败回 HTTP 401**　出处：—

规范做法是 SOAP Fault ter:NotAuthorized；有些固件直接 401 带 WWW-Authenticate: Digest。

### `auth.fault_http_status`

**SOAP Fault 的 HTTP 状态码**　出处：D7

Fault 既可能走 500 也可能走 200。客户端对 500 要放行给 body 解析。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `value` | `500` | 500 / 200 / 400 | 返回 Fault 时用的 HTTP 状态码 |

### `auth.fault_wording`

**鉴权 Fault 措辞变体**　出处：D8

客户端靠关键词匹配判定认证错误：notauthorized / not authorized / unauthorized / authentication / sender not authorized / failedauthentication。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `value` | `NotAuthorized` | NotAuthorized / SenderNotAuthorized / FailedAuthentication / Unknown | Fault 子码与措辞风格；Unknown=不含任何关键词 |


## Media 与快照（media）

### `media.profile_naming`

**profile 命名风格**　出处：B1

客户端判定主 / 子码流只看 profile Name 是否含 main|primary|high / sub|secondary|low；Profile_1 / Profile_2 这种没有主子语义的名字会逼它按顺序猜。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `value` | `main_sub` | main_sub / profile_n / chinese / token_only | 命名风格 |

### `media.stream_uri_userinfo`

**StreamUri 自带 user:pass@**　出处：B2

客户端会丢弃相机返回的 userinfo 再注入自己的凭据，用来验证这段清洗逻辑。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `userinfo` | `admin:wrongpass` | — | 嵌入 URI 的 user:pass |

### `media.stream_uri_nested`

**URI 套在 MediaUri/Uri 下**　出处：B3

响应结构多一层，只认顶层 Uri 的客户端会取空。

### `media.stream_uri_placeholder_ip`

**StreamUri 用占位 IP**　出处：—

固件把出厂默认 IP 写死进 URI，客户端必须用 XAddr 的 host 覆盖。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `address` | `192.168.1.99` | — | 占位 host，可填 0.0.0.0 |

### `media.snapshot_empty_body`

**快照 200 + 空体**　出处：B4

真踩过「200 + image/jpeg + 空体」。客户端必须按体长与魔数判定而不是状态码。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `value` | `empty` | empty / short_text / html_login / wrong_content_type | 坏响应形态 |

### `media.snapshot_auth`

**快照鉴权方式**　出处：B5

客户端按 Digest → Basic → 无 顺序试，且只在 401 时才换下一种。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `value` | `digest` | none / basic / digest / token | 鉴权方式；token=URI 里带一次性令牌 |

### `media.snapshot_uri_rotates`

**快照 URI 定期失效**　出处：B6

客户端连续 3 次取图失败才判 URI 失效并重探，中间会盲取。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `seconds` | `60` | 1 ~ 86400 | URI 有效期（秒） |

### `media.no_get_snapshot_uri`

**不实现 GetSnapshotUri**　出处：B7

回 ActionNotSupported，客户端应退化为从码流抽帧或干脆没有快照。

### `media.codec_mismatch`

**声明与实发编码不一致**　出处：—

很多家用机 capability 列了 G.711 / G.722 / AAC，backchannel SDP 却只有 PCMU。运行时必须以 SDP 为准。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `declared` | `AAC` | AAC / G722 / G726 / PCMA | ONVIF 里声明的音频编码 |
| `actual` | `PCMU` | PCMU / PCMA / G722 / AAC | SDP 与 RTP 里实际用的编码 |

### `media.resolution_mismatch`

**声明分辨率与码流不符**　出处：—

客户端不读 VideoEncoderConfiguration、全靠 ffprobe 探流，这条用来测两边不一致时它信谁。


## PTZ（ptz）

### `ptz.config_on_sub_only`

**PTZConfiguration 只挂子码流**　出处：C1

只看主码流 profile 的客户端会判定「没有 PTZ」。

### `ptz.usable_but_unadvertised`

**不挂 PTZConfiguration 但 PTZ 可用**　出处：C2

所有 profile 都不带 PTZConfiguration，但 GetNodes 非空且能真转。客户端应回落到 GetNodes 判定。

### `ptz.spaces_empty`

**SupportedPTZSpaces 为空**　出处：C3

能力声明四档之一：完整 / 为空 / 只有 Default*Space / 只有 pan。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `value` | `empty` | empty / default_only / pan_only | 声明档位 |

### `ptz.range_min_equals_max`

**PTZ Range 的 Min == Max**　出处：C4

除零或退化区间会让客户端的归一化计算炸掉。

### `ptz.zoom_malformed_response`

**非零 Zoom 触发畸形响应**　出处：C5

真机在 ContinuousMove 带非零 Zoom 时回显请求字节 + 500。

### `ptz.factory_300_presets`

**出厂预填 300 个预置位**　出处：C6

「预置点 1..300」加巡航扫描 / 远程重启等功能槽，共享同一个假 PTZPosition。用来压客户端的预置位列表 UI。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `count` | `300` | 1 ~ 1024 | 预置位数量 |

### `ptz.preset_percent_encoded`

**预置位名百分号编码**　出处：C7

相机把 %E9%A2%84 这类编码原样吐回，客户端要 unquote 才显示得对。

### `ptz.no_get_presets`

**不实现 GetPresets**　出处：C8

回 ActionNotSupported，或回一段 not implemented 文本。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `value` | `action_not_supported` | action_not_supported / text / empty_list | 不支持的表现形式 |

### `ptz.set_preset_return_shape`

**SetPreset 返回形态**　出处：C9

有的固件返回裸字符串 token，有的返回带 PresetToken 的对象。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `value` | `object` | object / bare_string / empty | 返回形态 |

### `ptz.response_jitter`

**PTZ 响应延迟抖动**　出处：C10

100~800ms 随机延迟，用来验证客户端的命令乱序防护。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `min_ms` | `100` | 0 ~ 60000 | 最小延迟 |
| `max_ms` | `800` | 0 ~ 60000 | 最大延迟 |

### `ptz.goto_preset_slow`

**GotoPreset 特别慢**　出处：—

响应正常但机械动作要好几秒，GetStatus 期间一直 MOVING。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `ms` | `5000` | 0 ~ 120000 | 到位耗时 |

### `ptz.move_without_status_change`

**移动不反映到 GetStatus**　出处：—

命令返回成功但 GetStatus 的 Position 永远不动，客户端无法闭环。


## 事件（events）

### `events.subscription_port_increment`

**订阅管理器换独立端口且递增**　出处：D1

真机形如 :1024/event-1024_1024，下一条订阅换 :1025。客户端必须用返回的订阅地址而不是主服务地址去 Pull。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `base_port` | `1024` | 1 ~ 65535 | 起始端口 |

### `events.subscription_host_bad`

**订阅地址 host 不可达**　出处：D2

固件把内网地址写进订阅 URL，客户端要么接管 host 要么彻底卡住。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `address` | `10.0.0.1` | — | 写进订阅地址的 host |

### `events.subscription_slot_limit`

**订阅槽位上限**　出处：D3

配合 A9（只增不回收）就是真实的槽位耗尽故障。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `max` | `4` | 1 ~ 1024 | 最大并发订阅数 |
| `on_overflow` | `fault` | fault / evict_oldest / silent_fail | 超限行为 |

### `events.no_get_event_properties`

**不实现 GetEventProperties**　出处：D4

客户端拿不到 TopicSet，只能盲订阅全部。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `value` | `action_not_supported` | action_not_supported / empty_topicset | 不支持的表现形式 |

### `events.bad_xml`

**GetEventProperties 返回非法 XML**　出处：D5

TP-Link TL-IPC 真机返回属性值不加引号的 XML（wstop:topic=true）。客户端必须先补引号再解析。

### `events.flat_address`

**订阅 Address 不套 Reference**　出处：D6

Address 直接放响应下，不包 SubscriptionReference 一层。

### `events.topic_style`

**Topic 命名风格**　出处：D9

标准 ONVIF / TP-Link（LineCrossDetector）/ Reolink（MyRuleDetector）/ Axis（tnsaxis: 前缀）四套。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `value` | `onvif` | onvif / tplink / reolink / axis | 命名风格 |

### `events.pull_always_empty`

**PullMessages 永远返回空**　出处：D10

订阅建得起来、Pull 也不报错，就是永远没有事件。最难查的一类故障。

### `events.state_not_paired`

**属性型事件不发配对的 false**　出处：D11

正常应成对发 IsMotion=true / false；只发 true 会让客户端的移动侦测永远不复位。

### `events.renew_fails`

**Renew 总是失败**　出处：—

客户端必须能在 Renew 失败后重建订阅而不是放弃。

### `events.subscription_expires_at_once`

**订阅建完立刻过期**　出处：—

CreatePullPointSubscription 成功但 TerminationTime 已是过去时间。

### `events.storm`

**事件风暴**　出处：—

每秒推 N 条事件，压客户端的队列与 UI。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `per_second` | `50` | 1 ~ 10000 | 每秒事件数 |

### `events.no_sync_point`

**不支持 SetSynchronizationPoint**　出处：—

客户端拿不到属性型 topic 的当前状态，只能等下一次变化。


## RTSP 与对讲（rtsp）

### `rtsp.audio_capability_lie`

**音频能力谎标**　出处：E1

ONVIF 声明的解码能力与 backchannel SDP 实际开放的 codec 不一致。客户端必须以 SDP 为准。

### `rtsp.g722_sample_rate_8000`

**G.722 采样率错写成 8000**　出处：E2

G.722 的 RTP 时钟率按规范写 8000 但实际是 16000，很多固件在 ONVIF 能力里也错写成 8000。客户端应强制按 16000 处理。

### `rtsp.bitrate_unit`

**码率单位变体**　出处：E3

有的固件按 kbps 报，有的按 bps 报，差 1000 倍。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `value` | `kbps` | kbps / bps | 码率字段单位 |

### `rtsp.audio_decoder_options_shape`

**GetAudioDecoderConfigurationOptions 响应形态**　出处：E4

形态 A：G711/G722/G726/AAC 子元素直挂 opts；形态 B：AudioDecoderOptions 列表，每项带 Encoding。两种都得支持。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `value` | `a` | a / b | a=子元素直挂，b=Options 列表 |

### `rtsp.sample_rate_shape`

**采样率字段形态**　出处：E5

SampleRateRange / SampleRateList / 单值三种形态都在真机上出现过。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `value` | `range` | range / list / single | 字段形态 |

### `rtsp.talkback_dual_track`

**对讲双轨布局**　出处：E6

海康是单轨（只有 sendonly 对讲轨）；大华是双轨（麦克风 recvonly 在前 + 对讲 sendonly 在后），客户端要挑对轨才推得进去。

### `rtsp.talkback_busy_slot`

**对讲忙槽位**　出处：E7

TEARDOWN 后 N 秒内对新的 backchannel DESCRIBE 回 401。海康球机实测 3~4 秒。客户端必须重试而不是判定凭据错误。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `seconds` | `4` | 1 ~ 120 | 忙槽位持续秒数 |

### `rtsp.auth_dual_challenge`

**同时发 Basic + Digest 挑战**　出处：E8

两条 WWW-Authenticate，顺序可配。有的客户端只看第一条。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `order` | `digest_first` | digest_first / basic_first | 两条挑战的顺序 |

### `rtsp.auth_strict_digest_params`

**严格 Digest 参数校验**　出处：E9

客户端 Authorization 里出现挑战没给过的参数（如 algorithm=MD5）就 401。真机上很多客户端因此连不上。

### `rtsp.sdp_nonstandard_codec`

**SDP 声明不规范 codec**　出处：E10

G7221 / G726-32 这类写法，客户端的 codec 表匹配不上要能优雅降级。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `name` | `G7221` | — | 写进 a=rtpmap 的编码名 |

### `rtsp.sdp_no_rtpmap`

**SDP 不写 rtpmap**　出处：E11

只给静态 payload type（0=PCMU / 8=PCMA），客户端要按 RFC 3551 静态表推断。

### `rtsp.sdp_session_level_control`

**只有 session 级 a=control**　出处：E12

媒体级没有 control 属性，客户端拼 SETUP URL 时只能用 session 级的。

### `rtsp.talkback_require_marker`

**校验 talkspurt 首包 marker**　出处：E13

相机要求每段话首包置 RTP marker 位，否则整段静音丢弃。

### `rtsp.setup_no_session_header`

**SETUP 响应不带 Session 头**　出处：E14

违反 RFC 2326，客户端后续 PLAY 无 Session 可用。

### `rtsp.ignore_backchannel_require`

**忽略 backchannel Require 头**　出处：E15

不带 Require 也返回含 sendonly 的 SDP，或严格按 RFC 2326 对不支持的 Require 回 551。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `value` | `ignore` | ignore / strict_551 | ignore=不看 Require 照给，strict_551=不支持就 551 |

### `rtsp.no_audio_output_config`

**不实现 AudioOutput/Decoder 配置**　出处：E16

AddAudioOutputConfiguration / AddAudioDecoderConfiguration 整体回 ActionNotSupported，客户端只能直接开 backchannel 试。

### `rtsp.talkback_stop_draining`

**对讲推流期停止排空**　出处：E19

相机不再读 TCP 接收缓冲，客户端的 sendall 卡死在内核缓冲区满。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `after_ms` | `3000` | 0 ~ 600000 | 推流开始后多久停止排空 |

### `rtsp.max_sessions`

**RTSP 并发会话上限**　出处：—

廉价相机常态：超过上限回 453 Not Enough Bandwidth。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `max` | `2` | 1 ~ 256 | 最大并发会话数 |

### `rtsp.periodic_teardown`

**周期性主动断流**　出处：—

每 N 秒相机自己 TEARDOWN，客户端必须能自动重连。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `seconds` | `300` | 1 ~ 86400 | 断流间隔 |

### `rtsp.rtp_packet_loss`

**RTP 丢包**　出处：—

按比例随机丢弃 RTP 包，测客户端的花屏恢复。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `percent` | `5` | 0 ~ 100 | 丢包率（%） |

### `rtsp.rtp_timestamp_jump`

**RTP 时间戳跳变**　出处：—

周期性让时间戳大幅跳跃，测客户端的抖动缓冲。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `seconds` | `30` | 1 ~ 86400 | 跳变间隔 |
| `delta_ms` | `5000` | -600000 ~ 600000 | 跳变量（毫秒，可负） |

### `rtsp.sps_pps_placement`

**SPS/PPS 位置**　出处：—

只在 SDP 的 sprop-parameter-sets / 只在带内 / 两处都有。只认一处的解码器会黑屏。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `value` | `both` | both / sdp_only / inband_only | 放置策略 |

### `rtsp.video_freeze`

**画面冻结**　出处：—

RTP 继续发但画面内容不变，测客户端的冻结检测。

### `rtsp.video_black`

**黑屏**　出处：—

切换到全黑样片，测客户端的黑屏检测。

### `rtsp.video_fps_change`

**帧率突变**　出处：—

实际帧率与声明不符且中途变化。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `fps` | `5` | 0.1 ~ 120 | 实际帧率 |

### `rtsp.no_rtcp`

**不发 RTCP SR**　出处：—

客户端拿不到 NTP 映射，音视频同步只能靠 RTP 时间戳硬估。


## 传输与设备（transport）

### `transport.malformed_http`

**畸形 HTTP 响应**　出处：F1

回显请求原始字节 + 500，或缺 Content-Length、状态行残缺。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `value` | `echo_500` | echo_500 / no_content_length / bad_status_line / truncated | 畸形形式 |
| `percent` | `100` | 0 ~ 100 | 触发概率（%） |

### `transport.self_signed_tls`

**自签证书 HTTPS**　出处：F2

TP-Link VIGI 的私有 API 走 20443 自签 HTTPS，客户端必须跳过证书校验。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `port` | `20443` | 1 ~ 65535 | HTTPS 端口 |

### `transport.overload_reboot`

**并发过载时模拟重启**　出处：F3

一秒内的 SOAP 请求数超过阈值就整机假死再回来，复现廉价固件被打挂。按速率而不是瞬时并发判定：单事件循环下请求本来就是串行处理的。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `threshold` | `8` | 1 ~ 1024 | 每秒请求数阈值 |
| `offline_seconds` | `20` | 1 ~ 600 | 假死秒数 |

### `transport.system_reboot_real`

**SystemReboot 真的离线**　出处：—

发 Bye → 关掉全部端口 N 秒 → 发 Hello 回来，而不是只回个 OK。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `seconds` | `30` | 1 ~ 600 | 离线秒数 |

### `transport.random_dropout`

**随机掉线**　出处：—

按概率整机离线一小段时间，模拟不稳定的 PoE / WiFi。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `percent` | `5` | 0 ~ 100 | 每次请求触发掉线的概率（%） |
| `seconds` | `10` | 1 ~ 600 | 每次掉线秒数 |

### `transport.huge_response`

**超大响应体**　出处：—

塞进大量填充，测客户端的解析上限与内存。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `kb` | `8192` | 1 ~ 262144 | 响应体大小（KB） |

### `transport.slowloris`

**慢发送**　出处：—

响应分成小块、每块之间等一会儿，测客户端的读超时。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `chunk_bytes` | `16` | 1 ~ 65536 | 每块字节数 |
| `interval_ms` | `500` | 1 ~ 60000 | 块间隔 |

### `transport.global_delay`

**整机响应延迟**　出处：—

所有 HTTP 响应统一延迟，模拟远端 / 弱网相机。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `ms` | `1000` | 0 ~ 120000 | 延迟毫秒数 |

### `transport.rtp_rate_limit`

**RTP 限速**　出处：—

按给定带宽发 RTP，低于码流需求就会持续积压。

| 参数 | 默认 | 取值 | 说明 |
|---|---|---|---|
| `kbps` | `256` | 8 ~ 100000 | 上限带宽（kbps） |


