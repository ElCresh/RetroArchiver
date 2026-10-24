; RetroArchiver-Setup.exe: installs RetroArchiver and the programs it needs
; to read floppies, CDs and DVDs on a Windows XP SP3 PC.
;
; Built with `make setup` (NSIS 3, makensis), which first checks the hashes of
; the third-party programs (installer/third-party.txt) and prepares them in
; build/third-party/, with the licence texts in build/third-party/licenses/.
; Defines coming from the Makefile:
;   ROOT      the repo root (the compiled .exe files)
;   VERSION   the version, YYMMDD of the release (e.g. 261017)
;   VIVERSION the same as a.b.c.d for the file properties (26.10.17.0)
;   OUTFILE   where to write the installer
;
; Following Windows conventions: the program in Program Files, SAMdisk and
; DiscImageCreator in its tools\ subfolder, each user's choices in
; Application Data\RetroArchiver (written by RetroArchiver.exe). Next to the
; exe there is only RetroArchiver.ini with the defaults, written from here.
; The archive of box sets goes in <system drive>:\RetroArchive.
;
; The same setup updates: it closes RetroArchiver, replaces the programs,
; and adds to RetroArchiver.ini only the keys that are missing. Whoever had
; RetroArchiver in C:\RetroArchiver gets it updated there; SAMdisk and DIC
; placed by the previous setup in C:\samdisk and C:\DIC move to tools\.
; The Visual C++ 14.27 runtime for DiscImageCreator is installed only if it is
; missing or older: the same x86 package works from XP SP3 to Windows 11.
;
; English and Italian: the setup asks at the start (Windows' language is the
; one proposed), shows its pages in that language and writes it as
; UILanguage in RetroArchiver.ini, so the program starts in the same one.
; The texts are in strings.nsh; the window's own texts in lang\*.lng.

Unicode true
SetCompressor /SOLID lzma

!ifndef ROOT
  !error "manca ROOT: compila con make setup"
!endif
!ifndef VERSION
  !define VERSION "0"
  !define VIVERSION "0.0.0.0"
!endif
!ifndef OUTFILE
  !define OUTFILE "RetroArchiver-Setup.exe"
!endif
!define THIRD_PARTY "${ROOT}/build/third-party"
!define TEXTS "${ROOT}/build/setup"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "WinMessages.nsh"
!include "WinVer.nsh"
!include "x64.nsh"
!include "${THIRD_PARTY}/third_party.nsh"

; the default archive, in the root of the system drive: the name is only
; changed here (and in SET_ARCHIVE_DIR of src/gui/settings.h)
!define ARCHIVE_NAME "RetroArchive"
; the third-party programs, inside the RetroArchiver folder
!define SAMDISK_DIR "$INSTDIR\tools\samdisk"
!define DIC_DIR     "$INSTDIR\tools\DIC"
; where the previous setup put them: removed if it was the one that put them there
!define OLD_SAMDISK_DIR "C:\samdisk"
!define OLD_DIC_DIR     "C:\DIC"
; the defaults next to the exe; the user's choices in Application Data
!define INI         "RetroArchiver.ini"
!define USER_DIR    "RetroArchiver"
!define CHECK_FILE  "install-check.txt"
!define UNINST      "Software\Microsoft\Windows\CurrentVersion\Uninstall\RetroArchiver"
!define VCREDIST_URL "https://download.visualstudio.microsoft.com/download/pr/56f631e5-4252-4f28-8ecc-257c7bf412b8/D305BAA965C9CD1B44EBCD53635EE9ECC6D85B54210E2764C8836F4E9DEFA345/VC_redist.x86.exe"
; STATUS_DLL_NOT_FOUND: DiscImageCreator without the Visual C++ runtime
!define DLL_NOT_FOUND "-1073741515"

Name "RetroArchiver"
OutFile "${OUTFILE}"
; an existing installation (even in C:\RetroArchiver) is updated in place
InstallDir "$PROGRAMFILES\RetroArchiver"
InstallDirRegKey HKLM "${UNINST}" "InstallLocation"
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show
BrandingText "RetroArchiver ${VERSION}"

VIProductVersion "${VIVERSION}"
VIAddVersionKey /LANG=1033 "ProductName" "RetroArchiver"
VIAddVersionKey /LANG=1033 "FileDescription" "RetroArchiver setup"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "SAMdisk, fdrawcmd.sys and DiscImageCreator have their own licences: THIRD-PARTY-NOTICES.txt"
VIAddVersionKey /LANG=1040 "ProductName" "RetroArchiver"
VIAddVersionKey /LANG=1040 "FileDescription" "Installazione di RetroArchiver"
VIAddVersionKey /LANG=1040 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1040 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=1040 "LegalCopyright" "SAMdisk, fdrawcmd.sys e DiscImageCreator hanno le loro licenze: THIRD-PARTY-NOTICES.txt"

; the language chosen at the start is remembered for the uninstaller and the next update
!define MUI_LANGDLL_REGISTRY_ROOT HKLM
!define MUI_LANGDLL_REGISTRY_KEY "${UNINST}"
!define MUI_LANGDLL_REGISTRY_VALUENAME "InstallerLanguage"
!define MUI_LANGDLL_ALWAYSSHOW
!define MUI_LANGDLL_WINDOWTITLE "RetroArchiver"
!define MUI_LANGDLL_INFO "Choose a language / Scegli la lingua"

Var Summary     ; text of the final page and of install-check.txt
Var FdResult      ; how the fdrawcmd.sys installation went this time
Var Runtime     ; "ok", "old", "missing" or "noucrt"
Var VcResult      ; how the runtime installation went this time
Var ArchiveDir    ; C:\RetroArchive, on the system drive
Var IniConv       ; keys converted from Italian in the defaults ini
Var IniConvUser   ; and in the ini of the user who installs

; ---- pages ----

!define MUI_ICON "${ROOT}/src/gui/retroarchiver.ico"
!define MUI_UNICON "${ROOT}/src/gui/retroarchiver.ico"
!define MUI_ABORTWARNING

!define MUI_WELCOMEPAGE_TITLE "$(L_WELCOME_TITLE)"
!define MUI_WELCOMEPAGE_TEXT "$(L_WELCOME_TEXT)"
!insertmacro MUI_PAGE_WELCOME

; informational only: no "I accept", just move on. THIRD-PARTY-NOTICES.txt
; is in English in every language
!define MUI_PAGE_HEADER_TEXT "$(L_TP_HEADER)"
!define MUI_PAGE_HEADER_SUBTEXT "$(L_TP_SUBTEXT)"
!define MUI_LICENSEPAGE_TEXT_TOP "$(L_TP_TOP)"
!define MUI_LICENSEPAGE_TEXT_BOTTOM "$(L_TP_BOTTOM)"
!define MUI_LICENSEPAGE_BUTTON "$(^NextBtn)"
!insertmacro MUI_PAGE_LICENSE "${TEXTS}/THIRD-PARTY-NOTICES.txt"

!define MUI_COMPONENTSPAGE_SMALLDESC
!insertmacro MUI_PAGE_COMPONENTS

!define MUI_DIRECTORYPAGE_TEXT_TOP "$(L_DIR_TOP)"
!insertmacro MUI_PAGE_DIRECTORY

!insertmacro MUI_PAGE_INSTFILES

!define MUI_FINISHPAGE_TITLE "$(L_FINISH_TITLE)"
!define MUI_FINISHPAGE_TEXT "$Summary"
!define MUI_FINISHPAGE_TEXT_LARGE
!define MUI_FINISHPAGE_TEXT_REBOOT "$(L_FINISH_REBOOT)"
!define MUI_FINISHPAGE_TEXT_REBOOTNOW "$(L_REBOOT_NOW)"
!define MUI_FINISHPAGE_TEXT_REBOOTLATER "$(L_REBOOT_LATER)"
!define MUI_FINISHPAGE_RUN "$INSTDIR\RetroArchiver.exe"
!define MUI_FINISHPAGE_RUN_TEXT "$(L_RUN)"
!define MUI_FINISHPAGE_RUN_FUNCTION RunUnelevated
!define MUI_PAGE_CUSTOMFUNCTION_SHOW FinishShow
!insertmacro MUI_PAGE_FINISH

!define MUI_UNCONFIRMPAGE_TEXT_TOP "$(L_UNCONFIRM)"
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

; the first one is the setup's choice when Windows speaks neither
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Italian"
!include "strings.nsh"

; after MUI_LANGUAGE: the language dialog lists the languages inserted above
Function .onInit
  !insertmacro MUI_LANGDLL_DISPLAY
  StrCpy $ArchiveDir $WINDIR 2
  StrCpy $ArchiveDir "$ArchiveDir\${ARCHIVE_NAME}"
FunctionEnd

Function un.onInit
  !insertmacro MUI_UNGETLANGUAGE
FunctionEnd

; ---- final page: the summary must not end up under the checkbox ----
; MUI puts "Apri RetroArchiver" (or the reboot choices) right below a
; fixed-height text, and the summary is longer: on the XP PC the last line
; ended up under the checkbox. Here the checkboxes move to the bottom of the
; page and the text grows by the same amount.

; $R0 = control, $R1 = shift in pixels (added to every y coordinate)
Function MoveDown
  ${If} $R0 == 0
  ${OrIf} $R0 == ""
    Return
  ${EndIf}
  System::Call '*(i,i,i,i)p.r1'
  System::Call 'user32::GetWindowRect(p $R0, p r1)'
  System::Call 'user32::MapWindowPoints(p 0, p $mui.FinishPage, p r1, i 2)'
  System::Call '*$1(i.r2,i.r3,i,i)'
  System::Free $1
  IntOp $3 $3 + $R1
  System::Call 'user32::SetWindowPos(p $R0, p 0, i r2, i r3, i 0, i 0, i 5)' ; SWP_NOSIZE|SWP_NOZORDER
FunctionEnd

; in $R2 the bottom edge of control $R0, in page coordinates
Function BottomEdge
  System::Call '*(i,i,i,i)p.r1'
  System::Call 'user32::GetWindowRect(p $R0, p r1)'
  System::Call 'user32::MapWindowPoints(p 0, p $mui.FinishPage, p r1, i 2)'
  System::Call '*$1(i,i,i,i.R2)'
  System::Free $1
FunctionEnd

Function FinishShow
  ; the last control under the text: the checkbox or "Riavvio piu' tardi"
  StrCpy $R0 $mui.FinishPage.Run
  ${If} $mui.FinishPage.RebootLater != ""
  ${AndIf} $mui.FinishPage.RebootLater != 0
    StrCpy $R0 $mui.FinishPage.RebootLater
  ${EndIf}
  ${If} $R0 == ""
  ${OrIf} $R0 == 0
    Return
  ${EndIf}
  Call BottomEdge
  System::Call '*(i,i,i,i)p.r1'
  System::Call 'user32::GetClientRect(p $mui.FinishPage, p r1)'
  System::Call '*$1(i,i,i,i.r4)'
  System::Free $1
  IntOp $R1 $4 - $R2
  IntOp $R1 $R1 - 10                ; a margin at the bottom
  ${If} $R1 <= 0
    Return
  ${EndIf}
  StrCpy $R0 $mui.FinishPage.Run
  Call MoveDown
  StrCpy $R0 $mui.FinishPage.RebootNow
  Call MoveDown
  StrCpy $R0 $mui.FinishPage.RebootLater
  Call MoveDown
  ; the text ends where the first checkbox now starts (MUI makes it as tall
  ; as it likes, even over the checkboxes: it must be shortened, not lengthened)
  StrCpy $R0 $mui.FinishPage.Run
  ${If} $mui.FinishPage.RebootNow != ""
  ${AndIf} $mui.FinishPage.RebootNow != 0
    StrCpy $R0 $mui.FinishPage.RebootNow
  ${EndIf}
  Call TopEdge
  StrCpy $7 $R2
  StrCpy $R0 $mui.FinishPage.Text
  Call TopEdge
  System::Call '*(i,i,i,i)p.r1'
  System::Call 'user32::GetWindowRect(p $mui.FinishPage.Text, p r1)'
  System::Call '*$1(i.r2,i,i.r5,i)'
  System::Free $1
  IntOp $5 $5 - $2                  ; width
  IntOp $6 $7 - $R2                 ; height: from the top of the text to the checkbox
  IntOp $6 $6 - 4
  System::Call 'user32::SetWindowPos(p $mui.FinishPage.Text, p 0, i 0, i 0, i r5, i r6, i 6)' ; SWP_NOMOVE|SWP_NOZORDER
FunctionEnd

; in $R2 the top edge of control $R0, in page coordinates
Function TopEdge
  System::Call '*(i,i,i,i)p.r1'
  System::Call 'user32::GetWindowRect(p $R0, p r1)'
  System::Call 'user32::MapWindowPoints(p 0, p $mui.FinishPage, p r1, i 2)'
  System::Call '*$1(i,i.R2,i,i)'
  System::Free $1
FunctionEnd

; the finish page opens the window through Explorer: started by the setup it
; would inherit the administrator token (seen on Windows 7 with UAC), and with
; another account's credentials it would save that account's settings. XP has
; no UAC: there it starts as before
Function RunUnelevated
  ${If} ${AtLeastWinVista}
    Exec '"$WINDIR\explorer.exe" "$INSTDIR\RetroArchiver.exe"'
  ${Else}
    Exec '"$INSTDIR\RetroArchiver.exe"'
  ${EndIf}
FunctionEnd

; ---- RetroArchiver is open: it must be closed before replacing the files ----

!macro CLOSE_APP un
Function ${un}CloseRetroArchiver
  FindWindow $0 "RetroArchiver"
  ${If} $0 = 0
    Return
  ${EndIf}
  MessageBox MB_OKCANCEL|MB_ICONINFORMATION "$(L_CLOSE_ASK)" /SD IDOK IDOK chiudi
  Abort "$(L_CLOSE_ABORT)"
chiudi:
  StrCpy $1 0
  ${Do}
    FindWindow $0 "RetroArchiver"
    ${If} $0 = 0
      DetailPrint "$(L_CLOSED)"
      Return
    ${EndIf}
    ${If} $1 = 0
      ; in front of everything: if it asks for confirmation, the question does not
      ; stay hidden behind the setup window (tried on the VM)
      System::Call 'user32::IsIconic(p r0) i .r2'
      ${If} $2 <> 0
        System::Call 'user32::ShowWindow(p r0, i 9)'
      ${EndIf}
      System::Call 'user32::SetForegroundWindow(p r0)'
      System::Call 'user32::PostMessage(p r0, i ${WM_CLOSE}, p 0, p 0)'
    ${EndIf}
    Sleep 500
    ; while it asks for confirmation the window is disabled: wait for the
    ; answer without counting the time
    System::Call 'user32::IsWindowEnabled(p r0) i .r2'
    ${If} $2 <> 0
    ${OrIf} $1 = 0
      IntOp $1 $1 + 1
    ${EndIf}
    ${If} $1 >= 20
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "$(L_CLOSE_STUCK)" /SD IDCANCEL IDRETRY chiudi
      Abort "$(L_CLOSE_STOPPED)"
    ${EndIf}
  ${Loop}
FunctionEnd
!macroend
!insertmacro CLOSE_APP ""
!insertmacro CLOSE_APP "un."

; ---- RetroArchiver.ini with the defaults ----
; Sections and keys in English, as src/gui/settings.h names them. Version
; 261017 wrote them in Italian ([Archiviazione] Cartella...): the update
; converts them with RetroArchiver.exe --convert-ini, the same code the
; window uses, before adding the defaults that are missing.

; an ini key is written only if it is missing: this way the update does not
; change the earlier choices (which today act as defaults)
!macro INI_IF_MISSING sec key value
  ClearErrors
  ReadINIStr $0 "$INSTDIR\${INI}" "${sec}" "${key}"
  ${If} ${Errors}
    WriteINIStr "$INSTDIR\${INI}" "${sec}" "${key}" "${value}"
    DetailPrint "  [${sec}] ${key}=${value}"
  ${EndIf}
!macroend

; an ini of 261017 in $R0 converted to the English keys: $R1 = keys converted
Function ConvertIni
  StrCpy $R1 0
  ${IfNot} ${FileExists} "$R0"
    Return
  ${EndIf}
  ClearErrors
  ExecWait '"$INSTDIR\RetroArchiver.exe" --convert-ini "$R0"' $R1
  ${If} ${Errors}
  ${OrIf} $R1 = 255
    StrCpy $R1 0
    DetailPrint "$(L_INI_CONV_FAIL)"
  ${ElseIf} $R1 > 0
    DetailPrint "$(L_INI_CONVERTED)"
  ${EndIf}
FunctionEnd

; the defaults next to the exe and the ini of the user who installs: the other
; users' ini are converted by the window at its first start
Function ConvertInis
  StrCpy $R0 "$INSTDIR\${INI}"
  Call ConvertIni
  StrCpy $IniConv $R1
  SetShellVarContext current
  StrCpy $R0 "$APPDATA\${USER_DIR}\${INI}"
  SetShellVarContext all
  Call ConvertIni
  StrCpy $IniConvUser $R1
FunctionEnd

; the first CD drive in $R8 ("E:"), otherwise D:
Function FirstCD
  StrCpy $R8 ""
  ${GetDrives} "CDROM" FirstCD_found
  ${If} $R8 == ""
    StrCpy $R8 "D:"
  ${EndIf}
FunctionEnd

Function FirstCD_found
  StrCpy $R8 $9 2
  Push "StopGetDrives"
FunctionEnd

Function WriteIni
  ${IfNot} ${FileExists} "$INSTDIR\${INI}"
    ; as RetroArchiver creates it: UTF-16 LE with BOM, so Windows writes it as
    ; Unicode and paths with accents are not lost
    FileOpen $0 "$INSTDIR\${INI}" w
    FileWriteByte $0 255
    FileWriteByte $0 254
    FileClose $0
    DetailPrint "$(L_INI_NEW)"
  ${Else}
    DetailPrint "$(L_INI_EXISTS)"
  ${EndIf}
  Call FirstCD
  Call ConvertInis
  !insertmacro INI_IF_MISSING "General" "ArchiveFolder" "$ArchiveDir"
  !insertmacro INI_IF_MISSING "General" "MaxReads" "3"
  !insertmacro INI_IF_MISSING "Floppy" "SAMdisk" "${SAMDISK_DIR}\samdisk.exe"
  !insertmacro INI_IF_MISSING "Floppy" "ReadFrom" "a:"
  !insertmacro INI_IF_MISSING "Optical" "DiscImageCreator" "${DIC_DIR}\DiscImageCreator.exe"
  !insertmacro INI_IF_MISSING "Optical" "Drive" "$R8"
  !insertmacro INI_IF_MISSING "Floppy" "FileName" "FLOPPY{nn}"
  !insertmacro INI_IF_MISSING "Optical" "CDFileName" "CD{nn}"
  !insertmacro INI_IF_MISSING "Optical" "DVDFileName" "DVD{nn}"
  ; empty: the CD drive's read offset (samples) for a drive DiscImageCreator
  ; does not know; written empty so that it is there to fill in
  !insertmacro INI_IF_MISSING "Optical" "DriveOffset" ""
  !insertmacro INI_IF_MISSING "Box" "CopyProtection" "auto"
  ; the program's language: the one chosen at the start of this setup, also
  ; in an update (a user's own choice in the window still wins)
  WriteINIStr "$INSTDIR\${INI}" "General" "UILanguage" "$(L_UILANG)"
  DetailPrint "  [General] UILanguage=$(L_UILANG)"
FunctionEnd

; the defaults' archive folder (in an update it may be the earlier one):
; created if missing. The uninstaller does not touch it.
Function CreateArchive
  ReadINIStr $0 "$INSTDIR\${INI}" "General" "ArchiveFolder"
  ${If} $0 == ""
    Return
  ${EndIf}
  ${If} ${FileExists} "$0\*.*"
    DetailPrint "$(L_ARCHIVE_IS)"
    Return
  ${EndIf}
  ClearErrors
  CreateDirectory "$0"
  ${If} ${Errors}
    DetailPrint "$(L_ARCHIVE_FAIL)"
  ${Else}
    DetailPrint "$(L_ARCHIVE_NEW)"
  ${EndIf}
FunctionEnd

; files from the previous setup that now have a different name
Function RemoveOldNames
  Delete "$INSTDIR\Disinstalla.exe"
  Delete "$INSTDIR\LEGGIMI.txt"
  Delete "$INSTDIR\verifica-installazione.txt"
  Delete "$INSTDIR\docs\procedura.txt"
  Delete "$SMPROGRAMS\RetroArchiver\Leggimi.lnk"
  Delete "$SMPROGRAMS\RetroArchiver\Disinstalla RetroArchiver.lnk"
FunctionEnd

; ---- checks ----

; the Visual C++ 2015-2019 x86 runtime: $Runtime = ok, old, missing or
; noucrt. On XP there is no WOW6432Node; on 64-bit Windows the registry of a
; 32-bit program ends up there by itself.
Function CheckRuntime
  Call CheckVcRuntime
  ; it also needs the Universal CRT: on XP the package copies it into
  ; system32, on 7 and 8.1 it installs update KB2999226 with wusa, which can
  ; fail (0x5 on the Windows 7 VM) while the package still exits with 0
  ${If} $Runtime == "ok"
  ${AndIfNot} ${FileExists} "$SYSDIR\ucrtbase.dll"
    StrCpy $Runtime "noucrt"
  ${EndIf}
FunctionEnd

Function CheckVcRuntime
  ClearErrors
  ReadRegDWORD $0 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x86" "Installed"
  ReadRegDWORD $1 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x86" "Major"
  ReadRegDWORD $2 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x86" "Minor"
  ${If} $0 = 1
    ${If} $1 > 14
    ${OrIf} $2 >= 27
      StrCpy $Runtime "ok"
      Return
    ${EndIf}
    StrCpy $Runtime "old"
    Return
  ${EndIf}
  ${If} ${FileExists} "$SYSDIR\vcruntime140.dll"
    StrCpy $Runtime "ok"
  ${Else}
    StrCpy $Runtime "missing"
  ${EndIf}
FunctionEnd

; has the fdrawcmd service started? $R6 = 1 or 0
Function FdrawcmdActive
  StrCpy $R0 '"$SYSDIR\sc.exe" query fdrawcmd'
  Call Launch
  StrCpy $R1 "RUNNING"
  Call WordAfter
FunctionEnd

; fdrawcmd.sys installed: $0 = 1 or 0. On 64-bit Windows this 32-bit setup
; would look in SysWOW64\drivers, while the driver is in system32\drivers
Function FdrawcmdPresent
  StrCpy $0 0
  ClearErrors
  ReadRegDWORD $1 HKLM "SYSTEM\CurrentControlSet\Services\fdrawcmd" "Start"
  ${If} ${RunningX64}
    ${DisableX64FSRedirection}
  ${EndIf}
  ${IfNot} ${Errors}
  ${AndIf} ${FileExists} "$SYSDIR\drivers\fdrawcmd.sys"
    StrCpy $0 1
  ${EndIf}
  ${If} ${RunningX64}
    ${EnableX64FSRedirection}
  ${EndIf}
FunctionEnd

; $R2 = the word that follows $R1 in $R0 (up to a space, comma or newline);
; $R6 = 1 if $R1 is there, 0 if not
Function WordAfter
  StrLen $R3 $R1
  StrCpy $R4 0
  StrCpy $R2 ""
  StrCpy $R6 0
  ${Do}
    StrCpy $R5 $R0 $R3 $R4
    ${If} $R5 == ""
      Return
    ${EndIf}
    ${If} $R5 S== $R1
      ${ExitDo}
    ${EndIf}
    IntOp $R4 $R4 + 1
  ${Loop}
  StrCpy $R6 1
  IntOp $R4 $R4 + $R3
  ${Do}
    StrCpy $R5 $R0 1 $R4
    ${If} $R5 == ""
    ${OrIf} $R5 == " "
    ${OrIf} $R5 == ","
    ${OrIf} $R5 == "$\r"
    ${OrIf} $R5 == "$\n"
    ${OrIf} $R5 == "$\t"
      ${ExitDo}
    ${EndIf}
    StrCpy $R2 "$R2$R5"
    IntOp $R4 $R4 + 1
  ${Loop}
FunctionEnd

; runs a program without a console and with input from NUL:
; DiscImageCreator with a console would stop at "Press any key"
; in: $R0 = command line; out: $R0 = output, $R1 = exit code
Function Launch
  ; no Windows error dialogs if a DLL is missing: the exit code is enough
  System::Call 'kernel32::SetErrorMode(i 0x8003) i .r9'
  nsExec::ExecToStack /TIMEOUT=20000 '"$SYSDIR\cmd.exe" /c "$R0 < NUL"'
  Pop $R1
  Pop $R0
  System::Call 'kernel32::SetErrorMode(i r9)'
FunctionEnd

Function WarnRuntime
  ${If} $VcResult != ""
  ${AndIf} $VcResult != "ok"
    StrCpy $0 "$(L_VC_FAILED)"
  ${ElseIf} $Runtime == "old"
    StrCpy $0 "$(L_VC_OLD)"
  ${ElseIf} $Runtime == "noucrt"
    StrCpy $0 "$(L_VC_NOUCRT)"
  ${Else}
    StrCpy $0 "$(L_VC_MISSING)"
  ${EndIf}
  DetailPrint "$(L_VC_DETAIL)"
  ; the package, once installed, does not try the update again: only
  ; Windows Update helps
  ${If} $Runtime == "noucrt"
    MessageBox MB_OK|MB_ICONEXCLAMATION "$(L_VC_BOX_UCRT)" /SD IDOK
  ${Else}
    MessageBox MB_OK|MB_ICONEXCLAMATION "$(L_VC_BOX)" /SD IDOK
  ${EndIf}
FunctionEnd

; ---- components ----

Section "RetroArchiver" SEC_RA
  SectionIn RO
  SetShellVarContext all
  Call CloseRetroArchiver
  Call RemoveOldNames

  SetOutPath "$INSTDIR"
  File "${ROOT}/RetroArchiver.exe"
  File "${ROOT}/dskrun.exe"
  File "${ROOT}/dskdiff.exe"
  File "${TEXTS}/README.txt"
  ; the copy protection signature files, read before each CD (Automatic)
  File "${TEXTS}/protection-signatures.txt"
  ; the window's texts: lang\<code>.lng next to the program
  SetOutPath "$INSTDIR\lang"
  File "${TEXTS}/lang/*.lng"
  SetOutPath "$INSTDIR\docs"
  File "${TEXTS}/setup.txt"
  File "${TEXTS}/procedure.txt"
  ; credits and licence texts of the third-party programs
  SetOutPath "$INSTDIR\licenses"
  File "${TEXTS}/THIRD-PARTY-NOTICES.txt"
  File "${THIRD_PARTY}/licenses/*.txt"
  SetOutPath "$INSTDIR"
  Call WriteIni
  Call CreateArchive

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "${UNINST}" "DisplayName" "RetroArchiver"
  WriteRegStr HKLM "${UNINST}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST}" "DisplayIcon" "$INSTDIR\RetroArchiver.exe"
  WriteRegStr HKLM "${UNINST}" "Publisher" "RetroArchiver"
  WriteRegStr HKLM "${UNINST}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINST}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST}" "NoRepair" 1
  ; MUI saves it only after the files page, which a silent setup skips
  WriteRegStr HKLM "${UNINST}" "${MUI_LANGDLL_REGISTRY_VALUENAME}" $LANGUAGE

  ; the shortcut starts from the program folder
  CreateDirectory "$SMPROGRAMS\RetroArchiver"
  CreateShortcut "$SMPROGRAMS\RetroArchiver\RetroArchiver.lnk" "$INSTDIR\RetroArchiver.exe"
  CreateShortcut "$SMPROGRAMS\RetroArchiver\Read me.lnk" "$INSTDIR\README.txt"
  CreateShortcut "$SMPROGRAMS\RetroArchiver\Third-party notices.lnk" "$INSTDIR\licenses\THIRD-PARTY-NOTICES.txt"
  CreateShortcut "$SMPROGRAMS\RetroArchiver\Uninstall RetroArchiver.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\RetroArchiver.lnk" "$INSTDIR\RetroArchiver.exe"
SectionEnd

Section "$(L_SEC_SAMDISK)" SEC_SAMDISK
  ; the previous setup put it in C:\samdisk: if it was the one that put it there, remove it
  ReadRegDWORD $0 HKLM "${UNINST}" "SAMdisk"
  ${If} $0 = 1
    DetailPrint "$(L_SAMDISK_MOVES)"
    !insertmacro T_samdisk_REMOVE "${OLD_SAMDISK_DIR}"
    DeleteRegValue HKLM "${UNINST}" "SAMdisk"
  ${EndIf}
  SetOutPath "${SAMDISK_DIR}"
  File /r "${THIRD_PARTY}/samdisk/*"
  SetOutPath "$INSTDIR"
SectionEnd

Section "$(L_SEC_FDRAWCMD)" SEC_FDRAWCMD
  ; it is an NSIS installer: with /S no questions. On the XP VM it took
  ; from 2 seconds to a minute and a half
  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  File "${THIRD_PARTY}/fdrawcmd/${T_fdrawcmd_FILE}"
  DetailPrint "$(L_FD_INSTALLING)"
  ClearErrors
  ExecWait '"$PLUGINSDIR\${T_fdrawcmd_FILE}" /S' $0
  ${If} ${Errors}
    StrCpy $FdResult "$(L_NOSTART)"
  ${ElseIf} $0 <> 0
    StrCpy $FdResult "$(L_FD_FAILED)"
  ${Else}
    StrCpy $FdResult "ok"
    ; a freshly installed driver usually starts only after a reboot
    Call FdrawcmdActive
    ${If} $R6 = 0
      SetRebootFlag true
    ${EndIf}
  ${EndIf}
  DetailPrint "fdrawcmd.sys: $FdResult"
  ${If} $FdResult != "ok"
    MessageBox MB_OK|MB_ICONEXCLAMATION "$(L_FD_BOX)" /SD IDOK
  ${EndIf}
  SetOutPath "$INSTDIR"
SectionEnd

; before DiscImageCreator, which needs it. Installed only if missing or
; older than 14.27: the x86 package works from XP SP3 to Windows 11,
; even 64-bit (DiscImageCreator is 32-bit). It does not work on Windows 2000.
Section "$(L_SEC_VCREDIST)" SEC_VCREDIST
  Call CheckRuntime
  ${If} $Runtime == "ok"
    DetailPrint "$(L_VC_PRESENT)"
    Return
  ${EndIf}
  ${If} ${AtMostWin2000}
    StrCpy $VcResult "$(L_VC_UNSUPPORTED)"
    DetailPrint "$(L_VC_RESULT)."
    Return
  ${EndIf}
  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  File "${THIRD_PARTY}/vcredist/${T_vcredist_FILE}"
  DetailPrint "$(L_VC_INSTALLING)"
  ClearErrors
  ExecWait '"$PLUGINSDIR\${T_vcredist_FILE}" /install /quiet /norestart' $0
  ; 0 installed, 3010 installed with reboot, 1638 a newer version of the same
  ; product is already there
  ${If} ${Errors}
    StrCpy $VcResult "$(L_NOSTART)"
  ${ElseIf} $0 = 0
  ${OrIf} $0 = 1638
    StrCpy $VcResult "ok"
  ${ElseIf} $0 = 3010
    StrCpy $VcResult "ok"
    SetRebootFlag true
  ${Else}
    StrCpy $VcResult "$(L_CODE)"
  ${EndIf}
  DetailPrint "$(L_VC_RESULT)"
  SetOutPath "$INSTDIR"
SectionEnd

Section "$(L_SEC_DIC)" SEC_DIC
  ReadRegDWORD $0 HKLM "${UNINST}" "DiscImageCreator"
  ${If} $0 = 1
    DetailPrint "$(L_DIC_MOVES)"
    !insertmacro T_dic_REMOVE "${OLD_DIC_DIR}"
    DeleteRegValue HKLM "${UNINST}" "DiscImageCreator"
  ${EndIf}
  SetOutPath "${DIC_DIR}"
  File /r "${THIRD_PARTY}/dic/*"
  SetOutPath "$INSTDIR"
  Call CheckRuntime
  ${If} $Runtime != "ok"
    Call WarnRuntime
  ${EndIf}
SectionEnd

; at the end: do the programs respond? The summary goes in the final page and
; in install-check.txt, next to the program: written by the setup, which is
; administrator, and valid for the whole installation (not for one user)
Section "-Check"
  DetailPrint "$(L_CHECK)"
  StrCpy $Summary "$(L_CHECK)"

  ${If} ${FileExists} "${SAMDISK_DIR}\samdisk.exe"
    StrCpy $R0 '"${SAMDISK_DIR}\samdisk.exe"'
    Call Launch
    StrCpy $R1 "SAMdisk "
    Call WordAfter
    ${If} $R2 == ""
      StrCpy $R2 "$(L_NOANSWER)"
    ${EndIf}
  ${Else}
    StrCpy $R2 "$(L_NOTINSTALLED)"
  ${EndIf}
  StrCpy $Summary "$Summary$\r$\n• SAMdisk: $R2"

  Call FdrawcmdPresent
  ${If} $0 = 1
    Call FdrawcmdActive
    ${If} $R6 = 1
      StrCpy $R2 "$(L_FD_ACTIVE)"
    ${Else}
      StrCpy $R2 "$(L_FD_AFTERBOOT)"
      SetRebootFlag true
    ${EndIf}
  ${ElseIf} $FdResult != ""
    StrCpy $R2 "$(L_NOTINSTALLED_WHY)"
  ${Else}
    StrCpy $R2 "$(L_NOTINSTALLED)"
  ${EndIf}
  StrCpy $Summary "$Summary$\r$\n• fdrawcmd.sys: $R2"

  Call CheckRuntime
  ${If} ${FileExists} "${DIC_DIR}\DiscImageCreator.exe"
    StrCpy $R0 '"${DIC_DIR}\DiscImageCreator.exe" /v'
    Call Launch
    ${If} $R1 == "${DLL_NOT_FOUND}"
      StrCpy $R2 "$(L_DIC_NORUNTIME)"
    ${Else}
      StrCpy $R1 "Build, "
      Call WordAfter
      StrCpy $R2 $R2 8
      ${If} $R2 == ""
        StrCpy $R2 "$(L_NOANSWER)"
      ${EndIf}
    ${EndIf}
  ${Else}
    StrCpy $R2 "$(L_NOTINSTALLED)"
  ${EndIf}
  StrCpy $Summary "$Summary$\r$\n• DiscImageCreator: $R2"

  ${If} $Runtime == "ok"
    StrCpy $R2 "$(L_RT_PRESENT)"
  ${ElseIf} $Runtime == "old"
    StrCpy $R2 "$(L_RT_OLD)"
  ${ElseIf} $Runtime == "noucrt"
    StrCpy $R2 "$(L_RT_NOUCRT)"
  ${Else}
    StrCpy $R2 "$(L_RT_MISSING)"
  ${EndIf}
  ${If} $VcResult == "ok"
  ${AndIf} $Runtime == "ok"
    StrCpy $R2 "$(L_RT_NOW)"
  ${EndIf}
  StrCpy $Summary "$Summary$\r$\n$(L_SUM_RUNTIME)"
  ReadINIStr $R2 "$INSTDIR\${INI}" "General" "ArchiveFolder"
  StrCpy $Summary "$Summary$\r$\n$(L_SUM_ARCHIVE)"
  ${If} $IniConv > 0
  ${OrIf} $IniConvUser > 0
    StrCpy $Summary "$Summary$\r$\n$(L_SUM_INICONV)"
  ${EndIf}
  ${If} $Runtime == "noucrt"
    StrCpy $Summary "$Summary$\r$\n$\r$\n$(L_SUM_NOUCRT)"
  ${ElseIf} $Runtime != "ok"
    StrCpy $Summary "$Summary$\r$\n$\r$\n$(L_SUM_NORUNTIME)"
  ${EndIf}
  StrCpy $Summary "$Summary$\r$\n$\r$\n$(L_SUM_ALSO)"

  DetailPrint "$Summary"
  FileOpen $0 "$INSTDIR\${CHECK_FILE}" w
  FileWriteWord $0 0xFEFF
  FileWriteUTF16LE $0 "RetroArchiver ${VERSION}$\r$\n$\r$\n$Summary$\r$\n"
  ${If} $Runtime != "ok"
  ${AndIf} $Runtime != "noucrt"
    FileWriteUTF16LE $0 "$\r$\n$(L_CHECK_VC)$\r$\n${VCREDIST_URL}$\r$\n"
  ${EndIf}
  FileClose $0
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_RA} "$(L_DESC_RA)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_SAMDISK} "$(L_DESC_SAMDISK)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_FDRAWCMD} "$(L_DESC_FDRAWCMD)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_VCREDIST} "$(L_DESC_VCREDIST)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_DIC} "$(L_DESC_DIC)"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ---- uninstall ----

; the choices of the user who uninstalls, in Application Data: asked first
Function un.RemoveSettings
  SetShellVarContext current
  ${If} ${FileExists} "$APPDATA\${USER_DIR}\${INI}"
    MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "$(L_UN_SETTINGS_ASK)" /SD IDNO IDNO tieni
    Delete "$APPDATA\${USER_DIR}\${INI}"
    RMDir "$APPDATA\${USER_DIR}"
    DetailPrint "$(L_UN_SETTINGS_GONE)"
  tieni:
  ${EndIf}
  SetShellVarContext all
FunctionEnd

Section "Uninstall"
  SetShellVarContext all
  Call un.CloseRetroArchiver

  Delete "$INSTDIR\RetroArchiver.exe"
  Delete "$INSTDIR\dskrun.exe"
  Delete "$INSTDIR\dskdiff.exe"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\protection-signatures.txt"
  Delete "$INSTDIR\${CHECK_FILE}"
  Delete "$INSTDIR\docs\setup.txt"
  Delete "$INSTDIR\docs\procedure.txt"
  RMDir "$INSTDIR\docs"
  ; only the languages this setup brings: a translation added by hand stays
  Delete "$INSTDIR\lang\en.lng"
  Delete "$INSTDIR\lang\it.lng"
  RMDir "$INSTDIR\lang"
  Delete "$INSTDIR\licenses\THIRD-PARTY-NOTICES.txt"
  Delete "$INSTDIR\licenses\*-LICENSE.txt"
  RMDir "$INSTDIR\licenses"
  ; the defaults belong to the installation; the user's choices do not
  Delete "$INSTDIR\${INI}"
  Call un.RemoveSettings

  ; of the third-party programs only their files: anything else next to them stays
  !insertmacro T_samdisk_REMOVE "${SAMDISK_DIR}"
  !insertmacro T_dic_REMOVE "${DIC_DIR}"
  RMDir "$INSTDIR\tools"

  Delete "$SMPROGRAMS\RetroArchiver\RetroArchiver.lnk"
  Delete "$SMPROGRAMS\RetroArchiver\Read me.lnk"
  Delete "$SMPROGRAMS\RetroArchiver\Third-party notices.lnk"
  Delete "$SMPROGRAMS\RetroArchiver\Uninstall RetroArchiver.lnk"
  RMDir "$SMPROGRAMS\RetroArchiver"
  Delete "$DESKTOP\RetroArchiver.lnk"

  DeleteRegKey HKLM "${UNINST}"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  ${If} ${FileExists} "$INSTDIR\*.*"
    DetailPrint "$(L_UN_DIR_STAYS)"
  ${EndIf}
  DetailPrint "$(L_UN_ARCHIVE_STAYS)"
  DetailPrint "$(L_UN_FD_STAYS)"
SectionEnd
