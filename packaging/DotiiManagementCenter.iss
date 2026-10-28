; Compile after creating release\DotiiManagementCenter-{#AppVersion}.
#ifndef AppVersion
  #define AppVersion "1.2.3"
#endif

[Setup]
AppId=DotiiManagementCenter
AppName=Dotii 管理中心
AppVersion={#AppVersion}
AppPublisher=Dotii
AppPublisherURL=https://github.com/Cvencent/Dotii-Display
AppSupportURL=https://github.com/Cvencent/Dotii-Display/issues
AppUpdatesURL=https://github.com/Cvencent/Dotii-Display/releases
DefaultDirName={localappdata}\Programs\Dotii Management Center
DefaultGroupName=Dotii 管理中心
PrivilegesRequired=lowest
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\release
OutputBaseFilename=Dotii-Setup-{#AppVersion}
SetupIconFile=..\bridge\assets\dotii.ico
UninstallDisplayName=Dotii 管理中心 {#AppVersion}
UninstallDisplayIcon={app}\DotiiManagementCenter.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#AppVersion}
VersionInfoProductVersion={#AppVersion}
VersionInfoDescription=Dotii 管理中心安装程序
VersionInfoProductName=Dotii 管理中心

[Languages]
Name: "chinesesimplified"; MessagesFile: "languages\ChineseSimplified.isl"

[Files]
Source: "..\release\DotiiManagementCenter-{#AppVersion}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Dotii 管理中心"; Filename: "{app}\DotiiManagementCenter.exe"
Name: "{autodesktop}\Dotii 管理中心"; Filename: "{app}\DotiiManagementCenter.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加快捷方式："

[Run]
Filename: "{app}\DotiiManagementCenter.exe"; Description: "启动 Dotii 管理中心"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}"
