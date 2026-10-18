; The setup's texts, in English and Italian: each key has its two
; translations next to each other. Included by retroarchiver.nsi after
; the MUI_LANGUAGE lines. Variables ($0, $INSTDIR...) are expanded when the
; text is used, defines (${T_samdisk_VERSION}...) when the setup is built.
; A new language: one more line per key, and its MUI_LANGUAGE in the .nsi.

!macro TEXT key en it
  LangString ${key} ${LANG_ENGLISH} "${en}"
  LangString ${key} ${LANG_ITALIAN} "${it}"
!macroend

; the program's language, written as UILanguage in the defaults ini
!insertmacro TEXT L_UILANG "en" "it"

; ---- pages ----
!insertmacro TEXT L_WELCOME_TITLE "RetroArchiver setup" "Installazione di RetroArchiver"
!insertmacro TEXT L_WELCOME_TEXT \
  "Installs RetroArchiver and the programs needed to archive floppies, CDs and DVDs:$\r$\n$\r$\n  • SAMdisk ${T_samdisk_VERSION}$\r$\n  • the fdrawcmd.sys driver ${T_fdrawcmd_VERSION}$\r$\n  • DiscImageCreator ${T_dic_VERSION}$\r$\n  • the Visual C++ runtime ${T_vcredist_VERSION}, only if it is missing$\r$\n$\r$\nIf RetroArchiver is already installed it is updated: the settings and the archive stay as they are." \
  "Installa RetroArchiver e i programmi che servono per archiviare floppy, CD e DVD:$\r$\n$\r$\n  • SAMdisk ${T_samdisk_VERSION}$\r$\n  • il driver fdrawcmd.sys ${T_fdrawcmd_VERSION}$\r$\n  • DiscImageCreator ${T_dic_VERSION}$\r$\n  • il runtime di Visual C++ ${T_vcredist_VERSION}, solo se manca$\r$\n$\r$\nSe RetroArchiver è già installato lo aggiorna: le impostazioni e l'archivio restano come sono."
!insertmacro TEXT L_TP_HEADER "Third-party programs" "Programmi di terzi"
!insertmacro TEXT L_TP_SUBTEXT "Credits and licences of the programs installed with RetroArchiver." \
  "Crediti e licenze dei programmi installati con RetroArchiver."
!insertmacro TEXT L_TP_TOP "Press Page Down to read the rest." \
  "Premi Pagina giù per leggere il resto (il testo è in inglese)."
!insertmacro TEXT L_TP_BOTTOM \
  "They are installed as their authors distribute them; the full licences go in the licenses folder. The Visual C++ runtime, if missing, is installed silently: installing it means accepting Microsoft's licence terms." \
  "Si installano come li distribuiscono gli autori; le licenze complete vanno nella cartella licenses. Il runtime di Visual C++, se manca, si installa in silenzio: installarlo vuol dire accettare le condizioni di licenza di Microsoft."
!insertmacro TEXT L_DIR_TOP \
  "RetroArchiver goes in this folder, with SAMdisk and DiscImageCreator in its tools subfolder. Each user's settings go in their own Application Data folder.$\r$\n$\r$\nThe archive of box sets goes in $ArchiveDir; it can be moved from the RetroArchiver window (a roomy NTFS disk is best)." \
  "In questa cartella va RetroArchiver, con SAMdisk e DiscImageCreator nella sottocartella tools. Le impostazioni di ogni utente vanno nella sua cartella Dati applicazioni.$\r$\n$\r$\nL'archivio dei cofanetti va in $ArchiveDir; si sposta dalla finestra di RetroArchiver (meglio un disco NTFS capiente)."
!insertmacro TEXT L_FINISH_TITLE "RetroArchiver is installed" "RetroArchiver è installato"
!insertmacro TEXT L_FINISH_REBOOT "$Summary$\r$\n$\r$\nfdrawcmd.sys works after the PC restarts." \
  "$Summary$\r$\n$\r$\nfdrawcmd.sys funziona dopo un riavvio del PC."
!insertmacro TEXT L_REBOOT_NOW "Restart now" "Riavvia adesso"
!insertmacro TEXT L_REBOOT_LATER "Restart later" "Riavvio più tardi"
!insertmacro TEXT L_RUN "Open RetroArchiver" "Apri RetroArchiver"
!insertmacro TEXT L_UNCONFIRM \
  "Removes RetroArchiver, with SAMdisk and DiscImageCreator. The archive stays, and so do the fdrawcmd.sys driver (it has its own entry in Add or Remove Programs) and the Visual C++ runtime." \
  "Tolgo RetroArchiver, con SAMdisk e DiscImageCreator. Restano l'archivio, il driver fdrawcmd.sys (ha la sua voce in Installazione applicazioni) e il runtime di Visual C++."

; ---- RetroArchiver is open ----
!insertmacro TEXT L_CLOSE_ASK \
  "RetroArchiver is open: press OK and it will be closed.$\r$\n$\r$\nIf it is reading a disk it asks whether to leave the box open: answer Yes, it can be resumed later from the archive." \
  "RetroArchiver è aperto: premi OK e lo chiudo.$\r$\n$\r$\nSe sta leggendo un disco ti chiede se lasciare il cofanetto aperto: rispondi Sì, si riprende dopo dall'archivio."
!insertmacro TEXT L_CLOSE_ABORT "RetroArchiver is still open: nothing has been changed." \
  "RetroArchiver è ancora aperto: niente è stato cambiato."
!insertmacro TEXT L_CLOSED "RetroArchiver closed." "RetroArchiver chiuso."
!insertmacro TEXT L_CLOSE_STUCK "RetroArchiver did not close. Close it yourself and press Retry." \
  "RetroArchiver non si è chiuso. Chiudilo tu e premi Riprova."
!insertmacro TEXT L_CLOSE_STOPPED "RetroArchiver is still open: setup stopped." \
  "RetroArchiver è ancora aperto: installazione interrotta."

; ---- the defaults ini and the archive ----
!insertmacro TEXT L_INI_NEW "Creating ${INI} with the defaults:" "Creo ${INI} con i default:"
!insertmacro TEXT L_INI_EXISTS "${INI} is already there: adding only the defaults that are missing." \
  "${INI} c'è già: aggiungo solo i default che mancano."
!insertmacro TEXT L_INI_CONVERTED "$R0: $R1 settings moved to the English keys." \
  "$R0: $R1 impostazioni passate alle chiavi inglesi."
!insertmacro TEXT L_INI_CONV_FAIL "Cannot convert $R0 to the English keys: RetroArchiver reads it as it is." \
  "Non riesco a convertire $R0 alle chiavi inglesi: RetroArchiver lo legge così com'è."
!insertmacro TEXT L_ARCHIVE_IS "Archive: $0" "Archivio: $0"
!insertmacro TEXT L_ARCHIVE_FAIL "Cannot create the archive folder $0: choose it from the RetroArchiver window." \
  "Non riesco a creare la cartella dell'archivio $0: si sceglie dalla finestra di RetroArchiver."
!insertmacro TEXT L_ARCHIVE_NEW "Creating the archive folder: $0" "Creo la cartella dell'archivio: $0"

; ---- the Visual C++ runtime ----
!insertmacro TEXT L_VC_FAILED "Could not install the Visual C++ 2015-2019 x86 runtime version 14.27 ($VcResult)" \
  "Non sono riuscito a installare il runtime di Visual C++ 2015-2019 x86 versione 14.27 ($VcResult)"
!insertmacro TEXT L_VC_OLD "The installed Visual C++ 2015-2019 x86 runtime is older than version 14.27" \
  "Il runtime di Visual C++ 2015-2019 x86 installato è più vecchio della versione 14.27"
!insertmacro TEXT L_VC_MISSING "The Visual C++ 2015-2019 x86 runtime version 14.27 is missing" \
  "Manca il runtime di Visual C++ 2015-2019 x86 versione 14.27"
!insertmacro TEXT L_VC_DETAIL "$0: without it DiscImageCreator does not start." "$0: senza, DiscImageCreator non parte."
!insertmacro TEXT L_VC_BOX \
  "$0, which DiscImageCreator needs: without it CDs and DVDs cannot be read (floppies can).$\r$\n$\r$\nTry again by running this setup once more, or install it by hand: VC_redist.x86.exe version 14.27.29114 from Microsoft's site:$\r$\n$\r$\n${VCREDIST_URL}$\r$\n$\r$\nThe link is also in $INSTDIR\${CHECK_FILE}." \
  "$0, che serve a DiscImageCreator: senza, i CD e i DVD non si leggono (i floppy sì).$\r$\n$\r$\nRiprova lanciando di nuovo questo setup, oppure installalo a mano: VC_redist.x86.exe versione 14.27.29114 dal sito Microsoft:$\r$\n$\r$\n${VCREDIST_URL}$\r$\n$\r$\nIl link è anche in $INSTDIR\${CHECK_FILE}."
!insertmacro TEXT L_VC_PRESENT "Visual C++ runtime 14.27 or newer: already there." \
  "Runtime di Visual C++ 14.27 o più nuovo: c'è già."
!insertmacro TEXT L_VC_UNSUPPORTED "it does not work on this version of Windows" "non va su questa versione di Windows"
!insertmacro TEXT L_VC_RESULT "Visual C++ runtime: $VcResult" "Runtime di Visual C++: $VcResult"
!insertmacro TEXT L_VC_INSTALLING "Installing the Visual C++ runtime ${T_vcredist_VERSION}..." \
  "Installo il runtime di Visual C++ ${T_vcredist_VERSION}..."
!insertmacro TEXT L_CODE "code $0" "codice $0"
!insertmacro TEXT L_NOSTART "it does not start" "non parte"

; ---- components ----
!insertmacro TEXT L_SEC_SAMDISK "SAMdisk ${T_samdisk_VERSION} (floppies)" "SAMdisk ${T_samdisk_VERSION} (floppy)"
!insertmacro TEXT L_SEC_FDRAWCMD "fdrawcmd.sys ${T_fdrawcmd_VERSION} (floppy driver)" \
  "fdrawcmd.sys ${T_fdrawcmd_VERSION} (driver per i floppy)"
!insertmacro TEXT L_SEC_VCREDIST "Visual C++ runtime ${T_vcredist_VERSION} (for CDs and DVDs)" \
  "Runtime di Visual C++ ${T_vcredist_VERSION} (per i CD e DVD)"
!insertmacro TEXT L_SEC_DIC "DiscImageCreator ${T_dic_VERSION} (CDs and DVDs)" "DiscImageCreator ${T_dic_VERSION} (CD e DVD)"
!insertmacro TEXT L_DESC_RA \
  "The RetroArchiver window, dskrun and dskdiff, the Read me, the guides and the licences of the third-party programs. Required." \
  "La finestra di RetroArchiver, dskrun e dskdiff, il Leggimi, le guide e le licenze dei programmi di terzi. Obbligatorio."
!insertmacro TEXT L_DESC_SAMDISK "Reads floppies. In the RetroArchiver folder, in tools\samdisk." \
  "Legge i floppy. Nella cartella di RetroArchiver, in tools\samdisk."
!insertmacro TEXT L_DESC_FDRAWCMD "The driver SAMdisk uses to talk to the floppy controller. It needs a restart after it is installed." \
  "Il driver con cui SAMdisk parla con il controller floppy. Dopo l'installazione serve un riavvio."
!insertmacro TEXT L_DESC_VCREDIST "DiscImageCreator needs it. Installed only if missing or older than 14.27; if it is already there it is left alone." \
  "Serve a DiscImageCreator. Si installa solo se manca o è più vecchio della 14.27; se c'è già non lo tocco."
!insertmacro TEXT L_DESC_DIC "Reads CDs and DVDs. In the RetroArchiver folder, in tools\DIC. It needs the Visual C++ runtime 14.27." \
  "Legge i CD e i DVD. Nella cartella di RetroArchiver, in tools\DIC. Ha bisogno del runtime di Visual C++ 14.27."
!insertmacro TEXT L_SAMDISK_MOVES "SAMdisk moves from ${OLD_SAMDISK_DIR} to ${SAMDISK_DIR}." \
  "SAMdisk passa da ${OLD_SAMDISK_DIR} a ${SAMDISK_DIR}."
!insertmacro TEXT L_DIC_MOVES "DiscImageCreator moves from ${OLD_DIC_DIR} to ${DIC_DIR}." \
  "DiscImageCreator passa da ${OLD_DIC_DIR} a ${DIC_DIR}."
!insertmacro TEXT L_FD_INSTALLING "Installing fdrawcmd.sys (${T_fdrawcmd_FILE} /S)..." \
  "Installo fdrawcmd.sys (${T_fdrawcmd_FILE} /S)..."
!insertmacro TEXT L_FD_FAILED "it failed (code $0)" "non riuscita (codice $0)"
!insertmacro TEXT L_FD_BOX \
  "Installing fdrawcmd.sys did not work ($FdResult). Without it SAMdisk cannot read floppies.$\r$\n$\r$\nTry again by running this setup once more, or install it by hand from ${T_fdrawcmd_URL}" \
  "L'installazione di fdrawcmd.sys non è riuscita ($FdResult). Senza, SAMdisk non legge i floppy.$\r$\n$\r$\nRiprova lanciando di nuovo questo setup, oppure installalo a mano da ${T_fdrawcmd_URL}"

; ---- the final check: the last page and install-check.txt ----
!insertmacro TEXT L_CHECK "Check:" "Verifica:"
!insertmacro TEXT L_NOANSWER "does not answer" "non risponde"
!insertmacro TEXT L_NOTINSTALLED "not installed" "non installato"
!insertmacro TEXT L_NOTINSTALLED_WHY "not installed ($FdResult)" "non installato ($FdResult)"
!insertmacro TEXT L_FD_ACTIVE "installed and running" "installato e attivo"
!insertmacro TEXT L_FD_AFTERBOOT "installed, it starts after a restart" "installato, si attiva dopo il riavvio"
!insertmacro TEXT L_DIC_NORUNTIME "the Visual C++ runtime is missing" "manca il runtime di Visual C++"
!insertmacro TEXT L_RT_PRESENT "present" "presente"
!insertmacro TEXT L_RT_OLD "older than 14.27" "più vecchio della 14.27"
!insertmacro TEXT L_RT_MISSING "missing" "manca"
!insertmacro TEXT L_RT_NOW "$R2 (installed now)" "$R2 (installato ora)"
!insertmacro TEXT L_SUM_RUNTIME "• Visual C++ runtime 14.27: $R2" "• runtime di Visual C++ 14.27: $R2"
!insertmacro TEXT L_SUM_ARCHIVE "• archive: $R2" "• archivio: $R2"
!insertmacro TEXT L_SUM_INICONV \
  "• settings moved from the Italian keys to the English ones: $IniConv in the defaults, $IniConvUser in yours" \
  "• impostazioni passate dalle chiavi italiane a quelle inglesi: $IniConv nei default, $IniConvUser nelle tue"
!insertmacro TEXT L_SUM_NORUNTIME \
  "Without the runtime CDs and DVDs cannot be read. Run this setup again, or install VC_redist.x86.exe 14.27.29114 by hand: the link is in ${CHECK_FILE}." \
  "Senza il runtime i CD e i DVD non si leggono. Rilancia questo setup, oppure installa a mano VC_redist.x86.exe 14.27.29114: il link è in ${CHECK_FILE}."
!insertmacro TEXT L_SUM_ALSO "This summary is also in $INSTDIR\${CHECK_FILE}." \
  "Questo riepilogo è anche in $INSTDIR\${CHECK_FILE}."
!insertmacro TEXT L_CHECK_VC "Visual C++ 2015-2019 x86 runtime version 14.27.29114 (the last one that works on XP):" \
  "Runtime di Visual C++ 2015-2019 x86 versione 14.27.29114 (l'ultima che va su XP):"

; ---- uninstall ----
!insertmacro TEXT L_UN_SETTINGS_ASK \
  "Also remove your RetroArchiver settings?$\r$\n($APPDATA\${USER_DIR})$\r$\n$\r$\nIf you reinstall RetroArchiver, with the settings you find again the archive folder and your other choices." \
  "Tolgo anche le tue impostazioni di RetroArchiver?$\r$\n($APPDATA\${USER_DIR})$\r$\n$\r$\nSe reinstalli RetroArchiver, con le impostazioni ritrovi la cartella dell'archivio e le altre scelte."
!insertmacro TEXT L_UN_SETTINGS_GONE "Settings removed: $APPDATA\${USER_DIR}" "Impostazioni tolte: $APPDATA\${USER_DIR}"
!insertmacro TEXT L_UN_DIR_STAYS \
  "The folder $INSTDIR stays: it holds files the setup did not put there (a redump DAT, an older archive)." \
  "La cartella $INSTDIR resta: dentro ci sono file non del setup (un DAT di redump, un archivio di prima)."
!insertmacro TEXT L_UN_ARCHIVE_STAYS "The archive stays where it is." "L'archivio resta dov'è."
!insertmacro TEXT L_UN_FD_STAYS \
  "fdrawcmd.sys stays installed: remove it from Add or Remove Programs, entry Fdrawcmd.sys." \
  "fdrawcmd.sys resta installato: si toglie da Installazione applicazioni, voce Fdrawcmd.sys."
