#!/usr/bin/env python3
"""Generate synthetic EDSK images to exercise dskdiff.

Each scenario reproduces a real situation met while archiving floppies:
identical reads, noise in the cylinders past the format, stable damage
in the data area, tracks that grow because SAMdisk saved multiple
copies of an unstable sector.

The images end up in tests/fixtures/ and are not versioned: they are
regenerated with  make test  or  python3 gen_edsk.py
"""
import os
import random
import struct

SIG = b"EXTENDED CPC DSK File\r\nDisk-Info\r\n"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")


def tib(cyl, head, nsec=18, size=2, bad=False, copies=None):
    """Track Information Block: 256 bytes in front of each track's data.
    bad     all sectors with a CRC error in the data (ST1/ST2 = 0x20)
    copies  {sector: n} sectors saved as n copies one after the other:
            the length at +6 is n times the nominal one and the sector
            has the CRC error, as SAMdisk writes them"""
    copies = copies or {}
    t = bytearray(256)
    t[0:12] = b"Track-Info\r\n"
    t[0x10] = cyl
    t[0x11] = head
    t[0x12] = 1             # data rate: SAMdisk writes 1 (250K, DD)
    t[0x14] = size
    t[0x15] = nsec
    t[0x16] = 0x4E          # gap3
    t[0x17] = 0xE5          # filler byte
    for i in range(nsec):
        n = copies.get(i + 1, 1)
        st = 0x20 if bad or n > 1 else 0
        o = 0x18 + i * 8
        t[o:o + 6] = bytes([cyl, head, i + 1, size, st, st])
        struct.pack_into("<H", t, o + 6, (128 << size) * n)
    return bytes(t)


def noise_track(cyl, head, nsec, rnd):
    """Cylinder past the format. SAMdisk writes an empty track as entry 0
    in the header table, with no Track-Info; when it thinks it sees
    sectors in the noise, it saves them with the CRC error and different
    data on every pass. Here the two alternate, so no extra cylinder
    ends up fully formatted and a single-sided disk still has noise from
    the second extra cylinder on."""
    if (cyl + head) % 2 == 0:
        return b""
    data = bytes(rnd.randrange(256) for _ in range(512 * nsec))
    return tib(cyl, head, nsec, bad=True) + data


MAX_COPIES = 3      # SAMdisk 3.8 keeps at most 3 (--max-copies)


def spread_copies(extra, nsec):
    """n extra copies spread over the last sectors, at most MAX_COPIES
    per sector: beyond that, SAMdisk discards them when rereading the
    file ("ignored 2 extra copies")."""
    copies, s = {}, nsec
    while extra > 0:
        k = min(extra, MAX_COPIES - 1)
        copies[s] = 1 + k
        extra -= k
        s -= 1
    return copies


def add_copies(data, copies, nsec, rnd):
    """Put the extra copies right after the sector's first copy, where
    whoever reads the EDSK looks for them. With copies only on the last
    sector the track is the same as before, with data appended."""
    out = bytearray()
    for s in range(1, nsec + 1):
        out += data[(s - 1) * 512:s * 512]
        out += bytes(rnd.randrange(256)
                     for _ in range(512 * (copies.get(s, 1) - 1)))
    return out


def header(tracks, cyls, heads):
    """EDSK header: the table at 0x34 declares the size of each track in
    256-byte blocks, Track-Info included; 0 means an unformatted track,
    absent from the file."""
    h = bytearray(256)
    h[0:len(SIG)] = SIG
    h[0x22:0x30] = b"floppyarc test"[:14]
    h[0x30] = cyls
    h[0x31] = heads
    for i, t in enumerate(tracks):
        h[0x34 + i] = len(t) // 256
    return bytes(h)


def build(cyls, heads=2, nsec=18, fmt_upto=None, seed=1,
          corrupt=None, grow=None, weak=None):
    """fmt_upto  formatted cylinders; beyond, noise (see noise_track)
       corrupt   (cyl, head) with stably altered data
       grow      ((cyl, head), n) n extra copies of the last sectors
       weak      [(cyl, head), ...] tracks that change on every read"""
    rnd = random.Random(seed)
    fmt_upto = cyls if fmt_upto is None else fmt_upto
    weak = weak or []
    tracks = []
    for c in range(cyls):
        for h in range(heads):
            if c >= fmt_upto:
                tracks.append(noise_track(c, h, nsec, rnd))
                continue
            data = bytearray((c * 7 + h * 3 + i) % 251
                             for i in range(512 * nsec))
            if corrupt == (c, h):
                for k in range(40):
                    data[1000 + k * 3] ^= 0xFF
            if (c, h) in weak:
                for k in range(60):
                    data[200 + k * 9] = rnd.randrange(256)
            copies = None
            if grow and grow[0] == (c, h):
                copies = spread_copies(grow[1], nsec)
                data = add_copies(data, copies, nsec, rnd)
            tracks.append(tib(c, h, nsec, copies=copies) + bytes(data))
    return header(tracks, cyls, heads) + b"".join(tracks)


def build_from_image(img, cyls, heads, nsec, extra=0):
    """Distribute an image sector by sector (512 bytes, C/H/S order) into
    EDSK tracks, plus `extra` empty cylinders like the ones SAMdisk
    writes when copying a real diskette."""
    tracks = []
    for c in range(cyls + extra):
        for h in range(heads):
            if c < cyls:
                lba = (c * heads + h) * nsec
                tracks.append(tib(c, h, nsec) + img[lba * 512:(lba + nsec) * 512])
            else:
                tracks.append(b"")
    return header(tracks, cyls + extra, heads) + b"".join(tracks)


def n83(name):
    """'MONKEY.003' -> b'MONKEY  003', as in FAT directory entries."""
    base, _, ext = name.partition(".")
    return (base.ljust(8) + ext.ljust(3)).encode("ascii")


def dir_entry(name11, attr, cluster, size, when):
    y, mo, d, hh, mi = when
    e = bytearray(32)
    e[0:11] = name11
    e[11] = attr
    struct.pack_into("<HHHI", e, 22, (hh << 11) | (mi << 5),
                     ((y - 1980) << 9) | (mo << 5) | d, cluster, size)
    return bytes(e)


def fat12_image(label, files, subdir=None, loops=0):
    """1.44 MB FAT12 as DOS formats it: BPB, two 9-sector FATs, 224 root
    directory entries. files: [(8.3 name, content, date)] with
    date = (year, month, day, hour, minutes)."""
    total, spt, heads = 2880, 18, 2
    img = bytearray(total * 512)
    img[0:3] = b"\xEB\x3C\x90"
    img[3:11] = b"MSDOS5.0"
    struct.pack_into("<HBHBHHBHHH", img, 11, 512, 1, 1, 2, 224, total,
                     0xF0, 9, spt, heads)
    img[0x26] = 0x29
    img[0x2B:0x36] = label.ljust(11).encode("ascii")
    img[0x36:0x3E] = b"FAT12   "
    img[510:512] = b"\x55\xAA"
    data_sec = 1 + 2 * 9 + 14            # boot, two FATs, root
    fat = {0: 0xFF0, 1: 0xFFF}
    next_cl = [2]

    def alloc(content):
        n = max(1, (len(content) + 511) // 512)
        first = next_cl[0]
        for i in range(n):
            cl = first + i
            fat[cl] = cl + 1 if i < n - 1 else 0xFFF
            off = (data_sec + cl - 2) * 512
            chunk = content[i * 512:(i + 1) * 512]
            img[off:off + len(chunk)] = chunk
        next_cl[0] += n
        return first

    when0 = (1990, 10, 1, 12, 0)
    root = [dir_entry(label.ljust(11).encode("ascii"), 0x08, 0, 0, when0)]
    for name, content, when in files:
        root.append(dir_entry(n83(name), 0x20, alloc(content), len(content), when))
    if subdir:
        dname, dfiles, dwhen = subdir
        entries = [dir_entry(n83(n), 0x20, alloc(c), len(c), w) for n, c, w in dfiles]
        dcl = next_cl[0]
        # loops: entries pointing to the same directory (circular chain)
        entries += [dir_entry(n83("LOOP%02d" % i), 0x10, dcl, 0, dwhen)
                    for i in range(loops)]
        content = (dir_entry(b".          ", 0x10, dcl, 0, dwhen) +
                   dir_entry(b"..         ", 0x10, 0, 0, dwhen) + b"".join(entries))
        alloc(content.ljust(512, b"\0"))
        root.append(dir_entry(n83(dname), 0x10, dcl, 0, dwhen))

    fatb = bytearray(9 * 512)
    for cl, val in fat.items():
        o = cl * 3 // 2
        if cl & 1:
            fatb[o] = (fatb[o] & 0x0F) | ((val << 4) & 0xF0)
            fatb[o + 1] = (val >> 4) & 0xFF
        else:
            fatb[o] = val & 0xFF
            fatb[o + 1] = (fatb[o + 1] & 0xF0) | ((val >> 8) & 0x0F)
    img[512:512 + len(fatb)] = fatb
    img[512 + 9 * 512:512 + 18 * 512] = fatb
    root_off = 512 * 19
    for i, e in enumerate(root):
        img[root_off + i * 32:root_off + (i + 1) * 32] = e
    return bytes(img)


FAT_FILES = [("MONKEY.003", b"M" * 1300, (1990, 10, 1, 12, 0)),
             ("README.TXT", b"Leggimi\r\n", (1990, 9, 15, 9, 30))]
FAT_SUB = ("SAVE", [("SAVE1.SAV", b"S" * 700, (1990, 11, 2, 20, 5))],
           (1990, 11, 2, 20, 0))

# images that build() parameters cannot describe
SPECIALI = {
    # 1.44 MB with a real FAT12 file system and three extra noise cylinders
    "fat_144": lambda: build_from_image(
        fat12_image("MONKEY3", FAT_FILES, FAT_SUB), 80, 2, 18, extra=3),
    # like fat_144, but SAVE contains entries that point back to SAVE: circular chain
    "fat_loop": lambda: build_from_image(
        fat12_image("MONKEY3", FAT_FILES, FAT_SUB, loops=15), 80, 2, 18, extra=3),
}


SCENARI = {
    # two identical reads of a healthy 1.44 MB
    "ident_a":    dict(cyls=80),
    "ident_b":    dict(cyls=80),
    # noise only in the extra cylinders: different there, identical below
    "extra_a":    dict(cyls=83, fmt_upto=80, seed=1),
    "extra_b":    dict(cyls=83, fmt_upto=80, seed=2),
    # stable damage in the data area
    "bad_a":      dict(cyls=80),
    "bad_b":      dict(cyls=80, corrupt=(12, 1)),
    # the two reads captured a different number of cylinders
    "short_a":    dict(cyls=82, fmt_upto=80, seed=1),
    "short_b":    dict(cyls=83, fmt_upto=80, seed=1),
    # 720K with 40 cylinders and 9 sectors, with two extra noise cylinders
    "dd40_a":     dict(cyls=42, nsec=9, fmt_upto=40, seed=1),
    "dd40_b":     dict(cyls=42, nsec=9, fmt_upto=40, seed=2),
    "dd40bad_a":  dict(cyls=42, nsec=9, fmt_upto=40, seed=1),
    "dd40bad_b":  dict(cyls=42, nsec=9, fmt_upto=40, seed=1, corrupt=(7, 0)),
    # 40 single-sided cylinders: 40 empty, noise on 41
    "ss40_a":     dict(cyls=42, heads=1, nsec=9, fmt_upto=40, seed=1),
    "ss40_b":     dict(cyls=42, heads=1, nsec=9, fmt_upto=40, seed=3),
    # three reads of a healthy disk
    "good_1":     dict(cyls=82, fmt_upto=80, seed=41),
    "good_2":     dict(cyls=82, fmt_upto=80, seed=42),
    "good_3":     dict(cyls=82, fmt_upto=80, seed=43),
    # three reads with unstable sectors: (30,0) with no majority,
    # (5,1) different in only one read out of three
    "weak_1":     dict(cyls=82, fmt_upto=80, seed=11, weak=[(30, 0)]),
    "weak_2":     dict(cyls=82, fmt_upto=80, seed=22, weak=[(30, 0)]),
    "weak_3":     dict(cyls=82, fmt_upto=80, seed=33, weak=[(30, 0)],
                       corrupt=(5, 1)),
    # three reads where the track grows with saved copies: in 2 sector
    # 18 has 3 copies, in 3 sector 17 too
    "grow_1":     dict(cyls=80, seed=1),
    "grow_2":     dict(cyls=80, seed=2, grow=((4, 0), 2)),
    "grow_3":     dict(cyls=80, seed=3, grow=((4, 0), 4)),
}


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for name, kw in SCENARI.items():
        with open(os.path.join(OUT, name + ".dsk"), "wb") as fh:
            fh.write(build(**kw))
    for name, make in SPECIALI.items():
        with open(os.path.join(OUT, name + ".dsk"), "wb") as fh:
            fh.write(make())
    print("%d images generated in %s" % (len(SCENARI) + len(SPECIALI), OUT))
