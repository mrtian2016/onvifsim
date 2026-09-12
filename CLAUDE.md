# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> 本仓库统一用中文交流与写文档（面向外部的 `README.md` 保持英文）。

## 项目状态

**`docs/plan.md` 的 M0–M7 已全部实现并验证通过。** 约 34000 行 C++、90 条故障注入、8 个品牌预设、Qt Widgets 界面，Linux / Windows / macOS 三平台构建与打包均已验证。

仓库已经开源在 `github.com/mrtian2016/onvifsim`。
**提交与推送前先问用户**，别自己 commit。

`docs/plan.md`（中文）仍是权威设计稿：目标与非目标、对象模型、各模块详细设计、里程碑。改动前先读它，按它的结构走；觉得某个决定不对就说出来，别闷头偏离。`docs/reference-client-facts.md` 是它背后的实证依据。`plan.md` §9 那 7 个「待拍板」的实际结论记在 `CHANGELOG.md` 的「设计取舍」一节。

当前验证基线（改完代码至少要保持住）：

| 项 | 状态 |
|---|---|
| 单元测试 | 14 个套件，三平台都 100% |
| 端到端 | 192 条，`tests/e2e/` |
| 故障注入覆盖 | 90/90 条都有 e2e 断言，无挂账 |
| 编译 | `-Werror`（MSVC 用 `/WX`）零警告 |

## 构建环境

参考工具链是已经建好的 conda 环境 `onvifsim` —— 这是**本机开发**用的。
CI 与发版走的是各平台官方的那套（官方 Qt 二进制 + runner 自带编译器），
理由见下面「GitHub Actions」。

```bash
conda activate onvifsim   # Qt 6.11.2 / CMake 4.4.3 / Ninja / GCC 15.3
```

环境里有的 Qt6 模块：Core、Gui、Network、Widgets、Test。**没有 QtMultimedia** —— 可选的本机对讲回放（`ENABLE_AUDIO_PLAYBACK`）在这个环境里装不上就编不了。环境里**没有 Python**；`ffprobe` 用的是系统的 `/usr/bin/ffprobe`。

```bash
cmake --preset conda-linux && cmake --build --preset conda-linux
ctest --preset conda-linux                     # 12 个 QtTest 套件
ctest --preset conda-linux -R tst_soap         # 跑单个测试目标
./build/conda-linux/bin/onvifsim --headless --scenario assets/scenarios/single-camera.json

# e2e：有自己的 venv，不走 conda（conda 环境里没有 Python）
cd tests/e2e && .venv/bin/python -m pytest -q --onvifsim-binary=../../build/conda-linux/bin/onvifsim
```

`tests/e2e/` 有独立的 `pyproject.toml` 与 venv（已 gitignore）。装依赖见 `tests/e2e/README.md`。

**Windows 交叉验证**：本机没有 mingw Qt，交叉编译走不通。实际做法是 ssh 到一台 Windows 机器原生构建（conda 环境 + VS Build Tools），脚本在那台机器的家目录：`build-win.bat` / `test-win.bat` / `pack-win.bat` / `installer.bat`。产物见下面「Windows 产物」。

## 硬性约束

这几条是刻意定下的、承重的，不要擅自放松：

- **零第三方库。** XML、HTTP、RTSP、RTP、Digest 鉴权、JPEG 全部基于 Qt 手写。运行期不要 ffmpeg、不要 Python、不要 Java。
- **源码必须能用 Qt 6.2 / CMake 3.21 / C++17 编过**，尽管 conda 环境里的版本高得多。目的是让发行版自带的 Qt（Ubuntu 22.04、Debian 12）也能构建。具体地：**不要用 `QHttpServer`**（6.4 之前是预览模块），HTTP 自己写。
- **ffmpeg 只允许出现在 `assets/media/scripts/gen-media.sh`**，用来在开发期预先生成内嵌的 H.264 样片。运行期绝不调用。
- **响应 XML 由 `soap/XmlWriter` 里的参数化模板函数生成**，不要拼 `QDomDocument` 树 —— 有几个 quirk 要求吐出故意畸形的 XML。
- **所有时间戳走 UTC**，并支持注入时钟偏移。

## 架构

### 分层

`core` / `net` / `soap` / `services` / `rtsp` / `media` / `events` / `discovery` / `vendor` / `control` 合成一个静态库 **`onvifsim-core`**，只依赖 QtCore / QtNetwork / QtGui（QtGui 用于快照画图）。`gui/`（Widgets）和 `cli/` 是两个链接它的可执行目标；单元测试只链接 core。

**GUI 永远不碰协议细节。** 它只是通过信号槽观察 `Simulator`。这样 headless 版本才能在没有 Widgets 的容器里编出来。

### 对象模型

```
Simulator
├── DiscoveryResponder   所有相机共用一个 UDP :3702 socket
├── ControlApi           REST 控制面，127.0.0.1:9000
├── LogBus               结构化日志总线 → GUI / stdout / 文件
└── VirtualCamera × N
    ├── CameraModel  Persona  Quirks
    ├── HttpServer → SoapDispatcher → Device/Media/Media2/PTZ/Imaging/Events/Analytics/DeviceIO
    ├── RtspServer → MediaSource、RtpSender/RtpReceiver（对讲）
    ├── EventEngine  PtzState  ImagingState
```

单进程、单 Qt 事件循环，所有 socket 异步。RTP 发送用 `Qt::PreciseTimer` 加「按 elapsed 累计补发」，抖动不累积。`RtspServer` 与 GUI 之间只通过信号通信、不互相持引用，将来可以整体 `moveToThread` 而不改接口。

每个 `VirtualCamera` 持有自己的 `bindAddress` 和一对 HTTP/RTSP 端口。三种网络模式：端口模式（默认，同一 IP 多端口）、独立 IP 模式（`net/IpAlias` 按平台提权加别名）、Docker macvlan（只在 Linux 宿主成立）。独立 IP 模式下，**ProbeMatch 的单播回复必须从相机自己的别名地址发出**，否则客户端看到的来源 IP 与它拿到的 XAddrs 对不上。

### 请求处理链

HTTP 解析 → HTTP 层鉴权（Basic/Digest，用于快照和厂商私有 API）→ SOAP 信封解析（`QXmlStreamReader`，命名空间感知，SOAP 1.1 与 1.2 都收）→ WS-Security UsernameToken 校验 → 按 Body 首元素的 `namespace + localName` 分发 → handler → XML 响应。每一步都往 `LogBus` 发一条带相机 id、客户端地址、耗时、结果的记录，GUI 里可展开看 SOAP 原文。

## 故障注入（Quirks）—— 本项目的核心

这个项目的意义不只是「做一台正确的 ONVIF 相机」，而是**按需复现真实固件的毛病**。每个 quirk 可全局或按相机独立开关，并由编号串起来：

- **每个 quirk 都带编号**（`A1`、`E7`、`D5` …），指向 `docs/reference-client-facts.md` §9，那里记着它对应的实际客户端代码或真机行为。分组：**A** 发现/建连、**B** Media/快照、**C** PTZ、**D** 事件、**E** RTSP/对讲、**F** 传输层。
- 新增一个 quirk 必须**同时**补齐：`Quirks` 表条目（key / 分组 / 参数 / 描述 / 出处编号）、REST 字段、GUI 复选框、**一条 e2e 断言**。
- `docs/quirks.md` 和 `--list-quirks` 都从代码里的 `Quirks` 表生成 —— 表是唯一真源，生成出来的文档不要手改。
- **不要凭空造 quirk。** 每一条都要能追溯到 `reference-client-facts.md` 里记录的真机实证。

`docs/reference-client-facts.md` **随项目一起公开**（头部已改写：参照客户端定义成 onvif-zeep + zeep + wsdiscovery 这套公开库组合，全文无公司名、无内网地址、无人名）。往里加内容时守住同一条线 —— 只写能从公开库和真机行为推出来的事实，不要写任何能识别出具体客户端身份的细节。

### 要盯住的客户端行为

很多设计取舍是被参照客户端的真实行为逼出来的，例如：它用 `GetCapabilities(Category=All)` 而**从不**用 `GetServices` 拿 XAddr；每次建连都建一条 PullPoint 订阅且永不退订（订阅槽位泄漏是真实高发故障）；对 XAddr **只接管 host:port、保留 path**；主/子码流判定**纯看 profile 的 `Name` 字串**；分辨率/编码/帧率靠 ffprobe 探流而不信 `VideoEncoderConfiguration`。`reference-client-facts.md` §10 给了 P0→P3 的端点优先级清单，按那个顺序实现。

## 测试

- **单元测试（QtTest，`tests/unit/tst_*.cpp`）**：SOAP 解析、UsernameToken 摘要、HTTP Digest（含严格参数模式）、SDP 各变体、H.264 NAL 切分与 FU-A 打包、RTSP 解析、Topic 过滤匹配、WS-Discovery 两种方言、PTZ 积分、出厂 300 预置位生成器。
- **端到端（`tests/e2e/`，pytest）**：通过 REST 起 headless 进程，用**参照客户端同款库**（onvif-zeep 0.2.12、wsdiscovery 2.1.2、ffprobe）驱动。onvif-zeep 按官方 WSDL 严格解析响应，正好当 schema 校验器用来抓手写 XML 的错。
- **CI**（`.github/workflows/ci.yml`）：三平台 micromamba 构建 + 单测（`-Werror`）；另有一个 ubuntu-22.04 的 job 用**发行版自带的 Qt 6.2** 构建，守住「源码必须能用 6.2 编过」这条硬性约束；Linux 上跑 e2e 并校验 `docs/quirks.md` 与代码同源。
- **GUI 层单独有回归**（`tests/e2e/test_gui_stream.py`）：起图形版反复拉流、确认进程还活着。协议层的 192 条一行界面代码都碰不到，这个盲区出过事故 —— 见下面「已知的坑」。

## 已知的坑

- **moc 不认 raw string 里的 `//`**（Qt 6.11.2 实测）：测试或源码里写 `R"XML(...http://...)XML"` 时，`//` 之后会被 moc 当成行注释，把后面的 `Q_OBJECT` 一起吃掉，症状是链接期报 `undefined reference to vtable for XxxTest`。**含 URL 或 XML 样本的字面量一律用普通转义字符串**，不要用 raw string。
- **`.qrc` 里的注释不能放在 `<RCC>` 元素外面**，rcc 的解析器会报 `Expected '>'`。注释写在 `<RCC>` 内部。
- **Windows PowerShell 5.1 按系统 ANSI 代码页读 `.ps1`**，UTF-8 无 BOM 的中文会把字符串引号配对冲掉，脚本直接语法错误。`packaging/**/*.ps1` 一律存成 **UTF-8 with BOM + CRLF**。
- **Windows 上 GUI 子系统的 exe 在命令行里拿不到控制台**，`--headless` / `--list-quirks` 一个字都看不到。所以 Windows 构建会额外出一个控制台子系统的 `onvifsim-cli.exe`（同一份 `main.cpp`，只是没有 `ONVIFSIM_HAVE_GUI`），命令行用它。
- **windeployqt 只管 Qt 自己的 DLL**。conda-forge 的 Qt6 还链着 zlib / pcre2 / zstd / brotli / freetype 这些第三方库，少了它们包在别的机器上直接 `0xC0000135`，而且错误里不会说是哪个。`cmake/Deploy.cmake` 用 `file(GET_RUNTIME_DEPENDENCIES)` 补齐，并显式拷 `qoffscreen.dll`（windeployqt 认为图形程序用不上它）。
- 静态库里的 Qt 资源不会自动初始化，需要在某个 .cpp 里显式调一次 `Q_INIT_RESOURCE(<qrc 基名>)`（`src/media/H264Source.cpp` 就是这么做的）。注意 `Q_INIT_RESOURCE` 展开出的 extern 声明会被**匿名 namespace 限定住**，导致链接期找不到全局符号 —— 调用它的函数必须待在文件作用域，不能塞进匿名 namespace。
- **macOS 上 Command Line Tools 与 Xcode 各带一套 SDK**，`xcrun` 默认用 CLT 那套。CLT 比 Xcode 新时（实测 macOS 27：CLT 给 `MacOSX27.0.sdk`、Xcode 里最新才 `26.5`），SDK 里的新架构标记 `arm64e.x1` 会让 linker 报 `tapi error: malformed file` —— 连 hello world 都编不过，跟项目无关。显式 `-DCMAKE_OSX_SYSROOT=<Xcode 的 SDK>` 绕过，详见 `docs/building.md`。
- **两个 `.qrc` 不能同名**：`qInitResources_<基名>` 会撞车。`src/core/scenarios.qrc` 与 `src/media/assets.qrc` 就是为此改的名。
- **linuxdeploy-plugin-qt 默认只收 `xcb` 一个平台插件**，而 `--headless` 会把 `QT_QPA_PLATFORM` 设成 `offscreen`（快照要 `QGuiApplication`）。结果是 AppImage 的无界面模式在**任何**机器上都报 `Could not find the Qt platform plugin "offscreen"` —— 偏偏 AppImage 正是最可能被丢进无头服务器 / CI 的那一份。`make-appimage.sh` 里靠 `EXTRA_PLATFORM_PLUGINS` 补 `libqoffscreen.so;libqminimal.so`。
- **linuxdeploy 生成的 `AppRun` 是个裸符号链接，对 Qt 程序不够用。** 它让 Qt 全靠 `usr/bin/qt.conf` 定位插件；可经 AppImage 运行时启动时，传给程序的 `argv[0]` 是 `.AppImage` 文件自己的路径，Qt 据此算出的「程序所在目录」落在用户下载 AppImage 的那个目录上 —— qt.conf 找不到，插件搜索路径是空的，**图形模式和 `--headless` 一起死**（`Could not find the Qt platform plugin "offscreen" in ""`）。`make-appimage.sh` 因此分两步：先填 AppDir，写一个导出 `QT_PLUGIN_PATH=$APPDIR/usr/plugins` 的 AppRun 包装脚本，再 `--output appimage`（linuxdeploy 见到已有 AppRun 会跳过，日志里是 `Existing AppRun detected`）。
  这个坑最毒的地方是**三种「验证」方式全都验不出来**：`--appimage-extract` 后跑解包的二进制、挂载后直接跑 `AppRun`、手工设 `QT_PLUGIN_PATH`，在坏包上统统正常 —— 只有用户实际用的那一种是坏的。首次发版时整整一轮都没看出来。
- **`.deb` 与 `.tar.gz` 都必须用发行版的 Qt 构建**，不能用 conda 的 —— 这两个包都不带 Qt 运行时。deb 的 `Depends` 是 `dpkg-shlibdeps` 从二进制实际链接的 `.so` 反推的；拿 conda Qt 6.11 编出来的程序打包，反推出的依赖是错的，装到只有 Qt 6.4 的系统上直接起不来，而 dpkg 对这种错配毫无察觉。tar.gz 更直接：conda 构建要求 `Qt_6.11` 版本化符号、rpath 还指着构建机的 conda 目录，解到别人机器上就是 `libQt6Core.so.6: version 'Qt_6.11' not found`。两个脚本里都有 `onvifsim_reject_conda_build`（`packaging/common.sh`）。
  首次发版时 tar.gz 正是这么坏的：守卫当时只写在 `make-deb.sh` 里，于是 `release.yml` 顺手把 tar.gz 排进了 conda 那个 job。**同一条约束落在两个产物上，守卫就要放在两个产物都经过的地方。**
- **macOS 的 `.app` 也要补 `libqoffscreen.dylib`。** macdeployqt 只收 cocoa，`.app` 里的 `--headless` 会和 AppImage 报一模一样的错。Windows 那边早就显式拷了 `qoffscreen.dll`、Linux 靠 `EXTRA_PLATFORM_PLUGINS`，**只有 macOS 这条一直漏着**（首次发版时 dmg 中招）。补的位置很讲究：**必须在 macdeployqt 之前拷进 bundle**，否则那份插件的依赖路径不会被改写成 `@executable_path/../Frameworks`，仍指着构建机上 conda 的绝对路径 —— 本机测一切正常，换台机器才坏。`Deploy.cmake` 里 `onvifsim_check_macos_bundle_paths` 用 `otool -L` 兜住这一条。
- **`QSettings` 里存过的值会让改默认值失效**。托盘默认从「关」改成「开」时，老用户的配置里躺着旧的 `false`，表现就是「说好的新行为呢」。改任何默认值都要配一次性迁移（见 `MainWindow` 里的 `ui/trayDefaultV2`），只对新装的机器改默认是不够的。

### 日志级别：协议握手不是错误

**HTTP Digest 与 RTSP 的第一次 401 是协议规定的握手**，不是故障 —— 客户端第一次不可能带凭据（它还不知道 nonce），必须先挨一个 401 拿挑战，紧接着带 nonce 重发就成功了。把这一步记成 `WARN`，日志看上去满屏报错，实际每条后面十几毫秒就跟着 200。真踩过：用户拿着一整屏「快照要求 digest 鉴权」的 WARN 来问，一统计是 401 二十八次、200 也二十八次、零失败。

所以：

- **没带 `Authorization` 的 401 → `Debug`**，文案说「回 401 挑战」，别写「鉴权失败」；
- **带了却过不去**（口令错、方案不对、nonce 重放、E9 严格参数）→ `Warning`，并写清**客户端到底发了什么**（有没有带、带的哪种方案、卡在哪一步）。只说「要求 Digest」对排查毫无帮助。

同理适用于任何「先拒后允」的协商流程。判断标准是**这条记录出现时，系统有没有真的出问题**。

### GUI 层是协议测试的盲区

`tests/unit` 按分层约束只链 `onvifsim-core`，e2e 又全跑在 `--headless` 上 —— **192 条用例一行界面代码都碰不到**。而界面自己有一套每秒刷新的逻辑，读的正是 RTSP 会话、订阅、对讲统计这些会随客户端行为变化的东西。

出过一次事故：对讲页的 `refresh()` 拿「RTSP 连接总数」去和「有对讲轨的会话」下拉框比数量，可普通取流的会话根本不会进那个下拉框，于是只要有人拉流，`refresh()` ↔ `reloadSessions()` 就无限互调直接爆栈 —— 用户看到的是「一读流就闪退」。

所以：

- 界面里任何 `refresh()` ↔ `reloadXxx()` 的互调都要有**重入保护**，比较的两个数必须是同一个口径；
- 改了界面里读会话 / 订阅 / 统计的代码，跑一遍 `tests/e2e/test_gui_stream.py`。

### 打包产物是第二个盲区

单测只链 `onvifsim-core`，e2e 跑的是构建目录里的二进制，CI 验的是「能不能编、
测过没过」。**没有任何一处跑过打好的包。** 首次发版打出来的六个产物里三个起不来，
而六个 job 全绿。唯一完好的 `.deb` 恰恰是唯一在 CI 里装了再跑一遍的那个。

所以四个打包脚本（deb / tar.gz / AppImage / dmg）和 `make-portable.ps1` 在产物
封口之前都要跑一遍产物本身：`--version`，再 `--headless` 起 5 秒看它还活着。
共用实现在 `packaging/common.sh` 的 `onvifsim_smoke_test`。**改打包脚本不要把它
绕过去。**

两个容易把这道守卫写成摆设的地方（都当场踩过）：

- **只跑 `--version` 等于没跑。** 它走不到 `QGuiApplication`，缺平台插件照样正常
  打印版本号 —— 坏掉的 AppImage 就是 `--version` 好好的、一进 `--headless` 直接
  core dump。
- **AppImage 必须去掉 `APPIMAGE_EXTRACT_AND_RUN`**（`make-appimage.sh` 自己为
  linuxdeploy 那几个工具导出过它）。带着它跑，AppImage 会自解压再执行里面的
  二进制，恰好绕开唯一会出问题的那条路径。`onvifsim_smoke_test` 里统一用
  `env -u` 摘掉。

同一个道理也适用于「静默回落」类的缺陷：`.qm` 漏打包不会报错，只会让英文界面
显示中文。`GuiEntry.cpp` 现在在译文一个都没加载上时会 `qWarning` 出来，并把找过
的目录一并打印。

## Linux 产物

三个包，都在 `packaging/linux/`：

| 产物 | 脚本 | 说明 |
|---|---|---|
| `.AppImage` | `make-appimage.sh` | 自带 Qt，下载 `chmod +x` 就跑，不挑发行版 |
| `.deb` | `make-deb.sh` | Debian / Ubuntu 装到 `/usr`，靠**系统 Qt**（见上面的坑） |
| `.tar.gz` | `make-tarball.sh` | 便携目录，靠系统 Qt |

## 图标

母版 `assets/logo/onvifsim.svg`，外加一份 32px 以下用的简化版 `onvifsim-small.svg`（发现环的虚线在小尺寸下糊成噪点）。改了母版跑 `tools/make-icons.sh` 重新生成，**生成物入库** —— 构建期不做矢量渲染，CI 上不用装 rsvg / Inkscape。

渲染用 Qt 自己的 `QSvgRenderer`（`tools/svg2png`，不挂进主构建），不是 rsvg：这样看到的就是程序运行时看到的。SVG 里用了 Qt 不支持的特性（CSS、filter、mask），生成这一步就直接报错，而不是等打完包才发现图标是空的。**所以母版只能用 SVG Tiny 1.2 的那些元素。**

接进程序有三条独立的路径，缺一条就有一处是白纸：

| 位置 | 靠什么 |
|---|---|
| 窗口标题栏、托盘、对话框 | `src/gui/icons.qrc` → `util::appIcon()` |
| Windows 资源管理器里的 exe | `packaging/windows/onvifsim.rc.in` 内嵌的 ICON 资源 |
| macOS Dock / Finder | `Info.plist` 的 `CFBundleIconFile` + `Contents/Resources/onvifsim.icns` |

`tst_gui_icons` 盯着第一条 —— qrc 别名写错、静态库少一次 `Q_INIT_RESOURCE`，`QIcon` 都只会安静地给出空图。

## Windows 产物

两个包，都由 `packaging/windows/` 下的脚本产出：

| 产物 | 脚本 | 说明 |
|---|---|---|
| `onvifsim-<版本>-windows-x64.zip` | `make-portable.ps1` | 免安装，解压即跑 |
| `onvifsim-<版本>-windows-x64-setup.exe` | `make-installer.ps1` | Inno Setup 引导安装，中英双语向导、开始菜单与桌面快捷方式、控制面板可卸载 |

包里**有两个可执行文件**：`onvifsim.exe`（图形子系统，双击不弹黑框）与 `onvifsim-cli.exe`（控制台子系统，命令行用）。原因见上面「已知的坑」。

安装包需要 Inno Setup 6（`winget install --id JRSoftware.InnoSetup -e`），ISCC 可能在 `%LOCALAPPDATA%\Programs\` 或 Program Files，脚本两处都找。**简体中文语言文件随项目带**（`packaging/windows/ChineseSimplified.isl`）—— Inno Setup 官方发行版不含它，写 `compiler:Languages\ChineseSimplified.isl` 在干净机器上必然编译失败。

打包时容易漏的东西（都在 `Deploy.cmake` / `make-portable.ps1` 里处理了，改动别弄丢）：Qt 之外的第三方 DLL、`qoffscreen.dll`、`i18n/*.qm`（漏了英文界面会静默退回中文原文，不报错）。

## 已知限制与二期候选

这些是实现过程中有意识留下的，不是疏漏 —— 动它们之前先想清楚是否值得：

- **`RtspServer::sessions()` 与 `SubscriptionManager::subscriptions()` 返回内部对象的裸指针**，GUI 与 REST 直接读它们的字段。同线程下没问题，但与「路数上去后把 `RtspServer` 整体 `moveToThread`」的计划冲突。真要多线程时需要先加一对值类型快照接口（`QList<RtspSessionInfo>` / `QList<SubscriptionInfo>`）。
- **`RtspServer` 只有 `sessionCountChanged` 信号**，会话内部状态（开始播放、加了 backchannel 轨）没有信号，GUI 的连接页靠每秒轮询。加 `sessionChanged(QByteArray)` 会更省电。（原来还有一对 `sessionStarted/sessionEnded`，但零接收者，已删。）
- **G.722 是近似实现**，不是 bit-exact ITU-T：保留了外部特征（1 字节 = 2 个 16kHz 采样、8 kB/s、160B/20ms），内部用 Haar 分带 + 对数量化，自编自解一致。电平表 / wav 导出 / 丢包统计都对，但不要拿它做互通性测试。（一处不准：G.722 的静音解出来不是零，而是 ±512 的常量抖动。目前 `ToneKind::Silence` 生产不可达，只影响读代码的人。）
- **`AudioSource::levelOf()` 对 AAC 返回 0**（没有 AAC 解码器，也不打算引第三方）。
- **`H264Source` 的帧切分不解 slice header**，多 slice 的外部 `.h264` 会被拆成多帧（表现为帧率翻倍）。内嵌样片是每帧单 slice，不受影响。
- **TP-Link VIGI 私有 API 的方法名与参数形状是自洽推定的**，`reference-client-facts.md` 只记了「两步 SHA-256」「subscribeMsg」「8 个开关」这些骨架。抓到真机报文后改动只在 `VigiStub::dispatch()` 一处。
- 二期候选见 `docs/plan.md` §7 末尾：Basic Notification 推送、Profile G 录像回放、H.265、mp4 导入、多播、IPv6、RTSPS/HTTPS、对讲本机回放、TP-Link MULTITRANS。

## GitHub Actions

### 工具链：官方 Qt，不走 conda

CI 与发版用**各平台官方的那一套** —— Qt 用官方二进制（`jurplel/install-qt-action`，
版本钉在两个 workflow 顶部的 `QT_VERSION`），编译器用 runner 自带的
（gcc / clang / MSVC）。conda 只是**本机开发**的参考环境。

原来 CI 也走 conda-forge（micromamba + `qt6-main` + `cxx-compiler`），图的是三平台
一个 Qt 版本、和开发机对得上。实际付出的账单：

- conda Qt 是 MSVC 编的，而 Windows runner 预装了 MinGW gcc，CMake 先摸到后者 →
  满屏 `undefined reference to __imp__ZNK9QIODevice`；
- `micromamba-shell` 只在 macOS / Linux 上有，Windows 要另写一套；而 `shell:` 这个
  键又不吃 matrix 上下文，步骤只能按平台硬拆；
- 自定义 shell 不会被注入 `set -e`，打包脚本失败过一次而 job 照样是绿的；
- conda 的 GCC 15 编出来的东西要 `CXXABI_1.3.15`，AppImage 拿到 Ubuntu 22.04 上
  直接起不来 —— 首次发版试跑时才撞上；
- conda-forge 的 Qt 还动态链着 zlib / pcre2 / zstd / brotli / freetype，Windows
  打包得自己把它们捞出来（官方 Qt 二进制自带，`Deploy.cmake` 里那套
  `GET_RUNTIME_DEPENDENCIES` 就是为它写的）。

五条里没有一条是「conda 本身不好」，都是**拿一个跨平台包管理器去替代各平台原生
工具链**带来的。教训：CI 上尽量用目标平台自己的那套东西。

**AppImage 必须在支持的最老发行版上构建**（现在钉 ubuntu-22.04 + runner 自带
gcc）。linuxdeploy 按设计不打包 libstdc++，所以产物对 GLIBCXX 的要求就是构建机
那一份。

### 改 workflow 先跑 actionlint

别靠推上去试：

```bash
docker run --rm -v "$PWD:/repo" -w /repo rhysd/actionlint:latest
```

有一次整个 workflow 被拒绝解析，页面只给一句「workflow file issue」、一个 job
都不起、看不出哪一行 —— actionlint 一秒指出是 `shell:` 这个键**不支持 matrix
上下文**。（`run:` 是吃的，所以平台差异一律用 `run` 里的矩阵变量表达，
例如 `cmake --preset ci ${{ matrix.cmake_extra }}`。）

### 其余几条

- **Windows runner 预装了 `C:\mingw64\bin\gcc`**，CMake 在 PATH 上先摸到它就会
  拿 MinGW 去链 MSVC 编的 Qt，报一屏 `undefined reference to __imp__...`。看着像
  缺库，其实是编译器选错。必须 vswhere + `Enter-VsDevShell`，再显式
  `-DCMAKE_CXX_COMPILER=cl`。DevShell 改完的环境写进 `$GITHUB_ENV`，后面的步骤
  （包括 `shell: bash`）就都在 MSVC 环境里了。
- **自定义 shell（`micromamba-shell` 之类）不会被注入 `set -e`。** GitHub 只对内置
  bash 注入。这条咬过两次：AppImage 打包失败而 job 全绿，差点发出去少了产物的
  Release；CI 里 macOS 的冒烟步骤路径写错（`bin/onvifsim`，可 macOS 产的是
  `bin/onvifsim.app/Contents/MacOS/onvifsim`），报了几个月的
  "No such file or directory" 而没人知道。现在一律 `shell: bash` + 自己写
  `set -euo pipefail`。
- **`download-artifact` 无差别全收会挂**：docker/build-push-action 自己会传一个
  `<owner>~<repo>~XXXX.dockerbuild` 空产物，下到它就
  `Artifact download failed after 5 retries`。用 `pattern` 按名字挑。
- **系统依赖**：AppImage 的 `linuxdeploy-plugin-qt` 解依赖要 `libegl1` / `libgl1`，
  跑冒烟还要 `fuse` / `libfuse2`；发行版 Qt 那条要 `qt6-l10n-tools`
  （`qt6-tools-dev` 只给 CMake 配置、不给 lconvert 的二进制）、
  `libgl1-mesa-dev`（Qt6Gui 的 WrapOpenGL）和 `qt6-qpa-plugins`（打包脚本的冒烟要
  加载 offscreen 平台插件，而它在装 deb 之前就跑了）。
- **发布前有一道产物清单校验**（publish job 里）。六个产物逐条点名，少哪个报哪个。
  `set -e` 只能管住已知的脚本，这道管的是最终结果 —— 加它的直接原因就是那次
  「绿灯但少了 AppImage」。

### 发版流程

改 `CHANGELOG.md` 的版本段标题为 `## [X.Y.Z] - 日期`（publish 是按这个抽发布说明
的），然后 `git tag -a vX.Y.Z && git push origin vX.Y.Z`。

**先用 `-rc` 结尾的 tag 试跑** —— workflow 认这个后缀会标成 prerelease，出了问题
删掉重来，不会在 Release 页面留下一个残缺的正式版本。这条不是形式主义：首次发版的
rc 有两轮都红在 AppImage 上，而其中一个缺陷本机根本复现不出来（开发机的
libstdc++ 比目标系统新）。

## 这轮复核定下来的几条规矩

开源前做过一次全量工程规范复核，下面几条是从中提炼的、以后要一直守住的：

- **不要 fail-open。** `if (camera) { 校验… }` 这种写法在没有相机时会把整段鉴权跳过。生产路径上到不了不是理由 —— 别人照着改的时候就到得了。没有相机就拒绝。
- **`0` 在很多位置是有语义的**（亮度 0 = 关灯、PTZ 速度 0 = 停、pan 0 = 转到正中）。所以 `toInt()` / `toDouble()` **必须查 `ok`**，静默归零等于替客户端执行了一次它没要求的反向操作，而且还回成功。
- **端口、时长这类值要卡范围，不要 `static_cast` 截断。** `--http-port 99999` 截成 33903 照常启动，用户完全无从查起。
- **信号发出去之前先把要用的东西拷出来。** `emit` 是同步的，槽里动了容器，`last()` / 引用当场失效。
- **承诺过的返回值要真的返回。** 头文件写了「失败返回 false」就不能恒返回 true；注释写了「退出码要能让 CI 看出问题」就得真的改退出码。
- **Ctrl-C 必须走 Qt 的正常退出路径**（`cli/CliOptions.cpp` 里装了 SIGINT/SIGTERM 处理）。否则 `aboutToQuit` 不跑，独立 IP 模式加的别名会留在系统网卡上。
- **公开头文件里不留没人用的 API。** 要么接上，要么删掉 —— 对第一次读代码的人是纯负担。

## 约定

- C++ 文件 4 空格缩进；CMake / JSON / YAML / Markdown 2 空格。LF、UTF-8、文件末尾留换行（见 `.editorconfig`）。
- CI 里通过 `cmake/CompilerWarnings.cmake` 开 `-Wall -Wextra -Werror`。
- 文档：README 有中英两份（`README.md` / `README.zh-CN.md`），**改一份要同步另一份**，英文那份保持英文。`docs/plan.md` 与 `docs/reference-client-facts.md` 是中文。
- **三类字串三套规矩**（这条踩过：切成英文之后「添加相机」的下拉是一列中文、故障注入页整页中文）：

  | 类别 | 规矩 | 落地方式 |
  |---|---|---|
  | 界面自己的按钮 / 标签 | 跟界面语言 | `tr()`，lupdate 扫得到 |
  | core 的数据表（预设名、quirk 标题 / 说明 / 参数说明） | 跟界面语言 | 源串是中文数据、不经 tr()；`tools/make-i18n.py` 从表生成 `src/core/QuirkStrings.cpp`（只含 `QT_TRANSLATE_NOOP`）让 lupdate 扫到，界面经 `gui/I18n.h` 查译文 |
  | 会弹到对话框的错误 | 跟界面语言 | core 里直接写 `QCoreApplication::translate("onvifsim::core", …)`，`.arg()` 在译文上做 |
  | **日志摘要** | **固定英文，不跟界面变** | 源码里直接写英文字面量 |

  日志固定英文是刻意的：它会落文件、进 stdout、走 REST 的 SSE。跟着 locale 变的话，issue 里贴上来的日志可能是维护者读不懂的语言，按文本做匹配的自动化也不可靠。

  **不要往日志里写中文。** `tst_i18n` 管不到这条，靠的是 review。

- 加了新的 `tr()` 或改了 quirk 表之后：

  ```bash
  tools/make-i18n.py                                  # 改过 quirk 表才需要
  lupdate6 -I src src -locations none -no-obsolete \
      -ts assets/i18n/onvifsim_zh_CN.ts assets/i18n/onvifsim_en.ts
  ```

  **注意扫的是整个 `src` 而不只是 `src/gui`** —— 少了 core 那部分，`QuirkStrings.cpp` 里的两百多条会被 lupdate 当废弃条目删掉（踩过）。`tst_i18n` 会校验每条都有英文译文，忘了跑就会挂。

  另外两条老坑仍然成立：

  - **`-I src` 不能省**。少了它 lupdate 解析不到 `gui/*.h`，context 会退化成不带命名空间的 `MainWindow`，与运行期的 `onvifsim::gui::MainWindow` 对不上，译文一句都不生效。
  - **不要用正则改 `.ts`** —— 跨 `<message>` 边界的贪婪匹配会把 XML 配对冲掉（踩过）。要批量改就用 XML 解析器。
  - 补不全译文不会报错：`QTranslator` 找不到译文就**静默**退回中文原文。
- 生成物不要手改：`docs/quirks.md` 由 `--list-quirks --markdown` 生成（`scripts/gen-docs.sh`），改了 quirk 表要重新生成并一起提交；CI 会校验它与代码同源。
