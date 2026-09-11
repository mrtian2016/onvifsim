# 架构与数据流

> 这份文档讲「东西是怎么摆的、请求是怎么流的」。
> 设计决策的来龙去脉在 `docs/plan.md`，怪癖的实证依据在
> `docs/reference-client-facts.md`。

## 一句话

**单进程、单 Qt 事件循环、全异步 socket**，一个进程装 N 台虚拟相机；
所有协议逻辑在一个只依赖 QtCore / QtNetwork / QtGui 的静态库里，
界面和命令行都只是它的壳。

## 硬性约束（这些是承重墙）

| 约束 | 为什么 |
|---|---|
| **零第三方库** | XML、HTTP、RTSP、RTP、Digest 鉴权、JPEG 全部基于 Qt 手写。运行期不要 ffmpeg、不要 Python、不要 Java —— 这样才能做到「下载即跑」 |
| **Qt 6.2 / CMake 3.21 / C++17** | Ubuntu 22.04 与 Debian 12 自带的 Qt 要能编。具体地：**不用 `QHttpServer`**（6.4 之前是预览模块），HTTP 自己写 |
| **响应 XML 走模板函数** | `soap/XmlWriter` 里的参数化模板，不拼 `QDomDocument` 树 —— 有几条 quirk 要求吐出**故意畸形**的 XML，树形 API 做不到 |
| **ffmpeg 只出现在 `assets/media/scripts/gen-media.sh`** | 开发期预生成内嵌的 H.264 样片，运行期绝不调用 |
| **所有时间戳走 UTC** | 并支持注入时钟偏移（quirk A8 要用） |

---

## 分层

```
              ┌──────────────┐  ┌──────────────┐
              │  gui/ (Widgets) │  cli/         │   两个壳
              └───────┬──────┘  └──────┬───────┘
                      └────────┬───────┘
                    ┌──────────▼──────────┐
                    │   onvifsim-core     │   静态库
                    │  (只依赖 Qt Core /  │
                    │   Network / Gui)    │
                    └─────────────────────┘
   core · net · soap · services · rtsp · media · events · discovery · vendor · control
```

- `core` / `net` / `soap` / `services` / `rtsp` / `media` / `events` / `discovery` /
  `vendor` / `control` 合成一个静态库 **`onvifsim-core`**；
- `gui/`（Widgets）和 `cli/` 是两个链接它的可执行目标；
- **单元测试只链接 core** —— 测协议不需要界面。

### GUI 永远不碰协议细节

界面只通过信号槽观察 `Simulator`，不直接调协议对象。
这条规矩换来的是：**headless 版本能在没有 Widgets 的容器里编出来**
（`-DBUILD_GUI=OFF`），Docker 镜像走的就是这条路。

`QtGui`（不是 Widgets）是 core 的依赖，因为快照要用 `QImage` + `QPainter` 画。
所以无界面模式跑的是 **`QGuiApplication` + `offscreen` 平台插件**，
不是 `QCoreApplication` —— `QFontDatabase` 没有 `QGuiApplication` 会直接 abort。
程序会自动设 `QT_QPA_PLATFORM=offscreen`（用户显式设过就尊重）。
`--version` / `--help` / `--list-*` 这些纯查询例外，走最轻的 `QCoreApplication`。

---

## 对象模型

```
Simulator                        根对象
├── DiscoveryResponder           所有相机共用一个 UDP :3702 socket
├── ControlApi                   REST 控制面，默认 127.0.0.1:9000
├── LogBus                       结构化日志总线 → GUI / stdout / 文件 / SSE
└── VirtualCamera × N
    ├── CameraModel              可序列化的身份 / profiles / 用户 / 能力
    ├── Persona                  品牌预设：路径风格、topic 命名、私有 API
    ├── Quirks                   这台相机的故障注入开关
    ├── HttpServer ──► SoapDispatcher ──► Device / Media / Media2 / PTZ /
    │                                      Imaging / Events / Analytics / DeviceIO
    ├── RtspServer ──► MediaSource、RtpSender、RtpReceiver（对讲）
    ├── EventEngine              topic 集、触发调度、订阅队列
    ├── SubscriptionManager      PullPoint 订阅生命周期
    ├── PtzState                 积分模型 + 预置位
    ├── ImagingState
    └── VendorApiStub            ISAPI / dahua-cgi / reolink-json / VIGI / TP-Link DS
```

三个东西刻意放在 `Simulator` 而不是每台相机上：

- **发现响应器** —— UDP 3702 是多播端口，一个进程只能绑一次。
  一个 socket 代所有相机应答，每台相机各回一份 ProbeMatch。
- **控制面** —— 它不属于任何一台相机。
- **日志总线** —— 跨相机的时间线才有排查价值。

### 状态放哪

`CameraModel` 是**可序列化的配置**（身份、profiles、用户、能力开关），
场景文件与 REST 直接读写它。
`PtzState` / `ImagingState` / `EventEngine` 是**运行期状态机**，不进场景文件。
协议 handler 只读这两类东西，自己不存状态。

---

## 请求处理链

```
TCP 连接
  → HTTP 解析（自己写的，keep-alive、分块、路由）
  → HTTP 层鉴权（Basic / Digest —— 快照与厂商私有 API 用这层）
  → SOAP 信封解析（QXmlStreamReader，命名空间感知，SOAP 1.1 与 1.2 都收）
  → WS-Security UsernameToken 校验（PasswordDigest，nonce 缓存，时间窗）
  → 按 Body 首元素的「namespace + localName」分发
  → handler
  → XmlWriter 生成响应 XML
```

**每一步都往 `LogBus` 发一条**带相机 id、客户端地址、耗时、结果的记录。
GUI 里能展开看 SOAP 原文，REST 的 `/api/log` 把同一份记录推成 SSE。

分发按 `namespace + localName` 而不是 SOAPAction —— 真机里 SOAPAction
经常缺失或写错，而 Body 首元素永远是对的。

### quirk 插在哪

故障注入不是一个集中的拦截器，而是**散布在每一层的判断点**：

| 层 | 例子 |
|---|---|
| 发现 | 换命名空间方言、省掉 XAddrs、延迟应答、回两次 |
| HTTP | 回畸形响应、慢发送、超大响应体、整机延迟 |
| 鉴权 | 收紧时间窗、只收 PasswordText、nonce 一次性、401 还是 Fault |
| XML 生成 | 属性不加引号、Address 不套 SubscriptionReference、裸字符串返回值 |
| 业务 | 省字段、改端口、Spaces 置空、Range 的 Min = Max |
| RTSP / RTP | SDP 变体、忙槽位 401、丢包、时间戳跳变、限速 |

每条 quirk 都有一个编号（`A1` / `E7` / `D5` …）指向
`reference-client-facts.md` §9 记的实证。**不凭空造 quirk。**

新增一条必须**同时**补齐四件事：`Quirks` 表条目、REST 字段、GUI 复选框、
**一条 e2e 断言**。最后一条由 `tests/e2e/test_quirk_coverage.py` 守着 ——
`GET /api/quirks` 与断言表对不上就直接失败。

---

## 网络模式：一个进程怎么装成 N 台相机

| 模式 | 做法 | 需要权限 | 客户端看到的 |
|---|---|---|---|
| **端口模式**（默认） | 同一个 IP，端口递增（8000/8554、8001/8555…） | 无 | N 台相机在同一个 IP 的不同端口上 |
| **独立 IP 模式** | 在宿主网卡上加 IP 别名，每台绑自己的地址、用标准端口 | 一次提权 | N 台相机各有各的 IP，和真相机没区别 |
| **外部编排** | 地址由外部（Docker macvlan）安排好，程序只负责绑定 | 容器权限 | 同上，而且各有各的 MAC |

### 独立 IP 模式的一个坑

**ProbeMatch 的单播回复必须从相机自己的别名地址发出。**
否则客户端看到的来源 IP 与它从 XAddrs 里拿到的地址对不上，
有些客户端会直接丢弃这条应答。单元测试专门盯这一点。

提权失败时**不静默降级**到端口模式 —— 把手动命令给用户
（`sudo ip addr add …`），让他知道发生了什么。

### Docker macvlan 只在 Linux 宿主上成立

macOS / Windows 上的 Docker Desktop 跑在一层轻量虚拟机里，
macvlan 接口连的是那层 VM 的网络，**到不了物理局域网**。
那两个平台请直接在宿主上跑。详见
[`packaging/docker/README.md`](../packaging/docker/README.md)。

---

## 媒体与实时性

### 视频

内嵌三档 H.264 Annex B 样片（1080p / 720p / 360p），
用 `assets/media/scripts/gen-media.sh` 在**开发期**预生成，编译进资源。
运行期只做三件事：解析 NAL、按 GOP 循环、FU-A 分片打包。

### 定时

RTP 发送用 `Qt::PreciseTimer`，并且**按 elapsed 累计补发** ——
定时器晚到了就一次多发几帧，抖动不累积。
这一条很要紧：帧率飘了 ffprobe 立刻就能看出来，客户端也会觉得画面不稳。

### 对讲（backchannel）

```
客户端                                     相机
  │  OPTIONS                                │
  │─────────────────────────────────────────►│
  │  DESCRIBE + Require: …/backchannel       │
  │─────────────────────────────────────────►│
  │  ◄──── SDP：多一条 a=sendonly 的音频轨 ──│
  │  SETUP  Transport: RTP/AVP/TCP;interleaved=0-1
  │─────────────────────────────────────────►│
  │  ◄──── 200 + Session ────────────────────│
  │  PLAY                                    │
  │─────────────────────────────────────────►│
  │  $\x00<len><RTP>  （交织通道推 PCMU）     │
  │─────────────────────────────────────────►│
```

`RtpReceiver` 统计包数、字节数、丢包、**talkspurt 首包的 marker 位**、
音量电平，全部通过 `GET /api/cameras/{id}/talkback` 暴露出来。
marker 计数不是凑数的 —— quirk E13 复现的正是「首包 marker=0
导致冷启动相机丢掉整个 talkburst」这个真实故障。

### 线程

目前全在主线程。`RtspServer` 与 GUI 之间**只通过信号通信、不互相持引用**，
将来要把它整体 `moveToThread` 不需要改接口。
16 路 1080p 大约 40 Mbps，单线程够用。

---

## 客户端行为：几条被真机逼出来的取舍

这些不是猜的，是 `reference-client-facts.md` 里记着的实测事实。
设计里很多看起来奇怪的地方都是被它们逼出来的：

- **拿 XAddr 只用 `GetCapabilities(Category=All)`，从不用 `GetServices`** ——
  所以 `GetCapabilities` 必须完整、必须能匿名调（除非开了对应 quirk）。
- **对 XAddr 只接管 host:port、保留 path** ——
  所以 quirk A5（报 `:2020/onvif/service`）能被扛过去，但报错 path 就不行。
- **主 / 子码流判定纯看 profile 的 `Name` 字串** ——
  所以 quirk B1 换个命名风格就能让客户端判错。
- **每次建连都建一条 PullPoint 订阅且永不退订** ——
  订阅槽位泄漏是真实高发故障，`event-storm` 场景专门复现它。
- **分辨率 / 编码 / 帧率靠 ffprobe 探流，不信 `VideoEncoderConfiguration`** ——
  所以 e2e 的判据也是探流结果，声明值只用来做「声明与实际是否一致」的对照。

---

## 测试

| 层 | 在哪 | 覆盖什么 |
|---|---|---|
| **单元** | `tests/unit/tst_*.cpp`（QtTest） | SOAP 解析、UsernameToken 摘要、HTTP Digest、SDP 各变体、H.264 NAL 切分与 FU-A 打包、RTSP 解析、Topic 过滤匹配、WS-Discovery 两种方言、PTZ 积分、出厂 300 预置位生成器 |
| **端到端** | `tests/e2e/`（pytest） | 用**参照客户端同款的库**（onvif-zeep 0.2.12、wsdiscovery 2.1.2、ffprobe）驱动一个真的在跑的 headless 进程 |
| **互操作手测** | [`compat-matrix.md`](compat-matrix.md) | ODM、VLC、Home Assistant、Frigate、go2rtc… |

e2e 用 onvif-zeep 有个额外好处：**它按官方 WSDL 严格解析响应，字段错就抛异常**，
等于免费的 schema 校验器。手写 XML 最怕的就是「看着对、客户端解析不了」，
这一层专治这个。

CI 三平台构建 + 单测，Linux 上额外跑 e2e、构建容器镜像、
并校验 `docs/quirks.md` 与代码同源。
另有一个 Ubuntu 22.04 的 job 用**发行版自带的 Qt 6.2** 构建，守住版本下限。

---

## 目录速查

```
src/
  core/       Simulator / VirtualCamera / CameraModel / Persona / Quirks / LogBus / Scenario
  net/        HttpServer / HttpAuth / NetUtil / IpAlias
  soap/       Envelope / WsSecurity / Fault / XmlWriter / Dispatcher / Namespaces
  services/   device media media2 ptz imaging events analytics deviceio
  events/     EventEngine / SubscriptionManager / EventTypes（含四套 topic 命名）
  ptz/        PtzState（积分模型 + 预置位）
  imaging/    ImagingState
  rtsp/       RtspServer / RtspSession / Sdp / RtpSender / RtpReceiver
  media/      H264Source / AudioSource / Snapshot
  discovery/  DiscoveryResponder / WsdMessages（2005/04 与 2009/01 双方言）
  vendor/     品牌私有 HTTP API 桩
  control/    ControlApi（REST + SSE + /metrics + OpenAPI）
  cli/        CliOptions / headless 入口
  gui/        MainWindow 与各 Tab
assets/
  media/      内嵌样片 + gen-media.sh（ffmpeg 只在这里）
  scenarios/  八个示例场景
  i18n/       Qt Linguist 双语
tests/
  unit/       QtTest
  e2e/        pytest（独立 venv）
packaging/    linux / windows / macos / docker
cmake/        Version / CompilerWarnings / Deploy
```
