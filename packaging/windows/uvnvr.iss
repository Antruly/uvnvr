; UvNVR Windows installer (Inno Setup 6.5+)
; Build:
;   ISCC.exe /DMyAppVersion=0.17.0 /DMyArch=x64 /DSourceDir=<stage> packaging\windows\uvnvr.iss

#ifndef MyAppVersion
  #define MyAppVersion "0.17.0"
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
english.PortsHint=The web UI uses the HTTP port and RTSP clients use the RTSP port. The values are written to the startup configuration file. An existing file is preserved.
english.HttpPort=HTTP / web port
english.RtspPort=RTSP port
english.PortsExisting=An existing configuration file will be preserved. Edit it to change ports after installation.
english.PortInvalid=Enter a port between 1 and 65535 for both HTTP and RTSP.
english.PortConflict=The HTTP and RTSP ports must differ.
english.LaunchWeb=Open the UvNVR web UI

chinesesimplified.PortsTitle=服务端口
chinesesimplified.PortsDescription=选择 UvNVR 监听的端口。
chinesesimplified.PortsHint=Web 界面使用 HTTP 端口，RTSP 客户端使用 RTSP 端口。端口会写入启动配置文件；已有文件不会被覆盖。
chinesesimplified.HttpPort=HTTP / Web 端口
chinesesimplified.RtspPort=RTSP 端口
chinesesimplified.PortsExisting=检测到已有配置文件，安装时不会覆盖。安装后可编辑该文件修改端口。
chinesesimplified.PortInvalid=HTTP 和 RTSP 端口都必须在 1 到 65535 之间。
chinesesimplified.PortConflict=HTTP 端口与 RTSP 端口不能相同。
chinesesimplified.LaunchWeb=打开 UvNVR Web 界面

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "uvnvr.json"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\{#MyAppName} Web UI"; Filename: "http://127.0.0.1:{code:GetHttpPort}/"; IconFilename: "{app}\bin\{#MyAppExeName}"
Name: "{group}\{#MyAppName} Configuration"; Filename: "{sys}\notepad.exe"; Parameters: """{commonappdata}\{#MyAppName}\uvnvr.json"""; IconFilename: "{app}\bin\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName} Web UI"; Filename: "http://127.0.0.1:{code:GetHttpPort}/"; IconFilename: "{app}\bin\{#MyAppExeName}"; Tasks: desktopicon
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\bin\{#MyAppExeName}"; Parameters: "--config ""{commonappdata}\{#MyAppName}\uvnvr.json"""; IconFilename: "{app}\bin\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
Root: HKLM; Subkey: "Software\UvNVR"; ValueType: string; ValueName: "InstallDir"; ValueData: "{app}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\UvNVR"; ValueType: string; ValueName: "HttpPort"; ValueData: "{code:GetHttpPort}"
Root: HKLM; Subkey: "Software\UvNVR"; ValueType: string; ValueName: "RtspPort"; ValueData: "{code:GetRtspPort}"

[Run]
Filename: "http://127.0.0.1:{code:GetHttpPort}/"; Description: "{cm:LaunchWeb}"; Flags: shellexec nowait postinstall skipifsilent

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

function ConfigPath: String;
begin
  Result := ExpandConstant('{commonappdata}\{#MyAppName}\uvnvr.json');
end;

procedure InitializeWizard;
begin
  HttpPort := '8080';
  RtspPort := '554';
  RegQueryStringValue(HKLM, 'Software\UvNVR', 'HttpPort', HttpPort);
  RegQueryStringValue(HKLM, 'Software\UvNVR', 'RtspPort', RtspPort);

  PortsPage := CreateInputQueryPage(
    wpSelectDir,
    ExpandConstant('{cm:PortsTitle}'),
    ExpandConstant('{cm:PortsDescription}'),
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

procedure ApplyServiceConfiguration;
var
  Config, ConfigDir, DataDir, AppDir, Exe, Cmd: String;
  ResultCode: Integer;
begin
  ConfigDir := ExpandConstant('{commonappdata}\{#MyAppName}');
  ForceDirectories(ConfigDir);
  Config := ConfigPath;
  AppDir := ExpandConstant('{app}');

  if not FileExists(Config) then
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
  end;

  Exe := ExpandConstant('{app}\bin\{#MyAppExeName}');
  Cmd := '/Create /TN UvNVR /TR "\"' + Exe + '\" --config \"' + Config +
         '\"" /SC ONSTART /RU SYSTEM /RL HIGHEST /F';
  Exec(ExpandConstant('{sys}\schtasks.exe'), Cmd, '', SW_HIDE,
       ewWaitUntilTerminated, ResultCode);
  Log('schtasks create returned ' + IntToStr(ResultCode));

  Exec(ExpandConstant('{sys}\netsh.exe'),
       'advfirewall firewall delete rule name="UvNVR"', '', SW_HIDE,
       ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\netsh.exe'),
       'advfirewall firewall add rule name="UvNVR" dir=in action=allow program="' +
       Exe + '" enable=yes', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Log('firewall update returned ' + IntToStr(ResultCode));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    ApplyServiceConfiguration;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    Exec(ExpandConstant('{sys}\schtasks.exe'),
         '/Delete /TN UvNVR /F', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\netsh.exe'),
         'advfirewall firewall delete rule name="UvNVR"', '', SW_HIDE,
         ewWaitUntilTerminated, ResultCode);
  end;
end;
