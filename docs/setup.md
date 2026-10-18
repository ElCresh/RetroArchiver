# RetroArchiver: requirements and setup guide

How to prepare the PC that archives floppies and CDs/DVDs. Each item says
whether it is **verified** (tested on the XP VM or on the PC) or **to be
verified**.

Status at 17 October 2026: floppies work and are tested on the physical
PC. CDs and DVDs are in the program and tested on the VM with a fake
DiscImageCreator; the real DIC starts on XP with the Visual C++ 14.27
runtime and reads the disc's table of contents, but dumping a real CD
still has to be tested on the physical PC.

**Everything is installed with `RetroArchiver-Setup.exe`** (see
[Installation](#installation)): RetroArchiver, SAMdisk, fdrawcmd.sys,
DiscImageCreator and, only if it is missing, the Visual C++ 14.27 runtime
that DiscImageCreator needs. Nothing to install by hand.

**Space for a CD:** DiscImageCreator saves the tracks, a single image, the
subchannel and the C2 data: an 80-minute CD needs about 1.6 GB.
RetroArchiver checks the free space before reading.

## In short

| What | Needed for | Status |
|---|---|---|
| PC with Windows XP SP3, 32-bit | everything | verified (VM and PC) |
| Floppy controller on the motherboard | floppies | required by fdrawcmd.sys |
| fdrawcmd.sys | floppies | in the setup; verified (PC) |
| SAMdisk 3.8.12 (3.x series) | floppies | in the setup; verified (PC) |
| CD/DVD drive | CDs/DVDs | present on the PC, model still to be read |
| DiscImageCreator 20260101 | CDs/DVDs | in the setup; starts on XP with the runtime (VM, PC) |
| Visual C++ Redistributable 14.27 x86 | DiscImageCreator | in the setup, only if missing; verified (VM: removed and reinstalled by the setup; PC: installed by the setup) |
| Administrator account | DiscImageCreator | to be verified |
| NTFS disk for the archive | DVDs over 4 GB | recommended |
| redump DAT "IBM PC compatible" | verifying CDs | optional |
| RetroArchiver-Setup.exe | everything | verified (VM and PC): install, update, uninstall (VM) |

## The PC

- **Windows XP SP3, 32-bit.** The RetroArchiver programs are static Win32
  executables: they run from XP to Windows 11 without installing anything.
- **Floppy controller on the motherboard.** USB floppy drives do not give
  the control that is needed (retries, non-standard formats), and
  fdrawcmd.sys needs a native controller. Reference machine: Gigabyte
  GA-EP45-DS3LR.
- **BIOS:** `Legacy Diskette A` set to `1.44M, 3.5 in`; `SATA RAID/AHCI
  Mode` set to `Disabled` (XP has no AHCI driver of its own: in IDE mode
  it installs without extra drivers). The archiving procedure is in
  [procedure.md](procedure.md).
- **Administrator account.** DiscImageCreator talks to the optical drive
  with direct SCSI commands, which on XP need administrator rights (to be
  verified at the first test).
- **Space and file system.** A floppy is about 1.5 MB per read, a CD up to
  ~800 MB, a DVD up to 8.5 GB. The archive goes on **NTFS**: FAT32 cannot
  hold files over 4 GB.
- **No Internet.** XP has had no updates since 2014: the files it needs
  are downloaded on another PC and carried over on a USB stick.

## Installation

1. Carry `RetroArchiver-Setup.exe` (about 13 MB) to the XP PC on a USB
   stick.
2. Double-click `RetroArchiver-Setup.exe` (an administrator account is
   needed). First it asks for the language, English or Italian (the one
   of Windows is proposed; an update proposes the previous choice): the
   pages, the final summary and `install-check.txt` are in that language,
   and RetroArchiver starts in it too. The pages: welcome, third-party
   programs (credits and licenses, in English in both languages,
   to read only: Next goes on), components (all selected), folder (Program
   Files: `C:\Programmi\RetroArchiver` on Italian XP,
   `C:\Program Files\RetroArchiver` on English XP), progress, finish.
   `<folder>` below is the chosen one.

   | Component | Where | |
   |---|---|---|
   | RetroArchiver | `<folder>\` | required: `RetroArchiver.exe`, `dskrun.exe`, `dskdiff.exe`, `README.txt`, the guides in `docs\`, the third-party notices and licenses in `licenses\` |
   | SAMdisk 3.8.12 | `<folder>\tools\samdisk\` | with its license |
   | fdrawcmd.sys 1.0.1.11 | Windows driver | runs Simon Owen's `FdInstall.exe /S` |
   | Visual C++ runtime 14.27.29114 x86 | Windows | only if missing or older: `VC_redist.x86.exe /install /quiet /norestart`; the same package works from XP SP3 to Windows 11, 64-bit included |
   | DiscImageCreator 20260101 | `<folder>\tools\DIC\` | the whole `Release_ANSI` folder, with `LICENSE` and `README.md` from the zip |

   It creates the archive folder `C:\RetroArchive` (on the system drive),
   the Start menu shortcuts (RetroArchiver, Read me, Third-party notices,
   Uninstall RetroArchiver) and the desktop one, and the RetroArchiver
   entry in Add or Remove Programs.
3. At the end the setup tests the programs and reports on the last page
   (and in `<folder>\install-check.txt`, written by the setup as
   administrator: it applies to the whole installation and any user can
   read it): the SAMdisk version, whether fdrawcmd.sys is installed and
   running, the DiscImageCreator version, the runtime ("present" or
   "installed now") and the archive folder. If the runtime does not
   install it says so, with a warning and the link to install it by hand:
   floppies still work, CDs and DVDs do not.
4. **Restart** if the last page asks: a freshly installed fdrawcmd.sys
   usually starts only after a restart (rarely the runtime asks too).

**The default RetroArchiver.ini.** Next to `RetroArchiver.exe` the setup
writes the defaults: `ArchiveFolder=C:\RetroArchive`, `MaxReads=3` and
`UILanguage` (the language chosen at its start) in `[General]`;
`SAMdisk=<folder>\tools\samdisk\samdisk.exe`, `ReadFrom=a:` and
`FileName` in `[Floppy]`; `DiscImageCreator=<folder>\tools\DIC\DiscImageCreator.exe`,
`Drive=` the first CD drive it finds (otherwise `D:`), `CDFileName` and
`DVDFileName` in `[Optical]`. If the file already exists its values stay:
only missing keys are added. RetroArchiver only reads it; each user's
choices go into their own `Application Data` (see
[RetroArchiver](#retroarchiver)).

Version 261017 wrote the keys in Italian (`[Archiviazione]` with
`Cartella`, `LeggiDa`...; `[Finestra]` with `X`, `Larghezza`...). An update
converts the defaults ini and the ini of the user who installs: each value
moves to its English key and the Italian keys go; `install-check.txt` says
how many were moved. The other users' ini are converted by the window the
first time they open it. Until then RetroArchiver still reads the Italian
keys where the English ones are missing.

**Updating.** Run the same setup again, or a newer one: if RetroArchiver
is open it asks to close it (with a box in progress the window asks
whether to leave it open: it can be resumed later from the archive), then
it replaces the programs. Settings and archive stay as they are. An
installation made by the earlier setup in `C:\RetroArchiver` is updated
in place: its `RetroArchiver.ini` becomes the defaults ini, so the earlier
choices stay (`ArchiveFolder=C:\RetroArchiver\Archivio` too). SAMdisk and
DiscImageCreator, which that setup had put in `C:\samdisk` and `C:\DIC`,
move to `tools\`: their old files are removed (only the ones the setup put
there). An old ini that still says `C:\samdisk\samdisk.exe` is not a
problem: if nothing is there RetroArchiver uses the one in `tools\` and
says so in the status bar. Files with the earlier names (`Disinstalla.exe`,
`LEGGIMI.txt`, `verifica-installazione.txt`, `docs\procedura.txt` and
their shortcuts) are removed.

**Uninstalling.** From Add or Remove Programs (or the Start menu): it
removes the programs, SAMdisk and DiscImageCreator, the defaults ini, the
licenses and the shortcuts. **It does not touch the archive**; for the
settings of the user who uninstalls (`Application Data\RetroArchiver`) it
asks (the suggested answer is to keep them). It does not remove
fdrawcmd.sys, which has its own *Fdrawcmd.sys 1.0.1.11* entry in Add or
Remove Programs, nor the Visual C++ runtime.

**Silently.** `RetroArchiver-Setup.exe /S` installs everything without
questions (`Uninstall.exe /S` removes it, keeping the user's settings);
the summary is in `install-check.txt`.

The sections below say what each program does and how it would be
installed by hand.

## Floppies

1. **fdrawcmd.sys** from <https://simonowen.com/fdrawcmd/> (`FdInstall.exe`,
   in the setup): it installs as a filter under the floppy controller.
   After installing, restart the PC (or disable and re-enable the
   controller in Device Manager).
2. **SAMdisk 3.8.12** from <https://simonowen.com/samdisk/>
   (`SAMdisk3812.zip`, in the setup): it is not installed, `samdisk.exe`
   is extracted to `<folder>\tools\samdisk\` (the setup does it). The
   **3.x series** is needed: the 4.0 alphas do not read real floppies on
   Windows.
3. **Check**, with an expendable floppy in the drive:

   ```
   "C:\Programmi\RetroArchiver\tools\samdisk\samdisk.exe" rpm a:
   ```

   It should report about 300 RPM. `samdisk list` does not list floppies.

## CDs and DVDs

1. **Drive.** DiscImageCreator recommends non-OEM PLEXTOR drives (in its
   documentation, `Doc\TestedDrive.txt`): they can read in "scrambled"
   mode, the lead-in and the lead-out, which full dumps and copy
   protections need. With other drives DIC works with the `swap` command
   or with limits. DIC reports your drive's model at the first test.

   A drive that cannot read the lead-in or the lead-out (for example the
   ASUS DRW-08D6MT) makes DIC's `cd` command fail at LBA -1. RetroArchiver
   then reads a **data CD of a single track** again by itself with DIC's
   `data` command, from sector 0 to the lead-out, and writes the `.cue`
   and the `.dat` that `data` does not write. Every sector of such a read
   carries its own EDC/ECC: if they all match and there are no C2 errors,
   one read is enough (outcome "clean", `"read_method": "dic-data"` in
   `metadata.json`); otherwise a second read is compared as usual. It is a
   faithful copy of the data, but not a redump-grade dump. The window
   remembers the drive in the user's `RetroArchiver.ini`
   (`[Optical] NoLeadInDrives`, vendor, product and firmware) and from then
   on reads its data CDs with `data` straight away; the status bar says so.
   Audio and mixed-mode CDs still need a drive that reads the lead-in.
2. **Visual C++ Redistributable 2015-2019, version 14.27.29114, x86** (in
   the setup, which installs it only if missing). It is the last version
   that installs on XP; newer ones do not (and Microsoft's "latest
   version" link downloads one of those). It does not install on Windows
   2000. It downloads without an account from Microsoft's server:

   <https://download.visualstudio.microsoft.com/download/pr/56f631e5-4252-4f28-8ecc-257c7bf412b8/D305BAA965C9CD1B44EBCD53635EE9ECC6D85B54210E2764C8836F4E9DEFA345/VC_redist.x86.exe>

   13.6 MB, SHA-256 `d305baa965c9cd1b44ebcd53635ee9ecc6d85b54210e2764c8836f4e9defa345`
   (it is also the long part of the address), signed by Microsoft
   Corporation. Install it with a double click, or silently with
   `VC_redist.x86.exe /install /quiet /norestart`. **Verified** on the XP VM.
3. **DiscImageCreator 20260101** (in the setup) from
   <https://github.com/saramibreak/DiscImageCreator/releases>
   (`DiscImageCreator_20260101.zip`, 546 KB, SHA-256
   `f8e4814fca491f0f713b346fb406ed6807ae514507d494d1caea0718dea06ffc`):
   the `Release_ANSI` folder is extracted to `<folder>\tools\DIC\`. The
   `zlib1.dll` that comes with it declares Windows Vista as its minimum,
   but XP loads it anyway (**verified** on the VM).
   From a folder with spaces (`C:\Program Files\...` on English Windows)
   DIC starts: `/v` and `ls` were tested on a Windows 11 host with DIC in
   `...\Program Files prova\RetroArchiver\tools\DIC\` (RetroArchiver
   quotes the path). Actually reading a disc from there, which also uses
   `EccEdc.exe` and the files next to DIC, is still to be tested.
   Started by hand without arguments, DIC ends with "Press any key to
   continue": that is normal. RetroArchiver starts it without a console
   and it does not hang.
4. **redump DAT** (optional) from <http://redump.org/downloads/>, system
   "IBM PC compatible": download it on another PC and put it in
   `<folder>\`, next to `RetroArchiver.exe`. With the DAT, a CD that is in
   the database is "verified by redump"; without it, two reads are made
   and compared. The window shows the DAT's date and warns when it is old.
5. **Check:** `<folder>\tools\DIC\DiscImageCreator.exe /v` should print
   the version (`32 bit, AnsiBuild, 20260101T233129`; verified on the VM).
   If "VCRUNTIME140.dll was not found" appears instead, the runtime of
   step 2 is missing. In RetroArchiver's box preparation the status bar
   shows the version when it finds the program.

## RetroArchiver

1. The setup puts it in Program Files (`<folder>`): `RetroArchiver.exe`
   (the window), `dskrun.exe` and `dskdiff.exe` (console),
   `RetroArchiver.ini` with the defaults, and the window's languages in
   `lang\` (`it.lng`, `en.lng`). By hand, copying the three `.exe` files
   to a folder is enough: they need nothing else, and without `lang\` the
   window speaks the English built into it.
2. Settings live in two `RetroArchiver.ini` files, both UTF-16 with a BOM
   (paths with accented letters survive):

   - **the defaults**, next to `RetroArchiver.exe`: the setup writes them,
     the window only reads them (in Program Files a normal user cannot
     write);
   - **the user's choices**, in `%APPDATA%\RetroArchiver\RetroArchiver.ini`
     (on XP `C:\Documents and Settings\<user>\Application Data\RetroArchiver\`,
     `Dati applicazioni` on Italian XP): the only file the window writes,
     by itself, and only with the values that differ from the defaults.
     The folder is created at the first choice.

   For each key the user's value wins, otherwise the default, otherwise
   the value built into the program. This way a new default brought by an
   update reaches whoever had not changed it. The keys:

   | Section | Key | Setup default | Without any ini | What |
   |---|---|---|---|---|
   | `General` | `ArchiveFolder` | `C:\RetroArchive` | `C:\RetroArchive` (system drive) | where the boxes go |
   | `General` | `MaxReads` | `3` | `3` | maximum reads per disk |
   | `General` | `Sounds` | | `0` | 1 = Windows sounds instead of beeps |
   | `General` | `UILanguage` | the setup's language | empty | the window's language, see below |
   | `Floppy` | `SAMdisk` | `<folder>\tools\samdisk\samdisk.exe` | `samdisk` | the program for floppies |
   | `Floppy` | `ReadFrom` | `a:` | `a:` | floppy drive, or a `.dsk` file for tests |
   | `Floppy` | `FileName` | `FLOPPY{nn}` | the same | file names, only by hand in the ini |
   | `Optical` | `DiscImageCreator` | `<folder>\tools\DIC\DiscImageCreator.exe` | `DiscImageCreator` | the program for CDs and DVDs |
   | `Optical` | `Drive` | the first CD drive | `D:` | the CD/DVD drive letter |
   | `Optical` | `CDFileName`, `DVDFileName` | `CD{nn}`, `DVD{nn}` | the same | file names, only by hand in the ini |
   | `Automatic` | `Enabled` | | `0` | 1 = the Automatic checkbox |
   | `Box` | `Platform`, `Language` | | empty | the last choice in the box preparation |
   | `Window` | `X`, `Y`, `Width`, `Height`, `Maximized` | | | the window position, only in the user's ini |

   The keys of 261017 and the ones that replaced them: `[Archiviazione]`
   `Cartella` → `[General] ArchiveFolder`, `LettureMax` → `MaxReads`,
   `Suoni` → `Sounds`; `SAMdisk`, `LeggiDa` → `[Floppy] SAMdisk`,
   `ReadFrom`; `NomeFloppy` → `[Floppy] FileName`; `DiscImageCreator`,
   `LettoreCD`, `NomeCD`, `NomeDVD` → `[Optical] DiscImageCreator`, `Drive`,
   `CDFileName`, `DVDFileName`; `AutoMode` → `[Automatic] Enabled`;
   `Piattaforma`, `Lingua` → `[Box] Platform`, `Language`; `[Finestra]`
   `X`, `Y`, `Larghezza`, `Altezza`, `Massimizzata` → `[Window] X`, `Y`,
   `Width`, `Height`, `Maximized`. `NomeFile` is dropped. A key in English
   wins over its Italian one; the window converts the user's ini by itself
   at its first start, the setup the defaults.

   `UILanguage` is the window's language: `it`, `en` (the name of a file in
   `lang\`) or empty to follow the language of Windows. The setup writes
   the language chosen at its start in the defaults; Program language
   (Lingua del programma) in the box preparation writes the user's
   choice, and the window changes at once. Windows in Italian gives
   Italian, any other language English, unless `lang\` has a file for it.
   If
   `SAMdisk` or `DiscImageCreator` cannot be found (an older ini with
   `C:\samdisk`), the window uses the ones in `tools\` next to the
   executable.

   The redump DAT goes next to `RetroArchiver.exe` (in Program Files this
   needs an administrator): the window finds it by itself and shows its
   date in the status bar.

   The language files, `lang\<code>.lng`, are UTF-8 text with one
   `key=text` per line (`en.lng` explains the format at its top). A new
   language is a copy of `en.lng` with the texts translated, saved as
   `lang\de.lng`, `lang\fr.lng`...: it appears in Program language by
   itself. A text that is missing, or whose `%` placeholders differ from
   the English ones, stays in English. The setup removes only `it.lng`
   and `en.lng`.
3. **Check:** open `RetroArchiver.exe`. It opens on the Archive (empty the
   first time). With New box (Nuovo cofanetto), the status bar at the
   bottom left should say "SAMdisk 3.8.12"; once title, platform and media
   are filled in, "Pronto" (ready) appears at the bottom right.

## Recommended folders

The setup's ones, on Italian XP:

```
C:\Programmi\RetroArchiver\RetroArchiver.exe
C:\Programmi\RetroArchiver\RetroArchiver.ini      (the defaults, read-only)
C:\Programmi\RetroArchiver\lang\                  (the window's languages: it.lng, en.lng)
C:\Programmi\RetroArchiver\<redump DAT>.dat
C:\Programmi\RetroArchiver\licenses\              (third-party notices and licenses)
C:\Programmi\RetroArchiver\tools\samdisk\samdisk.exe
C:\Programmi\RetroArchiver\tools\DIC\DiscImageCreator.exe   (with its files)
C:\Documents and Settings\<user>\Dati applicazioni\RetroArchiver\RetroArchiver.ini
C:\RetroArchive\                                  (the archive: better a large NTFS disk)
```

The archive is moved from the window (the Folder field, or the one at
the top of the Archive): the choice goes into the user's ini.

## Credits and licenses

RetroArchiver bundles and installs, unmodified, SAMdisk 3.8.12 and
fdrawcmd.sys 1.0.1.11 by Simon Owen (MIT), DiscImageCreator 20260101 by
sarami (Apache 2.0) and the Visual C++ 2015-2019 14.27.29114 runtime by
Microsoft (under its license terms: the setup installs it silently, so
installing it means accepting them). Authors, sites, licenses and what
RetroArchiver does with each program are in
[THIRD-PARTY-NOTICES.txt](../THIRD-PARTY-NOTICES.txt); the original license
texts in `installer/licenses/`. The setup shows the notices before the
components and installs them in `<folder>\licenses\`, with each program's
license also next to it in `tools\`; the Start menu has *Third-party
notices*. redump DATs have their own terms and the setup does not include
them.

Third-party files are not in the repository: `make setup` takes them from
a separate folder and checks their checksums (`installer/third-party.txt`,
which also names each component's license file).
