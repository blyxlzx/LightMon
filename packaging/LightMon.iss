; Inno Setup 6.5+. Build-Release.ps1 provides a clean allowlisted payload.
#ifndef AppVersion
  #define AppVersion "1.1.28"
#endif
#ifndef PayloadRoot
  #define PayloadRoot "..\build\payload"
#endif
#ifndef ReleaseOutputDir
  #define ReleaseOutputDir "..\dist"
#endif
#ifndef InstallerAppId
  #define InstallerAppId "{{7E2CD476-B2D2-4193-B06E-55D0A22E0966}"
#endif
#ifndef InstallerAppMutex
  #define InstallerAppMutex "Local\LightMon.Native.Panel.v1.Single"
#endif
#ifndef InstallerSetupMutex
  #define InstallerSetupMutex "Local\LightMon.Setup.v1"
#endif

[Setup]
AppId={#InstallerAppId}
AppName=LightMon
AppVerName=LightMon {#AppVersion}
AppVersion={#AppVersion}
AppPublisher=LightMon contributors
AppPublisherURL=https://github.com/blyxlzx/LightMon
AppComments=Windows 11 taskbar system monitor
VersionInfoVersion={#AppVersion}.0
DefaultDirName={localappdata}\Programs\LightMon
DefaultGroupName=LightMon
DisableProgramGroupPage=no
AllowNoIcons=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.22000
OutputDir={#ReleaseOutputDir}
OutputBaseFilename=LightMon-{#AppVersion}-Setup-x64
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\source\src\app.ico
SetupLogging=yes
Uninstallable=yes
UninstallDisplayIcon={app}\LightMon.exe
UninstallDisplayName=LightMon {#AppVersion}
AppMutex={#InstallerAppMutex}
SetupMutex={#InstallerSetupMutex}
CloseApplications=no
RestartApplications=no
AlwaysRestart=no
RestartIfNeededByRun=no
ChangesAssociations=no
ChangesEnvironment=no
LicenseFile=..\LICENSE

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "ChineseSimplified.isl"

[Tasks]
Name: "autostart"; Description: "Start at Windows login and recover from unexpected exit / 登录启动并自动恢复"; GroupDescription: "Startup / 启动方式:"; Flags: unchecked
Name: "desktopicon"; Description: "Create a desktop shortcut / 创建桌面快捷方式"; GroupDescription: "Shortcuts / 快捷方式:"; Flags: unchecked

[Files]
Source: "{#PayloadRoot}\LightMon.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadRoot}\LightMonCpu.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadRoot}\LightMonCpu.sha256"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadRoot}\LightMon.ini"; DestDir: "{app}"; Flags: onlyifdoesntexist uninsneveruninstall
Source: "{#PayloadRoot}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadRoot}\README.en.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadRoot}\CHANGELOG.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadRoot}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadRoot}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadRoot}\LICENSES\*"; DestDir: "{app}\LICENSES"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadRoot}\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadRoot}\scripts\*"; DestDir: "{app}\scripts"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadRoot}\source\*"; DestDir: "{app}\source"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadRoot}\third_party\*"; DestDir: "{app}\third_party"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadRoot}\packaging\*"; DestDir: "{app}\packaging"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\LightMon"; Filename: "{app}\LightMon.exe"; WorkingDir: "{app}"
Name: "{group}\LightMon README"; Filename: "{app}\README.md"
Name: "{group}\Uninstall LightMon"; Filename: "{uninstallexe}"
Name: "{autodesktop}\LightMon"; Filename: "{app}\LightMon.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\LightMon.exe"; WorkingDir: "{app}"; Description: "Launch LightMon / 启动 LightMon"; Flags: nowait postinstall skipifsilent runasoriginaluser

[Code]
function ConfigureStartup(const Action: String): Boolean;
var
  ExitCode: Integer;
  Args: String;
begin
  Args := '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' +
    ExpandConstant('{app}\scripts\Configure-Startup.ps1') + '" -Action ' + Action +
    ' -InstallDirectory "' + ExpandConstant('{app}') + '"';
  Result := Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
    Args, ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ExitCode);
  Result := Result and (ExitCode = 0);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    if WizardIsTaskSelected('autostart') then begin
      if not ConfigureStartup('Enable') then
        MsgBox('LightMon was installed, but startup could not be configured. Another installation may own the startup task. See README.md for the manual startup command. / 程序已安装，但自启动未设置成功；可能有其他安装占用了任务。请查看 README.md。', mbError, MB_OK);
    end else if not ConfigureStartup('Disable') then
      Log('Could not remove an owned startup entry. Other installations were not modified.');
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    if not ConfigureStartup('Disable') then
      MsgBox('Could not remove the startup task. Disable the LightMon Monitor task in Task Scheduler if it points to this installation.', mbError, MB_OK);
end;

function InitializeUninstall: Boolean;
begin
  if not UninstallSilent then
    MsgBox('Settings, traffic totals and local logs are kept in the install folder. / 设置、流量记录及本地日志将保留在安装目录。', mbInformation, MB_OK);
  Result := True;
end;
