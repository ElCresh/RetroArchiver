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
VIAddVersionKey /LANG=1040 "ProductName" "RetroArchiver"
VIAddVersionKey /LANG=1040 "FileDescription" "Installazione di RetroArchiver"
VIAddVersionKey /LANG=1040 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1040 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=1040 "LegalCopyright" "SAMdisk, fdrawcmd.sys e DiscImageCreator hanno le loro licenze: THIRD-PARTY-NOTICES.txt"

Var Summary     ; text of the final page and of install-check.txt
Var FdResult      ; how the fdrawcmd.sys installation went this time
Var Runtime     ; "ok", "old" or "missing"
Var VcResult      ; how the runtime installation went this time
Var ArchiveDir    ; C:\RetroArchive, on the system drive

Function .onInit
  StrCpy $ArchiveDir $WINDIR 2
  StrCpy $ArchiveDir "$ArchiveDir\${ARCHIVE_NAME}"
FunctionEnd

; ---- pages ----

!define MUI_ICON "${ROOT}/src/gui/retroarchiver.ico"
!define MUI_UNICON "${ROOT}/src/gui/retroarchiver.ico"
!define MUI_ABORTWARNING

!define MUI_WELCOMEPAGE_TITLE "Installazione di RetroArchiver"
!define MUI_WELCOMEPAGE_TEXT "Installa RetroArchiver e i programmi che servono per archiviare floppy, CD e DVD:$\r$\n$\r$\n  • SAMdisk ${T_samdisk_VERSION}$\r$\n  • il driver fdrawcmd.sys ${T_fdrawcmd_VERSION}$\r$\n  • DiscImageCreator ${T_dic_VERSION}$\r$\n  • il runtime di Visual C++ ${T_vcredist_VERSION}, solo se manca$\r$\n$\r$\nSe RetroArchiver è già installato lo aggiorna: le impostazioni e l'archivio restano come sono."
!insertmacro MUI_PAGE_WELCOME

; informational only: no "Accetto", just move on
!define MUI_PAGE_HEADER_TEXT "Programmi di terzi"
!define MUI_PAGE_HEADER_SUBTEXT "Crediti e licenze dei programmi installati con RetroArchiver."
!define MUI_LICENSEPAGE_TEXT_TOP "Premi Pagina giù per leggere il resto (il testo è in inglese)."
!define MUI_LICENSEPAGE_TEXT_BOTTOM "Si installano come li distribuiscono gli autori; le licenze complete vanno nella cartella licenses. Il runtime di Visual C++, se manca, si installa in silenzio: installarlo vuol dire accettare le condizioni di licenza di Microsoft."
!define MUI_LICENSEPAGE_BUTTON "$(^NextBtn)"
!insertmacro MUI_PAGE_LICENSE "${TEXTS}/THIRD-PARTY-NOTICES.txt"

!define MUI_COMPONENTSPAGE_SMALLDESC
!insertmacro MUI_PAGE_COMPONENTS

!define MUI_DIRECTORYPAGE_TEXT_TOP "In questa cartella va RetroArchiver, con SAMdisk e DiscImageCreator nella sottocartella tools. Le impostazioni di ogni utente vanno nella sua cartella Dati applicazioni.$\r$\n$\r$\nL'archivio dei cofanetti va in $ArchiveDir; si sposta dalla finestra di RetroArchiver (meglio un disco NTFS capiente)."
!insertmacro MUI_PAGE_DIRECTORY

!insertmacro MUI_PAGE_INSTFILES

!define MUI_FINISHPAGE_TITLE "RetroArchiver è installato"
!define MUI_FINISHPAGE_TEXT "$Summary"
!define MUI_FINISHPAGE_TEXT_LARGE
!define MUI_FINISHPAGE_TEXT_REBOOT "$Summary$\r$\n$\r$\nfdrawcmd.sys funziona dopo un riavvio del PC."
!define MUI_FINISHPAGE_TEXT_REBOOTNOW "Riavvia adesso"
!define MUI_FINISHPAGE_TEXT_REBOOTLATER "Riavvio più tardi"
!define MUI_FINISHPAGE_RUN "$INSTDIR\RetroArchiver.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Apri RetroArchiver"
!define MUI_PAGE_CUSTOMFUNCTION_SHOW FinishShow
!insertmacro MUI_PAGE_FINISH

!define MUI_UNCONFIRMPAGE_TEXT_TOP "Tolgo RetroArchiver, con SAMdisk e DiscImageCreator. Restano l'archivio, il driver fdrawcmd.sys (ha la sua voce in Installazione applicazioni) e il runtime di Visual C++."
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "Italian"

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

; ---- RetroArchiver is open: it must be closed before replacing the files ----

!macro CLOSE_APP un
Function ${un}CloseRetroArchiver
  FindWindow $0 "RetroArchiver"
  ${If} $0 = 0
    Return
  ${EndIf}
  MessageBox MB_OKCANCEL|MB_ICONINFORMATION "RetroArchiver è aperto: premi OK e lo chiudo.$\r$\n$\r$\nSe sta leggendo un disco ti chiede se lasciare il cofanetto aperto: rispondi Sì, si riprende dopo dall'archivio." /SD IDOK IDOK chiudi
  Abort "RetroArchiver è ancora aperto: niente è stato cambiato."
chiudi:
  StrCpy $1 0
  ${Do}
    FindWindow $0 "RetroArchiver"
    ${If} $0 = 0
      DetailPrint "RetroArchiver chiuso."
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
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "RetroArchiver non si è chiuso. Chiudilo tu e premi Riprova." /SD IDCANCEL IDRETRY chiudi
      Abort "RetroArchiver è ancora aperto: installazione interrotta."
    ${EndIf}
  ${Loop}
FunctionEnd
!macroend
!insertmacro CLOSE_APP ""
!insertmacro CLOSE_APP "un."

; ---- RetroArchiver.ini with the defaults ----

; an ini key is written only if it is missing: this way the update does not
; change the earlier choices (which today act as defaults)
!macro INI_IF_MISSING key value
  ClearErrors
  ReadINIStr $0 "$INSTDIR\${INI}" "Archiviazione" "${key}"
  ${If} ${Errors}
    WriteINIStr "$INSTDIR\${INI}" "Archiviazione" "${key}" "${value}"
    DetailPrint "  ${key}=${value}"
  ${EndIf}
!macroend

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
    DetailPrint "Creo ${INI} con i default:"
  ${Else}
    DetailPrint "${INI} c'è già: aggiungo solo i default che mancano."
  ${EndIf}
  Call FirstCD
  !insertmacro INI_IF_MISSING "Cartella" "$ArchiveDir"
  !insertmacro INI_IF_MISSING "LettureMax" "3"
  !insertmacro INI_IF_MISSING "SAMdisk" "${SAMDISK_DIR}\samdisk.exe"
  !insertmacro INI_IF_MISSING "LeggiDa" "a:"
  !insertmacro INI_IF_MISSING "DiscImageCreator" "${DIC_DIR}\DiscImageCreator.exe"
  !insertmacro INI_IF_MISSING "LettoreCD" "$R8"
  !insertmacro INI_IF_MISSING "NomeFloppy" "FLOPPY{nn}"
  !insertmacro INI_IF_MISSING "NomeCD" "CD{nn}"
  !insertmacro INI_IF_MISSING "NomeDVD" "DVD{nn}"
FunctionEnd

; the defaults' archive folder (in an update it may be the earlier one):
; created if missing. The uninstaller does not touch it.
Function CreateArchive
  ReadINIStr $0 "$INSTDIR\${INI}" "Archiviazione" "Cartella"
  ${If} $0 == ""
    Return
  ${EndIf}
  ${If} ${FileExists} "$0\*.*"
    DetailPrint "Archivio: $0"
    Return
  ${EndIf}
  ClearErrors
  CreateDirectory "$0"
  ${If} ${Errors}
    DetailPrint "Non riesco a creare la cartella dell'archivio $0: si sceglie dalla finestra di RetroArchiver."
  ${Else}
    DetailPrint "Creo la cartella dell'archivio: $0"
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

; the Visual C++ 2015-2019 x86 runtime: $Runtime = ok, old or missing.
; On XP there is no WOW6432Node; on 64-bit Windows the registry of a
; 32-bit program ends up there by itself.
Function CheckRuntime
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

; fdrawcmd.sys installed: $0 = 1 or 0
Function FdrawcmdPresent
  StrCpy $0 0
  ClearErrors
  ReadRegDWORD $1 HKLM "SYSTEM\CurrentControlSet\Services\fdrawcmd" "Start"
  ${IfNot} ${Errors}
  ${AndIf} ${FileExists} "$SYSDIR\drivers\fdrawcmd.sys"
    StrCpy $0 1
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
    StrCpy $0 "Non sono riuscito a installare il runtime di Visual C++ 2015-2019 x86 versione 14.27 ($VcResult)"
  ${ElseIf} $Runtime == "old"
    StrCpy $0 "Il runtime di Visual C++ 2015-2019 x86 installato è più vecchio della versione 14.27"
  ${Else}
    StrCpy $0 "Manca il runtime di Visual C++ 2015-2019 x86 versione 14.27"
  ${EndIf}
  DetailPrint "$0: senza, DiscImageCreator non parte."
  MessageBox MB_OK|MB_ICONEXCLAMATION "$0, che serve a DiscImageCreator: senza, i CD e i DVD non si leggono (i floppy sì).$\r$\n$\r$\nRiprova lanciando di nuovo questo setup, oppure installalo a mano: VC_redist.x86.exe versione 14.27.29114 dal sito Microsoft:$\r$\n$\r$\n${VCREDIST_URL}$\r$\n$\r$\nIl link è anche in $INSTDIR\${CHECK_FILE}." /SD IDOK
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

  ; the shortcut starts from the program folder
  CreateDirectory "$SMPROGRAMS\RetroArchiver"
  CreateShortcut "$SMPROGRAMS\RetroArchiver\RetroArchiver.lnk" "$INSTDIR\RetroArchiver.exe"
  CreateShortcut "$SMPROGRAMS\RetroArchiver\Read me.lnk" "$INSTDIR\README.txt"
  CreateShortcut "$SMPROGRAMS\RetroArchiver\Third-party notices.lnk" "$INSTDIR\licenses\THIRD-PARTY-NOTICES.txt"
  CreateShortcut "$SMPROGRAMS\RetroArchiver\Uninstall RetroArchiver.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\RetroArchiver.lnk" "$INSTDIR\RetroArchiver.exe"
SectionEnd

Section "SAMdisk ${T_samdisk_VERSION} (floppy)" SEC_SAMDISK
  ; the previous setup put it in C:\samdisk: if it was the one that put it there, remove it
  ReadRegDWORD $0 HKLM "${UNINST}" "SAMdisk"
  ${If} $0 = 1
    DetailPrint "SAMdisk passa da ${OLD_SAMDISK_DIR} a ${SAMDISK_DIR}."
    !insertmacro T_samdisk_REMOVE "${OLD_SAMDISK_DIR}"
    DeleteRegValue HKLM "${UNINST}" "SAMdisk"
  ${EndIf}
  SetOutPath "${SAMDISK_DIR}"
  File /r "${THIRD_PARTY}/samdisk/*"
  SetOutPath "$INSTDIR"
SectionEnd

Section "fdrawcmd.sys ${T_fdrawcmd_VERSION} (driver per i floppy)" SEC_FDRAWCMD
  ; it is an NSIS installer: with /S no questions. On the XP VM it took
  ; from 2 seconds to a minute and a half
  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  File "${THIRD_PARTY}/fdrawcmd/${T_fdrawcmd_FILE}"
  DetailPrint "Installo fdrawcmd.sys (${T_fdrawcmd_FILE} /S)..."
  ClearErrors
  ExecWait '"$PLUGINSDIR\${T_fdrawcmd_FILE}" /S' $0
  ${If} ${Errors}
    StrCpy $FdResult "non parte"
  ${ElseIf} $0 <> 0
    StrCpy $FdResult "non riuscita (codice $0)"
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
    MessageBox MB_OK|MB_ICONEXCLAMATION "L'installazione di fdrawcmd.sys non è riuscita ($FdResult). Senza, SAMdisk non legge i floppy.$\r$\n$\r$\nRiprova lanciando di nuovo questo setup, oppure installalo a mano da ${T_fdrawcmd_URL}" /SD IDOK
  ${EndIf}
  SetOutPath "$INSTDIR"
SectionEnd

; before DiscImageCreator, which needs it. Installed only if missing or
; older than 14.27: the x86 package works from XP SP3 to Windows 11,
; even 64-bit (DiscImageCreator is 32-bit). It does not work on Windows 2000.
Section "Runtime di Visual C++ ${T_vcredist_VERSION} (per i CD e DVD)" SEC_VCREDIST
  Call CheckRuntime
  ${If} $Runtime == "ok"
    DetailPrint "Runtime di Visual C++ 14.27 o più nuovo: c'è già."
    Return
  ${EndIf}
  ${If} ${AtMostWin2000}
    StrCpy $VcResult "non va su questa versione di Windows"
    DetailPrint "Runtime di Visual C++: $VcResult."
    Return
  ${EndIf}
  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  File "${THIRD_PARTY}/vcredist/${T_vcredist_FILE}"
  DetailPrint "Installo il runtime di Visual C++ ${T_vcredist_VERSION}..."
  ClearErrors
  ExecWait '"$PLUGINSDIR\${T_vcredist_FILE}" /install /quiet /norestart' $0
  ; 0 installed, 3010 installed with reboot, 1638 a newer version of the same
  ; product is already there
  ${If} ${Errors}
    StrCpy $VcResult "non parte"
  ${ElseIf} $0 = 0
  ${OrIf} $0 = 1638
    StrCpy $VcResult "ok"
  ${ElseIf} $0 = 3010
    StrCpy $VcResult "ok"
    SetRebootFlag true
  ${Else}
    StrCpy $VcResult "codice $0"
  ${EndIf}
  DetailPrint "Runtime di Visual C++: $VcResult"
  SetOutPath "$INSTDIR"
SectionEnd

Section "DiscImageCreator ${T_dic_VERSION} (CD e DVD)" SEC_DIC
  ReadRegDWORD $0 HKLM "${UNINST}" "DiscImageCreator"
  ${If} $0 = 1
    DetailPrint "DiscImageCreator passa da ${OLD_DIC_DIR} a ${DIC_DIR}."
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
  DetailPrint "Verifica:"
  StrCpy $Summary "Verifica:"

  ${If} ${FileExists} "${SAMDISK_DIR}\samdisk.exe"
    StrCpy $R0 '"${SAMDISK_DIR}\samdisk.exe"'
    Call Launch
    StrCpy $R1 "SAMdisk "
    Call WordAfter
    ${If} $R2 == ""
      StrCpy $R2 "non risponde"
    ${EndIf}
  ${Else}
    StrCpy $R2 "non installato"
  ${EndIf}
  StrCpy $Summary "$Summary$\r$\n• SAMdisk: $R2"

  Call FdrawcmdPresent
  ${If} $0 = 1
    Call FdrawcmdActive
    ${If} $R6 = 1
      StrCpy $R2 "installato e attivo"
    ${Else}
      StrCpy $R2 "installato, si attiva dopo il riavvio"
      SetRebootFlag true
    ${EndIf}
  ${ElseIf} $FdResult != ""
    StrCpy $R2 "non installato ($FdResult)"
  ${Else}
    StrCpy $R2 "non installato"
  ${EndIf}
  StrCpy $Summary "$Summary$\r$\n• fdrawcmd.sys: $R2"

  Call CheckRuntime
  ${If} ${FileExists} "${DIC_DIR}\DiscImageCreator.exe"
    StrCpy $R0 '"${DIC_DIR}\DiscImageCreator.exe" /v'
    Call Launch
    ${If} $R1 == "${DLL_NOT_FOUND}"
      StrCpy $R2 "manca il runtime di Visual C++"
    ${Else}
      StrCpy $R1 "Build, "
      Call WordAfter
      StrCpy $R2 $R2 8
      ${If} $R2 == ""
        StrCpy $R2 "non risponde"
      ${EndIf}
    ${EndIf}
  ${Else}
    StrCpy $R2 "non installato"
  ${EndIf}
  StrCpy $Summary "$Summary$\r$\n• DiscImageCreator: $R2"

  ${If} $Runtime == "ok"
    StrCpy $R2 "presente"
  ${ElseIf} $Runtime == "old"
    StrCpy $R2 "più vecchio della 14.27"
  ${Else}
    StrCpy $R2 "manca"
  ${EndIf}
  ${If} $VcResult == "ok"
    StrCpy $R2 "$R2 (installato ora)"
  ${EndIf}
  StrCpy $Summary "$Summary$\r$\n• runtime di Visual C++ 14.27: $R2"
  ReadINIStr $R2 "$INSTDIR\${INI}" "Archiviazione" "Cartella"
  StrCpy $Summary "$Summary$\r$\n• archivio: $R2"
  ${If} $Runtime != "ok"
    StrCpy $Summary "$Summary$\r$\n$\r$\nSenza il runtime i CD e i DVD non si leggono. Rilancia questo setup, oppure installa a mano VC_redist.x86.exe 14.27.29114: il link è in ${CHECK_FILE}."
  ${EndIf}
  StrCpy $Summary "$Summary$\r$\n$\r$\nQuesto riepilogo è anche in $INSTDIR\${CHECK_FILE}."

  DetailPrint "$Summary"
  FileOpen $0 "$INSTDIR\${CHECK_FILE}" w
  FileWriteWord $0 0xFEFF
  FileWriteUTF16LE $0 "RetroArchiver ${VERSION}$\r$\n$\r$\n$Summary$\r$\n"
  ${If} $Runtime != "ok"
    FileWriteUTF16LE $0 "$\r$\nRuntime di Visual C++ 2015-2019 x86 versione 14.27.29114 (l'ultima che va su XP):$\r$\n${VCREDIST_URL}$\r$\n"
  ${EndIf}
  FileClose $0
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_RA} "La finestra di RetroArchiver, dskrun e dskdiff, il Leggimi, le guide e le licenze dei programmi di terzi. Obbligatorio."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_SAMDISK} "Legge i floppy. Nella cartella di RetroArchiver, in tools\samdisk."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_FDRAWCMD} "Il driver con cui SAMdisk parla con il controller floppy. Dopo l'installazione serve un riavvio."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_VCREDIST} "Serve a DiscImageCreator. Si installa solo se manca o è più vecchio della 14.27; se c'è già non lo tocco."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_DIC} "Legge i CD e i DVD. Nella cartella di RetroArchiver, in tools\DIC. Ha bisogno del runtime di Visual C++ 14.27."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ---- uninstall ----

; the choices of the user who uninstalls, in Application Data: asked first
Function un.RemoveSettings
  SetShellVarContext current
  ${If} ${FileExists} "$APPDATA\${USER_DIR}\${INI}"
    MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "Tolgo anche le tue impostazioni di RetroArchiver?$\r$\n($APPDATA\${USER_DIR})$\r$\n$\r$\nSe reinstalli RetroArchiver, con le impostazioni ritrovi la cartella dell'archivio e le altre scelte." /SD IDNO IDNO tieni
    Delete "$APPDATA\${USER_DIR}\${INI}"
    RMDir "$APPDATA\${USER_DIR}"
    DetailPrint "Impostazioni tolte: $APPDATA\${USER_DIR}"
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
  Delete "$INSTDIR\${CHECK_FILE}"
  Delete "$INSTDIR\docs\setup.txt"
  Delete "$INSTDIR\docs\procedure.txt"
  RMDir "$INSTDIR\docs"
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
    DetailPrint "La cartella $INSTDIR resta: dentro ci sono file non del setup (un DAT di redump, un archivio di prima)."
  ${EndIf}
  DetailPrint "L'archivio resta dov'è."
  DetailPrint "fdrawcmd.sys resta installato: si toglie da Installazione applicazioni, voce Fdrawcmd.sys."
SectionEnd
