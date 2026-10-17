#!/usr/bin/env python3
"""Fixtures for CDs and DVDs: an ISO 9660 image built here, and the
folders that the fake DiscImageCreator (fake_dic.c) copies as if it had
read a disc. The files have the names and structure of the ones DIC
20260101 writes (package README.md, "Created files information"), with
the DISC prefix that the fake replaces with the name of the read.

    fixtures/iso/prova.iso      ISO 9660: PROVA_CD, 3 files and 2 folders
    fixtures/iso/joliet.iso     the same, with Joliet too: long accented names
    fixtures/dic/dvd_joliet/    DVD-ROM with joliet.iso
    fixtures/dic/cd_good/       mixed mode CD: data track + audio track
    fixtures/dic/cd_noise/      the same CD with a different audio track
    fixtures/dic/cd_c2/         like cd_good, but with C2 errors and SafeDisc
    fixtures/dic/cd_c2fixed/    like cd_good, with C2 errors fixed on rereads
    fixtures/dic/cd_noc2/       like cd_good, with a drive that does not report C2
    fixtures/dic/cd_brokendat/  .dat and .cue exist, but the .dat lists no tracks
    fixtures/dic/dvd_good/      DVD-ROM: the .iso and the logs
    fixtures/redump/            a test redump DAT with cd_good and
                                dvd_good, and DIC's default.dat

Run by make test, from the tests folder."""
import hashlib
import os
import random
import shutil
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
FIX = os.path.join(HERE, "fixtures")
SECTORS = 300           # ISO sectors
AUDIO = 120             # audio track sectors


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def dirrec(extent, size, isdir, name, date=(96, 5, 1, 10, 20, 0)):
    n = len(name)
    rec = bytes([0, 0]) + both32(extent) + both32(size) + bytes(date) + bytes([0])
    rec += bytes([2 if isdir else 0, 0, 0]) + both16(1) + bytes([n]) + name
    if n % 2 == 0:
        rec += b"\x00"
    return bytes([len(rec)]) + rec[1:]


def padded(s, n):
    return s.encode("ascii").ljust(n, b" ")


def ucs2(s, n=None):
    """Joliet: UCS-2 big endian (UTF-16 for characters outside the basic
    plane); with n, pads with UCS-2 spaces up to n bytes"""
    b = s.encode("utf-16-be")
    if n is not None:
        b = (b + b"\x00 " * n)[:n]
    return b


# the long names of the Joliet disc, in place of the 8.3 ones of ISO 9660
JOLIET_NAMES = {"GIOCO": "Gioco completo", "DATI": "Dati del mondo",
                "LEGGIMI.TXT": "Leggimi prima di installare.txt", "INSTALL.BAT": "Installa il gioco.bat",
                "MONDO.DAT": "Città del mondo – versione italiana.dat"}
JOLIET_LABEL = "Prova città"


def make_iso(joliet=False):
    """root: LEGGIMI.TXT, INSTALL.BAT, GIOCO/ -> DATI/ -> MONDO.DAT; with
    joliet also the Joliet supplementary descriptor, with the long names
    from JOLIET_NAMES and the JOLIET_LABEL label, pointing to the same
    files"""
    files = {
        "LEGGIMI.TXT": b"Installa il gioco con INSTALL.BAT\r\n",
        "INSTALL.BAT": b"@echo off\r\necho Installo il gioco...\r\n",
        "MONDO.DAT": bytes(random.Random(7).getrandbits(8) for _ in range(5000)),
    }
    lba = {"root": 20, "GIOCO": 21, "DATI": 22, "LEGGIMI.TXT": 23, "INSTALL.BAT": 24, "MONDO.DAT": 25,
           "jroot": 30, "jGIOCO": 31, "jDATI": 32}      # MONDO.DAT takes 25-27
    # without Joliet the path tables are in sectors 18-19, as usual; with
    # Joliet the descriptor terminator is there
    pt = (40, 41, 42, 43) if joliet else (18, 19, None, None)
    img = bytearray(SECTORS * 2048)

    def dir_sector(me, parent, entries, j=""):
        out = dirrec(lba[j + me], 2048, True, b"\x00") + dirrec(lba[j + parent], 2048, True, b"\x01")
        for name, isdir, size in entries:
            if j:
                ident = ucs2(JOLIET_NAMES[name]) if isdir else ucs2(JOLIET_NAMES[name] + ";1")
            else:
                ident = name.encode() if isdir else (name + ";1").encode()
            out += dirrec(lba[(j if isdir else "") + name], 2048 if isdir else size, isdir, ident)
        return out

    def tree(j=""):
        root = dir_sector("root", "root", [("INSTALL.BAT", False, len(files["INSTALL.BAT"])),
                                           ("GIOCO", True, 0),
                                           ("LEGGIMI.TXT", False, len(files["LEGGIMI.TXT"]))], j)
        gioco = dir_sector("GIOCO", "root", [("DATI", True, 0)], j)
        dati = dir_sector("DATI", "GIOCO", [("MONDO.DAT", False, len(files["MONDO.DAT"]))], j)
        for name, data in (("root", root), ("GIOCO", gioco), ("DATI", dati)):
            img[lba[j + name] * 2048:lba[j + name] * 2048 + len(data)] = data

    tree()
    if joliet:
        tree("j")
    for name, data in files.items():
        img[lba[name] * 2048:lba[name] * 2048 + len(data)] = data

    def ptable(big, j=""):
        out = b""
        for key, parent in (("root", 1), ("GIOCO", 1), ("DATI", 2)):
            name = b"\x00" if key == "root" else (ucs2(JOLIET_NAMES[key]) if j else key.encode())
            pk = struct.pack(">I" if big else "<I", lba[j + key]) + struct.pack(">H" if big else "<H", parent)
            e = bytes([len(name), 0]) + pk + name
            out += e + (b"\x00" if len(name) % 2 else b"")
        return out

    def descriptor(kind, j=""):
        lpt, mpt = ptable(False, j), ptable(True, j)
        at = pt[2:] if j else pt[:2]
        img[at[0] * 2048:at[0] * 2048 + len(lpt)] = lpt
        img[at[1] * 2048:at[1] * 2048 + len(mpt)] = mpt
        text = (lambda s, n: ucs2(s, n)) if j else padded
        vd = bytearray(2048)
        vd[0] = kind
        vd[1:6] = b"CD001"
        vd[6] = 1
        vd[8:40] = text("RETROARCHIVER", 32)
        vd[40:72] = text(JOLIET_LABEL if j else "PROVA_CD", 32)
        vd[80:88] = both32(SECTORS)
        if j:
            vd[88:91] = b"%/E"               # UCS-2 level 3
        vd[120:124] = both16(1)
        vd[124:128] = both16(1)
        vd[128:132] = both16(2048)
        vd[132:140] = both32(len(lpt))
        vd[140:144] = struct.pack("<I", at[0])
        vd[148:152] = struct.pack(">I", at[1])
        vd[156:190] = dirrec(lba[j + "root"], 2048, True, b"\x00")
        vd[190:318] = text("", 128)
        vd[318:446] = text("EDITORE DI PROVA", 128)
        vd[446:574] = text("", 128)
        vd[574:702] = text("RETROARCHIVER TEST", 128)
        vd[702:813] = text("", 111)
        vd[813:830] = b"1996050110200000\x00"
        vd[830:847] = b"1996050110200000\x00"
        vd[847:864] = b"0000000000000000\x00"
        vd[864:881] = b"0000000000000000\x00"
        vd[881] = 1
        return vd

    img[16 * 2048:17 * 2048] = descriptor(1)
    if joliet:
        img[17 * 2048:18 * 2048] = descriptor(2, "j")
    term = bytearray(2048)
    term[0] = 255
    term[1:6] = b"CD001"
    term[6] = 1
    t = 18 if joliet else 17
    img[t * 2048:(t + 1) * 2048] = term
    return bytes(img)


def bcd(v):
    return ((v // 10) << 4) | (v % 10)


def mode1(iso):
    """raw 2352-byte sectors: sync, MSF header, data, EDC/ECC (zero here)"""
    out = bytearray()
    for lba in range(len(iso) // 2048):
        a = lba + 150
        hdr = bytes([bcd(a // 4500), bcd(a // 75 % 60), bcd(a % 75), 1])
        out += b"\x00" + b"\xff" * 10 + b"\x00" + hdr + iso[lba * 2048:(lba + 1) * 2048] + b"\x00" * 288
    return bytes(out)


def audio(seed):
    r = random.Random(seed)
    return bytes(r.getrandbits(8) for _ in range(AUDIO * 2352))


def hashes(data):
    return {
        "size": len(data),
        "crc": "%08x" % (zlib.crc32(data) & 0xffffffff),
        "md5": hashlib.md5(data).hexdigest(),
        "sha1": hashlib.sha1(data).hexdigest(),
    }


def rom(name, data):
    h = hashes(data)
    return '\t\t<rom name="%s" size="%d" crc="%s" md5="%s" sha1="%s" />\n' % (
        name.replace("&", "&amp;"), h["size"], h["crc"], h["md5"], h["sha1"])


def datafile(header, games):
    out = '<?xml version="1.0" encoding="UTF-8"?>\n'
    out += ('<!DOCTYPE datafile PUBLIC "-//Logiqx//DTD ROM Management Datafile//EN" '
            '"http://www.logiqx.com/Dats/datafile.dtd">\n<datafile>\n\t<header>\n')
    for k, v in header:
        out += "\t\t<%s>%s</%s>\n" % (k, v, k)
    out += "\t</header>\n"
    for name, roms in games:
        out += '\t<game name="%s">\n\t\t<category>Games</category>\n\t\t<description>%s</description>\n' % (name, name)
        out += "".join(roms)
        out += "\t</game>\n"
    return out + "</datafile>\n"


DIC_HEADER = [("name", "-insert name-"), ("description", "-insert description-"),
              ("version", "-insert version-"), ("date", "-insert date-"),
              ("author", "-insert author-"), ("homepage", "-insert homepage-"), ("url", "-insert url-")]

DRIVE = """========== InquiryData ==========
\t          DeviceType: ReadOnlyDirectAccessDevice (CD/DVD etc)
\t      RemovableMedia: Yes
\t            VendorId: PLEXTOR
\t           ProductId: DVDR   PX-716A
\tProductRevisionLevel: 1.11
\t      VendorSpecific: 07/25/07 15:00
"""


def write(d, name, data):
    mode = "wb" if isinstance(data, bytes) else "w"
    with open(os.path.join(d, name), mode, **({} if mode == "wb" else {"newline": ""})) as f:
        f.write(data)


def cd_variant(name, iso_raw, aud, c2=0, protection="", c2_mode="", no_tracks=False):
    """c2_mode: "fixed" (C2 errors fixed on rereads), "nodrive" (the
    drive does not report C2: DIC drops /c2); no_tracks: the .dat does not
    list the tracks (a broken read that still has .dat and .cue)"""
    d = os.path.join(FIX, "dic", name)
    os.makedirs(d)
    t1, t2 = "DISC (Track 1).bin", "DISC (Track 2).bin"
    total = len(iso_raw) // 2352 + len(aud) // 2352
    write(d, t1, iso_raw)
    write(d, t2, aud)
    write(d, "DISC (Lead-out)(Track AA).bin", b"\x00" * 2352 * 2)
    write(d, "DISC.img", iso_raw + aud)
    write(d, "DISC.sub", b"\x00" * 96 * total)
    write(d, "DISC.c2", b"\x00" * 294 * total)
    write(d, "DISC.toc", bytes([0, 26, 1, 2]) + b"\x00" * 22)
    write(d, "DISC.cue", 'FILE "%s" BINARY\r\n  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n'
                         'FILE "%s" BINARY\r\n  TRACK 02 AUDIO\r\n    INDEX 00 00:00:00\r\n'
                         '    INDEX 01 00:02:00\r\n' % (t1, t2))
    write(d, "DISC.ccd", "[CloneCD]\r\nVersion=3\r\n[Disc]\r\nTocEntries=5\r\nSessions=1\r\n"
                         "DataTracksScrambled=0\r\nCDTextLength=0\r\n")
    roms = [] if no_tracks else [rom(t1, iso_raw), rom(t2, aud)]
    write(d, "DISC.dat", datafile(DIC_HEADER, [("DISC", roms + [rom("DISC (Lead-out)(Track AA).bin",
                                                                     b"\x00" * 4704)])]))
    write(d, "DISC_suppl.dat", datafile(DIC_HEADER, [("DISC", [rom("DISC.img", iso_raw + aud)])]))
    write(d, "DISC_20260101T233129.txt", "cd d DISC.bin 16 /c2 20 /sf /q \r\n")
    write(d, "DISC_drive.txt", DRIVE)
    disc = ("========== TOC ==========\r\n"
            "\t     Data Track  1, LBA        0 -      %d, Length      %d\r\n"
            "\t    Audio Track  2, LBA      %d -      %d, Length      %d\r\n"
            "\t                                        Total      %d\r\n"
            % (SECTORS - 1, SECTORS, SECTORS, total - 1, AUDIO, total))
    if protection:
        disc += "Detected protection [%s]\r\n" % protection
    write(d, "DISC_disc.txt", disc)
    write(d, "DISC_mainError.txt", "")
    write(d, "DISC_mainInfo.txt", "========== LBA[000016, 0x00010]: Main Channel ==========\r\n")
    for log in ("_subError", "_subInfo", "_subReadable", "_volDesc"):
        write(d, "DISC%s.txt" % log, "")
    write(d, "DISC_c2Error.txt", "" if not c2 else
          "".join("LBA[%06d, %#07x] Reread NG\r\n" % (100 + i, 100 + i) for i in range(c2)))
    write(d, "DISC.img_EdcEcc.txt",
          "[NO ERROR] User data vs. ecc/edc match all\r\nTotal errors: 0\r\nTotal warnings: 0\r\n" if not c2 else
          "[ERROR] Number of sector(s) where user data doesn't match the expected ECC/EDC: %d\r\n"
          "Total errors: %d\r\nTotal warnings: 0\r\n" % (c2, c2))
    # what DIC prints on screen, which the fake repeats
    con = ["AppVersion", "\t32 bit, AnsiBuild, 20260101T233129", "StartTime: 2026-10-16T21:24:53+0200",
           "Read TOC"]
    con += ["Creating .scm (LBA) %6d/%6d" % (k, total) for k in range(0, total + 1, 60)]
    if c2_mode == "nodrive":
        con.insert(2, "[WARNING] This drive doesn't support reporting C2 error. Disabled /c2")
    elif c2_mode == "fixed":
        con += ["In the process of correction (c2)", "Remains 2 c2 error. Rereading times:    1/  20",
                "Correction complete (c2)"]
    elif c2:
        con += ["In the process of correction (c2)", "Remains %d c2 error. Rereading times:   20/  20" % c2,
                "Done. See _c2Error.txt"]
    else:
        con += ["No C2 errors"]
    con += ["Exec \"EccEdc.exe\" check \"DISC.img\"", "Checking sectors: %6d/%6d" % (total, total)]
    if protection:
        con += ["Detected protection [%s]" % protection]
    con += ["EndTime: 2026-10-16T21:31:02+0200"]
    write(d, "console.txt", "\n".join(con) + "\n")


def dvd_variant(name, iso):
    d = os.path.join(FIX, "dic", name)
    os.makedirs(d)
    n = len(iso) // 2048
    write(d, "DISC.iso", iso)
    write(d, "DISC.dat", datafile(DIC_HEADER, [("DISC", [rom("DISC.iso", iso)])]))
    write(d, "DISC_20260101T233129.txt", "dvd d DISC.iso 8 /q \r\n")
    write(d, "DISC_drive.txt", DRIVE)
    write(d, "DISC_disc.txt", "========== TotalLength ==========\r\n\t%d (%#x) sectors\r\n" % (n, n))
    write(d, "DISC_DMI.bin", b"\x00" * 2048)
    write(d, "DISC_PFI.bin", b"\x01\x02" + b"\x00" * 2046)
    for log in ("_mainError", "_mainInfo", "_volDesc"):
        write(d, "DISC%s.txt" % log, "")
    con = ["AppVersion", "\t32 bit, AnsiBuild, 20260101T233129", "StartTime: 2026-10-16T22:00:00+0200"]
    con += ["Creating iso(LBA) %8d/%8d" % (k, n) for k in range(0, n + 1, 100)]
    con += ["EndTime: 2026-10-16T22:05:00+0200"]
    write(d, "console.txt", "\n".join(con) + "\n")


def main():
    for sub in ("iso", "dic", "redump"):
        shutil.rmtree(os.path.join(FIX, sub), ignore_errors=True)
        os.makedirs(os.path.join(FIX, sub))
    iso = make_iso()
    write(os.path.join(FIX, "iso"), "prova.iso", iso)
    jiso = make_iso(joliet=True)
    write(os.path.join(FIX, "iso"), "joliet.iso", jiso)
    raw = mode1(iso)
    good, other = audio(1), audio(2)
    cd_variant("cd_good", raw, good)
    cd_variant("cd_noise", raw, other)
    cd_variant("cd_c2", raw, good, c2=3, protection="SafeDisc")
    cd_variant("cd_c2fixed", raw, good, c2_mode="fixed")
    cd_variant("cd_noc2", raw, good, c2_mode="nodrive")
    cd_variant("cd_brokendat", raw, good, no_tracks=True)
    dvd_variant("dvd_good", iso)
    dvd_variant("dvd_joliet", jiso)

    # a redump DAT: the same format, with a real name in the header
    game = "Gioco di Prova (Italy)"
    red = datafile([("name", "IBM - PC compatible"), ("description", "IBM - PC compatible - Discs (3) (2026-09-01 10-00-00)"),
                    ("version", "2026-09-01 10-00-00"), ("date", "2026-09-01 10-00-00"),
                    ("author", "redump.org"), ("homepage", "redump.org"), ("url", "http://redump.org/")],
                   [("Un Altro Gioco (USA)", [rom("Un Altro Gioco (USA).bin", b"altro")]),
                    (game, [rom(game + ".cue", b"cue di redump"),
                            rom(game + " (Track 1).bin", raw), rom(game + " (Track 2).bin", good)]),
                    ("Gioco & DVD di Prova (Italy)", [rom("Gioco & DVD di Prova (Italy).iso", iso)])])
    write(os.path.join(FIX, "redump"), "IBM - PC compatible - Datfile (3) (2026-09-01 10-00-00).dat", red)
    with open(os.path.join(FIX, "redump", "default.dat"), "w") as f:
        f.write(datafile(DIC_HEADER, [("", [])]))
    print("CD and DVD fixtures written to", FIX)


if __name__ == "__main__":
    main()
