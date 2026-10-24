#!/usr/bin/env python3
"""Check a RetroArchiver bag: BagIt 1.0 structure, SHA-256 manifest and
tagmanifest, Payload-Oxum, profile fields. Standard library only, so it
runs wherever Python 3 is available.

    python3 check_bag.py BAG_FOLDER [profile.json]

Without a profile, the one the bag declares in BagIt-Profile-Identifier
is used: docs/bagit-profile.json for today's boxes (mixed ones too),
docs/bagit-profile-2.json for those closed by version 261017,
docs/bagit-profile-floppy.json for those closed before mixed media.
"""
import hashlib
import json
import os
import sys


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def read_manifest(path):
    out = {}
    with open(path, encoding="utf-8", newline="") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if line:
                digest, name = line.split("  ", 1)
                out[name] = digest
    return out


DOCS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs")
PROFILES = {
    "urn:retroarchiver:bagit-profile:3": os.path.join(DOCS, "bagit-profile.json"),
    "urn:retroarchiver:bagit-profile:2": os.path.join(DOCS, "bagit-profile-2.json"),
    "urn:retroarchiver:bagit-profile:floppy:1": os.path.join(DOCS, "bagit-profile-floppy.json"),
}


def check_media(info):
    """Media-Type, Media-Expected and Missing-Media must agree."""
    errors = []
    kinds = {"floppy": 0, "CD": 0, "DVD": 0}
    for part in info.get("Media-Type", "").split(", "):
        n, _, kind = part.partition(" ")
        if not n.isdigit() or kind not in kinds:
            return ["Media-Type %r is not a composition (\"3 floppy, 1 CD\")" % info.get("Media-Type")]
        kinds[kind] += int(n)
    if str(sum(kinds.values())) != info.get("Media-Expected"):
        errors.append("Media-Expected %r does not match Media-Type %r"
                      % (info.get("Media-Expected"), info.get("Media-Type")))
    for name in filter(None, info.get("Missing-Media", "").split(", ")):
        kind, _, n = name.partition(" ")
        if kind not in kinds or not n.isdigit() or not 1 <= int(n) <= kinds[kind]:
            errors.append("Missing-Media: %r is not a medium of the box" % name)
    return errors


TAG_FILES = ("bagit.txt", "bag-info.txt", "manifest-sha256.txt", "tagmanifest-sha256.txt")


def check(bag, profile_path=None):
    errors = []
    # .in-corso: the open marker of version 261017
    if os.path.exists(os.path.join(bag, ".in-progress")) or os.path.exists(os.path.join(bag, ".in-corso")):
        errors.append("the bag is still open (.in-progress)")
    # editing/: an edit under way or interrupted, never left in a closed bag
    if os.path.exists(os.path.join(bag, "editing")):
        errors.append("an edit was left halfway (editing/)")
    for name in TAG_FILES:
        if not os.path.isfile(os.path.join(bag, name)):
            errors.append("%s is missing" % name)
    if errors:
        return errors
    with open(os.path.join(bag, "bagit.txt"), encoding="utf-8", newline="") as f:
        if f.read() != "BagIt-Version: 1.0\nTag-File-Character-Encoding: UTF-8\n":
            errors.append("bagit.txt not compliant")
    info = {}
    with open(os.path.join(bag, "bag-info.txt"), encoding="utf-8", newline="") as f:
        for line in f:
            key, _, value = line.rstrip("\r\n").partition(": ")
            info[key] = value
    if profile_path is None:
        profile_path = PROFILES.get(info.get("BagIt-Profile-Identifier"))
        if profile_path is None:
            return errors + ["unknown profile: %r" % info.get("BagIt-Profile-Identifier")]
    with open(profile_path, encoding="utf-8") as f:
        profile = json.load(f)
    for key, rule in profile["Bag-Info"].items():
        if rule.get("required") and key not in info:
            errors.append("%s is missing in bag-info.txt" % key)
        if key in info and "values" in rule and info[key] not in rule["values"]:
            errors.append("%s is %r, allowed %r" % (key, info[key], rule["values"]))
    if profile["BagIt-Profile-Info"]["BagIt-Profile-Identifier"] in ("urn:retroarchiver:bagit-profile:2",
                                                                    "urn:retroarchiver:bagit-profile:3"):
        errors += check_media(info)
    payload = []
    for root, _, files in os.walk(os.path.join(bag, "data")):
        for name in files:
            rel = os.path.relpath(os.path.join(root, name), bag)
            payload.append(rel.replace(os.sep, "/"))
    manifest = read_manifest(os.path.join(bag, "manifest-sha256.txt"))
    if sorted(manifest) != sorted(payload):
        errors.append("the manifest does not list exactly the files in data/")
    for name, digest in manifest.items():
        path = os.path.join(bag, name)
        if os.path.exists(path) and sha256(path) != digest:
            errors.append("different hash for %s" % name)
    size = sum(os.path.getsize(os.path.join(bag, n)) for n in payload)
    oxum = "%d.%d" % (size, len(payload))
    if info.get("Payload-Oxum") != oxum:
        errors.append("Payload-Oxum %r, expected %s" % (info.get("Payload-Oxum"), oxum))
    tags = read_manifest(os.path.join(bag, "tagmanifest-sha256.txt"))
    for name in ("bagit.txt", "bag-info.txt", "manifest-sha256.txt"):
        if name not in tags or sha256(os.path.join(bag, name)) != tags[name]:
            errors.append("tagmanifest not valid for %s" % name)
    return errors


if __name__ == "__main__":
    prof = sys.argv[2] if len(sys.argv) > 2 else None
    errs = check(sys.argv[1], prof)
    for e in errs:
        print("  BAG: " + e)
    print("  BAG OK" if not errs else "  BAG NOT VALID")
    sys.exit(1 if errs else 0)
