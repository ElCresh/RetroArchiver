# RetroArchiver

Preservation tools for retro PC software boxes: 3.5" floppy disks with
[SAMdisk](https://simonowen.com/samdisk/), CDs and DVDs with
[DiscImageCreator](https://github.com/saramibreak/DiscImageCreator).
Every box becomes a self-verifying [BagIt](https://www.rfc-editor.org/rfc/rfc8493)
package with checksums, read reports and structured metadata.

RetroArchiver is designed to run on a dedicated machine with
**Windows XP SP3** and a **real floppy disk controller** on the
motherboard. USB floppy drives give no control over retries and cannot
read non-standard formats, and SAMdisk's `fdrawcmd.sys` driver needs a
native controller.

> **Status:** in active development. The window and the installer speak
> English and Italian; the console tools, the settings file and the
> messages of the core are in English. The guides in `docs/` are in
> English: [setup](docs/setup.md) and [archiving procedure](docs/procedure.md).
>
> Releases are named after their date, `YYMMDD`: 261017 is the release
> of 17 October 2026. `master` holds the latest release, `development`
> the current work.

## What it does

- **RetroArchiver** (GUI) opens on your archive: the boxes already
  preserved, their state, and what is still missing. For a new box you
  enter title, platform and media (how many floppies, CDs and DVDs),
  then insert one disk at a time. For each disk it scans, reads, compares
  and gives a verdict, then writes the BagIt package and verifies it.
- **dskrun** does the same from the console, for batch work.
- **dskdiff** compares two or more reads of the same floppy and tells you
  whether the differences actually matter.

### Floppy disks

Each floppy is captured as an **Extended DSK** image (`.dsk`) and never
converted: EDSK keeps everything SAMdisk saw, including multiple copies
of unstable sectors, real track sizes, unformatted tracks and extra
cylinders.

A disk is read at least twice. The two reads are compared track by
track; if they disagree, a third read decides. The verdict is one of:

- **clean**: the data is identical across reads;
- **noise in extra cylinders only**: differences exist only beyond the
  disk format, where the head reads magnetic noise (harmless);
- **unstable tracks**: some tracks change between reads, with a majority
  analysis across three or more reads;
- **damaged**: real data differs, and the disk needs attention.

Why this matters: comparing two raw dumps with `fc /b` usually gives
thousands of differences, almost all of them in unformatted cylinders.
`dskdiff` rebuilds the track map from the EDSK header and attributes
each difference to its cylinder, so the real problems stand out.

### CDs and DVDs

Optical discs are read with DiscImageCreator (`cd` with C2 error
re-reading and, when needed, protection detection, or `dvd`). If the
track checksums match a **redump.org DAT** placed next to the program,
one read is enough: the disc is identical to the one other archivists
verified. Otherwise two reads are compared.

**Copy protection.** DiscImageCreator's `/sf` looks for protections in
the disc's files, reading 64 sectors at a time, which failed on a real
IDE controller with an unprotected disc. So it is a choice of the box:
*Automatic* (the default) turns it on only when the mounted disc has a
signature file of a known protection (SafeDisc, SecuROM, LaserLock,
CD-Cops: the list is `protection-signatures.txt` next to the program,
from the names BinaryObjectScanner checks); *On* and *Off* force it
(`dskrun --copy-protection=auto|on|off`). A read made without it that
fails or has errors is offered again with the scan on. If with the scan
the drive keeps resetting itself (UNIT_ATTENTION, timeouts), the read is
stopped at the third reset and offered again without it.

**Drive offset.** For a drive missing from DiscImageCreator's
`driveOffset.txt`, set its read offset in samples (AccurateRip or redump
list) in Tools > Settings, CD and DVD (`[Optical] DriveOffset` of
`RetroArchiver.ini`), or `dskrun --drive-offset=N`: it is given to DiscImageCreator as the answer
to its question. Both choices, and the offset used, are recorded in
`metadata.json`.

**Drives that cannot read the lead-in.** DiscImageCreator's `cd` command
needs the lead-in or the lead-out; on a drive that cannot read them it
stops at LBA -1. When the TOC says the CD is a single data track,
RetroArchiver reads it again by itself with DiscImageCreator's `data`
command, from sector 0 to the lead-out, and writes the `.cue` and the
`.dat` that `data` does not write. If every sector matches its EDC/ECC
and there are no C2 errors, one read is enough (outcome *clean*,
`"read_method": "dic-data"` in `metadata.json`: a faithful copy of the
data, not a redump-grade dump); otherwise a second read is compared. The
window remembers the drive in `[Optical] NoLeadInDrives` and reads its
next data CDs with `data` straight away; Settings lists those drives and
can forget one. Audio and mixed-mode CDs still
need a drive that reads the lead-in.

### Mixed boxes

A box can contain floppies and optical discs together (Windows NT 4.0:
three boot floppies and a CD). RetroArchiver asks for the floppies
first, then the CDs, then the DVDs.

### The package

```
D:\Archive\Monkey Island\
  bagit.txt  bag-info.txt  manifest-sha256.txt  tagmanifest-sha256.txt
  data\  FLOPPY01.dsk  FLOPPY01.scan.txt  FLOPPY01.diff.txt  ...  metadata.json
         CD01\  CD01.cue  CD01 (Track 1).bin  ...  CD01.dat
         images\  box-front.jpg  box-back.tif  cd01-front.png  insert-01.tif
```

`metadata.json` records, for every medium, the reads with their SHA-256
hashes, the comparison verdict, the detected format and the file
listing (FAT12 for floppies, ISO 9660 with Joliet long names for
optical discs), plus tracks, C2/EDC errors and the redump match for CDs
and DVDs. The package rules are in
[`docs/bagit-profile.json`](docs/bagit-profile.json); boxes closed by
version 261017, with the outcomes in Italian, follow
[`docs/bagit-profile-2.json`](docs/bagit-profile-2.json) and are still read.

An interrupted box can be resumed at any time: from the archive view in
the GUI, or by running the same `dskrun` command again.

### Images of the box

Scans of the box, of the media labels and of the inserts can be attached
to a closed box (Archive > Images..., or `dskrun --add-image FILE --as
box-front`): JPEG, PNG, TIFF, BMP, GIF, and PDF for inserts. The files are
copied as they are into `data\images\`, named after what they show, with
their hash in the manifest and their description in `metadata.json`.
RetroArchiver does not scan, crop or convert. Removing an image moves it
to `set-aside\`. The archive view shows the thumbnails and a viewer; they
are decoded with the GDI+ that Windows has, loaded at run time. A box with
images uses schema `retroarchiver-4`, which versions up to 261104 refuse
to open instead of dropping the images' description. The images go to
Archive.org with the rest of the package.

### Moving the archive

The archive is the folder: there is no index. A box is its title, edition
and language in `metadata.json`, and the folder name is only a label, so
boxes can be copied to another PC or renamed by hand. Archive > Refresh
(F5) reads the folder again, and the list refreshes by itself when you
come back to the window. An empty archive says why, and offers the
subfolder that holds the boxes if they are one level down. Two folders
holding the same box are marked, and nothing is resumed or edited until
one of them is moved away.

### Freeing space

Failed and duplicate reads are set aside in `set-aside\`, outside the
package data. Archive > Free space... (or `dskrun --free-space`) removes
the ones that were superseded by a good read of the same medium, keeping
the text logs; damaged, replaced and rejected reads always stay, and the
package itself is never touched. The window asks first and uses the
Recycle Bin.

### Upload to Archive.org

A closed box can go to [archive.org](https://archive.org), one box at a
time, straight from the XP PC: the TLS 1.2 connection is made by
[BearSSL](https://www.bearssl.org/), built into the executable, with
Mozilla's list of root certificates. You need an Archive.org account and
its IA-S3 keys (<https://archive.org/account/s3.php>), entered in
Tools > Settings > Archive.org: they are kept encrypted for your Windows
user (DPAPI), never in clear.

- The item's fields (title, author, date, languages, subjects,
  description, rights) are proposed from the box and can be edited
  before uploading; the identifier is checked first. The item is public
  and cannot be deleted: the window says so before you confirm.
- The whole BagIt package goes up as it is, each file with its MD5 so a
  file damaged on the way is refused; an interrupted upload resumes
  where it stopped.
- When Archive.org has archived the files, RetroArchiver checks size,
  MD5 and SHA-1 of every file against the package. The result is kept
  in `uploads\archive.org.json` beside the package, outside its
  manifests, and shown in the archive view.
- After an edit, the box is marked *to update*: the update sends only
  the new and changed files (the old versions stay in the item's
  history), removes from the item the files the box no longer has, and
  writes only the changed fields. A field changed on archive.org since
  the last upload keeps the site's value unless you restore the box's.

## Requirements

- A PC with Windows XP SP3 (newer Windows versions work too) and a
  floppy controller on the motherboard.
- An administrator account (needed by `fdrawcmd.sys` and by
  DiscImageCreator to open the drives).
- For CDs, a drive that reports C2 errors; DiscImageCreator recommends
  Plextor drives.

The executables are statically linked 32-bit Win32 programs: no
runtime and no DLLs of their own.

## Installation

Run `RetroArchiver-Setup.exe` on the archiving PC. It installs:

- RetroArchiver, `dskrun` and `dskdiff` in `Program Files\RetroArchiver`;
- [SAMdisk](https://simonowen.com/samdisk/) 3.8.12 and the
  [fdrawcmd.sys](https://simonowen.com/fdrawcmd/) floppy driver;
- [DiscImageCreator](https://github.com/saramibreak/DiscImageCreator)
  for CDs and DVDs, and the Visual C++ 2015-2019 x86 runtime (14.27,
  the last version that installs on XP) only if it is missing.

Settings are kept per user in `Application Data\RetroArchiver`, and
changed in Tools > Settings: General (language, archive folder, maximum
reads, sounds), Floppy (SAMdisk, the drive or a `.dsk` image for tests,
file names), CD and DVD (DiscImageCreator, drive, names, redump DAT
folder, drive offset, drives without lead-in) and Automatic (the
countdown and how often the floppy drive is asked). They apply at once.
Help > About RetroArchiver shows the version, the license and the
third-party programs with the version found on the PC, opens their
licenses and copies all of it to the clipboard for a bug report. The
default archive folder is `RetroArchive` on the system drive. Running
the installer again updates an existing installation without touching
settings or archive.

The installer and the window speak English and Italian. The installer
asks at the start, and the window starts in the same language; later it
can be changed from Program language in Settings (Same as Windows
follows it). The window's texts are in
`lang\en.lng` and `lang\it.lng`: another language is one more file,
see [docs/setup.md](docs/setup.md). The console tools are always in
English, and so are the core's messages the window shows as they are (the
log lines, the outcomes and advice in the archive).

Use the **3.x** series of SAMdisk: the 4.0 alphas lost access to real
floppy drives on Windows.

## Usage

```
dskrun NUMBER_OF_FLOPPIES -t TITLE [options]

dskrun 4 -t "Monkey Island" -P MS-DOS -o D:\Archive
dskrun 3 -C 1 -t "Windows NT Workstation 4.0" -P "Windows NT"
```

```
dskdiff FIRST.DSK SECOND.DSK [THIRD.DSK ...] [-c CYLINDERS]
```

`dskdiff` exits with `0` for a stable disk, `1` for differences in the
data and `2` for errors. Run either program without arguments for the
full list of options.

## Building

The project is plain C with no external dependencies. It builds with
MinGW-w64 (for example in WSL):

```
make           # Win32 executables (i686-w64-mingw32-gcc); also checks
               # that they only import functions available on XP SP3
make native    # local build with gcc
make test      # generates the fixtures and runs all test suites
make setup     # dist/RetroArchiver-Setup.exe, with NSIS 3 (makensis)
```

The tests need no floppy drive: `tests/gen_edsk.py` builds synthetic
EDSK images (including a real FAT12 file system) that reproduce real
scenarios, and fake SAMdisk and DiscImageCreator programs replay
recorded behaviour for full archiving sessions.

Third-party files bundled by the installer are not stored in this
repository. They are listed in [`installer/third-party.txt`](installer/third-party.txt)
with version, download URL, SHA-256 and license; download them into
`installer/third-party/` and `make setup` verifies them before building.

One of them, Mozilla's CA certificate list (`cacert.pem`), ages: its
version must be the Mozilla date written in the file, the roots needed
today must be in it, and `make setup` warns when the date is more than 6
months older than the release version. BearSSL is different: it is a
source library in [`third_party/bearssl/`](third_party/bearssl/),
compiled into `RetroArchiver.exe`, so it has no line in the manifest.

## Project layout

```
src/core/     portable core: session, EDSK comparison, BagIt, metadata,
              FAT12 and ISO 9660 readers, SHA-256, JSON, process runner
src/gui/      RetroArchiver (Win32 GUI); lang.c, its texts by key
lang/         the window's languages, one key=text file each
src/dskrun.c  console archiver
src/dskdiff.c read comparison tool
installer/    NSIS installer and third-party manifest
third_party/  BearSSL, unmodified, compiled into RetroArchiver.exe
tests/        unit tests, dskdiff cases, golden outputs, session tests
docs/         setup guide, archiving procedure, BagIt profiles
```

## Third-party software

RetroArchiver drives these programs and its installer redistributes
them under their own licenses:

- SAMdisk and fdrawcmd.sys by Simon Owen: MIT License;
- DiscImageCreator by sarami: Apache License 2.0;
- Microsoft Visual C++ Redistributable: Microsoft redistributable
  license terms;
- Mozilla's CA certificate list, extracted by the curl project
  (`cacert.pem`): Mozilla Public License 2.0, installed unmodified as a
  separate file in `certs\` and read at every upload to Archive.org.

BearSSL by Thomas Pornin (MIT License) is compiled into
`RetroArchiver.exe` for the TLS of that upload; its unmodified sources
are in [`third_party/bearssl/`](third_party/bearssl/).

Details are in [`THIRD-PARTY-NOTICES.txt`](THIRD-PARTY-NOTICES.txt),
the license texts in [`installer/licenses/`](installer/licenses/). The
installer shows the notices before installing and copies them, with
the licenses, into `licenses\` next to the program.

## License

RetroArchiver is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation, either version 3 of the License, or (at your
option) any later version. It is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. The full text is in
[`LICENSE`](LICENSE); the installer puts it next to the program as
`LICENSE.txt`, and Help > About opens it.

The third-party programs above keep their own licenses.
