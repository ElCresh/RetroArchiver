#!/usr/bin/env python3
"""Complete dskrun sessions with the fake SAMdisk: checks the bag,
metadata.json and archivio.csv. Run by make test, from the tests folder."""
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


def run(title, ndisk, script, keys, extra_args=(), arch=ARCH, tpl="disco{nn}", dic=None, dats=NODAT):
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
# a log written by dskrun 1.0: new lines are appended
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
ok("healthy box: clean statuses", [d["status"] for d in m["media"]] == ["pulito", "pulito"])
ok("healthy box: label of disk 1", m["media"][0]["label"] == "Disk 1")
ok("healthy box: empty label of disk 2 taken from the FAT12", m["media"][1]["label"] == "MONKEY3"
   and "Etichetta del floppy 2 presa dal disco: MONKEY3" in out, json.dumps(m["media"][1]["label"]))
ok("healthy box: FAT12 of disk 2",
   (m["media"][1].get("filesystem") or {}).get("volume_label") == "MONKEY3", json.dumps(m["media"][1]))
ok("healthy box: 1.44M format", m["media"][1]["geometry"]["format"] == "1.44M")
ok("healthy box: Box-Status complete", info(b).get("Box-Status") == "completo")
with open(os.path.join(ARCH, "archivio.csv"), encoding="utf-8") as f:
    rows = f.read().splitlines()
ok("log: old header kept", rows[0] == "disco;master;letture;esito;quando")
ok("log: two new rows with the box", len(rows) == 4
   and rows[2].startswith("1;disco01.dsk;1;pulito;") and rows[2].endswith(";Sano")
   and rows[3].startswith("2;disco02.dsk;1;pulito;") and rows[3].endswith(";Sano"),
   "\n".join(rows))

# 2. marginal disk: three reads, all kept
rc, out = run("Marginale", 1, ["weak_1", "weak_2", "weak_3"], [""])
b = bag("Marginale")
m = meta(b)
ok("marginal: closed and valid", rc == 0 and check_bag.check(b, PROFILE) == [], out)
ok("marginal: status", m["media"][0]["status"] == "marginale")
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
ok("failed read: bag open", rc == 1 and os.path.exists(os.path.join(b, ".in-corso")), out)
ok("failed read: status", meta(b)["media"][0]["status"] == "lettura fallita")
ok("failed read: no image", not [f for f in data_files(b) if f.endswith(".dsk")])

# 4. skip and close: incomplete but valid box
rc, out = run("Saltati", 2, [], ["S", "F"])
b = bag("Saltati")
ok("incomplete: closed and valid", rc == 0 and check_bag.check(b, PROFILE) == [], out)
ok("incomplete: Box-Status", info(b).get("Box-Status") == "incompleto")
ok("incomplete: Missing-Media", info(b).get("Missing-Media") == "floppy 1, floppy 2", str(info(b)))
ok("incomplete: Media-Type", info(b).get("Media-Type") == "2 floppy", str(info(b)))

# 5. resume after quitting, with an orphan file left in data/
rc, out = run("Ripreso", 2, ["ident_a", "ident_b"], ["", "Q"])
b = bag("Ripreso")
ok("resume: first part left open", rc == 1, out)
with open(os.path.join(b, "data", "disco02_b.dsk"), "w") as f:
    f.write("resto di un'interruzione")
rc, out = run("Ripreso", 2, ["ident_a", "ident_b"], [""])
ok("resume: closed and valid", rc == 0 and check_bag.check(b, PROFILE) == [], out)
ok("resume: orphan removed with a warning", "Tolgo data/disco02_b.dsk" in out, out)
ok("resume: both disks clean", [d["status"] for d in meta(b)["media"]] == ["pulito", "pulito"])
ok("resume: images intact",
   sha(os.path.join(b, "data", "disco01.dsk")) == sha(os.path.join(FIX, "ident_a.dsk")) and
   sha(os.path.join(b, "data", "disco02.dsk")) == sha(os.path.join(FIX, "ident_a.dsk")))

# 6. title with an accent and characters forbidden in folder names
title = "Citt\u00e0: parte 1/2"
rc, out = run(title, 1, ["ident_a", "ident_b"], [""])
b = bag("Citt\u00e0_ parte 1_2")
ok("title with special characters: folder name cleaned", rc == 0 and os.path.isdir(b), out)
ok("title with special characters: real title in the bag", info(b).get("Title") == title)
ok("accented title: on a pipe the output stays UTF-8", "  Cofanetto: " + title + "\n" in out, out)

# 7. input closed: counts as quitting
rc, out = run("Eof", 1, ["ident_a", "ident_b"], [])
ok("input closed: bag open", rc == 1 and "lasciato aperto" in out, out)

# 8. box already closed
rc, out = run("Sano", 2, [], [""])
ok("box already closed: clear error", rc == 2 and "gia' chiuso" in out, out)

# 10. partial read: the half-written file does not enter the closed bag
rc, out = run("Parziale", 2, ["ident_a", "PARTIAL"], ["", "F"])
b = bag("Parziale")
ok("partial: closed and valid", rc == 0 and check_bag.check(b, PROFILE) == [], out)
ok("partial: no half-written file in data/", "disco01_b.dsk" not in data_files(b), str(data_files(b)))

# 11. naming scheme that gives the same name to different disks
rc, out = run("Collisione", 2, [], [""], ["-p", "{set}"])
ok("collision: clear error, nothing created", rc == 2 and "stesso nome" in out
   and not os.path.exists(bag("Collisione")), out)

# 11b. long title: the cut at 77 bytes makes disk 10 coincide with 1
longt = "A" * 76
rc, out = run(longt, 10, [], [""], ["-p", "{set}{n}"])
ok("long collision: clear error, nothing created", rc == 2 and "stesso nome" in out
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
   and d1["master"] == "disco01.dsk" and d1["status"] == "lettura fallita", json.dumps(d1))
rc, out = run("Morente", 1, [], ["S"])
d1 = meta(b)["media"][0]
ok("dying: skipping it does not delete the read",
   os.path.exists(good) and d1["status"] == "lettura fallita"
   and [r["file"] for r in d1["reads"]] == ["disco01.dsk"], json.dumps(d1) + "\n" + out)
rc, out = run("Morente", 1, ["ident_b"], [""])
d1 = meta(b)["media"][0]
ok("dying: the new read is compared with the old one and closes",
   rc == 0 and check_bag.check(b, PROFILE) == [] and d1["status"] == "pulito"
   and d1["master"] == "disco01.dsk" and sha(good) == first, json.dumps(d1) + "\n" + out)
ok("dying: the extra read removed because the disk is clean",
   [f for f in data_files(b) if f.endswith(".dsk")] == ["disco01.dsk"], str(data_files(b)))

# 12b. repeated attempts: beyond META_MAXREAD (6) the oldest are kept
for k in range(5):
    rc, out = run("Tentativi", 1, ["weak_%d" % (k % 3 + 1), "FAIL"], [""])
b = bag("Tentativi")
rc, out = run("Tentativi", 1, ["weak_3"], [""])
d1 = meta(b)["media"][0]
ok("attempts: at most six reads, with a warning", rc == 0 and "il massimo" in out
   and len(d1["reads"]) == 6 and d1["status"] == "marginale", json.dumps(d1) + "\n" + out)
ok("attempts: all six reads are there",
   [f for f in data_files(b) if f.endswith(".dsk")] == ["disco01.dsk"] +
   ["disco01_%s.dsk" % c for c in "bcdef"] and check_bag.check(b, PROFILE) == [], str(data_files(b)))

# 13. two titles that give the same folder: the second does not touch the first
rc, out = run("Prova: A", 2, ["ident_a", "ident_b"], ["", "Q"])
b = bag("Prova_ A")
before = open(os.path.join(b, "data", "metadata.json"), "rb").read()
rc, out = run("Prova_ A", 2, ["ident_a", "ident_b"], ["", ""])
ok("different title, same folder: clear error", rc == 2 and "contiene il cofanetto" in out, out)
ok("different title, same folder: the first stays open and intact",
   os.path.exists(os.path.join(b, ".in-corso"))
   and open(os.path.join(b, "data", "metadata.json"), "rb").read() == before
   and meta(b)["box"]["title"] == "Prova: A", out)
rc, out = run("Prova: A", 3, ["ident_a", "ident_b"], [""])
ok("different number of disks: the saved one wins, with a warning",
   rc == 0 and "aperto con 2 floppy: continuo con 2 floppy" in out
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
rc, out = run("Impronta", 2, ["ident_a", "ident_b"], [""])
d1 = meta(b)["media"][0]
ok("changed hash: stays open and says to read disk 1 again",
   rc == 1 and "rileggi il floppy 1" in out and os.path.exists(os.path.join(b, ".in-corso")), out)
aside = os.path.join(b, "messi-da-parte", "disco01.dsk.danneggiato")
ok("changed hash: the file set aside outside data/, not deleted",
   os.path.exists(aside) and not os.path.exists(img)
   and not [f for f in data_files(b) if "danneggiato" in f]
   and "come messi-da-parte/disco01.dsk.danneggiato, fuori dal pacchetto dei dati" in out,
   str(data_files(b)) + "\n" + out)
ok("changed hash: disk 1 goes back to unread",
   d1["status"] == "da leggere" and d1["reads"] == [] and d1["master"] is None, json.dumps(d1))
# a .danneggiato left in data/ by the previous version: it is moved too
old_aside = os.path.join(b, "data", "disco02.dsk.danneggiato")
with open(old_aside, "w") as f:
    f.write("vecchio\n")
rc, out = run("Impronta", 2, ["ident_a", "ident_b"], [""])
with open(os.path.join(b, "manifest-sha256.txt"), encoding="utf-8") as f:
    man = f.read()
ok("changed hash: disk 1 read again, the box closes valid",
   rc == 0 and check_bag.check(b, PROFILE) == []
   and [d["status"] for d in meta(b)["media"]] == ["pulito", "pulito"], out)
ok("changed hash: no .danneggiato in the manifest, the files set aside are there",
   "danneggiato" not in man and os.path.exists(aside)
   and os.path.exists(os.path.join(b, "messi-da-parte", "disco02.dsk.danneggiato"))
   and not os.path.exists(old_aside) and "Sposto data/disco02.dsk.danneggiato in messi-da-parte/" in out,
   man + "\n" + out)
ok("changed hash: at the end of the box the list of files set aside with the path",
   "Messi da parte, fuori dal pacchetto dei dati" in out and "2 file" in out
   and os.path.join(b, "messi-da-parte", "disco01.dsk.danneggiato") in out
   and os.path.join(b, "messi-da-parte", "disco02.dsk.danneggiato") in out, out)
ok("box without reads set aside: no list", "Messi da parte" not in run("Senza scarti", 1,
   ["ident_a", "ident_b"], [""])[1])

# 15. SAMdisk missing: it is not a damaged diskette
rc, out = run("Senza SAMdisk", 2, [], ["", ""], ["-x", os.path.join(WORK, "manca", "samdisk")])
b = bag("Senza SAMdisk")
ok("no SAMdisk: open, with the instruction", rc == 1 and "Non trovo" in out
   and "Inserisci il floppy 2" not in out, out)
ok("no SAMdisk: disk 1 stays unread",
   meta(b)["media"][0]["status"] == "da leggere" and meta(b)["media"][0]["reads"] == [],
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
        if "Lettura 1" in line:
            seen.append("lettura")
        if seen and "fake_samdisk: slow" in line:
            p.send_signal(signal.SIGINT)
            break
    out = "".join(lines) + p.stdout.read().decode("utf-8", "replace")
    rc = p.wait(timeout=20)
    left = subprocess.run(["pgrep", "-f", FAKE], stdout=subprocess.PIPE).stdout.decode().split()
    d1 = meta(bag("CtrlC"))["media"][0]
    ok("Ctrl+C: box open, disk interrupted, no disk 2",
       rc == 1 and d1["status"] == "interrotto" and "Inserisci il floppy 2" not in out
       and "rilancia per riprendere dal floppy 1" in out, out)
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
        if "--- floppy 1: Confronto ---" in line:
            time.sleep(0.3)
            p.send_signal(signal.SIGINT)
            break
    out = "".join(lines) + p.stdout.read().decode("utf-8", "replace")
    rc = p.wait(timeout=20)
    m = meta(bag("CtrlC confronto"))
    ok("Ctrl+C in the comparison: disk 1 closes clean, no disk 2",
       rc == 1 and m["media"][0]["status"] == "pulito" and "Inserisci il floppy 2" not in out
       and "Fermato: rilancia per riprendere dal floppy 2" in out and time.time() - t0 >= 2, out)

# 17. closed with F: disks never asked for are reported missing
rc, out = run("Chiuso presto", 3, ["ident_a", "ident_b"], ["", "F"])
m = meta(bag("Chiuso presto"))
ok("F: all disks in metadata.json, the unread ones missing",
   rc == 0 and [(d["n"], d["status"]) for d in m["media"]] ==
   [(1, "pulito"), (2, "mancante"), (3, "mancante")], json.dumps(m["media"]))

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
       rc == 1 and "non riesco a togliere data/avanzo.tmp" in out
       and os.path.exists(os.path.join(b, ".in-corso"))
       and not os.path.exists(os.path.join(b, "manifest-sha256.txt")), out)
    shutil.rmtree(tmparch, ignore_errors=True)

# 19. box folder opened but without metadata.json (first save failed)
os.makedirs(os.path.join(bag("Senza metadati"), "data"))
with open(os.path.join(bag("Senza metadati"), "bagit.txt"), "w") as f:
    f.write("BagIt-Version: 1.0\nTag-File-Character-Encoding: UTF-8\n")
with open(os.path.join(bag("Senza metadati"), ".in-corso"), "w") as f:
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
ok("-a: the source is shown on screen", "Lettura da:" in out, out)

# 23. fragile disk: the survey sees defective sectors, a single careful read
rc, out = run("Fragile", 2, ["BAD fat_144", "ident_a", "ident_b"], ["", ""])
b = bag("Fragile")
m = meta(b)
ok("fragile: box closed and valid", rc == 0 and check_bag.check(b, PROFILE) == [], out)
ok("fragile: the defective disk is read only once",
   m["media"][0]["status"] == "fragile" and len(m["media"][0]["reads"]) == 1, json.dumps(m["media"][0]))
ok("fragile: read with strong retries", "careful read --retries=30 --rescans=5" in out and "disco fragile" in out, out)
ok("fragile: the healthy disk follows the normal round",
   m["media"][1]["status"] == "pulito" and "careful read" not in out.split("floppy 2:")[-1], out)

# 24. interrupted disk resumed and skipped: moves on to the next disk, does not quit
rc, out = run("Salto interrotto", 2, ["ident_a", "ident_b"], ["", "Q"])
b = bag("Salto interrotto")
mp = os.path.join(b, "data", "metadata.json")
with open(mp, encoding="utf-8") as f:
    m = json.load(f)
m["media"][0]["status"] = "interrotto"      # as after a Ctrl+C on the second read
with open(mp, "w", encoding="utf-8") as f:
    json.dump(m, f)
rc, out = run("Salto interrotto", 2, ["ident_a", "ident_b"], ["S", ""])
m = meta(b)
ok("skip an interrupted disk: disk 2 is asked for and read",
   "Inserisci il floppy 2" in out and m["media"][1]["status"] == "pulito"
   and "riprendere dal floppy 1" not in out, out)
ok("skip an interrupted disk: disk 1 keeps reads and status",
   m["media"][0]["status"] == "interrotto" and len(m["media"][0]["reads"]) == 1 and rc == 1,
   json.dumps(m["media"][0]))

# 25. -r: a failed disk is asked for again at once, the second attempt succeeds
rc, out = run("Riprova", 1, ["FAIL", "ident_a", "ident_b"], ["", ""], ["-r"])
b = bag("Riprova")
ok("-r: disk 1 asked for again after the failure", out.count("Inserisci il floppy 1") == 2, out)
ok("-r: second attempt clean, box closed and valid",
   rc == 0 and check_bag.check(b, PROFILE) == [] and meta(b)["media"][0]["status"] == "pulito", out)
ok("-r: in the summary the retried disk counts once",
   "1 puliti, 0 marginali, 0 con errori, 0 saltati o da rifare" in out.split("Cofanetto chiuso.")[-1], out)

# 25b. -r: the good read from before the failure stays and is compared with the new one
rc, out = run("Riprova parziale", 1, ["ident_a", "FAIL", "ident_a"], ["", ""], ["-r"])
b = bag("Riprova parziale")
d1 = meta(b)["media"][0]
ok("-r: the first read stays and is compared with the new attempt",
   rc == 0 and check_bag.check(b, PROFILE) == [] and d1["status"] == "pulito"
   and "ha gia' 1 letture" in out and d1["master"] == "disco01.dsk"
   and sha(os.path.join(b, "data", "disco01.dsk")) == sha(os.path.join(FIX, "ident_a.dsk")),
   json.dumps(d1) + "\n" + out)

# 26. -r and Salta after a failure: moves on to the next disk
rc, out = run("Riprova saltata", 2, ["FAIL", "ident_a", "ident_b"], ["", "S", ""], ["-r"])
m = meta(bag("Riprova saltata"))
ok("-r and Skip: disk 2 is read, disk 1 stays missing",
   rc == 1 and out.count("Inserisci il floppy 1") == 2 and "Inserisci il floppy 2" in out
   and [d["status"] for d in m["media"]] == ["mancante", "pulito"], json.dumps(m["media"]) + "\n" + out)

# 27. mixed box: three floppies with the fake SAMdisk, then Q on the CD's turn
rc, out = run("Windows NT Workstation 4.0", 3, ["ident_a", "ident_b"] * 3,
              ["Setup Disk 1", "Setup Disk 2", "Setup Disk 3", "Q"],
              ["-C", "1", "-P", "Windows NT"], tpl=None)
b = bag("Windows NT Workstation 4.0")
m = meta(b)
ok("mixed: after the floppies the CD is asked for", rc == 1 and "Inserisci il CD 1 di 1" in out
   and os.path.exists(os.path.join(b, ".in-corso")), out)
ok("mixed: retroarchiver-2 schema with the composition",
   m["schema"] == "retroarchiver-2" and m["box"]["media_expected"] == 4
   and m["box"]["media"] == [{"type": "floppy", "count": 3}, {"type": "cd", "count": 1}]
   and m["box"]["names"] == {"floppy": "FLOPPY{nn}", "cd": "CD{nn}"}, json.dumps(m["box"]))
ok("mixed: type and number of each medium",
   [(d["n"], d["type"], d["number"], d["status"]) for d in m["media"]] ==
   [(1, "floppy", 1, "pulito"), (2, "floppy", 2, "pulito"), (3, "floppy", 3, "pulito"),
    (4, "cd", 1, "da leggere")], json.dumps(m["media"]))
ok("mixed: fixed names FLOPPY01..03, the CD not yet",
   [f for f in data_files(b) if f.endswith(".dsk")] == ["FLOPPY01.dsk", "FLOPPY02.dsk", "FLOPPY03.dsk"]
   and not os.path.exists(os.path.join(b, "data", "CD01")), str(data_files(b)))
# resumed: only the CD is asked for, and it is read twice (no redump DAT)
rc, out = run("Windows NT Workstation 4.0", 3, [], ["NTWKS40_IT"], ["-C", "1"], tpl=None,
              dic=["cd_good", "cd_good"])
m = meta(b)
cd = m["media"][3]
ok("mixed resumed: closed complete and valid", rc == 0 and check_bag.check(b, PROFILE) == []
   and "Inserisci il floppy" not in out and info(b).get("Box-Status") == "completo", out)
ok("mixed: the CD phases", all(x in out for x in ("--- CD 1: TOC ---", "--- CD 1: Lettura 1 ---",
   "--- CD 1: Controllo C2 ---", "--- CD 1: Redump ---", "--- CD 1: Lettura 2 ---",
   "--- CD 1: Confronto ---")), out)
ok("mixed: CD clean, one read kept", cd["status"] == "pulito" and cd["label"] == "NTWKS40_IT"
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
with open(os.path.join(ARCH, "archivio.csv"), encoding="utf-8") as f:
    rows = f.read().splitlines()
ok("mixed: CD row in archivio.csv",
   any(r.startswith("4;CD01;1;pulito;") and r.endswith(";Windows NT Workstation 4.0") for r in rows), "\n".join(rows))

# 27b. a CD with C2 errors (and SafeDisc) that redump does not know: two equal reads, with errors
rc, out = run("CD rovinato", 0, [], [""], ["-C", "1"], tpl=None, dic=["cd_c2", "cd_c2"])
b = bag("CD rovinato")
m = meta(b)
cd = m["media"][0]
ok("CD with errors: closed and valid", rc == 0 and check_bag.check(b, PROFILE) == [], out)
ok("CD with errors: errors status, both reads kept", cd["status"] == "errori"
   and [r["file"] for r in cd["reads"]] == ["CD01", "CD01_b"]
   and os.path.isdir(os.path.join(b, "data", "CD01_b")), json.dumps(cd))
ok("CD with errors: C2 and protection in the technical data", (cd.get("disc") or {}).get("c2_errors") == 3
   and cd["disc"].get("protection") == "SafeDisc", json.dumps(cd.get("disc")))
with open(os.path.join(b, "data", "CD01.diff.txt"), encoding="utf-8") as f:
    rep_txt = f.read()
ok("CD with errors: the comparison says what to do", "errori non risolti" in rep_txt
   and "Prova un altro lettore" in rep_txt, rep_txt)

# 27c. the same CD present in the redump DAT: one read is enough
rc, out = run("CD redump", 0, [], [""], ["-C", "1"], tpl=None, dic=["cd_c2"], dats=REDUMP)
b = bag("CD redump")
m = meta(b)
cd = m["media"][0]
ok("CD in redump: closed with one read", rc == 0 and check_bag.check(b, PROFILE) == []
   and cd["status"] == "verificato redump" and len(cd["reads"]) == 1
   and "Lettura 2" not in out and "DAT di redump: " in out, out)
ok("CD in redump: the game in the metadata", cd["compare"]["verdict"] == "redump"
   and ((cd.get("disc") or {}).get("redump") or {}).get("game") == "Gioco di Prova (Italy)"
   and cd["disc"]["redump"]["dat_date"] == "2026-09-01 10-00-00", json.dumps(cd))

# 27d. reads that do not match: three reads, two equal, unstable
rc, out = run("CD instabile", 0, [], [""], ["-C", "1"], tpl=None, dic=["cd_good", "cd_noise", "cd_noise"])
b = bag("CD instabile")
cd = meta(b)["media"][0]
ok("unstable CD: three reads kept", rc == 0 and cd["status"] == "instabile"
   and [r["file"] for r in cd["reads"]] == ["CD01", "CD01_b", "CD01_c"]
   and cd["compare"]["agree"] == 2 and check_bag.check(b, PROFILE) == [], json.dumps(cd) + out)
ok("unstable CD: after the second read another one is made", "Le letture non coincidono" in out, out)

# 27e. the second read fails: the CD stays read once
rc, out = run("CD una volta", 0, [], [""], ["-C", "1"], tpl=None, dic=["cd_good", "FAIL"])
cd = meta(bag("CD una volta"))["media"][0]
ok("CD read once: status and one read", rc == 0 and cd["status"] == "letto una volta"
   and [r["file"] for r in cd["reads"]] == ["CD01"]
   and not os.path.exists(os.path.join(bag("CD una volta"), "data", "CD01_b")), json.dumps(cd) + out)

# 27f. a DVD in the drive instead of the CD: read failed, the box stays open
rc, out = run("CD sbagliato", 0, [], [""], ["-C", "1"], tpl=None, dic=["WRONG"])
cd = meta(bag("CD sbagliato"))["media"][0]
ok("wrong disc: failed read with the explanation", rc == 1 and cd["status"] == "lettura fallita"
   and "Nel lettore non c'e' un CD" in out
   and not os.path.exists(os.path.join(bag("CD sbagliato"), "data", "CD01")), json.dumps(cd) + out)

# 27g. DVD in redump: the .iso in its folder
rc, out = run("Solo DVD", 0, [], [""], ["-D", "1"], tpl=None, dic=["dvd_good"], dats=REDUMP)
b = bag("Solo DVD")
dvd = meta(b)["media"][0]
ok("DVD: redump verified and valid", rc == 0 and dvd["status"] == "verificato redump"
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
   and "non ha Joliet" in dvd.get("filesystem_note", ""), json.dumps(dvd))

# 27h. DiscImageCreator missing: the box stays open and says so
rc, out = run("Senza DIC", 0, [], [""], ["-C", "1", "-X", os.path.join(WORK, "dic-che-non-esiste")], tpl=None)
cd = meta(bag("Senza DIC"))["media"][0]
ok("no DIC: open, CD unread", rc == 1 and cd["status"] == "da leggere"
   and "Non trovo" in out and "dopo aver sistemato DiscImageCreator" in out, json.dumps(cd) + out)

# 27i. -f and -C without floppies: only a CD, closed
rc, out = run("Solo CD", 0, [], [""], ["-C", "1"], tpl=None)
m = meta(bag("Solo CD"))
ok("CD only: closed, CD clean",
   rc == 0 and [(d["type"], d["status"]) for d in m["media"]] == [("cd", "pulito")], out)
rc, out = run("Composizione vuota", 0, [], [""], tpl=None)
ok("empty composition: usage instructions", rc == 2 and "-C N" in out, out)

# 27j. CD in redump read badly the first time: the second read is the
#      redump one, becomes the master and the first is removed
rc, out = run("CD redump alla seconda", 0, [], [""], ["-C", "1"], tpl=None, dic=["cd_noise", "cd_good"],
              dats=REDUMP)
b = bag("CD redump alla seconda")
cd = meta(b)["media"][0]
ok("redump on the second read: verified, the second read is the master", rc == 0 and cd["status"] == "verificato redump"
   and cd["master"] == "CD01_b" and [r["file"] for r in cd["reads"]] == ["CD01_b"]
   and not os.path.exists(os.path.join(b, "data", "CD01")) and check_bag.check(b, PROFILE) == [], json.dumps(cd) + out)
ok("redump on the second read: the redump data are the master's",
   ((cd.get("disc") or {}).get("redump") or {}).get("game") == "Gioco di Prova (Italy)", json.dumps(cd))

# 27k. DIC exits with an error but the read is complete: it is kept and counts as an error
rc, out = run("CD uscita 3", 0, [], [""], ["-C", "1"], tpl=None, dic=["EXIT3 cd_good", "cd_good"])
cd = meta(bag("CD uscita 3"))["media"][0]
ok("DIC exits with an error: read kept, errors status", rc == 0 and cd["status"] == "errori"
   and [r["file"] for r in cd["reads"]] == ["CD01", "CD01_b"]
   and "la lettura e' completa" in out and cd["disc"].get("dic_exit_code") == 3, json.dumps(cd) + out)

# 27l. first read broken (.dat without tracks), second failed: not "read once"
rc, out = run("CD rotto", 0, [], [""], ["-C", "1"], tpl=None, dic=["cd_brokendat", "FAIL"])
cd = meta(bag("CD rotto"))["media"][0]
ok("broken read then failed: failed read, box open", rc == 1
   and cd["status"] == "lettura fallita", json.dumps(cd) + out)

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
   and "Tolgo data/CD01_c" in out and "Lascio data/Extra" in out
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
rc, out = run("Vecchio", 3, ["ident_a", "ident_b"] * 2, ["", ""], tpl=None)
m = meta(b)
ok("old box resumed: closed and valid", rc == 0 and check_bag.check(b, PROFILE) == [], out)
ok("old box resumed: the new floppies with the old naming scheme",
   [f for f in data_files(b) if f.endswith(".dsk")] == ["DISK001.dsk", "DISK002.dsk", "DISK003.dsk"]
   and m["box"]["names"] == {"floppy": "DISK{nnn}"}, str(data_files(b)) + json.dumps(m["box"]))
ok("old box resumed: the earlier read intact, new schema",
   sha(os.path.join(b, "data", "DISK001.dsk")) == first and m["schema"] == "retroarchiver-2"
   and [d["type"] for d in m["media"]] == ["floppy"] * 3, json.dumps(m["box"]))

# 9. -k keeps all the reads even of a clean disk
rc, out = run("Tutte", 1, ["ident_a", "ident_b"], [""], ["-k"])
ok("-k: second read kept", "disco01_b.dsk" in data_files(bag("Tutte")), out)

print()
print("  %d cases, %s" % (count, "all green." if not fails else "with at least one FAIL."))
sys.exit(1 if fails else 0)
