; UvNVR Windows installer (Inno Setup 6.5+)
; Build:
;   ISCC.exe /DMyAppVersion=0.18.1 /DMyArch=x64 /DSourceDir=<stage> packaging\windows\uvnvr.iss

#ifndef MyAppVersion
  #define MyAppVersion "0.18.1"
#endif
#ifndef MyArch
  #define MyArch "x64"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\dist-packages\uvnvr-" + MyAppVersion + "-windows-" + MyArch
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist-packages"
#endif

#define MyAppName "UvNVR"
#define MyAppPublisher "Antruly"
#define MyAppURL "https://github.com/Antruly/uvnvr"
#define MyAppExeName "uvnvr.exe"

[Setup]
AppId={{6C3D0604-63A6-47AE-9929-0AFEE44C769F}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename=uvnvr-{#MyAppVersion}-windows-{#MyArch}-setup
SetupIconFile=..\..\resources\branding\uvnvr.ico
UninstallDisplayIcon={app}\bin\{#MyAppExeName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ShowLanguageDialog=yes
UsePreviousAppDir=yes
SetupLogging=yes
#if MyArch == "arm64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#else
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "ChineseSimplified.isl"

[CustomMessages]
english.PortsTitle=Service ports
english.PortsDescription=Choose the ports UvNVR listens on.
english.PortsHint=The web UI uses the HTTP port and RTSP clients use the RTSP port. The values are written to the startup configuration file; an existing file keeps its other settings.
english.HttpPort=HTTP / web port
english.RtspPort=RTSP port
english.PortsExisting=An existing configuration file will be kept, but its HTTP and RTSP ports are updated to the values on this page. Other settings are preserved.
english.PortUpdateFailed=The existing configuration file could not be updated automatically. Edit %ProgramData%\UvNVR\uvnvr.json and restart the UvNVR service to change the ports.
english.ServiceStartFailed=The UvNVR service could not be started. Check %ProgramData%\UvNVR\logs\service.log and the Windows Event Log, then start the service from services.msc.
english.PortInvalid=Enter a port between 1 and 65535 for both HTTP and RTSP.
english.PortConflict=The HTTP and RTSP ports must differ.
english.LaunchWeb=Open the UvNVR web UI
english.InstallService=Register UvNVR as a Windows service (recommended)
english.LaunchApp=Launch UvNVR and open the web UI
english.StartupGroup=Startup

chinesesimplified.PortsTitle=服务端口
chinesesimplified.PortsDescription=选择 UvNVR 监听的端口。
chinesesimplified.PortsHint=Web 界面使用 HTTP 端口，RTSP 客户端使用 RTSP 端口。端口会写入启动配置文件；已有文件保留其他设置。
chinesesimplified.HttpPort=HTTP / Web 端口
chinesesimplified.RtspPort=RTSP 端口
chinesesimplified.PortsExisting=检测到已有配置文件：保留其他设置，但 HTTP 与 RTSP 端口会更新为本页填写的值。
chinesesimplified.PortUpdateFailed=已有配置文件无法自动更新。请编辑 %ProgramData%\UvNVR\uvnvr.json，然后重启 UvNVR 服务以修改端口。
chinesesimplified.ServiceStartFailed=UvNVR 服务启动失败。请查看 %ProgramData%\UvNVR\logs\service.log 与 Windows 事件日志，然后在“服务”中手动启动 UvNVR。
chinesesimplified.PortInvalid=HTTP 和 RTSP 端口都必须在 1 到 65535 之间。
chinesesimplified.PortConflict=HTTP 端口与 RTSP 端口不能相同。
chinesesimplified.LaunchWeb=打开 UvNVR Web 界面
chinesesimplified.InstallService=注册为 Windows 服务（推荐正式环境）
chinesesimplified.LaunchApp=启动 UvNVR 并打开 Web 界面
chinesesimplified.StartupGroup=启动

[Tasks]
Name: "service"; Description: "{cm:InstallService}"; GroupDescription: "{cm:StartupGroup}"; Flags: checkedonce
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "uvnvr.json"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "set-ports.ps1"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName} Web UI"; Filename: "http://127.0.0.1:{code:GetHttpPort}/"; IconFilename: "{app}\bin\{#MyAppExeName}"
Name: "{group}\{#MyAppName} Configuration"; Filename: "{sys}\notepad.exe"; Parameters: """{commonappdata}\{#MyAppName}\uvnvr.json"""; IconFilename: "{app}\bin\{#MyAppExeName}"
Name: "{group}\{#MyAppName} (console)"; Filename: "{app}\bin\{#MyAppExeName}"; Parameters: "--config ""{commonappdata}\{#MyAppName}\uvnvr.json"" --open"; IconFilename: "{app}\bin\{#MyAppExeName}"; Check: IsConsoleSelected
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
; One desktop shortcut. With the service it opens the web UI (the service is
; already running); without it, the shortcut starts the console and browser.
Name: "{autodesktop}\{#MyAppName}"; Filename: "http://127.0.0.1:{code:GetHttpPort}/"; IconFilename: "{app}\bin\{#MyAppExeName}"; Tasks: desktopicon; Check: IsServiceSelected
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\bin\{#MyAppExeName}"; Parameters: "--config ""{commonappdata}\{#MyAppName}\uvnvr.json"" --open"; IconFilename: "{app}\bin\{#MyAppExeName}"; Tasks: desktopicon; Check: IsConsoleSelected

[Registry]
Root: HKLM; Subkey: "Software\UvNVR"; ValueType: string; ValueName: "InstallDir"; ValueData: "{app}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\UvNVR"; ValueType: string; ValueName: "HttpPort"; ValueData: "{code:GetHttpPort}"
Root: HKLM; Subkey: "Software\UvNVR"; ValueType: string; ValueName: "RtspPort"; ValueData: "{code:GetRtspPort}"

[Run]
Filename: "http://127.0.0.1:{code:GetHttpPort}/"; Description: "{cm:LaunchWeb}"; Flags: shellexec nowait postinstall skipifsilent; Check: IsServiceSelected
Filename: "{app}\bin\{#MyAppExeName}"; Parameters: "--config ""{commonappdata}\{#MyAppName}\uvnvr.json"" --open"; Description: "{cm:LaunchApp}"; Flags: nowait postinstall skipifsilent; Check: IsConsoleSelected

[UninstallRun]
Filename: "{sys}\schtasks.exe"; Parameters: "/Delete /TN UvNVR /F"; Flags: runhidden; RunOnceId: "DeleteTask"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""UvNVR"""; Flags: runhidden; RunOnceId: "DeleteFirewall"

[Code]
var
  PortsPage: TInputQueryWizardPage;
  HttpPort: String;
  RtspPort: String;

function GetHttpPort(Param: String): String;
begin
  Result := HttpPort;
end;

function GetRtspPort(Param: String): String;
begin
  Result := RtspPort;
end;

function IsServiceSelected: Boolean;
begin
  Result := WizardIsTaskSelected('service');
end;

function IsConsoleSelected: Boolean;
begin
  Result := not WizardIsTaskSelected('service');
end;

function ConfigPath: String;
begin
  Result := ExpandConstant('{commonappdata}\{#MyAppName}\uvnvr.json');
end;

procedure InitializeWizard;
var
  Description: String;
begin
  HttpPort := '8080';
  RtspPort := '554';
  RegQueryStringValue(HKLM, 'Software\UvNVR', 'HttpPort', HttpPort);
  RegQueryStringValue(HKLM, 'Software\UvNVR', 'RtspPort', RtspPort);

  Description := ExpandConstant('{cm:PortsDescription}');
  if FileExists(ConfigPath) then
    Description := Description + #13#10#13#10 + ExpandConstant('{cm:PortsExisting}');

  PortsPage := CreateInputQueryPage(
    wpSelectDir,
    ExpandConstant('{cm:PortsTitle}'),
    Description,
    ExpandConstant('{cm:PortsHint}'));
  PortsPage.Add(ExpandConstant('{cm:HttpPort}'), False);
  PortsPage.Add(ExpandConstant('{cm:RtspPort}'), False);
  PortsPage.Values[0] := HttpPort;
  PortsPage.Values[1] := RtspPort;

end;

function ValidatePort(S: String; out Value: Integer): Boolean;
begin
  Value := StrToIntDef(S, 0);
  Result := (Value >= 1) and (Value <= 65535);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  HttpValue, RtspValue: Integer;
begin
  Result := True;
  if CurPageID = PortsPage.ID then
  begin
    if (not ValidatePort(PortsPage.Values[0], HttpValue)) or
       (not ValidatePort(PortsPage.Values[1], RtspValue)) then
    begin
      MsgBox(ExpandConstant('{cm:PortInvalid}'), mbError, MB_OK);
      Result := False;
      exit;
    end;
    if HttpValue = RtspValue then
    begin
      MsgBox(ExpandConstant('{cm:PortConflict}'), mbError, MB_OK);
      Result := False;
      exit;
    end;
    HttpPort := IntToStr(HttpValue);
    RtspPort := IntToStr(RtspValue);
  end;
end;

procedure UpdateConfigPorts(Config: String);
var
  Cmd: String;
  ResultCode: Integer;
begin
  // The ports page is authoritative even when an older configuration
  // exists. The script rewrites only http.port and rtsp.port and keeps a
  // UTF-8 file without a BOM.
  Cmd := '-NoProfile -ExecutionPolicy Bypass -File "' +
         ExpandConstant('{app}\set-ports.ps1') + '" -Path "' + Config +
         '" -HttpPort ' + HttpPort + ' -RtspPort ' + RtspPort;
  Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Cmd, '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Log('config port update returned ' + IntToStr(ResultCode));
  if ResultCode <> 0 then
    SuppressibleMsgBox(ExpandConstant('{cm:PortUpdateFailed}'), mbError, MB_OK, IDOK);
end;

procedure ApplyServiceConfiguration;
var
  Config, ConfigDir, DataDir, AppDir, Exe, Cmd: String;
  ConfigExisted: Boolean;
  ResultCode: Integer;
begin
  ConfigDir := ExpandConstant('{commonappdata}\{#MyAppName}');
  ForceDirectories(ConfigDir);
  Config := ConfigPath;
  AppDir := ExpandConstant('{app}');
  ConfigExisted := FileExists(Config);

  if not ConfigExisted then
  begin
    if CompareText(Copy(AppDir, 1, 2), 'C:') = 0 then
      DataDir := ExpandConstant('{commonappdata}\{#MyAppName}')
    else
      DataDir := AppDir + '\data';
    StringChangeEx(DataDir, '\', '/', True);
    SaveStringToFile(Config,
      '{' + #13#10 +
      '  "http": { "port": ' + HttpPort + ', "host": "0.0.0.0" },' + #13#10 +
      '  "rtsp": { "port": ' + RtspPort + ' },' + #13#10 +
      '  "dataDir": "' + DataDir + '"' + #13#10 +
      '}' + #13#10, False);
  end
  else
    UpdateConfigPorts(Config);

  Exe := ExpandConstant('{app}\bin\{#MyAppExeName}');
  if WizardIsTaskSelected('service') then
  begin
    Cmd := 'create UvNVR binPath= "\"' + Exe + '\" --service --config \"' + Config +
           '\"" start= auto obj= LocalSystem DisplayName= "UvNVR"';
    Exec(ExpandConstant('{sys}\sc.exe'), Cmd, '', SW_HIDE,
         ewWaitUntilTerminated, ResultCode);
    Log('sc create returned ' + IntToStr(ResultCode));
    if ResultCode = 1073 then
    begin
      // Already registered (upgrade): update the binary path instead.
      Cmd := 'config UvNVR binPath= "\"' + Exe + '\" --service --config \"' + Config +
             '\"" start= auto';
      Exec(ExpandConstant('{sys}\sc.exe'), Cmd, '', SW_HIDE,
           ewWaitUntilTerminated, ResultCode);
      Log('sc config returned ' + IntToStr(ResultCode));
    end;
    Exec(ExpandConstant('{sys}\sc.exe'),
         'description UvNVR "UvNVR software network video recorder"', '', SW_HIDE,
         ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\sc.exe'), 'start UvNVR', '', SW_HIDE,
         ewWaitUntilTerminated, ResultCode);
    Log('sc start returned ' + IntToStr(ResultCode));
    if ResultCode <> 0 then
      SuppressibleMsgBox(ExpandConstant('{cm:ServiceStartFailed}'), mbError, MB_OK, IDOK);
  end
  else
  begin
    // The service task was cleared: remove a registration left by an
    // earlier version or an upgrade.
    Exec(ExpandConstant('{sys}\sc.exe'), 'stop UvNVR', '', SW_HIDE,
         ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\sc.exe'), 'delete UvNVR', '', SW_HIDE,
         ewWaitUntilTerminated, ResultCode);
    Log('sc delete returned ' + IntToStr(ResultCode));
  end;

  // 0.17 installs used a boot scheduled task; drop it so it cannot start a
  // second copy next to the service or the console shortcut.
  Exec(ExpandConstant('{sys}\schtasks.exe'), '/Delete /TN UvNVR /F', '', SW_HIDE,
       ewWaitUntilTerminated, ResultCode);
  Log('legacy task removal returned ' + IntToStr(ResultCode));

  Exec(ExpandConstant('{sys}\netsh.exe'),
       'advfirewall firewall delete rule name="UvNVR"', '', SW_HIDE,
       ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\netsh.exe'),
       'advfirewall firewall add rule name="UvNVR" dir=in action=allow program="' +
       Exe + '" enable=yes', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Log('firewall update returned ' + IntToStr(ResultCode));
end;

procedure StopExistingService;
var
  ResultCode: Integer;
begin
  // An upgrade must not overwrite a running uvnvr.exe. sc stop returns as
  // soon as the stop is pending, so give the process a moment to exit.
  Exec(ExpandConstant('{sys}\sc.exe'), 'stop UvNVR', '', SW_HIDE,
       ewWaitUntilTerminated, ResultCode);
  if ResultCode = 0 then
  begin
    Exec(ExpandConstant('{sys}\cmd.exe'), '/c ping -n 7 127.0.0.1 >nul', '',
         SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Log('service stop wait finished');
  end;
  // 0.17 used a boot scheduled task. Remove it and any console instance so
  // the old process cannot keep the previous ports bound after an upgrade.
  Exec(ExpandConstant('{sys}\schtasks.exe'), '/End /TN UvNVR', '', SW_HIDE,
       ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\schtasks.exe'), '/Delete /TN UvNVR /F', '', SW_HIDE,
       ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM uvnvr.exe', '', SW_HIDE,
       ewWaitUntilTerminated, ResultCode);
  Log('legacy process cleanup returned ' + IntToStr(ResultCode));
  Exec(ExpandConstant('{sys}\cmd.exe'), '/c ping -n 4 127.0.0.1 >nul', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    StopExistingService;
  if CurStep = ssPostInstall then
    ApplyServiceConfiguration;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    Exec(ExpandConstant('{sys}\sc.exe'),
         'stop UvNVR', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    if ResultCode = 0 then
      Exec(ExpandConstant('{sys}\cmd.exe'), '/c ping -n 7 127.0.0.1 >nul', '',
           SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\schtasks.exe'),
         '/End /TN UvNVR', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\taskkill.exe'),
         '/F /IM uvnvr.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\cmd.exe'), '/c ping -n 4 127.0.0.1 >nul', '',
         SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\sc.exe'),
         'delete UvNVR', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\schtasks.exe'),
         '/Delete /TN UvNVR /F', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'),
         'advfirewall firewall delete rule name="UvNVR"', '', SW_HIDE,
         ewWaitUntilTerminated, ResultCode);
  end;
end;
