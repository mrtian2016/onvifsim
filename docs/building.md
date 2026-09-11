# 构建指引

onvifsim 是 C++17 + Qt6，**零第三方库**：XML、HTTP、RTSP、RTP、Digest 鉴权、JPEG
全部基于 Qt 手写。所以「装依赖」这一步只有一件事 —— 装 Qt6。

两条路都支持，按你手上有什么选：

| 路径 | 适合谁 | Qt 版本 |
|---|---|---|
| **conda** | 本机开发，想要三平台一致的工具链 | 6.6+（参考环境是 6.11） |
| **发行版自带** | Ubuntu 22.04 / Debian 12 上直接编 | 6.2 / 6.4 |
| **Qt 官方二进制** | CI 与发版用的就是这条 | workflow 里钉的 `QT_VERSION` |

CI 和发版**不走 conda**：Qt 用官方二进制（`jurplel/install-qt-action`），编译器
用 runner 自带的。原因见 `CLAUDE.md` 的「GitHub Actions」一节 —— 简单说，拿一个
跨平台包管理器替代各平台原生工具链，代价比收益大。

## 硬性约束：Qt 6.2 是下限

源码必须能用 **Qt 6.2 / CMake 3.21 / C++17** 编过 —— 哪怕参考环境的版本高得多。
理由很实际：Ubuntu 22.04 自带的就是 Qt 6.2，Debian 12 是 6.4，
不守住这条线，最常见的两个部署目标就编不了。

具体到写代码上：

- **不要用 `QHttpServer`** —— 6.4 之前是预览模块，所以 HTTP 是自己写的；
- 不要用 `qt_standard_project_setup()`（6.3 才有）；
- 不要用 6.2 之后才加的 API。

CI 里有一个专门的 job（`distro-qt`）在 Ubuntu 22.04 上用系统 Qt 6.2 构建，
就是这条线的看门人。

---

## 路径一：conda

参考工具链是已经建好的 conda 环境：

```bash
conda activate onvifsim        # Qt 6.11 / CMake 4.4 / Ninja / GCC 15
cmake --preset conda-linux
cmake --build --preset conda-linux
ctest --preset conda-linux
./build/conda-linux/bin/onvifsim --headless --scenario assets/scenarios/single-camera.json
```

自己从零建一个：

```bash
conda create -n onvifsim -c conda-forge qt6-main cmake ninja cxx-compiler
conda activate onvifsim
```

三个平台的预设分别是 `conda-linux` / `conda-mac` / `conda-win`，
都靠 `$CONDA_PREFIX` 找 Qt，所以**一定要先 activate 再 configure**。

### 环境里没有 QtMultimedia

conda-forge 的 `qt6-main` 不带 Multimedia，所以可选的本机对讲回放
（`-DENABLE_AUDIO_PLAYBACK=ON`）在这个环境里编不了。
装了 `qt6-multimedia` 之后才能打开这个开关。

### 环境里没有 Python

e2e 测试有自己独立的 venv，不走 conda —— 见 [`tests/e2e/README.md`](../tests/e2e/README.md)。
`ffprobe` 用系统的那个（`/usr/bin/ffprobe`），也只在测试里用；**运行期一行 ffmpeg 都不碰**。

---

## 路径二：发行版自带的 Qt

### Ubuntu 22.04 / 24.04、Debian 12

```bash
sudo apt install build-essential cmake ninja-build \
                 qt6-base-dev qt6-base-dev-tools \
                 qt6-tools-dev qt6-tools-dev-tools libgl1-mesa-dev

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

`qt6-tools-dev-tools` 提供 `lrelease`，界面翻译（`assets/i18n/*.ts`）要它才编得出 `.qm`。
没装也能构建，只是界面只剩源语言（中文）。

### Fedora

```bash
sudo dnf install gcc-c++ cmake ninja-build qt6-qtbase-devel qt6-qttools-devel
```

### Arch

```bash
sudo pacman -S base-devel cmake ninja qt6-base qt6-tools
```

### macOS（Homebrew）

```bash
brew install qt6 cmake ninja
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="$(brew --prefix qt6)"
cmake --build build
```

### Windows（MSVC + 官方 Qt 安装器）

```powershell
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="C:/Qt/6.6.3/msvc2019_64"
cmake --build build
```

Ninja 生成器下要在「x64 Native Tools Command Prompt」里跑，
或者先跑一遍 `vcvars64.bat`，否则找不到编译器。

---

## CMake 选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `BUILD_GUI` | `ON` | 编 Qt Widgets 界面。容器 / 服务器上关掉，只留 headless |
| `BUILD_TESTS` | `ON` | 编 QtTest 单元测试 |
| `ENABLE_AUDIO_PLAYBACK` | `OFF` | 本机播放收到的对讲音频，需要 QtMultimedia |
| `ENABLE_WERROR` | `OFF` | 警告当错误。CI 打开（`ci` 预设） |

## CMake 预设

| 预设 | 用途 |
|---|---|
| `conda-linux` / `conda-mac` / `conda-win` | 日常开发，从 `$CONDA_PREFIX` 找 Qt |
| `headless` | `BUILD_GUI=OFF`，容器与无显示器的机器 |
| `release` | `Release` + `BUILD_TESTS=OFF`，打包用 |
| `ci` | 在 conda 基础上加 `ENABLE_WERROR=ON` |

```bash
cmake --preset headless && cmake --build --preset headless
```

---

## 无界面模式怎么跑起来的

`--headless` 用的是 **`QGuiApplication` + `offscreen` 平台插件**，不是 `QCoreApplication`。
原因是快照要靠 `QImage` + `QPainter` 画字，而 `QFontDatabase` 没有 `QGuiApplication`
会直接 abort。程序会自动设 `QT_QPA_PLATFORM=offscreen`（你显式设过就尊重你的）。

实际影响：

- **容器里要装 `libqt6gui6` 和 `qt6-qpa-plugins`**，光有 Core / Network 不够；
- 还要有字体（`fonts-dejavu-core`），否则快照上的时间戳画不出来；
- 但**不需要 X server、不需要 Wayland、不需要显示器**。

`--version` / `--help` / `--list-*` 这些纯查询走的是最轻的 `QCoreApplication`，
在完全没有图形库的机器上也能用。

---

## 构建之后

```bash
./build/conda-linux/bin/onvifsim                      # GUI
./build/conda-linux/bin/onvifsim --headless --cameras 8 --preset hikvision
./build/conda-linux/bin/onvifsim --list-presets
./build/conda-linux/bin/onvifsim --list-quirks
./build/conda-linux/bin/onvifsim --list-scenarios
```

生成故障注入文档（**`docs/quirks.md` 是生成物，不要手改**）：

```bash
./scripts/gen-docs.sh
```

跑端到端测试：见 [`tests/e2e/README.md`](../tests/e2e/README.md)。

---

## 打包

| 平台 | 命令 | 产物 |
|---|---|---|
| Linux | `./packaging/linux/make-appimage.sh` | `dist/onvifsim-<版本>-x86_64.AppImage` |
| Linux | `./packaging/linux/make-deb.sh` | `dist/onvifsim_<版本>-1_amd64.deb` |
| Linux | `./packaging/linux/make-tarball.sh` | `dist/onvifsim-<版本>-linux-x86_64.tar.gz` |
| Windows | `pwsh packaging/windows/make-portable.ps1` | `dist/onvifsim-<版本>-windows-x64.zip` |
| macOS | `./packaging/macos/make-dmg.sh` | `dist/onvifsim-<版本>-macos-<arch>.dmg` |
| 容器 | `docker build -f packaging/docker/Dockerfile -t onvifsim .` | 镜像 |

这些脚本都先要一份 `release` 预设的构建：

```bash
cmake --preset release && cmake --build --preset release
```

**`make-deb.sh` 与 `make-tarball.sh` 是例外，它们必须用发行版的 Qt 构建**，
不能用 conda 的那份 —— 这两个包都不带 Qt 运行时：

```bash
sudo apt install qt6-base-dev qt6-tools-dev qt6-l10n-tools dpkg-dev fakeroot
cmake -S . -B build/deb -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
cmake --build build/deb
./packaging/linux/make-deb.sh     build/deb dist
./packaging/linux/make-tarball.sh build/deb dist
```

deb 的 `Depends` 是 `dpkg-shlibdeps` 从二进制实际链接的 `.so` 反推出来的。
拿 conda Qt 6.11 编出来的程序去打包，反推出的依赖是错的，装到只有 Qt 6.4 的
系统上会直接起不来 —— 而 dpkg 对这种错配毫无察觉。tar.gz 更直接：conda 构建
要求 `Qt_6.11` 这样的版本化符号、rpath 还指着构建机的 conda 目录，解到别人
机器上一跑就是 `libQt6Core.so.6: version 'Qt_6.11' not found`。
两个脚本里都有一道 rpath 检查，发现 conda 的痕迹就直接拒绝。

v0.1.0 的 tar.gz 正是漏了这道检查发出去的（当时守卫只在 deb 那边有）。

部署工具（windeployqt / macdeployqt / linuxdeploy）的调用统一封在
`cmake/Deploy.cmake` 里，三个脚本共用同一份逻辑。它既能当 CMake 模块 `include()`，
也能直接当脚本跑：

```bash
cmake -DONVIFSIM_DEPLOY_BINARY=build/release/bin/onvifsim.exe -P cmake/Deploy.cmake
cmake -DONVIFSIM_DEPLOY_BUNDLE=build/release/bin/onvifsim.app -P cmake/Deploy.cmake
```

macOS 的 dmg 用 **ad-hoc 签名**（`codesign -s -`），不需要 Apple 开发者账号。
在 arm64 上这一步是必须的 —— 未签名的二进制根本起不来。
代价是别人第一次打开要右键「打开」，或者：

```bash
xattr -dr com.apple.quarantine /Applications/onvifsim.app
```

---

## Linux 的三种产物

| 产物 | 脚本 | 什么时候用 |
|---|---|---|
| `.AppImage` | `make-appimage.sh` | 自带 Qt，下载 `chmod +x` 就能跑，不挑发行版。给不想装东西的人 |
| `.deb` | `make-deb.sh` | Debian / Ubuntu 正经装到 `/usr`，进开始菜单、能 `apt remove`。靠系统 Qt |
| `.tar.gz` | `make-tarball.sh` | 解压即用的便携目录，靠系统 Qt。给要塞进自己脚本里的人 |

三份都带 `assets/logo/` 里的图标：AppImage 由 linuxdeploy 内嵌，deb 与 tar.gz
铺 `share/icons/hicolor/` 目录树。三份也都带 `share/onvifsim/i18n/*.qm`，
漏了英文界面会静默退回中文。

---

## 打包脚本里的冒烟守卫

四个打包脚本（deb / tar.gz / AppImage / dmg，以及 Windows 的
`make-portable.ps1`）在产物封口之前都会**跑一遍产物本身**：先 `--version`，
再 `--headless` 起 5 秒看它还活着。失败就不出包。共用实现在
`packaging/common.sh`。

这道守卫是 v0.1.0 之后补的。那次六个产物里三个起不来（AppImage 的 Qt 插件
路径、tar.gz 的 conda Qt、dmg 缺 offscreen 插件），而单测、e2e、CI 全绿 ——
因为没有任何一处跑过打好的产物。唯一完好的 `.deb`，恰恰是唯一在 CI 里装了
再跑一遍的那个。

两个容易把守卫写成摆设的地方：

- **只跑 `--version` 等于没跑。** 它走不到 `QGuiApplication`，缺平台插件照样
  打印版本号。必须跑 `--headless` —— 它会加载 offscreen 平台插件，和图形模式
  走同一条查找路径。
- **AppImage 要去掉 `APPIMAGE_EXTRACT_AND_RUN`。** 带着它跑，AppImage 会自解压
  再执行里面的二进制，恰好绕开「经运行时挂载启动」这条唯一会出问题的路径。

---

## 图标

母版是 `assets/logo/onvifsim.svg`（外加一份 32px 以下用的简化版
`onvifsim-small.svg` —— 发现环的虚线在小尺寸下会糊成噪点）。改了母版之后：

```bash
tools/make-icons.sh
```

它会渲染 `assets/logo/png/*.png`，再打包出 `packaging/windows/onvifsim.ico`
与 `packaging/macos/onvifsim.icns`。**生成物是入库的**，构建期不做矢量渲染，
CI 上就不用装 rsvg / Inkscape。

渲染器用的是 Qt 自己的 `QSvgRenderer`（`tools/svg2png`，一个不挂进主构建的
小工具），不是 rsvg：这样看到的就是程序运行时看到的，SVG 里一旦用了 Qt 不
支持的特性（CSS、filter、mask），生成这一步就会直接露馅。

图标接到程序里的路径有三条，缺一条就会出现「某个地方是白纸」：

| 位置 | 靠什么 |
|---|---|
| 窗口标题栏、托盘、对话框 | `src/gui/icons.qrc` → `util::appIcon()` |
| Windows 资源管理器里的 exe | `packaging/windows/onvifsim.rc.in` 内嵌的 ICON 资源 |
| macOS Dock / Finder | `Info.plist` 的 `CFBundleIconFile` + `Contents/Resources/onvifsim.icns` |

`tst_gui_icons` 盯着第一条：qrc 别名写错、静态库少一次 `Q_INIT_RESOURCE`，
QIcon 都只会安静地给出空图，不会报错。

---

## 常见问题

**`Could NOT find Qt6`** —— conda 路径下忘了 `conda activate`；
发行版路径下补 `-DCMAKE_PREFIX_PATH=/path/to/qt6`。

**Windows 上 `windeployqt` 找不到** —— 它在 Qt 的 `bin` 目录里，
`conda activate` 之后就在 PATH 上；用官方安装器的话要手工把
`C:\Qt\6.x.x\msvc2019_64\bin` 加进 PATH。

**AppImage 打不出来，报 FUSE 相关的错** —— 设 `APPIMAGE_EXTRACT_AND_RUN=1`
（`make-appimage.sh` 里已经默认设了），或者装 `libfuse2`。

**运行时报 `qt.qpa.plugin: Could not load the Qt platform plugin "offscreen"`** ——
缺 `qt6-qpa-plugins`（Debian 系）或对应的插件包。

**多播搜不到设备** —— 见 [`docs/compat-matrix.md`](compat-matrix.md) 里
各平台防火墙与多播的注意事项。macOS 14 起还必须在 `Info.plist` 里声明
`NSLocalNetworkUsageDescription`，否则系统会**静默**拦掉多播。


## Windows 的两种产物

| 产物 | 怎么出 | 适合谁 |
|---|---|---|
| `onvifsim-<版本>-windows-x64.zip` | `packaging/windows/make-portable.ps1` | 解压即跑，不写注册表、不留痕迹。CI 与临时排查用这个 |
| `onvifsim-<版本>-windows-x64-setup.exe` | `packaging/windows/make-installer.ps1` | 双击下一步的引导安装，有开始菜单项、桌面快捷方式，控制面板里能卸载 |

安装包需要 Inno Setup 6：

```powershell
winget install --id JRSoftware.InnoSetup -e
pwsh packaging\windows\make-installer.ps1 -BuildDir build\conda-win
```

`make-installer.ps1` 会先调 `make-portable.ps1 -KeepStage` 出便携目录，再用它当素材编译。
ISCC.exe 在 `%LOCALAPPDATA%\Programs\Inno Setup 6\`（winget 默认装用户级）
与 Program Files 两处都会找。

**简体中文语言文件随项目带**（`packaging/windows/ChineseSimplified.isl`）——
Inno Setup 官方发行版不带它，写 `compiler:Languages\ChineseSimplified.isl`
在任何干净机器上都会编译失败。

安装包默认装到用户目录、不要管理员权限；向导里可以切成「为所有用户安装」，
那时会走 UAC 提权装进 Program Files。


## macOS 上的 SDK 错配

新版 macOS 上可能撞到这个（实测 macOS 27 + Xcode 26.5）：

```
ld: tapi error: malformed file
.../MacOSX27.0.sdk/usr/lib/libSystem.B.tbd:4:20: error: unknown architecture
                   arm64e.x1-macos, arm64e.x1-maccatalyst ]
```

**原因**：Command Line Tools 与 Xcode 各带一套 SDK，而 `xcrun` 默认用的是 CLT 那套。
当 CLT 的 SDK 比 Xcode 新时（比如 CLT 给了 `MacOSX27.0.sdk`，Xcode 里最新才 `26.5`），
SDK 里出现了新架构标记 `arm64e.x1`，而正在用的 linker 还不认识它 ——
这时候连 hello world 都编不过，跟本项目没关系。

**验证**：

```bash
xcode-select -p                 # 指向 Xcode.app
xcrun --show-sdk-path           # 却返回 CommandLineTools 的 SDK —— 就是它
```

**绕过**：显式指定 Xcode 自带的 SDK。

```bash
export SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.5.sdk
cmake --preset conda-mac -DCMAKE_OSX_SYSROOT=$SDKROOT
```

**根治**：把 Xcode 升到与 CLT 匹配的版本，或者干脆卸掉 Command Line Tools
（`sudo rm -rf /Library/Developer/CommandLineTools`），让 `xcrun` 回到 Xcode 那套。

GitHub Actions 的 macOS runner 上不会有这个问题 —— 镜像里 Xcode 与 CLT 是配套的。
