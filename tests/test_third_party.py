"""Tests of installer/third_party.py with fake third-party files (no downloads).

    cd tests && python3 -I test_third_party.py
"""

import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
THIRD_PARTY = os.path.join(ROOT, "installer", "third_party.py")
REAL_LIST = os.path.join(ROOT, "installer", "third-party.txt")
REAL_DOWNLOADS = os.path.join(ROOT, "installer", "third-party")
# the root archive.org chains up to: third_party.py looks for it by name
CACERT_ROOT = "Go Daddy Root Certificate Authority - G2"
fails = 0


def ok(cond, what):
    global fails
    print("  %s %s" % ("ok  " if cond else "FAIL", what))
    if not cond:
        fails += 1


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def run(list_path, src, dest, *more):
    p = subprocess.run([sys.executable, "-I", THIRD_PARTY, list_path, src, dest] + list(more),
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def run_age(list_path, src, months, version):
    p = subprocess.run([sys.executable, "-I", THIRD_PARTY, "--cacert-age", str(months),
                        list_path, src, "--version", version],
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


# a cacert.pem like curl's: the header with Mozilla's date, then the roots
def write_bundle(path, date, roots=(CACERT_ROOT,)):
    with open(path, "w", encoding="utf-8") as f:
        f.write("##\n## Bundle of CA Root Certificates\n##\n")
        f.write("## Certificate data from Mozilla as of: %s GMT\n##\n\n" % date)
        for r in roots:
            f.write("%s\n%s\n-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n\n"
                    % (r, "=" * len(r)))


def real_version():
    with open(os.path.join(ROOT, "src", "core", "version.h"), encoding="utf-8") as f:
        for line in f:
            if "RA_VERSION" in line:
                return line.split('"')[1]
    return ""


def main():
    tmp = tempfile.mkdtemp(prefix="third_party_")
    try:
        src = os.path.join(tmp, "scaricati")
        dest = os.path.join(tmp, "build")
        os.makedirs(src)
        # a zip like DiscImageCreator's: license at the top, program in a folder
        z = os.path.join(src, "prog.zip")
        with zipfile.ZipFile(z, "w") as f:
            f.writestr("LICENSE", "licenza")
            f.writestr("Release/prog.exe", "exe")
            f.writestr("Release/Doc/note & varie.txt", "doc")
            f.writestr("Altro/escluso.txt", "no")
        inst = os.path.join(src, "Setup.exe")
        with open(inst, "wb") as f:
            f.write(b"MZ installer")
        # the license texts sit next to the list, in licenses/
        os.makedirs(os.path.join(tmp, "licenses"))
        with open(os.path.join(tmp, "licenses", "prog-LICENSE.txt"), "wb") as f:
            f.write(b"MIT License\n\nCopyright\n")
        with open(os.path.join(tmp, "licenses", "mpl.txt"), "wb") as f:
            f.write(b"Mozilla Public License Version 2.0\n")
        # the CA bundle: a data file, like an installer it is used as it is
        bundle = os.path.join(src, "cacert.pem")
        write_bundle(bundle, "Tue Oct 20 13:32:40 2026")
        list_path = os.path.join(tmp, "third-party.txt")

        def write_list(cacert_version="2026-10-20"):
            with open(list_path, "w", encoding="utf-8") as f:
                f.write("# commento\n\n")
                f.write("prog|Programma|1.0|prog.zip|%s|https://example.org/prog.zip|Release|MIT|prog-LICENSE.txt\n" % sha(z))
                f.write("drv|Driver|2.0|Setup.exe|%s|https://example.org/Setup.exe|-|freeware|-\n" % sha(inst))
                f.write("cacert|CA list|%s|cacert.pem|%s|https://example.org/cacert.pem|-|MPL 2.0|mpl.txt\n"
                        % (cacert_version, sha(bundle)))

        write_list()
        code, out = run(list_path, src, dest)
        ok(code == 0, "right files: exits with 0")
        ok(os.path.isfile(os.path.join(dest, "prog", "prog.exe")), "zip: the root becomes the folder")
        ok(os.path.isfile(os.path.join(dest, "prog", "LICENSE")), "zip: files at the top of the zip are included")
        ok(os.path.isfile(os.path.join(dest, "prog", "Doc", "note & varie.txt")), "zip: subfolders are kept")
        ok(not os.path.exists(os.path.join(dest, "prog", "escluso.txt")) and
           not os.path.exists(os.path.join(dest, "prog", "Altro")), "zip: nothing outside the root")
        ok(os.path.isfile(os.path.join(dest, "drv", "Setup.exe")), "installer: copied as is")
        nsh = open(os.path.join(dest, "third_party.nsh"), encoding="utf-8").read()
        ok('!define T_prog_VERSION "1.0"' in nsh, "nsh: version")
        ok('Delete "${DIR}\\Doc\\note & varie.txt"' in nsh, "nsh: the macro removes the files")
        ok(nsh.index('RMDir "${DIR}\\Doc"') < nsh.index('RMDir "${DIR}"\n'), "nsh: subfolders first")
        lic = os.path.join(dest, "licenses", "prog-LICENSE.txt")
        ok(os.path.isfile(lic) and open(lic, "rb").read() == b"MIT License\r\n\r\nCopyright\r\n",
           "licence: in licenses/, with CRLF line endings")
        ok(os.path.isfile(os.path.join(dest, "prog", "prog-LICENSE.txt")), "licence: also next to the program")
        ok('Delete "${DIR}\\prog-LICENSE.txt"' in nsh, "licence: the macro removes it")
        ok(os.listdir(os.path.join(dest, "drv")) == ["Setup.exe"] and
           sorted(os.listdir(os.path.join(dest, "licenses"))) == ["mpl.txt", "prog-LICENSE.txt"],
           "licence '-': no file")
        ok('!define T_prog_LICENSE_FILE "prog-LICENSE.txt"' in nsh, "nsh: the licence file name")

        # ---- the CA bundle ----
        ok(os.listdir(os.path.join(dest, "cacert")) == ["cacert.pem"], "bundle: copied as is")
        ok('!define T_cacert_VERSION "2026-10-20"' in nsh and
           'Delete "${DIR}\\cacert.pem"' in nsh, "bundle: version and removal in the nsh")
        code, out = run(list_path, src, dest, "--version", "261026")
        ok(code == 0 and "older than version" not in out, "bundle: a fresh one says nothing")
        code, out = run(list_path, src, dest, "--version", "280101")
        ok(code == 0 and "14 months older than version 280101" in out,
           "bundle: an old one is only a warning in a normal run")
        code, out = run_age(list_path, src, 12, "261026")
        ok(code == 0, "--cacert-age: a fresh one passes")
        code, out = run_age(list_path, src, 12, "280101")
        ok(code == 1 and "14 months older" in out, "--cacert-age: an old one stops the release")
        code, out = run_age(list_path, src, 6, "270601")
        ok(code == 1 and "7 months older" in out, "--cacert-age: the months are the limit given")

        write_list("2026-09-01")
        code, out = run(list_path, src, dest)
        ok(code == 1 and "the list says version 2026-09-01, the file says 2026-10-20" in out,
           "bundle: the version in the list must be the date in the file")
        write_list()

        write_bundle(bundle, "Tue Oct 20 13:32:40 2026", ("Some Other Root CA",))
        write_list()
        code, out = run(list_path, src, dest)
        ok(code == 1 and CACERT_ROOT in out and "archive.org needs it" in out,
           "bundle: without the root we need it stops")
        with open(bundle, "w", encoding="utf-8") as f:
            f.write("## no date here\n%s\n" % CACERT_ROOT)
        write_list()
        code, out = run(list_path, src, dest)
        ok(code == 1 and "Certificate data from Mozilla as of:" in out,
           "bundle: without Mozilla's date line it stops")
        write_bundle(bundle, "Tue Oct 20 13:32:40 2026")
        write_list()

        os.rename(os.path.join(tmp, "licenses", "prog-LICENSE.txt"), os.path.join(tmp, "licenses", "altro.txt"))
        code, out = run(list_path, src, dest)
        ok(code == 1 and "licence text is missing" in out, "missing licence text: stops")
        os.rename(os.path.join(tmp, "licenses", "altro.txt"), os.path.join(tmp, "licenses", "prog-LICENSE.txt"))

        with open(inst, "ab") as f:
            f.write(b"!")
        os.remove(z)
        code, out = run(list_path, src, dest)
        ok(code == 1, "missing or changed file: exits with 1")
        ok("prog.zip is missing" in out and "https://example.org/prog.zip" in out, "says what is missing and where from")
        ok("Setup.exe has a different hash" in out, "says which hash is wrong")

        with open(list_path, "a", encoding="utf-8") as f:
            f.write("rotta|solo tre|campi\n")
        code, out = run(list_path, src, dest)
        ok(code != 0 and "9 fields" in out, "bad line in the list: stops")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # the real list: the bundle is there, and if it has been downloaded it must
    # still be good for a release (the download folder is not in the repo)
    real = open(REAL_LIST, encoding="utf-8").read()
    ok("\ncacert|" in real, "installer/third-party.txt: the cacert line is there")
    ok(os.path.isfile(os.path.join(ROOT, "installer", "licenses", "MPL-2.0.txt")),
       "installer/licenses/MPL-2.0.txt: the MPL 2.0 text is there")
    # BearSSL is compiled in, so it has no line in the list: only its licence,
    # which must stay the same as the vendored one
    vendored = os.path.join(ROOT, "third_party", "bearssl", "LICENSE.txt")
    copy = os.path.join(ROOT, "installer", "licenses", "BearSSL-LICENSE.txt")
    ok(os.path.isfile(copy) and open(copy, "rb").read() == open(vendored, "rb").read(),
       "BearSSL: the licence in installer/licenses is the vendored one")
    # the About window names the vendored commit: it must be the notices' one
    about = open(os.path.join(ROOT, "src", "gui", "about.c"), encoding="utf-8").read()
    notices = open(os.path.join(ROOT, "THIRD-PARTY-NOTICES.txt"), encoding="utf-8").read()
    m = re.search(r'BEARSSL_VERSION L"0\.6\+ git ([0-9a-f]{8}) \((\d{4}-\d\d-\d\d)\)"', about)
    ok(m is not None and ("commit " + m.group(1)) in notices and ("(" + m.group(2) + ")") in notices,
       "BearSSL: the About window names the commit of THIRD-PARTY-NOTICES.txt")
    if os.path.isfile(os.path.join(REAL_DOWNLOADS, "cacert.pem")):
        code, out = run_age(REAL_LIST, REAL_DOWNLOADS, 12, real_version())
        ok(code == 0, "the downloaded cacert.pem is still good for a release")
        if code != 0:
            print(out)
    else:
        print("  skip the downloaded cacert.pem: not in installer/third-party")
    print()
    print("  third_party.py: %s" % ("all green." if not fails else "%d checks failed." % fails))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
