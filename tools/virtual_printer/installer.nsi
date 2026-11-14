Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"

Name "Honor Forge Virtual Printer ${VERSION}"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\Honor Forge Virtual Printer"
InstallDirRegKey HKCU "Software\HonorForge\VirtualPrinter" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "Honor Forge Virtual Printer"
VIAddVersionKey "FileDescription" "Honor Forge 虚拟打印机安装程序"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Honor Forge contributors"
!define MUI_ABORTWARNING
!define MUI_ICON "${APP_ICON}"
!define MUI_UNICON "${APP_ICON}"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\HonorForgeVirtualPrinter.exe"
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!define MUI_FINISHPAGE_RUN_TEXT "启动虚拟打印机并打开中控"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"

Function .onInit
    SetShellVarContext current
FunctionEnd

Function CheckStopped
    IfFileExists "$INSTDIR\HonorForgeVirtualPrinter.exe" 0 done
    check:
    ExecWait '"$INSTDIR\HonorForgeVirtualPrinter.exe" --check-running' $0
    ${If} $0 == 2
        IfSilent 0 prompt
        SetErrorLevel 2
        Quit
        prompt:
        MessageBox MB_RETRYCANCEL|MB_ICONINFORMATION "请先从托盘退出虚拟打印机，再重试升级。配置、G-code 和历史将保留。" IDRETRY check
        Abort
    ${EndIf}
    done:
FunctionEnd

Section "虚拟打印机"
    Call CheckStopped
    SetOutPath "$INSTDIR"
    File /r "${APP_DIR}\*.*"
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    CreateDirectory "$SMPROGRAMS\Honor Forge Virtual Printer"
    CreateShortCut "$SMPROGRAMS\Honor Forge Virtual Printer\虚拟打印机.lnk" "$INSTDIR\HonorForgeVirtualPrinter.exe"
    CreateShortCut "$DESKTOP\Honor Forge 虚拟打印机.lnk" "$INSTDIR\HonorForgeVirtualPrinter.exe"
    WriteRegStr HKCU "Software\HonorForge\VirtualPrinter" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\HonorForgeVirtualPrinter" "DisplayName" "Honor Forge Virtual Printer"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\HonorForgeVirtualPrinter" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\HonorForgeVirtualPrinter" "Publisher" "Honor Forge"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\HonorForgeVirtualPrinter" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\HonorForgeVirtualPrinter" "NoModify" 1
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\HonorForgeVirtualPrinter" "NoRepair" 1
SectionEnd

Section "Uninstall"
    SetShellVarContext current
    check:
    ExecWait '"$INSTDIR\HonorForgeVirtualPrinter.exe" --check-running' $0
    ${If} $0 == 2
        IfSilent 0 prompt
        SetErrorLevel 2
        Quit
        prompt:
        MessageBox MB_RETRYCANCEL|MB_ICONINFORMATION "请先从托盘退出虚拟打印机，再重试卸载。用户数据将保留。" IDRETRY check
        Abort
    ${EndIf}
    ReadRegStr $1 HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "HonorForgeVirtualPrinter"
    ${If} $1 == '"$INSTDIR\HonorForgeVirtualPrinter.exe" --no-browser'
        DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "HonorForgeVirtualPrinter"
    ${EndIf}
    Delete "$DESKTOP\Honor Forge 虚拟打印机.lnk"
    Delete "$SMPROGRAMS\Honor Forge Virtual Printer\虚拟打印机.lnk"
    RMDir "$SMPROGRAMS\Honor Forge Virtual Printer"
    Delete "$INSTDIR\HonorForgeVirtualPrinter.exe"
    Delete "$INSTDIR\README.md"
    Delete "$INSTDIR\使用说明.html"
    Delete "$INSTDIR\source-manifest.json"
    Delete "$INSTDIR\third-party.json"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir /r "$INSTDIR\_internal"
    RMDir /r "$INSTDIR\licenses"
    RMDir "$INSTDIR"
    DeleteRegKey HKCU "Software\HonorForge\VirtualPrinter"
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\HonorForgeVirtualPrinter"
    ; %LOCALAPPDATA%\HonorForge\VirtualPrinter is intentionally retained.
SectionEnd
