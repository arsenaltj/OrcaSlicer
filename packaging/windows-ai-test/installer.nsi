Unicode True

!ifndef PAYLOAD_DIR
    !error "PAYLOAD_DIR is required"
!endif
!ifndef OUTPUT_FILE
    !error "OUTPUT_FILE is required"
!endif
!ifndef VERSION_TAG
    !define VERSION_TAG "internal-beta"
!endif
!ifndef ICON_FILE
    !error "ICON_FILE is required"
!endif

!include "MUI2.nsh"

Name "OrcaSlicer AI 内部测试版"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\OrcaSlicer AI Beta"
InstallDirRegKey HKCU "Software\OrcaSlicerAIBeta" "InstallLocation"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "OrcaSlicer AI ${VERSION_TAG}"

VIProductVersion "2.5.0.0"
VIAddVersionKey /LANG=2052 "ProductName" "OrcaSlicer AI 内部测试版"
VIAddVersionKey /LANG=2052 "ProductVersion" "${VERSION_TAG}"
VIAddVersionKey /LANG=2052 "FileVersion" "${VERSION_TAG}"
VIAddVersionKey /LANG=2052 "CompanyName" "OrcaSlicer AI Internal"
VIAddVersionKey /LANG=2052 "FileDescription" "OrcaSlicer AI Windows x64 安装程序"
VIAddVersionKey /LANG=2052 "LegalCopyright" "OrcaSlicer contributors"

!define MUI_ABORTWARNING
!define MUI_ICON "${ICON_FILE}"
!define MUI_UNICON "${ICON_FILE}"
!define MUI_FINISHPAGE_RUN "$INSTDIR\03-start-orcaslicer-ai.bat"
!define MUI_FINISHPAGE_RUN_TEXT "启动 OrcaSlicer AI"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"

Section "OrcaSlicer AI" SecMain
    SetShellVarContext current
    SetOutPath "$INSTDIR"

    ; Preserve a colleague's local config only for developer packages. An
    ; internal preconfigured build must replace stale credentials on upgrade.
!ifndef PRECONFIGURED_CREDENTIALS
    InitPluginsDir
    IfFileExists "$INSTDIR\setup\ai-config.bat" 0 +2
        CopyFiles /SILENT "$INSTDIR\setup\ai-config.bat" "$PLUGINSDIR\ai-config.bat"
!endif

    SetOverwrite on
    File /r "${PAYLOAD_DIR}\*.*"

!ifndef PRECONFIGURED_CREDENTIALS
    IfFileExists "$PLUGINSDIR\ai-config.bat" 0 +2
        CopyFiles /SILENT "$PLUGINSDIR\ai-config.bat" "$INSTDIR\setup\ai-config.bat"
!endif

    WriteUninstaller "$INSTDIR\Uninstall.exe"

    CreateDirectory "$SMPROGRAMS\OrcaSlicer AI Beta"
    CreateShortcut "$SMPROGRAMS\OrcaSlicer AI Beta\启动 OrcaSlicer AI.lnk" "$INSTDIR\03-start-orcaslicer-ai.bat" "" "$INSTDIR\OrcaSlicer\orca-slicer.exe" 0
    CreateShortcut "$SMPROGRAMS\OrcaSlicer AI Beta\配置 AI 服务.lnk" "$INSTDIR\01-configure-ai.bat"
    CreateShortcut "$SMPROGRAMS\OrcaSlicer AI Beta\卸载.lnk" "$INSTDIR\Uninstall.exe"
    CreateShortcut "$DESKTOP\OrcaSlicer AI Beta.lnk" "$INSTDIR\03-start-orcaslicer-ai.bat" "" "$INSTDIR\OrcaSlicer\orca-slicer.exe" 0

    WriteRegStr HKCU "Software\OrcaSlicerAIBeta" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\OrcaSlicerAIBeta" "DisplayName" "OrcaSlicer AI 内部测试版"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\OrcaSlicerAIBeta" "DisplayVersion" "${VERSION_TAG}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\OrcaSlicerAIBeta" "Publisher" "OrcaSlicer AI Internal"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\OrcaSlicerAIBeta" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\OrcaSlicerAIBeta" "DisplayIcon" "$INSTDIR\OrcaSlicer\orca-slicer.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\OrcaSlicerAIBeta" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\OrcaSlicerAIBeta" "NoModify" 1
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\OrcaSlicerAIBeta" "NoRepair" 1
SectionEnd

Section "Uninstall"
    SetShellVarContext current

    IfFileExists "$INSTDIR\tools\ai\stop_orca_ai_sidecar.ps1" 0 +2
        ExecWait '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\tools\ai\stop_orca_ai_sidecar.ps1" -Endpoint "http://127.0.0.1:18764"'

    Delete "$DESKTOP\OrcaSlicer AI Beta.lnk"
    RMDir /r "$SMPROGRAMS\OrcaSlicer AI Beta"

    RMDir /r "$INSTDIR\OrcaSlicer"
    RMDir /r "$INSTDIR\tools"
    RMDir /r "$INSTDIR\runtime"
    Delete "$INSTDIR\01-configure-ai.bat"
    Delete "$INSTDIR\02-check-environment.bat"
    Delete "$INSTDIR\03-start-orcaslicer-ai.bat"
    Delete "$INSTDIR\04-stop-ai.bat"
    Delete "$INSTDIR\extract-package.bat"
    Delete "$INSTDIR\README-zh-CN.txt"
    Delete "$INSTDIR\BUILD-INFO.txt"
    Delete "$INSTDIR\installer.nsi"
    Delete "$INSTDIR\setup\Check-Environment.ps1"
    Delete "$INSTDIR\setup\ai-config.bat"
    Delete "$INSTDIR\setup\ai-config.example.bat"
    Delete "$INSTDIR\setup\preconfigured-ai-credentials.marker"
    RMDir "$INSTDIR\setup"
    ; Keep generated models when it contains user data; remove it only when empty.
    RMDir "$INSTDIR\generated_models"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"

    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\OrcaSlicerAIBeta"
    DeleteRegKey HKCU "Software\OrcaSlicerAIBeta"
SectionEnd
