; Inno Setup 安装包配置（可选 —— 主发布产物是免安装 zip）。
;
; 用法：先跑 make-portable.ps1 生成 dist\onvifsim-<版本>-windows-x64\，
; 再用 Inno Setup 6 编译这个脚本：
;   iscc /DAppVersion=0.1.0 /DStageDir=..\..\dist\onvifsim-0.1.0-windows-x64 onvifsim.iss

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef StageDir
  #define StageDir "..\..\dist\onvifsim-" + AppVersion + "-windows-x64"
#endif

[Setup]
AppId={{9C1F1B2E-8F2A-4C1B-9F3D-0F6A2B7C4D51}
AppName=onvifsim
AppVersion={#AppVersion}
AppPublisher=onvifsim
AppPublisherURL=https://github.com/mrtian2016/onvifsim
DefaultDirName={autopf}\onvifsim
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE
OutputDir=..\..\dist
OutputBaseFilename=onvifsim-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; 安装程序自己的图标，以及「添加或删除程序」里显示的图标。后者不写的话，
; 控制面板会拿 exe 的第一个图标，卸载条目上却是一张白纸。
SetupIconFile=onvifsim.ico
UninstallDisplayIcon={app}\onvifsim.exe
; 默认装到用户目录，不要求管理员；要装到 Program Files 就把这行改成 admin。
PrivilegesRequiredOverridesAllowed=dialog
PrivilegesRequired=lowest
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
; Inno Setup 官方发行版**不带**简体中文，compiler:Languages\ChineseSimplified.isl
; 这个路径在任何一台干净机器上都会编译失败。所以把语言文件随项目一起带上，
; 走相对路径引用（来源见该文件头部的说明）。
Name: "chinese"; MessagesFile: "ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
; 直接放开始菜单根下，不建同名子文件夹 —— 只有一个程序时再套一层文件夹
; 是 XP 时代的习惯，Win10/11 的开始菜单里反而更难找。
Name: "{autoprograms}\onvifsim"; Filename: "{app}\onvifsim.exe"; \
  Comment: "ONVIF 摄像头模拟器"
Name: "{autoprograms}\onvifsim 命令行"; Filename: "{app}\onvifsim-cli.exe"; \
  Parameters: "--help"; Comment: "无界面模式的用法说明"; Tasks: cliShortcut
Name: "{autodesktop}\onvifsim"; Filename: "{app}\onvifsim.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "cliShortcut"; Description: "同时创建命令行版的快捷方式"; \
  GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Run]
Filename: "{app}\onvifsim.exe"; Description: "{cm:LaunchProgram,onvifsim}"; \
  Flags: nowait postinstall skipifsilent

[UninstallDelete]
; 界面把窗口位置、最近场景这些写在注册表里（QSettings），卸载时一并清掉，
; 不然重装之后还会带着上一次的布局与「托盘已迁移」标记。
Type: filesandordirs; Name: "{app}\logs"
