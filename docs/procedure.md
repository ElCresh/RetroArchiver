# 3.5" floppy archiving procedure

What RetroArchiver (and `dskrun`, its console twin) does with each floppy,
and how to read what it reports. Machine setup is in [setup.md](setup.md).

## Before inserting a floppy

**Write-protect tab locked.** RetroArchiver only reads: the tab makes
sure nothing can write to the original.

## What happens for each floppy

1. **Scan** (`samdisk scan a:`), saved as `FLOPPY01.scan.txt`. It shows
   the geometry and, for each track, the sectors. SAMdisk marks bad
   sectors in square brackets (`5[dc]`, `9[ic,-512]`); `7d` is a sector
   with deleted data, which is normal and also used by copy protections.
2. **Reads** (`samdisk copy a: FLOPPY01.dsk`). Normally two reads, and a
   third if the first two disagree, up to the maximum set in the window
   (default 3).
   If the scan already found bad sectors, the disk is treated as
   **fragile**: one read only, with strong retries
   (`--retries=30 --rescans=5`), because every extra pass risks finishing
   it off.
3. **Comparison** of the reads, the same analysis as `dskdiff`, saved as
   `FLOPPY01.diff.txt`, with the outcome in the table.
4. At the end of the box, the BagIt package is closed: SHA-256 of every
   file in `manifest-sha256.txt`, re-checked before closing.

The outcomes, as `metadata.json` stores them (the window shows them in its language;
version 261017 stored the Italian words, which are still read):

| Outcome | When |
| --- | --- |
| clean (pulito) | the reads are identical, or differ only in the extra cylinders or the header |
| marginal (marginale) | after the last read the reads still differ in the data area (with three or more: tracks that change), or a read has no EDSK header and its tracks cannot be mapped |
| error (errore) | a read could not be compared (unreadable file) |
| fragile | the scan found bad sectors; read once with strong retries, not compared |
| read-failed (lettura fallita) | SAMdisk could not read the disk: retry or skip |

A disk that has to be read again keeps its earlier reads: the new ones
are added (`_b`, `_c`, ... up to six) and all of them are compared together.

## Why .dsk (EDSK) and not raw

A raw image is the bare concatenation of the sectors: no metadata, a fixed
size, a perfectly regular geometry taken for granted. EDSK keeps what
SAMdisk saw: the multiple copies of unstable sectors, the real size of
each track, unformatted tracks and extra cylinders. Images are archived as
`.dsk` and never converted.

SAMdisk 3.8.12 does not accept `.img` or `.raw` as output names.

## Reading the comparison

**Where the real disk ends.** In an EDSK of a standard 1.44 MB disk the
data of cylinders 0 to 79 take **1,515,776 bytes**: 256 bytes of header,
plus 160 tracks, each with a 256-byte Track Information Block and 9,216
bytes of data. Beyond that offset are the extra cylinders, where the disk
is not formatted and SAMdisk reads magnetic noise that differs at every
pass: differences there say nothing about the disk. Differences before it
are real data.

**Tracks that change size.** In an EDSK a track grows when SAMdisk saves
several copies of a sector that gave different data on successive
rereads: it means it met errors there and had to insist. Two reads of the
same track with different sizes show where that happened.

**Three or more reads.** `dskdiff` then counts, for each track, how many
different variants there are and whether one has a majority.

**Non-standard formats.** The number of cylinders is detected from the
formatted tracks and rounded down to the nearest standard geometry, so a
40-cylinder disk is not compared as if it had 80. `dskdiff -c N` forces
the number of cylinders when a disk really has a non-standard format.

## Checking a package later

After moving the archive, a package can be checked again with
`python3 tests/check_bag.py "D:\Archive\Monkey Island"` (any BagIt 1.0
validator works too).

## Editing a closed box

A closed box can be changed from the archive: select it and press
**Edit** (or Archive > Edit box...). The dialog has two tabs:

- **Box**: title, platform, publisher, year, language, edition, notes.
  Changing the title or the edition renames the folder, "Title (Edition)";
  the line at the bottom shows the new name.
- **Media**: how many floppies, CDs and DVDs, and one row per medium with
  its outcome and how it was read. Tick the media to read again, or to
  read if they are missing. A lower number removes the **last** media of
  that kind; file names never change.

**Save** shows what will change and asks for confirmation; the package
is checked, changed and closed again in one step. **Save and read** then
reads the ticked media one at a time, with the usual flow (reads,
comparison, EDC/ECC, redump). Each medium is saved as soon as it is read:

- the new read is better: it is kept, the old one goes to
  `set-aside/NAME.replaced`;
- it is worse: the old one stays, the new one goes to
  `set-aside/NAME.rejected`, and the window offers Retry (clean the disc);
- as good but different: the window asks which one to keep;
- identical: nothing changes.

The order, from best: verified by redump, clean read with DiscImageCreator
`cd` (or a clean floppy), clean read with `data`, marginal or unstable,
fragile or read once, with errors, failed.

Nothing is ever deleted: removed media go to `set-aside/NAME.removed`,
damaged files of a medium read again to `.damaged`, and the old manifests
and `metadata.json` to `set-aside/previous-DATE/`. The `history` list in
`metadata.json` says what changed, when, why (the optional Reason) and,
for a CD, with which drive and method. Stopping in the middle leaves the
medium in hand as it was; those already saved keep their new reads.

The change goes through a journal in the bag's `editing/` folder: after a
power cut the archive finishes it or puts the old box back when it opens,
never a mix. Boxes made by 261017 must be converted before they can be
edited.

From the console: `dskrun --reread cd1,cd2 -t "Windows 2000 Server"`
(or `--reread missing` to complete a closed incomplete box).

## SAMdisk traps already met

Verified by hand on the target PC.

- SAMdisk **3.x** is needed: the 4.0 alphas lost access to real floppy
  drives on Windows.
- `samdisk list` does not list floppies: to check that the drive answers,
  use `samdisk rpm a:` (about 300 rpm).
- The short form `-R5` is rejected ("unknown option -- 5"): the number of
  rescans only works in the long form, `--retries=30 --rescans=5`.
- `samdisk scan` also works on a `.dsk` file: that is how the scan of a
  test image is made without a floppy.
- The progress of `copy` (`Reading Cyl 3  Head 0`) is written only to a
  real console, never to a pipe: RetroArchiver runs SAMdisk in a hidden
  console for this reason.
