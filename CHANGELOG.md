# 更新日志

本文件记录 onvifsim 的所有值得注意的改动。

格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循[语义化版本](https://semver.org/lang/zh-CN/)。

> 版本号由 `git describe --tags` 从 tag 推出（见 `cmake/Version.cmake`），
> 发布时打 `vX.Y.Z` 这样的 tag，`.github/workflows/release.yml` 会自动出三平台产物。

## [未发布]

首个可用版本。从零实现了 `docs/plan.md` 里的 M0–M7 全部里程碑。

### 新增 —— 核心

- **ONVIF 服务**：Device（28 个操作，含 `GetCapabilities(Category=All)` 这个客户端唯一的建连入口）、
  Media(ver10) 33 个操作、Media2(ver20)、PTZ 16 个操作、Imaging、Events（PullPoint 全流程）、
  Analytics、DeviceIO。SOAP 1.1 与 1.2 都收，WS-Security UsernameToken 的
  PasswordText 与 PasswordDigest 都支持。
- **WS-Discovery**：两套命名空间方言（2005/04 与 OASIS 2009/01），默认照抄 Probe 的方言回复；
  逐网卡 join 组播、ProbeMatch 从相机自己的地址单播回、Resolve/ResolveMatch、Hello/Bye。
- **RTSP / RTP**：OPTIONS / DESCRIBE / SETUP / PLAY / PAUSE / GET_PARAMETER / SET_PARAMETER /
  TEARDOWN；UDP 单播与 TCP interleaved 两种传输；H.264 走 RFC 6184（单 NAL + FU-A），
  音频 PCMU / PCMA / G.722 / AAC（RFC 3640）；RTCP SR 每 5 秒。
  **ONVIF 对讲 backchannel 完整 6 步**：带 `Require` 头的 DESCRIBE → 多一条 `a=sendonly` 轨 →
  interleaved SETUP → PLAY → 收 RTP → marker 校验与电平统计。
- **媒体资产**：内嵌三档 H.264 样片（360p/720p/1080p，15fps，共 3.4 MB）加全黑 / 冻结 / 噪点三种特殊样片，
  外加 10 秒 AAC。运行期不依赖 ffmpeg —— 样片由 `assets/media/scripts/gen-media.sh` 在开发期预生成。
  快照用 QImage + QPainter 实时画，每张内容都不同（正好测客户端有没有刷新）。
- **8 个品牌预设**：Generic、海康、大华、Reolink、TP-Link VIGI、TP-Link TL-IPC、Axis、宇视。
  预设决定设备信息三件套、RTSP / 快照路径风格、profile 与 topic 命名、对讲轨道布局、
  默认打开哪些故障注入。
- **厂商私有 API 桩**：海康 ISAPI（含 multipart 长连接 alertStream）、大华 CGI（含 eventManager attach
  与心跳）、Reolink JSON-RPC、TP-Link VIGI（20443 自签 HTTPS + 两步 SHA-256 鉴权）、
  TL-IPC `/stok=` 灯光。全部与 ONVIF 侧共享同一份状态 —— 私有接口改灯光，
  ONVIF 那边读到的也是改后的值。
- **REST 控制面**（默认 `127.0.0.1:9000`）：相机增删改、quirk 开关、事件触发、PTZ、
  会话与订阅查询、对讲统计、离线模拟、场景导入导出、网卡与 IP 别名、SSE 日志流、
  Prometheus `/metrics`、OpenAPI。
- **Qt Widgets 界面**：相机列表 + 8 个 Tab（概览 / 身份 / 流 / PTZ / 事件 / 对讲 / 故障注入 / 连接）、
  可折叠 SOAP 原文的日志面板、中英双语（363 条译文）。
  **故障注入页完全由 `QuirkRegistry` 驱动**，往表里加一条 quirk，界面自动多一项。
- **命令行与场景文件**：`--headless`、`--scenario`、`--cameras`、`--preset`、`--quirk`、
  `--list-quirks/--list-presets/--list-scenarios`；8 个开箱即用的场景。

### 新增 —— 故障注入

- **90 条开关**，分七组（发现 / 建连鉴权 / Media 与快照 / PTZ / 事件 / RTSP 与对讲 / 传输），
  每条带一个出处编号指向 `docs/reference-client-facts.md` 的实测记录。
  开关可全局设、按相机设、运行时用 REST 改；场景文件里的开关**叠加**在品牌预设自带的之上。
- `docs/quirks.md` 由 `--list-quirks --markdown` 从代码里的表生成，不会与实现漂移。

### 设计取舍（`docs/plan.md` §9「待拍板」的实际结论）

1. **名称与许可证** —— `onvifsim` / MIT，按计划。
2. **样片帧率** —— 选 15 fps。三档合计 3.4 MB，比计划里 15 fps 的 5.5 MB 估算还小，
   25 fps 那档就没必要了。
3. **Profile G 录像回放** —— 二期。
4. **Basic Notification 推送** —— 二期。PullPoint 已经覆盖了参照客户端的全部路径。
5. **对讲本机回放** —— 做成可选编译项 `ENABLE_AUDIO_PLAYBACK`（默认关）。
   QtMultimedia 不作为硬依赖；收到的音频可以存 wav，回放要显式打开这个选项。
6. **TP-Link MULTITRANS 私有对讲** —— 二期，只对一个品牌有意义。
7. **私有 API 桩的范围** —— 按计划 §4.13 列的最小集全做了，不是只做 probe 那一个端点。
   理由：厂商识别命中之后客户端会接着调灯光、PTZ、事件流，只做识别端点等于让它在第二步就断。

另外两条实现期才定的：

- **G.722 是近似实现**，不是 bit-exact ITU-T。保留了外部特征（1 字节 = 2 个 16 kHz 采样、
  8 kB/s、160 B/20 ms），内部用 Haar 分带 + 对数量化。电平表、wav 导出、丢包统计都对，
  但不能拿它做编解码互通性测试。抄一整套 ITU 参考表而且抄错了没人发现，不划算。
- **headless 模式用 `QGuiApplication` 配 offscreen 平台插件**，而不是 `QCoreApplication`。
  因为快照是 QImage + QPainter 画的文字，`QFontDatabase` 没有 GUI 应用对象会直接 abort。
  offscreen 插件是 Qt 自带的，不引入外部依赖，容器与 CI 里照常跑。
  `--list-*` / `--version` 这类纯查询仍然走最轻的 `QCoreApplication`。

### 修复

- **界面版一拉 RTSP 流就闪退**（Windows 上最先暴露，Linux 同样中招）。
  对讲页的 `refresh()` 拿「RTSP 连接总数」去和「有对讲轨的会话」下拉框比数量，
  而普通取流的会话根本不会进那个下拉框 —— 于是只要有人在拉流，
  `refresh()` 与 `reloadSessions()` 就无限互相调用，直接爆栈。
  改成按「有对讲轨的会话数」比较，并加了重入保护。
  `tests/e2e/test_gui_stream.py` 专门盯这条：起图形版、反复拉流、确认进程还活着。
- **Windows 打包缺一大批运行期依赖**。windeployqt 只管 Qt 自己的 DLL，
  conda-forge 的 Qt6 却链着 zlib / pcre2 / zstd / double-conversion / brotli /
  freetype / libpng 这些第三方库，还差 MSVC 运行时 —— 结果包在没装 conda 的机器上
  一启动就 `0xC0000135`。`cmake/Deploy.cmake` 现在用
  `file(GET_RUNTIME_DEPENDENCIES)` 递归补齐，并顺手剔掉用不上的
  DirectX 编译器与图片格式插件（69 MB → 37 MB）。
- **Windows 包缺 offscreen 平台插件**，无界面模式一起来就是
  "Could not find the Qt platform plugin offscreen"。
- **`--scenario` 会无条件顶掉场景文件里的 `controlApiPort`**；
  `--http-port` / `--rtsp-port` / `--bind` 显式给出时现在能覆盖整个场景的端口排布
  （场景里的端口撞上本机已有服务时总得有个不改文件的出路）。
- **PowerShell 脚本缺 UTF-8 BOM**：Windows PowerShell 5.1 按系统 ANSI 代码页读 `.ps1`，
  中文注释会把字符串引号配对冲掉，脚本直接语法错误。
- MSVC 的 `C4310` / `C4458` 警告清零（CI 的 `/WX` 会因此失败）。

<!-- 以下来自开源前的一次全量工程规范复核 -->
- SSE 日志流在广播与 `stop()` 时边遍历边删客户端列表（未定义行为，有 SSE 订阅者时
  点「停止」几乎必崩）。两处改为先取快照。
- `ControlApi::start()` 每次都重连 `recordPosted`，而 `stop()` 不断开 ——
  启停 N 次后每条日志往每个订阅者推 N 遍。
- 事件计划的抖动计算整型溢出：间隔超过约 4.3 小时就会溢出成负数，被
  `qMax(100, ms)` 钉成 100 ms，「每 12 小时一次」当场变成「每 100 毫秒一次」。
- quirk C1（`ptz.config_on_sub_only`）在 Media / Media2 / PTZ 三个服务里语义不一致：
  前两者按「只有子码流挂」，PtzService 却按「除主码流外都挂」，于是相机对外宣称
  `Profile_3` 没有 PTZ、却又照单全收 `Profile_3` 的 `ContinuousMove`。三处收敛到
  新增的 `services/ProfileQuirks`。
- `POST /api/scenario` 与从文件加载场景的 quirk 合并语义分叉（替换 vs 叠加）。
  统一走新增的 `Simulator::applyScenario()`；`applyGlobalQuirksToAll()` 与
  `POST /api/cameras` 也改为叠加。
- `VirtualCamera::applyModelChanges()` 名不副实：改端口 / 绑定地址 / 能力开关
  全部静默不生效（接口回 200，用户以为改了）。现在真的重铺服务与路由，
  必要时按新配置重新监听。
- 快照只卡单边尺寸不卡总像素，一次请求能同步分配 177 MB。补总像素上限并等比缩放。
- RTP over TCP（interleaved）没有任何背压，客户端停止读取时写缓冲无限增长。
- quirk E19 关掉之后读缓冲恢复不了，会话挂死占着并发槽位。
- 一批把 0 当成有效值的解析：TP-Link 数字型 `brightness` 会静默关灯并回成功、
  大华亮度 / IO、海康亮度与 PTZ 速度、PTZ 属性 `x`/`y`、`Content-Length` 为负时
  会把缓冲区剩余全当 body。
- 端口静默截断（`--http-port 99999` 变成 33903 照常启动）与
  `offline?seconds=` 溢出导致相机永久离线。
- 一批被吞掉的错误返回值：`HttpServer::writeChunk()` 恒返回 true（违反头文件契约）、
  恢复上线时端口没绑上却照样宣告在线、`Scenario::save()` 与日志导出写失败仍报成功、
  PTZ 的「回 Home / 转到预置位 / 删除预置位」静默无反应、网络对话框里填错绑定地址
  被静默丢弃、对讲录音写失败静默截断。
- 三个非 QObject 的 PIMPL 类（`SoapDispatcher` / `H264Source` / `AudioSource`）
  可拷贝，拷一次就是 double free。补 `Q_DISABLE_COPY`。
- `GetServiceCapabilities` 的鉴权级别在 8 个服务里分成两派，统一为 `PreAuth`。
- 界面的重入保护是 bool 而非计数器，内层调用会把外层的保护提前清掉；
  改为计数器 + RAII guard。
- 托盘菜单的「退出」不保存窗口布局 —— 而托盘默认开启，这是绝大多数用户的正常退出路径。
- 日志面板暂停时缓冲溢出是逐条 `remove(0, 1)`，每来一条日志做一次两万元素的
  memmove，而这个槽在网络处理链上被同步调用（表现为「一暂停日志，拉流就卡」）。
- 控制面此前**一条日志都不产**，`logcat::Control` 是个永远筛出空的复选框；
  同时修掉「上报的状态码与实际发出的不一致」。
- E10（SDP 声明不规范 codec）时，SDP 里 `a=rtpmap` 写 16000、发送侧却按 8000 递增
  RTP 时间戳，客户端两倍速播放。两处收敛到 `sdp::effectiveAudioClockRate()`。
- 事件计划表在 REST 改过之后界面不同步，用户一按「应用」会把过时的界面值倒灌回引擎。
- E9（严格一次性 nonce）下 `useCount` 在校验口令**之前**自增：一次打错口令就烧掉
  nonce，重试时的失败原因变成「nonce 只允许用一次」，把真正的原因盖住。
- `CreateUsers` 不校验 `GetServiceCapabilities` 对外宣称的 `MaxUsers=16`。
- 畸形的 `TopicExpression` 静默退化成「全收」：客户端自以为订了个很精确的过滤器，
  实际拿到全量事件，两边都发现不了。现在回 `ter:InvalidFilterFault`。
- `Ctrl-C` / `SIGTERM` 会直接杀掉进程，`aboutToQuit` 挂的清理**从来没跑过** ——
  独立 IP 模式加的 IP 别名因此会留在系统网卡上。现在走 Qt 的正常退出路径，
  顺带让「启动时有相机失败」真的反映到退出码上（注释承诺了很久，实现一直是 0）。
- `/metrics` 三处不合规：`onvifsim_camera_online` 没有 HELP/TYPE、同一 family 的样本
  交错输出、相机 id 进 label 值不转义（id 是 REST 完全可控的字符串）。
- 存场景再加载并不等价：`videoSource`（含 Bounds）、`audioSource`、`audioOutput`、
  `audioDecoder`、`metadata`、`sessionTimeoutSec`、`ptzNode` 的名字、`imaging` 的五个
  开关在序列化时被整段丢掉。一台 4K 相机存盘再加载，Bounds 会悄悄退回 1920×1080。
  新增 `tst_model` 逐字段钉住往返等价（做过变异验证：去掉任一字段测试必挂）。
- 界面：没有选中相机时对讲页与身份页残留上一台相机的统计数字（灰着但可读、且是错的）。
- 日志面板的条数上限 setter 与 spinbox 信号互相调用，靠「值相等」截断递归是巧合不是设计。
- `Simulator::addCamera()` 挑端口时只跳过本进程其它相机占的端口，不看系统上
  有没有别的进程在用。结果是 `POST /api/cameras` 会挑中一个别人正在用的端口、
  start() 失败回 409，表现为「加相机随机失败」，而原因在一个毫不相干的进程上。
- `tests/e2e` 的 `find_binary()` 不把显式指定的路径转成绝对路径，而子进程用的是
  临时工作目录 —— `--onvifsim-binary=../../build/...` 必然失败，却被 GUI 回归测试里
  的无差别 `except` 伪装成「缺 offscreen 插件」静默跳过。

### 变更

- **控制面的 `handleRequest` 从 598 行拆成 12 个函数**，出口统一（一处发日志、
  一处报状态码）；`MediaService` / `DeviceService` / `PtzService` 三个 600 行量级的
  构造函数按原有的注释分区拆成 `registerXxxOps()`。
- 新增 `--assets-dir`：`PersonaRegistry::loadOverrides()` 与 `SimulatorConfig::assetsDirectory`
  这条「运行时用外部 JSON 覆盖内置预设」的特性**完整实现了却完全没接线**，现在接上了。
- `/api/cameras/{id}/sessions` 增加发送侧统计（`packetsSent` / `bytesSent` /
  `lossPercentReported` / `jitter`）。整条 RTCP RR 解析链的产出此前无处可去。
- `/metrics` 增加 `onvifsim_sse_clients`、`onvifsim_talkback_bytes_total`、
  `onvifsim_discovery_probes_total`、`onvifsim_discovery_matches_total`。
- 删掉 29 个零调用的公开 API、9 个零接收者的信号（其中 `RtpSender::statsChanged`
  是**每个 RTP 包**发一次，8 路 1080p 下每秒上万次空转）、以及在生产里从不生效的
  `WsdProbeCache`（`reference-client-facts.md` 里没有任何「真机对重发 Probe 只回一次」
  的实证，而默认关着又没有入口能打开）。
- 去重：ISO 8601 duration 解析器两份、`declaredResolution()` 两份、PTZ 坐标空间 URI
  两套、四份逐字相同的「相机判空」包装、`httpDate()` 两份（格式串还不一样）。

### 修复 —— 国际化

切成英文之后界面只有壳是英文的，内容全是中文。`tr()` 覆盖了界面自己的 330 处
按钮与标签，但从 core 流出来的字串一条都没进翻译系统：

- **相机预设列表**（7 条）：`Persona::displayName` 是数据，直接塞进菜单。
- **故障注入页整页**（247 条）：quirk 的标题 / 说明 / 参数说明都在 `Quirks.cpp`
  的表里 —— 这是产品的核心页面，英文模式下整页中文。
- **日志面板**（约 70 条）。
- **出错弹窗**（约 40 条，从 core 经 `errorOut` 传到 `QMessageBox`）。

处理方式按类别分开：

- 数据表与错误文案**跟界面语言**。表里的串不经 tr()，所以由 `tools/make-i18n.py`
  从表生成 `src/core/QuirkStrings.cpp`（只含 `QT_TRANSLATE_NOOP`）让 lupdate 扫到，
  界面经新增的 `gui/I18n.h` 查译文；错误文案在 core 里直接走
  `QCoreApplication::translate("onvifsim::core", …)`。
- **日志摘要固定英文**，不跟界面变 —— 日志会落文件、进 stdout、走 SSE，
  跟着 locale 变的话 issue 里贴上来的日志可能维护者读不懂，文本匹配的自动化也不可靠。
- 新增 `tst_i18n`：表里每一条可显示字串都必须有英文译文，且生成的
  `QuirkStrings.cpp` 必须与表同步。忘了跑生成脚本会直接测试失败，
  而不是等到「英文界面下少了一条译文」才发现。

### 新增 —— 开源准备

- 项目标志：矢量母版 `assets/logo/onvifsim.svg`（外加 32px 以下用的简化版），
  由 `tools/make-icons.sh` 生成三平台图标 —— 窗口/托盘走 qrc 位图、Windows 走
  exe 内嵌 ICON 资源、macOS 走 `.icns`。`tst_gui_icons` 盯住第一条。
- 界面截图：`assets/screenshots/overview-en.png` 与 `overview-zh.png`，
  两份 README 各用各的语言那张。
- `SECURITY.md`（先写清**什么不算漏洞**：默认口令、公开的测试私钥、故意犯病的
  quirk，否则这三样必然被反复当成漏洞报）、`CONTRIBUTING.md`（加 quirk 的五步
  硬清单）、issue / PR 模板，其中一个模板专门给「真机怪癖」。
- README 补齐 macOS 下载、Docker 用法、安全说明、文档索引、badge。

### 新增 —— 工程

- 端到端测试（`tests/e2e/`）：pytest + onvif-zeep 0.2.12 + wsdiscovery 2.1.2 + ffprobe，
  用**参照客户端同款的库**驱动一个真的在跑的 headless 进程。
  覆盖发现、建连、Media 取流、快照、PTZ、PullPoint 事件、RTSP 对讲，
  以及每一条故障注入开关的行为断言。
  - **90 条 quirk 全部有断言**，没有挂账项。黑屏与画面冻结用 ffmpeg 的
    `blackdetect` / `freezedetect` 滤镜判定（ffprobe 只报元信息、给不出像素）。
  - `test_quirk_coverage.py` 拿 `GET /api/quirks` 与断言表对账，
    新增 quirk 却忘了写断言会直接失败。
  - RTSP / RTP 客户端是手写的（`helpers/rtsp.py`），不引第三方流媒体库。
- CI（`.github/workflows/ci.yml`）：三平台（Ubuntu / Windows / macOS）用 micromamba
  装 Qt 6 构建 + 单元测试，`-Werror`；Linux 上额外跑 e2e、构建容器镜像、
  校验 `docs/quirks.md` 与代码同源。
  另有一个 Ubuntu 22.04 的 job 用**发行版自带的 Qt 6.2** 构建，
  守住「源码必须能用 Qt 6.2 编过」这条硬性约束。
- Release（`.github/workflows/release.yml`）：打 tag 出三平台产物并挂到 GitHub Release，
  同时推 headless 镜像到 ghcr.io。
- 打包配方：
  - Linux —— AppImage（自带 Qt，下载即跑）与 tar.gz（靠系统 Qt），含 `.desktop` 与图标；
  - Windows —— windeployqt 免安装 zip，外加 **Inno Setup 引导式安装包**
    （`make-installer.ps1` 一条命令出 `setup.exe`，中英双语向导、开始菜单与桌面快捷方式、
    控制面板可卸载；默认装用户目录不要管理员权限）。
    简体中文语言文件随项目带 —— Inno Setup 官方发行版不含它；
  - macOS —— macdeployqt dmg + **ad-hoc 签名**（无开发者账号也能跑），
    `Info.plist` 里带 `NSLocalNetworkUsageDescription`，否则 macOS 14 会静默拦掉多播；
  - Docker —— headless 镜像与 macvlan 编排示例。
- `cmake/Deploy.cmake`：把 windeployqt / macdeployqt / linuxdeploy 封成 CMake 函数，
  既能当模块 `include()`，也能 `cmake -P` 当脚本跑，三平台的打包脚本共用同一份逻辑。
- `scripts/gen-docs.sh`：从 `--list-quirks --markdown` 生成 `docs/quirks.md`。
- 文档：`docs/building.md`、`docs/control-api.md`、`docs/scenarios.md`、
  `docs/architecture.md`、`docs/compat-matrix.md`。

### 安全

- **控制面在没有设访问令牌时不再发送任何 CORS 头。** 原来每个响应都带
  `Access-Control-Allow-Origin: *`，配合「默认无鉴权」，等于用户开着 onvifsim 时
  随便访问一个网页，页面里的 JS 就能删相机、重载场景、调 `/api/network`。
  绑回环地址挡不住这个 —— 浏览器本来就跑在本机。设了 token 之后照常发。
  curl / 脚本 / e2e 不受影响。
- **修复 IP 别名提权路径上的 shell 注入。** 提权时的命令串原来是拿「给人看的」
  命令行文本拼出来丢给 `pkexec sh -c` 的，其中网卡名原样插值，而网卡名一路来自
  REST body / 场景文件 / 命令行。现在逐参数做 POSIX 单引号转义，并在
  `IpAlias::add/remove/addRange` 三个入口校验网卡名。
- 控制面绑到非回环地址却没设令牌时，启动打一条醒目的 WARN。
- TP-Link VIGI 预设的自签证书与私钥改为 **DER 二进制**资源（`assets/tls/`），
  源码里不再出现 PEM 的 armor 行。这把钥匙本来就是公开的测试钥匙，
  但明文 armor 会让各家密钥扫描器误报甚至拒绝推送。

### 已知问题

- 两条 quirk 还没有 e2e 断言（`rtsp.video_freeze`、`rtsp.video_black`），
  它们要解码逐帧比对，ffprobe 给不出这个信息；已登记在
  `tests/e2e/helpers/quirk_cases.py` 的 `UNCOVERED` 里，暂由互操作手测矩阵兜底。
- macvlan 编排只在 Linux 宿主上成立。macOS / Windows 的 Docker Desktop 跑在
  一层轻量虚拟机里，macvlan 接口到不了物理局域网。

<!--
每次发布时把「未发布」里的内容挪到新版本段落下，并在这里开一个空的新段落。
分类固定用这六个：新增 / 变更 / 弃用 / 移除 / 修复 / 安全。
-->
