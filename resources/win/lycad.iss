; LyCAD — Windows 安装包脚本（Inno Setup 6）
;
; 本地编译（在仓库根目录）。版本号从 VERSION 读，别在命令行里写死：
;   set /p MyVer=<VERSION
;   iscc /DMyAppVersion=%MyVer% /DMyArch=x64 /DMyArchAllowed=x64compatible ^
;        /DMySourceDir=%CD%\build\lycad /DMyOutputDir=%CD%\dist resources\win\lycad.iss
;
; CI 的调用方式见 .github/workflows/release.yaml 的「编译 Windows 安装包」步骤。
;
; 说明：源目录（MySourceDir）指向 CMake 的安装暂存树，里面应当已经含
; windeployqt 部署好的 Qt DLL 与插件（bin\platforms、bin\imageformats…）。
; 与 LyCode 一样，Windows 产物有 zip 与 setup.exe 两种形态：zip 解压即用，
; setup.exe 走本脚本。
;
; 本目录里的 ChineseSimplified.isl 是社区维护的简体中文语言包
; （jrsoftware 的 istrans 收集，维护者 Kira <847320916@qq.com>，
;   https://github.com/kira-96/Inno-Setup-Chinese-Simplified-Translation），
; 按原样随仓库分发，要求 Inno Setup 6.5.0+。

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif

; MyArch 只用于产物文件名；MyArchAllowed 是 Inno 的目标架构约束。
; 两者分开是因为文件名想写 x64，而 Inno 的约束值要写 x64compatible。
; 出 arm64 包时用：
;   /DMyArch=arm64 /DMyArchAllowed=arm64
#ifndef MyArch
  #define MyArch "x64"
#endif
#ifndef MyArchAllowed
  #define MyArchAllowed "x64compatible"
#endif

#ifndef MySourceDir
  #define MySourceDir "..\..\build\lycad"
#endif
#ifndef MyOutputDir
  #define MyOutputDir "..\..\dist"
#endif
; 仓库根目录，用来取 LICENSE 与图标等仓库内文件（相对本脚本所在目录）。
#ifndef MyRepoRoot
  #define MyRepoRoot "..\.."
#endif
; 中文翻译文件路径。SourcePath 是含文件名的完整路径，取目录要用 ExtractFileDir。
#ifndef MyChineseIsl
  #define MyChineseIsl ExtractFileDir(SourcePath) + "\ChineseSimplified.isl"
#endif

#define MyAppName "LyCAD"
#define MyAppExeName "lycad.exe"
#define MyAppPublisher "lhdjply"
#define MyAppUrl "https://github.com/lhdjply/LyCAD"

[Setup]
; AppId 一旦发布就不要再改：升级安装与卸载识别都靠它。
; 这个 GUID 是 LyCAD 自己的，与 LyCode 的不能相同，否则两个软件会互相覆盖安装。
AppId={{2DA4F664-7957-45AD-88BB-DF5168E233BD}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppUrl}
AppSupportURL={#MyAppUrl}
AppUpdatesURL={#MyAppUrl}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
AllowNoIcons=yes
; 默认按用户安装（不弹 UAC），需要时可在向导里切换为全机器安装。
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed={#MyArchAllowed}
ArchitecturesInstallIn64BitMode={#MyArchAllowed}
OutputDir={#MyOutputDir}
OutputBaseFilename={#MyAppName}-{#MyAppVersion}-win-{#MyArch}-setup
; 应用图标：exe 的 PE 资源图标与本文件用的是同一枚 .ico
;（PE 资源见 resources/win/lycad.rc，两者都以 resources/app/lycad.ico 为准）。
SetupIconFile={#MyRepoRoot}\resources\app\lycad.ico
UninstallDisplayIcon={app}\bin\{#MyAppExeName}
LicenseFile={#MyRepoRoot}\LICENSE
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; 6.3+ 才有 x64compatible / arm64 这两个约束值。
MinVersion=10.0

[Languages]
; 中文放首位 = 默认语言。ChineseSimplified.isl 需要 Inno Setup 6.5.0+。
Name: "chinese"; MessagesFile: "compiler:Default.isl,{#MyChineseIsl}"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; bin 下是主程序 + windeployqt 部署的 Qt DLL 与插件（platforms/imageformats/…），必须递归。
Source: "{#MySourceDir}\bin\*"; DestDir: "{app}\bin"; Flags: recursesubdirs createallsubdirs ignoreversion
; Windows 上 CMake 只装 bin/（.desktop 与图标主题是 Linux 的事，根 CMakeLists.txt
; 里按平台分派），这里给 share 留一条：将来若加了平台无关的数据文件，不会漏装。
Source: "{#MySourceDir}\share\*"; DestDir: "{app}\share"; Flags: recursesubdirs createallsubdirs ignoreversion skipifsourcedoesntexist
Source: "{#MyRepoRoot}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\bin\{#MyAppExeName}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\bin\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
