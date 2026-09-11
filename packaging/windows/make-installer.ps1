<#
.SYNOPSIS
  Windows 引导式安装包：先出便携目录，再用 Inno Setup 编成 setup.exe。

.DESCRIPTION
  免安装 zip 仍然是主发布产物，这个是给"想要双击下一步"的人准备的。
  安装包做的事：选目录、建开始菜单与桌面快捷方式、写卸载信息、装完可直接启动。

  默认装到用户目录（不要管理员权限）。安装向导里可以切到「为所有用户安装」，
  那时会走 UAC 提权装进 Program Files。

.EXAMPLE
  pwsh packaging/windows/make-installer.ps1
  pwsh packaging/windows/make-installer.ps1 -BuildDir build/conda-win
#>
[CmdletBinding()]
param(
    [string]$BuildDir = "build/release",
    [string]$OutputDir = "dist"
)

$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "../..")
$outputPath = if ([System.IO.Path]::IsPathRooted($OutputDir)) { $OutputDir }
              else { Join-Path $repoRoot $OutputDir }

# ---- 1. 先出便携目录（安装包的素材就是它）----
Write-Host "== 准备便携目录 =="
& (Join-Path $PSScriptRoot "make-portable.ps1") -BuildDir $BuildDir -OutputDir $OutputDir -KeepStage
if ($LASTEXITCODE -ne 0) { throw "make-portable.ps1 失败" }

$stage = Get-ChildItem $outputPath -Directory -Filter "onvifsim-*-windows-x64" |
         Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $stage) { throw "找不到便携目录，make-portable.ps1 是不是没跑成功？" }

# 版本号从目录名里抠，保证与便携包一致。
if ($stage.Name -notmatch "^onvifsim-(.+)-windows-x64$") { throw "目录名认不出版本号：$($stage.Name)" }
$version = $Matches[1]
Write-Host "版本 $version，素材 $($stage.FullName)"

# ---- 2. 找 ISCC ----
# Inno Setup 6 既可能装在 Program Files，也可能是用户级安装（winget 默认走这条）。
$candidates = @(
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
)
$iscc = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) {
    $fromPath = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($fromPath) { $iscc = $fromPath.Source }
}
if (-not $iscc) {
    throw @"
找不到 Inno Setup 的 ISCC.exe。装一下：
    winget install --id JRSoftware.InnoSetup -e
找过这些位置：
$($candidates -join "`n")
"@
}
Write-Host "ISCC: $iscc"

# ---- 3. 编译 ----
Write-Host "== 编译安装包 =="
& $iscc "/DAppVersion=$version" "/DStageDir=$($stage.FullName)" `
        "/O$outputPath" (Join-Path $PSScriptRoot "onvifsim.iss")
if ($LASTEXITCODE -ne 0) { throw "ISCC 编译失败（退出码 $LASTEXITCODE）" }

$setup = Join-Path $outputPath "onvifsim-$version-windows-x64-setup.exe"
if (-not (Test-Path $setup)) { throw "编译说成功了，却找不到 $setup" }

$size = "{0:N1} MB" -f ((Get-Item $setup).Length / 1MB)
Write-Host ""
Write-Host "安装包好了：$setup  ($size)"
