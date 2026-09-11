# onvifsim — ONVIF 摄像头模拟器 完整计划

> 状态：计划稿，未开工。独立开源项目，不依赖任何现有代码库。
> 编译环境已备好：conda `onvifsim`（Qt 6.11.2 / CMake 4.4.3 / GCC 15.3）。
> 真实客户端的交互事实与怪癖清单见 [reference-client-facts.md](reference-client-facts.md)，本文引用其编号（A1、E7 …）。

## 1. 目标与非目标

**目标**

- 一个可执行文件，Windows / macOS / Linux 双击即用，运行期零外部依赖（不要 ffmpeg、不要 Python、不要 Java）。
- 模拟任意多台 ONVIF 相机：发现、鉴权、取流、快照、PTZ、图像、事件、对讲，覆盖 Profile S 全部 + Profile T 常用子集。
- 面向 NVR / 监控软件 / 家庭自动化 / 客户端 App 的开发者做测试：既能当「一台标准相机」，也能按钮一按变成「一台有毛病的相机」。每一种毛病都来自真机实证，不是凭空想的。
- 可编程：REST 控制面 + 无界面模式 + 场景文件，能塞进 CI。

**非目标**

- 不做真实视频编码（没有编码器，画面来自内嵌的预编码样片）。
- 不是 ONVIF 一致性测试工具，不替代官方 Device Test Tool。
- 不做门禁 / 访问控制类 Profile（A / C），不做 ONVIF 客户端。

## 2. 项目定位

| 项 | 决定 |
|---|---|
| 名称 | `onvifsim`（仓库、二进制、CMake target 同名） |
| 许可证 | MIT |
| 位置 | `~/projects/onvifsim`，独立 git 仓库 |
| 语言 / 框架 | C++17，Qt 6.2+（Core / Gui / Network / Widgets），CMake ≥ 3.21 |
| 第三方库 | 无。XML、HTTP、RTSP、RTP、Digest、JPEG 全用 Qt 自带或手写 |
| 构建环境 | 三平台统一 conda（`qt6-main` 在 linux-64 / win-64 / osx-arm64 都有），CI 用 micromamba |
| 文档语言 | README 中英双份，界面中英双语 |

Qt 版本门槛定 6.2 而不是 6.11：Ubuntu 22.04 / Debian 12 的发行版 Qt 也能编，方便别人拿源码自己构建。不用 `QHttpServer`（6.4 前是预览模块，发行版常不带），HTTP 自己写。

## 3. 架构

### 3.1 进程与线程

- 单进程，单 Qt 事件循环，所有 socket 异步。RTP 发送用 `Qt::PreciseTimer` 加「按 elapsed 累计补发」，抖动不累积。
- `RtspServer` 与 GUI 之间只通过信号槽通信、不互相持引用，将来路数上去（>16 路 1080p）可以整体 `moveToThread` 而不改接口。
- 无界面模式和 GUI 模式共用同一个 `Simulator` 核心，GUI 只是一个观察者。

### 3.2 对象模型

```
Simulator
├── DiscoveryResponder        WS-Discovery，所有相机共用一个 UDP 3702 socket
├── ControlApi                REST 控制面（127.0.0.1:9000）
├── LogBus                    结构化日志总线 → GUI / stdout / 文件
└── VirtualCamera × N
    ├── CameraModel           身份、profiles、用户表、scopes、能力开关
    ├── Persona               品牌预设（路径风格 / 命名空间习惯 / 私有 API）
    ├── Quirks                故障注入开关集
    ├── HttpServer            该相机的 HTTP 端口：SOAP 各服务 + 快照 + 私有 API
    │   └── SoapDispatcher → Device / Media / Media2 / PTZ / Imaging / Events / Analytics / DeviceIO
    ├── SubscriptionServers   PullPoint 订阅管理器可开在独立端口（D1）
    ├── RtspServer            该相机的 RTSP 端口：主/子/三码流 + 对讲 backchannel
    │   ├── MediaSource       内嵌 H.264 样片循环 + PCMU/PCMA/AAC 音频
    │   └── RtpSender / RtpReceiver
    ├── EventEngine           PullPoint 订阅状态机 + topic 集 + 触发调度
    ├── PtzState              位置积分、预置位、限位
    └── ImagingState          亮度 / IR / 补光灯
```

每台相机有自己的 **绑定地址 + 端口对**，三种网络模式见 §3.3。

### 3.3 网络模式：一个进程怎么装成 N 台相机

| 模式 | 每台相机的地址 | 权限 | 客户端看到的 | 适用 |
|---|---|---|---|---|
| 端口模式（默认） | 同一 IP，HTTP 8000+i / RTSP 8554+i | 无 | 同一 IP 上 N 个 XAddr | 快速起、CI、无管理员权限 |
| 独立 IP 模式 | 宿主网卡上的 IP 别名，各绑 80 / 554 | 加别名需一次管理员权限 | LAN 里 N 台独立 IP 的相机，与真机无异 | 联调 NVR / 客户端的多相机流程 |
| Docker macvlan 模式 | 每容器一台，独立 IP + 独立 MAC | Linux + docker | 连 MAC 都不重复 | Linux CI、要过「按 MAC 去重」的客户端 |

ONVIF 客户端完全按 XAddr 走，端口模式功能上与独立 IP 模式等价，差别只在「像不像 N 台机器」。

**独立 IP 模式的实现要点**

- IP 别名三平台都有原生命令：Linux `ip addr add 192.168.1.201/24 dev eth0`，macOS `ifconfig en0 alias 192.168.1.201 255.255.255.0`，Windows `netsh interface ip add address "Ethernet" 192.168.1.201 255.255.255.0`。程序内置「一键分配 IP 段」：选网卡、填起始地址与台数，经 pkexec / sudo / UAC 提权执行，退出时回收；也可以让用户自己加好后程序只负责绑定。
- 每个 `VirtualCamera` 持有 `bindAddress`，`HttpServer` / `RtspServer` 只在该地址上 listen。
- WS-Discovery：3702 组播**接收** socket 按物理网卡各一个（别名与物理网卡共享组播组，不必每别名一个）；ProbeMatch 是**单播回给探测方**，必须从相机自己的别名地址发出（每相机一个发送用 UDP socket bind 到别名），否则客户端看到的来源 IP 与 XAddrs 不一致；Hello / Bye 是组播，发送 socket 要设 `IP_MULTICAST_IF` 为别名地址。
- 标准端口 80 / 554：Linux 用 `setcap cap_net_bind_service=+ep` 免 root，macOS / Windows 需管理员运行；不想提权时独立 IP 模式也允许配高端口。
- 已知限制：所有别名共用网卡 MAC，ARP 表里 N 个 IP 对应同一个 MAC。按 EPR 或 IP 去重的客户端不受影响；按 MAC 去重的走 macvlan 模式。
- IPv6 可同样加 ULA 别名，二期。

**Docker macvlan 模式**：`packaging/docker/` 给 headless 镜像 + 一份 `compose.macvlan.yml` 示例（`driver: macvlan`，`parent: eth0`，每个服务固定 `ipv4_address` 与 `mac_address`），一台相机一个容器，场景文件通过环境变量或挂载传入。Docker Desktop（macOS / Windows）的 macvlan 到不了物理 LAN，这条只在 Linux 宿主上成立。

### 3.4 请求处理链

HTTP 请求 → 解析（keep-alive、Content-Length）→ HTTP 层鉴权（快照 / 私有 API 用 Basic / Digest）→ SOAP Envelope 解析（`QXmlStreamReader`，命名空间感知，SOAP 1.1 与 1.2 都收）→ WS-Security UsernameToken 校验 → 按 Body 首元素的 namespace + local name 分发 → handler 产出 XML → 回包。每一步都往 `LogBus` 发一条带相机 id、客户端地址、耗时、结果的记录，GUI 可展开看 SOAP 原文。

响应 XML 用参数化模板函数生成，不用 `QDomDocument` 拼树。所有时间戳 UTC，可注入时钟偏移。

### 3.5 仓库目录结构

```
onvifsim/
├── CMakeLists.txt                 顶层：选项（BUILD_GUI / BUILD_TESTS / ENABLE_AUDIO_PLAYBACK）、版本号、子目录
├── CMakePresets.json              conda-linux / conda-win / conda-mac / release 预设，一条命令配置
├── cmake/
│   ├── Version.cmake              从 git describe 生成 version.h
│   ├── Deploy.cmake               windeployqt / macdeployqt / linuxdeploy 封装
│   └── CompilerWarnings.cmake     -Wall -Wextra -Werror（CI 开）
├── src/
│   ├── core/                      纯逻辑，不依赖 GUI，libonvifsim-core（静态库）
│   │   ├── Simulator.{h,cpp}      根对象：相机集合、发现、控制面、日志
│   │   ├── VirtualCamera.{h,cpp}
│   │   ├── CameraModel.{h,cpp}    身份 / profiles / 用户 / scopes（可序列化为 JSON）
│   │   ├── Persona.{h,cpp}        品牌预设表
│   │   ├── Quirks.{h,cpp}         故障注入开关集（每项：key / 分组 / 参数 / 描述 / 出处编号）
│   │   ├── LogBus.{h,cpp}
│   │   └── Scenario.{h,cpp}       场景文件读写与校验
│   ├── net/                       传输层
│   │   ├── HttpServer.{h,cpp}     HTTP/1.1 解析、keep-alive、路由
│   │   ├── HttpAuth.{h,cpp}       Basic / Digest（含严格参数模式 E9、双挑战 E8）
│   │   ├── HttpResponse.{h,cpp}   含延迟 / 慢发送 / 挂起（长轮询）/ 畸形响应能力
│   │   ├── NetUtil.{h,cpp}        网卡枚举、按目标选源 IP
│   │   └── IpAlias.{h,cpp}        三平台 IP 别名增删（提权执行）与探测
│   ├── soap/                      SOAP 通用层
│   │   ├── Envelope.{h,cpp}       解析（SOAP 1.1 / 1.2，命名空间感知）
│   │   ├── WsSecurity.{h,cpp}     UsernameToken 校验、nonce 缓存、时间窗
│   │   ├── Fault.{h,cpp}          子码 + 措辞变体 + HTTP 状态策略（D7 / D8）
│   │   ├── XmlWriter.{h,cpp}      响应生成辅助（前缀表、常用片段、可选「属性不加引号」畸形模式 D5）
│   │   ├── Dispatcher.{h,cpp}     按 namespace + localName 分发到服务
│   │   └── Namespaces.h           所有 ONVIF / WS-* 命名空间常量
│   ├── services/                  每个 ONVIF 服务一个目录，每个操作一个 handler
│   │   ├── device/  media/  media2/  ptz/  imaging/  events/  analytics/  deviceio/
│   │   └── ServiceBase.{h,cpp}    注册表、能力声明、权限级别
│   ├── events/                    PullPoint 状态机、topic 集、订阅、触发调度
│   ├── ptz/                       PtzState 积分模型、预置位（含出厂 300 槽生成器 C6）
│   ├── imaging/                   ImagingState
│   ├── rtsp/
│   │   ├── RtspServer.{h,cpp}     会话、方法分发、鉴权、超时、并发上限
│   │   ├── RtspSession.{h,cpp}
│   │   ├── Sdp.{h,cpp}            含单轨 / 双轨 / 无 rtpmap / session 级 control 等变体
│   │   ├── RtpSender.{h,cpp}      UDP / interleaved、RTCP SR
│   │   ├── RtpReceiver.{h,cpp}    backchannel 接收与统计、marker 校验（E13）
│   │   └── Backchannel.{h,cpp}
│   ├── media/
│   │   ├── H264Source.{h,cpp}     Annex B 解析、循环、时间戳、FU-A 打包
│   │   ├── AudioSource.{h,cpp}    PCMU / PCMA 合成、AAC ADTS 循环、RFC 3640 打包
│   │   ├── Snapshot.{h,cpp}       QImage 画图 + JPEG
│   │   └── assets.qrc             内嵌样片
│   ├── discovery/                 WS-Discovery Probe / Resolve / Hello / Bye（2005/04 与 2009/01 双方言）
│   ├── vendor/                    品牌私有 HTTP API 桩（hikvision/ dahua/ reolink/ vigi/ tplink/）
│   ├── control/                   REST 控制面、SSE 日志、/metrics、OpenAPI 静态文档
│   ├── cli/                       参数解析、headless 入口（onvifsim-cli 可单独构建）
│   ├── gui/                       Widgets：MainWindow、各 Tab、日志面板、i18n
│   └── main.cpp                   有 GUI 编译进 GUI，否则走 cli
├── assets/
│   ├── media/                     样片源文件与 scripts/gen-media.sh（ffmpeg 只在这一步用）
│   ├── presets/*.json             品牌预设数据（编译期嵌入，也可运行时覆盖）
│   ├── scenarios/*.json           示例场景
│   └── i18n/*.ts                  Qt Linguist 翻译
├── tests/
│   ├── unit/                      QtTest，每模块一个 tst_*.cpp
│   └── e2e/                       pytest + onvif-zeep + wsdiscovery + ffprobe，pyproject.toml 独立依赖
├── packaging/
│   ├── linux/                     AppImage 配方、desktop 文件、图标
│   ├── windows/                   windeployqt 脚本、可选 Inno Setup
│   ├── macos/                     Info.plist、dmg 脚本、ad-hoc 签名
│   └── docker/                    headless 镜像 Dockerfile + compose.macvlan.yml
├── docs/
│   ├── plan.md                    本计划
│   ├── reference-client-facts.md  参照客户端实测事实与怪癖清单（已脱敏，随项目公开）
│   ├── architecture.md            架构与数据流（随实现更新）
│   ├── quirks.md                  故障注入全表（从 Quirks 表自动生成）
│   ├── control-api.md
│   ├── scenarios.md
│   ├── compat-matrix.md           互操作手测矩阵
│   └── building.md                三平台构建指引（conda 路径 + 发行版路径）
├── .github/workflows/
│   ├── ci.yml                     三平台 build + 单测 + Linux e2e
│   └── release.yml                tag 触发打包上传
├── .clang-format  .clang-tidy  .editorconfig  .gitignore
├── LICENSE（MIT）  README.md  README.zh-CN.md  CHANGELOG.md
```

分层约束：`core` / `net` / `soap` / `services` / `rtsp` / `media` / `events` / `discovery` / `vendor` / `control` 合成一个静态库 `onvifsim-core`，只依赖 QtCore / QtNetwork / QtGui（快照画图）；`gui` 与 `cli` 是两个可执行目标链接它。单元测试也只链接 core。这样 headless 版本可以在没有 Widgets 的容器里编，GUI 永远碰不到协议细节。

## 4. 模块详细设计

### 4.1 HttpServer

- HTTP/1.1，keep-alive，POST / GET / PUT，body 上限 1 MB，不支持 chunked 请求（回 411）。
- Basic 与 Digest（RFC 2617，MD5，`qop=auth`，nonce 有效期、过期返回 `stale=true`）。
  - 可同时发 Basic + Digest 两条 `WWW-Authenticate`，顺序可配（E8）。
  - 严格参数模式：客户端 Authorization 里出现挑战没给的参数（如 `algorithm=MD5`）就 401（E9）。
- 路由：`/onvif/device_service`（固定，客户端硬编码）、`/onvif/media_service`、`/onvif/media2_service`、`/onvif/ptz_service`、`/onvif/imaging_service`、`/onvif/event_service`、`/onvif/analytics_service`、`/onvif/deviceio_service`、`/onvif/snapshot`、订阅管理器路径、品牌私有 API 路径。所有非 device 路径可改（A5 的 `:2020/onvif/service` 风格）。
- 支持慢发送（slowloris）、断连、超大响应、畸形响应（回显请求字节 + 500，C5 / F1）等故障注入钩子。
- 并发上限：超过 N 条并发 SOAP 请求时可选 500 / 断连 / 模拟重启（F3）。

### 4.2 SOAP 与鉴权层

- 信封：SOAP 1.2（`application/soap+xml`）为主，SOAP 1.1（`text/xml`）也收。WS-Addressing `Action` / `To` 可校验可忽略。
- WS-Security UsernameToken：`PasswordText` 与 `PasswordDigest`（Base64(SHA1(nonce + created + password))）。Created 时间窗默认 ±5 分钟可配；nonce 缓存防重放可开关。参照客户端不做时钟补偿，时间窗收紧就能复现「全线 401」（A8）。
- 鉴权失败的表现可切换：规范的 SOAP Fault `ter:NotAuthorized`，或 HTTP 401（带 `WWW-Authenticate: Digest`）。Fault 措辞有变体表（`NotAuthorized` / `Sender not authorized` / `FailedAuthentication`，D8）。Fault 的 HTTP 状态 200 或 500 可选（D7）。
- 用户表：多用户，四级（Administrator / Operator / User / Anonymous），操作按级别门控。`GetSystemDateAndTime` / `GetCapabilities` / `GetServices` / `GetWsdlUrl` 等 PRE_AUTH 操作允许匿名（可注入「连这些都要鉴权」）。
- SOAP Fault 生成器：`ter:*` 子码 + 可读文本，覆盖 NotAuthorized / ActionNotSupported / InvalidArgVal / NoProfile / NoToken / NoPTZ 等。

### 4.3 Device 服务

GetDeviceInformation（字段可部分缺失，A10）、GetSystemDateAndTime / SetSystemDateAndTime、GetCapabilities（Category=All，onvif-zeep 的唯一入口，各服务 XAddr 由此来）、GetServices（含 IncludeCapability，可关掉整个操作 A7，可同时列 Media 与 Media2 且 Media2 在前 A6）、GetServiceCapabilities、GetScopes / SetScopes / AddScopes / RemoveScopes、GetHostname / SetHostname、GetNetworkInterfaces、GetNetworkProtocols、GetDNS、GetNTP、GetDiscoveryMode / SetDiscoveryMode、GetUsers / CreateUsers / DeleteUsers / SetUser、GetEndpointReference、GetWsdlUrl、GetRelayOutputs / SetRelayOutputState、GetSystemLog、SystemReboot（真模拟：发 Bye → 全端口离线 N 秒 → 发 Hello）。

XAddr 的 host 与 port 独立可配（A4 / A5）：可以报 `192.168.1.1`、主机名、非常规端口，让客户端的「接管 host 保留 path」逻辑有东西可测。

### 4.4 Media（ver10）与 Media2（ver20）

- Media 读操作：GetProfiles / GetProfile、GetVideoSources、GetVideoSourceConfigurations、GetVideoEncoderConfigurations / GetVideoEncoderConfiguration / GetVideoEncoderConfigurationOptions、GetAudioSources、GetAudioEncoderConfigurations / Options、GetAudioOutputs、GetAudioOutputConfigurations、GetAudioDecoderConfigurations、GetAudioDecoderConfigurationOptions（两种响应形态 E4、采样率三种形态 E5、码率单位两种 E3、G.722 采样率可错写 8000 E2）、GetCompatibleAudioOutputConfigurations、GetCompatibleAudioDecoderConfigurations、GetStreamUri、GetSnapshotUri、GetOSDs、GetMetadataConfigurations、GetVideoSourceModes、StartMulticastStreaming / StopMulticastStreaming。
- Media 写操作（必须真的改 profile 状态，客户端靠它绑对讲）：CreateProfile / DeleteProfile、SetVideoEncoderConfiguration（改分辨率即切换内嵌样片档位）、SetAudioEncoderConfiguration、AddAudioOutputConfiguration / RemoveAudioOutputConfiguration、AddAudioDecoderConfiguration / RemoveAudioDecoderConfiguration（可整体不实现，E16）。
- Media2：GetProfiles(Type=All)、GetStreamUri(Protocol=RtspUnicast)、GetSnapshotUri、GetVideoEncoderConfigurations、GetAnalyticsConfigurations。Profile T 客户端会先探 Media2，缺失就要正确回 ActionNotSupported。
- Profile 命名两套可切（B1）：`MainStream / SubStream` 风格，或 `Profile_1 / Profile_2` 这种没有主子语义的名字。
- GetStreamUri：`StreamSetup` 只认 RTP-Unicast + RTSP 就够；返回 URI 形态可切换：标准 `Uri`、嵌套 `MediaUri/Uri`（B3）、自带 `user:pass@`（B2）、占位 IP `192.168.1.99`、`0.0.0.0`、端口错。
- 默认三个 profile：main 1080p / sub 720p / third 360p，每个可独立设编码（H.264 / H.265 声明）、帧率、码率、音频编码。PTZConfiguration 挂在哪个 profile 上可选：全部 / 只主 / 只子 / 一个都不挂但 PTZ 照常可用（C1 / C2）。

### 4.5 PTZ

GetNodes / GetNode、GetConfigurations / GetConfiguration / GetConfigurationOptions、ContinuousMove / RelativeMove / AbsoluteMove / Stop、GetStatus、GetPresets / SetPreset / GotoPreset / RemovePreset、GotoHomePosition / SetHomePosition、SendAuxiliaryCommand。

- 状态机：pan / tilt ∈ [-1, 1]、zoom ∈ [0, 1]，连续移动按速度积分，到限位停，`GetStatus` 返回随时间变化的真实位置。GUI 用十字准星显示。ContinuousMove 里 PanTilt 与 Zoom 同时出现或只出现一个都要接。
- 能力声明四档（C3 / C4）：`SupportedPTZSpaces` 完整 / 为空 / 只有 `PTZConfiguration.Default*Space` / 只有 pan；Range 可设 `Min == Max`。
- 预置位：正常若干条；出厂预填 300 槽（`预置点 1..300` + 巡航扫描 / 远程重启功能槽，共享同一个假 PTZPosition，C6）；名字百分号编码原样吐回（C7）；GetPresets 不实现回 `ActionNotSupported` 或 `not implemented` 文本（C8）；SetPreset 返回裸字符串或对象两种形态（C9）。
- 非零 Zoom 触发畸形 HTTP（C5）；SOAP 响应延迟抖动 100~800 ms 用来验证客户端的命令乱序防护（C10）。

### 4.6 Imaging

GetImagingSettings（客户端只读 `IrCutFilter`）/ SetImagingSettings、GetOptions、GetMoveOptions、Move / Stop / GetStatus、GetServiceCapabilities。VideoSourceToken 来自 Media 的 GetVideoSources。亮度 / 对比度 / IR cut filter / 白光灯与品牌私有接口联动（Reolink 的 GetWhiteLed、海康的 supplementLight / ircutFilter 改的是同一份状态）。

### 4.7 Events

- GetEventProperties：TopicSet 按品牌预设不同；四档表现（D4 / D5）：完整 / 空 / 不实现 / 返回「属性值不加引号」的非法 XML（`wstop:topic=true`，TP-Link 真机）。
- GetServiceCapabilities（WSPullPointSupport / MaxPullPoints / MaxNotificationProducers）。
- CreatePullPointSubscription：解析 Filter TopicExpression（ConcreteSet 方言，支持 `//.` 与 `|`）与 InitialTerminationTime。返回的订阅地址是后续 Pull / Renew / Unsubscribe 的 HTTP 目标兼 `wsa:To`：可与主服务同端口同 host，也可开独立端口且每次订阅递增（`:1024/event-1024_1024` → `:1025/…`，D1），host 可报不可达的内网地址（D2），`Address` 可直接放响应下不套 `SubscriptionReference`（D6）。订阅槽位上限 N，超限回 Fault 或踢掉最老的（D3、A9：onvif-zeep 每次建连都建订阅且永不退订，槽位泄漏是真实高发故障）。
- PullMessages：真长轮询，无消息时把 HTTP 响应挂起到 Timeout 再回空（用 `QTimer`，不阻塞事件循环）；MessageLimit 生效；空拉取是正常心跳。可注入「永远返回空」（D10）。
- Renew（`wsnt` 命名空间，`TerminationTime` PT1H）/ Unsubscribe / SetSynchronizationPoint（回发所有属性型 topic 当前状态，`PropertyOperation=Initialized`）。
- Subscribe（Basic Notification，向 ConsumerReference 主动 POST Notify）— 二期。
- 属性型事件成对发 `IsMotion=true` / `false`（D11），可切成只发 true。

Topic 集与命名风格（D9）三套：标准 ONVIF（`tns1:RuleEngine/CellMotionDetector/Motion`、`tns1:VideoSource/MotionAlarm`、`tns1:RuleEngine/LineDetector/Crossed`、`tns1:RuleEngine/FieldDetector/ObjectsInside`、`tns1:RuleEngine/TamperDetector/Tamper`、`tns1:VideoSource/GlobalSceneChange`、`tns1:AudioAnalytics/Audio/DetectedSound`、`tns1:VideoSource/ImageTooDark`、`tns1:Device/Trigger/DigitalInput`、`tns1:Device/Trigger/Relay`、`tns1:Monitoring/ProcessorUsage`）；TP-Link 风格（`LineCrossDetector/LineCross`、`IntrusionDetector/Intrusion`）；Reolink / 自定义 `MyRuleDetector/{PeopleDetect,VehicleDetect,DogCatDetect,FaceDetect}`；Axis 风格带 `tnsaxis:` 前缀。Source / Data 的 SimpleItem 命名按预设（`VideoSourceConfigurationToken` / `VideoAnalyticsConfigurationToken` / `Rule` / `State` / `IsMotion`），人车分类放 Data 的 `Type=Human|Vehicle`。

触发方式：GUI 按钮（含持续时长）、REST、定时随机（可设频率）、脚本计划（每 30s 触发 5s）、事件风暴（每秒 N 条）。

### 4.8 Analytics / DeviceIO

Analytics：GetSupportedRules、GetRules、GetAnalyticsModules，最小实现（Frigate / HA 会拉规则列表）。DeviceIO：GetAudioOutputs、GetRelayOutputs、GetDigitalInputs。

### 4.9 RTSP / RTP

- RTSP 1.0：OPTIONS、DESCRIBE、SETUP、PLAY、PAUSE、GET_PARAMETER（keepalive）、SET_PARAMETER、TEARDOWN。会话超时默认 60s 可配，不 keepalive 就回收。鉴权 Basic / Digest，与 HTTP 共用用户表和挑战变体（E8 / E9）。
- 传输：RTP/AVP UDP 单播 与 RTP/AVP/TCP interleaved 都做。
- SDP：video H264 pt96（`sprop-parameter-sets` / `profile-level-id`）；audio PCMU pt0 / PCMA pt8 / G722 pt9（RTP 时钟按 8 kHz）/ AAC pt97 `mpeg4-generic`（RFC 3640）。可注入：不写 rtpmap 只给静态 PT（E11）、只有 session 级 `a=control`（E12）、声明 G7221 / G726 这类不规范 codec（E10）。
- RTP：H.264 RFC 6184 单 NAL + FU-A，90 kHz 时间戳，Marker，SSRC；RTCP SR 每 5s，收 RR 做统计。
- 对讲 backchannel（ONVIF Streaming Spec 的做法，用 PLAY 不是 RECORD）：
  1. DESCRIBE 带 `Require: www.onvif.org/ver20/backchannel` 时 SDP 多一条 `a=sendonly` 音频轨（客户端→相机）；不带该头时不出现。可注入：忽略该头照样返回一样的 SDP（E15），或严格按 RFC 2326 对不支持的 Require 回 551。
  2. 轨道布局可选单轨（海康）或双轨「麦克风 recvonly 在前 + 对讲 sendonly 在后」（大华，E6）。
  3. SETUP 对讲轨用 `RTP/AVP/TCP;unicast;interleaved=0-1`，响应必须带 `Session`（可注入不带，E14）。
  4. PLAY 带同样的 Require 头，之后客户端在 interleaved channel 0 推 RTP。
  5. 接收侧：解 RTP 头，按声明 codec 解 PCMU / PCMA / G722 算电平与丢包；校验 talkspurt 首包 marker（E13，缺 marker 可选静音丢弃）；统计写 REST。
  6. 忙槽位：TEARDOWN 后 N 秒内对新 DESCRIBE 回 401（E7，海康球机实测 3~4s）；推流期可注入「停止排空」让客户端 sendall 卡住（E19）。
  7. 二期：Qt Multimedia 本机回放（可选编译项，不做硬依赖）、保存 wav。
- 并发会话上限 N，超过回 453（廉价相机常态）；每 N 分钟主动 TEARDOWN；丢包率；时间戳跳变；SPS/PPS 只在 SDP 不带内或带内重复；帧冻结；黑屏；帧率突变；不发 RTCP。
- 每 profile 独立 stream path，路径风格随品牌预设。
- 二期：多播、RTSP over HTTP、RTSPS。

### 4.10 媒体资产

- 内嵌 H.264 Annex B 样片三档，`testsrc2` 带时间码，Baseline，GOP 1s，15 fps，10s 循环：360p 744 KB、720p 1.5 MB、1080p 3.2 MB（已实测生成），合计约 5.5 MB。25 fps 版体积约 +60%，待拍板。
- 循环时时间戳连续递增，不回绕。
- 音频：PCMU / PCMA 运行时合成（正弦 / 静音 / 扫频），AAC 内嵌 10s ADTS（84 KB）循环。「声明 AAC 实际发 PCMU」这类谎标（E1）就是声明与实发两处分别可配。
- 外部样片：一期支持用户指定 Annex B `.h264` 文件替换画面；二期做最小 mp4 demuxer（avcC → Annex B）直接吃 `.mp4`，以及 H.265（RFC 7798）。
- 特殊样片：全黑、冻结帧、噪点，用于测客户端的黑屏 / 冻结检测。

### 4.11 快照

`GET /onvif/snapshot?token=...`。运行时用 `QImage` 画一张与样片同风格、叠加当前时间与相机名的 JPEG（Qt Gui 自带 JPEG 编码），每次内容不同，正好测「客户端快照有没有刷新」。

鉴权：无 / Basic / Digest / 一次性 token 四种（B5，参照客户端按 Digest → Basic → 无 的顺序试且只在 401 时才换）。可注入：GetSnapshotUri 不实现（B7）、URI 定期轮换失效（B6）、`200 + image/jpeg + 空体`（B4）、`200 + image/jpeg + 20 字节错误文本`、返回 HTML 登录页、Content-Type 错、慢、404。

### 4.12 WS-Discovery

- 监听 `239.255.255.250:3702`（IPv4，每个网卡都 join，Windows 上必须逐接口加入）。
- 方言：参照客户端用的是 WS-Discovery 1.0（`schemas.xmlsoap.org/ws/2005/04/discovery` + `2004/08/addressing`），ONVIF 常见另一套是 2009/01。默认「照抄 Probe 的命名空间回复」，可强制 2005 / 2009（A1）。
- Probe 不一定带 `Types`（参照客户端的 Probe 就没有），不能以 `dn:NetworkVideoTransmitter` 为应答前提。同一 Probe 会重发 4 次（MessageID 相同），每份都回或去重回一次可选。
- ProbeMatch：EndpointReference `urn:uuid:…`（单次运行内固定，客户端按它去重）、Types、Scopes（`onvif://www.onvif.org/type/video_encoder`、`/type/NetworkVideoTransmitter`、`/Profile/Streaming`、`/Profile/T`、`/name/…`、`/hardware/…`、`/location/…`，空格 `%20` 编码，顺序可调）、XAddrs（可多条，可故意把不可达 IP 放第一位）、MetadataVersion（缺失会让某些客户端整包丢，A3）。
- Resolve / ResolveMatch：ProbeMatch 不带 XAddrs 时客户端会补发 Resolve（A2）。
- 启动发 Hello、退出发 Bye。
- 可注入：不回复、延迟、回复两次、XAddrs 错 IP / 0.0.0.0 / 主机名、Scopes 缺 name、缺 MetadataVersion、缺 XAddrs。

### 4.13 品牌预设（Persona）

Generic（标准 ONVIF）/ Hikvision（含 Annke、LaView 品牌字串）/ Dahua（含 Amcrest、Lorex、Imou）/ Reolink / TP-Link VIGI / TP-Link TL-IPC / Axis / Uniview。每个预设决定：

- `GetDeviceInformation` 的 Manufacturer / Model / FirmwareVersion / SerialNumber / HardwareId 三件套可逐字改，客户端按这些字串选厂商适配器。
- RTSP 路径风格：`/Streaming/Channels/101`、`/cam/realmonitor?channel=1&subtype=0`、`/h264Preview_01_main`、`/stream1`、`/axis-media/media.amp`。
- 快照路径、鉴权表现、TopicSet 与 SimpleItem 命名、Media2 有无、订阅地址风格、对讲轨道布局。
- 私有 HTTP API 桩（按参照客户端实际会调的端点收敛，返回静态 XML / JSON，能联动事件与灯光状态）：
  - 海康 ISAPI（HTTP Digest）：`/ISAPI/System/deviceInfo`、`/ISAPI/PTZCtrl/channels/{ch}/capabilities|presets|presets/{n}/goto|continuous`（PUT，`<PTZData>` pan/tilt -100..100）、`/ISAPI/Streaming/channels/{ch}01`、`/ISAPI/Streaming/channels/{ch}01/picture`、`/ISAPI/Image/channels/{ch}/supplementLight[/capabilities]`、`/ISAPI/Image/channels/{ch}/ircutFilter`、`/ISAPI/System/IO/outputs[/1/trigger]`、`/ISAPI/Event/notification/alertStream`（multipart 长连接推 `EventNotificationAlert`，eventType / targetType / plate）。
  - 大华 CGI（HTTP Digest）：`/cgi-bin/magicBox.cgi?action=getSystemInfo`、`/cgi-bin/ptz.cgi?action=getCurrentProtocolCaps|getPresets|start|stop`、`/cgi-bin/configManager.cgi?action=getConfig&name=Encode|Lighting_V2` 与 setConfig、`/cgi-bin/coaxialControlIO.cgi`、`/cgi-bin/snapshot.cgi`、`/cgi-bin/eventManager.cgi?action=attach&codes=[…]&heartbeat=5`（长连接，每 5s 一行 Heartbeat，事件 `Code=VideoMotion|CrossLineDetection|CrossRegionDetection|SmartMotionHuman|SmartMotionVehicle|FaceDetection|TrafficJunction|VideoBlind;action=Start|Stop|Pulse`）。
  - Reolink（JSON-RPC）：`POST /api.cgi?cmd=Login` 发 token，之后 `cmd=GetDevInfo|GetAbility|GetAiState|GetEnc|GetWhiteLed|SetWhiteLed|GetPtzPreset|SetPtzPreset|DelPtzPreset|PtzCtrl|AudioAlarmPlay|Snap`，token 过期用 `code=-6` 表示。
  - TP-Link VIGI：`20443` 端口 HTTPS JSON-RPC（自签证书，F2）`doAuth` 两步 SHA-256 挑战、`subscribeMsg`、8 个检测开关、`getPresetPoint`；ONVIF 仍在 80。
  - TP-Link TL-IPC：`POST /stok=<t>/ds` 私有 JSON（只做灯光）；对讲的 MULTITRANS 私有协议（554 端口、`MULTITRANS rtsp://host/multitrans`、interleaved channel 1、PCMA、2048 字节线路包）列为二期可选。

### 4.14 故障注入（Quirks）

每项全局或按相机独立开关，都有 REST 字段与 GUI 复选框，都有对应 e2e 断言，每项标注来源编号（见 reference-client-facts.md §9）。分组：

- **发现**：A1 命名空间方言 / A2 缺 XAddrs / A3 缺 MetadataVersion / A4 XAddrs 错 IP / 不回复 / 延迟 / 回两次 / Scopes 缺 name。
- **建连与鉴权**：A5 XAddr 非常规端口 / A6 Media2 排前 / A7 无 GetServices / A8 时间窗收紧 / A9 订阅槽位泄漏 / A10 设备信息缺字段 / 只收 PasswordText / nonce 严格一次 / PRE_AUTH 也要鉴权 / 401 vs Fault / D7 Fault 走 200 或 500 / D8 认证 Fault 措辞。
- **Media 与快照**：B1 profile 命名 / B2 URI 带 userinfo / B3 嵌套 Uri / 占位 IP / B4 空体 / B5 快照鉴权四种 / B6 URI 轮换 / B7 无 GetSnapshotUri / 声明与实发编码不一致 / 分辨率声明与实际不符。
- **PTZ**：C1 PTZConfiguration 挂子码流 / C2 不挂但可用 / C3 Spaces 为空 / C4 Min=Max / C5 非零 Zoom 畸形响应 / C6 出厂 300 预置位 / C7 百分号编码名 / C8 无 GetPresets / C9 SetPreset 返回形态 / C10 RTT 抖动 / GotoPreset 慢 / 移动不改 GetStatus。
- **事件**：D1 订阅端口递增 / D2 订阅 host 不可达 / D3 槽位上限与互踢 / D4 无 GetEventProperties / D5 非法 XML / D6 扁平 Address / D9 topic 命名风格 / D10 永远空拉 / D11 成对状态 / Renew 失败 / 订阅立即过期 / 事件风暴 / 不支持 SetSynchronizationPoint。
- **RTSP 与对讲**：E1 能力谎标 / E2 G.722 采样率错写 / E3 码率单位 / E4 E5 响应形态 / E6 双轨 / E7 忙槽位 401 / E8 双挑战 / E9 严格 Digest 参数 / E10 不规范 codec / E11 无 rtpmap / E12 session 级 control / E13 marker 校验 / E14 无 Session 头 / E15 忽略 Require 或回 551 / E16 无 AudioOutput 配置 / E19 推流卡死 / 并发上限 453 / 周期 TEARDOWN / 丢包 / 时间戳跳变 / SPS-PPS 位置 / 冻结 / 黑屏 / 帧率突变 / 不发 RTCP。
- **传输与设备**：F1 畸形 HTTP / F2 自签 TLS / F3 并发过载重启 / SystemReboot 真离线 / 随机掉线 / 响应超大 / slowloris / 整机延迟 / RTP 限速。

### 4.15 控制面（REST）

默认 `127.0.0.1:9000`，可选 token。

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | `/api/cameras` | 列表与状态 |
| POST | `/api/cameras` | 从预设创建 |
| PATCH / DELETE | `/api/cameras/{id}` | 改属性、quirks、上下线 |
| POST | `/api/cameras/{id}/events` | 触发 topic（state、duration） |
| POST | `/api/cameras/{id}/ptz` | 移动 / 预置位 |
| GET | `/api/cameras/{id}/sessions` | RTSP 会话、订阅、HTTP 客户端 |
| GET | `/api/cameras/{id}/talkback` | 对讲接收统计 |
| POST | `/api/cameras/{id}/offline?seconds=` | 离线模拟 |
| POST / GET | `/api/scenario` | 加载 / 导出场景 |
| GET / POST | `/api/network` | 网卡列表、网络模式、分配 / 回收 IP 别名 |
| GET | `/api/quirks` | 全部开关的元数据（名称、分组、参数、出处） |
| GET | `/api/log` | SSE 日志流 |
| GET | `/metrics` | Prometheus 文本，便于压测 |

附 OpenAPI JSON。

### 4.16 命令行

```
onvifsim                                   # GUI，首启自动建 1 台 Generic 相机并开始
onvifsim --headless --scenario lab.json    # 无界面
onvifsim --headless --cameras 8 --preset hikvision --bind 0.0.0.0
onvifsim --headless --cameras 4 --ip-range 192.168.1.201 --iface eth0   # 独立 IP 模式，别名需已存在或有权限
onvifsim --list-presets | --list-quirks | --version
```

### 4.17 GUI（Widgets）

- 工具栏：添加相机（选预设）/ 全部启动停止 / 加载保存场景 / 网卡选择 / 网络模式（端口 / 独立 IP，含一键分配 IP 段）/ 设置。
- 左栏：相机列表，图标 + 状态（在线、流数、订阅数、最近事件）。
- 右栏 Tab：
  - 概览：XAddr、各 profile RTSP URL、用户名密码、一键复制。
  - 身份：品牌预设、厂商 / 型号 / 硬件 id 三件套、序列号、scopes、用户表。
  - 流：profiles 表（分辨率 / 编码 / 帧率 / 码率 / 音频 / PTZConfiguration 有无），选样片或外部文件。
  - PTZ：位置十字、速度、预置位列表（含一键生成出厂 300 槽）。
  - 事件：topic 列表、触发按钮与时长、自动触发计划、当前订阅与队列长度。
  - 对讲：电平表、接收统计、marker / 丢包计数、保存 wav。
  - 故障注入：按 §4.14 分组的复选框 + 参数，每项 hover 显示出处说明。
  - 连接：RTSP 会话、HTTP 客户端 IP 与 UA。
- 底部：日志面板，按相机 / 级别过滤，SOAP 原文可折叠，导出。
- 中英双语跟随系统，深浅色跟随系统，系统托盘可选。

### 4.18 场景文件

JSON，含全部相机与 quirks。随包附示例：单相机、8 路混品牌、恶劣网络、对讲测试（海康单轨 / 大华双轨 / 忙槽位）、事件风暴、老固件（只有 GetCapabilities）、TP-Link 全家桶（Spaces 为空 + 非法 XML + 订阅端口递增 + 百分号预置位名）。

## 5. 测试策略

- **单元（QtTest）**：SOAP 解析、UsernameToken 摘要、HTTP Digest（含严格参数模式）、SDP 生成各变体、H.264 NAL 切分与 FU-A 打包、RTSP 解析、Topic 过滤匹配、WS-Discovery 两种方言报文、PTZ 积分、出厂预置位生成器。
- **端到端（`tests/e2e/`，pytest + onvif-zeep 0.2.12 + wsdiscovery 2.1.2 + ffprobe）**：通过 REST 起 headless 进程。用参照客户端同款库，等于直接验证「最挑剔的客户端」能否用。覆盖发现（含 4 次重发去重）、建连（GetCapabilities 路径）、GetProfiles / GetStreamUri、ffprobe 拉流出 codec / 分辨率 / fps / 音频、快照三种鉴权、PTZ 四档能力声明、PullPoint 全流程含订阅换端口、对讲 DESCRIBE→SETUP→PLAY→推 RTP 后 REST 查到字节数与 marker 计数；每个 quirk 一条断言。onvif-zeep 按官方 WSDL 严格解析响应，字段错就抛异常，正好当 schema 校验器用。
- **互操作手测矩阵**（记入 `docs/compat-matrix.md`）：ONVIF Device Manager、VLC / ffplay、Home Assistant ONVIF 集成、Frigate、Synology Surveillance Station、iSpy / Agent DVR、Blue Iris、tinyCam、go2rtc、云瞰。
- **CI**：GitHub Actions 三平台构建 + 单测，Linux 跑 e2e，tag 触发 Release 产物。

## 6. 打包发布

| 平台 | 产物 |
|---|---|
| Linux | AppImage + tar.gz |
| Windows | windeployqt 便携 zip（免安装） |
| macOS | macdeployqt dmg，ad-hoc 签名（无开发者账号也能跑） |
| 容器 | headless Docker 镜像，给 CI 用 |

语义化版本，GitHub Release 自动附产物。文档：README（中 / 英）、`docs/quirks.md`（从代码里的 Quirks 表自动生成）、`docs/control-api.md`、`docs/scenarios.md`、`docs/compat-matrix.md`、`docs/building.md`。

## 7. 里程碑

| 里程碑 | 内容 | 验收 | 估时 |
|---|---|---|---|
| M0 骨架 | CMake、HttpServer、SOAP 分发、UsernameToken、Device 服务（GetCapabilities + GetServices）、WS-Discovery 双方言、headless、日志 | ONVIF Device Manager 与 onvif-zeep 能发现并读设备信息；wsdiscovery 2.1.2 一次搜索只出现一条 | 1 天 |
| M1 取流 | Media / Media2、RTSP UDP+TCP、内嵌样片三档、PCMU / PCMA / AAC、快照 | ffplay / VLC 出画面有声音；onvif-zeep GetStreamUri 后 ffprobe 读出 codec / 分辨率 / fps / 音频 | 2 天 |
| M2 PTZ + Imaging | 状态机、预置位、四档能力声明、图像参数 | ODM 摇杆能动，GetStatus 随时间变化；四档声明各有 e2e | 1 天 |
| M3 事件 | PullPoint 全流程含订阅换端口、三套 topic 命名、触发面 | onvif-zeep 收到 Motion；HA 出现运动传感器实体；槽位上限 e2e | 1.5 天 |
| M4 对讲 | backchannel SDP 单轨 / 双轨、SETUP interleaved、PLAY、收 RTP、电平、忙槽位 | ODM / 云瞰对讲后 REST 统计非零且 marker 计数正确 | 1.5 天 |
| M5 预设 + 故障注入 | 八个预设、私有 API 桩、§4.14 全部 quirks + e2e | 每个 quirk 一条通过的 e2e；`--list-quirks` 与 docs/quirks.md 同源 | 2.5 天 |
| M6 GUI | Widgets 全功能、场景、i18n | 手测矩阵至少 ODM / VLC / HA 三家通过 | 2 天 |
| M7 发布 | 三平台产物、CI、文档 | 三平台干净机器下载即跑 | 1 天 |

二期候选：Basic Notification 推送、Profile G（Recording / Replay，RTSP Range 回放）、H.265、mp4 导入、多播、IPv6、RTSPS / HTTPS、预置位多视角样片、Qt Multimedia 对讲回放、TP-Link MULTITRANS 私有对讲。

代码量估算 9k 到 11k 行 C++（含 GUI），不含测试。

## 8. 风险与对策

- **定时器抖动**：`Qt::PreciseTimer` + 按 elapsed 补发多帧；Windows 上 Qt 已处理 1 ms 精度。
- **组播**：Windows 多网卡要逐接口 join；macOS 首次运行有防火墙弹窗；提供网卡选择。独立 IP 模式下 ProbeMatch 源地址必须与 XAddrs 一致，单元测试要盯这一点。
- **IP 别名提权**：三平台提权方式各异（pkexec / osascript / UAC），失败要给出手动命令而不是静默降级到端口模式。
- **同机测试**：客户端与模拟器同机时 XAddr 必须给 LAN IP，按 Probe 来源接口选，不能给 127.0.0.1。
- **标准端口**：80 / 554 要管理员权限，默认高端口，界面提示。
- **手写 XML 易错**：以 onvif-zeep 的严格 schema 解析当校验器，e2e 覆盖每个操作。
- **怪癖只在一个客户端上验过**：reference-client-facts.md 里的事实来自一个客户端的代码与它踩过的真机；别的客户端（HA / Frigate / ODM）的期待可能不同，M6 手测矩阵负责补。
- **性能**：16 路 1080p 约 40 Mbps，单线程够用；再高走 moveToThread。
- **许可**：Qt LGPL 动态链接，样片自生成无版权问题。

## 9. 待拍板

1. 名称 `onvifsim` 与许可证 MIT。
2. 样片帧率：15 fps（5.5 MB）还是 25 fps（约 9 MB）。
3. Profile G 录像回放是否进一期（建议二期）。
4. Basic Notification 是否进一期（建议二期）。
5. 对讲本机回放（Qt Multimedia 可选依赖）要不要。
6. TP-Link MULTITRANS 私有对讲是否做（建议二期，只对一个品牌有意义）。
7. 私有 API 桩是按 §4.13 列的最小集做，还是只做 probe 那一个端点让厂商识别命中。
