#!/usr/bin/env python3
"""Complete dskrun sessions with the fake SAMdisk: checks the bag,
metadata.json and archive.csv. Run by make test, from the tests folder."""
import hashlib
import json
import os
import shutil
import signal
import subprocess
import tempfile
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_bag  # noqa: E402

BUILD = os.path.join(HERE, "..", "build")
DSKRUN = os.path.join(BUILD, "dskrun")
FAKE = os.path.join(BUILD, "fake_samdisk")
FAKE_DIC = os.path.join(BUILD, "fake_dic")
FIX = os.path.join(HERE, "fixtures")
WORK = os.path.join(HERE, "tmp_session")
ARCH = os.path.join(WORK, "archivio")
PROFILE = os.path.join(HERE, "..", "docs", "bagit-profile.json")
NODAT = os.path.join(WORK, "senza-dat")         # no redump DAT
REDUMP = os.path.join(FIX, "redump")            # the test DAT from gen_dic.py

fails = 0
count = 0


def ok(desc, cond, extra=""):
    global fails, count
    count += 1
    if cond:
        print("  ok   " + desc)
    else:
        fails += 1
        print("  FAIL " + desc)
        for line in extra.splitlines():
            print("       | " + line)


def run(title, ndisk, script, keys, extra_args=(), arch=ARCH, tpl="disco{nn}", dic=None, dats=NODAT, env_extra=None):
    sfile = os.path.join(WORK, "script.txt")
    with open(sfile, "w") as f:
        f.write("\n".join(script) + "\n")
    if os.path.exists(sfile + ".pos"):
        os.remove(sfile + ".pos")
    env = dict(os.environ, FAKE_SAMDISK_DIR=FIX, FAKE_SAMDISK_SCRIPT=sfile,
               FAKE_DIC_DIR=os.path.join(FIX, "dic"))
    # CD and DVD reads: without a script the fake DIC always reads cd_good or dvd_good
    if dic is not None:
        dfile = os.path.join(WORK, "dic.txt")
        with open(dfile, "w") as f:
            f.write("\n".join(dic) + "\n")
        if os.path.exists(dfile + ".pos"):
            os.remove(dfile + ".pos")
        env["FAKE_DIC_SCRIPT"] = dfile
    env.update(env_extra or {})
    p = subprocess.run(
        [DSKRUN, str(ndisk), "-t", title, "-o", arch, "-x", FAKE, "-X", FAKE_DIC, "-d", "E:", "-R", dats]
        + (["-p", tpl] if tpl else []) + list(extra_args),
        input="".join(k + "\n" for k in keys).encode("utf-8"),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def bag(name):
    return os.path.join(ARCH, name)


def meta(b):
    with open(os.path.join(b, "data", "metadata.json"), encoding="utf-8") as f:
        return json.load(f)


def info(b):
    with open(os.path.join(b, "bag-info.txt"), encoding="utf-8") as f:
        return dict(line.rstrip("\n").split(": ", 1) for line in f)


def data_files(b):
    return sorted(os.listdir(os.path.join(b, "data")))


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def tree_sha(path):
    """the fingerprint of a read that is a folder: the "hash  path" lines in order"""
    if not os.path.isdir(path):
        return sha(path)
    rels = []
    for root, _, files in os.walk(path):
        for name in files:
            rels.append(os.path.relpath(os.path.join(root, name), path).replace(os.sep, "/"))
    text = "".join("%s  %s\n" % (sha(os.path.join(path, r)), r) for r in sorted(rels, key=lambda r: r.encode()))
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


shutil.rmtree(WORK, ignore_errors=True)
os.makedirs(ARCH)
os.makedirs(NODAT)
# the log of version 261017 (and dskrun 1.0): it stays as it is, the new one is archive.csv
with open(os.path.join(ARCH, "archivio.csv"), "w") as f:
    f.write("disco;master;letture;esito;quando\n1;D:\\x.dsk;2;pulito;2026-01-01 10:00\n")

print("\ndskrun sessions")

# 1. healthy two-disk box, with label and FAT12
rc, out = run("Sano", 2, ["ident_a", "ident_b", "fat_144", "fat_144"], ["Disk 1", ""])
b = bag("Sano")
ok("healthy box: closed", rc == 0, out)
ok("healthy box: valid bag", check_bag.check(b, PROFILE) == [], out)
ok("healthy box: only the masters in data/", data_files(b) == [
    "disco01.diff.txt", "disco01.dsk", "disco01.scan.txt",
    "disco02.diff.txt", "disco02.dsk", "disco02.scan.txt", "metadata.json"], str(data_files(b)))
with open(os.path.join(b, "data", "disco01.diff.txt"), encoding="utf-8") as f:
    rep_txt = f.read()
ok("healthy box: the .diff.txt has names relative to data/, not PC paths",
   "  A: disco01.dsk " in rep_txt and "  B: disco01_b.dsk " in rep_txt and ARCH not in rep_txt, rep_txt)
m = meta(b)
ok("healthy box: clean statuses", [d["status"] for d in m["media"]] == ["clean", "clean"])
ok("healthy box: label of disk 1", m["media"][0]["label"] == "Disk 1")
ok("healthy box: empty label of disk 2 taken from the FAT12", m["media"][1]["label"] == "MONKEY3"
   and "Label of floppy 2 taken from the disk: MONKEY3" in out, json.dumps(m["media"][1]["label"]))
ok("healthy box: FAT12 of disk 2",
   (m["media"][1].get("filesystem") or {}).get("volume_label") == "MONKEY3", json.dumps(m["media"][1]))
ok("healthy box: 1.44M format", m["media"][1]["geometry"]["format"] == "1.44M")
ok("healthy box: Box-Status complete", info(b).get("Box-Status") == "complete")
with open(os.path.join(ARCH, "archivio.csv"), encoding="utf-8") as f:
    old_rows = f.read().splitlines()
ok("log: the old archivio.csv is left as it was", len(old_rows) == 2 and old_rows[0] == "disco;master;letture;esito;quando")
with open(os.path.join(ARCH, "archive.csv"), encoding="utf-8") as f:
    rows = f.read().splitlines()
ok("log: archive.csv with the English header and two rows", len(rows) == 3
   and rows[0] == "disk;master;reads;status;when;box"
   and rows[1].startswith("1;disco01.dsk;1;clean;") and rows[1].endswith(";Sano")
   and rows[2].startswith("2;disco02.dsk;1;clean;") and rows[2].endswith(";Sano"),
   "\n".join(rows))

# 2. marginal disk: three reads, all kept
rc, out = run("Marginale", 1, ["weak_1", "weak_2", "weak_3"], [""])
b = bag("Marginale")
m = meta(b)
ok("marginal: closed and valid", rc == 0 and check_bag.check(b, PROFILE) == [], out)
ok("marginal: status", m["media"][0]["status"] == "marginal")
ok("marginal: three reads kept",
   [r["file"] for r in m["media"][0]["reads"]] == ["disco01.dsk", "disco01_b.dsk", "disco01_c.dsk"])
with open(os.path.join(b, "data", "disco01.diff.txt"), encoding="utf-8") as f:
    rep_txt = f.read()
ok("marginal: the comparison in the bag names the reads without PC paths",
   "  1) disco01.dsk " in rep_txt and "  3) disco01_c.dsk " in rep_txt
   and ARCH not in rep_txt and os.sep + "data" + os.sep not in rep_txt, rep_txt)
ok("marginal: verdict", m["media"][0]["compare"]["verdict"] == "unstable"
   and m["media"][0]["compare"]["agree"] == 1
   and m["media"][0]["compare"]["unstable_tracks"] == 2, json.dumps(m["media"][0]["compare"]))

# 3. failed read: the bag stays open
rc, out = run("Fallita", 1, ["FAIL"], [""])
b = bag("Fallita")
ok("failed read: bag open", rc == 1 and os.path.exists(os.path.join(b, ".in-progress")), out)
ok("failed read: status", meta(b)["media"][0]["status"] == "read-failed")
ok("failed read: no image", not [f for f in data_files(b) if f.endswith(".dsk")])

# 4. skip and close: incomplete but valid box
rc, out = run("Saltati", 2, [], ["S", "F"])
b = bag("Saltati")
ok("incomplete: closed and valid", rc == 0 and check_bag.check(b, PROFILE) == [], out)
ok("incomplete: Box-Status", info(b).get("Box-Status") == "incomplete")
ok("incomplete: Missing-Media", info(b).get("Missing-Media") == "floppy 1, floppy 2", str(info(b)))
ok("incomplete: Media-Type", info(b).get("Media-Type") == "2 floppy", str(info(b)))

# 5. resume after quitting, with an orphan file left in data/
rc, out = run("Ripreso", 2, ["ident_a", "ident_b"], ["", "Q"])
b = bag("Ripreso")
ok("resume: first part left open", rc == 1, out)
with open(os.path.join(b, "data", "disco02_b.dsk"), "w") as f:
    f.write("resto di un'interruzione")
# disk 2 is another disk: another image, or it would be a duplicate of disk 1
rc, out = run("Ripreso", 2, ["good_2", "good_2"], [""])
ok("resume: closed and valid", rc == 0 and check_bag.check(b, PROFILE) == [], out)
ok("resume: orphan removed with a warning", "Removing data/disco02_b.dsk" in out, out)
ok("resume: both disks clean", [d["status"] for d in meta(b)["media"]] == ["clean", "clean"])
ok("resume: images intact",
   sha(os.path.join(b, "data", "disco01.dsk")) == sha(os.path.join(FIX, "ident_a.dsk")) and
   sha(os.path.join(b, "data", "disco02.dsk")) == sha(os.path.join(FIX, "good_2.dsk")))

# 6. title with an accent and characters forbidden in folder names
title = "Citt\u00e0: parte 1/2"
rc, out = run(title, 1, ["ident_a", "ident_b"], [""])
b = bag("Citt\u00e0_ parte 1_2")
ok("title with special characters: folder name cleaned", rc == 0 and os.path.isdir(b), out)
ok("title with special characters: real title in the bag", info(b).get("Title") == title)
ok("accented title: on a pipe the output stays UTF-8", "  Box: " + title + "\n" in out, out)

# 7. input closed: counts as quitting
rc, out = run("Eof", 1, ["ident_a", "ident_b"], [])
ok("input closed: bag open", rc == 1 and "left open" in out, out)

# 8. box already closed
rc, out = run("Sano", 2, [], [""])
ok("box already closed: clear error", rc == 2 and "is already closed" in out, out)

# 10. partial read: the half-written file does not enter the closed bag
rc, out = run("Parziale", 2, ["ident_a", "PARTIAL"], ["", "F"])
b = bag("Parziale")
ok("partial: closed and valid", rc == 0 and check_bag.check(b, PROFILE) == [], out)
ok("partial: no half-written file in data/", "disco01_b.dsk" not in data_files(b), str(data_files(b)))

# 11. naming scheme that gives the same name to different disks
rc, out = run("Collisione", 2, [], [""], ["-p", "{set}"])
ok("collision: clear error, nothing created", rc == 2 and "the same name" in out
   and not os.path.exists(bag("Collisione")), out)

# 11b. long title: the cut at 77 bytes makes disk 10 coincide with 1
longt = "A" * 76
rc, out = run(longt, 10, [], [""], ["-p", "{set}{n}"])
ok("long collision: clear error, nothing created", rc == 2 and "the same name" in out
   and not os.path.exists(bag(longt)), out)

# 12. dying disk: a new attempt does not delete the only good read
rc, out = run("Morente", 1, ["ident_a", "FAIL"], [""])
b = bag("Morente")
good = os.path.join(b, "data", "disco01.dsk")
ok("dying: first round open with one read", rc == 1 and os.path.exists(good), out)
first = sha(good) if os.path.exists(good) else None
rc, out = run("Morente", 1, ["FAIL"], [""])
d1 = meta(b)["media"][0]
ok("dying: after the second attempt the good read is still there",
   os.path.exists(good) and sha(good) == first, out)
ok("dying: the good read stays in metadata.json",
   [r["file"] for r in d1["reads"]] == ["disco01.dsk"] and d1["reads"][0]["sha256"] == first
   and d1["master"] == "disco01.dsk" and d1["status"] == "read-failed", json.dumps(d1))
rc, out = run("Morente", 1, [], ["S"])
d1 = meta(b)["media"][0]
ok("dying: skipping it does not delete the read",
   os.path.exists(good) and d1["status"] == "read-failed"
   and [r["file"] for r in d1["reads"]] == ["disco01.dsk"], json.dumps(d1) + "\n" + out)
rc, out = run("Morente", 1, ["ident_b"], [""])
d1 = meta(b)["media"][0]
ok("dying: the new read is compared with the old one and closes",
   rc == 0 and check_bag.check(b, PROFILE) == [] and d1["status"] == "clean"
   and d1["master"] == "disco01.dsk" and sha(good) == first, json.dumps(d1) + "\n" + out)
ok("dying: the extra read removed because the disk is clean",
   [f for f in data_files(b) if f.endswith(".dsk")] == ["disco01.dsk"], str(data_files(b)))

# 12b. repeated attempts: beyond META_MAXREAD (6) the oldest are kept
for k in range(5):
    rc, out = run("Tentativi", 1, ["weak_%d" % (k % 3 + 1), "FAIL"], [""])
b = bag("Tentativi")
rc, out = run("Tentativi", 1, ["weak_3"], [""])
d1 = meta(b)["media"][0]
ok("attempts: at most six reads, with a warning", rc == 0 and "the maximum" in out
   and len(d1["reads"]) == 6 and d1["status"] == "marginal", json.dumps(d1) + "\n" + out)
ok("attempts: all six reads are there",
   [f for f in data_files(b) if f.endswith(".dsk")] == ["disco01.dsk"] +
   ["disco01_%s.dsk" % c for c in "bcdef"] and check_bag.check(b, PROFILE) == [], str(data_files(b)))

# 13. two titles that give the same folder: the second does not touch the first
rc, out = run("Prova: A", 2, ["ident_a", "ident_b"], ["", "Q"])
b = bag("Prova_ A")
before = open(os.path.join(b, "data", "metadata.json"), "rb").read()
rc, out = run("Prova_ A", 2, ["ident_a", "ident_b"], ["", ""])
ok("different title, same folder: clear error", rc == 2 and "holds the box" in out, out)
ok("different title, same folder: the first stays open and intact",
   os.path.exists(os.path.join(b, ".in-progress"))
   and open(os.path.join(b, "data", "metadata.json"), "rb").read() == before
   and meta(b)["box"]["title"] == "Prova: A", out)
# (each disk of a box gets its own image: the same one would be a duplicate)
rc, out = run("Prova: A", 3, ["good_2", "good_2"], [""])
ok("different number of disks: the saved one wins, with a warning",
   rc == 0 and "opened with 2 floppy: going on with 2 floppy" in out
   and meta(b)["box"]["media_expected"] == 2, out)

# 14. image changed after the read: set aside, the disk is read again
rc, out = run("Impronta", 2, ["ident_a", "ident_b"], ["", "Q"])
b = bag("Impronta")
img = os.path.join(b, "data", "disco01.dsk")
with open(img, "r+b") as f:
    f.seek(1000)
    c = f.read(1)
    f.seek(1000)
    f.write(bytes([c[0] ^ 0xFF]))
rc, out = run("Impronta", 2, ["good_2", "good_2"], [""])
d1 = meta(b)["media"][0]
ok("changed hash: stays open and says to read disk 1 again",
   rc == 1 and "read floppy 1 again" in out and os.path.exists(os.path.join(b, ".in-progress")), out)
aside = os.path.join(b, "set-aside", "disco01.dsk.damaged")
ok("changed hash: the file set aside outside data/, not deleted",
   os.path.exists(aside) and not os.path.exists(img)
   and not [f for f in data_files(b) if "damaged" in f]
   and "as set-aside/disco01.dsk.damaged, out of the data package" in out,
   str(data_files(b)) + "\n" + out)
ok("changed hash: disk 1 goes back to unread",
   d1["status"] == "to-read" and d1["reads"] == [] and d1["master"] is None, json.dumps(d1))
# a .danneggiato left in data/ by the previous version: it is moved too
old_aside = os.path.join(b, "data", "disco02.dsk.danneggiato")
with open(old_aside, "w") as f:
    f.write("vecchio\n")
rc, out = run("Impronta", 2, ["ident_a", "ident_b"], [""])
with open(os.path.join(b, "manifest-sha256.txt"), encoding="utf-8") as f:
    man = f.read()
ok("changed hash: disk 1 read again, the box closes valid",
   rc == 0 and check_bag.check(b, PROFILE) == []
   and [d["status"] for d in meta(b)["media"]] == ["clean", "clean"], out)
ok("changed hash: no .danneggiato in the manifest, the files set aside are there",
   "damaged" not in man and "danneggiato" not in man and os.path.exists(aside)
   and os.path.exists(os.path.join(b, "set-aside", "disco02.dsk.danneggiato"))
   and not os.path.exists(old_aside) and "Moving data/disco02.dsk.danneggiato to set-aside/" in out,
   man + "\n" + out)
ok("changed hash: at the end of the box the list of files set aside with the path",
   "Set aside, out of the data package" in out and "2 file(s)" in out
   and os.path.join(b, "set-aside", "disco01.dsk.damaged") in out
   and os.path.join(b, "set-aside", "disco02.dsk.danneggiato") in out, out)
ok("box without reads set aside: no list", "Messi da parte" not in run("Senza scarti", 1,
   ["ident_a", "ident_b"], [""])[1])

# 15. SAMdisk missing: it is not a damaged diskette
rc, out = run("Senza SAMdisk", 2, [], ["", ""], ["-x", os.path.join(WORK, "manca", "samdisk")])
b = bag("Senza SAMdisk")
ok("no SAMdisk: open, with the instruction", rc == 1 and "Cannot find" in out
   and "Insert floppy 2" not in out, out)
ok("no SAMdisk: disk 1 stays unread",
   meta(b)["media"][0]["status"] == "to-read" and meta(b)["media"][0]["reads"] == [],
   json.dumps(meta(b)["media"]))

# 16. Ctrl+C during a read: SAMdisk stopped, box left open
if os.name == "posix":
    sfile = os.path.join(WORK, "script.txt")
    with open(sfile, "w") as f:
        f.write("SLOW\n")
    if os.path.exists(sfile + ".pos"):
        os.remove(sfile + ".pos")
    env = dict(os.environ, FAKE_SAMDISK_DIR=FIX, FAKE_SAMDISK_SCRIPT=sfile)
    t0 = time.time()
    p = subprocess.Popen([DSKRUN, "2", "-t", "CtrlC", "-o", ARCH, "-p", "disco{nn}", "-x", FAKE],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    p.stdin.write(b"\n\n")
    p.stdin.flush()
    seen, lines = [], []
    for raw in p.stdout:
        line = raw.decode("utf-8", "replace")
        lines.append(line)
        if "Read 1" in line:
            seen.append("lettura")
        if seen and "fake_samdisk: slow" in line:
            p.send_signal(signal.SIGINT)
            break
    out = "".join(lines) + p.stdout.read().decode("utf-8", "replace")
    rc = p.wait(timeout=20)
    left = subprocess.run(["pgrep", "-f", FAKE], stdout=subprocess.PIPE).stdout.decode().split()
    d1 = meta(bag("CtrlC"))["media"][0]
    ok("Ctrl+C: box open, disk interrupted, no disk 2",
       rc == 1 and d1["status"] == "interrupted" and "Insert floppy 2" not in out
       and "start again to resume from floppy 1" in out, out)
    ok("Ctrl+C: SAMdisk stopped at once", not left and time.time() - t0 < 4.5, str(left))

# 16b. Ctrl+C during the comparison (slowed down on purpose): the disk is
#      closed, it stops before the next one
if os.name == "posix":
    sfile = os.path.join(WORK, "script.txt")
    with open(sfile, "w") as f:
        f.write("ident_a\nident_b\n")
    if os.path.exists(sfile + ".pos"):
        os.remove(sfile + ".pos")
    env = dict(os.environ, FAKE_SAMDISK_DIR=FIX, FAKE_SAMDISK_SCRIPT=sfile, RETROARCHIVER_CONFRONTO_LENTO="2000")
    t0 = time.time()
    p = subprocess.Popen([DSKRUN, "2", "-t", "CtrlC confronto", "-o", ARCH, "-p", "disco{nn}", "-x", FAKE],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    p.stdin.write(b"\n\n")
    p.stdin.flush()
    lines = []
    for raw in p.stdout:
        line = raw.decode("utf-8", "replace")
        lines.append(line)
        if "--- floppy 1: Compare ---" in line:
            time.sleep(0.3)
            p.send_signal(signal.SIGINT)
            break
    out = "".join(lines) + p.stdout.read().decode("utf-8", "replace")
    rc = p.wait(timeout=20)
    m = meta(bag("CtrlC confronto"))
    ok("Ctrl+C in the comparison: disk 1 closes clean, no disk 2",
       rc == 1 and m["media"][0]["status"] == "clean" and "Insert floppy 2" not in out
       and "Stopped: start again to resume from floppy 2" in out and time.time() - t0 >= 2, out)

# 17. closed with F: disks never asked for are reported missing
rc, out = run("Chiuso presto", 3, ["ident_a", "ident_b"], ["", "F"])
m = meta(bag("Chiuso presto"))
ok("F: all disks in metadata.json, the unread ones missing",
   rc == 0 and [(d["n"], d["status"]) for d in m["media"]] ==
   [(1, "clean"), (2, "missing"), (3, "missing")], json.dumps(m["media"]))

# 18. a leftover that cannot be removed: the box does not close
# (in a system folder: under WSL's /mnt/c chmod has no effect)
if os.name == "posix" and os.geteuid() != 0:
    tmparch = tempfile.mkdtemp(prefix="ra_spazzino_")
    rc, out = run("Spazzino", 2, ["ident_a", "ident_b"], ["", "Q"], arch=tmparch)
    b = os.path.join(tmparch, "Spazzino")
    with open(os.path.join(b, "data", "avanzo.tmp"), "w") as f:
        f.write("x")
    os.chmod(os.path.join(b, "data"), 0o555)
    try:
        rc, out = run("Spazzino", 2, [], ["F"], arch=tmparch)
    finally:
        os.chmod(os.path.join(b, "data"), 0o755)
    ok("stuck leftover: clear error, box open",
       rc == 1 and "Cannot remove data/avanzo.tmp" in out
       and os.path.exists(os.path.join(b, ".in-progress"))
       and not os.path.exists(os.path.join(b, "manifest-sha256.txt")), out)
    shutil.rmtree(tmparch, ignore_errors=True)

# 19. box folder opened but without metadata.json (first save failed)
os.makedirs(os.path.join(bag("Senza metadati"), "data"))
with open(os.path.join(bag("Senza metadati"), "bagit.txt"), "w") as f:
    f.write("BagIt-Version: 1.0\nTag-File-Character-Encoding: UTF-8\n")
with open(os.path.join(bag("Senza metadati"), ".in-corso"), "w") as f:     # the marker of 261017
    f.write("x\n")
rc, out = run("Senza metadati", 1, ["ident_a", "ident_b"], [""])
ok("no metadata.json: starts over and closes",
   rc == 0 and check_bag.check(bag("Senza metadati"), PROFILE) == [], out)

# 20. archive folder with intermediate folders to create
deep = os.path.join(WORK, "nuova", "sotto", "archivio")
rc, out = run("Profondo", 1, ["ident_a", "ident_b"], [""], arch=deep)
ok("intermediate folders: created, box closed",
   rc == 0 and check_bag.check(os.path.join(deep, "Profondo"), PROFILE) == [], out)

# 21. output not on a console (file, pipe): no beep in the text
rc, out = run("Bip", 1, ["ident_a", "ident_b"], [""])
ok("no beep outside the console", rc == 0 and "\a" not in out, repr(out[:200]))

# 22. -a: read from an image file instead of a: (test with the real SAMdisk)
rc, out = run("Da immagine", 1, [], [""], ["-a", os.path.join(FIX, "fat_144.dsk")])
b = bag("Da immagine")
ok("-a: box read from an image", rc == 0 and check_bag.check(b, PROFILE) == []
   and sha(os.path.join(b, "data", "disco01.dsk")) == sha(os.path.join(FIX, "fat_144.dsk")), out)
ok("-a: the source is shown on screen", "Reading from:" in out, out)

# 23. fragile disk: the survey sees defective sectors, a single careful read
rc, out = run("Fragile", 2, ["BAD fat_144", "ident_a", "ident_b"], ["", ""])
b = bag("Fragile")
m = meta(b)
ok("fragile: box closed and valid", rc == 0 and check_bag.check(b, PROFILE) == [], out)
ok("fragile: the defective disk is read only once",
   m["media"][0]["status"] == "fragile" and len(m["media"][0]["reads"]) == 1, json.dumps(m["media"][0]))
ok("fragile: read with strong retries", "careful read --retries=30 --rescans=5" in out and "fragile disk" in out, out)
ok("fragile: the healthy disk follows the normal round",
   m["media"][1]["status"] == "clean" and "careful read" not in out.split("floppy 2:")[-1], out)

# 24. interrupted disk resumed and skipped: moves on to the next disk, does not quit
rc, out = run("Salto interrotto", 2, ["ident_a", "ident_b"], ["", "Q"])
b = bag("Salto interrotto")
mp = os.path.join(b, "data", "metadata.json")
with open(mp, encoding="utf-8") as f:
    m = json.load(f)
m["media"][0]["status"] = "interrotto"      # as 261017 wrote it after a Ctrl+C: Italian, still read
with open(mp, "w", encoding="utf-8") as f:
    json.dump(m, f)
rc, out = run("Salto interrotto", 2, ["good_2", "good_2"], ["S", ""])
m = meta(b)
ok("skip an interrupted disk: disk 2 is asked for and read",
   "Insert floppy 2" in out and m["media"][1]["status"] == "clean"
   and "resume from floppy 1" not in out, out)
ok("skip an interrupted disk: disk 1 keeps reads and status (the Italian one read as English)",
   m["media"][0]["status"] == "interrupted" and len(m["media"][0]["reads"]) == 1 and rc == 1,
   json.dumps(m["media"][0]))

# 25. -r: a failed disk is asked for again at once, the second attempt succeeds
rc, out = run("Riprova", 1, ["FAIL", "ident_a", "ident_b"], ["", ""], ["-r"])
b = bag("Riprova")
ok("-r: disk 1 asked for again after the failure", out.count("Insert floppy 1") == 2, out)
ok("-r: second attempt clean, box closed and valid",
   rc == 0 and check_bag.check(b, PROFILE) == [] and meta(b)["media"][0]["status"] == "clean", out)
ok("-r: in the summary the retried disk counts once",
   "1 clean, 0 marginal, 0 with errors, 0 skipped or to redo" in out.split("Box closed.")[-1], out)

# 25b. -r: the good read from before the failure stays and is compared with the new one
rc, out = run("Riprova parziale", 1, ["ident_a", "FAIL", "ident_a"], ["", ""], ["-r"])
b = bag("Riprova parziale")
d1 = meta(b)["media"][0]
ok("-r: the first read stays and is compared with the new attempt",
   rc == 0 and check_bag.check(b, PROFILE) == [] and d1["status"] == "clean"
   and "already has 1 reads" in out and d1["master"] == "disco01.dsk"
   and sha(os.path.join(b, "data", "disco01.dsk")) == sha(os.path.join(FIX, "ident_a.dsk")),
   json.dumps(d1) + "\n" + out)

# 26. -r and Salta after a failure: moves on to the next disk
rc, out = run("Riprova saltata", 2, ["FAIL", "ident_a", "ident_b"], ["", "S", ""], ["-r"])
m = meta(bag("Riprova saltata"))
ok("-r and Skip: disk 2 is read, disk 1 stays missing",
   rc == 1 and out.count("Insert floppy 1") == 2 and "Insert floppy 2" in out
   and [d["status"] for d in m["media"]] == ["missing", "clean"], json.dumps(m["media"]) + "\n" + out)

# 27. mixed box: three floppies with the fake SAMdisk, then Q on the CD's turn
rc, out = run("Windows NT Workstation 4.0", 3, ["ident_a", "ident_b", "good_2", "good_2", "good_3", "good_3"],
              ["Setup Disk 1", "Setup Disk 2", "Setup Disk 3", "Q"],
              ["-C", "1", "-P", "Windows NT"], tpl=None)
b = bag("Windows NT Workstation 4.0")
m = meta(b)
ok("mixed: after the floppies the CD is asked for", rc == 1 and "Insert CD 1 of 1" in out
   and os.path.exists(os.path.join(b, ".in-progress")), out)
ok("mixed: retroarchiver-3 schema with the composition",
   m["schema"] == "retroarchiver-3" and m["box"]["media_expected"] == 4
   and m["box"]["media"] == [{"type": "floppy", "count": 3}, {"type": "cd", "count": 1}]
   and m["box"]["names"] == {"floppy": "FLOPPY{nn}", "cd": "CD{nn}"}, json.dumps(m["box"]))
ok("mixed: type and number of each medium",
   [(d["n"], d["type"], d["number"], d["status"]) for d in m["media"]] ==
   [(1, "floppy", 1, "clean"), (2, "floppy", 2, "clean"), (3, "floppy", 3, "clean"),
    (4, "cd", 1, "to-read")], json.dumps(m["media"]))
ok("mixed: fixed names FLOPPY01..03, the CD not yet",
   [f for f in data_files(b) if f.endswith(".dsk")] == ["FLOPPY01.dsk", "FLOPPY02.dsk", "FLOPPY03.dsk"]
   and not os.path.exists(os.path.join(b, "data", "CD01")), str(data_files(b)))
# resumed: only the CD is asked for, and it is read twice (no redump DAT)
rc, out = run("Windows NT Workstation 4.0", 3, [], ["NTWKS40_IT"], ["-C", "1"], tpl=None,
              dic=["cd_good", "cd_good"])
m = meta(b)
cd = m["media"][3]
ok("mixed resumed: closed complete and valid", rc == 0 and check_bag.check(b, PROFILE) == []
   and "Insert floppy" not in out and info(b).get("Box-Status") == "complete", out)
ok("mixed: the CD phases", all(x in out for x in ("--- CD 1: TOC ---", "--- CD 1: Read 1 ---",
   "--- CD 1: C2 check ---", "--- CD 1: Redump ---", "--- CD 1: Read 2 ---",
   "--- CD 1: Compare ---")), out)
ok("mixed: CD clean, one read kept", cd["status"] == "clean" and cd["label"] == "NTWKS40_IT"
   and [r["file"] for r in cd["reads"]] == ["CD01"] and cd["master"] == "CD01"
   and cd["compare"]["verdict"] == "identical" and cd["compare"]["agree"] == 2, json.dumps(cd))
ok("mixed: the DIC files in data/CD01, the second read removed",
   os.path.isfile(os.path.join(b, "data", "CD01", "CD01.cue"))
   and os.path.isfile(os.path.join(b, "data", "CD01", "CD01 (Track 2).bin"))
   and os.path.isfile(os.path.join(b, "data", "CD01", "dic-console.txt"))
   and not os.path.exists(os.path.join(b, "data", "CD01_b"))
   and "CD01.scan.txt" in data_files(b) and "CD01.diff.txt" in data_files(b), str(data_files(b)))
ok("mixed: hash of the CD folder", tree_sha(os.path.join(b, "data", "CD01")) == cd["reads"][0]["sha256"]
   and cd["reads"][0]["bytes"] == sum(os.path.getsize(os.path.join(r, f)) for r, _, fs
                                      in os.walk(os.path.join(b, "data", "CD01")) for f in fs), json.dumps(cd["reads"]))
disc = cd.get("disc") or {}
ok("mixed: technical data of the CD", [t["mode"] for t in disc.get("tracks", [])] == ["MODE1/2352", "AUDIO"]
   and disc.get("sectors") == 420 and disc.get("c2_errors") == 0 and disc.get("redump") is None
   and (cd.get("filesystem") or {}).get("volume_label") == "PROVA_CD"
   and any(f["name"] == "GIOCO/DATI/MONDO.DAT" for f in cd["filesystem"]["files"]), json.dumps(cd))
with open(os.path.join(b, "manifest-sha256.txt"), encoding="utf-8") as f:
    man = f.read()
ok("mixed: the manifest lists the CD files", "  data/CD01/CD01 (Track 1).bin\n" in man, man)
i = info(b)
ok("mixed: bag-info with the composition",
   i.get("Media-Type") == "3 floppy, 1 CD" and i.get("Media-Expected") == "4"
   and i.get("Media-Archived") == "4" and "Missing-Media" not in i and i.get("Platform") == "Windows NT", str(i))
with open(os.path.join(ARCH, "archive.csv"), encoding="utf-8") as f:
    rows = f.read().splitlines()
ok("mixed: CD row in archive.csv",
   any(r.startswith("4;CD01;1;clean;") and r.endswith(";Windows NT Workstation 4.0") for r in rows), "\n".join(rows))

# 27b. a CD with C2 errors (and SafeDisc) that redump does not know: two equal reads, with errors
rc, out = run("CD rovinato", 0, [], [""], ["-C", "1"], tpl=None, dic=["cd_c2", "cd_c2"])
b = bag("CD rovinato")
m = meta(b)
cd = m["media"][0]
ok("CD with errors: closed and valid", rc == 0 and check_bag.check(b, PROFILE) == [], out)
ok("CD with errors: errors status, both reads kept", cd["status"] == "errors"
   and [r["file"] for r in cd["reads"]] == ["CD01", "CD01_b"]
   and os.path.isdir(os.path.join(b, "data", "CD01_b")), json.dumps(cd))
ok("CD with errors: C2 and protection in the technical data", (cd.get("disc") or {}).get("c2_errors") == 3
   and cd["disc"].get("protection") == "SafeDisc", json.dumps(cd.get("disc")))
with open(os.path.join(b, "data", "CD01.diff.txt"), encoding="utf-8") as f:
    rep_txt = f.read()
ok("CD with errors: the comparison says what to do", "unresolved errors" in rep_txt
   and "Try another drive" in rep_txt, rep_txt)

# 27c. the same CD present in the redump DAT: one read is enough
rc, out = run("CD redump", 0, [], [""], ["-C", "1"], tpl=None, dic=["cd_c2"], dats=REDUMP)
b = bag("CD redump")
m = meta(b)
cd = m["media"][0]
ok("CD in redump: closed with one read", rc == 0 and check_bag.check(b, PROFILE) == []
   and cd["status"] == "redump-verified" and len(cd["reads"]) == 1
   and "Read 2" not in out and "Redump DAT: " in out, out)
ok("CD in redump: the game in the metadata", cd["compare"]["verdict"] == "redump"
   and ((cd.get("disc") or {}).get("redump") or {}).get("game") == "Gioco di Prova (Italy)"
   and cd["disc"]["redump"]["dat_date"] == "2026-09-01 10-00-00", json.dumps(cd))

# 27d. reads that do not match: three reads, two equal, unstable
rc, out = run("CD instabile", 0, [], [""], ["-C", "1"], tpl=None, dic=["cd_good", "cd_noise", "cd_noise"])
b = bag("CD instabile")
cd = meta(b)["media"][0]
ok("unstable CD: three reads kept", rc == 0 and cd["status"] == "unstable"
   and [r["file"] for r in cd["reads"]] == ["CD01", "CD01_b", "CD01_c"]
   and cd["compare"]["agree"] == 2 and check_bag.check(b, PROFILE) == [], json.dumps(cd) + out)
ok("unstable CD: after the second read another one is made", "The reads do not match" in out, out)

# 27e. the second read fails: the CD stays read once
rc, out = run("CD una volta", 0, [], [""], ["-C", "1"], tpl=None, dic=["cd_good", "FAIL"])
cd = meta(bag("CD una volta"))["media"][0]
ok("CD read once: status and one read", rc == 0 and cd["status"] == "read-once"
   and [r["file"] for r in cd["reads"]] == ["CD01"]
   and not os.path.exists(os.path.join(bag("CD una volta"), "data", "CD01_b")), json.dumps(cd) + out)

# 27f. a DVD in the drive instead of the CD: read failed, the box stays open
rc, out = run("CD sbagliato", 0, [], [""], ["-C", "1"], tpl=None, dic=["WRONG"])
cd = meta(bag("CD sbagliato"))["media"][0]
ok("wrong disc: failed read with the explanation", rc == 1 and cd["status"] == "read-failed"
   and "The drive does not hold a CD" in out
   and not os.path.exists(os.path.join(bag("CD sbagliato"), "data", "CD01")), json.dumps(cd) + out)
rc, out = run("CD senza lead-in", 0, [], [""], ["-C", "1"], tpl=None, dic=["LEADIN"])
cd = meta(bag("CD senza lead-in"))["media"][0]
ok("drive without lead-in: clear reason, no copy protection scan proposed",
   rc == 1 and cd["status"] == "read-failed" and "cannot read the lead-in" in out
   and "copy protection scan would not help" in out, json.dumps(cd) + out)

failed = os.path.join(bag("CD sbagliato"), "set-aside", "CD01.failed")
ok("wrong disc: DIC's logs kept in set-aside, not deleted",
   os.path.isfile(os.path.join(failed, "dic-console.txt")) and "set-aside/CD01.failed" in out
   and check_bag.check(bag("CD sbagliato"), PROFILE) != None, out)

# 27g. DVD in redump: the .iso in its folder
rc, out = run("Solo DVD", 0, [], [""], ["-D", "1"], tpl=None, dic=["dvd_good"], dats=REDUMP)
b = bag("Solo DVD")
dvd = meta(b)["media"][0]
ok("DVD: redump verified and valid", rc == 0 and dvd["status"] == "redump-verified"
   and os.path.isfile(os.path.join(b, "data", "DVD01", "DVD01.iso"))
   and check_bag.check(b, PROFILE) == [], json.dumps(dvd) + out)
ok("DVD: file system and game", (dvd.get("filesystem") or {}).get("volume_label") == "PROVA_CD"
   and dvd["disc"]["redump"]["game"] == "Gioco & DVD di Prova (Italy)", json.dumps(dvd))
ok("DVD: empty label taken from the ISO 9660", dvd["label"] == "PROVA_CD", json.dumps(dvd["label"]))

# 27g2. DVD with Joliet: long names and label in UTF-8 in the metadata
rc, out = run("DVD Joliet", 0, [], [""], ["-D", "1"], tpl=None, dic=["dvd_joliet", "dvd_joliet"])
fs = (meta(bag("DVD Joliet"))["media"][0].get("filesystem") or {})
ok("Joliet: label and long accented names in the metadata", rc == 0
   and fs.get("names") == "Joliet" and fs.get("volume_label") == "Prova città"
   and any(f["name"] == "Gioco completo/Dati del mondo/Città del mondo – versione italiana.dat"
           and f["bytes"] == 5000 for f in fs.get("files", []))
   and "filesystem_note" not in meta(bag("DVD Joliet"))["media"][0], json.dumps(fs, ensure_ascii=False) + out)
ok("no Joliet: 8.3 names, and it says so", (dvd.get("filesystem") or {}).get("names") == "ISO 9660"
   and "has no Joliet" in dvd.get("filesystem_note", ""), json.dumps(dvd))

# 27h. DiscImageCreator missing: the box stays open and says so
rc, out = run("Senza DIC", 0, [], [""], ["-C", "1", "-X", os.path.join(WORK, "dic-che-non-esiste")], tpl=None)
cd = meta(bag("Senza DIC"))["media"][0]
ok("no DIC: open, CD unread", rc == 1 and cd["status"] == "to-read"
   and "Cannot find" in out and "once DiscImageCreator is fixed" in out, json.dumps(cd) + out)

# 27i. -f and -C without floppies: only a CD, closed
rc, out = run("Solo CD", 0, [], [""], ["-C", "1"], tpl=None)
m = meta(bag("Solo CD"))
ok("CD only: closed, CD clean",
   rc == 0 and [(d["type"], d["status"]) for d in m["media"]] == [("cd", "clean")], out)
rc, out = run("Composizione vuota", 0, [], [""], tpl=None)
ok("empty composition: usage instructions", rc == 2 and "-C N" in out, out)

# 27j. CD in redump read badly the first time: the second read is the
#      redump one, becomes the master and the first is removed
rc, out = run("CD redump alla seconda", 0, [], [""], ["-C", "1"], tpl=None, dic=["cd_noise", "cd_good"],
              dats=REDUMP)
b = bag("CD redump alla seconda")
cd = meta(b)["media"][0]
ok("redump on the second read: verified, the second read is the master", rc == 0 and cd["status"] == "redump-verified"
   and cd["master"] == "CD01_b" and [r["file"] for r in cd["reads"]] == ["CD01_b"]
   and not os.path.exists(os.path.join(b, "data", "CD01")) and check_bag.check(b, PROFILE) == [], json.dumps(cd) + out)
ok("redump on the second read: the redump data are the master's",
   ((cd.get("disc") or {}).get("redump") or {}).get("game") == "Gioco di Prova (Italy)", json.dumps(cd))

# 27k. DIC exits with an error but the read is complete: it is kept and counts as an error
rc, out = run("CD uscita 3", 0, [], [""], ["-C", "1"], tpl=None, dic=["EXIT3 cd_good", "cd_good"])
cd = meta(bag("CD uscita 3"))["media"][0]
ok("DIC exits with an error: read kept, errors status", rc == 0 and cd["status"] == "errors"
   and [r["file"] for r in cd["reads"]] == ["CD01", "CD01_b"]
   and "the read is complete" in out and cd["disc"].get("dic_exit_code") == 3, json.dumps(cd) + out)

# 27l. first read broken (.dat without tracks), second failed: not "read once"
rc, out = run("CD rotto", 0, [], [""], ["-C", "1"], tpl=None, dic=["cd_brokendat", "FAIL"])
cd = meta(bag("CD rotto"))["media"][0]
ok("broken read then failed: failed read, box open", rc == 1
   and cd["status"] == "read-failed", json.dumps(cd) + out)

# 27m. resumed with folders in data/ that metadata.json does not mention:
#      only those named like a CD read (CD01, CD01_c) are removed
rc, out = run("CD avanzi", 0, [], ["Q"], ["-C", "1"], tpl=None)
b = bag("CD avanzi")
for sub in ("CD01", "CD01_c", "Extra"):
    os.makedirs(os.path.join(b, "data", sub))
    with open(os.path.join(b, "data", sub, "x.txt"), "w") as f:
        f.write("avanzo\n")
rc, out = run("CD avanzi", 0, [], [""], ["-C", "1"], tpl=None, dic=["cd_good", "cd_good"])
ok("leftovers: half reads removed, unrelated folder left", rc == 0
   and "Removing data/CD01_c" in out and "Leaving data/Extra" in out
   and os.path.isfile(os.path.join(b, "data", "Extra", "x.txt"))
   and not os.path.exists(os.path.join(b, "data", "CD01_c"))
   and not os.path.exists(os.path.join(b, "data", "CD01", "x.txt"))
   and check_bag.check(b, PROFILE) == [], out)

# 28. box opened with the previous version (retroarchiver-floppy-1, DISK{nnn}),
#     resumed with this one: the names stay of the same kind, the schema becomes new
rc, out = run("Vecchio", 3, ["ident_a", "ident_b"], ["", "Q"], tpl="DISK{nnn}")
b = bag("Vecchio")
mp = os.path.join(b, "data", "metadata.json")
with open(mp, encoding="utf-8") as f:
    m = json.load(f)
old_box = dict(m["box"])
for k in ("media", "names"):
    old_box.pop(k)
old_box["media_type"] = "floppy"
m["schema"] = "retroarchiver-floppy-1"
m["box"] = old_box
for d in m["media"]:
    d.pop("type")
    d.pop("number")
with open(mp, "w", encoding="utf-8") as f:
    json.dump(m, f, indent=2)
first = sha(os.path.join(b, "data", "DISK001.dsk"))
rc, out = run("Vecchio", 3, ["good_2", "good_2", "good_3", "good_3"], ["", ""], tpl=None)
m = meta(b)
ok("old box resumed: closed and valid", rc == 0 and check_bag.check(b, PROFILE) == [], out)
ok("old box resumed: the new floppies with the old naming scheme",
   [f for f in data_files(b) if f.endswith(".dsk")] == ["DISK001.dsk", "DISK002.dsk", "DISK003.dsk"]
   and m["box"]["names"] == {"floppy": "DISK{nnn}"}, str(data_files(b)) + json.dumps(m["box"]))
ok("old box resumed: the earlier read intact, new schema",
   sha(os.path.join(b, "data", "DISK001.dsk")) == first and m["schema"] == "retroarchiver-3"
   and [d["type"] for d in m["media"]] == ["floppy"] * 3, json.dumps(m["box"]))

# 28b. a box left open by version 261017: schema retroarchiver-2 with the
#      outcomes in Italian, the .in-corso marker and a messi-da-parte folder.
#      It is open and resumable, keeps its folder, and closing removes .in-corso
rc, out = run("Aperto 261017", 3, ["ident_a", "ident_b", "good_1", "good_1"], ["", "", "Q"])
b = bag("Aperto 261017")
os.rename(os.path.join(b, ".in-progress"), os.path.join(b, ".in-corso"))
os.makedirs(os.path.join(b, "messi-da-parte"))
with open(os.path.join(b, "messi-da-parte", "disco01_b.dsk.danneggiato"), "w") as f:
    f.write("old")
with open(os.path.join(b, "data", "disco02_c.dsk.danneggiato"), "w") as f:
    f.write("left in data by an earlier version")
mp = os.path.join(b, "data", "metadata.json")
with open(mp, encoding="utf-8") as f:
    m = json.load(f)
m["schema"] = "retroarchiver-2"
for d, it in zip(m["media"], ["pulito", "pulito", "da leggere"]):
    d["status"] = it
with open(mp, "w", encoding="utf-8") as f:
    json.dump(m, f, indent=2)
rc, out = run("Aperto 261017", 3, ["good_2", "good_2"], [""], tpl=None)
m = meta(b)
ok("261017 open box: resumed from floppy 3 and closed valid", rc == 0 and check_bag.check(b, PROFILE) == []
   and "Insert floppy 3" in out and "Insert floppy 1" not in out, out)
ok("261017 open box: outcomes in English, new schema, markers gone",
   [d["status"] for d in m["media"]] == ["clean", "clean", "clean"] and m["schema"] == "retroarchiver-3"
   and not os.path.exists(os.path.join(b, ".in-corso")) and not os.path.exists(os.path.join(b, ".in-progress"))
   and info(b).get("Box-Status") == "complete", json.dumps(m["media"]))
ok("261017 open box: its messi-da-parte folder is kept and used",
   os.path.exists(os.path.join(b, "messi-da-parte", "disco01_b.dsk.danneggiato"))
   and os.path.exists(os.path.join(b, "messi-da-parte", "disco02_c.dsk.danneggiato"))
   and not os.path.exists(os.path.join(b, "set-aside")), str(os.listdir(b)))

# 28c. a box closed by 261017 (profile 2, Box-Status in Italian) still checks out
b = bag("Sano")
bi = os.path.join(b, "bag-info.txt")
with open(bi, encoding="utf-8") as f:
    text = f.read()
text = text.replace("urn:retroarchiver:bagit-profile:3", "urn:retroarchiver:bagit-profile:2")
text = text.replace("Box-Status: complete", "Box-Status: completo")
with open(bi, "w", encoding="utf-8", newline="") as f:
    f.write(text)
tag = os.path.join(b, "tagmanifest-sha256.txt")
with open(tag, encoding="utf-8") as f:
    lines = [l if not l.endswith("  bag-info.txt\n") else "%s  bag-info.txt\n" % sha(bi) for l in f]
with open(tag, "w", encoding="utf-8", newline="") as f:
    f.writelines(lines)
ok("261017 closed box: valid with profile 2", check_bag.check(b) == [], str(check_bag.check(b)))

# 9. -k keeps all the reads even of a clean disk
rc, out = run("Tutte", 1, ["ident_a", "ident_b"], [""], ["-k"])
ok("-k: second read kept", "disco01_b.dsk" in data_files(bag("Tutte")), out)

# 29. the same disk inserted twice: floppy 3 gives the same image as floppy 1.
# Its reads are set aside, it stays to be read, and on resume the right one closes the box
rc, out = run("Doppione", 3, ["ident_a", "ident_a", "fat_144", "fat_144", "ident_a", "ident_a"], ["", "", ""])
b = bag("Doppione")
d3 = meta(b)["media"][2]
ok("same disk twice: the log says floppy 3 is identical to floppy 1",
   "floppy 3 is identical to floppy 1" in out and "floppy 2 is identical" not in out, out)
ok("same disk twice: box open, floppy 3 duplicate of floppy 1 with no reads",
   rc == 1 and d3["status"] == "duplicate" and d3.get("duplicate_of") == 1
   and d3["reads"] == [] and d3["master"] is None, json.dumps(d3) + "\n" + out)
dup = os.path.join(b, "set-aside", "disco03.dsk.duplicate")
ok("same disk twice: the reads set aside outside data/, not deleted",
   os.path.exists(dup) and "disco03.dsk" not in data_files(b)
   and sha(dup) == sha(os.path.join(FIX, "ident_a.dsk")), str(data_files(b)))
rc, out = run("Doppione", 3, ["good_3", "good_3"], [""])
d3 = meta(b)["media"][2]
ok("same disk twice: on resume floppy 3 is asked for and read from zero",
   rc == 0 and "Insert floppy 3" in out and d3["status"] == "clean" and "duplicate_of" not in d3
   and len(d3["reads"]) == 1 and sha(os.path.join(b, "data", "disco03.dsk")) == sha(os.path.join(FIX, "good_3.dsk"))
   and check_bag.check(b, PROFILE) == [], json.dumps(d3) + "\n" + out)
ok("same disk twice: the duplicate stays among the files set aside",
   os.path.exists(dup) and "disco03.dsk.duplicate" in out, out)
# with -r the right disk is asked for at once
rc, out = run("Doppione subito", 2, ["ident_a", "ident_a", "ident_a", "ident_a", "good_2", "good_2"],
              ["", "", ""], ["-r"])
ok("same disk twice with -r: floppy 2 asked again at once, the box closes",
   rc == 0 and out.count("Insert floppy 2") == 2
   and [d["status"] for d in meta(bag("Doppione subito"))["media"]] == ["clean", "clean"], out)

# 30. the copy protection scan (DiscImageCreator /sf): the box chooses, and with
# auto the files on the disc decide. RETROARCHIVER_DISC_ROOT is the disc's files
DISC_PLAIN = os.path.join(WORK, "disco-normale")
DISC_SAFEDISC = os.path.join(WORK, "disco-safedisc")
os.makedirs(os.path.join(DISC_PLAIN, "SETUP"), exist_ok=True)
os.makedirs(os.path.join(DISC_SAFEDISC, "GIOCO"), exist_ok=True)
for path in (os.path.join(DISC_PLAIN, "SETUP", "SETUP.EXE"), os.path.join(DISC_SAFEDISC, "GIOCO", "clcd32.dll"),
             os.path.join(DISC_SAFEDISC, "00000001.TMP"), os.path.join(DISC_SAFEDISC, "GIOCO", "GIOCO.EXE")):
    open(path, "wb").close()
DICLOG = os.path.join(WORK, "dic-log.txt")


def dic_log():
    """the fake DIC's reads, one line each ("cd sf offset none"), and a new log"""
    if not os.path.exists(DICLOG):
        return []
    with open(DICLOG) as f:
        lines = f.read().splitlines()
    os.remove(DICLOG)
    return lines


def disc_env(root, **more):
    env = dict(RETROARCHIVER_DISC_ROOT=root, FAKE_DIC_LOG=DICLOG)
    env.update(more)
    return env


dic_log()
rc, out = run("Protezione auto pulito", 0, [], [""], ["-C", "1"], tpl=None, dic=["cd_good", "cd_good"],
              env_extra=disc_env(DISC_PLAIN))
m = meta(bag("Protezione auto pulito"))
cd = m["media"][0]
ok("protection auto, no signature files: no /sf and the log says so",
   rc == 0 and dic_log() == ["cd nosf offset none"] * 2
   and "No copy protection files on the disc" in out, out)
ok("protection auto, no signature files: the box choice and how the CD was read in the metadata",
   m["box"].get("copy_protection_scan") == "auto" and "copy_protection_scan" not in cd
   and cd["disc"].get("copy_protection_scan") == {"choice": "auto", "used": 0, "signature_files": ""}
   and cd["disc"].get("drive_offset") == "unknown", json.dumps(m["box"]) + json.dumps(cd.get("disc")))

rc, out = run("Protezione auto SafeDisc", 0, [], [""], ["-C", "1"], tpl=None, dic=["cd_good", "cd_good"],
              env_extra=disc_env(DISC_SAFEDISC))
cd = meta(bag("Protezione auto SafeDisc"))["media"][0]
ok("protection auto, SafeDisc files: /sf for both reads",
   rc == 0 and dic_log() == ["cd sf offset none"] * 2, out)
ok("protection auto, SafeDisc files: the log names what it found",
   "Found 00000001.TMP, clcd32.dll (SafeDisc): enabling the protection scan" in out
   or "Found clcd32.dll, 00000001.TMP (SafeDisc): enabling the protection scan" in out, out)
scan = cd["disc"].get("copy_protection_scan") or {}
ok("protection auto, SafeDisc files: in the metadata",
   scan.get("choice") == "auto" and scan.get("used") == 1 and "clcd32.dll" in scan.get("signature_files", "")
   and scan["signature_files"].endswith("(SafeDisc)"), json.dumps(cd.get("disc")))

rc, out = run("Protezione attiva", 0, [], [""], ["-C", "1", "--copy-protection=on"], tpl=None,
              dic=["cd_good", "cd_good"], env_extra=disc_env(DISC_PLAIN))
m = meta(bag("Protezione attiva"))
ok("protection on: /sf without signature files, and the choice in the box",
   rc == 0 and dic_log() == ["cd sf offset none"] * 2 and m["box"]["copy_protection_scan"] == "on"
   and m["media"][0]["disc"]["copy_protection_scan"] == {"choice": "on", "used": 1, "signature_files": None}
   and "Copy protection scan on, as chosen" in out, out + json.dumps(m["media"][0].get("disc")))

rc, out = run("Protezione disattiva", 0, [], [""], ["-C", "1", "--copy-protection=off"], tpl=None,
              dic=["cd_good", "cd_good"], env_extra=disc_env(DISC_SAFEDISC))
m = meta(bag("Protezione disattiva"))
ok("protection off: no /sf even with SafeDisc files",
   rc == 0 and dic_log() == ["cd nosf offset none"] * 2 and m["box"]["copy_protection_scan"] == "off"
   and "Found" not in out, out)

rc, out = run("Protezione sbagliata", 0, [], [""], ["-C", "1", "--copy-protection=forse"], tpl=None)
ok("protection: an unknown value gives the usage", rc == 2 and "--copy-protection=auto|on|off" in out, out)

# the safety net: a read without /sf fails, the CD is asked again (with -r) and read with /sf
rc, out = run("Protezione rete", 0, [], ["", ""], ["-C", "1", "-r"], tpl=None,
              dic=["FAIL", "cd_good", "cd_good"], env_extra=disc_env(DISC_PLAIN))
cd = meta(bag("Protezione rete"))["media"][0]
ok("safety net: after a failed read without /sf the next ones use it",
   rc == 0 and dic_log() == ["cd nosf offset none", "cd sf offset none", "cd sf offset none"]
   and "The read of CD 1 without the copy protection scan failed: reading it again looks for protections" in out
   and "Read CD 1 again, looking for copy protections" in out, out)
ok("safety net: the disc keeps the scan on, the read says how it was made",
   cd["status"] == "clean" and cd.get("copy_protection_scan") == "on"
   and cd["disc"]["copy_protection_scan"]["used"] == 1 and cd["disc"]["copy_protection_scan"]["choice"] == "on",
   json.dumps(cd))
# an old firmware (PLEXTOR PX-760A 1.06 on the PC): DIC refuses before reading;
# the log says to update it, and the next read does not turn the scan on
rc, out = run("Firmware vecchio", 0, [], ["", ""], ["-C", "1", "-r"], tpl=None,
              dic=["FIRMWARE", "cd_good", "cd_good"], env_extra=disc_env(DISC_PLAIN))
cd = meta(bag("Firmware vecchio"))["media"][0]
ok("old firmware: the log says to update it, no copy protection scan after it",
   rc == 0 and "update the drive's firmware" in out
   and dic_log() == ["cd nosf offset none"] * 3 and cd["status"] == "clean", out)

# reads with errors: proposed again, Skip keeps what there is
rc, out = run("Protezione errori", 0, [], ["", "S"], ["-C", "1", "-r"], tpl=None,
              dic=["cd_c2", "cd_c2"], env_extra=disc_env(DISC_PLAIN))
cd = meta(bag("Protezione errori"))["media"][0]
ok("safety net: errors without /sf, proposed again; S keeps the two reads",
   rc == 0 and dic_log() == ["cd nosf offset none"] * 2
   and "without the copy protection scan has errors" in out and "S to keep what was read" in out
   and cd["status"] == "errors" and len(cd["reads"]) == 2, out + json.dumps(cd))
rc, out = run("Protezione errori ancora", 0, [], ["", ""], ["-C", "1", "-r"], tpl=None,
              dic=["cd_c2", "cd_c2", "cd_c2"], env_extra=disc_env(DISC_PLAIN))
ok("safety net: the third read with /sf, then no more asking",
   rc == 0 and dic_log() == ["cd nosf offset none"] * 2 + ["cd sf offset none"]
   and out.count("looking for copy protections") == 1, out)
# chosen off: the net does not turn it on
rc, out = run("Protezione rete spenta", 0, [], ["", "S"], ["-C", "1", "-r", "--copy-protection=off"], tpl=None,
              dic=["FAIL", "FAIL"], env_extra=disc_env(DISC_PLAIN))
ok("safety net: not with the scan chosen off",
   dic_log() == ["cd nosf offset none"] and "looks for protections" not in out
   and "Insert CD 1 of 1" in out.split("read-failed")[1], out)

# the drive resets itself with /sf (seen on the PC: UNIT_ATTENTION, timeouts): the read is
# stopped after 3 resets and the CD read again without the scan
t0 = time.time()
rc, out = run("Protezione reset", 0, [], ["", ""], ["-C", "1", "-r", "--copy-protection=on"], tpl=None,
              dic=["RESETS3 cd_good", "cd_good", "cd_good"], env_extra=disc_env(DISC_PLAIN))
b = bag("Protezione reset")
cd = meta(b)["media"][0]
ok("resets with /sf: stopped at once, the CD read again without it",
   rc == 0 and dic_log() == ["cd sf offset none", "cd nosf offset none", "cd nosf offset none"]
   and "The drive was reset 3 times while DiscImageCreator looked for copy protections" in out
   and "still resetting" in out and time.time() - t0 < 9
   and "reading it again without it" in out and "without looking for copy protections" in out, out)
ok("resets with /sf: the disc keeps the scan off, the stopped read set aside",
   cd["status"] == "clean" and cd.get("copy_protection_scan") == "off"
   and cd["disc"]["copy_protection_scan"]["used"] == 0
   and os.path.isdir(os.path.join(b, "set-aside", "CD01.failed")), json.dumps(cd))
# without /sf: resets are no reason to turn it on
rc, out = run("Reset senza protezione", 0, [], ["", "S"], ["-C", "1", "-r"], tpl=None,
              dic=["RESETS2 FAIL"], env_extra=disc_env(DISC_PLAIN))
ok("resets without /sf: no scan proposed, the log says why",
   dic_log() == ["cd nosf offset none"] and "the copy protection scan would not help" in out
   and "looks for protections" not in out and "Insert CD 1 of 1" in out.split("read-failed")[1], out)

# 31. the drive's read offset: DIC asks for it when it does not know the drive
rc, out = run("Offset mancante", 0, [], ["", "S"], ["-C", "1", "-r"], tpl=None, dic=["cd_good"],
              env_extra=disc_env(DISC_PLAIN, FAKE_DIC_ASK_OFFSET="1"))
b = bag("Offset mancante")
ok("offset not set: the read fails and the log says where to set it, with the model",
   dic_log() == ["cd nosf offset failed"]
   and "read offset is not set" in out and "for model ASUS DRW-08D6MT in" in out and "[Optical] DriveOffset" in out
   and "looks for protections" not in out, out)
ok("offset not set: DIC's logs set aside", os.path.isdir(os.path.join(b, "set-aside", "CD01.failed")),
   str(os.listdir(b)))
rc, out = run("Offset dato", 0, [], [""], ["-C", "1", "--drive-offset=+6"], tpl=None, dic=["cd_good", "cd_good"],
              env_extra=disc_env(DISC_PLAIN, FAKE_DIC_ASK_OFFSET="1"))
b = bag("Offset dato")
cd = meta(b)["media"][0]
ok("offset set: DIC reads it from its input", rc == 0 and dic_log() == ["cd nosf offset 6"] * 2, out)
ok("offset set: in the metadata, and the input file is not in the package",
   cd["disc"].get("drive_offset") == "+6"
   and not os.path.exists(os.path.join(b, "data", "CD01", "dic-input.txt")), json.dumps(cd.get("disc")))
rc, out = run("Offset dato senza domanda", 0, [], [""], ["-C", "1", "--drive-offset=-472"], tpl=None,
              dic=["cd_good", "cd_good"], env_extra=disc_env(DISC_PLAIN))
cd = meta(bag("Offset dato senza domanda"))["media"][0]
ok("offset set but not asked: DIC does not read it, the metadata say what was given",
   rc == 0 and dic_log() == ["cd nosf offset none"] * 2 and cd["disc"].get("drive_offset") == "-472", out)
rc, out = run("Offset sbagliato", 0, [], [""], ["-C", "1", "--drive-offset=sei"], tpl=None)
ok("offset: not a number gives the usage", rc == 2 and "--drive-offset=N" in out, out)

# 32. a drive that cannot read the lead-in (ASUS DRW-08D6MT on the PC)
dic_log()
# a data CD on that drive: read again with the data command, verified by EDC/ECC with one read
rc, out = run("CD dati senza lead-in", 0, [], [""], ["-C", "1", "--drive-offset=+6"], tpl=None,
              dic=["LEADIN data_good", "data_good"], env_extra=disc_env(DISC_PLAIN, FAKE_DIC_ASK_OFFSET="1"))
b = bag("CD dati senza lead-in")
cd = meta(b)["media"][0]
ok("no lead-in, data CD: read again with data, from 0 to the lead-out, with the offset",
   rc == 0 and dic_log() == ["cd nosf offset 6", "data 0-300 nosf offset 6"]
   and "reading this data CD again with the data command" in out
   and "Reading the CD with DiscImageCreator data, sectors 0 to 300" in out, out)
ok("no lead-in, data CD: one read, clean, verified by EDC/ECC and said so",
   cd["status"] == "clean" and len(cd["reads"]) == 1 and cd["compare"]["verdict"] == "edc-ecc"
   and check_bag.check(b, PROFILE) == []
   and (cd.get("disc") or {}).get("read_method") == "dic-data"
   and "not redump-grade" in cd["disc"].get("read_note", "")
   and cd["disc"].get("read_errors") == 0 and cd["disc"].get("edc_errors") == 0, json.dumps(cd) + out)
ok("no lead-in, data CD: .cue and .dat written by RetroArchiver, the file system read",
   os.path.isfile(os.path.join(b, "data", "CD01", "CD01.cue"))
   and os.path.isfile(os.path.join(b, "data", "CD01", "CD01.dat"))
   and [t["mode"] for t in cd["disc"]["tracks"]] == ["MODE1/2352"] and cd["disc"]["sectors"] == 300
   and (cd.get("filesystem") or {}).get("volume_label") == "PROVA_CD", json.dumps(cd))
with open(os.path.join(b, "data", "CD01", "dic-console.txt"), encoding="utf-8") as f:
    con = f.read()
ok("no lead-in, data CD: the failed cd attempt stays in the console log, its other logs do not",
   "can't read the lead-in" in con and "RetroArchiver: read method: data 0-300" in con
   and not os.path.exists(os.path.join(b, "set-aside", "CD01.failed")), con)
ok("no lead-in, data CD: the drive is remembered", "Drive ASUS DRW-08D6MT 1.00 cannot read the lead-in: "
   "remembered" in out, out)

# the Sony AD-7200S (offset +48, seen on the PC): it stops at LBA -2 with
# "OUT OF RANGE" and never says "can't read the lead-in"
dic_log()
rc, out = run("CD dati lettore Sony", 0, [], [""], ["-C", "1"], tpl=None,
              dic=["LEADIN2 data_good", "data_good"], env_extra=disc_env(DISC_PLAIN))
cd = meta(bag("CD dati lettore Sony"))["media"][0]
ok("no lead-in at LBA -2, without DIC saying so: read again with data, clean",
   rc == 0 and dic_log() == ["cd nosf offset none", "data 0-300 nosf offset none"]
   and cd["status"] == "clean" and (cd.get("disc") or {}).get("read_method") == "dic-data",
   json.dumps(cd) + out)

# a mixed CD: no data command, the reason is said
rc, out = run("CD misto senza lead-in", 0, [], [""], ["-C", "1"], tpl=None, dic=["LEADIN cd_good"],
              env_extra=disc_env(DISC_PLAIN))
cd = meta(bag("CD misto senza lead-in"))["media"][0]
ok("no lead-in, mixed CD: no data read, a drive that reads the lead-in is needed",
   rc == 1 and cd["status"] == "read-failed" and dic_log() == ["cd nosf offset none"]
   and "this CD has 1 audio track of 2" in out and "Use a drive that can read the lead-in" in out, out)

# EDC/ECC errors: not verified by one read, the second read (data straight away) compares
rc, out = run("CD dati errori EDC", 0, [], [""], ["-C", "1"], tpl=None,
              dic=["LEADIN data_edc", "data_edc", "data_edc"], env_extra=disc_env(DISC_PLAIN))
cd = meta(bag("CD dati errori EDC"))["media"][0]
reads = dic_log()
ok("no lead-in, EDC/ECC errors: a second read with data, no cd before it",
   reads[:3] == ["cd nosf offset none", "data 0-300 nosf offset none", "data 0-300 nosf offset none"]
   and "Read 2" in out and "known not to read the lead-in" in out, out + str(reads))
ok("no lead-in, EDC/ECC errors: errors status, two reads",
   cd["status"] == "errors" and len(cd["reads"]) >= 2 and cd["disc"]["edc_errors"] == 2, json.dumps(cd))

# the drive known from the first CD: the second goes straight to data
model_env = disc_env(DISC_PLAIN, RETROARCHIVER_DRIVE_MODEL="ASUS  DRW-08D6MT 1.00",
                     RETROARCHIVER_TOC=os.path.join(FIX, "dic", "data_good", "DISC_disc.txt"))
rc, out = run("Due CD senza lead-in", 0, [], ["", ""], ["-C", "2"], tpl=None,
              dic=["LEADIN data_good", "data_good", "data_good"], env_extra=model_env)
m = meta(bag("Due CD senza lead-in"))
ok("remembered drive: the second CD skips cd",
   rc == 0 and dic_log() == ["cd nosf offset none"] + ["data 0-300 nosf offset none"] * 2
   and "the drive is known not to read the lead-in" in out
   and [d["status"] for d in m["media"]] == ["clean", "clean"], out)

# 33. two editions of the same title are two boxes, each in TITLE (EDITION)
rc1, out1 = run("Office", 1, ["ident_a", "ident_b"], [""], ["-e", "97"])
rc2, out2 = run("Office", 1, ["ident_a", "ident_b"], [""], ["-e", "2000"])
ok("two editions: two closed boxes, one folder each",
   rc1 == 0 and rc2 == 0 and info(bag("Office (97)")).get("Edition") == "97"
   and info(bag("Office (2000)")).get("Edition") == "2000"
   and check_bag.check(bag("Office (2000)"), PROFILE) == [], out1 + out2)
rc, out = run("Office", 1, ["ident_a", "ident_b"], [""], ["-e", "97"])
ok("two editions: the same edition again is the closed box", rc == 2 and "is already closed" in out, out)
# a box of 261017 is in the folder of its title alone, edition or not
rc, out = run("Antico", 2, ["ident_a", "ident_b"], ["", "Q"], ["-e", "1.0"])
os.rename(bag("Antico (1.0)"), bag("Antico"))
before = open(os.path.join(bag("Antico"), "data", "metadata.json"), "rb").read()
rc, out = run("Antico", 1, ["fat_144", "fat_144"], [""], ["-e", "2.0"])
ok("old folder, other edition: a new box, the old one untouched",
   rc == 0 and os.path.isdir(bag("Antico (2.0)"))
   and open(os.path.join(bag("Antico"), "data", "metadata.json"), "rb").read() == before
   and os.path.exists(os.path.join(bag("Antico"), ".in-progress")), out)
rc, out = run("Antico", 2, ["fat_144", "fat_144"], [""], ["-e", "1.0"])
ok("old folder, same edition: resumed and closed there",
   rc == 0 and "Resuming" in out and not os.path.exists(bag("Antico (1.0)"))
   and check_bag.check(bag("Antico"), PROFILE) == [], out)
# the edition typed in the title gives the same folder: not mixed up
rc, out = run("Word (6)", 2, ["ident_a", "ident_b"], ["", "Q"])
rc, out = run("Word", 2, ["ident_a", "ident_b"], [""], ["-e", "6"])
ok("edition in the title, same folder: clear error",
   rc == 2 and "holds the box" in out and meta(bag("Word (6)"))["box"]["title"] == "Word (6)", out)

print()
print("  %d cases, %s" % (count, "all green." if not fails else "with at least one FAIL."))
sys.exit(1 if fails else 0)
