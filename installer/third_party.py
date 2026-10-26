"""Prepare the third-party programs for the installer.

    python3 -I installer/third_party.py LIST SOURCE DEST [--version YYMMDD]
    python3 -I installer/third_party.py --cacert-age MONTHS LIST SOURCE --version YYMMDD

For each line of LIST (installer/third-party.txt) check that SOURCE has the
file with the right hash; if one is missing, or differs, say what to
download and from where and exit with 1, preparing nothing. Otherwise put
the files to install in DEST/<id>/ (a zip is unpacked) and write
DEST/third_party.nsh with versions and names for retroarchiver.nsi.

The licence texts (license_file field) live in the repo, in licenses/
next to LIST: they go in DEST/licenses/ and, for a zip, also in
DEST/<id>/, with CRLF line endings for XP's Notepad.

The CA bundle (id "cacert") gets two checks of its own, because it ages:
its version field must be the date written in the file, the roots
RetroArchiver needs today must be in it, and with --version it must not be
much older than the release. A normal run only warns about the age (6
months, enough for `make setup`); --cacert-age MONTHS checks nothing else
and exits with 1 when the bundle is older than that, which is how
publish/publish.sh stops a release with a stale bundle.
"""

import hashlib
import os
import shutil
import sys
import zipfile

FIELDS = ("id", "name", "version", "file", "sha256", "url", "root", "license", "license_file")

# The CA bundle: Mozilla's roots as curl extracts them. Its date line is
# "## Certificate data from Mozilla as of: Tue Oct 20 13:32:40 2026 GMT".
CACERT_ID = "cacert"
CACERT_DATE = "Certificate data from Mozilla as of:"
# the roots RetroArchiver needs today: www.archive.org and s3.us.archive.org
# chain up to this one. One missing means the bundle is unusable for upload.
CACERT_ROOTS = ("Go Daddy Root Certificate Authority - G2",)
# older than this, in months, and `make setup` says so; publish.sh asks for 12
CACERT_WARN_MONTHS = 6
MONTH_NAMES = ("Jan", "Feb", "Mar", "Apr", "May", "Jun",
               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")


def read_list(path):
    items = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p.strip() for p in line.split("|")]
            if len(parts) != len(FIELDS):
                sys.exit("%s:%d: %d fields separated by '|' are needed, found %d"
                         % (path, n, len(FIELDS), len(parts)))
            items.append(dict(zip(FIELDS, parts)))
    return items


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def license_path(list_path, it):
    if it["license_file"] == "-":
        return None
    return os.path.join(os.path.dirname(os.path.abspath(list_path)), "licenses", it["license_file"])


def check_licenses(items, list_path):
    ok = True
    for it in items:
        p = license_path(list_path, it)
        if p and not os.path.isfile(p):
            print("  %s: licence text is missing: %s" % (it["name"], p))
            ok = False
    return ok


# the licence text with Windows line endings
def copy_crlf(src, dst):
    with open(src, "rb") as f:
        data = f.read().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
    with open(dst, "wb") as f:
        f.write(data)


# the date in the bundle's header as (year, month, day), None if it is not there
def bundle_date(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        head = f.read(4096)
    i = head.find(CACERT_DATE)
    if i < 0:
        return None
    # "Tue Oct 20 13:32:40 2026 GMT": month name, day, then the year
    words = head[i + len(CACERT_DATE):].split()
    for n, w in enumerate(words):
        if w in MONTH_NAMES and n + 4 < len(words):
            try:
                return int(words[n + 3]), MONTH_NAMES.index(w) + 1, int(words[n + 1])
            except ValueError:
                return None
    return None


# RetroArchiver's version (YYMMDD of the release) as (year, month, day)
def version_date(ver):
    if len(ver) != 6 or not ver.isdigit():
        return None
    return 2000 + int(ver[:2]), int(ver[2:4]), int(ver[4:])


# whole months from a to b, both (year, month, day)
def months_between(a, b):
    n = (b[0] - a[0]) * 12 + b[1] - a[1]
    if b[2] < a[2]:
        n -= 1
    return n


def bundle_text(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


# The bundle's own checks. `fatal` makes an old bundle an error instead of a
# warning; everything else (wrong version field, missing root) is always an
# error. Returns False when the caller must stop.
def check_cacert(it, src, version, limit, fatal):
    path = os.path.join(src, it["file"])
    d = bundle_date(path)
    if d is None:
        print('  %s: no "%s" line in %s: is it the bundle from %s?'
              % (it["name"], CACERT_DATE, it["file"], it["url"]))
        return False
    iso = "%04d-%02d-%02d" % d
    ok = True
    if it["version"] != iso:
        print("  %s: the list says version %s, the file says %s: fix the line"
              % (it["name"], it["version"], iso))
        ok = False
    text = bundle_text(path)
    for root in CACERT_ROOTS:
        if root not in text:
            print('  %s: the root "%s" is not in %s: archive.org needs it'
                  % (it["name"], root, it["file"]))
            ok = False
    v = version_date(version) if version else None
    if v:
        age = months_between(d, v)
        if age > limit:
            print("  %s: Mozilla's data of %s is %d months older than version %s"
                  % (it["name"], iso, age, version))
            print("      download a new %s from %s and change the list"
                  % (it["file"], it["url"]))
            if fatal:
                ok = False
    return ok


def check(items, src):
    bad = []
    for it in items:
        p = os.path.join(src, it["file"])
        if not os.path.isfile(p):
            bad.append((it, "is missing"))
        elif sha256(p) != it["sha256"].lower():
            bad.append((it, "has a different hash (wrong or damaged file)"))
    for it, why in bad:
        print("  %s %s: %s %s" % (it["name"], it["version"], it["file"], why))
        print("      download it from %s" % it["url"])
        print("      and put it in %s (SHA-256 %s)" % (src, it["sha256"]))
    return not bad


# the names inside the zip that must be installed, with the relative path
def zip_members(z, root):
    for info in z.infolist():
        name = info.filename
        if name.endswith("/"):
            continue
        if root == ".":
            rel = name
        elif name.startswith(root + "/"):
            rel = name[len(root) + 1:]
        elif "/" not in name:
            rel = name                      # licence and readme at the top of the zip
        else:
            continue
        parts = rel.split("/")
        if rel.startswith("/") or ".." in parts or ":" in rel:
            sys.exit("%s: unacceptable path in the zip: %s" % (z.filename, name))
        yield info, rel


def prepare(it, src, dest, list_path):
    out = os.path.join(dest, it["id"])
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    p = os.path.join(src, it["file"])
    files = []
    if it["root"] == "-":
        shutil.copyfile(p, os.path.join(out, it["file"]))
        files.append(it["file"])
    else:
        with zipfile.ZipFile(p) as z:
            for info, rel in zip_members(z, it["root"]):
                target = os.path.join(out, *rel.split("/"))
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with z.open(info) as a, open(target, "wb") as b:
                    shutil.copyfileobj(a, b)
                files.append(rel)
    if not files:
        sys.exit("%s: nothing in the zip under '%s'" % (it["file"], it["root"]))
    lic = license_path(list_path, it)
    if lic:
        os.makedirs(os.path.join(dest, "licenses"), exist_ok=True)
        copy_crlf(lic, os.path.join(dest, "licenses", it["license_file"]))
        if it["root"] != "-":         # also next to the program
            copy_crlf(lic, os.path.join(out, it["license_file"]))
            files.append(it["license_file"])
    return sorted(files)


def nsis(s):
    return s.replace("$", "$$").replace('"', '$\\"')


# for each component: name, version, and the macro that removes the installed files
def write_nsh(path, prepared):
    lines = ["; generated by installer/third_party.py: do not edit", ""]
    for it, files in prepared:
        k = "T_%s_" % it["id"]
        for key in ("name", "version", "file", "url", "license_file"):
            lines.append('!define %s%s "%s"' % (k, key.upper(), nsis(it[key])))
        lines.append("!macro %sREMOVE DIR" % k)
        dirs = set()
        for rel in files:
            lines.append('  Delete "${DIR}\\%s"' % nsis(rel.replace("/", "\\")))
            parts = rel.split("/")[:-1]
            for i in range(1, len(parts) + 1):
                dirs.add("\\".join(parts[:i]))
        for d in sorted(dirs, key=lambda d: -d.count("\\")):
            lines.append('  RMDir "${DIR}\\%s"' % nsis(d))
        lines.append('  RMDir "${DIR}"')
        lines.append("!macroend")
        lines.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


USAGE = ("usage: third_party.py LIST SOURCE DEST [--version YYMMDD]\n"
         "       third_party.py --cacert-age MONTHS LIST SOURCE --version YYMMDD")


def find_cacert(items, list_path):
    for it in items:
        if it["id"] == CACERT_ID:
            return it
    sys.exit("%s: no line with id '%s'" % (list_path, CACERT_ID))


# only the CA bundle, with its age as an error: for publish.sh
def cacert_age(args, version):
    if len(args) != 3 or not version:
        sys.exit(USAGE)
    if not args[0].isdigit():
        sys.exit(USAGE)
    months, list_path, src = int(args[0]), args[1], args[2]
    it = find_cacert(read_list(list_path), list_path)
    if not check([it], src) or not check_cacert(it, src, version, months, True):
        print("  The CA bundle is not the one to release: see %s." % list_path)
        sys.exit(1)
    print("  %s %s: hash ok, no more than %d months older than %s"
          % (it["name"], it["version"], months, version))


def main():
    args = sys.argv[1:]
    version = ""
    if "--version" in args:
        i = args.index("--version")
        if i + 1 >= len(args):
            sys.exit(USAGE)
        version = args[i + 1]
        del args[i:i + 2]
    if args and args[0] == "--cacert-age":
        cacert_age(args[1:], version)
        return
    if len(args) != 3:
        sys.exit(USAGE)
    list_path, src, dest = args
    items = read_list(list_path)
    if not check_licenses(items, list_path):
        sys.exit(1)
    if not check(items, src):
        print("  Third-party programs missing or different from the tested ones: see %s." % list_path)
        print("  Or point to another folder: make setup THIRDPARTY=folder")
        sys.exit(1)
    if not check_cacert(find_cacert(items, list_path), src, version, CACERT_WARN_MONTHS, False):
        sys.exit(1)
    os.makedirs(dest, exist_ok=True)
    shutil.rmtree(os.path.join(dest, "licenses"), ignore_errors=True)
    prepared = [(it, prepare(it, src, dest, list_path)) for it in items]
    write_nsh(os.path.join(dest, "third_party.nsh"), prepared)
    for it, files in prepared:
        print("  %s %s: hash ok, %d file(s)" % (it["name"], it["version"], len(files)))


if __name__ == "__main__":
    main()
