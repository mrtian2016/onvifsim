<#
.SYNOPSIS
  Windows 便携版：windeployqt 收齐 Qt 运行时，再打成免安装 zip。

.DESCRIPTION
  产物解压即跑，不需要装 Qt、不需要管理员权限。
  windeployqt 的调用统一走 cmake/Deploy.cmake（脚本模式），
  这样三个平台的部署逻辑只有一份。

.EXAMPLE
  pwsh packaging/windows/make-portable.ps1
  pwsh packaging/windows/make-portable.ps1 -BuildDir build/release -OutputDir dist
#>
[CmdletBinding()]
param(
    [string]$BuildDir = "build/release",
    [string]$OutputDir = "dist",
    # 保留展开后的目录（默认打完 zip 就删）。做安装包时要用它当素材，
    # 所以 make-installer.ps1 会带上这个开关。
    [switch]$KeepStage
)

$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "../..")

$buildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir }
             else { Join-Path $repoRoot $BuildDir }
$outputPath = if ([System.IO.Path]::IsPathRooted($OutputDir)) { $OutputDir }
              else { Join-Path $repoRoot $OutputDir }

$binary = Join-Path $buildPath "bin/onvifsim.exe"
if (-not (Test-Path $binary)) {
    $binary = Join-Path $buildPath "onvifsim.exe"
}
if (-not (Test-Path $binary)) {
    throw "找不到 onvifsim.exe。先构建：cmake --preset conda-win; cmake --build build/conda-win"
}

# 命令行版：GUI 子系统的 onvifsim.exe 在管道里拿不到 stdout，
# 版本号只能问控制台子系统的这个。它也要一起进包 —— 用户在 cmd 里
# 跑 --headless 时用的就是它。
$cliBinary = Join-Path (Split-Path $binary) "onvifsim-cli.exe"
$hasCli = Test-Path $cliBinary

$versionSource = if ($hasCli) { $cliBinary } else { $binary }
$versionLine = (& $versionSource --version | Out-String).Trim()
if (-not $versionLine) { throw "取不到版本号（$versionSource --version 没有输出）" }
$version = $versionLine.Split(" ")[1].Trim()
$name = "onvifsim-$version-windows-x64"
# 放 dist 下而不是临时目录：安装包脚本要按相对路径引用它，
# 搁在 %TEMP% 里的话 .iss 那边没法写一个稳定的路径。
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$stage = Join-Path $outputPath $name
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

Write-Host "打包 $name"
Copy-Item $binary $stage
if ($hasCli) { Copy-Item $cliBinary $stage }

# Qt 的 DLL、平台插件、样式插件都由 windeployqt 收。
& cmake "-DONVIFSIM_DEPLOY_BINARY=$(Join-Path $stage 'onvifsim.exe')" `
        "-DONVIFSIM_DEPLOY_DIR=$stage" `
        -P (Join-Path $repoRoot "cmake/Deploy.cmake")
if ($LASTEXITCODE -ne 0) { throw "windeployqt 失败" }

# windeployqt 会顺手拷 DirectX 的着色器编译器（dxcompiler / dxil，合起来 20 MB+）。
# 那是 Qt 的 D3D RHI 后端用的，本项目只有 Widgets 与 QPainter，一行 3D 都没有。
# vc_redist 安装器同理：MSVC 运行时的 DLL 已经在包里了。
foreach ($junk in @("dxcompiler.dll", "dxil.dll", "vc_redist.x64.exe", "vc_redist.x86.exe")) {
    $path = Join-Path $stage $junk
    if (Test-Path $path) { Remove-Item $path -Force; Write-Host "  剔除 $junk" }
}

# 界面翻译（构建期由 lrelease 生成到 bin/i18n）。漏了它整个英文界面就是哑的 ——
# 而且不会报错，只是所有文字都退回中文原文，很难一眼看出来。
$i18nSource = Join-Path (Split-Path $binary) "i18n"
if (Test-Path $i18nSource) {
    Copy-Item $i18nSource (Join-Path $stage "i18n") -Recurse
    $qmCount = (Get-ChildItem (Join-Path $stage "i18n") -Filter *.qm | Measure-Object).Count
    Write-Host "  翻译 $qmCount 份"
} else {
    Write-Warning "找不到 $i18nSource —— 打出来的包不会有英文界面"
}

Copy-Item (Join-Path $repoRoot "assets/scenarios") (Join-Path $stage "scenarios") -Recurse
Copy-Item (Join-Path $repoRoot "LICENSE") $stage
Copy-Item (Join-Path $repoRoot "README.md") $stage
foreach ($optional in @("README.zh-CN.md", "CHANGELOG.md")) {
    $path = Join-Path $repoRoot $optional
    if (Test-Path $path) { Copy-Item $path $stage }
}

@"
onvifsim —— ONVIF 摄像头模拟器（Windows 免安装版）

解压到任意目录，双击 onvifsim.exe 即可。Qt 运行时已经打包在内，不需要另外安装。

命令行（无界面）：
  onvifsim-cli.exe --headless --scenario scenarios\single-camera.json
  onvifsim-cli.exe --list-quirks

  为什么有两个 exe：onvifsim.exe 是图形子系统的程序，双击不会弹黑框，
  但在命令行里它拿不到控制台、看不到输出；onvifsim-cli.exe 是控制台子系统的，
  命令行用它。两个的功能完全一样。

注意：
- 默认用高端口（HTTP 8000+ / RTSP 8554+），不需要管理员权限。
  要用标准的 80 / 554 得以管理员身份运行。
- 首次运行 Windows 防火墙会弹窗，要放行「专用网络」，
  否则局域网里的客户端发现不到，也拉不了流。
"@ | Set-Content -Path (Join-Path $stage "README-FIRST.txt") -Encoding UTF8

New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$zip = Join-Path $outputPath "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip

if ($KeepStage) {
    Write-Host "展开目录保留在：$stage"
} else {
    Remove-Item $stage -Recurse -Force
}

Write-Host "打好了：$zip"
