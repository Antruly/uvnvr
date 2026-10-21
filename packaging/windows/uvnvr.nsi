!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "nsDialogs.nsh"
!include "StrFunc.nsh"

${StrRep}

!ifndef VERSION
  !define VERSION "0.10.0"
!endif
!ifndef ARCH
  !define ARCH "x64"
!endif
!ifndef SOURCE
  !define SOURCE "..\dist-packages\uvnvr-${VERSION}-windows-${ARCH}"
!endif
!ifndef ROOT
  !define ROOT ".."
!endif

Name "UvNVR ${VERSION}"
!ifndef OUTDIR
  !define OUTDIR "..\dist-packages"
!endif
OutFile "${OUTDIR}\uvnvr-${VERSION}-windows-${ARCH}-setup.exe"
InstallDir "$PROGRAMFILES64\UvNVR"
InstallDirRegKey HKLM "Software\UvNVR" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!define MUI_ABORTWARNING
!define MUI_ICON "${ROOT}\resources\branding\uvnvr.ico"
!define MUI_UNICON "${ROOT}\resources\branding\uvnvr.ico"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
Page custom PortsPageCreate PortsPageLeave
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "SimpChinese"

LangString UVNVR_PORTS_TITLE ${LANG_ENGLISH} "Service ports"
LangString UVNVR_PORTS_TITLE ${LANG_SIMPCHINESE} "服务端口"
LangString UVNVR_PORTS_INTRO ${LANG_ENGLISH} "Choose the ports UvNVR listens on. The web UI uses the HTTP port; RTSP clients use the RTSP port. Ports are written to the startup configuration file."
LangString UVNVR_PORTS_INTRO ${LANG_SIMPCHINESE} "请选择 UvNVR 监听的端口。Web 界面使用 HTTP 端口，RTSP 客户端使用 RTSP 端口。端口会写入启动配置文件。"
LangString UVNVR_PORTS_EXISTING ${LANG_ENGLISH} "An existing configuration file will be preserved. Edit it to change ports after installation."
LangString UVNVR_PORTS_EXISTING ${LANG_SIMPCHINESE} "检测到已有配置文件，安装时不会覆盖。安装后可编辑该文件修改端口。"
LangString UVNVR_HTTP_PORT ${LANG_ENGLISH} "HTTP / web port"
LangString UVNVR_HTTP_PORT ${LANG_SIMPCHINESE} "HTTP / Web 端口"
LangString UVNVR_RTSP_PORT ${LANG_ENGLISH} "RTSP port"
LangString UVNVR_RTSP_PORT ${LANG_SIMPCHINESE} "RTSP 端口"
LangString UVNVR_PORT_INVALID ${LANG_ENGLISH} "Enter a port between 1 and 65535 for both HTTP and RTSP."
LangString UVNVR_PORT_INVALID ${LANG_SIMPCHINESE} "HTTP 和 RTSP 端口都必须在 1 到 65535 之间。"
LangString UVNVR_PORT_CONFLICT ${LANG_ENGLISH} "The HTTP and RTSP ports must differ."
LangString UVNVR_PORT_CONFLICT ${LANG_SIMPCHINESE} "HTTP 端口与 RTSP 端口不能相同。"

Var HttpPort
Var RtspPort
Var HttpPortEdit
Var RtspPortEdit
Var PortsExisting
Var ProgramDataDir
Var DataDir

Function .onInit
  ReadEnvStr $ProgramDataDir "PROGRAMDATA"
  ${If} $ProgramDataDir == ""
    StrCpy $ProgramDataDir "C:\ProgramData"
  ${EndIf}
  StrCpy $HttpPort "8080"
  StrCpy $RtspPort "554"
  ReadRegStr $0 HKLM "Software\UvNVR" "HttpPort"
  ${If} $0 != ""
    StrCpy $HttpPort $0
  ${EndIf}
  ReadRegStr $0 HKLM "Software\UvNVR" "RtspPort"
  ${If} $0 != ""
    StrCpy $RtspPort $0
  ${EndIf}
  StrCpy $PortsExisting "0"
  ${If} ${FileExists} "$ProgramDataDir\UvNVR\uvnvr.json"
    StrCpy $PortsExisting "1"
  ${EndIf}
FunctionEnd

Function PortsPageCreate
  !insertmacro MUI_HEADER_TEXT "$(UVNVR_PORTS_TITLE)" "$(UVNVR_PORTS_INTRO)"
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}

  ${NSD_CreateLabel} 0 0 100% 30u "$(UVNVR_PORTS_INTRO)"
  Pop $0
  ${NSD_CreateLabel} 0 36u 100% 10u "$(UVNVR_HTTP_PORT)"
  Pop $0
  ${NSD_CreateNumber} 0 48u 90u 12u "$HttpPort"
  Pop $HttpPortEdit
  ${NSD_CreateLabel} 0 68u 100% 10u "$(UVNVR_RTSP_PORT)"
  Pop $0
  ${NSD_CreateNumber} 0 80u 90u 12u "$RtspPort"
  Pop $RtspPortEdit

  ${If} $PortsExisting == "1"
    ${NSD_CreateLabel} 0 106u 100% 20u "$(UVNVR_PORTS_EXISTING)"
    Pop $0
  ${EndIf}

  nsDialogs::Show
FunctionEnd

Function PortsPageLeave
  ${NSD_GetText} $HttpPortEdit $HttpPort
  ${NSD_GetText} $RtspPortEdit $RtspPort
  IntOp $0 $HttpPort + 0
  IntOp $1 $RtspPort + 0
  ${If} $0 < 1
  ${OrIf} $0 > 65535
  ${OrIf} $1 < 1
  ${OrIf} $1 > 65535
    MessageBox MB_ICONEXCLAMATION "$(UVNVR_PORT_INVALID)"
    Abort
  ${EndIf}
  ${If} $0 == $1
    MessageBox MB_ICONEXCLAMATION "$(UVNVR_PORT_CONFLICT)"
    Abort
  ${EndIf}
  StrCpy $HttpPort $0
  StrCpy $RtspPort $1
FunctionEnd

Section "UvNVR" SecMain
  SetOutPath "$INSTDIR"
  ; The portable config belongs to the zip only; installed builds use
  ; %ProgramData%\UvNVR\uvnvr.json so upgrades cannot shadow it.
  File /r /x uvnvr.json "${SOURCE}\*.*"

  CreateDirectory "$ProgramDataDir\UvNVR"
  ${IfNot} ${FileExists} "$ProgramDataDir\UvNVR\uvnvr.json"
    StrCpy $2 "$INSTDIR" 1
    ${If} $2 == "C"
    ${OrIf} $2 == "c"
      ${StrRep} $DataDir "$ProgramDataDir" "\" "/"
      StrCpy $DataDir "$DataDir/UvNVR"
    ${Else}
      ${StrRep} $DataDir "$INSTDIR\data" "\" "/"
    ${EndIf}
    FileOpen $1 "$ProgramDataDir\UvNVR\uvnvr.json" w
    FileWrite $1 "{$\r$\n"
    FileWrite $1 '  "http": { "port": $HttpPort, "host": "0.0.0.0" },$\r$\n'
    FileWrite $1 '  "rtsp": { "port": $RtspPort },$\r$\n'
    FileWrite $1 '  "dataDir": "$DataDir"$\r$\n'
    FileWrite $1 "}$\r$\n"
    FileClose $1
  ${EndIf}

  CreateDirectory "$SMPROGRAMS\UvNVR"
  CreateShortCut "$SMPROGRAMS\UvNVR\UvNVR Web UI.lnk" "http://127.0.0.1:$HttpPort/"
  CreateShortCut "$SMPROGRAMS\UvNVR\UvNVR Configuration.lnk" "$WINDIR\notepad.exe" '"$ProgramDataDir\UvNVR\uvnvr.json"'
  CreateShortCut "$SMPROGRAMS\UvNVR\Uninstall.lnk" "$INSTDIR\uninstall.exe"
  CreateShortCut "$DESKTOP\UvNVR Web UI.lnk" "http://127.0.0.1:$HttpPort/"

  WriteRegStr HKLM "Software\UvNVR" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\UvNVR" "HttpPort" "$HttpPort"
  WriteRegStr HKLM "Software\UvNVR" "RtspPort" "$RtspPort"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\UvNVR" "DisplayName" "UvNVR ${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\UvNVR" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\UvNVR" "Publisher" "Antruly"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\UvNVR" "UninstallString" "$INSTDIR\uninstall.exe"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\UvNVR" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\UvNVR" "NoRepair" 1

  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; Run the recorder at boot. The JSON file carries the selected ports; editing
  ; it later applies on the next service start.
  nsExec::ExecToLog 'schtasks /Create /TN UvNVR /TR "\"$INSTDIR\bin\uvnvr.exe\" --config \"$ProgramDataDir\UvNVR\uvnvr.json\"" /SC ONSTART /RU SYSTEM /RL HIGHEST /F'
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="UvNVR"'
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="UvNVR" dir=in action=allow program="$INSTDIR\bin\uvnvr.exe" enable=yes'
SectionEnd

Section "Uninstall"
  nsExec::ExecToLog 'schtasks /Delete /TN UvNVR /F'
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="UvNVR"'
  Delete "$SMPROGRAMS\UvNVR\UvNVR Web UI.lnk"
  Delete "$SMPROGRAMS\UvNVR\UvNVR Configuration.lnk"
  Delete "$SMPROGRAMS\UvNVR\Uninstall.lnk"
  Delete "$DESKTOP\UvNVR Web UI.lnk"
  RMDir "$SMPROGRAMS\UvNVR"
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\share"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\THIRD_PARTY_NOTICES.md"
  Delete "$INSTDIR\uvnvr.json"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\UvNVR"
  DeleteRegKey HKLM "Software\UvNVR"
  ; $ProgramDataDir\UvNVR is intentionally preserved, including uvnvr.json.
SectionEnd
