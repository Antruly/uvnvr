!include "MUI2.nsh"
!include "LogicLib.nsh"

!ifndef VERSION
  !define VERSION "0.1.0"
!endif
!ifndef SOURCE
  !define SOURCE "..\dist-packages\uvnvr-${VERSION}-windows-x64"
!endif
!ifndef ROOT
  !define ROOT ".."
!endif

Name "UvNVR ${VERSION}"
OutFile "..\dist-packages\uvnvr-${VERSION}-windows-x64-setup.exe"
InstallDir "$PROGRAMFILES64\UvNVR"
InstallDirRegKey HKLM "Software\UvNVR" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!define MUI_ABORTWARNING
!define MUI_ICON "${ROOT}\resources\branding\uvnvr.ico"
!define MUI_UNICON "${ROOT}\resources\branding\uvnvr.ico"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "SimpChinese"

Section "UvNVR" SecMain
  SetOutPath "$INSTDIR"
  File /r "${SOURCE}\*.*"

  CreateDirectory "$PROGRAMDATA\UvNVR"
  CreateDirectory "$SMPROGRAMS\UvNVR"
  CreateShortCut "$SMPROGRAMS\UvNVR\UvNVR Web UI.lnk" "http://127.0.0.1:8080/"
  CreateShortCut "$SMPROGRAMS\UvNVR\Uninstall.lnk" "$INSTDIR\uninstall.exe"

  WriteRegStr HKLM "Software\UvNVR" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\UvNVR" "DisplayName" "UvNVR ${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\UvNVR" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\UvNVR" "Publisher" "Antruly"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\UvNVR" "UninstallString" "$INSTDIR\uninstall.exe"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\UvNVR" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\UvNVR" "NoRepair" 1

  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; Run the recorder at boot. This avoids requiring a console-service wrapper.
  nsExec::ExecToLog 'schtasks /Create /TN UvNVR /TR "\"$INSTDIR\bin\uvnvr.exe\" --port 8080 --rtsp-port 554 --data-dir \"$PROGRAMDATA\\UvNVR\"" /SC ONSTART /RU SYSTEM /RL HIGHEST /F'
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="UvNVR"'
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="UvNVR" dir=in action=allow program="$INSTDIR\bin\uvnvr.exe" enable=yes'
SectionEnd

Section "Uninstall"
  nsExec::ExecToLog 'schtasks /Delete /TN UvNVR /F'
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="UvNVR"'
  Delete "$SMPROGRAMS\UvNVR\UvNVR Web UI.lnk"
  Delete "$SMPROGRAMS\UvNVR\Uninstall.lnk"
  RMDir "$SMPROGRAMS\UvNVR"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\UvNVR"
  DeleteRegKey HKLM "Software\UvNVR"
  ; $PROGRAMDATA\UvNVR is intentionally preserved.
SectionEnd
