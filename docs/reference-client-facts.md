# 参照客户端实测事实与怪癖清单

> 这份清单记的是「ONVIF 客户端在真实设备上到底会遇到什么」，是本项目每一条故障注入开关的依据。
>
> **技术栈**：文中的「参照客户端」指一套典型的 Python ONVIF 客户端组合 ——
> onvif-zeep 0.2.12 + zeep 4.3.2 + wsdiscovery 2.1.2，加上自写的 async SOAP 与 RTSP 对讲客户端。
> 这几个库都是开源的，文中关于它们解析行为的描述都可以对着源码复核。
>
> **「真机」**指市面上常见的 IP 摄像头（海康、大华、Reolink、TP-Link VIGI / TL-IPC 等）在
> 实际对接中表现出来的行为。这些行为不少是偏离 ONVIF 规范的，但客户端必须扛住 —— 这正是
> 本项目要复现它们的原因。
>
> 编号（A1、E7 …）被 `plan.md` 与代码注释引用；`docs/quirks.md` 是从代码里的 quirk 表生成的
> 对照表，只列开关本身，不含这里的背景。

## 1. WS-Discovery

- 客户端库 wsdiscovery 2.1.2：`searchServices(timeout=3)` 是「发 Probe → 纯 sleep 3s → 返回累积结果」，不是等到第一个应答。模拟器在 3s 窗口内任何时刻回都行。
- Probe 多播到 `239.255.255.250:3702`（IPv6 `FF02::C`），监听 socket `bind(('', 3702))`。同一 Probe **重发 4 次**（MessageID 相同），随机间隔 50 / 250 / 500 ms。
- Probe **不带 `<d:Types>` 过滤**（`types=None`）。模拟器不能以 `dn:NetworkVideoTransmitter` 出现为应答前提。
- 命名空间：
  - `NS_DISCOVERY = http://schemas.xmlsoap.org/ws/2005/04/discovery`（WS-Discovery 1.0，不是 OASIS 2009/01）
  - `NS_ADDRESSING = http://schemas.xmlsoap.org/ws/2004/08/addressing`（不是 2005/08）
  - SOAP 1.2 信封
  - 解析器先按 `NS_ADDRESSING` 找 `Action`，找不到整包丢弃。ProbeMatch 里 `ProbeMatch / Types / Scopes / XAddrs / MetadataVersion` 全按 2005/04 取。
- ProbeMatch 必填：`MessageID`、`RelatesTo`（缺则索引异常）、`MetadataVersion`（缺则整包丢）。`To` 可缺。
- 收到 ProbeMatch 若 `XAddrs` 为空，客户端自动多播 `Resolve`。
- 去重键 = EPR（`wsa:Address`，通常 `urn:uuid:…`），同 EPR 后到覆盖先到；上层只取 `getXAddrs()[0]`。所以多网卡回多份时**最后到达那份的第一个 XAddr 胜出**。
- 上层从 Scopes 读取的只有 `onvif.org/name/…`（`unquote` 解码，首次命中锁定）与 `onvif.org/hardware/…`；`location` / `Profile` / `type` 全部忽略。匹配是子串包含不是前缀。
- Scopes 在线路上是空格分隔一行，值里空格用 `%20`。
- 上层额外做的事：读 `/proc/net/arp` 按 IP 查 MAC；「已添加」判定按 hostname 比对库里已有相机的 RTSP / ONVIF URL。

## 2. 建连（onvif-zeep 0.2.12）

- `ONVIFCamera(...)` 构造时**无条件调 `update_xaddrs()`**，一行就产生网络请求。
- 构造时清掉进程的 `http_proxy` / `https_proxy` 环境变量。
- WSDL 从本地磁盘读（`onvif/wsdl/`），绝不从相机拉。模拟器不需要 `?wsdl` 端点。
- devicemgmt 的 XAddr 硬编码 `http://{host}:{port}/onvif/device_service`，不看任何返回值。
- `update_xaddrs()` 序列：
  1. 只有 `adjust_time=True` 才调 `GetSystemDateAndTime` 做时钟补偿。**参照客户端所有调用点都没传**，即不做时钟补偿。
  2. `GetCapabilities({'Category': 'All'})`，不是 `GetServices`。遍历 Device / Media / PTZ / Imaging / Events / Analytics 各大类取 `XAddr`，按命名空间存表。
  3. 紧接着 `create_events_service()` + `CreatePullPointSubscription()`，存订阅地址，整段裸 `except: pass`。**每次构造都在相机上建一条 PullPoint 订阅且从不 Unsubscribe。**
- `create_*_service()` 时按命名空间查 XAddr 表，查不到直接抛 `Device doesn't support service`，不回落约定路径。
- 服务命名空间表：
  ```
  devicemgmt  http://www.onvif.org/ver10/device/wsdl
  media       http://www.onvif.org/ver10/media/wsdl
  ptz         http://www.onvif.org/ver20/ptz/wsdl
  imaging     http://www.onvif.org/ver20/imaging/wsdl
  deviceio    http://www.onvif.org/ver10/deviceIO/wsdl
  events      http://www.onvif.org/ver10/events/wsdl   (EventBinding / PullPointSubscriptionBinding)
  analytics   http://www.onvif.org/ver20/analytics/wsdl
  ```
- 鉴权：WS-Security UsernameToken **PasswordDigest**（`Base64(SHA1(nonce + created + password))`），每个 SOAP 请求都带。不是 HTTP Digest。
- zeep `strict=False`、`xml_huge_tree=True`：响应多字段 / 顺序错 / 少可选字段也会吃下。

### 参照客户端的手写 async SOAP 栈（PullPoint 长轮询专用）

- UsernameToken：nonce `os.urandom(16)` 每次新生成；`Created` 秒精度无毫秒；`mustUnderstand="1"`；username 为空时不带 Security 头（匿名）。
- 信封 SOAP 1.2，所有前缀声明在 Envelope 上，可选 `wsa:Action` / `wsa:To`，`wsa` 用 `http://www.w3.org/2005/08/addressing`（与 WS-Discovery 那边不同）。Content-Type `application/soap+xml; charset=utf-8`。
- HTTP 层：`401 / 403` → 认证错误；`>= 400 且 != 500` → 一般错误；**500 放行给 body 解析**（SOAP Fault 常以 500 返回）；**SOAP Fault 也可能走 HTTP 200**。Fault 文本含 `notauthorized / not authorized / unauthorized / authentication / sender not authorized / failedauthentication` 任一关键词 → 认证错误。
- 畸形 XML 修补：给标签内**未加引号的属性值**补引号（`wstop:topic=true` → `topic="true"`）。TP-Link TL-IPC 的 GetEventProperties 就返回这种非法 XML。
- `GetServices`（`IncludeCapability=false`）解析：按**带版本**的命名空间片段匹配（`ver10/events`、`ver20/ptz`、`ver10/media`），因为 Media(ver10) 与 Media2(ver20) 的 ns 都含 `/media/`，只按服务名匹配且 Media2 排前面时会把 media 解析到 Media2 端点，往那里发 ver10 的 `GetProfiles` 吃 `ActionNotSupported`（海康 / Axis 双栈固件真实布局）。
- XAddr 处理：**只接管 host:port，保留 path**。真机 TL-IPC652P-A4 报 `:2020/onvif/service` 而实际连的是 80，接管后照样通。GetServices 失败回落约定路径 `/onvif/{event,ptz,media}_service`。
- 服务 URL 缓存 10 分钟，按 base_url。

## 3. Media / 快照

- 加相机：`GetProfiles()` 后**对每一个 profile** 发 `GetStreamUri`，`StreamSetup = {Stream: "RTP-Unicast", Transport: {Protocol: "RTSP"}}`。读 `profile.token` 与 `profile.Name`。
- URI 提取：`resp` 是 str 直接用；否则 `resp.Uri`；否则 dict 的 `Uri`。对讲路径另外还认 `MediaUri.Uri` 嵌套。
- 主 / 子码流判定**只看 profile `Name`**：小写含 `main / primary / high` 为主，含 `sub / secondary / low` 为子；都不含则按顺序猜。
- 凭据：把 `user:pass@` 注入 RTSP URL，**丢弃相机返回 URI 里自带的 userinfo**。
- has_ptz 判定：任一 profile 有 `PTZConfiguration`；都没有才 `GetNodes()` 看非空；异常即 False。
- 超时：整个解析 15s。
- 分辨率 / 编码 / 帧率 / 音频编码**不读 ONVIF 的 VideoEncoderConfiguration，全靠 ffprobe 探 RTSP**（`-rtsp_transport tcp -rw_timeout … -show_streams`，8s）。ffprobe 不在 PATH 时跳过预检。
- PTZ 连接缓存同时调 `create_imaging_service()` + `media.GetVideoSources()` 取 `video_sources[0].token`。
- 快照：`GetProfiles()` 后**只用 `profiles[0]`** 的 token 调 `GetSnapshotUri`，取 `Uri`。探 URI 超时 6s，取图 5s，探不到退避 300s，连续 3 次取图失败判 URI 失效重探。
- 取图鉴权顺序 `DigestAuth → BasicAuth → 无`，**只有 401 才换下一种**；`follow_redirects=True`。
- 成功判据：空体直接拒（真踩过「200 + image/jpeg + 空体」）；JPEG / PNG 魔数直接信；无魔数则 `content-type` 以 `image/` 开头且 body ≥ 64 字节才接受。

## 4. 音频 / 对讲

### 4.1 能力探测

- 两条来源互相校验：ONVIF 声明的解码能力 vs backchannel SDP 实际开放的 codec。实测结论：很多家用摄像头 capability 列了 G.711 / G.722 / AAC，但 backchannel SDP 只列 PCMU —— 运行时必须以 SDP 为准。
- `GetAudioDecoderConfigurationOptions({ProfileToken: profiles[0].token})`，异常返回空不抛。两种响应形态都支持：
  - A：opts 直挂 `G711 / G722 / G726 / AAC` 子元素，各含 `Bitrate` + `SampleRateRange`
  - B：`AudioDecoderOptions` 列表，每项 `Encoding` + `SampleRateList | SampleRateRange` + `BitrateList | BitrateRange`
- 规范化：`G711` 同时产出 PCMU 与 PCMA；`G722` 采样率**强制 16000**（即使相机错写 8000）；`G726` 跳过。
- SampleRate 形态：`{Min, Max}`（优先 Max）/ `List[int]` / 裸数字 / `Items` 容器。Bitrate `< 1024` 视作 kbps，否则 `// 1000`。
- `GetAudioOutputs` / `GetAudioSources` 从不调用。
- SDP 侧：`describe_only()` → 解析 → **挑 sendonly 轨** → 列 codec。不挑轨时大华（麦克风轨 + 对讲轨）会报告麦克风轨的 codec。
- 选码：优先 SDP 集合，空退 capability，都空兜底 PCMU。偏好 `G722 > PCMA > PCMU`。
- Phase 1 失败整体降级为 `PCMU 8000 / 64kbps / 1ch` + degraded；SDP 阶段失败不算 degraded。

### 4.2 对讲 URI 解析

顺序，每步失败只 debug 跳过：
1. `GetProfiles()` → `profiles[0]`
2. `GetCompatibleAudioOutputConfigurations({ProfileToken})` → `[0].token` → `AddAudioOutputConfiguration({ProfileToken, ConfigurationToken})`
3. `GetCompatibleAudioDecoderConfigurations` → `AddAudioDecoderConfiguration`（**写操作，模拟器要真的改 profile**）
4. `GetStreamUri`（RTP-Unicast / RTSP）
5. 返回 URI 必须含 `rtsp://`（大小写不敏感），再注入凭据。

目标选择：有 ONVIF URL 走 ONVIF；否则海康 / 大华直接用主码流 RTSP + 厂商账号。凭据优先级 `vendor_api_* → onvif_* → rtsp_main 的 userinfo`。

### 4.3 完整 RTSP backchannel 序列

- 连接：scheme 必须 `rtsp`，端口默认 554，`create_connection(timeout=6.0)`。
- 两档超时：握手 6.0s（OPTIONS / DESCRIBE / SETUP / PLAY），推流期 sendall 2.0s（握手完立刻收窄，TEARDOWN 也受它兜）。
- 请求头：`User-Agent`、`CSeq` 从 1 递增、有 body 才 `Content-Length`、`Session` 仅在已有会话且方法不是 DESCRIBE / OPTIONS / SETUP 时带、`Authorization`。
- 401 重试：用 `WWW-Authenticate` 更新挑战后**原样重发一次**。
- 响应解析：状态行必须 `RTSP/1.0` 开头；头名小写化；相机常同时发两条 `WWW-Authenticate`（TP-LINK 实测 Basic 一条 Digest 一条）→ **偏好 Digest，不让 Basic 覆盖**。
- Digest 参数：**只带挑战里出现的字段**。TP-LINK RTSP Server 多带一个挑战没要求的 `algorithm=MD5` 就 401（同 nonce 同 response，去掉即 200）。
- ① `OPTIONS <uri>`，≥400 即错。
- ② `DESCRIBE <uri>`，`Accept: application/sdp`，`Require: www.onvif.org/ver20/backchannel`。
  - 401 有界重试：6 次 × 1.0s（只对 backchannel DESCRIBE、只在配了用户名时、探测路径不重试）。真机情报：**海康球机上一条会话槽位释放需 ~3-4s**，期间对新 DESCRIBE 回 401 当「忙」信号。
- ③ SDP 解析：`m=audio <port> <proto> <fmt…>` 起段；只认 `a=rtpmap` / `a=fmtp` / `a=control` / `a=sendonly|recvonly|sendrecv|inactive`。
- ④ 选轨：**第一条 `sendonly` 的 m=audio**；没有则 `medias[0]`。大华同时返回麦克风轨（recvonly）+ 对讲轨（sendonly），推到麦克风轨相机不播；海康只返回一条对讲轨。
- ⑤ 选码：先按 `a=rtpmap` 名字匹配（PT 与静态表不符时用 SDP 的 PT）；兜底按 `m=audio` 行静态 PT 反查 `{0: PCMU, 8: PCMA, 9: G722}`；都不匹配时 PTT 路径**强推 PCMU PT=0**（部分廉价相机 SDP 只声明 G7221 / G726，固件喇叭对 PCMU 照样能解）。
- ⑥ track URI：优先 m=audio 段 `a=control`；没有则全 SDP 首个 `a=control`（session 级）。拼接：`*` → base；`rtsp://` 开头原样；`/` 开头 scheme+netloc+control（丢 query）；否则 base 目录 + control。
- ⑦ `SETUP <track_uri>`，`Transport: RTP/AVP/TCP;unicast;interleaved=0-1`。**TCP interleaved，不用 UDP。**响应必须 200 且带 `Session`（取 `;` 前）。
- ⑧ `PLAY <uri>`，只带 `Require: www.onvif.org/ver20/backchannel`。**是 PLAY 不是 RECORD。**
- ⑨ RTP：头 12 字节，V=2；**`marker=0x80` 仅 talkspurt 首包**（漏设时冷启动相机丢掉首个 talkburst，解码器还热时又正常，故偶发）；seq / ts / ssrc 随机初值；interleaved 封装 `$ | 0x00 | len(2B) | RTP`，**channel 0**。
- 编码参数：PCMU PT0 8k 20ms 160B 静音 0xFF；PCMA PT8 同上静音 0xD5；G722 PT9 采样 16k **RTP 时钟 8k** 20ms 160B；AAC 未实现（无 RFC 3640 封装）。
- 发送节奏：喂入侧每帧 sleep 20ms；一次性灌大块会让相机 jitter buffer 溢出（音频跳跃 / 噪点）。队列满丢最旧包。
- ⑩ `TEARDOWN <uri>` + `Session`，不做 401 重试，整段吞异常。

### 4.4 TP-Link TL-IPC 的 MULTITRANS 私有对讲（与 ONVIF 无关）

- 国内线 TL-IPC 没有 ONVIF backchannel sendonly 轨：带不带 `Require` 头 SDP 一样，相机返 200 却默默忽略该头（按 RFC 2326 应返 551）。真机 SDP：`m=video H265 / m=audio PCMA(麦克风) / m=application TP-LINK smart/1/90000`。
- 协议：TCP 554；`MULTITRANS rtsp://<host>/multitrans RTSP/1.0` + `CSeq: 0` + `X-Client-UUID` → 401（Basic + Digest 双挑战）或 200 → 带 Digest 重发 → 200 + `Session` → 同连接发 JSON `{"type":"request","seq":0,"params":{"method":"get","talk":{"mode":"half_duplex"}}}` → `{"error_code":0}` → 音频走 interleaved **channel 0x01**，PT=8 PCMA。
- Digest HA2 变体兜底：`(MULTITRANS, absolute) → (DESCRIBE, absolute) → (MULTITRANS, path) → (DESCRIBE, path)`。
- 三条真机常量：前导静音 300ms（不发会吞开头一两个字，且必须按实时节奏发）；线路包 **2048B = 256ms**（256 / 512B 断续，1024B 起清晰，**必须是 1024 的整数倍**，1536B 卡顿，3072 / 4096B 无声）；关连接前等 0.6s 排空否则话尾被切。
- 线路恒 PCMA，输入 PCMU 用 256 项查表转码。
- 判定：厂商 / 型号含 `tp-link | tplink`，排除 VIGI 与 tapo，命中 `tl-ipc / tl ipc / tlipc`。

## 5. PTZ

### 5.1 能力判定

1. `GetProfiles()` → 选 profile（见 5.2）
2. node token：先 `profile.PTZConfiguration.NodeToken`；没有则 `GetNodes()`（空 → 「不支持 PTZ」），用 `nodes[0]`；有则 `GetNode({NodeToken})`
3. 读 `node.SupportedPTZSpaces.ContinuousPanTiltVelocitySpace / ContinuousZoomVelocitySpace`
4. 每个 space 看 `XRange / YRange`，判据「Min 或 Max 任一非零」（有固件 Min=Max=1）。`x_ok` → pan，`y_ok` → tilt，**同一 space 内 x 与 y 都 ok** 才 pan_tilt（决定对角键）；zoom 看任一 zoom space 的 XRange。
5. **一条速度空间都没声明**：看 `PTZConfiguration.DefaultContinuousPanTiltVelocitySpace / DefaultContinuousZoomVelocitySpace`；都没有 → 返回 degraded 且不写任何 `supports_*`，让「保守全支持」默认生效。真机 TL-IPC652P-A4：SupportedPTZSpaces 为空但八向 ContinuousMove 全可用。写成 `false` 的后果：方向键好使、点画面控制失效。
6. 超时 6s；异常一律 degraded + 全支持。

### 5.2 profile 选择（两条路径取舍相反）

- zeep 栈：挑第一个带 `PTZConfiguration` 的 profile；一个都没有时**回落 `profiles[0]`**。多码流相机常只有一路 profile 挂 PTZConfiguration，拿错去调 ContinuousMove / GetPresets 直接 Fault `The requested profile token does not reference a PTZ configuration`。
- 手写栈：同样挑第一个带 PTZConfiguration 的，**没有则返回 None 不回落**。GetProfiles 瞬时失败原样上抛不缓存（以前吞成 None 导致一次超时把 Stop 指令吞掉，相机转到机械限位）。token 缓存 10 分钟，只缓存成功值。profile 元素 token 从 attrib `token` 或子元素文本取；PTZConfiguration 靠直接子元素 localname。

### 5.3 运动与预置位

- zeep 栈：`ContinuousMove` 的 `Velocity = {PanTilt: {x, y}, Zoom: {x}}` **两者同时发**，分量 clamp [-1, 1]，对角方向两轴各 ±1 不归一化（合速度 1.41）；`Stop` 的 `PanTilt = Zoom = True`；`GetPresets({ProfileToken})` 读 `token / Name / PTZPosition.PanTilt.x/y / Zoom.x`；`SetPreset` 返回值两种形态都接（裸字符串或带 `.token` 对象）；`GotoPreset` 带 `Speed`；`RemovePreset`。任何异常 invalidate 整条连接缓存。
- 命令闸门：按相机串行化，`asyncio.Lock` FIFO，5s 超时后宁可乱序也发；排队中的 ContinuousMove 被新的替换则丢弃，Stop / GotoPreset / SetPreset 永不丢。原因：自动追踪最快 10 命令/秒、SOAP RTT 100~300ms，**一条迟到的反向 ContinuousMove 落在 Stop 之后云台一直转**。
- 手写栈：**只发被命令的那个轴**（zoom 非零只发 `<tt:Zoom x>`，否则只发 `<tt:PanTilt x y>`），因为 TL-IPC652P-A4 对非零 Zoom 会吐畸形 HTTP（回显请求字节 + 500）。`GotoPreset` 不发 Speed。GetPresets 从 localname `preset` 元素取 attrib `token` 与子元素 `Name`。
- 出厂预填过滤：海康等固件把 **300 个预置位槽出厂全部预填**（「预置点 1..300」+ 巡航扫描 / 远程重启功能槽），共享同一个假 PTZPosition（如 `(0.0, 1.0, 0.0)`）。客户端启发式：同一精确位置聚 ≥ 8 且过半占位名 → 整组丢；占位名总数 ≥ 30 → 丢。占位名正则 `^(?:预置点|预置位|preset)\s*[_-]?\s*(\d+)$` 且数字须与 token 一致。
- 百分号编码名：同为 TP-Link，一台返回 `啊啊`、另一台返回 `%E6%B5%8B%E8%AF%95`。只在「像 `%XX` 且能严格解 UTF-8」时解码，且必须在过滤之前解码。
- `GetPresets` 未实现的识别：Fault 文本匹配 `not implemented | actionnotsupported`（大小写不敏感）→ 返回空列表。

## 6. 事件：PullPoint

- 生命周期：`CreatePullPointSubscription` → 循环 `PullMessages` → 到期前 `Renew` → 退出 `Unsubscribe`。不可恢复错误由外层退避重连（2s 起，×2 封顶 30s，收到事件即重置；认证错误固定 30s）。
- 报文（手写 SOAP 1.2）：
  | 操作 | Body | wsa:Action | 目标 |
  |---|---|---|---|
  | Create | `<tev:CreatePullPointSubscription><tev:InitialTerminationTime>PT1H</tev:InitialTerminationTime></tev:CreatePullPointSubscription>` | `…/ver10/events/wsdl/EventPortType/CreatePullPointSubscriptionRequest` | event service URL |
  | Pull | `<tev:PullMessages><tev:Timeout>PT8S</tev:Timeout><tev:MessageLimit>100</tev:MessageLimit></tev:PullMessages>` | `…/ver10/events/wsdl/PullPointSubscription/PullMessagesRequest` | **订阅地址**，`wsa:To` = 订阅地址 |
  | Renew | `<wsnt:Renew><wsnt:TerminationTime>PT1H</wsnt:TerminationTime></wsnt:Renew>` | `http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/RenewRequest` | 订阅地址 + `wsa:To` |
  | Unsubscribe | `<wsnt:Unsubscribe/>` | `…/wsn/bw-2/SubscriptionManager/UnsubscribeRequest` | 订阅地址 + `wsa:To`，超时 5s |
- 常量：TTL PT1H，到期前 5 分钟续订，Pull 超时 PT8S，MessageLimit 100，Pull 的 HTTP 超时 20s。
- 三个互操作细节：
  1. **订阅地址会换端口**：`SubscriptionReference/Address` 才是后续目标，同时是 HTTP 目标和 `wsa:To`。客户端**保留设备给的 port + path，只把 host 换成连得上的那个**。真机 TL-IPC652P-A4 的订阅地址是 `http://<ip>:1025/event-1025_1025`，**且每次订阅端口递增**（1024 → 1025 → …）。地址提取：先找 `SubscriptionReference` 下的 `Address`，找不到在整个响应里找 `Address`（少数固件直接放响应下）。
  2. **空拉取不是错误**：8s 返回零条是正常心跳，不计退避。
  3. **能力探测建的订阅必须 Unsubscribe**：槽位常只有 4~8 个，泄漏让后续订阅失败，症状「加相机时好时坏」。探测的错误语义：传输 / 认证错误上抛（判不出来），其它 Fault（如 ActionNotSupported）→ 判否。真机：**并发订阅会互踢**，生产订阅跑着时再跑探测，生产订阅立刻断开并 churn 重连。
- NotificationMessage 解析：遍历 localname `notificationmessage`；Topic 取 localname `topic` 的 `itertext`；**`Message/Source` 与 `Message/Data` 下所有 `SimpleItem` 展平成一个 dict**，属性名 `Name / name`、`Value / value` 都认（人车分类通常在 Data 的 `Type=Human`，规则名在 Source 的 `Rule=MyLine`）；时间取 `Message` 元素的 `UtcTime / utcTime`，失败落 now。
- 未映射 topic 每 60s 汇总一行 debug，且只对「未知 topic」留痕；已映射但 `IsMotion=false` 的「事件结束」是有意丢弃。
- topic 映射：按 `/` 拆段、每段去掉命名空间前缀、小写拼回后**子串包含**匹配：
  ```
  cellmotiondetector/motion, videosource/motionalarm, motionalarm     → motion
  tamperdetector/tamper, videosource/globalscenechange, globalscenechange → tamper
  linedetector/crossed, fielddetector/objectsinside, motionregiondetector/motion,
  linecrossdetector/linecross (TP-Link 实测), intrusiondetector/intrusion (TP-Link 实测) → zone_transition
  peopledetect / persondetect / humandetect → person;  vehicledetect / cardetect → car
  facedetect / facedetector → face;  loiterdetector → loitering
  audiodetection / audioclassification → audio_anomaly
  ```
  免责：这张表是 ONVIF 通用约定，`MyRuleDetector` 类自定义 topic 各家不同。
- SimpleItem 分类覆盖：key `Type / ObjectType / ClassType / Class / ObjectClass`，值 `human / person / people / pedestrian → person`，`vehicle / car / truck / bus / motorcycle / bike / bicycle → car`，`face`、`animal / pet`、`dog`、`cat`。
- 状态位丢弃：`IsMotion / State / IsInside / Active / LogicalState` 任一值 ∈ `{false, 0, off, no, inactive}` → 整条丢弃（避免一次入侵产生开始 + 结束两条）。
- `GetEventProperties`：把 `TopicSet` 树扁平化成 `A/B/C` 路径，叶子判据 = 带 `topic="true"` 属性或无子 topic 元素；跳过 `MessageDescription / Source / Data / SimpleItemDescription / ElementItemDescription / ParentTopic`；任何失败返回空列表。真机 TL-IPC652P-A4 不实现（返回空）且响应是属性不带引号的非法 XML。

## 7. 厂商识别与私有接口

### 7.1 识别流程

- 两级嗅探：一级 `Hikvision / Dahua / Reolink / Vigi / TlIpc` **并发 probe，取第一个 `matches()` 命中的**；二级 `GenericOnvif`（恒 True）。不能合并成一级，否则兜底会随机抢走品牌适配器。
- 嗅探期端口强制走 80 / 443 通用规则；probe 超时默认 2.5s。
- host 从 ONVIF URL → 主码流 RTSP 取；凭据 `vendor_api_* → onvif_* → rtsp 的 userinfo`。
- `matches()` 在 `manufacturer + model + hardware_id` 拼成的小写串上做子串匹配：

| 适配器 | 匹配 |
|---|---|
| Hikvision | `hikvision`, `annke`, `laview` |
| Dahua | `dahua`, `amcrest`, `lorex`, `imou`, `laview` |
| Reolink | `reolink` |
| VIGI | 排除 `tapo / tl-ipc / tl ipc / tlipc` 后命中 `vigi / insight` |
| TL-IPC | 排除 VIGI / tapo 后命中 `tl-ipc / tl ipc / tlipc` |
| Generic | 恒 True |

`laview` 同时在海康与大华表里，谁先回谁赢。

### 7.2 各厂商端点

**Hikvision（ISAPI，HTTP Digest）**
```
probe   GET  /ISAPI/System/deviceInfo
caps    GET  /ISAPI/PTZCtrl/channels/{ch}/capabilities
        GET  /ISAPI/PTZCtrl/channels/{ch}/presets
        GET  /ISAPI/Streaming/channels/{ch}01
        GET  /ISAPI/Image/channels/{ch}/supplementLight/capabilities
        GET  /ISAPI/System/IO/outputs
events  GET  /ISAPI/Event/notification/alertStream        multipart XML 长连接
presets PUT  /ISAPI/PTZCtrl/channels/{ch}/presets/{token}/goto
        PUT  /ISAPI/PTZCtrl/channels/{ch}/presets/{token}
PTZ     PUT  /ISAPI/PTZCtrl/channels/{ch}/continuous       body <PTZData><pan/><tilt/><zoom/></PTZData>，-100..100
alarm   PUT  /ISAPI/System/IO/outputs/1/trigger
light   GET/PUT /ISAPI/Image/channels/{ch}/supplementLight
ircut   GET/PUT /ISAPI/Image/channels/{ch}/ircutFilter
snap    GET  /ISAPI/Streaming/channels/{ch}01/picture
```
事件 `EventNotificationAlert`：读 `eventType / targetType / licensePlate|plateNo|plate`。映射：`linedetection / fielddetection / regionentrance / regionexiting → zone_transition`，`vmd / pir → motion`，`facedetection / facesnap → face`，`anpr / vehicledetection → car`，`tamperdetection / shelteralarm → tamper`；`targetType ∈ {human, person}` 覆盖为 person，`{vehicle, car}` 为 car。

**Dahua（CGI，HTTP Digest）**
```
probe   GET /cgi-bin/magicBox.cgi?action=getSystemInfo   （失败回落 RPC2 magicBox.getSystemInfo，乐橙线部分固件藏 CGI）
caps    GET /cgi-bin/ptz.cgi?action=getCurrentProtocolCaps&channel={ch-1}
        GET /cgi-bin/ptz.cgi?action=getPresets&channel={ch-1}
        GET /cgi-bin/configManager.cgi?action=getConfig&name=Encode
events  GET /cgi-bin/eventManager.cgi?action=attach&codes=[…]&heartbeat=5   长连接，每 5s 一行 Heartbeat
PTZ     GET /cgi-bin/ptz.cgi?action=start|stop…
siren   GET /cgi-bin/coaxialControlIO.cgi
light   GET /cgi-bin/configManager.cgi?action=getConfig&name=Lighting_V2 / setConfig
snap    GET /cgi-bin/snapshot.cgi
另有 DHIP 二进制 TCP 5000 与 RPC2
```
订阅事件码：`VideoMotion, CrossLineDetection, CrossRegionDetection, SmartMotionHuman, SmartMotionVehicle, FaceDetection, TrafficJunction, VideoBlind`；只接受 `action ∈ {Start, Pulse}`。

**Reolink（JSON-RPC `/api.cgi`，token 认证，不用 Digest）**
```
POST /api.cgi?cmd=Login   body [{"cmd":"Login","param":{"User":{"userName":…,"password":…}}}]
     → data[0].value.Token.name，之后每请求带 &token=<t>
POST /api.cgi?cmd=<Cmd>&token=<t>   body [{"cmd":…,"action":0,"param":{"channel": ch-1}}]
```
命令：`GetDevInfo`、`GetAbility`、`GetAiState`（→ 事件能力）、`GetEnc`（→ 编码信息）、`GetWhiteLed / SetWhiteLed`（→ 报警输出 + 灯光）、`GetPtzPreset / SetPtzPreset / DelPtzPreset`（→ PTZ + 预置位）、`PtzCtrl`、`AudioAlarmPlay`、`Snap`。token 过期：`code == -6` 或 `error.rspCode == -6`。AI 状态映射：`people / person → person, vehicle / car → car, dog_cat → pet, dog, cat, package → package_arrival, face, md → motion`；`active=False` 只有 package 转 `package_removal`，其余丢弃。

**TP-Link VIGI（20443 HTTPS JSON-RPC + ONVIF 80 双源）**
- `default_api_port = 20443`，TLS 自签（客户端 `verify=False`）。probe 走 ONVIF `GetDeviceInformation`（OpenAPI 的 getDeviceStatus 拿不到厂商 / 型号）。
- 同时持 ONVIF SOAP 客户端（80）与 ONVIF PTZ（连续 PTZ 走 ONVIF；OpenAPI 的 motorMove 是绝对坐标）。
- 认证 `POST /` `{"method":"doAuth","params":null}` 两步 SHA-256 挑战 / 应答，stok 25 分钟刷新。
- 事件 `subscribeMsg`，订阅前必须逐个打开 8 个检测开关：`PeopleDetection, VehicleDetection, AudioAnomalyDetection, LoiterDetection, SceneChangeDetection, AreaEntryDetection, AreaLeaveDetection, DropAndTakeDetection`（漏了的表现是订阅成功但零事件；不支持的型号返 -10030 静默跳过）。
- `getPresetPoint` → 预置位；不实现快照。

**TP-Link TL-IPC（`/stok=<t>/ds` 私有 JSON）**
- 继承通用 ONVIF 适配器，事件 / PTZ 全走 ONVIF，只用私有 API 补灯光。`POST /stok=<stok>/ds`，`error_code` 非 0 即错，stok 失效只重试一次。

## 8. Imaging

- 只调两个操作：`GetVideoSources()` 取 `video_sources[0].token`（取不到 imaging 置 None）；`GetImagingSettings({VideoSourceToken})` 读 `IrCutFilter`（`ON` 白天 / `OFF` 夜视 / `AUTO`）。另 `GetServiceCapabilities` 用于能力展示。
- 从不调 `SetImagingSettings / GetOptions / Move / GetStatus`；IR-cut 的写走厂商私有 API。

## 9. 怪癖清单（→ 故障注入开关）

### A. 发现 / 建连
| # | 怪癖 | 来源 |
|---|---|---|
| A1 | ProbeMatch 用 2009/01 ns（而非 2005/04）→ wsdiscovery 2.1.2 直接丢包 | 库命名空间常量 |
| A2 | ProbeMatch 不带 XAddrs → 客户端补发 Resolve | 库行为 |
| A3 | ProbeMatch 缺 `MetadataVersion` → 解析抛异常整包丢 | 库行为 |
| A4 | XAddrs 第一条是不可达的内网 / 占位 IP（多网口 / NAT，`192.168.1.1`、`192.168.1.99`、hostname） | 客户端注释，真机 |
| A5 | XAddr 端口非 80（真机 `:2020/onvif/service`） | TL-IPC652P-A4 |
| A6 | GetServices 同时列 Media + Media2 且 Media2 在前 → media 解析到 Media2 → ver10 GetProfiles 吃 ActionNotSupported | 海康 / Axis 双栈 |
| A7 | 不实现 GetServices（客户端回落约定路径） | 客户端注释 |
| A8 | WS-Security `Created` 时间窗严格 → 客户端不做时钟补偿，时钟偏了全线 401 | 库默认 `adjust_time=False` |
| A9 | onvif-zeep 每次构造都建一条 PullPoint 订阅且永不 Unsubscribe → 槽位泄漏 | 库行为 |
| A10 | GetDeviceInformation 只应答部分字段 / 返回空 | 客户端全字段 `or None` |

### B. Media / 快照
| # | 怪癖 | 来源 |
|---|---|---|
| B1 | profile 名不含 main / sub / primary / secondary / high / low → 主子码流按顺序猜 | 客户端判定逻辑 |
| B2 | GetStreamUri 返回的 URI 自带 userinfo | 客户端丢弃重注入 |
| B3 | GetStreamUri 返回 `MediaUri.Uri` 嵌套形态 | 两条路径一认一不认 |
| B4 | 快照 `200 + image/jpeg + 空体` | 客户端注释「真踩过」 |
| B5 | 快照只接受 Basic 不接受 Digest / 只在 401 才换鉴权 | 客户端顺序 |
| B6 | 快照 URI 随重配 / 重启失效 | 客户端 3 次失败重探 |
| B7 | GetSnapshotUri 不实现 | 客户端 300s 退避 |

### C. PTZ
| # | 怪癖 | 来源 |
|---|---|---|
| C1 | 只有一路 profile 挂 PTZConfiguration；拿错就 Fault `does not reference a PTZ configuration` | 工单日志 |
| C2 | 一个 profile 都不挂 PTZConfiguration，但 ContinuousMove 照常可用 | 两条路径取舍相反 |
| C3 | SupportedPTZSpaces 为空但八向 ContinuousMove 全可用 | TL-IPC652P-A4 |
| C4 | Range 里 `Min == Max` | 客户端放宽判据 |
| C5 | 非零 Zoom 让固件吐畸形 HTTP（回显请求字节 + 500） | TL-IPC652P-A4 |
| C6 | 出厂预填 300 个预置位，共享同一个假 PTZPosition，含功能槽 | 海康 |
| C7 | 预置位名字百分号编码原样吐回（同品牌两台行为不同） | TP-Link |
| C8 | GetPresets 未实现 → `ActionNotSupported` / `not implemented` | 客户端识别正则 |
| C9 | SetPreset 返回值形态不定（裸字符串 vs 对象） | 客户端两种都接 |
| C10 | SOAP RTT 100~300ms 抖动 → 迟到的 ContinuousMove 落在 Stop 之后云台一直转 | 客户端命令闸门注释 |

### D. 事件
| # | 怪癖 | 来源 |
|---|---|---|
| D1 | 订阅地址换端口且每次订阅递增（1024 → 1025 → …） | TL-IPC652P-A4 |
| D2 | 订阅地址 host 是设备自报的内网地址 | 客户端 rehost 注释 |
| D3 | 订阅槽位只有 4~8 个；并发订阅互踢 | 客户端注释 + 真机 |
| D4 | GetEventProperties 不实现 / 返回空 | TL-IPC652P-A4 |
| D5 | GetEventProperties 返回属性值不加引号的非法 XML | TL-IPC652P-A4 |
| D6 | `Address` 直接放响应下不套 `SubscriptionReference` | 客户端兜底注释 |
| D7 | SOAP Fault 走 HTTP 200 | 客户端注释 |
| D8 | 认证 Fault 措辞各家不一 | 客户端关键词表 |
| D9 | topic 命名与通用约定不同（`LineCrossDetector/LineCross`） | TL-IPC652P-A4 |
| D10 | 长轮询返回零条是正常心跳 | 客户端注释 |
| D11 | `IsMotion=false` 这类「事件结束」通知 | 客户端丢弃逻辑 |

### E. 对讲 / 音频
| # | 怪癖 | 来源 |
|---|---|---|
| E1 | capability 列了 G.711 / G.722 / AAC，backchannel SDP 只列 PCMU | 客户端注释 |
| E2 | ONVIF 把 G.722 的 SampleRate 错写成 8000 | 客户端强制改回 |
| E3 | Bitrate 单位不定（64 vs 64000） | 客户端解析 |
| E4 | GetAudioDecoderConfigurationOptions 两种响应形态 | 客户端解析 |
| E5 | SampleRate 形态不定（range / list / scalar / Items） | 客户端解析 |
| E6 | 大华同时返回麦克风轨（recvonly）+ 对讲轨（sendonly） | 客户端注释 |
| E7 | 上条会话槽位未释放时对新 DESCRIBE 回 401；海康球机释放需 ~3-4s | 客户端注释 |
| E8 | 同时发 Basic + Digest 两条 `WWW-Authenticate` | TP-LINK IP-Camera |
| E9 | Digest 多带一个挑战没要求的 `algorithm=MD5` 就 401 | TP-LINK RTSP Server |
| E10 | SDP 只声明 G7221 / G726，固件对 PCMU PT=0 照样能解 | 客户端注释 |
| E11 | SDP 不列 rtpmap，只给静态 PT | 客户端兜底 |
| E12 | 只有 session 级 `a=control` | 客户端兜底 |
| E13 | talkspurt 首包 marker=0 → 冷启动相机丢首个 talkburst | 客户端注释 |
| E14 | 不返回 SETUP 的 `Session` 头 | 客户端校验 |
| E15 | 相机返 200 却忽略 `Require: backchannel`（应返 551） | TL-IPC652P-A4 |
| E16 | GetCompatibleAudioOutputConfigurations / AddAudioOutputConfiguration 不实现 | 客户端静默跳过 |
| E17 | 音频包小于相机 DMA period 时断续（MULTITRANS） | TL-IPC48AW 实测 |
| E18 | 关连接太快切话尾；开通道立即推吞开头 | MULTITRANS 实测 |
| E19 | 推流期 sendall 卡死（相机停止排空） | 客户端超时注释 |

### F. 传输层
| # | 怪癖 | 来源 |
|---|---|---|
| F1 | 固件回畸形 HTTP，客户端把原始字节流塞进异常文本 | 客户端注释 |
| F2 | 自签 HTTPS 证书（VIGI 20443） | 客户端 `verify=False` |
| F3 | 廉价固件遇并发 SOAP 风暴超时甚至 reboot | 客户端 per-camera 探测互斥锁注释 |

## 10. 最小可用清单（按参照客户端实际会打的端点排序）

**P0（不实现就加不进来）**
1. WS-Discovery 3702 多播，2005/04 ns ProbeMatch，含 EPR / Types / Scopes（`name`、`hardware`）/ XAddrs / MetadataVersion
2. `POST /onvif/device_service`：`GetCapabilities(Category=All)`、`GetDeviceInformation`、`GetServices`
3. Media(ver10)：`GetProfiles`、`GetStreamUri(RTP-Unicast / RTSP)`、`GetVideoSources`
4. WS-Security UsernameToken PasswordDigest 校验
5. 真能拉的 RTSP 流（ffprobe 要能读出 codec / width / height / fps / audio_codec）

**P1**
6. PTZ(ver20)：`GetNodes`、`GetNode`、`ContinuousMove`、`Stop`、`GetPresets`、`SetPreset`、`GotoPreset`、`RemovePreset`
7. Events(ver10)：`CreatePullPointSubscription`、`PullMessages`、`Renew`、`Unsubscribe`、`GetEventProperties`（订阅地址可换端口）
8. Media：`GetSnapshotUri` + HTTP JPEG 端点（Digest）

**P2**
9. RTSP backchannel：`OPTIONS` / `DESCRIBE`（Require 头 → SDP 含 `a=sendonly`）/ `SETUP`（`RTP/AVP/TCP;interleaved=0-1`）/ `PLAY` / `TEARDOWN` + 接收 interleaved RTP
10. Media：`GetAudioDecoderConfigurationOptions`、`GetCompatibleAudioOutputConfigurations`、`AddAudioOutputConfiguration`、`GetCompatibleAudioDecoderConfigurations`、`AddAudioDecoderConfiguration`
11. `GetServiceCapabilities`（Device / Media / PTZ / Imaging / Events）
12. Imaging(ver20)：`GetImagingSettings`（只需 `IrCutFilter`）

**P3**
13. `GetDeviceInformation` 三件套可配 + 各厂商至少 probe 端点桩，让厂商识别命中
